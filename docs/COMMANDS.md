# Command reference

Every command is `openblizz <command> [subcommand] [<product>] [options]`.
Options can appear in any order after the command; flags that take a value use
`--name VALUE`. Product ids are the NGDP codes printed by `openblizz products`
(`w3`, `s1`, `rtro`, ...). Run `openblizz --help` for the compact summary.

Exit codes:

| Code | Meaning |
|---|---|
| `0` | success (for `verify`: everything matched) |
| `1` | error; the reason is printed to stderr as `OpenBlizz error: ...` |
| `2` | `verify` found missing or corrupt files (listed on stdout) |

Common options (accepted by every network command):

| Option | Default | Meaning |
|---|---|---|
| `--region us` | `us` | Ribbit/version server region: `us`, `eu`, `kr`, `tw`, `cn`, `sg`. Affects which build and CDN hosts are used, not your account |
| `--locale enUS` | `enUS` | Install/verify locale tag (`enUS`, `ptBR`, `deDE`, `esES`, `frFR`, `itIT`, `koKR`, `plPL`, `ruRU`, `zhCN`, `zhTW`, ...) |
| `--cookie-jar PATH` | `~/.config/openblizz/battlenet-cookies.txt` | where the browser session is stored/read |
| `--library-file PATH` | `~/.local/state/openblizz/library.json` | where ownership state is stored/read |

## Typical workflow

```bash
openblizz login                                      # once; opens your browser, then scans your library
openblizz library list                               # what you own, grouped by franchise
openblizz install w3 --directory ~/Games/Warcraft3   # ~35 GB for enUS
openblizz launch --directory ~/Games/Warcraft3/x86_64 --exe "Warcraft III.exe" \
                 --prefix ~/Games/openblizz/warcraft3 -- -launch
# later
openblizz update w3 --directory ~/Games/Warcraft3    # fetch the new build
openblizz verify w3 --directory ~/Games/Warcraft3    # quick check; add --deep to re-hash everything
openblizz repair w3 --directory ~/Games/Warcraft3    # re-download what verify flagged
```

---

## Catalog and metadata (no login required)

### `openblizz products`

```
openblizz products
openblizz products --all [--region us]
openblizz products --shop [--family SLUG]
```

| Mode | Source | Shows |
|---|---|---|
| (default) | built-in curated catalog | the Blizzard titles OpenBlizz knows how to handle, grouped by franchise (Warcraft, StarCraft, Diablo, Blizzard Arcade, Hearthstone, Heroes of the Storm, Overwatch), with the install command for each. Classic CD-key titles (Diablo II, LoD) are shown as "legacy installer only" |
| `--all` | Ribbit `v2/summary` | every published NGDP product code (hundreds, including PTR/beta/internal codes), with the franchise when known and "Other Battle.net products" otherwise |
| `--shop` | Battle.net storefront navigation menu | every game sold on the shop (Blizzard, Call of Duty and third-party titles), with a `Library` column cross-referenced against your scanned library and the matching OpenBlizz id when installable |
| `--shop --family SLUG` | one storefront family page | the editions/bundles of that family (`warcraft-rts`, `starcraft-remastered`, `diablo-iv`, `blizzard-arcade-collection`, ...). Slugs are printed by `--shop` |

Call of Duty and third-party products are listed for completeness but **cannot
be installed** by OpenBlizz, for two different reasons:

- **Call of Duty** (`odin`, `zeus`, `fore`, `lazr`, `nina`, `auks`, `wlby`, ...)
  *is* published through Ribbit/NGDP: `versions`, `cdns` and the build config
  are public and readable. However the `versions` row carries a **KeyRing** and
  the game content is TACT-encrypted with keys that only the Battle.net client
  receives after an entitlement check; the games also require that client and
  its anti-cheat at runtime. OpenBlizz therefore catalogues them under the
  *Call of Duty* franchise for ownership and metadata only, and `plan`/`install`
  stop with an explicit message instead of downloading undecryptable data.
  Implementing this would mean reproducing Blizzard's key delivery and the
  Battle.net client's runtime, which is out of scope for a clean-room project.
- **Third-party storefront titles** (The Witcher 3 Remastered and similar) only
  have *placeholder* NGDP entries (empty root, a 39-byte install manifest): the
  actual game is not delivered through the Blizzard CDN at all, so there is
  nothing OpenBlizz could download.

### `openblizz versions <product> [--region us]`

Prints the current build from the Ribbit `versions` endpoint:
`product`, `region`, `build_config`, `cdn_config`, `build_id`, `version`.

### `openblizz cdns <product> [--region us]`

Prints each CDN path and its hosts (`tpr/war3  us.cdn.blizzard.com level3.blizzard.com ...`).

### `openblizz plan <product> [options]`

