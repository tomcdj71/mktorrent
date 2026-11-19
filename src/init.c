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


#include <stdlib.h>       /* exit() */
#include <sys/types.h>    /* off_t */
#include <errno.h>        /* errno */
#include <string.h>       /* strerror() */
#include <stdio.h>        /* perror(), printf() etc. */
#include <sys/stat.h>     /* the stat structure */
#include <unistd.h>       /* getopt(), getcwd(), sysconf() */
#include <string.h>       /* strcmp(), strlen(), strncpy() */
#include <strings.h>      /* strcasecmp() */
#include <inttypes.h>     /* PRId64 etc. */
#include <getopt.h>       /* getopt_long() */
#include <unistd.h>       /* getopt(), optarg, optind */
#include <dirent.h>
#include <fcntl.h>
#include <time.h>
#include <ctype.h>
#include <libgen.h>       /* basename() */
#include <limits.h>       /* PATH_MAX */
#include <pthread.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#include "export.h"
#include "mktorrent.h"
#include "ftw.h"
#include "msg.h"
#include "ll.h"
#include "sha1_backend.h"

/* Forward declarations */
EXPORT void cleanup_metafile(struct metafile *m);

#ifndef MAX_OPENFD
#define MAX_OPENFD 100	/* Maximum number of file descriptors
			   file_tree_walk() will open */
#endif

/*
 * Display program information, version and build configuration
 */
static int display_version(void)
{
	printf("mktorrent %s (c) 2007, 2009 Emil Renner Berthing\n", VERSION);
#ifdef USE_PTHREADS
	printf("sha1=%s", mkt_sha1_backend_name());
#ifdef USE_ISAL
	printf(" (isal available)");
#endif
	printf("\n");
#endif
	return EXIT_SUCCESS;
}

static void strip_ending_dirseps(char *s)
{
	char *end = s;

	if (!s) return;  /* Guard against NULL pointer */

	while (*end)
		end++;

	while (end > s && *(--end) == DIRSEP_CHAR)
		*end = '\0';
}

static const char *get_basename(const char *s)
{
	const char *r = s;

	if (!s) return "";  /* Guard against NULL pointer */

	while (*s != '\0') {
		if (*s == DIRSEP_CHAR)
			r = ++s;
		else
			++s;
	}

	return r;
}

static void set_absolute_file_path(struct metafile *m)
{
	/* if the file_path is already an absolute path just
	   return that */
	if (m->metainfo_file_path && *m->metainfo_file_path == DIRSEP_CHAR) {
		/* we need to reallocate the string, because we want to be able to
		 * free() it in cleanup_metafile(), and that would not be possible
		 * if m->metainfo_file_path pointed to a string from argv[]
		 */
		m->metainfo_file_path = strdup(m->metainfo_file_path);
		FATAL_IF0(m->metainfo_file_path == NULL, "out of memory\n");
		return;
	}

	char cwd[PATH_MAX]; /* Buffer for current working directory */
	size_t cwd_len;
	size_t result_len;
	char *result;

	/* Get the current working directory */
	if (getcwd(cwd, PATH_MAX) == NULL) {
		fatal("getcwd failed: %s\n", strerror(errno));
	}
	
	cwd_len = strlen(cwd);

	/* Calculate the required final string length once to avoid multiple reallocations */
	if (m->metainfo_file_path == NULL) {
		/* Need space for: cwd + '/' + torrent_name + '.torrent' + '\0' */
		result_len = cwd_len + 1 + strlen(m->torrent_name) + 8 + 1;
	} else {
		/* Need space for: cwd + '/' + metainfo_file_path + '\0' */
		result_len = cwd_len + 1 + strlen(m->metainfo_file_path) + 1;
	}

	/* Allocate the final string once with the exact size needed */
	result = malloc(result_len);
	FATAL_IF0(result == NULL, "out of memory\n");

	/* First copy the current working directory */
	memcpy(result, cwd, cwd_len);
	
	/* Then add the appropriate suffix */
	if (m->metainfo_file_path == NULL) {
		sprintf(result + cwd_len, DIRSEP "%s.torrent", m->torrent_name);
	} else {
		sprintf(result + cwd_len, DIRSEP "%s", m->metainfo_file_path);
	}

	m->metainfo_file_path = result;
}

/*
 * Check if a URL is valid
 * Returns 1 for valid URLs, 0 for invalid ones
 * Note: This is used directly by validate_url_list now
 */
static int is_valid_url(const char *url) __attribute__((unused));
static int is_valid_url(const char *url)
{
	/* Quick validation checks */
	if (!url || !*url) 
		return 0;

	/* Use direct pointer comparison for protocols to avoid multiple strncmp calls */
	const char *domain = NULL;
	size_t domain_offset = 0;
	
	if (strncmp(url, "http://", 7) == 0) {
		domain_offset = 7;
	} else if (strncmp(url, "https://", 8) == 0) {
		domain_offset = 8;
	} else if (strncmp(url, "udp://", 6) == 0) {
		domain_offset = 6;
	} else {
		return 0; /* Invalid protocol */
	}
	
	domain = url + domain_offset;
	
	/* Empty domain check */
	if (!*domain)
		return 0;
	
	/* Find dot in domain without using strchr for better performance */
	const char *dot = domain;
	while (*dot && *dot != '.')
		dot++;
	
	/* Must have a dot and at least one character after it */
	return (*dot == '.' && dot[1] != '\0');
}

/*
 * Split comma-separated string into a linked list
 * validate_url: no longer used, kept for backward compatibility
 * Errors:
 * [ 37%] Building C object CMakeFiles/mktorrent.dir/src/init.c.o
 * warning: unused parameter 'validate_url' [-Wunused-parameter]
 * /187 | static struct ll *get_slist(char *s, int validate_url)
 * |                                      ~~~~^~~~~~~~~~~~
 * [ 50%] Building C object CMakeFiles/mktorrent.dir/src/ll.c.o
 */
static struct ll *get_slist(const char *s, int validate_url)
{
	char *buf;
	char *start;
	char *e;
	struct ll *list;

	(void)validate_url;

	if (!s) {
		fprintf(stderr, "Error: NULL string passed to get_slist\n");
		return NULL;
	}

	/* Copy so we never mutate argv / caller storage (MK-015). */
	buf = strdup(s);
	if (!buf) {
		fprintf(stderr, "Error: out of memory in get_slist\n");
		return NULL;
	}

