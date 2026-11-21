# Installation Guide

This guide provides detailed instructions for installing MKTorrent on your system.

## Table of Contents
- [Prerequisites](#prerequisites)
- [Quick Installation](#quick-installation)
- [Detailed Installation](#detailed-installation)
  - [Building from Source](#building-from-source)
  - [Package Installation](#package-installation)
- [Configuration Options](#configuration-options)
- [Verification](#verification)
- [Troubleshooting](#troubleshooting)

## Prerequisites

### Required Dependencies
| Dependency    | Minimum Version | Purpose                 |
| ------------- | --------------- | ----------------------- |
| CMake         | 3.20            | Build system            |
| GCC/Clang     | Any recent      | Compiler                |
| POSIX threads | -               | Multi-threading support |

### Optional Dependencies
| Dependency                      | Purpose                                     | Benefit                                      |
| ------------------------------- | ------------------------------------------- | -------------------------------------------- |
| OpenSSL                         | SHA-1 implementation                        | Faster hashing                               |
| liburing                        | `--io-uring` piece reader                   | Cold-cache I/O overlap (Linux)               |
| isa-l_crypto (`libisal_crypto`) | Optional piece SHA-1 via Intel ISA-L Crypto | Higher CPU-bound hash throughput when linked |

> Note: Distro packages named `libisal` / `libisal-dev` are the **storage** library, not Crypto. Build [isa-l_crypto](https://github.com/intel/isa-l_crypto) from source (needs `nasm`) and point `PKG_CONFIG_PATH` at its `libisal_crypto.pc` when no distro package exists.

### Installing Dependencies

**Ubuntu/Debian:**
```bash
# Required dependencies
sudo apt-get update
sudo apt-get install build-essential cmake

# Optional but recommended
sudo apt-get install libssl-dev liburing-dev
# Optional ISA-L Crypto: build/install isa-l_crypto, then ensure pkg-config finds libisal_crypto
```

## Quick Installation

### Direct Package Download

Prefer assets from the [latest GitHub release](https://github.com/tomcdj71/mktorrent/releases/latest). Deb names look like:

`mktorrent_<version>_openssl-ON_pthreads-ON-ubuntu-latest_amd64.deb`

(and the other OpenSSL/pthreads ON/OFF combinations). After downloading:

```bash
sudo dpkg -i mktorrent_*.deb
```

### From Source (Quick)
```bash
git clone https://github.com/tomcdj71/mktorrent.git
cd mktorrent
cmake -B build
cmake --build build
sudo cmake --install build
```

## Detailed Installation

### Building from Source

1. **Clone the Repository**
   ```bash
   git clone https://github.com/tomcdj71/mktorrent.git
   cd mktorrent
   ```

2. **Configure the Build**
   
   Basic configuration:
   ```bash
   cmake -B build
   ```

   Advanced configuration:
   ```bash
   cmake -B build \
     -DCMAKE_BUILD_TYPE=Release \
     -DMKTORRENT_OPENSSL=ON \
     -DMKTORRENT_PTHREADS=ON \
     -DMKTORRENT_LONG_OPTIONS=ON
   ```

   On Linux with `liburing-dev` installed, io_uring support turns on automatically unless you pass `-DMKTORRENT_IO_URING=OFF`. Force it with `-DMKTORRENT_IO_URING=ON`.

   On Linux with pthreads and `libisal_crypto` visible to pkg-config, ISA-L piece SHA-1 turns on automatically unless you pass `-DMKTORRENT_ISAL=OFF`. Force it with `-DMKTORRENT_ISAL=ON`.

3. **Build the Project**
   ```bash
   cmake --build build
   ```

4. **Install (Optional)**
   ```bash
   sudo cmake --install build
   ```

### Package Installation

#### Ubuntu/Debian
1. **Download Package**
   - From [releases page](https://github.com/tomcdj71/mktorrent/releases), or
   - Using wget:
     ```bash
     # Pick VERSION from the releases page (example: 1.5.0)
     VERSION="1.5.0"
     OPENSSL="ON"
     PTHREADS="ON"
     wget "https://github.com/tomcdj71/mktorrent/releases/download/v${VERSION}/mktorrent_${VERSION}_openssl-${OPENSSL}_pthreads-${PTHREADS}-ubuntu-latest_amd64.deb"
     ```

2. **Install Package**
   ```bash
   sudo dpkg -i mktorrent_*_amd64.deb
   ```

## Configuration Options

### CMake Build Options

| Option                         | Description                                                                                                              | Default               |
| ------------------------------ | ------------------------------------------------------------------------------------------------------------------------ | --------------------- |
| `MKTORRENT_OPENSSL`            | Use OpenSSL's SHA-1 implementation                                                                                       | ON                    |
| `MKTORRENT_PTHREADS`           | Enable POSIX threads for faster hashing                                                                                  | ON                    |
| `MKTORRENT_LONG_OPTIONS`       | Enable GNU-style long options                                                                                            | ON                    |
| `MKTORRENT_IO_URING`           | Opt-in `--io-uring` reader (needs liburing + pthreads). If unset, auto-ON on Linux when liburing is found                | OFF (auto when unset) |
| `MKTORRENT_ISAL`               | Optional ISA-L Crypto SHA-1 for piece workers (needs `libisal_crypto` + pthreads). If unset, auto-ON on Linux when found | OFF (auto when unset) |
| `MKTORRENT_NO_HASH_CHECK`      | Disable hash-size consistency check                                                                                      | OFF                   |
| `MKTORRENT_USE_GITREV_VERSION` | Include git commit in version string                                                                                     | OFF                   |
| `MKTORRENT_TEST`               | Enable test suite                                                                                                        | OFF                   |
| `MKTORRENT_TEST_PERFORMANCE`   | Enable performance tests                                                                                                 | OFF                   |
| `CMAKE_BUILD_TYPE`             | Build type (Debug/Release/RelWithDebInfo)                                                                                | —                     |

### Example Configurations

**Maximum Performance:**
```bash
cmake -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DMKTORRENT_OPENSSL=ON \
  -DMKTORRENT_PTHREADS=ON
```

**Minimal Build:**
```bash
cmake -B build \
  -DMKTORRENT_OPENSSL=OFF \
  -DMKTORRENT_PTHREADS=OFF \
  -DMKTORRENT_LONG_OPTIONS=OFF
```

## Verification

After installation, verify that mktorrent is working correctly:

```bash
# Check version
mktorrent --version

# Check help
mktorrent --help

# Create a test torrent
mktorrent -a http://example.com/announce -o test.torrent test_file
```

## Troubleshooting

### Common Issues and Solutions

#### 1. CMake Configuration Fails
```
Problem: CMake command not found
Solution: sudo apt-get install cmake
```

#### 2. Build Fails with OpenSSL Errors
```
Problem: OpenSSL development files missing
Solution: sudo apt-get install libssl-dev
```

#### 3. Threading Support Issues
```
Problem: POSIX threads not found
Solution: sudo apt-get install libc6-dev
```

### Getting Help

If you encounter problems:

1. **Check Prerequisites**
   - Verify all required dependencies are installed
   - Ensure minimum versions are met

2. **Gather Information**
   - System information
   - Complete error messages
   - Build logs

3. **Seek Help**
   1. Read [Home](Home.md), [Security](Security.md), and [Hashing-IO](Hashing-IO.md)
   2. Search [existing issues](https://github.com/tomcdj71/mktorrent/issues)
   3. Open a new issue with:
      - Detailed system information
      - Complete error messages
      - Steps to reproduce
      - Build configuration used

