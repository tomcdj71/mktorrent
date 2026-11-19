#ifndef MKTORRENT_MKTORRENT_H
#define MKTORRENT_MKTORRENT_H

#ifdef _WIN32
#define DIRSEP      "\\"
#define DIRSEP_CHAR '\\'
#else
#define DIRSEP      "/"
#define DIRSEP_CHAR '/'
#endif

/* number of bytes in one MB */
#define ONEMEG 1048576

/* max torrent size in MB for a given piece length in bits */
/* where an X bit piece length equals a 2^X byte piece size */
#define BIT23MAX 12800
#define BIT22MAX 6400
#define BIT21MAX 3200
#define BIT20MAX 1600
#define BIT19MAX 800
#define BIT18MAX 400
#define BIT17MAX 200
#define BIT16MAX 100
#define BIT15MAX 50

/* Stable process exit codes */
#define MKTORRENT_EXIT_OK         0
#define MKTORRENT_EXIT_USAGE      1
#define MKTORRENT_EXIT_IO         2
#define MKTORRENT_EXIT_INTERRUPT  130

/* DoS / input caps (overridable via CLI where noted) */
#define MKTORRENT_DEFAULT_MAX_FILES 1000000ULL
#define MKTORRENT_MAX_URL_LEN       8192
#define MKTORRENT_MAX_COMMENT_LEN   65536
#define MKTORRENT_MAX_PATTERNS      256
#define MKTORRENT_MAX_PATTERN_LEN   1024
#define MKTORRENT_IOURING_MAX_DEPTH 256
#define MKTORRENT_IOURING_DEFAULT_DEPTH 32

#ifndef SHA_DIGEST_LENGTH
#define SHA_DIGEST_LENGTH 20
#endif

#include <stdint.h>

#include "ll.h"

struct file_data {
	char *path;       /* path recorded inside the torrent */
	char *read_path;  /* path used for hashing; NULL means use path */
	uintmax_t size;
};

struct dht_node {
	char *host;
	unsigned port;
};

static inline const char *file_read_path(const struct file_data *f)
{
	return (f->read_path != NULL) ? f->read_path : f->path;
}

struct metafile {
	/* options */
	unsigned int piece_length; /* piece length in bytes (after init) */
	int piece_length_is_bytes; /* 1 if CLI already set piece_length in bytes */
	struct ll *announce_list;  /* announce URLs */
	char *comment;             /* optional comment */
	const char *torrent_name;  /* name of torrent (name of directory) */
	char *metainfo_file_path;  /* absolute path to the metainfo file */
	struct ll *web_seed_list;  /* web seed URLs */
	struct ll *node_list;      /* DHT bootstrap nodes (host, port) */
	int target_is_directory;   /* target is a directory / multi-file */
	int no_creation_date;      /* don't write the creation date */
	int no_created_by;         /* don't write the creating software */
	int private;               /* set the private flag */
	char *source;              /* set source for private trackers */
	int cross_seed;            /* ensure info hash is unique for easier cross-seeding */
	int verbose;               /* be verbose */
	int quiet;                 /* suppress progress/success messages */
	int json_output;           /* emit a single JSON object on stdout after success */
	int force_overwrite;       /* overwrite existing output file */
	int no_follow_symlinks;    /* skip symlinks instead of following */
	int skip_hidden;           /* skip basenames starting with '.' */
	int direct_io;             /* opt-in O_DIRECT for large files */
	int io_uring;              /* opt-in io_uring reader (Linux + liburing) */
	unsigned io_uring_depth;   /* ring depth (default 32, max 256) */
	int io_uring_sqpoll;       /* IORING_SETUP_SQPOLL (experimental) */
	int io_uring_depth_set;    /* 1 if --io-uring-depth given */
	int io_uring_sqpoll_set;   /* 1 if --io-uring-sqpoll given */
	struct ll *exclude_list;   /* exclude list (fnmatch) */
	struct ll *include_list;   /* include list (fnmatch); empty = all */
	long threads;              /* number of threads used for hashing */
	uintmax_t max_files;       /* refuse after this many files (default 1e6) */
	uintmax_t max_bytes;       /* 0 = unlimited aggregate size */

	/* information calculated by read_dir() */
	uintmax_t size;            /* combined size of all files */
	uintmax_t file_count;      /* number of files added */
	struct ll *file_list;      /* list of files and their sizes */
	unsigned int pieces;       /* number of pieces */

	/* filled by write_metainfo() */
	unsigned char info_hash[SHA_DIGEST_LENGTH];
	int info_hash_valid;

	/* internal while scanning (not CLI-facing) */
	const char *scan_prefix;   /* optional path prefix inside torrent */
	const char *scan_root_abs; /* absolute root while walking a directory */
};

#endif /* MKTORRENT_MKTORRENT_H */
