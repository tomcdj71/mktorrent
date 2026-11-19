/*
This file is part of mktorrent
Copyright (C) 2007, 2009 Emil Renner Berthing

mktorrent is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

mktorrent is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
*/

/* This file should only be compiled if USE_PTHREADS is defined */
#ifndef USE_PTHREADS
#error "This file should only be compiled with pthreads support"
#endif

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdlib.h>       /* exit(), malloc() */
#include <sys/types.h>    /* off_t */
#include <errno.h>        /* errno */
#include <string.h>       /* strerror() */
#include <stdio.h>        /* printf() etc. */
#include <fcntl.h>        /* open() */
#include <unistd.h>       /* read(), close() */
#include <inttypes.h>     /* PRId64 etc. */
#include <pthread.h>
#include <time.h>         /* nanosleep() */
#include <sys/stat.h>     /* fstat() */
#include <signal.h>
#include <sys/mman.h>     /* mmap, munmap, madvise */
#include <sys/resource.h> /* getrlimit, setrlimit */
#include <limits.h>       /* LLONG_MAX / OFF_MAX */

#include "export.h"
#include "mktorrent.h"
#include "hash.h"
#include "msg.h"
#include "sha1_backend.h"
#ifdef USE_IO_URING
#include "iouring_read.h"
#endif

#ifndef PROGRESS_PERIOD
#define PROGRESS_PERIOD 200000
#endif

#ifndef O_BINARY
#define O_BINARY 0
#endif

#ifndef O_DIRECT
#define O_DIRECT 0
#endif

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#define OPENFLAGS (O_RDONLY | O_BINARY | O_CLOEXEC)
#define DIRECT_IO_THRESHOLD (100 * 1024 * 1024) /* 100MB */
/* OpenSSL/bundled: match Rudde pool depth (3 buffers per worker). */
#define BUFFERS_PER_THREAD_OPENSSL 3
#define MAX_RETRY_COUNT 3          /* Number of retries for I/O operations */
#define RETRY_DELAY_BASE 50000     /* Base microseconds delay between retries (50ms) */

#ifndef OFF_MAX
# ifdef LLONG_MAX
#  define OFF_MAX ((off_t)LLONG_MAX)
# else
#  define OFF_MAX ((off_t)(((uintmax_t)1 << (sizeof(off_t) * 8 - 1)) - 1))
# endif
#endif


#define DEFAULT_MAX_MEMORY_PERCENT 75  /* Default maximum memory usage (% of available) */
#define MIN_MEMORY_PER_THREAD (10 * 1024 * 1024)  /* Minimum 10MB per thread */

/* External declaration of the force_exit flag for clean shutdowns */
extern volatile sig_atomic_t force_exit;

struct piece {
	struct piece *next;
	unsigned char *dest;
	unsigned long len;
	unsigned char *data;    /* owned buffer or pointer into a mapped file */
	int owned;              /* 1 = free data with piece; 0 = mmap view */
};

/* Kept until workers finish so zero-copy piece views stay valid. */
struct mapped_file {
	struct mapped_file *next;
	void *addr;
	size_t len;
};

struct queue {
	struct piece *free;
	struct piece *full;
	unsigned int buffers_max;
	unsigned int buffers;
	pthread_mutex_t mutex_free;
	pthread_mutex_t mutex_full;
	pthread_cond_t cond_empty;
	pthread_cond_t cond_full;
	unsigned int done;
	unsigned int pieces;
	unsigned int pieces_hashed;
	unsigned int zc_inflight; /* outstanding mmap-view pieces */
	struct mapped_file *maps;
	int verbose;
	int cleanup_in_progress; /* Flag to indicate cleanup is in progress */
};

/* Queue management functions */

/* Set queue to done state and wake all waiters */
static void set_done(struct queue *q)
{
    pthread_mutex_lock(&q->mutex_full);
    q->done = 1;
    pthread_mutex_unlock(&q->mutex_full);
    pthread_cond_broadcast(&q->cond_full);
    pthread_cond_broadcast(&q->cond_empty);
}

/* Add a short relative delay to a CLOCK_REALTIME timespec. */
static void timespec_add_ms(struct timespec *ts, long ms)
{
	ts->tv_nsec += (ms % 1000) * 1000000L;
	ts->tv_sec += ms / 1000;
	if (ts->tv_nsec >= 1000000000L) {
		ts->tv_sec += 1;
		ts->tv_nsec -= 1000000000L;
	}
}

/* Get a free piece from the queue.
 * Wait until a buffer is available, cleanup starts, or force_exit is set.
 * Short timed waits let SIGINT (force_exit) be observed without a hard 30s fail. */
static struct piece *get_free(struct queue *q, size_t piece_length __attribute__((unused)))
{
	struct piece *p;
	struct timespec ts;

	pthread_mutex_lock(&q->mutex_free);

	while (q->free == NULL && !force_exit && !q->cleanup_in_progress) {
		clock_gettime(CLOCK_REALTIME, &ts);
		timespec_add_ms(&ts, 200);
		pthread_cond_timedwait(&q->cond_empty, &q->mutex_free, &ts);
	}

	if (q->free == NULL || force_exit || q->cleanup_in_progress) {
		pthread_mutex_unlock(&q->mutex_free);
		return NULL;
	}

	p = q->free;
	q->free = p->next;

	pthread_mutex_unlock(&q->mutex_free);

	return p;
}

/* Recycle or free one piece (caller holds mutex_free). */
static void recycle_piece_locked(struct queue *q, struct piece *p)
{
	if (p->owned) {
		p->next = q->free;
		q->free = p;
	} else {
		if (q->zc_inflight > 0)
			q->zc_inflight--;
		free(p);
	}
}

