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


#include <sys/types.h>    /* off_t */
#include <stdio.h>        /* fprintf() etc. */
#include <string.h>       /* strlen() etc. */
#include <time.h>         /* time() */
#include <inttypes.h>     /* PRIuMAX */
#include <stdlib.h>       /* random() */
#include <stdarg.h>       /* va_list */
#include <string.h>       /* memset */

#ifdef USE_OPENSSL
#include <openssl/sha.h>  /* SHA_DIGEST_LENGTH */
#include <openssl/evp.h>
#else
#include "sha1.h"
#endif

#include "export.h"       /* EXPORT */
#include "mktorrent.h"    /* struct metafile */
#include "output.h"

/* Length of random data for cross-seeding */
#define CROSS_SEED_RAND_LENGTH 16

/*
 * write announce list
 * Returns 0 on success, -1 on error
 */
static int write_announce_list(FILE *f, struct ll *list)
{
	int err;

	/* the announce list is a list of lists of urls */
	err = fprintf(f, "13:announce-listl");
	if (err < 0) {
		fprintf(stderr, "Error writing announce-list start\n");
		return -1;
	}

	/* go through them all.. */
	LL_FOR(tier_node, list) {
		/* .. and print the lists */
		err = fprintf(f, "l");
		if (err < 0) {
			fprintf(stderr, "Error writing tier list start\n");
			return -1;
		}

		LL_FOR(announce_url_node, LL_DATA_AS(tier_node, struct ll*)) {
			const char *announce_url =
				LL_DATA_AS(announce_url_node, const char*);

			err = fprintf(f, "%lu:%s",
					(unsigned long) strlen(announce_url), announce_url);
			if (err < 0) {
				fprintf(stderr, "Error writing announce URL\n");
				return -1;
			}
		}

		err = fprintf(f, "e");
		if (err < 0) {
			fprintf(stderr, "Error writing tier list end\n");
			return -1;
		}
	}
	
	err = fprintf(f, "e");
	if (err < 0) {
		fprintf(stderr, "Error writing announce-list end\n");
		return -1;
	}
	
	return 0;
}

/*
 * write web seed list
 * Returns 0 on success, -1 on error
 */
static int write_web_seed_list(FILE *f, struct ll *list)
{
	int err;
	
	/* print the entry and start the list */
	err = fprintf(f, "8:url-listl");
	if (err < 0) {
		fprintf(stderr, "Error writing url-list start\n");
		return -1;
	}
	
	/* go through the list and write each URL */
	LL_FOR(node, list) {
		const char *web_seed_url = LL_DATA_AS(node, const char*);
		err = fprintf(f, "%lu:%s",
			(unsigned long) strlen(web_seed_url), web_seed_url);
		if (err < 0) {
			fprintf(stderr, "Error writing web seed URL\n");
			return -1;
		}
	}
	
	/* end the list */
	err = fprintf(f, "e");
	if (err < 0) {
		fprintf(stderr, "Error writing url-list end\n");
		return -1;
	}
	
	return 0;
}

/*
 * write metainfo to the file stream using all the information
 * we've gathered so far and the hash string calculated.
 * Also computes the info-hash (SHA-1 of the bencoded info dict).
 */
struct info_sha {
#ifdef USE_OPENSSL
	EVP_MD_CTX *ctx;
#else
	SHA_CTX ctx;
#endif
	int ok;
};

static int info_sha_init(struct info_sha *h)
{
	memset(h, 0, sizeof(*h));
#ifdef USE_OPENSSL
	h->ctx = EVP_MD_CTX_new();
	if (h->ctx == NULL)
		return -1;
	if (EVP_DigestInit_ex(h->ctx, EVP_sha1(), NULL) != 1) {
		EVP_MD_CTX_free(h->ctx);
		h->ctx = NULL;
		return -1;
	}
#else
	SHA1_Init(&h->ctx);
#endif
	h->ok = 1;
	return 0;
}

static void info_sha_update(struct info_sha *h, const void *data, size_t len)
{
	if (!h || !h->ok || len == 0)
		return;
#ifdef USE_OPENSSL
	if (EVP_DigestUpdate(h->ctx, data, len) != 1)
		h->ok = 0;
#else
	SHA1_Update(&h->ctx, (const uint8_t *)data, (unsigned long)len);
#endif
}

static int info_sha_final(struct info_sha *h, unsigned char out[SHA_DIGEST_LENGTH])
{
	if (!h || !h->ok)
		return -1;
#ifdef USE_OPENSSL
	unsigned int outlen = 0;
	if (EVP_DigestFinal_ex(h->ctx, out, &outlen) != 1 || outlen != SHA_DIGEST_LENGTH) {
		EVP_MD_CTX_free(h->ctx);
		h->ctx = NULL;
		return -1;
	}
	EVP_MD_CTX_free(h->ctx);
	h->ctx = NULL;
#else
	SHA1_Final(out, &h->ctx);
#endif
	return 0;
}