	list = ll_new();
	if (!list) {
		fprintf(stderr, "Error: Failed to create new list in get_slist\n");
		free(buf);
		return NULL;
	}

	if (!*buf) {
		free(buf);
		return list;
	}

	start = buf;
	while (1) {
		e = strchr(start, ',');
		if (!e) {
			if (*start &&
			    ll_append(list, start, strlen(start) + 1) == NULL) {
				fprintf(stderr, "Error: Failed to append to list in get_slist\n");
				ll_free(list, NULL);
				free(buf);
				return NULL;
			}
			break;
		}
		*e = '\0';
		if (start != e &&
		    ll_append(list, start, strlen(start) + 1) == NULL) {
			fprintf(stderr, "Error: Failed to append to list in get_slist\n");
			ll_free(list, NULL);
			free(buf);
			return NULL;
		}
		start = e + 1;
	}

	free(buf);
	return list;
}

static size_t ll_count_nodes(struct ll *list)
{
	size_t n = 0;

	if (!list)
		return 0;
	LL_FOR(node, list)
		n++;
	return n;
}

/* Merge fnmatch patterns into dest; enforce MK-018 caps. */
static int add_patterns(struct ll *dest, const char *optarg)
{
	struct ll *more;
	size_t dest_n;
	size_t more_n;

	more = get_slist(optarg, 0);
	if (more == NULL)
		return -1;

	more_n = 0;
	LL_FOR(node, more) {
		const char *pat = LL_DATA_AS(node, const char *);
		size_t len = strlen(pat);

		if (len > MKTORRENT_MAX_PATTERN_LEN) {
			fprintf(stderr,
				"Error: pattern exceeds %d bytes\n",
				MKTORRENT_MAX_PATTERN_LEN);
			ll_free(more, NULL);
			return -1;
		}
		more_n++;
	}

	dest_n = ll_count_nodes(dest);
	if (dest_n + more_n > MKTORRENT_MAX_PATTERNS) {
		fprintf(stderr,
			"Error: more than %d exclude/include patterns\n",
			MKTORRENT_MAX_PATTERNS);
		ll_free(more, NULL);
		return -1;
	}

	ll_extend(dest, more);
	return 0;
}

/*
 * Parse -l / --piece-length.
 * Bare 15–28 = exponent; optional K/M/G suffix or bare >= 2^15 = bytes
 * (must be power of two in [2^15, 2^28]).
 */
static int parse_piece_length_arg(const char *s, unsigned int *bytes_out)
{
	char *end = NULL;
	unsigned long long v;
	size_t len;
	char suf = 0;
	unsigned shift = 0;

	if (!s || !*s)
		return -1;

	len = strlen(s);
	suf = s[len - 1];
	if (suf == 'K' || suf == 'k' || suf == 'M' || suf == 'm'
	    || suf == 'G' || suf == 'g') {
		char *tmp = strndup(s, len - 1);
		if (!tmp)
			return -1;
		errno = 0;
		v = strtoull(tmp, &end, 10);
		if (errno || end == tmp || *end != '\0') {
			free(tmp);
			return -1;
		}
		free(tmp);
		if (suf == 'K' || suf == 'k')
			shift = 10;
		else if (suf == 'M' || suf == 'm')
			shift = 20;
		else
			shift = 30;
		if (v == 0 || v > (ULLONG_MAX >> shift))
			return -1;
		v <<= shift;
	} else {
		errno = 0;
		v = strtoull(s, &end, 10);
		if (errno || end == s || *end != '\0')
			return -1;
		if (v >= 15 && v <= 28) {
			*bytes_out = 1u << (unsigned)v;
			return 0;
		}
	}

	if (v < (1ull << 15) || v > (1ull << 28))
		return -1;
	if ((v & (v - 1)) != 0)
		return -1;
	*bytes_out = (unsigned int)v;
	return 0;
}

static int parse_dht_node(const char *s, struct dht_node *out)
{
	const char *colon;
	char *host;
	unsigned long port;
	char *end = NULL;
	size_t host_len;
	size_t i;

	if (!s || !*s || !out)
		return -1;
	colon = strrchr(s, ':');
	if (!colon || colon == s || colon[1] == '\0')
		return -1;
	host_len = (size_t)(colon - s);
	for (i = 0; i < host_len; i++) {
		if ((unsigned char)s[i] < 0x20)
			return -1;
	}
	errno = 0;
	port = strtoul(colon + 1, &end, 10);
	if (errno || end == colon + 1 || *end != '\0' || port < 1 || port > 65535)
		return -1;
	host = strndup(s, host_len);
	if (!host)
		return -1;
	out->host = host;
	out->port = (unsigned)port;
	return 0;
}

static void free_dht_node(void *p)
{
	struct dht_node *n = p;
	if (n)
		free(n->host);
}

/* Return 1 if candidate_abs is root_abs or a path under it. */
static int path_under_root(const char *root_abs, const char *candidate_abs)
{
	size_t rlen;

	if (!root_abs || !*root_abs || !candidate_abs || !*candidate_abs)
		return 0;
	rlen = strlen(root_abs);
	while (rlen > 1 && root_abs[rlen - 1] == DIRSEP_CHAR)
		rlen--;
	if (strncmp(candidate_abs, root_abs, rlen) != 0)
		return 0;
	return candidate_abs[rlen] == '\0' || candidate_abs[rlen] == DIRSEP_CHAR;
}

/*
 * checks if target is a directory
 * sets the file_list and size if it isn't
 */
static int is_dir(struct metafile *m, char *target)
{
	struct stat s;		/* stat structure for stat()/lstat() to fill */

	if (!target || !*target) {
		fprintf(stderr, "Error: Empty target path\n");
		return -1;
	}

	/* Align top-level with --no-follow-symlinks (MK-017). */
	if (m->no_follow_symlinks) {
		if (lstat(target, &s) == -1) {
			fprintf(stderr, "Cannot lstat '%s': %s\n", target, strerror(errno));
			return -1;
		}
		if (S_ISLNK(s.st_mode)) {
			fprintf(stderr, "'%s' is a symlink; use without --no-follow-symlinks to follow\n",
				target);
			return -1;
		}
	} else if (stat(target, &s) == -1) {
		fprintf(stderr, "Cannot stat '%s': %s\n", target, strerror(errno));
		return -1;
	}

	/* if it is a directory, just return 1 */
	if (S_ISDIR(s.st_mode))
		return 1;

	/* if it isn't a regular file either, something is wrong.. */
	if (!S_ISREG(s.st_mode)) {
		fprintf(stderr, "'%s' is neither a directory nor regular file\n", target);
		return -1;
	}

	/* if it has negative size, something it wrong */
	if (s.st_size < 0) {
		fprintf(stderr, "'%s' has negative size\n", target);
		return -1;
	}

	/* since we know the torrent is just a single file and we've
	   already stat'ed it, we might as well set the file list */
	char *path_copy = strdup(target);
	if (!path_copy) {
		fprintf(stderr, "Error: Out of memory when duplicating path\n");
		return -1;
	}

	struct file_data fd = {
		path_copy,
		NULL,
		(uintmax_t) s.st_size
	};

	if (ll_append(m->file_list, &fd, sizeof(fd)) == NULL) {
		fprintf(stderr, "Error: Out of memory when appending to file list\n");
		free(path_copy);
		return -1;
	}

	/* ..and size variable */
	m->size = (uintmax_t) s.st_size;

	/* now return 0 since it isn't a directory */
	return 0;
}