/* Add a piece to the free queue. Always takes mutex_free (safe vs workers). */
static void put_free(struct queue *q, struct piece *p,
		     int unlock_mutex __attribute__((unused)))
{
	if (p == NULL)
		return;

	pthread_mutex_lock(&q->mutex_free);
	if (!q->cleanup_in_progress)
		recycle_piece_locked(q, p);
	else if (p->owned) {
		free(p->data);
		free(p);
	} else {
		free(p);
	}
	pthread_mutex_unlock(&q->mutex_free);
	pthread_cond_signal(&q->cond_empty);
}

/* Add a piece to the full queue */
static void put_full(struct queue *q, struct piece *p)
{
    pthread_mutex_lock(&q->mutex_full);
    p->next = q->full;
    q->full = p;
    pthread_mutex_unlock(&q->mutex_full);
    pthread_cond_signal(&q->cond_full);
}

static void free_maps(struct queue *q)
{
	struct mapped_file *m = q->maps;

	while (m) {
		struct mapped_file *next = m->next;

		if (m->addr && m->len)
			munmap(m->addr, m->len);
		free(m);
		m = next;
	}
	q->maps = NULL;
}

static void free_piece_node(struct piece *p)
{
	if (!p)
		return;
	if (p->owned)
		free(p->data);
	free(p);
}

/* Free all allocated buffers */
static void free_buffers(struct queue *q)
{
    struct piece *p = q->free;
    struct piece *next;
    
    /* Mark that cleanup is in progress to prevent threads from accessing freed memory */
    q->cleanup_in_progress = 1;

    while (p) {
        next = p->next;
        free_piece_node(p);
        p = next;
    }
    q->free = NULL;

    p = q->full;
    while (p) {
        next = p->next;
        free_piece_node(p);
        p = next;
    }
    q->full = NULL;
    q->zc_inflight = 0;

    /* Unmap after piece views are gone. */
    free_maps(q);
}

/* Allocate a piece header + payload. When align_pages is set, data is page-aligned
 * (required for O_DIRECT / preferred for io_uring). */
static struct piece *alloc_piece(size_t piece_length, int align_pages)
{
	struct piece *p = calloc(1, sizeof(*p));
	long pagesz;
	void *buf = NULL;

	if (!p)
		return NULL;

	pagesz = sysconf(_SC_PAGESIZE);
	if (pagesz <= 0)
		pagesz = 4096;

	if (align_pages) {
		if (posix_memalign(&buf, (size_t)pagesz, piece_length) != 0) {
			free(p);
			return NULL;
		}
	} else {
		buf = malloc(piece_length);
		if (!buf) {
			free(p);
			return NULL;
		}
	}
	p->data = buf;
	p->owned = 1;
	return p;
}

/* Register an mmap that must outlive in-flight zero-copy pieces. */
static int register_map(struct queue *q, void *addr, size_t len)
{
	struct mapped_file *m = malloc(sizeof(*m));

	if (!m)
		return -1;
	m->addr = addr;
	m->len = len;
	m->next = q->maps;
	q->maps = m;
	return 0;
}

/*
 * Queue a zero-copy piece view. Caps outstanding ZC pieces at buffers_max
 * so the reader cannot run unbound ahead of workers.
 */
static int put_full_zc(struct queue *q, unsigned char *data, unsigned long len,
		       unsigned char *dest)
{
	struct piece *p;
	struct timespec ts;

	pthread_mutex_lock(&q->mutex_free);
	while (q->zc_inflight >= q->buffers_max && !force_exit
			&& !q->cleanup_in_progress) {
		clock_gettime(CLOCK_REALTIME, &ts);
		timespec_add_ms(&ts, 200);
		pthread_cond_timedwait(&q->cond_empty, &q->mutex_free, &ts);
	}
	if (force_exit || q->cleanup_in_progress) {
		pthread_mutex_unlock(&q->mutex_free);
		return -1;
	}
	q->zc_inflight++;
	pthread_mutex_unlock(&q->mutex_free);

	p = calloc(1, sizeof(*p));
	if (!p) {
		pthread_mutex_lock(&q->mutex_free);
		if (q->zc_inflight > 0)
			q->zc_inflight--;
		pthread_mutex_unlock(&q->mutex_free);
		pthread_cond_signal(&q->cond_empty);
		return -1;
	}
	p->data = data;
	p->len = len;
	p->dest = dest;
	p->owned = 0;
	put_full(q, p);
	return 0;
}

/* Robust read implementation that handles interruptions and partial reads */
static ssize_t robust_read(int fd, void *buf, size_t count)
{
    size_t total = 0;
    ssize_t n;

    while (total < count) {
        n = read(fd, (unsigned char *)buf + total, count - total);
        
        if (n == 0) /* EOF */
            break;
            
        if (n == -1) {
            if (errno == EINTR)
                continue; /* Interrupted, try again */
            return -1;    /* Real error */
        }
        
        total += n;
    }
    
    return total;
}

/* Apply file access hints to improve read performance */
static void apply_file_access_hints(int fd, size_t file_size)
{
	(void)file_size;
#ifdef POSIX_FADV_SEQUENTIAL
	/* Sequential hint only — WILLNEED can hurt warm-cache throughput. */
	if (posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL) != 0) {
		/* best-effort */
	}
#endif
}

/* Count pieces currently on the full list (caller holds mutex_full). */
static unsigned count_full_locked(const struct queue *q)
{
	unsigned c = 0;
	const struct piece *p;

	for (p = q->full; p != NULL; p = p->next)
		c++;
	return c;
}

/*
 * For max_n==1: wait for any piece (OpenSSL path).
 * For max_n>1 (ISA-L): wait until >= max_n pieces are queued, or the reader
 * finished (done), so multi-buffer waves stay full. Drain up to max_n.
 * Returns batch size, or 0 to exit the worker.
 */
