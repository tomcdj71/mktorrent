/*
 * Internal io_uring helpers for mktorrent piece reads.
 * Only compiled when USE_IO_URING is defined.
 */
#ifndef MKTORRENT_IOURING_READ_H
#define MKTORRENT_IOURING_READ_H

#include <stddef.h>
#include <sys/types.h>

#ifndef MKTORRENT_IOURING_MAX_DEPTH
#define MKTORRENT_IOURING_MAX_DEPTH 256
#endif
#ifndef MKTORRENT_IOURING_DEFAULT_DEPTH
#define MKTORRENT_IOURING_DEFAULT_DEPTH 32
#endif

#define MKT_URING_F_SQPOLL 1u

struct mkt_uring;

struct mkt_uring *mkt_uring_create(unsigned depth, unsigned flags);
void mkt_uring_destroy(struct mkt_uring *u);

unsigned mkt_uring_inflight(const struct mkt_uring *u);
unsigned mkt_uring_depth(const struct mkt_uring *u);

/* Queue a read at file offset into buf. user_data is returned on completion. */
int mkt_uring_submit_read(struct mkt_uring *u, int fd, void *buf, size_t len,
			  off_t offset, void *user_data);

typedef void (*mkt_uring_cqe_cb)(void *user_data, int res, void *cb_arg);

/* Reap at least min_complete CQEs (0 = non-blocking drain of ready). */
int mkt_uring_reap(struct mkt_uring *u, int min_complete,
		   mkt_uring_cqe_cb cb, void *cb_arg);

/*
 * Cancel in-flight ops and drain the CQ. Every completed read CQE invokes cb
 * with its user_data so callers can reclaim piece buffers before destroy.
 * Cancel CQEs (NULL user_data) are consumed without calling cb.
 */
int mkt_uring_cancel_drain(struct mkt_uring *u, mkt_uring_cqe_cb cb, void *cb_arg);

#endif /* MKTORRENT_IOURING_READ_H */
