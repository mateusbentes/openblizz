# Architecture and implementation notes

OpenBlizz is one static library (`openblizz_core`) plus a thin CLI
(`src/main.cpp`). The library is split into protocol, format, content, account
and runner layers; `tests/core_tests.cpp` covers selected parsers, storage and
account helpers with synthetic fixtures (no Blizzard data is checked in), not
every network or orchestration path.

```
src/http.cpp          libcurl wrapper: GET / Range GET / POST / file download, cookie jars
src/hash.cpp          MD5, SHA-256 (OpenSSL), Jenkins lookup3 (CASC checksums)
src/formats.cpp       BPSV tables, build/CDN config, BLTE, IN/DL/EN manifests, archive indexes
src/catalog.cpp       Ribbit V2 over HTTPS (summary, versions, cdns) + curated product list
src/tvfs.cpp          TVFS container parser and recursive VFS resolver
src/casc.cpp          local CASC storage writer/reader (data.NNN + .idx journals)
src/installer.cpp     plan / install / update / verify / repair orchestration
src/library.cpp       account library: session renewal, ownership providers, shop parsers, library.json
src/browser_login.cpp browser discovery, WebDriver BiDi / DevTools drivers, cookie capture
src/runner.cpp        launch backends (umu/Proton, Wine, native)
include/openblizz/    public headers (+ table.hpp: aligned grouped tables, franchise ordering)
```

## Protocol layer

`Catalog` reads Ribbit V2 through its HTTPS mirror
(`{region}.version.battle.net/v2/...`): `summary` for every product code,
`products/<p>/versions` and `/cdns` for the current build. Config objects are
checked against their MD5 identifiers. Decoded install objects are checked
against their install-manifest content keys when consumed; archive indexes are
structurally parsed, while not every raw CDN object is globally hash-validated
before use.

`HttpClient` wraps libcurl: plain GET for configs and manifests, Range GET
for archive-backed objects (neighbouring objects are coalesced into ≤ 32 MiB
requests). The current browser automation uses loopback WebSocket connections
for WebDriver BiDi/CDP; `HttpClient::post()` is a generic client capability and
is not the login handshake. Hosts in the selected CDN record are tried in turn
before a download is declared failed.

## Format layer

- **BPSV** (`Name!TYPE:len|...`) for Ribbit and `.build.info`.
- **BLTE** with the `N` (raw), `Z` (zlib) and `4` (LZ4) chunk modes. `E` chunks
  are decoded when their public KeyRing entry is available: the implementation
  parses the key/IV/type header, applies the CASC Salsa20 variant (4- or 8-byte
  IV with the chunk index mixed into the first four IV bytes) or ARC4, then
  decodes the inner compression marker. Missing keys and nested encryption fail
  explicitly; OpenBlizz does not invent or ship private Blizzard keys.
- **Encoding** (`EN`): content key → encoding key(s) and encoded sizes.
- **Install** (`IN`) and **Download** (`DL`) manifest parsers. Installer tag
  selection currently uses Windows, locale and Release tags; `x86_64` is build
  metadata, not an independent selector in `Installer::select_entries()`.
- **Archive indexes** (`.index`): EKey → (archive, offset, size).
- **TVFS**: path table with prefix folders, VFS table, CFT table with 9-byte
  EKeys and encoded sizes; `VfsResolver` mounts `vfs-root` and follows the
  nested `vfs-N` references listed in the build config (`war3.w3mod` and its
  `_locales/*.w3mod` children for Warcraft III).

## Content layer

`Installer::plan` resolves build config → CDN config → optional KeyRing → encoding → install
manifest → selected files → archive indexes (cached under `$XDG_CACHE_HOME/openblizz`) and, when
the build has a TVFS root, the whole virtual file system filtered to `enUS`
plus the requested locale. The download-manifest parser exists, but the
current planning path does not use a download manifest.
An empty install manifest is rejected before archive indexes are fetched, so
metadata-only products do not perform unnecessary CASC I/O.