static unsigned get_full_batch(struct queue *q, struct piece **out, unsigned max_n)
{
	struct timespec ts;
	unsigned n = 0;
	struct piece *p;

	if (max_n == 0)
		max_n = 1;

	pthread_mutex_lock(&q->mutex_full);

	if (q->cleanup_in_progress) {
		pthread_mutex_unlock(&q->mutex_full);
		return 0;
	}

	if (max_n == 1) {
		while (q->full == NULL && !q->done && !force_exit
				&& !q->cleanup_in_progress) {
			clock_gettime(CLOCK_REALTIME, &ts);
			timespec_add_ms(&ts, 200);
			pthread_cond_timedwait(&q->cond_full, &q->mutex_full, &ts);
		}
	} else {
		/* Fill a multi-buffer wave before hashing. */
		while (count_full_locked(q) < max_n && !q->done && !force_exit
				&& !q->cleanup_in_progress) {
			clock_gettime(CLOCK_REALTIME, &ts);
			timespec_add_ms(&ts, 200);
			pthread_cond_timedwait(&q->cond_full, &q->mutex_full, &ts);
		}
	}

	if (q->cleanup_in_progress || force_exit
			|| (q->full == NULL && q->done)) {
		pthread_mutex_unlock(&q->mutex_full);
		return 0;
	}

	if (q->full == NULL) {
		pthread_mutex_unlock(&q->mutex_full);
		return 0;
	}

	while (n < max_n && q->full != NULL && !q->cleanup_in_progress) {
		p = q->full;
		q->full = p->next;
		out[n++] = p;
	}

	pthread_mutex_unlock(&q->mutex_full);
	return n;
}

/* Return a hashed batch to the free list. */
static void put_free_batch(struct queue *q, struct piece **batch, unsigned n)
{
	unsigned i;

	if (n == 0)
		return;

	pthread_mutex_lock(&q->mutex_free);
	if (q->cleanup_in_progress) {
		pthread_mutex_unlock(&q->mutex_free);
		return;
	}
	for (i = 0; i < n; i++)
		recycle_piece_locked(q, batch[i]);
	q->pieces_hashed += n;
	pthread_mutex_unlock(&q->mutex_free);
	pthread_cond_broadcast(&q->cond_empty);
}

/*
 * Lean OpenSSL/bundled path: one piece at a time (matches Rudde upstream shape).
 * Returns 1 if a piece was hashed, 0 if the worker should exit.
 */
static int worker_one_piece(struct queue *q)
{
	struct piece *p;
	struct timespec ts;

	pthread_mutex_lock(&q->mutex_full);
	if (q->cleanup_in_progress) {
		pthread_mutex_unlock(&q->mutex_full);
		return 0;
	}
	while (q->full == NULL && !q->done && !force_exit && !q->cleanup_in_progress) {
		clock_gettime(CLOCK_REALTIME, &ts);
		timespec_add_ms(&ts, 200);
		pthread_cond_timedwait(&q->cond_full, &q->mutex_full, &ts);
	}
	if (q->cleanup_in_progress || force_exit || (q->full == NULL && q->done)) {
		pthread_mutex_unlock(&q->mutex_full);
		return 0;
	}
	if (q->full == NULL) {
		pthread_mutex_unlock(&q->mutex_full);
		return 1; /* spuriously empty; keep going */
	}
	p = q->full;
	q->full = p->next;
	pthread_mutex_unlock(&q->mutex_full);

	if (q->cleanup_in_progress)
		return 0;

	mkt_sha1_piece(p->data, p->len, p->dest);
	put_free_batch(q, &p, 1);
	return 1;
}

/* Worker thread function that processes the pieces in the queue */
static void *worker(void *arg)
{
	struct queue *q = (struct queue *)arg;
	unsigned max_batch = mkt_sha1_max_batch();
	struct piece *batch[16]; /* ISAL_SHA1_MAX_LANES == 16 when USE_ISAL */
	const void *data_ptrs[16];
	size_t lens[16];
	unsigned char *outs[16];
	unsigned n;
	unsigned i;

	/* OpenSSL / bundled: lean single-piece loop (no batch machinery). */
	if (max_batch <= 1) {
		while (worker_one_piece(q))
			;
		return NULL;
	}

	if (max_batch > 16)
		max_batch = 16;

	while (1) {
		n = get_full_batch(q, batch, max_batch);
		if (n == 0) {
			if (q->cleanup_in_progress || force_exit || q->done)
				return NULL;
			continue;
		}

		if (q->cleanup_in_progress)
			return NULL;

		for (i = 0; i < n; i++) {
			data_ptrs[i] = batch[i]->data;
			lens[i] = batch[i]->len;
			outs[i] = batch[i]->dest;
		}
		mkt_sha1_pieces_mb(data_ptrs, lens, outs, n);
		put_free_batch(q, batch, n);
	}

	return NULL;
}

/* Thread function that displays progress information */
static void *print_progress(void *arg)
{
    struct queue *q = (struct queue *)arg;
    int percent_complete;
    struct timespec req = {0, 0};
    
    /* Setup for clean thread cancellation */
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

    req.tv_sec = 0;
    req.tv_nsec = PROGRESS_PERIOD * 1000;

    while (1) {
        /* Check if cleanup is in progress */
        if (q->cleanup_in_progress) {
            return NULL;
        }
        
        /* Set a cancellation point */
        pthread_testcancel();
        
        nanosleep(&req, NULL);

        /* Set another cancellation point */
        pthread_testcancel();
        
        /* Check again if cleanup is in progress */
        if (q->cleanup_in_progress) {
            return NULL;
        }

        pthread_mutex_lock(&q->mutex_free);
        /* Check if cleanup started while waiting for lock */
        if (q->cleanup_in_progress) {
            pthread_mutex_unlock(&q->mutex_free);
            return NULL;
        }
        
        if (q->pieces) {
            percent_complete = (q->pieces_hashed * 100) / q->pieces;
            fprintf(stderr, "\rHashing: %d%%", percent_complete);
            fflush(stderr);
        }
        
        if (q->done) {
            if (q->pieces && q->pieces_hashed == q->pieces)
                fprintf(stderr, "\rHashing: 100%%\n");
            pthread_mutex_unlock(&q->mutex_free);
            fflush(stderr);
            return NULL;
        }
        pthread_mutex_unlock(&q->mutex_free);
    }

    return NULL;
}

