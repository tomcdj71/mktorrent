# Hashing and I/O

## Default I/O policy

- **`-t 1`:** buffered `read()` straight into owned piece buffers (Rudde-style), with `posix_fadvise(SEQUENTIAL)`.
- **`-t > 1`:** **zero-copy mmap** — map each file and queue piece views that point into the mapping (no memcpy of full pieces). Cross-file partial pieces still copy into an owned buffer. Mappings stay alive until hashing finishes. Falls back to `read()` if mmap fails or `--direct-io` is set.
- **O_DIRECT** only when `--direct-io` is passed (and the file is large enough). Warm page cache workloads are usually faster without O_DIRECT.
- **io_uring** only when `--io-uring` is passed (Linux build linked with liburing). Reads fill the existing piece-buffer queue so SHA-1 workers overlap with disk I/O. It is never the default; use it when cold-cache benches show a clear gain versus buffered I/O. Combinable with `--direct-io`.
  - `--io-uring-depth=<n>` (1–256, default **32**) clamps the ring size (also limited by the piece-buffer pool).
  - `--io-uring-sqpoll` enables `IORING_SETUP_SQPOLL`; setup failure is fatal (no silent fallback). Treat as experimental and enable only when your own benches show a benefit. Optional bench modes: set `BENCH_IOURING_KNOBS=1` so `bench_hash.sh` also records `iouring-d8`, `iouring-d32`, `iouring-d64`, and `iouring-sqpoll`.
  - On a cold 200 MiB sample (`-t 1`), default depth behaved like depth 8; depth 64 was slightly slower; sqpoll was about the same as the default.

## Threading

- Default worker count is the number of online CPU cores, capped at **64**.
- Override with `-t` / `--threads` (must be an integer in **[1, 64]**).
- OpenSSL/bundled workers use a lean one-piece-at-a-time loop and a piece-buffer pool of about **3 × threads** (Rudde-style). ISA-L keeps multi-buffer batching and a larger pool.
- Workers are **not** CPU-pinned by default.

## Security

CLI threat model, symlink advice, and interrupt-safe io_uring teardown: [Security](Security.md).

## Benchmarks

Opt-in suite (`PERFORMANCE_TESTS=1`) writes `tests/test_results/bench.jsonl` via `tests/bench_hash.sh`:

| Field | Meaning |
|-------|---------|
| `mode` | `buffered` (default), `direct` (`--direct-io`), `iouring`, or `iouring+direct` (plus optional depth/sqpoll); with `--sha1`, also `sha1-openssl` / `sha1-isal` |
| `threads` | worker count |
| `fileMB` | fixture size |
| `cold` | `true` after cache drop, `false` warm, `null` if drop skipped |
| `seconds` / `MBps` | wall time and throughput |
| `ok` | create succeeded |

```bash
PERFORMANCE_TESTS=1 cmake --build build --target test
jq -s 'sort_by(.fileMB,.threads,.mode,.cold)' tests/test_results/bench.jsonl
```

Use `BENCH_DROP_CACHES=1` for cold runs (requires root or passwordless `sudo -n`). See [Testing](Testing.md).

## SHA-1 backends

Piece hashing in the pthread worker pool can use one of:

| Backend | When | Notes |
|---------|------|--------|
| **OpenSSL** | `--sha1=auto` (default) when OpenSSL is linked; or `--sha1=openssl` | `SHA1()` from libcrypto. |
| **ISA-L Crypto** (`libisal_crypto`) | Build linked with `USE_ISAL` and `--sha1=isal` | Multi-buffer `sha1_mb`: workers wait for a full wave (up to 16 pieces), then one `HASH_ENTIRE` submit/flush. Same digests as OpenSSL. Hash workers capped at **1–2**. |
| **Bundled** `sha1.c` | `MKTORRENT_OPENSSL=OFF` | Used when OpenSSL is not linked; `--sha1=openssl` selects this in that build. |

