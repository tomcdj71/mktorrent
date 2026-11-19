/*
 * Piece SHA-1 backend selection (OpenSSL / bundled / optional ISA-L Crypto).
 */
#ifndef MKTORRENT_SHA1_BACKEND_H
#define MKTORRENT_SHA1_BACKEND_H

#include <stddef.h>

#include "export.h"

#ifndef SHA_DIGEST_LENGTH
#define SHA_DIGEST_LENGTH 20
#endif

/* Runtime preference when multiple backends are compiled in. */
enum mkt_sha1_pref {
	MKT_SHA1_AUTO = 0,
	MKT_SHA1_OPENSSL = 1,
	MKT_SHA1_ISAL = 2
};

EXPORT void mkt_sha1_set_pref(enum mkt_sha1_pref pref);
EXPORT enum mkt_sha1_pref mkt_sha1_get_pref(void);

/* Resolved backend name used for piece hashing: "isal", "openssl", or "bundled". */
EXPORT const char *mkt_sha1_backend_name(void);

/* 1 if this build was linked with ISA-L Crypto SHA-1. */
EXPORT int mkt_sha1_isal_available(void);

/* 1 if the active preference will use the ISA-L path (needs USE_ISAL + pref). */
EXPORT int mkt_sha1_using_isal(void);

/*
 * Max pieces to submit in one multi-buffer wave (ISA-L lanes), or 1 otherwise.
 * Workers use this for get_full_batch sizing.
 */
EXPORT unsigned mkt_sha1_max_batch(void);

/*
 * Hash one contiguous buffer into a 20-byte digest (BitTorrent piece SHA-1).
 * Uses the active backend selected by mkt_sha1_set_pref / auto.
 */
EXPORT void mkt_sha1_piece(const void *data, size_t len,
	unsigned char out[SHA_DIGEST_LENGTH]);

/*
 * Hash n contiguous piece buffers (1 <= n <= mkt_sha1_max_batch()).
 * With ISA-L active, submits up to ISAL_SHA1_MAX_LANES digests to one mgr
 * before flush. Otherwise loops mkt_sha1_piece / OpenSSL.
 */
EXPORT void mkt_sha1_pieces_mb(const void *const *data, const size_t *len,
	unsigned char *const *out, unsigned n);

#endif /* MKTORRENT_SHA1_BACKEND_H */