#ifdef USE_IO_URING
/* Completion callback state for process_file_iouring */
struct iouring_done {
	struct queue *q;
	struct metafile *m;
	unsigned char **pos_ptr;
	int error;
	/* Last short piece (end of this file) held for cross-file carry */
	struct piece *tail;
	size_t tail_len;
};

/* Reclaim a piece buffer after cancel/error drain (never put_full). */
static void iouring_on_drain_cqe(void *user_data, int res, void *cb_arg)
{
	struct piece *p = user_data;
	struct iouring_done *st = cb_arg;

	(void)res;
	if (p == NULL)
		return;
	put_free(st->q, p, 0);
}

static void iouring_on_cqe(void *user_data, int res, void *cb_arg)
{
	struct piece *p = user_data;
	struct iouring_done *st = cb_arg;
	size_t expected;

	if (p == NULL)
		return;

	/* Already failing: still reclaim buffers from in-flight CQEs. */
	if (st->error) {
		put_free(st->q, p, 0);
		return;
	}

	expected = p->len;

	if (res < 0) {
		fprintf(stderr, "io_uring read failed: %s\n", strerror(-res));
		st->error = 1;
		put_free(st->q, p, 0);
		return;
	}

	if ((size_t)res != expected) {
		/* Short read: treat as end-of-file tail (or error if zero mid-stream) */
		if (res == 0 && expected > 0) {
			fprintf(stderr, "io_uring unexpected EOF\n");
			st->error = 1;
			put_free(st->q, p, 0);
			return;
		}
		p->len = (unsigned long)res;
		if (st->tail) {
			/* Should only have one tail per file */
			fprintf(stderr, "io_uring: multiple short reads\n");
			st->error = 1;
			put_free(st->q, p, 0);
			return;
		}
		st->tail = p;
		st->tail_len = (size_t)res;
		return;
	}

	/* Full piece — dest was set at submit time */
	put_full(st->q, p);
}

/*
 * Read a file via io_uring into piece buffers (no mmap). Overlaps disk reads with
 * SHA-1 workers. Preserves cross-file partial piece in *r_ptr / *p_ptr.
 */
static int process_file_iouring(struct file_data *f, struct metafile *m,
				struct queue *q, unsigned char **pos_ptr,
				size_t *r_ptr, struct piece **p_ptr)
{
	int fd;
	int open_flags = OPENFLAGS;
	int result = 0;
	size_t r = *r_ptr;
	struct piece *p = *p_ptr;
	unsigned char *pos = *pos_ptr;
	off_t file_off = 0;
	uintmax_t remaining = f->size;
	unsigned depth;
	struct mkt_uring *ring = NULL;
	struct iouring_done st;

	if (p == NULL) {
		fprintf(stderr, "Error: NULL piece pointer passed to process_file_iouring\n");
		return -1;
	}

	if (f->size > (uintmax_t)OFF_MAX) {
		fprintf(stderr, "Error: '%s' is larger than off_t can address\n",
			file_read_path(f));
		return -1;
	}

	if (m->direct_io && f->size >= DIRECT_IO_THRESHOLD && O_DIRECT != 0)
		open_flags |= O_DIRECT;

	fd = open(file_read_path(f), open_flags);
	if (fd == -1) {
		if (errno == EINVAL && (open_flags & O_DIRECT)) {
			fprintf(stderr, "Warning: Direct I/O not supported for '%s', retrying without it\n",
				f->path);
			open_flags &= ~O_DIRECT;
			fd = open(file_read_path(f), open_flags);
		}
		if (fd == -1) {
			fprintf(stderr, "Cannot open '%s' for reading: %s\n",
				file_read_path(f), strerror(errno));
			return -1;
		}
	}

	apply_file_access_hints(fd, (size_t)f->size);

	if (m->verbose && !m->json_output && !m->quiet) {
		fprintf(stderr, "\rProcessing file (io_uring): %s (%" PRIuMAX " bytes)      ",
			file_read_path(f), f->size);
		fflush(stderr);
	}

	depth = m->io_uring_depth ? m->io_uring_depth : MKTORRENT_IOURING_DEFAULT_DEPTH;
	if (depth > q->buffers_max)
		depth = q->buffers_max;
	if (depth > MKTORRENT_IOURING_MAX_DEPTH)
		depth = MKTORRENT_IOURING_MAX_DEPTH;
	if (depth < 1)
		depth = 1;

	{
		unsigned uflags = 0;

		if (m->io_uring_sqpoll)
			uflags |= MKT_URING_F_SQPOLL;
		ring = mkt_uring_create(depth, uflags);
	}
	if (!ring) {
		fprintf(stderr, "io_uring setup failed%s: %s\n",
			m->io_uring_sqpoll ? " (sqpoll)" : "", strerror(errno));
		close(fd);
		return -1;
	}

	memset(&st, 0, sizeof(st));
	st.q = q;
	st.m = m;
	st.pos_ptr = pos_ptr;

	/* Finish a cross-file partial piece first (single read, then pipeline). */
	if (r > 0 && remaining > 0) {
		size_t need = m->piece_length - r;
		ssize_t n;

		if (need > remaining)
			need = (size_t)remaining;

		/* Use synchronous read for the partial head — keeps dest/pos simple. */
		n = robust_read(fd, p->data + r, need);
		if (n < 0) {
			fprintf(stderr, "Cannot read from '%s': %s\n",
				file_read_path(f), strerror(errno));
			result = -1;
			goto out;
		}
		r += (size_t)n;
		remaining -= (uintmax_t)n;
		file_off += (off_t)n;

		if (r == m->piece_length) {
			p->dest = pos;
			p->len = m->piece_length;
			put_full(q, p);
			pos += SHA_DIGEST_LENGTH;
			r = 0;
			p = get_free(q, m->piece_length);
			if (!p) {
				result = -1;
				goto out;
			}
		}
	}

	/* Pipeline full (and final short) piece reads. Reuse caller's free piece when r==0. */
	while ((remaining > 0 || mkt_uring_inflight(ring) > 0) && !force_exit && !st.error) {
		while (remaining > 0 &&
		       mkt_uring_inflight(ring) < mkt_uring_depth(ring) &&
		       !force_exit && !st.tail) {
			size_t to_read;
			struct piece *np;

			if (r > 0)
				break;

			if (p) {
				np = p;
				p = NULL;
			} else {
				np = get_free(q, m->piece_length);
				if (!np)
					break;
			}

			to_read = m->piece_length;
			if (to_read > remaining)
				to_read = (size_t)remaining;

			/* O_DIRECT short/unaligned tail: buffered pread */
			if ((open_flags & O_DIRECT) && to_read < m->piece_length) {
				ssize_t n = pread(fd, np->data, to_read, file_off);
				if (n < 0) {
					fprintf(stderr, "Cannot pread '%s': %s\n",
						file_read_path(f), strerror(errno));
					put_free(q, np, 0);
					st.error = 1;
					break;
				}
				file_off += n;
				remaining -= (uintmax_t)n;
				st.tail = np;
				st.tail_len = (size_t)n;
				continue;
			}

			np->len = (unsigned long)to_read; /* expected until CQE */
			if (to_read == m->piece_length) {
				np->dest = pos;
				pos += SHA_DIGEST_LENGTH;
			}

			if (mkt_uring_submit_read(ring, fd, np->data, to_read,
						  file_off, np) != 0) {
				fprintf(stderr, "io_uring submit failed: %s\n", strerror(errno));
				put_free(q, np, 0);
				st.error = 1;
				break;
			}
			file_off += (off_t)to_read;
			remaining -= (uintmax_t)to_read;
		}

		if (mkt_uring_inflight(ring) > 0) {
			if (mkt_uring_reap(ring, 1, iouring_on_cqe, &st) < 0) {
				st.error = 1;
				break;
			}
		} else if (remaining == 0) {
			break;
		}
	}

	if (force_exit || st.error) {
		/* Reclaim every in-flight piece before destroy/free_buffers. */
		mkt_uring_cancel_drain(ring, iouring_on_drain_cqe, &st);
		result = -1;
		if (st.tail) {
			put_free(q, st.tail, 0);
			st.tail = NULL;
		}
		if (p) {
			put_free(q, p, 0);
			p = NULL;
		}
		*p_ptr = NULL;
		*r_ptr = 0;
		goto out;
	}

	/* Carry partial piece to next file (or final irregular piece). */
	if (st.tail) {
		if (p)
			put_free(q, p, 0);
		*p_ptr = st.tail;
		*r_ptr = st.tail_len;
		*pos_ptr = pos;
	} else if (r > 0 && p) {
		*p_ptr = p;
		*r_ptr = r;
		*pos_ptr = pos;
	} else if (p) {
		*p_ptr = p;
		*r_ptr = 0;
		*pos_ptr = pos;
	} else {
		/* All pieces queued; need a fresh buffer for a possible next file */
		p = get_free(q, m->piece_length);
		*p_ptr = p;
		*r_ptr = 0;
		*pos_ptr = pos;
		if (!p)
			result = -1;
	}

out:
	mkt_uring_destroy(ring);
	close(fd);
	return result;
}
#endif /* USE_IO_URING */