```
openblizz plan w3 [--region us] [--locale enUS] [--all-locales] [--no-data] [--data-limit BYTES]
```

Resolves everything `install` would do **without writing to a game
directory**: downloads build/CDN config, encoding, install and download
manifests, archive indexes (cached under `~/.cache/openblizz`) and, for TVFS
products, the virtual file system; then prints the summary (version, build
config, selected files, selected bytes, encoding mappings). Use it to see the
download size before committing disk space.

### `openblizz vfs manifests <product> [--region us]`

Lists the TVFS manifests of the current build (`vfs-root` and the nested
`vfs-N` entries) with their encoding keys and sizes. Only meaningful for
products with a TVFS root (currently Warcraft III: Reforged).

### `openblizz vfs list <product> [--region us] [--root war3.w3mod] [--manifests-only] [--summary]`

Walks the virtual file system and prints every virtual path with its content
key and size.

| Option | Meaning |
|---|---|
| `--root PATH` | restrict to a subtree, e.g. `war3.w3mod` or `war3.w3mod/_locales/ptbr.w3mod` |
| `--manifests-only` | only show entries that are themselves nested manifests |
| `--summary` | per-top-level-directory counts and byte totals instead of every file |

---

## Account session

### `openblizz login`

```
openblizz login [--browser-exe PATH|FLATPAK_ID] [--timeout 600] [--keep-browser] [--cookie-jar PATH]
```

1. Picks a browser: `--browser-exe`, else `$OPENBLIZZ_BROWSER`, else the desktop
   default (`xdg-settings get default-web-browser`), else the first installed
   Firefox-family (Firefox, ESR, Developer Edition, LibreWolf, Waterfox, Floorp)
   or Chromium-family (Chromium, Chrome, Brave, Edge, Vivaldi, Opera) browser,
   whether native, snap or flatpak.
2. Starts it with a **dedicated, isolated profile**
   (`~/.config/openblizz/browser-profile`, never your personal profile) on the
   official Battle.net login page, driven through WebDriver BiDi (Firefox) or
   Chrome DevTools Protocol (Chromium) on localhost.
3. You log in there: password, authenticator/MFA, captcha, "remember me".
   OpenBlizz does not see or store the password.
4. When the account page loads, OpenBlizz reads the session cookies for
   `battle.net`, verifies them against `account.battle.net/api/`, saves them with
   `0600` permissions to the cookie jar, closes the browser window and runs
   `library scan`.

| Option | Default | Meaning |
|---|---|---|
| `--browser-exe` | auto | path to a browser binary, or a flatpak id (`org.mozilla.firefox`, `com.brave.Browser`) |
| `--timeout` | `600` | seconds to wait for you to finish logging in |
| `--keep-browser` | off | leave the browser window open after the session is captured (debugging) |

### `openblizz logout [--cookie-jar PATH]`

Deletes the saved session cookies. The library file is kept (run `library
scan` after the next login to refresh it).

---

## Library (ownership)

Ownership state is a JSON file (`~/.local/state/openblizz/library.json`,
`0600`) that `install`/`update`/`repair` consult before touching the CDN. It
is refreshed automatically when older than 6 hours and a valid session exists.

### `openblizz library scan [--dump PATH] [--cookie-jar PATH] [--library-file PATH]`

Queries, with the saved session, the same internal JSON endpoints the Battle.net
account page uses:

| Endpoint | Evidence produced |
|---|---|
| `account.battle.net/api/games-and-subs` | game accounts; `titleId` is the FourCC of the NGDP code (`22323` = `W3`, `1095647827` = `ANBS`), so any NGDP product on your account is recognised, including ones absent from the curated catalog |
| `account.battle.net/api/classic-games` | classic CD keys (Diablo II, Warcraft II BNE, StarCraft Anthology, ...) |
| `account.battle.net/api/transactions?regionId=1,2,3` | purchase history; resolves licence-only titles (Warcraft I/II Remastered, Blizzard Arcade Collection, bundles). Refunded or charged-back orders are ignored |

Resulting states: `owned`, `not owned` (title absent from an endpoint that
would list it, or `Trial`), `unknown` (could not be checked), `owned (manual)`
(added with `library add`). Purchases that do not map to an installable
product are listed under "Purchases not mapped to an installable product" so
nothing is hidden.

`--dump PATH` writes the raw JSON responses (`0600`) so unmapped `titleId`
values can be reported.

These endpoints are not part of Blizzard's documented developer API; they can
change without notice. Entries carry the source `account page` / `purchase
history` in the Evidence column so you can tell them apart.

### `openblizz library list [--all] [--library-file PATH]`

Prints the owned products grouped by franchise with `Id`, `Game`, `Status`,
`Evidence` columns. `--all` also shows not-owned and unknown catalog entries.