static int emit_bytes(FILE *f, struct info_sha *h, const void *buf, size_t len)
{
	if (fwrite(buf, 1, len, f) != len)
		return -1;
	info_sha_update(h, buf, len);
	return 0;
}

static int emit_fmt(FILE *f, struct info_sha *h, const char *fmt, ...)
{
	char stackbuf[4096];
	char *buf = stackbuf;
	size_t cap = sizeof(stackbuf);
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, cap, fmt, ap);
	va_end(ap);
	if (n < 0)
		return -1;
	if ((size_t)n >= cap) {
		cap = (size_t)n + 1;
		buf = malloc(cap);
		if (!buf)
			return -1;
		va_start(ap, fmt);
		n = vsnprintf(buf, cap, fmt, ap);
		va_end(ap);
		if (n < 0 || (size_t)n >= cap) {
			free(buf);
			return -1;
		}
	}
	int rc = emit_bytes(f, h, buf, (size_t)n);
	if (buf != stackbuf)
		free(buf);
	return rc;
}

static int write_announce_list_hashed(FILE *f, struct ll *list)
{
	/* announce-list is outside info; keep original helper */
	return write_announce_list(f, list);
}

static int write_file_list_hashed(FILE *f, struct info_sha *h, struct ll *list)
{
	char *a, *b;

	if (emit_fmt(f, h, "5:filesl") != 0)
		return -1;

	LL_FOR(file_node, list) {
		struct file_data *fd = LL_DATA_AS(file_node, struct file_data*);

		if (emit_fmt(f, h, "d6:lengthi%" PRIuMAX "e4:pathl", fd->size) != 0)
			return -1;

		/* Emit path segments without mutating fd->path (MK-020). */
		a = fd->path;
		while ((b = strchr(a, DIRSEP_CHAR)) != NULL) {
			if (emit_fmt(f, h, "%lu:%.*s",
					(unsigned long)(b - a), (int)(b - a), a) != 0)
				return -1;
			a = b + 1;
		}
		if (emit_fmt(f, h, "%lu:%see", (unsigned long)strlen(a), a) != 0)
			return -1;
	}

	if (emit_fmt(f, h, "e") != 0)
		return -1;
	return 0;
}

static int write_cross_seed_data_hashed(FILE *f, struct info_sha *h)
{
	int i;
	if (emit_fmt(f, h, "12:x_cross_seed%u:mktorrent-", CROSS_SEED_RAND_LENGTH * 2 + 10) != 0)
		return -1;
	for (i = 0; i < CROSS_SEED_RAND_LENGTH; i++) {
		unsigned char rand_byte = random();
		char hex[3];
		hex[0] = "0123456789ABCDEF"[rand_byte >> 4];
		hex[1] = "0123456789ABCDEF"[rand_byte & 0x0F];
		if (emit_bytes(f, h, hex, 2) != 0)
			return -1;
	}
	return 0;
}