/*
 * Process a file using memory mapping for large files, or read() for smaller ones
 * Returns 0 on success, -1 on error
 */
static int process_file(struct file_data *f, struct metafile *m, 
                        struct queue *q, unsigned char **pos_ptr,
                        size_t *r_ptr, struct piece **p_ptr)
{
#ifdef USE_IO_URING
	if (m->io_uring)
		return process_file_iouring(f, m, q, pos_ptr, r_ptr, p_ptr);
#endif

    int fd;
    size_t r = *r_ptr;
    struct piece *p = *p_ptr;
    unsigned char *pos = *pos_ptr;
    int result = 0;
    int open_flags = OPENFLAGS;
    int retry_count = 0;
    
    /* Check for NULL piece pointer */
    if (p == NULL) {
        fprintf(stderr, "Error: NULL piece pointer passed to process_file\n");
        return -1;
    }

    /* MK-014: refuse sizes that cannot be addressed with off_t */
    if (f->size > (uintmax_t)OFF_MAX) {
        fprintf(stderr, "Error: '%s' is larger than off_t can address\n",
            file_read_path(f));
        return -1;
    }
    
    /* O_DIRECT is opt-in via --direct-io (buffered I/O is faster on warm cache) */
    if (m->direct_io && f->size >= DIRECT_IO_THRESHOLD && O_DIRECT != 0) {
        open_flags |= O_DIRECT;
    }
    
retry_open:

    /* open the current file for reading */
    fd = open(file_read_path(f), open_flags);
    if (fd == -1) {
        /* If direct I/O failed, retry without it */
        if (errno == EINVAL && (open_flags & O_DIRECT)) {
            fprintf(stderr, "Warning: Direct I/O not supported for '%s', retrying without it\n", f->path);
            open_flags &= ~O_DIRECT;
            goto retry_open;
        }
        
        /* If we should retry on error */
        if (retry_count < MAX_RETRY_COUNT && 
            (errno == EAGAIN || errno == EBUSY || errno == ENFILE || 
             errno == EMFILE || errno == EINTR)) {
            retry_count++;
            /* Exponential backoff with jitter */
            unsigned int delay = RETRY_DELAY_BASE * (1 << (retry_count - 1));
            /* Add some randomness to avoid thundering herd */
            delay += (rand() % delay) / 2;
            fprintf(stderr, "Warning: Temporary failure opening '%s' (retry %d/%d): %s\n", 
                    f->path, retry_count, MAX_RETRY_COUNT, strerror(errno));
            usleep(delay);
            goto retry_open;
        }
        
        fprintf(stderr, "Cannot open '%s' for reading: %s\n",
            file_read_path(f), strerror(errno));
        return -1;
    }
    
    /* Get file stats */
    struct stat file_stat;
    if (fstat(fd, &file_stat) != 0) {
        fprintf(stderr, "Cannot stat '%s': %s\n",
            file_read_path(f), strerror(errno));
        close(fd);
        return -1;
    }
    
    /* Apply file access pattern hints */
    apply_file_access_hints(fd, file_stat.st_size);
    
    /* Verbose output (never mix with JSON stdout) */
    if (m->verbose && !m->json_output && !m->quiet) {
        fprintf(stderr, "\rProcessing file: %s (%" PRIuMAX " bytes)      ",
            file_read_path(f), f->size);
        fflush(stderr);
    }

    /*
     * Multi-thread default: mmap and hash piece views in place (zero-copy).
     * Single-thread stays on owned-buffer read() — mmap views regress t=1
     * warm throughput vs OpenSSL+read on measured hosts. Also fall back for
     * --direct-io, mmap failure, and cross-file leftovers. --io-uring stays
     * the cold-disk overlap path (handled above).
     */
    if (m->threads > 1 && !(open_flags & O_DIRECT) && file_stat.st_size > 0
		&& (uintmax_t)file_stat.st_size == f->size
		&& (uintmax_t)file_stat.st_size <= (uintmax_t)SIZE_MAX) {
	void *file_map = mmap(NULL, (size_t)file_stat.st_size, PROT_READ,
			MAP_PRIVATE, fd, 0);

	if (file_map != MAP_FAILED) {
		unsigned char *base = file_map;
		size_t off = 0;
		size_t rem = (size_t)file_stat.st_size;

#ifdef MADV_SEQUENTIAL
		(void)madvise(file_map, rem, MADV_SEQUENTIAL);
#endif
		/* Finish a partial owned piece from the previous file. */
		while (r > 0 && rem > 0) {
			size_t need = m->piece_length - r;

			if (need > rem)
				need = rem;
			memcpy(p->data + r, base + off, need);
			r += need;
			off += need;
			rem -= need;
			if (r == m->piece_length) {
				p->dest = pos;
				p->len = m->piece_length;
				put_full(q, p);
				pos += SHA_DIGEST_LENGTH;
				r = 0;
				if (force_exit) {
					result = -1;
					goto zc_done;
				}
				p = get_free(q, m->piece_length);
				if (!p) {
					result = -1;
					goto zc_done;
				}
			}
		}

		/* Full pieces wholly inside this mapping — no memcpy. */
		while (rem >= m->piece_length) {
			if (put_full_zc(q, base + off, m->piece_length, pos) != 0) {
				result = -1;
				goto zc_done;
			}
			off += m->piece_length;
			rem -= m->piece_length;
			pos += SHA_DIGEST_LENGTH;
			if (force_exit) {
				result = -1;
				goto zc_done;
			}
		}

		/* Trailing bytes carry into the owned buffer for the next file. */
		if (rem > 0) {
			if (!p) {
				p = get_free(q, m->piece_length);
				if (!p) {
					result = -1;
					goto zc_done;
				}
				r = 0;
			}
			memcpy(p->data + r, base + off, rem);
			r += rem;
		}

		if (register_map(q, file_map, (size_t)file_stat.st_size) != 0) {
			fprintf(stderr, "Error: Out of memory tracking mmap for '%s'\n",
				f->path);
			munmap(file_map, (size_t)file_stat.st_size);
			result = -1;
			goto zc_done;
		}
		/* Mapping stays until free_buffers after workers join. */
		file_map = NULL;

zc_done:
		if (file_map)
			munmap(file_map, (size_t)file_stat.st_size);
		close(fd);
		*r_ptr = r;
		*p_ptr = p;
		*pos_ptr = pos;
		return result;
	}
	/* mmap failed — fall through to read(). */
    }

    /* Owned-buffer read() path (direct I/O, tiny/unmappable files, fallback). */
    {
    uintmax_t remaining_file_size = f->size;
    retry_count = 0;

    while (remaining_file_size > 0) {
        size_t to_read = m->piece_length - r;
        unsigned char *read_pos;
        ssize_t bytes_read;

        if (to_read > remaining_file_size)
            to_read = (size_t)remaining_file_size;

        read_pos = p->data + r;
        bytes_read = robust_read(fd, read_pos, to_read);

        if (bytes_read < 0) {
            if (retry_count < MAX_RETRY_COUNT) {
                unsigned int delay;

                retry_count++;
                delay = RETRY_DELAY_BASE * (1 << (retry_count - 1));
                delay += (rand() % delay) / 2;
                fprintf(stderr, "Warning: Read error on '%s' (retry %d/%d): %s\n",
                    f->path, retry_count, MAX_RETRY_COUNT, strerror(errno));
                usleep(delay);
                continue;
            }
            fprintf(stderr, "Cannot read from '%s': %s\n",
                f->path, strerror(errno));
            close(fd);
            result = -1;
            break;
        }

        if (bytes_read == 0)
            break;

        r += (size_t)bytes_read;
        remaining_file_size -= (uintmax_t)bytes_read;
        retry_count = 0;

        if (r == m->piece_length) {
            p->dest = pos;
            p->len = m->piece_length;
            put_full(q, p);
            pos += SHA_DIGEST_LENGTH;
            r = 0;

            if (force_exit) {
                result = -1;
                break;
            }

            p = get_free(q, m->piece_length);
            if (p == NULL) {
                fprintf(stderr, "Error: Failed to get buffer while reading '%s'\n",
                    f->path);
                result = -1;
                break;
            }
        }
    }

    if (close(fd) != 0) {
        fprintf(stderr, "Cannot close '%s': %s\n",
            f->path, strerror(errno));
    }

    *r_ptr = r;
    *p_ptr = p;
    *pos_ptr = pos;
    return result;
    }
}

