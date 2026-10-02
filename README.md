# OpenBlizz

OpenBlizz is an independent, native, cross-platform client for authenticated users to download, install, update, verify, repair, and launch their owned Blizzard games on Linux. World of Warcraft is intentionally outside the initial product scope.

The initial supported catalog includes:

- Warcraft III: Reforged (`w3`)
- Warcraft III legacy/TFT (`w3-legacy-tft`)
- Warcraft II: Remastered (`w2r`)
- Warcraft II: Battle.net Edition (`w2bn`)
- Warcraft I: Remastered (`w1r`)
- Warcraft I legacy (`war1`)
- StarCraft: Remastered (`s1`)

OpenBlizz does not distribute Battle.net, Agent.exe, game files, private keys, or proprietary Blizzard assets.

## Status

This repository is an early, buildable implementation. The NGDP/Ribbit catalog, CDN configuration retrieval, BLTE decoding, install/download manifest parsing, local caching, verification primitives, authentication hand-off, and Proton/Wine runner are implemented incrementally. Product-specific CASC/TVFS installation and entitlement backends are deliberately covered by tests and explicit capability checks rather than silently claiming unsupported behavior.

## Logging in (steamcmd-like)

```bash
./build/openblizz login
```

`login` opens an **isolated** browser window on the official Battle.net login
page. It uses your desktop default browser when possible (`xdg-settings`),
otherwise the first supported browser it finds:

| Family | Browsers | Protocol |
|---|---|---|
| Firefox | Firefox, Firefox ESR/Developer Edition, LibreWolf, Waterfox, Floorp | WebDriver BiDi |
| Chromium | Chromium, Chrome, Brave, Edge, Vivaldi, Opera | Chrome DevTools |

Native packages, snaps (`/snap/bin/firefox`, Ubuntu's `/usr/bin/firefox`
wrapper) and flatpaks (`org.mozilla.firefox`, `com.brave.Browser`, ...) are all
detected; for snaps and flatpaks the isolated profile is created inside the
directory the sandbox can access. Force a browser with `--browser-exe PATH`
(or a flatpak id) or `OPENBLIZZ_BROWSER`.

Complete the login there, including authenticator/MFA and captcha. OpenBlizz
never sees the password: when the account page loads, it reads the resulting
session cookies through the browser's local automation endpoint, verifies them
against `account.battle.net/api/`, stores them with owner-only permissions at
`~/.config/openblizz/battlenet-cookies.txt`, closes that browser window and
scans your library. The dedicated profile is never shared with your normal
browser profile.

### How ownership is resolved

Battle.net `titleId` values are the FourCC of the program code (`22323` =
`"W3"`, `5730135` = `"WoW"`, `1095647827` = `"ANBS"`). OpenBlizz decodes them
and matches the code against the full Ribbit product summary, so any NGDP game
on your account is recognised, not only the curated catalog
(`openblizz products --all` lists every published product code). Because the
account page enumerates every game account and classic CD key, a curated
product that does not appear after a successful scan is reported as
`not_owned`; `unknown` is now reserved for products that genuinely could not be
checked (no session, or a failed request).

From then on `library scan`, `library list`, `install`, `update` and `repair`
reuse that saved session and renew it automatically through the site's own
login redirect, exactly like a browser does. `openblizz logout` deletes the
saved session. This is the only authentication path: OpenBlizz does not use
the Battle.net desktop app, its Agent, the OAuth developer API or manually
exported cookie files.

## Build on Debian/Ubuntu

```bash
sudo apt install cmake g++ pkg-config \
  libcurl4-openssl-dev libssl-dev zlib1g-dev liblz4-dev \
  nlohmann-json3-dev

cmake -S . -B build -DOPENBLIZZ_BUILD_TESTS=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

## CLI examples

List the products known by the current catalog:

```bash
./build/openblizz products
```

Output is grouped by franchise (Warcraft, StarCraft, Diablo, ...) in aligned
tables; `library list` uses the same layout with a Status and Evidence column
(account page, purchase history, manual). This is the public supported product
catalog, not an account inventory; `products --all` lists every NGDP product
code and `products --shop` the public storefront highlights.

Read the current build and CDN metadata:

```bash
./build/openblizz versions w3 --region us
./build/openblizz cdns w3 --region us
```

Inspect the current build manifests without downloading game content:

```bash
./build/openblizz plan w3 --region us --locale enUS
```

Log in once (opens your default browser on the official Battle.net page):

```bash
./build/openblizz login
./build/openblizz logout   # forget the saved session
```

## Account library

OpenBlizz keeps account-library state separately from the public product
catalog. `login` runs a scan automatically; re-run it any time:

```bash
./build/openblizz library scan
./build/openblizz library list
```

The scan reads the licenses attached to your account through the same internal
JSON endpoints the Battle.net account page itself uses (`/api/games-and-subs`,
`/api/classic-games` and the purchase history `/api/transactions`), with the
session saved by `login`. Products returned with a `Good`, `Free`, `Inactive`
or similar status become `owned`; `Trial` becomes `not_owned`; titles that
create a game account but are absent become `not_owned`; licence-only titles
(Warcraft I/II Remastered, ...) are resolved from the purchase history and
otherwise stay `unknown`. Add `--dump PATH` to save the raw responses
(owner-only permissions) so unmapped `titleId` values can be added to the
mapping.

These endpoints are not part of Blizzard's documented developer API. They can
change without notice and their use by third-party tools may fall outside
Blizzard's terms; entries are labelled `account-web` in the library file.

A manual override exists for products the account page cannot express; it
does not claim proof of ownership:

```bash
./build/openblizz library add w3
./build/openblizz library remove w3
```

Library state is stored with owner-only permissions at
`~/.local/state/openblizz/library.json` unless `--library-file` is used.

Install with the saved session (no Battle.net app needed):

```bash
./build/openblizz install w3 \
  --directory "$HOME/Games/Warcraft3" \
  --locale enUS --jobs 4
