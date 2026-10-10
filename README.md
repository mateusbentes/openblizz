# OpenBlizz

OpenBlizz is an independent, clean-room command-line client for Linux that
downloads, installs, updates, verifies, repairs and launches (through
Proton/umu or Wine) the Blizzard games you own — without the Battle.net
desktop app. It speaks the public NGDP/TACT/CASC protocols directly, logs you
in through your own browser on the official Battle.net page, and reads your
library from your own account page.

Warcraft III: Reforged is the reference product (full CASC data install);
the catalog also covers Warcraft I/II Remastered, StarCraft: Remastered,
StarCraft II, Diablo (Immortal, II: Resurrected, III, IV), World of Warcraft,
Blizzard Arcade Collection, Hearthstone, Heroes of the Storm, Overwatch and
known third-party Battle.net metadata such as The Witcher 3: Wild Hunt
Remastered (`lyra`). Any other NGDP product attached to your account is
recognised as well.

OpenBlizz is not affiliated with or endorsed by Blizzard Entertainment. It
does not distribute Battle.net, Agent.exe, game files, private keys or any
proprietary asset, and it never asks for or stores your password.

## Install the latest Linux binary

For Linux x86_64 and AArch64, the repository includes a user-local installer.
It detects the CPU architecture, downloads the matching binary and its
`SHA256SUMS` file from the latest GitHub Release, verifies the checksum, and
atomically installs the result as `~/.local/bin/openblizz`.
It never uses `sudo`, changes system directories, or modifies shell profiles:

```bash
# Recommended: download, review and run the script locally
curl --fail --silent --show-error --location \
  https://raw.githubusercontent.com/mateusbentes/openblizz/main/scripts/install-openblizz.sh \
  --output /tmp/install-openblizz.sh
bash /tmp/install-openblizz.sh
```

To execute it directly without saving the script:

```bash
curl -fsSL \
  https://raw.githubusercontent.com/mateusbentes/openblizz/main/scripts/install-openblizz.sh \
  | bash
```

The script also accepts `--version vX.Y.Z` to install a pinned release. When a
new `v*` tag is pushed,
the Linux GitHub Actions workflow publishes `openblizz-linux-x86_64`,
`openblizz-linux-aarch64` and `SHA256SUMS`; if the repository has no Release
yet, the first such tag creates it.
If `~/.local/bin` is not in `PATH` on your distribution, the script prints the
one-line export to add to your shell profile; it does not edit that file.

## Quick start

```bash
# 1. Get the openblizz binary (prebuilt, no sudo; see above) ...
curl -fsSL https://raw.githubusercontent.com/mateusbentes/openblizz/main/scripts/install-openblizz.sh | bash
#    ... or build from source (Debian/Ubuntu shown; every distribution in docs/BUILDING.md)
#    sudo apt install git cmake g++ pkg-config libcurl4-openssl-dev libssl-dev zlib1g-dev liblz4-dev nlohmann-json3-dev
#    git clone https://github.com/mateusbentes/openblizz.git && cd openblizz
#    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"$(nproc)"
#    sudo cmake --install build      # optional: puts openblizz in /usr/local/bin
#    The steps below assume `openblizz` is in PATH (prebuilt installer or
#    cmake --install). After a source build without installing, replace
#    `openblizz` with `./build/openblizz` (run from the repository directory).

# 2. Install umu-launcher, which runs the games through Proton (GE-Proton is
#    downloaded automatically on first launch). Package names per distribution
#    are in docs/RUNTIME.md; e.g. Arch: sudo pacman -S umu-launcher,
#    Fedora: sudo dnf install umu-launcher, others: zipapp/.deb/.rpm from
#    https://github.com/Open-Wine-Components/umu-launcher/releases
umu-run --version

# 3. Log in once (opens an isolated window of your default browser; OpenBlizz
#    never sees your password). The session is kept and scanned automatically.
openblizz login

# 4. See what you own, grouped by franchise
openblizz library list

# 5. Install (Warcraft III: Reforged, ~35 GB for enUS; ptBR adds that language)
openblizz install w3 --directory ~/Games/Warcraft3 --locale ptBR

# 6. Play. -launch skips the hand-off to the Battle.net app; log in on the
#    game's own login screen. First start is slow (prefix + GE-Proton download).
openblizz launch --directory ~/Games/Warcraft3/x86_64 --exe "Warcraft III.exe" \
                 --prefix ~/Games/openblizz/warcraft3 -- -launch
```