### `openblizz library add <product>` / `openblizz library remove <product>`

Manual override for products the account page cannot express. `add` marks the
product `owned (manual)` so `install` proceeds without `--force`; it is
recorded as manual, not as proof of ownership. `remove` deletes the manual
entry (scanned entries are rebuilt by the next scan).

---

## Installing and maintaining a game

All four commands take `<product> --directory DIR` and require a valid session
(`openblizz login`). They print the plan summary first, then act.

Before downloading they check ownership: `not owned` aborts (override with
`--force`), `unknown` only warns, `owned`/`owned (manual)` proceeds.

### `openblizz install <product> --directory DIR [options]`

```
openblizz install w3 --directory ~/Games/Warcraft3 [--region us] [--locale enUS]
                  [--all-locales] [--no-data] [--jobs 4] [--limit N] [--data-limit BYTES] [--force]
```

| Option | Default | Meaning |
|---|---|---|
| `--directory DIR` | required | game root; created if missing |
| `--locale` | `enUS` | locale whose data is stored in addition to enUS |
| `--all-locales` | off | store every `_locales/*.w3mod` (W3: roughly +3 GB per language) |
| `--no-data` | off | only the install manifest (executables/DLLs); skip the CASC data store |
| `--jobs N` | `4` | parallel downloads |
| `--limit N` | all | only process the first N install-manifest files (testing) |
| `--data-limit BYTES` | all | stop filling the CASC store after this many bytes (testing) |
| `--force` | off | ignore a `not owned` library state |

What is written (Warcraft III: Reforged):

```
DIR/
  .build.info                 build/CDN configuration the game reads at startup
  x86_64/Warcraft III.exe     install-manifest files (executables, DLLs, World Editor)
  x86_64/...
  Data/config/..              build config, cdn config, patch config
  Data/indices/*.index        archive indexes
  Data/data/*.idx, data.NNN   local CASC storage with the TVFS content
```

Downloads are resumable: re-running `install` skips already verified objects
(`(already verified)`) and continues where it stopped. Objects are fetched
through CDN archives using HTTP Range requests, so interrupted downloads waste
little bandwidth.

### `openblizz update <product> --directory DIR [--region us] [--locale enUS] [--jobs 4]`

Re-resolves the current build and downloads only objects that changed or are
missing, then rewrites `.build.info`. Same as `install` with the default
options; kept as a separate verb for clarity.

### `openblizz verify <product> --directory DIR [--region us] [--locale enUS] [--all-locales] [--deep]`

Checks the installation against the current build:

- default: every install-manifest file (executables, DLLs) is fully hashed
  (MD5 against the content key); `.build.info`, `Data/config` and the presence
  and size of every CASC object referenced by the TVFS are checked through the
  `.idx` files (fast, mostly metadata);
- `--deep`: additionally re-hashes every stored CASC object — reads the whole
  installation (~35 GB for W3 enUS).

Prints one line per problem and exits `2` when anything is wrong, `0` otherwise.

### `openblizz repair <product> --directory DIR [--region us] [--locale enUS] [--jobs 4]`

Re-downloads everything `verify --deep` would flag and prints `Files repaired: N`.

---

## Launching

### `openblizz launch`

```
openblizz launch --directory DIR --exe GAME.exe [--prefix PREFIX]
                 [--backend proton|umu|wine|native] [--proton GE-Proton] [-- GAME_ARGS...]
```

| Option | Default | Meaning |
|---|---|---|
| `--directory DIR` | required | working directory; `--exe` is resolved relative to it |
| `--exe FILE` | required | executable name or absolute path |
| `--prefix PATH` | `./.openblizz-prefix` | `WINEPREFIX` for Proton/Wine; use a dedicated directory per game |
| `--backend` | `proton` | `proton` and `umu` both run `umu-run` (Proton via the Unified Launcher); `wine` runs system Wine; `native` executes the file directly |
| `--proton NAME` | `GE-Proton` | `PROTONPATH` for umu: `GE-Proton` (latest GE, downloaded by umu on first run), `GE-Proton9-27`, or an absolute path to a Proton install (e.g. `~/.steam/root/steamapps/common/Proton - Experimental`) |
| `-- ARGS` | none | everything after `--` goes to the game |

The exact command line is printed (`Launching: ...`) so it can be pasted into a
Steam shortcut or a desktop file. `GAMEID` is set to `umu-openblizz`.

Warcraft III: Reforged needs `-launch` to start without the Battle.net app:

```bash
openblizz launch --directory ~/Games/Warcraft3/x86_64 --exe "Warcraft III.exe" \
  --prefix ~/Games/openblizz/warcraft3 -- -launch
```

See [RUNTIME.md](RUNTIME.md) for installing umu/Proton per distribution and
adding the game to Steam.