`Installer::install` writes install-manifest files atomically (`.part` then
rename) after MD5 verification, and streams every TVFS object into
`CascStorage`, which produces exactly the layout the game executable opens:
`Data/data/data.NNN` archives (30-byte header: reversed EKey, size, Jenkins
checksum A, checksum B), 16 bucketed version-7 `.idx` journals,
`Data/config/xx/yy/<hash>`, `Data/indices/<hash>.index` and `.build.info`.
Storage reopens and resumes, so interrupted installs continue; `verify`
checks manifest files by hash and CASC objects by presence/size (`--deep`
re-hashes them). `repair` runs the non-deep verification pass, removes journal
entries for CASC objects reported with size failures, and downloads the
selected missing or broken data again.

## Account layer

Authentication is browser-only. `BrowserLogin` discovers a browser (explicit
path, `OPENBLIZZ_BROWSER`, `xdg-settings` default, then known names; native,
snap or flatpak), starts it with an isolated profile and a localhost
automation port, and drives it with either **WebDriver BiDi** (Firefox family;
`--remote-debugging-port` + WebSocket, `session.new`, `storage.getCookies`) or
the **Chrome DevTools Protocol** (Chromium family; `/json/list`,
`Storage.getCookies`). It polls the current URL until the account page is
reached, captures the `battle.net` cookies, validates them against
`account.battle.net/api/`, writes a Netscape cookie jar with mode 0600 and
closes the window unless `--keep-browser` was requested. The password never
crosses the automation channel: only URLs and cookies are read.

`LibraryManager` renews the session the way a browser would (following the
`oauth2/authorization/account-settings` redirect chain with the long-lived
remember/login cookies), then queries the account page's own JSON
endpoints:

| Endpoint | Provider | Mapping |
|---|---|---|
| `/api/games-and-subs` | `account-web` | `titleId` is the big-endian FourCC of the NGDP product code (`22323` → `W3`, `21297` → `S1`, `1095647827` → `ANBS`); decoded codes are matched against the Ribbit summary, so unknown products are still recognised and listed under "Other Battle.net products" |
| `/api/classic-games` | `account-web` | CD-key titles by name (Diablo II, Warcraft II BNE, ...) |
| `/api/transactions?regionId=1`, `2`, and `3` | `account-purchases` | `productTitle` → product(s); bundles expand (e.g. "Warcraft Remastered Battle Chest" → `w1r`, `w2r`); refunded/charged-back orders are skipped |

The merged result is written to `library.json` (0600) and refreshed
automatically when older than six hours. `install`/`update`/`repair` consult
it: `not owned` blocks (unless `--force`), `unknown` warns. Custom
`--cookie-jar` and `--library-file` paths are supported by the relevant CLI
commands; account auto-refresh currently uses the default cookie jar.

`products --shop` parses the storefront navigation menu (`/family/...` and
`/product/...` cards) and family pages so the storefront entries exposed by
those pages, including third-party titles, are visible and cross-referenced
with the library. The result depends on what the storefront publishes to the
current page payload. The known third-party product `lyra` is mapped from its
purchase title, but its current public build has an empty install manifest and
is rejected as metadata-only rather than reported as a successful install.

These account endpoints are not part of Blizzard's documented developer API
and may change; the code isolates them in `library.cpp` and labels their
output accordingly. Blizzard's public/developer OAuth API, the Battle.net
Agent and manual cookie-import workflow were removed on purpose. Session
renewal still follows the account site's internal OAuth2 redirect as part of
the browser-session flow.

## Runner layer

`Runner::launch` builds a shell command with `WINEPREFIX`, `PROTONPATH` and
`GAMEID=umu-openblizz` and executes `umu-run` (Proton/umu backends), `wine`,
or the executable itself (`native`). Arguments after `--` are forwarded
verbatim; the exact command is printed so it can be reused in Steam shortcuts.

## Testing

`ctest` runs `openblizz_tests`: BPSV/config parsing, representative BLTE raw
and encrypted Salsa20 fixtures, encoding and archive-index lookups, TVFS parsing/resolution,
CASC journal round-trips and salvage, FourCC decoding, account JSON parsers,
shop menu/family parsers, cookie-jar handling and table rendering. The tests
are plain `assert()` based and need no network; HttpClient networking,
Installer orchestration, Runner execution and the full set of BLTE compression
modes are not covered end to end.
