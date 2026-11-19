# MKTorrent

MKTorrent is a command-line utility for creating BitTorrent metainfo files on Linux.

## Features

- Multi-threaded hashing (OpenSSL or bundled SHA-1)
- Large files and multi-target / `--from-file` / `--stdin` inputs
- Configurable piece length (exponent or `K`/`M`/`G` sizes)
- Optional announce URLs (DHT-only), web seeds, DHT bootstrap `--node`
- Private flag, source string, comment, cross-seed
- Include/exclude filters (`fnmatch`), `--skip-hidden`, `--no-follow-symlinks`
- Opt-in `--direct-io` and `--io-uring` (liburing) hashing paths
- JSON create summary on stdout (`--json`)
- Linux only

## Quick Start

1. [Install MKTorrent](Installation-Guide.md)
2. Create a torrent:
   ```bash
   mktorrent -a http://tracker.example.com/announce -o output.torrent input_file
   ```

## Documentation

- [Installation Guide](Installation-Guide.md) — build and package install
- [Command Line Options](Command-Line-Options.md) — full CLI reference
- [Hashing and I/O](Hashing-IO.md) — buffered read / zero-copy mmap / direct / io_uring
- [Security](Security.md) — threat model and input limits
- [Testing](Testing.md) — test suite and benches
- [Release Packages](Release-Packages.md) — package variants
- [Release Notes](Release-Notes.md) — changelog

## Support

1. Search [existing issues](https://github.com/tomcdj71/mktorrent/issues)
2. Open a new issue with detailed information

## License

MKTorrent is released under the GNU General Public License v3.0. See the [LICENSE](https://github.com/tomcdj71/mktorrent/blob/master/LICENSE) file for details.