static void read_files(struct metafile *m, struct queue *q, unsigned char *pos)
{
    size_t r = 0;          /* number of bytes read from file(s)
                              into the read buffer */
#ifndef NO_HASH_CHECK
    uintmax_t counter = 0; /* number of bytes hashed
                              should match size when done */
#endif
	struct piece *p = get_free(q, m->piece_length);
    int file_count = 0;
    
    /* Check if get_free failed (interrupt or cleanup) */
    if (p == NULL) {
        fprintf(stderr, "Error: Failed to get buffer for processing. Aborting.\n");
        force_exit = 1;
        return;
    }

    /* go through all the files in the file list */
    LL_FOR(file_node, m->file_list) {
        struct file_data *f = LL_DATA_AS(file_node, struct file_data*);
        file_count++;

        if (process_file(f, m, q, &pos, &r, &p) != 0) {
            if (p) {  /* Check if p is not NULL before using it */
                put_free(q, p, 0);
            }
            return;
        }

#ifndef NO_HASH_CHECK
        counter += f->size;
#endif

        /* Check if we should abort due to user interrupt */
        if (force_exit) {
            if (p) {  /* Check if p is not NULL before using it */
                put_free(q, p, 0);
            }
            return;
        }
        
        /* Check if p became NULL during processing (interrupt/cleanup) */
        if (p == NULL) {
            fprintf(stderr, "Error: Lost buffer during processing. Aborting.\n");
            force_exit = 1;
            return;
        }
    }

    /* finally append the hash of the last irregular piece to the hash string */
    if (r) {
        if (p) {  /* Check if p is not NULL before using it */
            p->dest = pos;
            p->len = r;
            put_full(q, p);
        }
#ifndef NO_HASH_CHECK
        /* counter already includes this piece */
#endif
    } else {
        if (p) {  /* Check if p is not NULL before using it */
            put_free(q, p, 0);
        }
    }

#ifndef NO_HASH_CHECK
    if (counter != m->size) {
        fprintf(stderr, "Counted %" PRIuMAX " bytes, but hashed %" PRIuMAX " bytes; "
            "something is wrong...\n", m->size, counter);
        force_exit = 1;
    }
#endif
}

