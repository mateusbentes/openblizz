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

Recommended toolchain: GCC 11+ or Clang 14+, CMake 3.20+,
`pkg-config`/`pkgconf`. GCC 10 may also build this source on distributions that
provide sufficient C++20 support; Debian 11 additionally needs a newer CMake.
The build has no network access requirements (no FetchContent, no submodules).

The distribution sections are dependency/source-build recipes, not a claim
that every listed distribution, release or CPU was tested. Package availability
and names may change. The release workflow builds on Ubuntu 24.04 for x86_64
and AArch64; validate a recipe on your target before packaging or promising
support. Minimum dependency versions below are guidance, not CMake-enforced
version checks for each library.

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
ctest --test-dir build --output-on-failure   # synthetic + Linux loopback tests; no external network
./build/openblizz --help
```

CMake options:

| Option | Default | Meaning |
|---|---|---|
| `CMAKE_BUILD_TYPE` | (empty) | `Release` for an optimised binary, `Debug` for symbols |
| `OPENBLIZZ_BUILD_TESTS` | `ON` | register core and third-party synthetic tests; Linux also registers libcurl HTTP Range tests against a temporary loopback server. No external network or credentials are needed |
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
   # Install Distrobox from Discover (Flatpak), or use an existing Distrobox.
   # Do not assume host pacman works while the SteamOS root is read-only.
   distrobox create --name ob --image archlinux:latest
   distrobox enter ob
   sudo pacman -Syu --needed git cmake gcc pkgconf curl openssl zlib lz4 nlohmann-json
   git clone https://github.com/mateusbentes/openblizz.git && cd openblizz
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"$(nproc)"
   ```
   Run `openblizz` from inside the container (`distrobox enter ob -- ~/openblizz/build/openblizz ...`)
   or export it to the host with `distrobox-export --bin ~/openblizz/build/openblizz`.
   The game directory and `~/.config/openblizz` are shared with the host, so
   `launch` and adding the game to Steam work normally. When no `--prefix` is
   supplied, `launch` uses `./.openblizz-prefix` relative to its current
   working directory, which is not necessarily `$HOME`; pass an explicit
   prefix when you need a stable location.
2. **Copy a binary built on Arch Linux**: SteamOS is Arch-based, but this is
   an ABI-dependent convenience rather than a guarantee. Prefer the container
   build or test the copied binary on the target SteamOS image.

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

This is a source-build recipe, not a tested musl support guarantee. The
published Linux binaries target glibc and are not Alpine-native. Fully static
linking also needs static versions of every transitive dependency and has
not been validated by the release workflow.

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

These are starting instructions for an unvalidated port, not a claim that
the complete client works on macOS. Browser automation and runner code use
POSIX/Linux assumptions; only Linux is built by the release workflow.
Proton is Linux-only, and selecting `native` does not make the client portable.

## Windows (experimental)

Use MSYS2 (UCRT64) for an experimental **build-only** attempt:

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,curl,openssl,zlib,lz4,nlohmann-json}
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The current browser-login and runner implementations are POSIX-oriented, so
this is not a supported end-to-end Windows path. `library add` changes only
local metadata; it is not a replacement for the valid account session required
by `install`, `update` and `repair`. Use the Linux browser-login flow for the
supported path. Do not copy a live cookie jar casually; if you move one to
another machine, preserve its `0600` permissions and protect it like a
credential. `launch --backend native` is only a direct process hand-off and
does not make a Windows build generally supported.

## Verifying the build

```bash
ctest --test-dir build --output-on-failure      # 4 suites on Linux: core, third-party pipeline, loopback HTTP Range, file safety
./build/openblizz products                      # offline: curated catalog
./build/openblizz versions w3                   # network: Ribbit
./build/openblizz plan w3 --no-data             # network: manifests; no game-directory data written (cache may warm)
```

## Development build

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-debug -j"$(nproc)"
./build-debug/openblizz_tests
./build-debug/openblizz_thirdparty_tests
./build-debug/openblizz_http_tests                # Linux only: loopback libcurl Range tests
./build-debug/openblizz_file_safety_tests         # Linux only: symlink/no-follow/path regressions
ctest --test-dir build-debug --output-on-failure
```

Warnings are enabled (`-Wall -Wextra -Wpedantic`) for `openblizz_core`; the
CLI and test targets do not currently have those flags applied directly.
Address/UB sanitizers:

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
- By default, the binary writes to the XDG locations listed in
  [FILES.md](FILES.md), to the game directory passed with `--directory`, and
  to `./.openblizz-prefix` when `launch` is used without `--prefix`. Explicit
  `--cookie-jar`, `--library-file`, `--dump`, `--profile-dir` and `--prefix`
  options may direct output elsewhere. `library scan --dump` writes raw account
  JSON responses; treat the dump as sensitive account/purchase data and redact
  it before sharing.
