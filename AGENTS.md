# Ladybird Browser - Development Guide

## Cursor Cloud specific instructions

### Overview

Ladybird is an independent web browser written in C++23 with Rust components (LibJS engine). It uses CMake + Ninja + vcpkg for building, and Qt6 for the Linux UI.

### Build & Run

Standard commands are documented in `Documentation/BuildInstructionsLadybird.md`. Quick reference:

- **Build**: `CC=gcc-14 CXX=g++-14 ./Meta/ladybird.py build`
- **Run**: `CC=gcc-14 CXX=g++-14 ./Meta/ladybird.py run`
- **Test**: `CC=gcc-14 CXX=g++-14 ./Meta/ladybird.py test`
- **Lint**: `Meta/lint-ci.sh` (runs all linters), `cargo fmt --check`, `cargo clippy -- -D clippy::all`

### Network-restricted environment workarounds

The vcpkg build downloads ~70 third-party packages from various sources. In Cursor Cloud, only `github.com` and `gitlab.com` are accessible. Many vcpkg sources (freedesktop.org, sqlite.org, gnu.org, sourceforge.net, googlesource.com, appspot.com) are blocked.

**Pre-downloaded source workaround**: A Python script at `Build/vcpkg/downloads/prefetch_blocked_vcpkg_sources.py` and manual git-archive steps pre-populate the vcpkg downloads cache from GitHub mirrors. Key steps:

1. For `vcpkg_from_git` deps from `googlesource.com`: find a GitHub mirror via `api.github.com/search/commits?q=hash:<commit>`, fetch the commit with `git fetch <mirror_url> <commit> --depth 1 -n`, then create the archive with `git -c core.autocrlf=false archive <commit> -o <downloads_dir>/<port>-<commit>.tar.gz` (no prefix directory).
2. For `vcpkg_from_gitlab` deps from `freedesktop.org`: find GitHub mirrors and download matching tarballs.
3. For `vcpkg_download_distfile` deps from SourceForge/sqlite.org/gnu.org: search GitHub for exact-byte mirrors (e.g., `discord/lilliput-dep-source` for giflib, `glaucuslinux/core` for libpng-apng patch).

**Overlay ports**: Three packages cannot be downloaded from accessible sources. Overlay ports in `/tmp/vcpkg-overlay-ports/` provide workarounds:
- `vcpkg-tool-gn`: uses system `gn` binary (install via `apt install generate-ninja`)
- `vcpkg-make`: uses system automake files from `/usr/share/automake-1.16/`
- `sqlite3`: uses system `libsqlite3-dev`

Set `VCPKG_OVERLAY_PORTS=/tmp/vcpkg-overlay-ports` when building.

### Compiler requirements

- Minimum: clang 19+ or gcc 14+ (see `Meta/find_compiler.py`)
- Rust: edition 2024 required, so Rust 1.85+ is needed (`rustup update stable`)
- CMake 3.30+ required (install via `pip3 install cmake` if system version is too old; ensure `~/.local/bin` is in PATH)

### Running the browser in headless environments

Ladybird requires an X11 display. Set `DISPLAY=:1`, `QT_QPA_PLATFORM=xcb`, and `XDG_RUNTIME_DIR=/tmp/runtime-ubuntu`. Vulkan warnings (`vkCreateInstance returned -9`) are expected in environments without GPU support and do not affect functionality.

### Test notes

- `TestDNSResolver` fails in network-restricted environments (cannot create TLS sockets). This is expected.
- Tests run via `ctest` with the `Release` preset. Pattern filtering: `./Meta/ladybird.py test <regex_pattern>`.