Info-dict hashing (tiny) stays on OpenSSL/bundled. Non-pthread builds do not use ISA-L.

Optional dependency: install **isa-l_crypto** (not the storage-only `libisal`) and leave `MKTORRENT_ISAL` unset so CMake auto-enables it on Linux when `pkg-config` finds `libisal_crypto`. Force with `-DMKTORRENT_ISAL=ON` / `OFF`.

`-V` / `--version` and verbose mode print the active piece backend (`sha1=isal|openssl|bundled`). Verbose also notes when ISA-L caps `-t` to 1–2 workers.

### CPU-bound tmpfs protocol

To compare backends without disk noise:

```bash
# Build with ISA-L, then:
BENCH_SHA1_TMPFS=1 BENCH_TMPFS_MB=2048 PERFORMANCE_TESTS=1 \
  cmake --build build --target test
# or drive bench_hash.sh after sourcing the test framework
```

Recipe: random file on **tmpfs** (`/dev/shm` or a dedicated tmpfs mount; prefer **≥2 GiB** when RAM allows; warm page-cache on disk is a fallback), piece length **1 MiB** (`-l 20`), same binary with `--sha1=openssl -t 1` vs `--sha1=isal` (and openssl `-t $(nproc)`). Keep ISA-L as `--sha1=auto` only when median MiB/s gain is **≥15%** at `-t 1` or at the openssl full-thread setting.

On a warm **2 GiB** / 1 MiB-piece host sample after multi-buffer batching, ISA-L was still behind OpenSSL (~1060 vs ~1400 MiB/s at `-t 1`; openssl `-t $(nproc)` ~1890). A standalone batch-16 microbench on the same CPU was ~1290 vs ~1440 MiB/s OpenSSL — so **`auto` stays on OpenSSL**; use `--sha1=isal` to force the batched path when experimenting on other CPUs.

## SHA-1 throughput

On a warm **200 MiB** buffered run (no io_uring), median scaling from `-t 1` to `-t $(nproc)` was about **1.4×**. OpenSSL builds already outpace the bundled `sha1.c` (about **2×** on the same host). Optional ISA-L multi-buffer is available via `--sha1=isal` but did not meet the ≥15 % keep rule on the measured hosts.

### Versus Rudde/mktorrent 1.1

Same protocol as above (warm **2 GiB**, `-l 20`, OpenSSL piece hashing). After dropping the mmap+memcpy default and aligning the OpenSSL worker/queue with the upstream shape, a laptop sample (5 medians) was about **tied at `-t 1`** and about **+10 % at `-t $(nproc)`** versus a stock Rudde 1.1 build (`USE_OPENSSL` + pthreads). Re-check on your CPU if you care about the margin.

### Zero-copy mmap (`-t > 1`)

Versus the post-Rudde `read()`-into-piece tip on the same laptop protocol (median of 7): about **−4 % at `-t 1`** (still on the read path; within noise) and about **+40 % at `-t 8`**. Always-on mmap views for `-t 1` were slower (~−12 %), so zero-copy stays multi-thread-only. On a 2-vCPU host with a warm **512 MiB** sample, `-t 2` was about **+5 %** (memory-constrained; not the laptop multi-thread margin).

## Many small files

A tree of **5 000 × 4 KiB** files (~20 MiB total) is several times slower to hash than one 20 MiB file, mainly from open/metadata overhead. Prefetching the next file descriptor did not improve warm or cold wall time enough to justify complexity, so hashing stays sequential open/read across the file list.

## What was removed

Linux `libaio` and file-level prefetch worker code were removed and should not be restored. Prefer opt-in **`--io-uring`** for overlapping disk reads with SHA-1 workers when it helps on your hardware.

## Progress output

Progress and status messages go to **stderr**. Use `-q` / `--quiet` or `--json` to suppress them. Stdout is reserved for the JSON summary when `--json` is set.