/*
 * called by file_tree_walk() on every file and directory in the subtree
 * counts the number of (readable) files, their commulative size and adds
 * their names and individual sizes to the file list
 */
static int add_file_entry(struct metafile *m, const char *torrent_path,
		const char *read_path, uintmax_t size)
{
	struct file_data fd;

	if (m->max_files && m->file_count >= m->max_files) {
		fprintf(stderr, "fatal error: too many files (max %" PRIuMAX ")\n",
			m->max_files);
		return -1;
	}
	if (m->max_bytes && m->size > UINTMAX_MAX - size) {
		fprintf(stderr, "fatal error: aggregate size overflow\n");
		return -1;
	}
	if (m->max_bytes && m->size + size > m->max_bytes) {
		fprintf(stderr, "fatal error: aggregate size exceeds --max-bytes (%" PRIuMAX ")\n",
			m->max_bytes);
		return -1;
	}

	fd.path = strdup(torrent_path);
	fd.read_path = read_path ? strdup(read_path) : NULL;
	fd.size = size;

	if (fd.path == NULL || (read_path && fd.read_path == NULL) ||
			ll_append(m->file_list, &fd, sizeof(fd)) == NULL) {
		free(fd.path);
		free(fd.read_path);
		fprintf(stderr, "fatal error: out of memory\n");
		return -1;
	}

	m->size += size;
	m->file_count++;
	return 0;
}

static int process_node(const char *path, const struct stat *sb, void *data)
{
	struct metafile *m = data;
	char torrent_path[PATH_MAX];
	char read_path_buf[PATH_MAX];
	const char *rel;
	const char *read_path = NULL;

	/* skip non-regular files */
	if (!S_ISREG(sb->st_mode))
		return 0;

	/* ignore the leading "./" */
	rel = path + 2;

	/* MK-009: no access() TOCTOU; open during hash reports unreadable files. */

	if (sb->st_size < 0) {
		fprintf(stderr, "warning: '%s' has negative size, skipping\n", rel);
		return 0;
	}

	if (m->scan_prefix && *m->scan_prefix) {
		if (snprintf(torrent_path, sizeof(torrent_path), "%s%s%s",
				m->scan_prefix, DIRSEP, rel) >= (int)sizeof(torrent_path)) {
			fprintf(stderr, "warning: path too long, skipping '%s'\n", rel);
			return 0;
		}
	} else {
		if (snprintf(torrent_path, sizeof(torrent_path), "%s", rel)
				>= (int)sizeof(torrent_path)) {
			fprintf(stderr, "warning: path too long, skipping '%s'\n", rel);
			return 0;
		}
	}

	if (m->scan_root_abs && *m->scan_root_abs) {
		char resolved[PATH_MAX];

		if (snprintf(read_path_buf, sizeof(read_path_buf), "%s%s%s",
				m->scan_root_abs, DIRSEP, rel) >= (int)sizeof(read_path_buf)) {
			fprintf(stderr, "warning: path too long, skipping '%s'\n", rel);
			return 0;
		}
		/* Reject paths that resolve outside the scan root (MK-003). */
		if (realpath(read_path_buf, resolved) == NULL) {
			fprintf(stderr, "warning: cannot resolve '%s', skipping\n", rel);
			return 0;
		}
		if (!path_under_root(m->scan_root_abs, resolved)) {
			fprintf(stderr, "warning: '%s' escapes scan root, skipping\n", rel);
			return 0;
		}
		read_path = read_path_buf;
	}

	if (m->verbose && !m->json_output && !m->quiet)
		fprintf(stderr, "adding %s\n", torrent_path);

	return add_file_entry(m, torrent_path, read_path, (uintmax_t)sb->st_size);
}

/*
 * 'elp!
 */
