/*
 * Piece SHA-1 backend: optional ISA-L Crypto multi-buffer, OpenSSL, or bundled.
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#ifdef USE_OPENSSL
#include <openssl/sha.h>
#else
#include "sha1.h"
#endif

#ifdef USE_ISAL
#include <isa-l_crypto.h>
#endif

#include "sha1_backend.h"

static enum mkt_sha1_pref g_pref = MKT_SHA1_AUTO;

void mkt_sha1_set_pref(enum mkt_sha1_pref pref)
{
	g_pref = pref;
}

enum mkt_sha1_pref mkt_sha1_get_pref(void)
{
	return g_pref;
}

int mkt_sha1_isal_available(void)
{
#ifdef USE_ISAL
	return 1;
#else
	return 0;
#endif
}

static void hash_openssl_or_bundled(const void *data, size_t len,
	unsigned char out[SHA_DIGEST_LENGTH])
{
#ifdef USE_OPENSSL
	SHA1(data, len, out);
#else
	SHA_CTX ctx;

	SHA1_Init(&ctx);
	SHA1_Update(&ctx, (const uint8_t *)data, (unsigned long)len);
	SHA1_Final(out, &ctx);
#endif
}

#ifdef USE_ISAL
/*
 * Per-worker TLS: ISA-L CTX managers are not thread-safe. Align to 64 bytes
 * as required by the multi-buffer CTX API. Pool covers one multi-lane wave.
 */
struct mkt_isal_tls {
	ISAL_SHA1_HASH_CTX_MGR *mgr;
	ISAL_SHA1_HASH_CTX *ctx[ISAL_SHA1_MAX_LANES];
	int ready;
};

static __thread struct mkt_isal_tls tls_isal;

static void digest_words_to_bytes(const uint32_t *words,
	unsigned char out[SHA_DIGEST_LENGTH])
{
	int i;

	for (i = 0; i < ISAL_SHA1_DIGEST_NWORDS; i++) {
		uint32_t w = words[i];

		out[i * 4 + 0] = (unsigned char)((w >> 24) & 0xff);
		out[i * 4 + 1] = (unsigned char)((w >> 16) & 0xff);
		out[i * 4 + 2] = (unsigned char)((w >> 8) & 0xff);
		out[i * 4 + 3] = (unsigned char)(w & 0xff);
	}
}

static int mkt_isal_tls_ensure(void)
{
	int ret;
	unsigned i;

	if (tls_isal.ready)
		return 0;

	ret = posix_memalign((void **)&tls_isal.mgr, 64, sizeof(*tls_isal.mgr));
	if (ret != 0 || tls_isal.mgr == NULL)
		return -1;
	memset(tls_isal.mgr, 0, sizeof(*tls_isal.mgr));

	for (i = 0; i < ISAL_SHA1_MAX_LANES; i++) {
		ret = posix_memalign((void **)&tls_isal.ctx[i], 64,
			sizeof(*tls_isal.ctx[i]));
		if (ret != 0 || tls_isal.ctx[i] == NULL) {
			while (i > 0) {
				i--;
				free(tls_isal.ctx[i]);
				tls_isal.ctx[i] = NULL;
			}
			free(tls_isal.mgr);
			tls_isal.mgr = NULL;
			return -1;
		}
		memset(tls_isal.ctx[i], 0, sizeof(*tls_isal.ctx[i]));
		isal_hash_ctx_init(tls_isal.ctx[i]);
	}

	if (isal_sha1_ctx_mgr_init(tls_isal.mgr) != 0) {
		for (i = 0; i < ISAL_SHA1_MAX_LANES; i++) {
			free(tls_isal.ctx[i]);
			tls_isal.ctx[i] = NULL;
		}
		free(tls_isal.mgr);
		tls_isal.mgr = NULL;
		return -1;
	}
	tls_isal.ready = 1;
	return 0;
}

static int hash_isal_batch(const void *const *data, const size_t *len,
	unsigned char *const *out, unsigned n)
{
	ISAL_SHA1_HASH_CTX *completed = NULL;
	unsigned i;

	if (n == 0)
		return 0;
	if (n > ISAL_SHA1_MAX_LANES)
		n = ISAL_SHA1_MAX_LANES;

	if (mkt_isal_tls_ensure() != 0)
		return -1;

	/*
	 * Intel multi-buffer pattern: submit up to MAX_LANES HASH_ENTIRE jobs,
	 * flush until idle, then read digests from each ctx (order preserved).
	 */
	for (i = 0; i < n; i++) {
		uint32_t piece_len;

		if (len[i] > UINT32_MAX)
			return -1;
		piece_len = (uint32_t)len[i];
		isal_hash_ctx_init(tls_isal.ctx[i]);
		if (isal_sha1_ctx_mgr_submit(tls_isal.mgr, tls_isal.ctx[i],
				&completed, data[i], piece_len,
				ISAL_HASH_ENTIRE) != 0)
			return -1;
	}

	do {
		if (isal_sha1_ctx_mgr_flush(tls_isal.mgr, &completed) != 0)
			return -1;
	} while (completed != NULL);

	for (i = 0; i < n; i++) {
		digest_words_to_bytes(tls_isal.ctx[i]->job.result_digest, out[i]);
		isal_hash_ctx_init(tls_isal.ctx[i]);
	}

	return 0;
}

static int want_isal(void)
{
	if (g_pref == MKT_SHA1_ISAL)
		return 1;
	if (g_pref == MKT_SHA1_OPENSSL)
		return 0;
	/*
	 * auto: OpenSSL. Multi-buffer ISA-L (full-wave batches) was still
	 * slower than OpenSSL on measured warm/tmpfs hosts (see Hashing-IO);
	 * use --sha1=isal to force the batched path.
	 */
	return 0;
}
#endif /* USE_ISAL */

int mkt_sha1_using_isal(void)
{
#ifdef USE_ISAL
	return want_isal();
#else
	return 0;
#endif
}

unsigned mkt_sha1_max_batch(void)
{
#ifdef USE_ISAL
	if (want_isal())
		return ISAL_SHA1_MAX_LANES;
#endif
	return 1;
}

const char *mkt_sha1_backend_name(void)
{
#ifdef USE_ISAL
	if (want_isal())
		return "isal";
#endif
#ifdef USE_OPENSSL
	(void)g_pref;
	return "openssl";
#else
	(void)g_pref;
	return "bundled";
#endif
}

void mkt_sha1_piece(const void *data, size_t len,
	unsigned char out[SHA_DIGEST_LENGTH])
{
	const void *datas[1];
	size_t lens[1];
	unsigned char *outs[1];

	datas[0] = data;
	lens[0] = len;
	outs[0] = out;
	mkt_sha1_pieces_mb(datas, lens, outs, 1);
}

void mkt_sha1_pieces_mb(const void *const *data, const size_t *len,
	unsigned char *const *out, unsigned n)
{
	unsigned i;

	if (n == 0)
		return;

#ifdef USE_ISAL
	if (want_isal()) {
		unsigned off = 0;

		while (off < n) {
			unsigned chunk = n - off;

			if (chunk > ISAL_SHA1_MAX_LANES)
				chunk = ISAL_SHA1_MAX_LANES;
			if (hash_isal_batch(data + off, len + off, out + off,
					chunk) != 0) {
				/* Fall back piece-by-piece on unexpected failure. */
				for (i = off; i < n; i++)
					hash_openssl_or_bundled(data[i], len[i], out[i]);
				return;
			}
			off += chunk;
		}
		return;
	}
#endif
	for (i = 0; i < n; i++)
		hash_openssl_or_bundled(data[i], len[i], out[i]);
}