```

This verifies the saved account session, refuses products your library marks
`not_owned` (override with `--force`) and downloads public TACT/NGDP content.
For Warcraft III: Reforged the install manifest only covers the executables.
The game data lives in a local CASC storage that `Warcraft III.exe` opens at
startup, so `install` also mounts the TVFS manifests of the build
(`vfs-root` -> `war3.w3mod` -> nested `vfs-N`), downloads every referenced
object through the CDN archives and writes `Data/data/*.idx` + `data.NNN`,
`Data/config`, `Data/indices` and `.build.info`. By default only `enUS` plus
`--locale` are stored (about 35 GB for enUS); `--all-locales` keeps every
`_locales/*.w3mod` and `--no-data` restores the executables-only behaviour.
Interrupted downloads resume, `verify --deep` re-hashes every stored object,
and `repair` re-downloads missing or damaged ones. The TVFS contents can be
inspected without installing:
```bash
./build/openblizz vfs manifests w3
./build/openblizz vfs list w3 --root war3.w3mod | head
```

Run a Windows game executable through Proton/umu:

```bash
./build/openblizz launch \
  --directory "$HOME/Games/Warcraft3/x86_64" \
  --exe "Warcraft III.exe" \
  --prefix "$HOME/Games/openblizz/warcraft3" \
  --proton GE-Proton -- -launch
```
Arguments after `--` are passed to the game; Warcraft III needs `-launch` to
start without the Battle.net app.

## Authentication boundary

OpenBlizz never asks for or stores a Battle.net password. Login happens in an
isolated window of your own browser on the official Battle.net page (password,
MFA, captcha); OpenBlizz only keeps the resulting session cookies, with
owner-only permissions, and renews them the way a browser would. Neither the
Battle.net desktop app, its Agent, nor the OAuth developer API are used.

Ownership comes from your own account page and purchase history. Blizzard
publishes no third-party entitlement API, so these endpoints are undocumented
and may change; OpenBlizz labels them as such instead of pretending they are a
stable API.

## Technical sources

- [Blizzard Battle.net developer portal](https://develop.battle.net/documentation)
- [TACT/NGDP documentation](https://wowdev.wiki/TACT)
- [Ribbit product endpoints](https://us.version.battle.net/v2/summary)
- [CASC/TVFS format documentation](https://wowdev.wiki/CASC)

### Ownership sources used by `library scan`

| Source | What it proves | Covers |
|---|---|---|
| `account.battle.net/api/games-and-subs` | game accounts (W3, SC, SC2, WoW, D3/D4, OW, HS, ...) | titles that create a game account; absence => `not_owned` only for those |
| `account.battle.net/api/classic-games` | registered CD keys | Warcraft II BNE, Warcraft: Orcs & Humans, StarCraft Anthology, Diablo II |
| `account.battle.net/api/transactions?regionId=1,2,3` | purchase history (`productTitle`) | license-only titles such as Warcraft I/II Remastered, bundles; refunded or charged-back orders are ignored |

Titles bought on Battle.net that OpenBlizz cannot install (DLC, services,
third-party games like *The Witcher 3: Wild Hunt — Remastered*) are listed
under "Purchases not mapped to an installable product" so nothing is hidden.
`openblizz products --shop` prints the public storefront highlights
(including third-party titles); it is not a full catalog because the shop
renders the rest client-side behind a login.

## License and trademarks

The OpenBlizz source is licensed under Apache-2.0. OpenBlizz is an independent project and is not affiliated with or endorsed by Blizzard Entertainment. Warcraft and Battle.net are trademarks of their respective owners. See [NOTICE](NOTICE) and [TRADEMARKS.md](TRADEMARKS.md).