static void print_help()
{
	printf(
	  "Usage: mktorrent [OPTIONS] <target> [target...]\n\n"
	  "Options:\n"
#ifdef USE_LONG_OPTIONS
	  "-a, --announce=<url>[,<url>]* : specify announce URLs (optional; DHT-only OK)\n"
	  "                                additional -a adds backup trackers\n"
	  "-c, --comment=<comment>       : add a comment to the metainfo\n"
	  "-d, --no-date                 : don't write the creation date\n"
	  "-D, --no-created-by           : don't write the creating software name and version\n"
	  "-e, --exclude=<pat>[,<pat>]*  : exclude files matching <pat> on basename or\n"
	  "                                relative path (see glob(7); e.g. subdir/*.tmp)\n"
	  "-f, --force                   : overwrite output file if it exists\n"
	  "-h, --help                    : show this help screen\n"
	  "-j, -J, --json                : print a JSON summary on stdout after success\n"
	  "-0, --null                    : NUL-separate paths for --from-file/--stdin\n"
	  "-l, --piece-length=<n>        : piece length as exponent 15-28, or bytes\n"
	  "                                with optional K/M/G (e.g. 1M); power of two\n"
	  "                                in [32KiB, 256MiB]; default from total size\n"
	  "-n, --name=<name>             : set the name of the torrent\n"
	  "                                default is the basename of the target\n"
	  "-o, --output=<filename>       : set the path and filename of the created file\n"
	  "                                default is <name>.torrent\n"
	  "-p, --private                 : set the private flag\n"
	  "-q, --quiet                   : suppress progress and success messages\n"
	  "-s, --source=<source>         : add source string embedded in infohash\n"
#ifdef USE_PTHREADS
	  "-t, --threads=<n>             : use <n> threads for calculating hashes\n"
	  "                                default is the number of CPU cores (max 64)\n"
#endif
	  "-v, --verbose                 : be verbose\n"
	  "-V, --version                 : display version information\n"
	  "-w, --web-seed=<url>[,<url>]* : add web seed URLs\n"
	  "                                additional -w adds more URLs\n"
	  "-x, --cross-seed              : ensure info hash is unique for easier cross-seeding\n"
	  "    --from-file=<file>        : read target paths from file (requires -n)\n"
	  "    --stdin                   : read target paths from stdin (requires -n)\n"
	  "    --no-follow-symlinks      : skip symbolic links instead of following them\n"
	  "    --direct-io               : use O_DIRECT for large files when supported\n"
#ifdef USE_IO_URING
	  "    --io-uring                : use io_uring for piece reads (Linux, opt-in)\n"
	  "    --io-uring-depth=<n>      : io_uring queue depth (1-256, default 32)\n"
	  "    --io-uring-sqpoll         : experimental IORING_SETUP_SQPOLL\n"
#endif
#ifdef USE_PTHREADS
	  "    --sha1=auto|openssl|isal  : piece SHA-1 backend (default auto=OpenSSL)\n"
#endif
	  "    --max-files=<n>           : refuse after n files (default 1000000)\n"
	  "    --max-bytes=<n>           : refuse if aggregate size exceeds n (0=unlimited)\n"
	  "    --node=<host>:<port>      : add a DHT bootstrap node (repeatable)\n"
	  "    --include=<pat>[,<pat>]*  : only include matching basename/relative path\n"
	  "    --skip-hidden             : skip basenames starting with '.'\n"
#else
	  "-a <url>[,<url>]* : specify announce URLs (optional; DHT-only OK)\n"
	  "                    additional -a adds backup trackers\n"
	  "-c <comment>      : add a comment to the metainfo\n"
	  "-d                : don't write the creation date\n"
	  "-D                : don't write the creating software name and version\n"
	  "-e <pat>[,<pat>]* : exclude files matching basename or relative path\n"
	  "                    see the man page glob(7)\n"
	  "-f                : overwrite output file if it exists\n"
	  "-h                : show this help screen\n"
	  "-j                : print a JSON summary on stdout after success\n"
	  "-0                : NUL-separate paths for path lists\n"
	  "-l <n>            : set the piece length to 2^n bytes (15-28),\n"
	  "                    default is calculated from the total size\n"
	  "-n <name>         : set the name of the torrent,\n"
	  "                    default is the basename of the target\n"
	  "-o <filename>     : set the path and filename of the created file\n"
	  "                    default is <name>.torrent\n"
	  "-p                : set the private flag\n"
	  "-q                : suppress progress and success messages\n"
	  "-s                : add source string embedded in infohash\n"
#ifdef USE_PTHREADS
	  "-t <n>            : use <n> threads for calculating hashes\n"
	  "                    default is the number of CPU cores (max 64)\n"
#endif
	  "-v                : be verbose\n"
	  "-V                : display version information\n"
	  "-w <url>[,<url>]* : add web seed URLs\n"
	  "                    additional -w adds more URLs\n"
	  "-x                : ensure info hash is unique for easier cross-seeding\n"
#endif
	  "\nPlease send bug reports, patches, feature requests, praise and\n"
	  "general gossip about the program to: contact.tomc@yahoo.fr\n");
}

/*
 * print the full announce list
 */
static void print_announce_list(struct ll *list)
{
	unsigned int tier = 1;

	LL_FOR(node, list) {

		struct ll *inner_list = LL_DATA(node);

		fprintf(stderr, "    %u : %s\n",
			tier, LL_DATA_AS(LL_HEAD(inner_list), const char*));

		LL_FOR_FROM(inner_node, LL_NEXT(LL_HEAD(inner_list))) {
			fprintf(stderr, "        %s\n", LL_DATA_AS(inner_node, const char*));
		}

		tier += 1;
	}
}

/*
 * print the list of web seed URLs
 */
static void print_web_seed_list(struct ll *list)
{
	fprintf(stderr, "  Web Seed URL: ");

	if (LL_IS_EMPTY(list)) {
		fprintf(stderr, "none\n");
		return;
	}

	fprintf(stderr, "%s\n", LL_DATA_AS(LL_HEAD(list), const char*));
	LL_FOR_FROM(node, LL_NEXT(LL_HEAD(list))) {
		fprintf(stderr, "                %s\n", LL_DATA_AS(node, const char*));
	}
}

/*
 * print out all the options (stderr — keep stdout free for JSON)
 */
static void dump_options(struct metafile *m)
{
	fprintf(stderr, "Options:\n"
	       "  Announce URLs:\n");

	print_announce_list(m->announce_list);

	fprintf(stderr, "  Torrent name: %s\n"
	       "  Metafile:     %s\n"
	       "  Piece length: %u\n"
#ifdef USE_PTHREADS
	       "  Threads:      %ld\n"
#endif
	       "  Be verbose:   yes\n",
	       m->torrent_name, m->metainfo_file_path, m->piece_length
#ifdef USE_PTHREADS
	       ,m->threads
#endif
	       );

#ifdef USE_PTHREADS
	fprintf(stderr, "  SHA-1:        %s\n", mkt_sha1_backend_name());
#endif

	fprintf(stderr, "  Write date:   ");
	if (m->no_creation_date)
		fprintf(stderr, "no\n");
	else
		fprintf(stderr, "yes\n");
	
	fprintf(stderr, "  Write created_by:   ");
	if (m->no_created_by)
		fprintf(stderr, "no\n");
	else
		fprintf(stderr, "yes\n");

	print_web_seed_list(m->web_seed_list);

	/* Print source string only if set */
	if (m->source)
		fprintf(stderr, "\n Source:      %s\n\n", m->source);

	fprintf(stderr, "  Comment:      ");
	if (m->comment == NULL)
		fprintf(stderr, "none\n\n");
	else
		fprintf(stderr, "\"%s\"\n\n", m->comment);
}

static int file_data_cmp_by_name(const void *a, const void *b)
{
	const struct file_data *x = a, *y = b;
	return strcmp(x->path, y->path);
}

static void file_data_clear(void *data)
{
	struct file_data *fd = data;
	free(fd->path);
	free(fd->read_path);
}

static void free_inner_list(void *data)
{
	struct ll *list = data;
	/* URL strings are owned copies (data_size > 0); ll_free frees them */
	ll_free(list, NULL);
}

