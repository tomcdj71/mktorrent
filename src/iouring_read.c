/*
 * io_uring ring helpers for mktorrent (opt-in --io-uring).
 * Requires liburing; compiled only with USE_IO_URING.
 */

#ifndef USE_IO_URING
#error "iouring_read.c requires USE_IO_URING"
#endif

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <liburing.h>

#include "iouring_read.h"

struct mkt_uring {
	struct io_uring ring;
	unsigned depth;
	unsigned inflight;
	int ring_ok;
};

struct mkt_uring *mkt_uring_create(unsigned depth, unsigned flags)
{
	struct mkt_uring *u;
	int ret;
	unsigned setup_flags = 0;

	if (depth < 1)
		depth = 1;
	if (depth > MKTORRENT_IOURING_MAX_DEPTH)
		depth = MKTORRENT_IOURING_MAX_DEPTH;

	u = calloc(1, sizeof(*u));
	if (!u)
		return NULL;

	u->depth = depth;
	if (flags & MKT_URING_F_SQPOLL)
		setup_flags |= IORING_SETUP_SQPOLL;

	ret = io_uring_queue_init((unsigned)depth, &u->ring, setup_flags);
	if (ret < 0) {
		free(u);
		errno = -ret;
		return NULL;
	}
	u->ring_ok = 1;
	return u;
}

void mkt_uring_destroy(struct mkt_uring *u)
{
	if (!u)
		return;
	if (u->ring_ok)
		io_uring_queue_exit(&u->ring);
	free(u);
}

unsigned mkt_uring_inflight(const struct mkt_uring *u)
{
	return u ? u->inflight : 0;
}

unsigned mkt_uring_depth(const struct mkt_uring *u)
{
	return u ? u->depth : 0;
}

int mkt_uring_submit_read(struct mkt_uring *u, int fd, void *buf, size_t len,
			  off_t offset, void *user_data)
{
	struct io_uring_sqe *sqe;

	if (!u || !u->ring_ok || !buf || len == 0)
		return -1;

	sqe = io_uring_get_sqe(&u->ring);
	if (!sqe)
		return -1;

	io_uring_prep_read(sqe, fd, buf, len, (uint64_t)offset);
	io_uring_sqe_set_data(sqe, user_data);

	if (io_uring_submit(&u->ring) < 0)
		return -1;

	u->inflight++;
	return 0;
}

int mkt_uring_reap(struct mkt_uring *u, int min_complete,
		   mkt_uring_cqe_cb cb, void *cb_arg)
{
	struct io_uring_cqe *cqe;
	int got = 0;

	if (!u || !u->ring_ok || !cb)
		return -1;

	while (got < min_complete || (min_complete == 0 && got == 0)) {
		int ret;

		if (min_complete > 0)
			ret = io_uring_wait_cqe(&u->ring, &cqe);
		else {
			ret = io_uring_peek_cqe(&u->ring, &cqe);
			if (ret == -EAGAIN)
				break;
		}
		if (ret < 0)
			return (got > 0) ? got : -1;

		if (u->inflight > 0)
			u->inflight--;

		cb(io_uring_cqe_get_data(cqe), cqe->res, cb_arg);
		io_uring_cqe_seen(&u->ring, cqe);
		got++;

		/* Drain any additional ready CQEs without blocking */
		while (io_uring_peek_cqe(&u->ring, &cqe) == 0) {
			if (u->inflight > 0)
				u->inflight--;
			cb(io_uring_cqe_get_data(cqe), cqe->res, cb_arg);
			io_uring_cqe_seen(&u->ring, cqe);
			got++;
		}

		if (min_complete == 0)
			break;
		if (got >= min_complete)
			break;
	}

	return got;
}

int mkt_uring_cancel_drain(struct mkt_uring *u, mkt_uring_cqe_cb cb, void *cb_arg)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;

	if (!u || !u->ring_ok)
		return -1;

	/* Best-effort cancel of all pending SQEs for this ring */
	sqe = io_uring_get_sqe(&u->ring);
	if (sqe) {
		io_uring_prep_cancel(sqe, NULL, IORING_ASYNC_CANCEL_ANY);
		io_uring_sqe_set_data(sqe, NULL);
		io_uring_submit(&u->ring);
	}

	/*
	 * Reap until every submitted read has completed. Cancel CQEs carry
	 * NULL user_data and must not decrement inflight.
	 */
	while (u->inflight > 0) {
		void *data;

		if (io_uring_wait_cqe(&u->ring, &cqe) < 0)
			break;

		data = io_uring_cqe_get_data(cqe);
		if (data != NULL) {
			u->inflight--;
			if (cb)
				cb(data, cqe->res, cb_arg);
		}
		io_uring_cqe_seen(&u->ring, cqe);
	}

	/* Sweep any leftover CQEs (e.g. late cancel completions) */
	while (io_uring_peek_cqe(&u->ring, &cqe) == 0) {
		void *data = io_uring_cqe_get_data(cqe);

		if (data != NULL && u->inflight > 0) {
			u->inflight--;
			if (cb)
				cb(data, cqe->res, cb_arg);
		}
		io_uring_cqe_seen(&u->ring, cqe);
	}

	return 0;
}