/* Get available system memory */
static size_t get_available_memory(void)
{
    size_t available_memory = 0;
    
#ifdef _SC_PHYS_PAGES
#ifdef _SC_PAGESIZE
    /* Use sysconf to get physical memory */
    long phys_pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGESIZE);
    
    if (phys_pages > 0 && page_size > 0) {
        available_memory = (size_t)phys_pages * (size_t)page_size;
    }
#endif
#endif

    /* If sysconf fails or isn't available, use a conservative default */
    if (available_memory == 0) {
        /* Default to 1GB if we can't determine */
        available_memory = 1024 * 1024 * 1024;
        fprintf(stderr, "Warning: Could not determine available memory, using default of 1GB\n");
    }
    
    return available_memory;
}

/* Calculate memory limits based on available memory and thread count */
static size_t calculate_memory_limit(long thread_count)
{
    /* Get total physical memory */
    size_t available_memory = get_available_memory();
    
    /* Default percentage to use */
    int max_percent = DEFAULT_MAX_MEMORY_PERCENT;
    
    /* Check if MKTORRENT_MAX_MEMORY_PERCENT environment variable is set */
    const char *max_memory_percent_str = getenv("MKTORRENT_MAX_MEMORY_PERCENT");
    if (max_memory_percent_str != NULL) {
        int percent = atoi(max_memory_percent_str);
        if (percent > 0 && percent <= 100) {
            max_percent = percent;
        } else {
            fprintf(stderr, "Warning: Invalid MKTORRENT_MAX_MEMORY_PERCENT value, using default\n");
        }
    }
    
    /* Calculate maximum memory to use */
    size_t max_memory = (available_memory * max_percent) / 100;
    
    /* Calculate minimum memory needed per thread */
    size_t min_memory_needed = thread_count * MIN_MEMORY_PER_THREAD;
    
    /* Ensure we have at least the minimum needed */
    if (max_memory < min_memory_needed) {
        fprintf(stderr, "Warning: Available memory (%zu MB) may be too low for optimal performance with %ld threads\n", 
            max_memory / (1024 * 1024), thread_count);
        /* Still use what we have */
        return max_memory;
    }
    
    return max_memory;
}

/* Set resource limits for the process */
static void set_resource_limits(size_t memory_limit)
{
#ifdef RLIMIT_AS
    struct rlimit rlim;
    
    /* Get current limits */
    if (getrlimit(RLIMIT_AS, &rlim) != 0) {
        fprintf(stderr, "Warning: Failed to get resource limits: %s\n", strerror(errno));
        return;
    }
    
    /* Set the memory limit if it's lower than the current limit or if current limit is unlimited */
    if (rlim.rlim_cur == RLIM_INFINITY || (memory_limit > 0 && memory_limit < rlim.rlim_cur)) {
        rlim.rlim_cur = memory_limit;
        
        if (setrlimit(RLIMIT_AS, &rlim) != 0) {
            fprintf(stderr, "Warning: Failed to set memory limit: %s\n", strerror(errno));
        }
    }
#endif
}

