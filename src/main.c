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


#include <stdlib.h>      /* exit(), srandom() */
#include <errno.h>       /* errno */
#include <string.h>      /* strerror() */
#include <stdio.h>       /* printf() etc. */
#include <sys/stat.h>    /* S_IRUSR, S_IWUSR, S_IRGRP, S_IROTH */
#include <fcntl.h>       /* open() */
#include <unistd.h>      /* close(), read() */
#include <time.h>        /* clock_gettime() */
#include <signal.h>      /* signal handling */
#include <inttypes.h>    /* PRIuMAX etc. */
#include <pthread.h>

/* Define constants that may be missing */
#ifndef MAX_OPENFD
#define MAX_OPENFD 100
#endif

#ifndef PROGRESS_PERIOD
#define PROGRESS_PERIOD 200000
#endif

/* Define clock types if not already defined */
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

#include "export.h"
#include "mktorrent.h"
#include "init.h"
#include "hash.h"
#include "output.h"
#include "msg.h"
#include "ll.h"
#include "sha1_backend.h"

#ifdef ALLINONE
/* include all .c files in alphabetical order */

#include "ftw.c"

#ifdef USE_PTHREADS
#include "hash_pthreads.c"
#else
#include "hash.c"
#endif

#include "init.c"
#include "ll.c"
#include "msg.c"
#include "output.c"

#ifndef USE_OPENSSL
#include "sha1.c"
#endif

#endif /* ALLINONE */

#ifndef O_BINARY
#define O_BINARY 0
#endif

#ifndef S_IRGRP
#define S_IRGRP 0
#endif

#ifndef S_IROTH
#define S_IROTH 0
#endif

#ifndef VERSION
#define VERSION "unspecified_version"
#endif

#ifndef BUILD_CFG
#define BUILD_CFG "unspecified_build_type"
#endif

/* global variables — async-signal-safe flag only */
EXPORT volatile sig_atomic_t force_exit = 0;

/* Signal handler for graceful exit (async-signal-safe: no I/O). */
static void handle_signal(int signum)
{
	if (signum == SIGINT || signum == SIGTERM)
		force_exit = 1;
}

/*
 * create and open the metainfo file for writing and create a stream for it
 * we don't want to overwrite anything, so abort if the file is already there
 * and force is false
 */
