# Building OpenBlizz

OpenBlizz is a single C++20 program built with CMake. It links against four
system libraries and one header-only library:

| Dependency | Used for | Minimum |
|---|---|---|
| libcurl (with TLS) | HTTPS to Ribbit/NGDP, Blizzard CDNs, account pages | 7.68 |
| OpenSSL (libcrypto) | MD5 / SHA-256 / Jenkins hashing of TACT objects | 1.1 |
| zlib | BLTE `Z` chunks | 1.2 |
| LZ4 | BLTE `4` chunks | 1.9 |
| nlohmann/json (header-only) | account JSON, library file, DevTools/BiDi messages | 3.9 |

Toolchain: GCC 11+ or Clang 14+, CMake 3.20+, `pkg-config`/`pkgconf`. The
build has no network access requirements (no FetchContent, no submodules).

Everything below assumes you cloned the repository:

```bash
git clone https://github.com/mateusbentes/openblizz.git
cd openblizz
```

## Install a prebuilt Linux binary

If you only want to use OpenBlizz on Linux x86_64 or AArch64, you do not need a
compiler or a container. The installer detects `uname -m`, downloads the
matching `openblizz-linux-x86_64` or `openblizz-linux-aarch64` asset and the
matching `SHA256SUMS` file from the latest GitHub Release, verifies it, and
installs it atomically at `~/.local/bin/openblizz`:

```bash
curl --fail --silent --show-error --location \
  https://raw.githubusercontent.com/mateusbentes/openblizz/main/scripts/install-openblizz.sh \
  --output /tmp/install-openblizz.sh
bash /tmp/install-openblizz.sh
```

The script requires Bash, `curl` (or `wget`), `sha256sum`, `mktemp` and the
standard `install`/`mv` utilities. It never uses `sudo`, does not edit
`.bashrc`/`.zshrc`, and prints an explicit `PATH` line if `~/.local/bin` is
not already available. To pin a release:

```bash
bash /tmp/install-openblizz.sh --version v0.1.0
```

If the repository has no prebuilt asset yet, a maintainer must push the first
`v*` tag; see [RELEASING.md](RELEASING.md). Other CPU architectures should use
the source build below until matching release assets are published. The Linux
Release workflow builds x86_64 on `ubuntu-24.04` and AArch64 on
`ubuntu-24.04-arm`.

## Generic build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure   # unit tests, ~0.1 s, no network
./build/openblizz --help
```

CMake options:

| Option | Default | Meaning |
|---|---|---|
| `CMAKE_BUILD_TYPE` | (empty) | `Release` for an optimised binary, `Debug` for symbols |
| `OPENBLIZZ_BUILD_TESTS` | `ON` | build and register `openblizz_tests` with CTest |
| `CMAKE_INSTALL_PREFIX` | `/usr/local` | where `cmake --install build` puts `bin/openblizz` |
| `CMAKE_CXX_COMPILER` | system default | e.g. `clang++` |

System-wide install (optional):

```bash
sudo cmake --install build            # -> /usr/local/bin/openblizz
# or without root:
cmake --install build --prefix "$HOME/.local"   # -> ~/.local/bin/openblizz
```

The binary has no data files; it is fully relocatable.

## Dependencies per distribution

### Debian 12+, Ubuntu 22.04+, Linux Mint, Pop!_OS, elementary, Zorin

```bash
sudo apt update
sudo apt install -y git cmake g++ pkg-config \
  libcurl4-openssl-dev libssl-dev zlib1g-dev liblz4-dev nlohmann-json3-dev
```

Ubuntu 22.04 ships GCC 11 and CMake 3.22, both sufficient. Debian 11
(bullseye) ships GCC 10 and CMake 3.18: install `cmake` from
`bullseye-backports` (3.25) and `g++-10` works for this code base.

### Fedora 39+, RHEL 9 / Rocky / AlmaLinux (with EPEL), CentOS Stream

```bash
sudo dnf install -y git cmake gcc-c++ pkgconf-pkg-config \
  libcurl-devel openssl-devel zlib-devel lz4-devel json-devel
```

`json-devel` is Fedora's package name for nlohmann/json. On RHEL 9 enable EPEL
first (`sudo dnf install epel-release`) and CRB (`sudo dnf config-manager
--set-enabled crb`) for `json-devel`.

### Arch Linux, Manjaro, EndeavourOS, CachyOS, Garuda

```bash
sudo pacman -S --needed git cmake gcc pkgconf curl openssl zlib lz4 nlohmann-json
```

### SteamOS 3 (Steam Deck)

The root filesystem is read-only and `pacman` is not usable without disabling
it. Two supported routes:

1. **Build inside a container** (recommended, survives SteamOS updates):
   ```bash
   # Desktop mode, Konsole
   sudo pacman -S distrobox   # or install Distrobox from Discover (Flatpak)
   distrobox create --name ob --image archlinux:latest
   distrobox enter ob
   sudo pacman -Syu --needed git cmake gcc pkgconf curl openssl zlib lz4 nlohmann-json
   git clone https://github.com/mateusbentes/openblizz.git && cd openblizz
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"$(nproc)"
   ```
   Run `openblizz` from inside the container (`distrobox enter ob -- ~/openblizz/build/openblizz ...`)
   or export it to the host with `distrobox-export --bin ~/openblizz/build/openblizz`.
   The game directory, Proton prefix and `~/.config/openblizz` live in your
   home directory and are shared with the host, so `launch` and adding the game
   to Steam work normally.
2. **Copy a binary built on Arch Linux**: SteamOS is Arch-based and ships
   libcurl, OpenSSL 3, zlib and lz4 in `/usr/lib`, so a binary built on an
   up-to-date Arch machine runs unchanged.

### openSUSE Tumbleweed / Leap 15.5+

```bash
sudo zypper install -y git cmake gcc-c++ pkg-config \
  libcurl-devel libopenssl-devel zlib-devel liblz4-devel nlohmann_json-devel