EXPORT int write_metainfo(FILE *f, struct metafile *m, unsigned char *hash_string)
{
	int err;
	struct info_sha ih;

	m->info_hash_valid = 0;

	/* progress / status on stderr only */
	if (!m->json_output && !m->quiet) {
		fprintf(stderr, "writing metainfo file... ");
		fflush(stderr);
	}

	err = fprintf(f, "d");
	if (err < 0) {
		fprintf(stderr, "Error writing dictionary start\n");
		return -1;
	}

	if (!LL_IS_EMPTY(m->announce_list)) {
		struct ll *first_tier =
			LL_DATA_AS(LL_HEAD(m->announce_list), struct ll*);
		const char *first_announce_url
			= LL_DATA_AS(LL_HEAD(first_tier), const char*);

		err = fprintf(f, "8:announce%lu:%s",
			(unsigned long) strlen(first_announce_url), first_announce_url);
		if (err < 0) {
			fprintf(stderr, "Error writing announce URL\n");
			return -1;
		}

		if (LL_NEXT(LL_HEAD(m->announce_list)) || LL_NEXT(LL_HEAD(first_tier))) {
			if (write_announce_list_hashed(f, m->announce_list) != 0)
				return -1;
		}
	}

	if (m->comment != NULL) {
		err = fprintf(f, "7:comment%lu:%s",
				(unsigned long)strlen(m->comment),
				m->comment);
		if (err < 0) {
			fprintf(stderr, "Error writing comment\n");
			return -1;
		}
	}

#ifndef VERSION
#define VERSION "1.1"
#endif
	char const *const created_by = "mktorrent " VERSION;
	if (!m->no_created_by) {
		err = fprintf(f, "10:created by%zu:%s", strlen(created_by), created_by);
		if (err < 0) {
			fprintf(stderr, "Error writing created by\n");
			return -1;
		}
	}

	if (!m->no_creation_date) {
		err = fprintf(f, "13:creation datei%lde",
			(long)time(NULL));
		if (err < 0) {
			fprintf(stderr, "Error writing creation date\n");
			return -1;
		}
	}

	/* info key prefix is NOT part of the info-hash */
	err = fprintf(f, "4:info");
	if (err < 0) {
		fprintf(stderr, "Error writing info key\n");
		return -1;
	}

	if (info_sha_init(&ih) != 0) {
		fprintf(stderr, "Error initializing info-hash\n");
		return -1;
	}

	if (emit_fmt(f, &ih, "d") != 0) {
		fprintf(stderr, "Error writing info dictionary start\n");
		return -1;
	}

	if (!m->target_is_directory) {
		if (emit_fmt(f, &ih, "6:lengthi%" PRIuMAX "e",
			LL_DATA_AS(LL_HEAD(m->file_list), struct file_data*)->size) != 0) {
			fprintf(stderr, "Error writing single file length\n");
			return -1;
		}
	} else {
		if (write_file_list_hashed(f, &ih, m->file_list) != 0)
			return -1;
	}

	if (m->cross_seed) {
		if (write_cross_seed_data_hashed(f, &ih) != 0)
			return -1;
	}

	if (emit_fmt(f, &ih, "4:name%lu:%s12:piece lengthi%ue6:pieces%u:",
		(unsigned long)strlen(m->torrent_name), m->torrent_name,
		m->piece_length, m->pieces * SHA_DIGEST_LENGTH) != 0) {
		fprintf(stderr, "Error writing torrent name and piece data\n");
		return -1;
	}

	if (emit_bytes(f, &ih, hash_string, m->pieces * SHA_DIGEST_LENGTH) != 0) {
		fprintf(stderr, "Error writing piece hashes\n");
		return -1;
	}

	if (m->private) {
		if (emit_fmt(f, &ih, "7:privatei1e") != 0) {
			fprintf(stderr, "Error writing private flag\n");
			return -1;
		}
	}

	if (m->source) {
		if (emit_fmt(f, &ih, "6:source%lu:%s",
			(unsigned long) strlen(m->source), m->source) != 0) {
			fprintf(stderr, "Error writing source\n");
			return -1;
		}
	}

	if (emit_fmt(f, &ih, "e") != 0) {
		fprintf(stderr, "Error ending info dictionary\n");
		return -1;
	}

	if (info_sha_final(&ih, m->info_hash) != 0) {
		fprintf(stderr, "Error finalizing info-hash\n");
		return -1;
	}
	m->info_hash_valid = 1;

	if (m->node_list && !LL_IS_EMPTY(m->node_list)) {
		err = fprintf(f, "5:nodesl");
		if (err < 0) {
			fprintf(stderr, "Error writing nodes list start\n");
			return -1;
		}
		LL_FOR(node_el, m->node_list) {
			struct dht_node *n = LL_DATA_AS(node_el, struct dht_node *);

			err = fprintf(f, "l%lu:%si%uee",
				(unsigned long)strlen(n->host), n->host, n->port);
			if (err < 0) {
				fprintf(stderr, "Error writing DHT node\n");
				return -1;
			}
		}
		err = fprintf(f, "e");
		if (err < 0) {
			fprintf(stderr, "Error writing nodes list end\n");
			return -1;
		}
	}

	if (!LL_IS_EMPTY(m->web_seed_list)) {
		if (LL_IS_SINGLETON(m->web_seed_list)) {
			const char *first_web_seed =
				LL_DATA_AS(LL_HEAD(m->web_seed_list), const char*);

			err = fprintf(f, "8:url-list%lu:%s",
					(unsigned long) strlen(first_web_seed), first_web_seed);
			if (err < 0) {
				fprintf(stderr, "Error writing web seed URL\n");
				return -1;
			}
		} else {
			if (write_web_seed_list(f, m->web_seed_list) != 0)
				return -1;
		}
	}

	err = fprintf(f, "e");
	if (err < 0) {
		fprintf(stderr, "Error ending root dictionary\n");
		return -1;
	}

	if (fflush(f) != 0) {
		fprintf(stderr, "Error flushing metainfo file to disk\n");
		return -1;
	}

	if (!m->json_output && !m->quiet) {
		fprintf(stderr, "done\n");
		fflush(stderr);
	}

	return 0;
}