/*
 * Validates a comma-separated list of URLs
 * Returns 1 if all URLs are valid, 0 if any are invalid
 */
static int validate_url_list(const char *url_list)
{
	/* Handle NULL input */
	if (!url_list) {
		fprintf(stderr, "Error: NULL URL list\n");
		return 0;
	}
	
	const char *url_start = url_list;
	const char *url_end;
	int valid = 1;
	
	/* Process URLs in place without making a copy */
	while (*url_start) {
		/* Skip leading commas and spaces */
		while (*url_start == ',' || *url_start == ' ')
			url_start++;
			
		/* End of string */
		if (!*url_start)
			break;
			
		/* Find the end of this URL (next comma or end of string) */
		url_end = url_start;
		while (*url_end && *url_end != ',')
			url_end++;
			
		/* Empty URL segment */
		if (url_end == url_start) {
			url_start = url_end;
			if (*url_start)
				url_start++;
			continue;
		}

		if ((size_t)(url_end - url_start) > MKTORRENT_MAX_URL_LEN) {
			fprintf(stderr, "Error: URL exceeds %d bytes\n", MKTORRENT_MAX_URL_LEN);
			valid = 0;
			break;
		}
		{
			const char *p;
			for (p = url_start; p < url_end; p++) {
				if ((unsigned char)*p < 0x20) {
					fprintf(stderr, "Error: URL contains control characters\n");
					valid = 0;
					break;
				}
			}
			if (!valid)
				break;
		}
		
		/* Check protocol prefix without duplicating memory */
		if ((url_end - url_start >= 7 && strncmp(url_start, "http://", 7) == 0) ||
		    (url_end - url_start >= 8 && strncmp(url_start, "https://", 8) == 0) ||
		    (url_end - url_start >= 6 && strncmp(url_start, "udp://", 6) == 0)) {
			
			/* Find domain start */
			const char *domain;
			if (strncmp(url_start, "http://", 7) == 0)
				domain = url_start + 7;
			else if (strncmp(url_start, "https://", 8) == 0)
				domain = url_start + 8;
			else
				domain = url_start + 6;
				
			/* Find dot in domain */
			const char *dot = domain;
			while (dot < url_end && *dot != '.')
				dot++;
				
			/* Validate domain format */
			if (dot == domain || dot >= url_end - 1) {
				fprintf(stderr, "Error: Invalid URL format\n");
				valid = 0;
				break;
			}
		} else {
			fprintf(stderr, "Error: Invalid URL protocol\n");
			valid = 0;
			break;
		}
		
		/* Move to next URL */
		url_start = url_end;
		if (*url_start)
			url_start++;
	}
	
	return valid;
}

static int targets_push(char ***targets, int *ntargets, int *targets_cap, char *path)
{
	if (*ntargets >= *targets_cap) {
		int new_cap = *targets_cap ? *targets_cap * 2 : 8;
		char **tmp = realloc(*targets, (size_t)new_cap * sizeof(**targets));
		FATAL_IF0(tmp == NULL, "out of memory\n");
		*targets = tmp;
		*targets_cap = new_cap;
	}
	(*targets)[(*ntargets)++] = path;
	return 0;
}

/* Read paths from a stream: newline mode (skip blanks/#) or NUL-separated. */
static int load_paths_from_stream(FILE *fp, int null_sep,
				  char ***targets, int *ntargets, int *targets_cap)
{
	if (null_sep) {
		size_t cap = 256;
		size_t len = 0;
		char *buf = malloc(cap);
		FATAL_IF0(buf == NULL, "out of memory\n");
		int ch;
		while ((ch = fgetc(fp)) != EOF || len > 0) {
			if (ch == EOF || ch == '\0') {
				buf[len] = '\0';
				if (len > 0) {
					strip_ending_dirseps(buf);
					if (buf[0] != '\0') {
						char *dup = strdup(buf);
						FATAL_IF0(dup == NULL, "out of memory\n");
						targets_push(targets, ntargets, targets_cap, dup);
					}
				}
				len = 0;
				if (ch == EOF)
					break;
				continue;
			}
			if (len + 1 >= cap) {
				cap *= 2;
				char *tmp = realloc(buf, cap);
				FATAL_IF0(tmp == NULL, "out of memory\n");
				buf = tmp;
			}
			buf[len++] = (char)ch;
		}
		free(buf);
		return 0;
	}

	char line[PATH_MAX];
	while (fgets(line, sizeof(line), fp)) {
		char *p = line;
		char *nl;
		size_t len = strlen(line);

		/* MK-011: refuse silent truncation when the line fills the buffer. */
		if (len == sizeof(line) - 1 && line[len - 1] != '\n') {
			int ch;
			while ((ch = fgetc(fp)) != EOF && ch != '\n')
				;
			FATAL_IF(1, "path too long in path list (max %d bytes)\n",
				PATH_MAX - 1);
		}

		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '\0' || *p == '#' || *p == '\n' || *p == '\r')
			continue;
		nl = strchr(p, '\n');
		if (nl) *nl = '\0';
		nl = strchr(p, '\r');
		if (nl) *nl = '\0';
		strip_ending_dirseps(p);
		if (*p == '\0')
			continue;
		char *dup = strdup(p);
		FATAL_IF0(dup == NULL, "out of memory\n");
		targets_push(targets, ntargets, targets_cap, dup);
	}
	return 0;
}

/*
 * parse and check the command line options given
 * and fill out the appropriate fields of the
 * metafile structure
 */
