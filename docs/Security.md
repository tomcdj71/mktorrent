# Security notes

## Threat model

mktorrent is a **local CLI**: it reads paths you pass (or list via `--from-file` / `--stdin`) and writes a `.torrent` metainfo file. It is not a network service. Risks are hostile directory trees, hostile CLI input, operator mistakes, and resource exhaustion.

## Recommendations

- For trees you do not fully trust, use **`--no-follow-symlinks`** so nested symlinks are skipped (default walk still follows symlinks for BitTorrent compatibility). With a resolved scan root, paths that `realpath` outside that root are skipped. Prefer **`--skip-hidden`** and **`--include`** / **`--exclude`** to limit what is hashed.
- Cap large trees with **`--max-files`** (default 1 000 000) and optionally **`--max-bytes`**.
- Prefer **`--io-uring`** only on Linux builds with liburing when you need cold-read throughput. Interrupt / error paths drain the ring and reclaim piece buffers before teardown. **`--io-uring-depth`** / **`--io-uring-sqpoll`** require **`--io-uring`**.
- **`-t` / `--threads`** must be an integer in **[1, 64]**; invalid values exit with usage status.
- **`-f` / `--force`** overwrites the user-chosen `-o` path with truncation (`O_TRUNC`). That is intentional for CLIs; automation should still point `-o` at a dedicated path so a wrong target is not clobbered.
- Piece count is checked against `UINT_MAX` and `SIZE_MAX / 20` before allocating the hash string.
- Announce/web-seed URLs are capped at **8192** bytes; comments at **65536** bytes, without control characters.
- Overlong lines in `--from-file` / newline path lists are rejected (no silent truncation).
- Exclude/include patterns are capped at **256** entries, each ≤ **1024** bytes (fnmatch DoS bound).
- JSON summary strings escape controls and invalid UTF-8 bytes as `\u00XX` so stdout stays structurally valid JSON for UTF-8-only consumers.

## I/O limits

- Default hashing uses buffered/`--io-uring` reads into piece buffers, or multi-thread zero-copy `mmap` views (`-t > 1`) without copying full pieces.
- Files larger than `off_t` can address are refused before open.

## Cryptography and dependencies

- BitTorrent v1 piece hashes are **SHA-1** by protocol; that is not an implementation choice. Optional **ISA-L Crypto** is another SHA-1 implementation for piece workers only — digests and infohashes stay identical.
- Default builds link **OpenSSL** for SHA-1 (`MKTORRENT_OPENSSL=ON`). Distro ABI/CVE cadence applies. Build with `-DMKTORRENT_OPENSSL=OFF` to use the bundled `sha1.c` instead.
- `libisal_crypto` appears in package `Depends` only when the binary is dynamically linked against it (`dpkg-shlibdeps`).

## Signal handling

`SIGINT` / `SIGTERM` only set an async-signal-safe flag; cleanup runs on the main path after hashing observes it.

## Related

See [Hashing-IO](Hashing-IO.md) for I/O modes and [Testing](Testing.md) for the test suite.
