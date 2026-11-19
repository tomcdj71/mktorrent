# Command Line Options

## Basic Syntax

```bash
mktorrent [options] <target> [target...]
```

Where each `<target>` is a file or directory to include. Announce URLs are optional (DHT-only torrents are supported).

## Common Options

| Short | Long Option | Description | Example |
|-------|------------|-------------|----------|
| `-a` | `--announce` | Tracker's announce URL (optional; repeatable) | `-a http://tracker.example.com/announce` |
| `-o` | `--output` | Output file name | `-o my.torrent` |
| `-l` | `--piece-length` | Exponent 15–28, or bytes with optional `K`/`M`/`G` (power of two, 32 KiB–256 MiB; default: auto) | `-l 20` or `-l 1M` |
| `-p` | `--private` | Make torrent private | `-p` |
| `-c` | `--comment` | Add comment | `-c "My torrent"` |
| `-n` | `--name` | Set torrent name (default: basename; required with multiple targets / `--from-file` / `--stdin`) | `-n "Custom Name"` |
| `-s` | `--source` | Set source flag for private trackers | `-s "MY-GROUP"` |
| `-t` | `--threads` | Set number of threads for hashing (max 64) | `-t 4` |
| `-v` | `--verbose` | Be verbose | `-v` |
| `-q` | `--quiet` | Suppress progress/success messages | `-q` |
| `-j` / `-J` | `--json` | Print a JSON summary on stdout after success | `--json` |
| `-h` | `--help` | Show help | `-h` |
| `-V` | `--version` | Show version | `-V` |

## Advanced Options

| Short | Long Option | Description | Example |
|-------|------------|-------------|----------|
| `-w` | `--web-seed` | Add web seed URLs | `-w http://direct.example.com/file` |
| `-e` | `--exclude` | Exclude files matching basename or relative path (fnmatch; wins over include) | `-e "subdir/*.tmp"` |
| | `--include` | Only include files matching basename or relative path (fnmatch; dirs still walked) | `--include='*.dat'` |
| | `--skip-hidden` | Skip basenames starting with `.` | `--skip-hidden` |
| `-f` | `--force` | Overwrite output file | `-f` |
| `-d` | `--no-date` | Don't write creation date | `--no-date` |
| `-D` | `--no-created-by` | Don't write created by | `--no-created-by` |
| `-x` | `--cross-seed` | Ensure unique info hash | `--cross-seed` |
| `-0` | `--null` | NUL-separate paths for `--from-file` / `--stdin` | `-0 --stdin` |
| | `--from-file` | Read target paths from a file (one per line) | `--from-file list.txt` |
| | `--stdin` | Read target paths from stdin | `--stdin -n name` |
| | `--no-follow-symlinks` | Skip symbolic links instead of following them | `--no-follow-symlinks` |
| | `--max-files` | Refuse after *n* files (default 1000000) | `--max-files=1000` |
| | `--max-bytes` | Refuse after *n* total bytes (0 = unlimited) | `--max-bytes=1073741824` |
| | `--node` | Add DHT bootstrap node `host:port` (repeatable) | `--node=127.0.0.1:6881` |
| | `--direct-io` | Opt-in O_DIRECT for large files | `--direct-io` |
| | `--io-uring` | Opt-in io_uring piece reads (Linux + liburing) | `--io-uring` |
| | `--io-uring-depth` | Ring depth 1–256 (default 32; requires `--io-uring`) | `--io-uring-depth=64` |
| | `--io-uring-sqpoll` | Experimental `IORING_SETUP_SQPOLL` (requires `--io-uring`) | `--io-uring-sqpoll` |
| | `--sha1` | Piece SHA-1 backend: `auto` (OpenSSL), `openssl`, or `isal` (batched multi-buffer; needs `USE_ISAL`; workers capped 1–2) | `--sha1=isal` |

## Piece Length

`-l` / `--piece-length` accepts either:

- an **exponent** `15–28` → piece size `2^n` bytes (example: `-l 20` → 1 MiB), or
- a **byte size** that is a power of two in `[32 KiB, 256 MiB]`, with optional `K`/`M`/`G` (1024-based; examples: `-l 1M`, `-l 1048576`).

The piece length is automatically chosen based on the total size when `-l` is omitted:

| Total Size | Exponent | Piece Length |
|------------|----------|--------------|
| ≤ 50MB | 15 | 32 KiB |
| ≤ 100MB | 16 | 64 KiB |
| ≤ 200MB | 17 | 128 KiB |
| ≤ 400MB | 18 | 256 KiB |
| ≤ 800MB | 19 | 512 KiB |
| ≤ 1.6GB | 20 | 1 MiB |
| ≤ 3.2GB | 21 | 2 MiB |
| ≤ 6.4GB | 22 | 4 MiB |
| > 6.4GB | 23 | 8 MiB |

## JSON summary

With `--json` / `-j` / `-J`, a successful create prints one JSON object on stdout:

| Field | Type | Meaning |
|-------|------|---------|
| `ok` | bool | always `true` on success |
| `path` | string | output `.torrent` path |
| `name` | string | torrent name |
| `length` | number | total bytes |
| `pieceLength` | number | piece length in bytes |
| `pieces` | number | piece count |
| `private` | bool | private flag |
| `infoHash` | string | hex SHA-1 of the bencoded `info` dict |
| `announce` | string[] | flattened announce URLs |
| `nodes` | object[] | DHT bootstrap nodes `{host,port}` (may be empty) |

Invalid UTF-8 bytes in string fields are escaped as `\u00XX`.

## Exit Status

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | Usage or argument error |
| 2 | I/O, hashing, or write failure |
| 130 | Interrupted by SIGINT |

## Examples

1. Create a basic torrent:
```bash
mktorrent -a http://tracker.example.com/announce path/to/files/
```

2. Create a private torrent with custom piece length (2 MiB pieces):
```bash
mktorrent -a http://tracker.example.com/announce -p -l 21 -o release.torrent path/to/files/
```

3. Create a torrent with multiple trackers and web seeds:
```bash
mktorrent -a http://tracker1.example.com/announce \
          -a http://tracker2.example.com/announce \
          -w http://direct1.example.com/file \
          -w http://direct2.example.com/file \
          path/to/files/
```

4. Create a torrent and emit JSON for scripting/automation:
```bash
mktorrent --json -a http://tracker.example.com/announce -o out.torrent path/to/file
```

5. Create a DHT-only torrent (no announce):
```bash
mktorrent -o dht.torrent path/to/file
```

6. Create a torrent for a private tracker with source flag:
```bash
mktorrent -a http://private-tracker.example.com/announce \
          -p \
          -s "MY-GROUP" \
          -c "Release Info" \
          -n "Release.Name.1080p" \
          path/to/files/
```

7. Create a torrent with multiple threads and verbose output:
```bash
mktorrent -a http://tracker.example.com/announce \
          -t 4 \
          -v \
          path/to/files/
```

8. Multiple targets (requires `-n`):
```bash
mktorrent -n my-release -o out.torrent file1.iso file2.iso
```

9. Human piece size, include filter, and DHT node:
```bash
mktorrent -l 1M --include '*.mkv' --skip-hidden \
          --node 127.0.0.1:6881 -o out.torrent path/to/files/
```