EXPORT int init(struct metafile *m, int argc, char *argv[])
{
	int c;			/* return value of getopt() */
	const char *from_file = NULL;
	int use_stdin = 0;
	int null_sep = 0;
	char **targets = NULL;
	int ntargets = 0;
	int targets_cap = 0;
	const uintmax_t piece_len_maxes[] = {
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		(uintmax_t) BIT15MAX * ONEMEG, (uintmax_t) BIT16MAX * ONEMEG,
		(uintmax_t) BIT17MAX * ONEMEG, (uintmax_t) BIT18MAX * ONEMEG,
		(uintmax_t) BIT19MAX * ONEMEG, (uintmax_t) BIT20MAX * ONEMEG,
		(uintmax_t) BIT21MAX * ONEMEG, (uintmax_t) BIT22MAX * ONEMEG,
		(uintmax_t) BIT23MAX * ONEMEG
	};

	const int num_piece_len_maxes = sizeof(piece_len_maxes) /
	    sizeof(piece_len_maxes[0]);

	enum {
		OPT_FROM_FILE = 1000,
		OPT_NO_FOLLOW_SYMLINKS,
		OPT_DIRECT_IO,
		OPT_IO_URING,
		OPT_MAX_FILES,
		OPT_MAX_BYTES,
		OPT_STDIN,
		OPT_NODE,
		OPT_INCLUDE,
		OPT_SKIP_HIDDEN,
		OPT_IO_URING_DEPTH,
		OPT_IO_URING_SQPOLL,
		OPT_SHA1
	};

#ifdef USE_LONG_OPTIONS
	/* the option structure to pass to getopt_long() */
	static struct option long_options[] = {
		{"announce", 1, NULL, 'a'},
		{"comment", 1, NULL, 'c'},
		{"no-date", 0, NULL, 'd'},
		{"no-created-by", 0, NULL, 'D'},
		{"exclude", 1, NULL, 'e'},
		{"force", 0, NULL, 'f'},
		{"help", 0, NULL, 'h'},
		{"json", 0, NULL, 'j'},
		{"piece-length", 1, NULL, 'l'},
		{"name", 1, NULL, 'n'},
		{"output", 1, NULL, 'o'},
		{"private", 0, NULL, 'p'},
		{"quiet", 0, NULL, 'q'},
		{"source", 1, NULL, 's'},
#ifdef USE_PTHREADS
		{"threads", 1, NULL, 't'},
#endif
		{"verbose", 0, NULL, 'v'},
		{"version", 0, NULL, 'V'},
		{"web-seed", 1, NULL, 'w'},
		{"cross-seed", 0, NULL, 'x'},
		{"from-file", 1, NULL, OPT_FROM_FILE},
		{"stdin", 0, NULL, OPT_STDIN},
		{"null", 0, NULL, '0'},
		{"no-follow-symlinks", 0, NULL, OPT_NO_FOLLOW_SYMLINKS},
		{"direct-io", 0, NULL, OPT_DIRECT_IO},
		{"io-uring", 0, NULL, OPT_IO_URING},
		{"max-files", 1, NULL, OPT_MAX_FILES},
		{"max-bytes", 1, NULL, OPT_MAX_BYTES},
		{"node", 1, NULL, OPT_NODE},
		{"include", 1, NULL, OPT_INCLUDE},
		{"skip-hidden", 0, NULL, OPT_SKIP_HIDDEN},
		{"io-uring-depth", 1, NULL, OPT_IO_URING_DEPTH},
		{"io-uring-sqpoll", 0, NULL, OPT_IO_URING_SQPOLL},
#ifdef USE_PTHREADS
		{"sha1", 1, NULL, OPT_SHA1},
#endif
		{NULL, 0, NULL, 0}
	};
#endif

	/* Check for --version first.
	 * Return 1 = clean early exit (help/version); 0 = continue; fatal() aborts. */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-V") == 0) {
			display_version();
			return 1;
		}
	}

	m->announce_list = ll_new();
	FATAL_IF0(m->announce_list == NULL, "out of memory\n");

	m->web_seed_list = ll_new();
	FATAL_IF0(m->web_seed_list == NULL, "out of memory\n");

	m->file_list = ll_new();
	FATAL_IF0(m->file_list == NULL, "out of memory\n");

	m->exclude_list = ll_new();
	FATAL_IF0(m->exclude_list == NULL, "out of memory\n");

	m->node_list = ll_new();
	FATAL_IF0(m->node_list == NULL, "out of memory\n");

	m->include_list = ll_new();
	FATAL_IF0(m->include_list == NULL, "out of memory\n");

	/* now parse the command line options */