Later: `openblizz update w3 --directory ~/Games/Warcraft3` to patch,
`verify`/`repair` to check the files, and [docs/RUNTIME.md](docs/RUNTIME.md#optional-adding-a-game-to-steam-steam-deck--big-picture)
to add the game to Steam as a non-Steam game. Without umu, `launch --backend wine`
uses the system Wine instead.

On ARM64, `openblizz-linux-aarch64` is the native OpenBlizz client. Running
the downloaded Windows x86/x86-64 game still requires an ARM64-capable
Proton+FEX/umu stack installed separately; OpenBlizz does not bundle Proton,
FEX, Steam Runtime or an x86-64 rootfs. See
[docs/RUNTIME.md](docs/RUNTIME.md#linux-aarch64-openblizz-versus-protonfex)
for the distinction and the explicit `--proton` example.

## Documentation

| Document | Content |
|---|---|
| [docs/BUILDING.md](docs/BUILDING.md) | dependencies and build commands for Debian/Ubuntu, Fedora/RHEL, Arch, SteamOS, openSUSE, Alpine, Void, Gentoo, Nix, Solus, macOS and Windows; install, dev and sanitizer builds; packaging notes |
| [docs/SCOPE.md](docs/SCOPE.md) | what is installable and what is only listed (Battle.net exclusives, mobile-origin PC builds, Call of Duty, third-party shop titles, classic CD-key games) and where to play the rest; the umu/Proton/Wine runtime stack |
| [docs/COMMANDS.md](docs/COMMANDS.md) | every command and option: `products`, `versions`, `cdns`, `plan`, `vfs`, `login`, `logout`, `library`, `install`, `update`, `verify`, `repair`, `launch`; exit codes; typical workflow |
| [docs/RUNTIME.md](docs/RUNTIME.md) | running games: installing umu per distribution, choosing a Proton build, Warcraft III specifics, adding the game to Steam / Steam Deck, `.desktop` launcher, performance variables |
| [docs/FILES.md](docs/FILES.md) | where everything lives (cookie jar, browser profile, library.json, cache), the game directory layout, environment variables, network endpoints contacted |
| [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) | build, login, library, download and launch problems with their fixes |
| [docs/IMPLEMENTATION.md](docs/IMPLEMENTATION.md) | architecture: protocol, format, content, account and runner layers; how ownership is derived |
| [docs/RELEASING.md](docs/RELEASING.md) | maintainer instructions for tags, GitHub Actions, assets and the prebuilt installer |
| [SOURCES.md](SOURCES.md) | public specifications and references used for the clean-room implementation |
| [CONTRIBUTING.md](CONTRIBUTING.md), [TRADEMARKS.md](TRADEMARKS.md), [NOTICE](NOTICE), [LICENSE](LICENSE) | contribution rules, trademark policy, legal notices (Apache-2.0) |

## How it works

**Catalog and content.** Builds and CDN hosts come from Ribbit
(`{region}.version.battle.net`); configs, encoding, install/download manifests,
archive indexes and TVFS manifests from the public Blizzard CDNs. For
Warcraft III: Reforged the install manifest contains ordinary game files,
including executables, DLLs and World Editor; the game data lives in a local
CASC storage, so `install` mounts the TVFS
(`vfs-root` → `war3.w3mod` → nested `vfs-N`), downloads every referenced
object through CDN archives with HTTP Range requests and writes
`Data/data/*.idx` + `Data/data/data.NNN`, `Data/config`, `Data/indices` and
`.build.info` — the same layout the official installer produces. Downloads
resume, `verify --deep` re-hashes every stored CASC object, and `repair`
re-downloads install files that fail their hash check plus CASC objects that
are missing or have a size mismatch.

**Login.** `openblizz login` opens an **isolated** profile of your default
browser (Firefox family via WebDriver BiDi, Chromium family via DevTools;
native, snap or flatpak) on the official login page. You enter password, MFA
and captcha there; OpenBlizz only reads the resulting `battle.net` session
cookies, verifies them against `account.battle.net/api/`, stores them with
owner-only permissions and closes the window. Account-dependent commands such
as `library scan`, `library list`, `install`, `update` and `repair` reuse and
renew that session; metadata, verification, launch and local library-editing
commands do not require it. `logout` deletes it. The Battle.net app, its Agent
and the public/developer OAuth API are not used by OpenBlizz.

**Ownership.** `library scan` (run automatically after login and every six
hours) reads the same internal JSON the account page uses:
`games-and-subs` (game accounts; `titleId` is the FourCC of the NGDP code),
`classic-games` (CD keys) and `transactions` (purchase history, including
purchase-history entitlements such as Warcraft I/II Remastered and the
Blizzard Arcade Collection). `install` refuses products marked `not owned` unless `--force`
is given. These endpoints are not a documented API and may change; OpenBlizz
labels their output accordingly rather than pretending they are stable.

**Output.** Lists are grouped by franchise (Warcraft, StarCraft, Diablo,
Blizzard Arcade, Hearthstone, Heroes of the Storm, Overwatch, Other) in aligned
tables. `products` shows the curated catalog, `products --all` every NGDP code,
`products --shop` the storefront entries exposed by the current navigation and
family pages (including Call of Duty and third-party titles, whose installability depends on the current NGDP build — see
[docs/COMMANDS.md](docs/COMMANDS.md#openblizz-products)), `library list` what you own
with the evidence for each entry.

## Supported products

| Id | Game | Install | Notes |
|---|---|---|---|
| `w3` | Warcraft III: Reforged | full (executables + CASC data) | reference product; `-launch` required |
| `w3-legacy-tft` | Warcraft III legacy / TFT | executables | launch verified with GE-Proton10-10 + umu; no `-launch` required |
| `w2r`, `w1r` | Warcraft II / I Remastered | yes | ownership via purchase history; installable NGDP products |
| `w2bn`, `war1` | Warcraft II BNE, Warcraft: Orcs & Humans | yes | classic CD keys |
| `s1`, `s2` | StarCraft: Remastered, StarCraft II | yes | |
| `anbs`, `osi`, `d3`, `fenris` | Diablo Immortal (PC build), II: Resurrected, III, IV | yes | `anbs`: Windows build only, not the phone game |
| `wow`, `wow_classic`, `gryphon` | World of Warcraft, Classic, Warcraft Rumble (PC build) | yes | `gryphon`: Windows build only |
| `rtro` | Blizzard Arcade Collection | yes | ownership via purchase history; installable NGDP product |
| `hsb`, `hero`, `pro` | Hearthstone (PC build), Heroes of the Storm, Overwatch | yes | |
| `lyra` | The Witcher 3: Wild Hunt Remastered | metadata-only currently | purchase mapping and public NGDP inspection; current build has an empty install manifest |
| `d2-classic`, `d2-lod` | Diablo II, Lord of Destruction | no (legacy installer) | ownership tracked only |

"Install" means OpenBlizz can download the build through NGDP; whether a
title runs well under Proton depends on the game (anti-cheat, launcher
requirements). The verified launch path is currently Warcraft III: Legacy/TFT
with GE-Proton10-10 and umu. Warcraft III: Reforged installation is tested,
but its in-game launch remains sensitive to hardware, GPU drivers, display
server and Proton version. Mobile-origin games (Diablo Immortal, Warcraft
Rumble, Hearthstone) are installed as their Windows builds; phone builds are
not on NGDP. The full category-by-category
breakdown, including Call of Duty and third-party shop titles, is in
[docs/SCOPE.md](docs/SCOPE.md).

## Limitations (honest list)

- The prebuilt installer currently publishes Linux x86_64/glibc and AArch64/
  glibc binaries; other architectures should build from source until matching
  Release assets are added.
- Ownership comes from undocumented account-page endpoints; a Blizzard change
  can break `library scan` until the parser is updated (installing with
  `--force` keeps working).
- Call of Duty titles are on NGDP (`versions`/`cdns` work), but their complete
  content/key/runtime combination is not validated and they require the
  Battle.net client and anti-cheat. They remain metadata-only. The current
  third-party `lyra` build is also metadata-only; `plan lyra` rejects its empty
  install manifest instead of claiming success.
- BLTE `E` chunks are now decoded when the referenced public KeyRing entry is
  available (Salsa20 and ARC4). Missing keys, incomplete manifests and
  unsupported product runtimes still fail explicitly; OpenBlizz does not ship
  private keys or proprietary game assets.
- Only the Windows x86_64 build of each product is selected.
- No GUI, no game-side patching (umu/Proton fixes apply as usual).

## License and trademarks

The OpenBlizz source is licensed under Apache-2.0. OpenBlizz is an independent
project and is not affiliated with or endorsed by Blizzard Entertainment.
Warcraft, StarCraft, Diablo, Overwatch, Hearthstone, Battle.net and other
names are trademarks of their respective owners. See [NOTICE](NOTICE) and
[TRADEMARKS.md](TRADEMARKS.md).