```

Leap 15.5 defaults to GCC 7; add `gcc12-c++` and configure with
`-DCMAKE_CXX_COMPILER=g++-12`.

### Alpine Linux 3.18+ (musl)

```bash
sudo apk add git cmake g++ pkgconf curl-dev openssl-dev zlib-dev lz4-dev nlohmann-json
```

musl is fully supported; the binary is static-friendly if you add
`-DCMAKE_EXE_LINKER_FLAGS=-static` and the `*-static` dev packages.

### Void Linux

```bash
sudo xbps-install -S git cmake gcc pkg-config \
  libcurl-devel openssl-devel zlib-devel liblz4-devel json-c++
```

### Gentoo

```bash
sudo emerge --ask dev-vcs/git dev-build/cmake virtual/pkgconfig \
  net-misc/curl dev-libs/openssl sys-libs/zlib app-arch/lz4 dev-cpp/nlohmann_json
```

### NixOS / Nix

```bash
nix-shell -p git cmake gcc pkg-config curl openssl zlib lz4 nlohmann_json
# then the generic build commands
```

A `flake.nix` is not shipped yet; contributions welcome.

### Homebrew on Linux (including AArch64)

Homebrew does support Linux ARM64/AArch64. Its current support-tier
documentation lists ARM64/AArch64 as a supported Linux architecture; Tier 1
requires a supported Ubuntu release, glibc ≥ 2.39, the default prefix
`/home/linuxbrew/.linuxbrew` (or a compatible short prefix), and available
bottles. Formula-specific bottle availability still varies.

For a local source build of OpenBlizz:

```bash
brew install cmake pkg-config curl openssl@3 zlib lz4 nlohmann-json
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

For a future Homebrew formula, OpenBlizz should be built from its source
tarball with architecture-neutral CMake dependencies (`curl`, `openssl@3`,
`zlib`, `lz4`, `nlohmann-json` and CMake as a build dependency), not by
downloading the GitHub prebuilt binary. Homebrew can then build the formula
on both Intel x86_64 and ARM64 and produce bottles according to its CI and
policy. The AArch64 GitHub Release and a Homebrew formula are complementary:
the Release is a quick binary installer, while Homebrew owns reproducible
formula builds and bottles.

### Solus

```bash
sudo eopkg install -c system.devel
sudo eopkg install curl-devel openssl-devel zlib-devel lz4-devel nlohmann-json
```

### Other distributions

Any distribution that provides CMake ≥ 3.20, a C++20 compiler and development
packages for curl, openssl, zlib, lz4 and nlohmann/json works. If
nlohmann/json is missing from the package manager, drop the single header in
place:

```bash
mkdir -p third_party/nlohmann
curl -L -o third_party/nlohmann/json.hpp \
  https://github.com/nlohmann/json/releases/latest/download/json.hpp
cmake -S . -B build -DNLOHMANN_JSON_INCLUDE_DIR="$PWD/third_party"
```

## macOS (experimental)

```bash
brew install cmake pkg-config curl openssl@3 lz4 nlohmann-json
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)" \
  -DCURL_ROOT="$(brew --prefix curl)"
cmake --build build -j"$(sysctl -n hw.ncpu)"
```

Catalog, plan, install, verify, repair, login (Firefox/Chromium) and library
commands work. `launch` has only `native` and `wine` backends on macOS; Proton
is Linux-only.

## Windows (experimental)

Use MSYS2 (UCRT64):

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-{gcc,cmake,pkgconf,curl,openssl,zlib,lz4,nlohmann-json}
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Browser login relies on POSIX process control and `chmod`; on Windows use
`library add` or run the login step on Linux and copy
`battlenet-cookies.txt`. `launch --backend native` runs the game directly.

## Verifying the build

```bash
ctest --test-dir build --output-on-failure      # parsers, hashes, TVFS, CASC, shop/account JSON
./build/openblizz products                      # offline: curated catalog
./build/openblizz versions w3                   # network: Ribbit
./build/openblizz plan w3 --no-data             # network: CDN config + manifests, nothing written
```

## Development build

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-debug -j"$(nproc)"
./build-debug/openblizz_tests
```

Warnings are enabled (`-Wall -Wextra -Wpedantic`) and the tree must build
without any. Address/UB sanitizers:

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
```

## Packaging notes

- Runtime dependencies: `libcurl`, `libcrypto`, `libz`, `liblz4` (all usually
  already installed). nlohmann/json is header-only (build-time only).
- Optional runtime tools (not linked): a Firefox- or Chromium-family browser
  for `login`; `umu-run` or `wine` for `launch`; `xdg-settings` to detect the
  default browser.
- The binary writes only to the XDG directories listed in
  [FILES.md](FILES.md) and to the game directory you pass with `--directory`.
