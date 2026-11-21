# Release Packages

Each GitHub release publishes Debian packages for common OpenSSL / pthreads combinations.

## Package Types

### Debian Packages (.deb)

Naming pattern:

```
mktorrent_<version>_openssl-<ON|OFF>_pthreads-<ON|OFF>-ubuntu-latest_amd64.deb
```

Example for version `1.5.0`:

```
mktorrent_1.5.0_openssl-ON_pthreads-ON-ubuntu-latest_amd64.deb
mktorrent_1.5.0_openssl-ON_pthreads-OFF-ubuntu-latest_amd64.deb
mktorrent_1.5.0_openssl-OFF_pthreads-ON-ubuntu-latest_amd64.deb
mktorrent_1.5.0_openssl-OFF_pthreads-OFF-ubuntu-latest_amd64.deb
```

See [releases](https://github.com/tomcdj71/mktorrent/releases) for the exact current filenames.

### Package Contents

Each `.deb` typically contains:

- `/usr/bin/mktorrent` — main executable
- man page and package metadata
- Shared-library `Depends` derived at package time (`dpkg-shlibdeps`), so OpenSSL / liburing / `libisal_crypto` appear when the binary is linked against them

## Build Configuration

Common CMake options used in CI package builds:

```cmake
-DMKTORRENT_LONG_OPTIONS=ON
-DCMAKE_BUILD_TYPE=RelWithDebInfo
```

Variant matrix:

- `-DMKTORRENT_OPENSSL=ON|OFF`
- `-DMKTORRENT_PTHREADS=ON|OFF`

On Linux, when `liburing` is present and `MKTORRENT_IO_URING` is left unset, CMake auto-enables io_uring for the opt-in `--io-uring` flag. CI’s openssl+pthreads matrix installs `liburing-dev` for that path.

When `libisal_crypto` is present and `MKTORRENT_ISAL` is left unset, CMake auto-enables optional ISA-L Crypto piece SHA-1. Default CI cells force `MKTORRENT_ISAL=OFF`; one openssl+pthreads cell builds isa-l_crypto from source and asserts `USE_ISAL`.

## Build Variants

### Full featured

- OpenSSL: ON, POSIX threads: ON, long options: ON
- Best for general use

### Minimal

- OpenSSL: OFF, POSIX threads: OFF
- Smallest dependency footprint (bundled SHA-1, single-threaded)

### Other combinations

OpenSSL ON / pthreads OFF, and OpenSSL OFF / pthreads ON, cover systems that need one without the other.

## Verification

After install:

```bash
mktorrent -V
mktorrent -h
```