EXPORT unsigned char *make_hash(struct metafile *m)
{
	struct queue q = {
		NULL, NULL, 0, 0,
		PTHREAD_MUTEX_INITIALIZER,
		PTHREAD_MUTEX_INITIALIZER,
		PTHREAD_COND_INITIALIZER,
		PTHREAD_COND_INITIALIZER,
		0, 0, 0,
		0, NULL, /* zc_inflight, maps */
		m->verbose,
		0  /* cleanup_in_progress */
	};
	pthread_t print_progress_thread;	/* progress printer thread */
	pthread_t *workers;
	unsigned char *hash_string;		/* the hash string */
	int i;
	int err;
	/* ISA-L multi-buffer saturates 1–2 cores; cap workers so lanes stay full. */
	long hash_threads = m->threads;
	size_t buffers_per_thread;
	size_t memory_limit;
	size_t buffer_size_per_thread;

	if (mkt_sha1_using_isal()) {
		if (hash_threads > 2)
			hash_threads = 2;
		if (hash_threads < 1)
			hash_threads = 1;
	}

	memory_limit = calculate_memory_limit(hash_threads);
	set_resource_limits(memory_limit);
	buffer_size_per_thread = m->piece_length + sizeof(struct piece);

	if (mkt_sha1_using_isal()) {
		/*
		 * ISA-L waits for a full multi-buffer wave (up to 16 pieces).
		 * Ensure ≥2 waves in the pool — not the OpenSSL 3×t regime.
		 */
		size_t need = (size_t)mkt_sha1_max_batch() * 2;
		buffers_per_thread = (need + (size_t)hash_threads - 1)
			/ (size_t)hash_threads;
		if (buffers_per_thread < 2)
			buffers_per_thread = 2;
	} else {
		/* Rudde 1.1: buffers_max = 3 * threads */
		buffers_per_thread = BUFFERS_PER_THREAD_OPENSSL;
	}
	
	/* Seed random number generator for retry jitter */
	srand(time(NULL));

	workers = malloc((size_t)hash_threads * sizeof(pthread_t));
	hash_string = malloc(m->pieces * SHA_DIGEST_LENGTH);
	if (workers == NULL || hash_string == NULL) {
		fprintf(stderr, "Error: Out of memory allocating resources\n");
		free(workers);
		free(hash_string);
		return NULL;
	}

	/* Calculate estimated memory usage */
	size_t estimated_memory = ((size_t)hash_threads * buffers_per_thread * buffer_size_per_thread) + 
		(m->pieces * SHA_DIGEST_LENGTH);
	int align_pages = (m->direct_io || m->io_uring) ? 1 : 0;
	
	if (m->verbose && !m->json_output && !m->quiet) {
		fprintf(stderr, "Memory information:\n");
		fprintf(stderr, "  Memory limit: %zu MB\n", memory_limit / (1024 * 1024));
		fprintf(stderr, "  Estimated usage: %zu MB\n", estimated_memory / (1024 * 1024));
		fprintf(stderr, "  Hash workers: %ld", hash_threads);
		if (mkt_sha1_using_isal() && hash_threads != m->threads)
			fprintf(stderr, " (ISA-L capped from -t %ld)", m->threads);
		fprintf(stderr, "\n");
		fprintf(stderr, "  Buffers per thread: %zu\n", buffers_per_thread);
		if (align_pages)
			fprintf(stderr, "  Piece buffers: page-aligned\n");
	}

	q.pieces = m->pieces;
	q.buffers_max = buffers_per_thread * (size_t)hash_threads;
	
    /* Initialize piece buffers BEFORE starting worker threads */
    size_t total_buffers = buffers_per_thread * (size_t)hash_threads;
    if (m->verbose && !m->json_output && !m->quiet)
        fprintf(stderr, "Initializing %zu piece buffers...\n", total_buffers);
    
    for (size_t buf_index = 0; buf_index < total_buffers; buf_index++) {
        struct piece *p = alloc_piece(m->piece_length, align_pages);
        if (p == NULL) {
            fprintf(stderr, "Error: Out of memory allocating piece buffer\n");
            
            /* Free any buffers we managed to allocate */
            free_buffers(&q);
            
            /* Clean up other resources */
            free(workers);
            free(hash_string);
            
            return NULL;
        }
        
        /* Add the buffer to the free queue */
        p->next = q.free;
        q.free = p;
        q.buffers++;
    }
    
    if (m->verbose && !m->json_output && !m->quiet) {
        fprintf(stderr, "Successfully initialized %u piece buffers\n", q.buffers);
    }

	/* create worker threads */
	for (i = 0; i < hash_threads; i++) {
		err = pthread_create(&workers[i], NULL, worker, &q);
		if (err) {
			fprintf(stderr, "Error: Cannot create thread: %s\n", strerror(err));
			/* Terminate any already created threads */
			while (--i >= 0) {
				pthread_cancel(workers[i]);
				pthread_join(workers[i], NULL);
			}
			
			free(workers);
			free(hash_string);
			free_buffers(&q);
			return NULL;
		}
		/* No CPU affinity pin by default — pinning did not help vs Rudde A/B. */
	}

	/* Progress printer pollutes stdout — skip in JSON mode */
	int progress_started = 0;
	if (!m->json_output && !m->quiet) {
		err = pthread_create(&print_progress_thread, NULL, print_progress, &q);
		if (err) {
			fprintf(stderr, "Error: Cannot create progress thread: %s\n", strerror(err));
			for (i = 0; i < hash_threads; i++) {
				pthread_cancel(workers[i]);
				pthread_join(workers[i], NULL);
			}

			free(workers);
			free(hash_string);
			free_buffers(&q);
			return NULL;
		}
		progress_started = 1;
	}

	/* read files and feed pieces to the workers */
	read_files(m, &q, hash_string);

	/* we're done, let the pieces in the queue be hashed and collected */
	set_done(&q);

	/* wait for the worker threads to signal completion */
	for (i = 0; i < hash_threads; i++) {
		int join_result = pthread_join(workers[i], NULL);
		if (join_result != 0) {
			fprintf(stderr, "Warning: Failed to join worker thread %d: %s\n", 
					i, strerror(join_result));
		}
	}

	if (progress_started) {
		/* Progress thread exits when q->done is set; join without cancel. */
		if (pthread_join(print_progress_thread, NULL) != 0) {
			fprintf(stderr, "Warning: Failed to join progress thread\n");
		}
	}

	/* free worker threads */
	free(workers);

	/* free all the read buffers left in the queue */
	free_buffers(&q);

	/* return the generated hash string */
	return hash_string;
}