#ifdef USE_PTHREADS
#define OPT_STRING "0a:c:e:dDfhl:jJn:o:pqs:t:vw:xV"
#else
#define OPT_STRING "0a:c:e:dDfhl:jJn:o:pqs:vw:xV"
#endif
#ifdef USE_LONG_OPTIONS
	while ((c = getopt_long(argc, argv, OPT_STRING,
				long_options, NULL)) != -1) {
#else
	while ((c = getopt(argc, argv, OPT_STRING)) != -1) {
#endif
#undef OPT_STRING
		switch (c) {
		case '0':
			null_sep = 1;
			break;
		case 'a':
			if (m->announce_list == NULL) {
				m->announce_list = ll_new();
				FATAL_IF0(m->announce_list == NULL, "out of memory\n");
			}

			/* Check if URL list is valid before trying to add it */
			if (!validate_url_list(optarg)) {
				/* Error message already printed by validate_url_list */
				return -1;
			}

			struct ll *url_list = get_slist(optarg, 0);
			if (url_list == NULL || ll_append(m->announce_list, url_list, 0) == NULL) {
				fprintf(stderr, "Error: failed to add announce URL to list\n");
				if (url_list) ll_free(url_list, NULL);
				return -1;
			}
			break;
		case 'c': {
			size_t clen;
			size_t i;

			clen = strlen(optarg);
			FATAL_IF(clen > MKTORRENT_MAX_COMMENT_LEN,
				"comment exceeds %d bytes\n", MKTORRENT_MAX_COMMENT_LEN);
			for (i = 0; i < clen; i++) {
				FATAL_IF0((unsigned char)optarg[i] < 0x20,
					"comment contains control characters\n");
			}
			if (m->comment)
				free(m->comment);
			m->comment = strdup(optarg);
			FATAL_IF0(m->comment == NULL, "out of memory\n");
			break;
		}
		case 'd':
			m->no_creation_date = 1;
			break;
		case 'D':
			m->no_created_by = 1;
			break;
		case 'e':
			if (add_patterns(m->exclude_list, optarg) != 0)
				return -1;
			break;
		case 'f':
			m->force_overwrite = 1;
			break;
		case 'h':
			print_help();
			return 1;
		case 'j':
		case 'J':
			m->json_output = 1;
			break;
		case 'l': {
			unsigned int bytes = 0;
			FATAL_IF0(parse_piece_length_arg(optarg, &bytes) != 0,
				"piece length must be exponent 15-28 or a power-of-two "
				"size with optional K/M/G in [32KiB, 256MiB]\n");
			m->piece_length = bytes;
			m->piece_length_is_bytes = 1;
			break;
		}
		case 'n':
			m->torrent_name = optarg;
			break;
		case 'o':
			m->metainfo_file_path = optarg;
			break;
		case 'p':
			m->private = 1;
			break;
		case 'q':
			m->quiet = 1;
			break;
		case 's':
			m->source = optarg;
			break;
#ifdef USE_PTHREADS
		case 't': {
			char *end = NULL;
			long v;

			errno = 0;
			v = strtol(optarg, &end, 10);
			FATAL_IF0(errno != 0 || end == optarg || *end != '\0' ||
				  v < 1 || v > 64,
				"the number of threads must be a number between 1 and 64\n");
			m->threads = v;
			break;
		}
#endif
		case 'v':
			m->verbose = 1;
			break;
		case 'w':
			/* Validate web seed URL list first */
			if (!validate_url_list(optarg)) {
				/* Error message already printed by validate_url_list */
				return -1;
			}

			struct ll *web_seed_list = get_slist(optarg, 0);
			if (web_seed_list == NULL) {
				fprintf(stderr, "Error: failed to add web seed URL to list\n");
				return -1;
			}
			ll_extend(m->web_seed_list, web_seed_list);
			break;
		case 'x':
			m->cross_seed = 1;
			break;
#ifdef USE_LONG_OPTIONS
		case OPT_FROM_FILE:
			from_file = optarg;
			break;
		case OPT_STDIN:
			use_stdin = 1;
			break;
		case OPT_NO_FOLLOW_SYMLINKS:
			m->no_follow_symlinks = 1;
			break;
		case OPT_DIRECT_IO:
			m->direct_io = 1;
			break;
		case OPT_IO_URING:
#ifndef USE_IO_URING
			fatal("--io-uring requires a build with liburing (USE_IO_URING)\n");
#else
#ifndef USE_PTHREADS
			fatal("--io-uring requires pthreads support\n");
#else
			m->io_uring = 1;
#endif
#endif
			break;
		case OPT_IO_URING_DEPTH: {
			char *end = NULL;
			unsigned long v;

			errno = 0;
			v = strtoul(optarg, &end, 10);
			FATAL_IF0(errno != 0 || end == optarg || *end != '\0'
				|| v < 1 || v > MKTORRENT_IOURING_MAX_DEPTH,
				"--io-uring-depth must be an integer in [1, 256]\n");
			m->io_uring_depth = (unsigned)v;
			m->io_uring_depth_set = 1;
			break;
		}
		case OPT_IO_URING_SQPOLL:
			m->io_uring_sqpoll = 1;
			m->io_uring_sqpoll_set = 1;
			break;
		case OPT_NODE: {
			struct dht_node node;

			FATAL_IF0(parse_dht_node(optarg, &node) != 0,
				"--node requires host:port with port in 1-65535\n");
			if (ll_append(m->node_list, &node, sizeof(node)) == NULL) {
				free(node.host);
				fatal("out of memory\n");
			}
			break;
		}
		case OPT_INCLUDE:
			if (add_patterns(m->include_list, optarg) != 0)
				return -1;
			break;
		case OPT_SKIP_HIDDEN:
			m->skip_hidden = 1;
			break;
#ifdef USE_PTHREADS
		case OPT_SHA1:
			if (strcasecmp(optarg, "auto") == 0) {
				mkt_sha1_set_pref(MKT_SHA1_AUTO);
			} else if (strcasecmp(optarg, "openssl") == 0) {
				/* Without OpenSSL the bundled SHA-1 is used. */
				mkt_sha1_set_pref(MKT_SHA1_OPENSSL);
			} else if (strcasecmp(optarg, "isal") == 0) {
#ifndef USE_ISAL
				fatal("--sha1=isal requires a build with ISA-L Crypto (USE_ISAL)\n");
#else
				mkt_sha1_set_pref(MKT_SHA1_ISAL);
#endif
			} else {
				fatal("--sha1 must be auto, openssl, or isal\n");
			}
			break;
#endif
		case OPT_MAX_FILES: {
			char *end = NULL;
			unsigned long long v;

			errno = 0;
			v = strtoull(optarg, &end, 10);
			FATAL_IF0(errno != 0 || end == optarg || *end != '\0' || v == 0,
				"--max-files must be a positive integer\n");
			m->max_files = (uintmax_t)v;
			break;
		}
		case OPT_MAX_BYTES: {
			char *end = NULL;
			unsigned long long v;

			errno = 0;
			v = strtoull(optarg, &end, 10);
			FATAL_IF0(errno != 0 || end == optarg || *end != '\0',
				"--max-bytes must be a non-negative integer\n");
			m->max_bytes = (uintmax_t)v;
			break;
		}
#endif
		case '?':
			fatal("use -h for help.\n");
		}
	}

	FATAL_IF0((m->io_uring_depth_set || m->io_uring_sqpoll_set) && !m->io_uring,
		"--io-uring-depth and --io-uring-sqpoll require --io-uring\n");
	if (m->io_uring_depth == 0)
		m->io_uring_depth = MKTORRENT_IOURING_DEFAULT_DEPTH;

	/* collect positional targets */
	for (int i = optind; i < argc; i++) {
		strip_ending_dirseps(argv[i]);
		targets_push(&targets, &ntargets, &targets_cap, argv[i]);
	}

	/* optionally load more targets from --from-file */
	if (from_file) {
		FILE *fp = fopen(from_file, "r");
		FATAL_IF(fp == NULL, "cannot open --from-file '%s': %s\n",
			from_file, strerror(errno));
		load_paths_from_stream(fp, null_sep, &targets, &ntargets, &targets_cap);
		fclose(fp);
	}

	if (use_stdin)
		load_paths_from_stream(stdin, null_sep, &targets, &ntargets, &targets_cap);

	FATAL_IF0(ntargets <= 0,
		"must specify the contents, use -h for help\n");

	if (ntargets > 1 || from_file || use_stdin) {
		FATAL_IF0(m->torrent_name == NULL || *m->torrent_name == '\0',
			"-n/--name is required with multiple targets, --from-file, or --stdin\n");
	}

#ifdef USE_PTHREADS
	/* threads already validated as [1,64] when -t was passed; else auto */
	if (!m->threads) {
#ifdef _SC_NPROCESSORS_ONLN
		m->threads = sysconf(_SC_NPROCESSORS_ONLN);
		if (m->threads <= 0)
#endif
			m->threads = 2; /* some sane default */
		if (m->threads > 64)
			m->threads = 64;
	}
#endif

	/* if the torrent name isn't set use the basename of the first target */
	if (m->torrent_name == NULL)
		m->torrent_name = get_basename(targets[0]);

	/* if we still don't have a torrent name, default to "unnamed" */
	if (m->torrent_name == NULL || *m->torrent_name == '\0') {
		m->torrent_name = "unnamed";
	}

	/* make sure m->metainfo_file_path is the absolute path to the file */
	set_absolute_file_path(m);

	/* if we should be verbose print out all the options
	   as we have set them (never mix with JSON stdout) */
	if (m->verbose && !m->json_output && !m->quiet)
		dump_options(m);

	if (ntargets == 1 && !from_file) {
		/* classic single-target behaviour */
		int isdir = is_dir(m, targets[0]);
		if (isdir < 0) {
			free(targets);
			return -1;
		}
		m->target_is_directory = isdir;
		if (m->target_is_directory) {
			char absroot[PATH_MAX];

			FATAL_IF(realpath(targets[0], absroot) == NULL,
				"cannot resolve '%s': %s\n", targets[0], strerror(errno));
			m->scan_root_abs = absroot;
			FATAL_IF(chdir(absroot), "cannot change directory to '%s': %s\n",
				absroot, strerror(errno));

			if (file_tree_walk("." DIRSEP, MAX_OPENFD, process_node, m)) {
				m->scan_root_abs = NULL;
				free(targets);
				return -1;
			}
			m->scan_root_abs = NULL;
		}
	} else {
		/* multi-target / from-file: always multi-file torrent */
		char saved_cwd[PATH_MAX];
		m->target_is_directory = 1;
		FATAL_IF0(getcwd(saved_cwd, sizeof(saved_cwd)) == NULL,
			"getcwd failed: %s\n");

		for (int i = 0; i < ntargets; i++) {
			struct stat st;
			char absbuf[PATH_MAX];
			const char *base;

			if (m->no_follow_symlinks) {
				if (lstat(targets[i], &st) != 0) {
					fprintf(stderr, "Cannot lstat '%s': %s\n",
						targets[i], strerror(errno));
					free(targets);
					return -1;
				}
				if (S_ISLNK(st.st_mode)) {
					fprintf(stderr, "warning: skipping symlink '%s'\n",
						targets[i]);
					continue;
				}
			} else if (stat(targets[i], &st) != 0) {
				fprintf(stderr, "Cannot stat '%s': %s\n",
					targets[i], strerror(errno));
				free(targets);
				return -1;
			}

			base = get_basename(targets[i]);
			if (realpath(targets[i], absbuf) == NULL) {
				fprintf(stderr, "Cannot resolve '%s': %s\n",
					targets[i], strerror(errno));
				free(targets);
				return -1;
			}

			if (S_ISDIR(st.st_mode)) {
				m->scan_prefix = base;
				m->scan_root_abs = absbuf;
				FATAL_IF(chdir(absbuf),
					"cannot change directory to '%s': %s\n",
					absbuf, strerror(errno));
				if (file_tree_walk("." DIRSEP, MAX_OPENFD, process_node, m)) {
					if (chdir(saved_cwd) != 0) {
						fprintf(stderr, "cannot restore directory '%s': %s\n",
							saved_cwd, strerror(errno));
					}
					m->scan_prefix = NULL;
					m->scan_root_abs = NULL;
					free(targets);
					return -1;
				}
				FATAL_IF(chdir(saved_cwd),
					"cannot restore directory '%s': %s\n",
					saved_cwd, strerror(errno));
				m->scan_prefix = NULL;
				m->scan_root_abs = NULL;
			} else if (S_ISREG(st.st_mode)) {
				if (add_file_entry(m, base, absbuf, (uintmax_t)st.st_size) != 0) {
					free(targets);
					return -1;
				}
			} else {
				fprintf(stderr, "'%s' is neither a directory nor regular file\n",
					targets[i]);
				free(targets);
				return -1;
			}
		}
	}

	/* free strdup'd --from-file/--stdin entries; argv entries must not be freed */
	if (from_file || use_stdin) {
		for (int i = 0; i < ntargets; i++) {
			int from_argv = 0;
			for (int j = optind; j < argc; j++) {
				if (targets[i] == argv[j]) {
					from_argv = 1;
					break;
				}
			}
			if (!from_argv)
				free(targets[i]);
		}
	}
	free(targets);

	FATAL_IF0(LL_IS_EMPTY(m->file_list), "no files to hash\n");

	ll_sort(m->file_list, file_data_cmp_by_name);

	/* determine the piece length based on the torrent size if
	   it was not user specified. CLI may already set bytes. */
	if (!m->piece_length_is_bytes) {
		int i;
		unsigned int exp = 0;

		for (i = 15; i < num_piece_len_maxes && exp == 0; i++)
			if (m->size <= piece_len_maxes[i])
				exp = (unsigned int)i;
		if (exp == 0)
			exp = (unsigned int)num_piece_len_maxes;
		m->piece_length = 1u << exp;
	} else {
		FATAL_IF0(m->piece_length < (1u << 15) || m->piece_length > (1u << 28)
			|| (m->piece_length & (m->piece_length - 1)) != 0,
			"piece length must be a power of two in [32KiB, 256MiB]\n");
	}

	/* calculate the number of pieces
	   pieces = ceil( size / piece_length ); guard overflow before malloc */
	{
		uintmax_t pieces_u;

		FATAL_IF0(m->piece_length == 0, "invalid piece length\n");
		pieces_u = (m->size + m->piece_length - 1) / m->piece_length;
		FATAL_IF0(pieces_u == 0 || pieces_u > UINT_MAX,
			"too many pieces for this torrent size/piece length\n");
		FATAL_IF0(pieces_u > SIZE_MAX / SHA_DIGEST_LENGTH,
			"piece count exceeds addressable hash buffer size\n");
		m->pieces = (unsigned int)pieces_u;
	}

	/* now print the size and piece count if we should be verbose */
	if (m->verbose && !m->json_output && !m->quiet)
		fprintf(stderr, "\n%" PRIuMAX " bytes in all\n"
			"that's %u pieces of %u bytes each\n\n",
			m->size, m->pieces, m->piece_length);

	return 0;
}

EXPORT void cleanup_metafile(struct metafile *m)
{
	ll_free(m->announce_list, free_inner_list);

	ll_free(m->file_list, file_data_clear);

	/* Owned string copies from get_slist (data_size > 0) */
	ll_free(m->web_seed_list, NULL);

	ll_free(m->exclude_list, NULL);
	ll_free(m->include_list, NULL);

	ll_free(m->node_list, free_dht_node);

	free(m->comment);
	free(m->metainfo_file_path);
}