static FILE *open_file(const char *path, int force)
{
	int fd;  /* file descriptor */
	FILE *f; /* file stream */

	if (!path || !*path) {
		fprintf(stderr, "Error: Invalid path specified\n");
		return NULL;
	}

	int flags = O_WRONLY | O_BINARY | O_CREAT;
	if (!force)
		flags |= O_EXCL;
	else
		flags |= O_TRUNC; /* MK-012: avoid stale tail on shorter overwrite */

	/* open and create the file if it doesn't exist already */
	fd = open(path, flags, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (fd < 0) {
		fprintf(stderr, "Error: Cannot create '%s': %s\n", path, strerror(errno));
		return NULL;
	}

	/* create the stream from this filedescriptor */
	f = fdopen(fd, "wb");
	if (f == NULL) {
		close(fd);  /* Don't leak file descriptor if fdopen fails */
		fprintf(stderr, "Error: Cannot create stream for '%s': %s\n",
            path, strerror(errno));
		return NULL;
	}

	return f;
}

/*
 * close the metainfo file
 */
static int close_file(FILE *f)
{
	if (!f) return 0;  /* Accept NULL pointers */

	/* close the metainfo file */
	if (fclose(f) != 0) {
		fprintf(stderr, "Error: Cannot close stream: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

/*
 * Count the number of nodes in a linked list
 */
static unsigned int count_ll_nodes(struct ll *list)
{
	unsigned int count = 0;

	if (!list) return 0;

	LL_FOR(node, list) {
		count++;
	}

	return count;
}

/* Return UTF-8 sequence length (1-4) or 0 if invalid at s. */
static int utf8_seq_len(const unsigned char *s)
{
	unsigned char c = s[0];

	if (c < 0x80)
		return 1;
	if ((c & 0xe0) == 0xc0) {
		if ((s[1] & 0xc0) != 0x80 || c < 0xc2)
			return 0;
		return 2;
	}
	if ((c & 0xf0) == 0xe0) {
		if ((s[1] & 0xc0) != 0x80 || (s[2] & 0xc0) != 0x80)
			return 0;
		if (c == 0xe0 && s[1] < 0xa0)
			return 0;
		if (c == 0xed && s[1] >= 0xa0)
			return 0;
		return 3;
	}
	if ((c & 0xf8) == 0xf0) {
		if ((s[1] & 0xc0) != 0x80 || (s[2] & 0xc0) != 0x80
		    || (s[3] & 0xc0) != 0x80)
			return 0;
		if (c == 0xf0 && s[1] < 0x90)
			return 0;
		if (c > 0xf4 || (c == 0xf4 && s[1] >= 0x90))
			return 0;
		return 4;
	}
	return 0;
}

/* Emit a JSON string; invalid UTF-8 bytes become \\u00XX (MK-019). */
static void json_print_string(const char *s)
{
	putchar('"');
	if (s) {
		const unsigned char *p = (const unsigned char *)s;
		while (*p) {
			unsigned char c = *p;
			int n;

			switch (c) {
			case '"':
			case '\\':
				putchar('\\');
				putchar(c);
				p++;
				continue;
			case '\b':
				fputs("\\b", stdout);
				p++;
				continue;
			case '\f':
				fputs("\\f", stdout);
				p++;
				continue;
			case '\n':
				fputs("\\n", stdout);
				p++;
				continue;
			case '\r':
				fputs("\\r", stdout);
				p++;
				continue;
			case '\t':
				fputs("\\t", stdout);
				p++;
				continue;
			default:
				break;
			}
			if (c < 0x20) {
				printf("\\u%04x", c);
				p++;
				continue;
			}
			n = utf8_seq_len(p);
			if (n > 0) {
				fwrite(p, 1, (size_t)n, stdout);
				p += n;
			} else {
				printf("\\u%04x", c);
				p++;
			}
		}
	}
	putchar('"');
}

static void json_print_info_hash(const unsigned char *digest)
{
	static const char hex[] = "0123456789abcdef";
	putchar('"');
	for (int i = 0; i < SHA_DIGEST_LENGTH; i++) {
		putchar(hex[digest[i] >> 4]);
		putchar(hex[digest[i] & 0x0f]);
	}
	putchar('"');
}

static void json_print_announce(const struct metafile *m)
{
	int first = 1;
	fputs("[", stdout);
	if (m->announce_list) {
		LL_FOR(tier_node, m->announce_list) {
			struct ll *tier = LL_DATA_AS(tier_node, struct ll*);
			LL_FOR(url_node, tier) {
				const char *url = LL_DATA_AS(url_node, const char*);
				if (!first)
					putchar(',');
				first = 0;
				json_print_string(url);
			}
		}
	}
	fputs("]", stdout);
}

/* Machine-readable create summary (single JSON object on stdout). */
static void print_json_summary(const struct metafile *m)
{
	const char *path = m->metainfo_file_path ? m->metainfo_file_path : "";
	const char *name = m->torrent_name ? m->torrent_name : "";

	fputs("{\"ok\":true,\"path\":", stdout);
	json_print_string(path);
	fputs(",\"name\":", stdout);
	json_print_string(name);
	printf(",\"length\":%" PRIuMAX, m->size);
	printf(",\"pieceLength\":%u", m->piece_length);
	printf(",\"pieces\":%u", m->pieces);
	printf(",\"private\":%s", m->private ? "true" : "false");
	fputs(",\"infoHash\":", stdout);
	if (m->info_hash_valid)
		json_print_info_hash(m->info_hash);
	else
		fputs("null", stdout);
	fputs(",\"announce\":", stdout);
	json_print_announce(m);
	fputs(",\"nodes\":[", stdout);
	if (m->node_list) {
		int first = 1;
		LL_FOR(node_el, m->node_list) {
			struct dht_node *n = LL_DATA_AS(node_el, struct dht_node *);
			if (!first)
				putchar(',');
			first = 0;
			fputs("{\"host\":", stdout);
			json_print_string(n->host);
			printf(",\"port\":%u}", n->port);
		}
	}
	fputs("]}\n", stdout);
	fflush(stdout);
}

static int argv_wants_json(int argc, char *argv[])
{
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--json") == 0
		    || strcmp(argv[i], "-j") == 0
		    || strcmp(argv[i], "-J") == 0)
			return 1;
	}
	return 0;
}

/* Machine-readable failure (single JSON object on stdout). */
static void print_json_error(const char *error, int code)
{
	fputs("{\"ok\":false,\"error\":", stdout);
	json_print_string(error ? error : "unknown error");
	printf(",\"code\":%d}\n", code);
	fflush(stdout);
}

static void maybe_print_json_error(const struct metafile *m, int argc, char *argv[],
				   const char *error, int code)
{
	if (m->json_output || argv_wants_json(argc, argv))
		print_json_error(error, code);
}

/*
 * main().. it starts
 */
int main(int argc, char *argv[])
{
	/* Check for --version first */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-V") == 0) {
			printf("mktorrent %s (c) 2007, 2009 Emil Renner Berthing\n", VERSION);
#ifdef USE_PTHREADS
			printf("sha1=%s", mkt_sha1_backend_name());
#ifdef USE_ISAL
			printf(" (isal available)");
#endif
			printf("\n");
#endif
			return MKTORRENT_EXIT_OK;
		}
	}

	FILE *file = NULL;	/* stream for writing to the metainfo file */
	unsigned char *hash = NULL; /* hash string */
	int exit_code = MKTORRENT_EXIT_OK;
	struct metafile m = {
		.piece_length = 0,
		.piece_length_is_bytes = 0,
		.announce_list = NULL,
		.comment = NULL,
		.torrent_name = NULL,
		.metainfo_file_path = NULL,
		.web_seed_list = NULL,
		.node_list = NULL,
		.target_is_directory = 0,
		.no_creation_date = 0,
		.no_created_by = 0,
		.private = 0,
		.source = NULL,
		.cross_seed = 0,
		.verbose = 0,
		.quiet = 0,
		.json_output = 0,
		.force_overwrite = 0,
		.no_follow_symlinks = 0,
		.skip_hidden = 0,
		.direct_io = 0,
		.io_uring = 0,
		.io_uring_depth = MKTORRENT_IOURING_DEFAULT_DEPTH,
		.io_uring_sqpoll = 0,
		.io_uring_depth_set = 0,
		.io_uring_sqpoll_set = 0,
		.exclude_list = NULL,
		.include_list = NULL,
		.threads = 0,
		.max_files = MKTORRENT_DEFAULT_MAX_FILES,
		.max_bytes = 0,
		.size = 0,
		.file_count = 0,
		.file_list = NULL,
		.pieces = 0,
		.info_hash = {0},
		.info_hash_valid = 0,
		.scan_prefix = NULL,
		.scan_root_abs = NULL,
	};

	/* Set up signal handling */
	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	/* Seed PRNG with current time */
	struct timespec ts;
	if (clock_gettime(CLOCK_REALTIME, &ts) == -1) {
		fprintf(stderr, "Failed to get time: %s\n", strerror(errno));
		return MKTORRENT_EXIT_IO;
	}
	srandom(ts.tv_nsec ^ ts.tv_sec);

	/* Process command line options.
	 * init() returns: 0 continue, 1 clean early exit (-h/--version), else error. */
	int init_result = init(&m, argc, argv);
	if (init_result == 1) {
		cleanup_metafile(&m);
		return MKTORRENT_EXIT_OK;
	}
	if (init_result != 0) {
		fprintf(stderr, "Failed to initialize from command line arguments\n");
		maybe_print_json_error(&m, argc, argv,
				      "Failed to initialize from command line arguments",
				      MKTORRENT_EXIT_USAGE);
		cleanup_metafile(&m);
		return MKTORRENT_EXIT_USAGE;
	}

	/* Check for interrupt */
	if (force_exit) {
		fprintf(stderr, "Operation cancelled by user\n");
		maybe_print_json_error(&m, argc, argv,
				      "Operation cancelled by user",
				      MKTORRENT_EXIT_INTERRUPT);
		cleanup_metafile(&m);
		return MKTORRENT_EXIT_INTERRUPT;
	}

	/* open the file stream now, so we don't have to abort
	   _after_ we did all the hashing in case we fail */
	file = open_file(m.metainfo_file_path, m.force_overwrite);
	if (!file) {
		maybe_print_json_error(&m, argc, argv,
				      "Failed to open metainfo output file",
				      MKTORRENT_EXIT_IO);
		cleanup_metafile(&m);
		return MKTORRENT_EXIT_IO;
	}

	/* Start measuring time */
	struct timespec start_time;
	if (clock_gettime(CLOCK_MONOTONIC, &start_time) == -1) {
		fprintf(stderr, "Failed to get start time: %s\n", strerror(errno));
	}

	/* Calculate hash string... */
	hash = make_hash(&m);
	if (!hash || force_exit) {
		if (force_exit) {
			fprintf(stderr, "Hashing cancelled by user\n");
			exit_code = MKTORRENT_EXIT_INTERRUPT;
		} else {
			fprintf(stderr, "Failed to create hashes\n");
			exit_code = MKTORRENT_EXIT_IO;
		}
		goto cleanup;
	}

	/* Stop measuring time */
	struct timespec end_time;
	if (clock_gettime(CLOCK_MONOTONIC, &end_time) == -1) {
		fprintf(stderr, "Failed to get end time: %s\n", strerror(errno));
	} else {
		/* Calculate elapsed time */
		double elapsed = (end_time.tv_sec - start_time.tv_sec) +
						(end_time.tv_nsec - start_time.tv_nsec) / 1e9;

		if (m.verbose && !m.json_output && !m.quiet) {
			fprintf(stderr, "Hashing completed in %.2f seconds (%.2f MB/s)\n",
				elapsed, (m.size / (1024.0 * 1024.0)) / elapsed);
		}
	}

	/* Write the metainfo to file */
	if (write_metainfo(file, &m, hash) != 0) {
		fprintf(stderr, "Failed to write metainfo file\n");
		exit_code = MKTORRENT_EXIT_IO;
		goto cleanup;
	}

cleanup:
	/* Clean up resources */
	if (file && close_file(file) != 0) {
		/* close_file already printed an error message */
		if (exit_code == MKTORRENT_EXIT_OK)
			exit_code = MKTORRENT_EXIT_IO;
	}

	if (exit_code == MKTORRENT_EXIT_OK && m.json_output) {
		print_json_summary(&m);
	} else if (exit_code != MKTORRENT_EXIT_OK && m.json_output) {
		const char *err = "Failed to create torrent";
		if (exit_code == MKTORRENT_EXIT_INTERRUPT)
			err = "Hashing cancelled by user";
		else if (exit_code == MKTORRENT_EXIT_IO)
			err = "I/O error while creating torrent";
		print_json_error(err, exit_code);
	} else if (exit_code == MKTORRENT_EXIT_OK && m.verbose && !m.quiet) {
		fprintf(stderr, "Torrent created successfully: %s\n", m.metainfo_file_path);
		if (m.target_is_directory) {
			fprintf(stderr, "Files: %u, Total size: %" PRIuMAX " bytes\n",
				count_ll_nodes(m.file_list), m.size);
		}
	}

	cleanup_metafile(&m);
	free(hash);

	/* Return success or failure */
	return exit_code;
}
