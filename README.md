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
Blizzard Arcade Collection, Hearthstone, Heroes of the Storm and Overwatch.
Any other NGDP product attached to your account is recognised as well.

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
# 1. Build (Debian/Ubuntu shown; every distribution in docs/BUILDING.md)
sudo apt install cmake g++ pkg-config libcurl4-openssl-dev libssl-dev zlib1g-dev liblz4-dev nlohmann-json3-dev
git clone https://github.com/mateusbentes/openblizz.git && cd openblizz
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"$(nproc)"
sudo cmake --install build          # optional: /usr/local/bin/openblizz

# 2. Log in once (opens an isolated window of your default browser)
openblizz login

# 3. See what you own, grouped by franchise
openblizz library list

# 4. Install and play (W3 Reforged, ~35 GB for enUS)
openblizz install w3 --directory ~/Games/Warcraft3 --locale ptBR
openblizz launch --directory ~/Games/Warcraft3/x86_64 --exe "Warcraft III.exe" \
                 --prefix ~/Games/openblizz/warcraft3 -- -launch
```

`launch` needs [umu-launcher](https://github.com/Open-Wine-Components/umu-launcher)
(GE-Proton is downloaded automatically on first run) or Wine.

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
Warcraft III: Reforged the install manifest only contains the executables; the
game data lives in a local CASC storage, so `install` mounts the TVFS
(`vfs-root` → `war3.w3mod` → nested `vfs-N`), downloads every referenced
object through CDN archives with HTTP Range requests and writes
`Data/data/*.idx` + `data.NNN`, `Data/config`, `Data/indices` and
`.build.info` — the same layout the official installer produces. Downloads
resume, `verify --deep` re-hashes everything, `repair` re-downloads what is
missing or damaged.

**Login.** `openblizz login` opens an **isolated** profile of your default
browser (Firefox family via WebDriver BiDi, Chromium family via DevTools;
native, snap or flatpak) on the official login page. You enter password, MFA
and captcha there; OpenBlizz only reads the resulting `battle.net` session
cookies, verifies them against `account.battle.net/api/`, stores them with
owner-only permissions and closes the window. Every later command reuses and
renews that session; `logout` deletes it. The Battle.net app, its Agent and
the OAuth developer API are not used.

**Ownership.** `library scan` (run automatically after login and every six
hours) reads the same internal JSON the account page uses:
`games-and-subs` (game accounts; `titleId` is the FourCC of the NGDP code),
`classic-games` (CD keys) and `transactions` (purchase history, for
licence-only titles such as Warcraft I/II Remastered or the Blizzard Arcade
Collection). `install` refuses products marked `not owned` unless `--force`
is given. These endpoints are not a documented API and may change; OpenBlizz
labels their output accordingly rather than pretending they are stable.

**Output.** Lists are grouped by franchise (Warcraft, StarCraft, Diablo,
Blizzard Arcade, Hearthstone, Heroes of the Storm, Overwatch, Other) in aligned
tables. `products` shows the curated catalog, `products --all` every NGDP code,
`products --shop` the full storefront (including Call of Duty and third-party
titles, which are listed but cannot be installed — see
[docs/COMMANDS.md](docs/COMMANDS.md#openblizz-products)), `library list` what you own
with the evidence for each entry.

## Supported products

| Id | Game | Install | Notes |
|---|---|---|---|
| `w3` | Warcraft III: Reforged | full (executables + CASC data) | reference product; `-launch` required |
| `w3-legacy-tft` | Warcraft III legacy / TFT | executables | same game account as `w3` |
| `w2r`, `w1r` | Warcraft II / I Remastered | yes | licence-only; ownership via purchase history |
| `w2bn`, `war1` | Warcraft II BNE, Warcraft: Orcs & Humans | yes | classic CD keys |
| `s1`, `s2` | StarCraft: Remastered, StarCraft II | yes | |
| `anbs`, `osi`, `d3`, `fenris` | Diablo Immortal (PC build), II: Resurrected, III, IV | yes | `anbs`: Windows build only, not the phone game |
| `wow`, `wow_classic`, `gryphon` | World of Warcraft, Classic, Warcraft Rumble (PC build) | yes | `gryphon`: Windows build only |
| `rtro` | Blizzard Arcade Collection | yes | licence-only; purchase history |
| `hsb`, `hero`, `pro` | Hearthstone (PC build), Heroes of the Storm, Overwatch | yes | |
| `d2-classic`, `d2-lod` | Diablo II, Lord of Destruction | no (legacy installer) | ownership tracked only |

"Install" means OpenBlizz can download the build through NGDP; whether a
title runs well under Proton depends on the game (anti-cheat, launcher
requirements). Warcraft III: Reforged is tested end to end. Mobile-origin
games (Diablo Immortal, Warcraft Rumble, Hearthstone) are installed as their
Windows builds; phone builds are not on NGDP. The full category-by-category
breakdown, including Call of Duty and third-party shop titles, is in
[docs/SCOPE.md](docs/SCOPE.md).

## Limitations (honest list)

- The prebuilt installer currently publishes Linux x86_64/glibc and AArch64/
  glibc binaries; other architectures should build from source until matching
  Release assets are added.
- Ownership comes from undocumented account-page endpoints; a Blizzard change
  can break `library scan` until the parser is updated (installing with
  `--force` keeps working).
- Call of Duty titles are on NGDP (`versions`/`cdns` work) but their content is
  TACT-encrypted with keys only the Battle.net client receives, and they need
  that client at runtime; third-party storefront titles have placeholder NGDP
  entries only. Neither can be installed by OpenBlizz.
- Encrypted TACT content (some products' protected files) fails with an
  explicit error instead of being decrypted.
- Only the Windows x86_64 build of each product is selected.
- No GUI, no game-side patching (umu/Proton fixes apply as usual).

## License and trademarks

The OpenBlizz source is licensed under Apache-2.0. OpenBlizz is an independent
project and is not affiliated with or endorsed by Blizzard Entertainment.
Warcraft, StarCraft, Diablo, Overwatch, Hearthstone, Battle.net and other
names are trademarks of their respective owners. See [NOTICE](NOTICE) and
[TRADEMARKS.md](TRADEMARKS.md).
