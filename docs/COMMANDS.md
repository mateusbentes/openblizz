# Command reference

Every command is `openblizz <command> [subcommand] [<product>] [options]`.
Options can appear in any order after the command; flags that take a value use
`--name VALUE`. Product ids are OpenBlizz catalog ids printed by
`openblizz products` (`w3`, `s1`, `rtro`, ...). Most are NGDP codes, but the
curated catalog also contains legacy or ownership-only ids such as `d2-classic`
and `d2-lod`. Run `openblizz --help` for the compact summary.

Exit codes:

| Code | Meaning |
|---|---|
| `0` | success (for `verify`: everything matched) |
| `1` | OpenBlizz command error; the reason is printed to stderr as `OpenBlizz error: ...` |
| `2` | `verify` found missing or corrupt files (listed on stdout) |
| other non-zero status | `launch` returns the status produced by the child process through the host shell; it is not normalized by OpenBlizz |

Common concepts and their usual scopes (not every option is accepted by every command):

| Option | Default | Meaning |
|---|---|---|
| `--region us` | `us` | Ribbit/version server region for commands that expose it: `us`, `eu`, `kr`, `tw`, `cn`, `sg`. It affects builds/CDN hosts, not your account |
| `--locale enUS` | `enUS` | locale for plan/install/update/verify/repair (`enUS`, `ptBR`, `deDE`, `esES`, `frFR`, `itIT`, `koKR`, `plPL`, `ruRU`, `zhCN`, `zhTW`, ...) |
| `--cookie-jar PATH` | XDG config default | browser session path for login/logout/scan and account validation; account-library auto-refresh uses the default jar in the current implementation |
| `--library-file PATH` | XDG state default | ownership state path for library commands and account-gated install/update/repair |

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
| (default) | built-in curated catalog | the Blizzard titles OpenBlizz knows how to handle, plus known Battle.net third-party metadata, grouped by franchise (Warcraft, StarCraft, Diablo, Blizzard Arcade, Hearthstone, Heroes of the Storm, Overwatch and third-party titles), with the install command for each. Classic CD-key titles (Diablo II, LoD) are shown as "legacy installer only" |
| `--all` | Ribbit `v2/summary` | every published summary row without a non-empty `Flags` field, with columns `Product code`, `Account titleId`, and `Ribbit seqn`; many are PTR/beta/internal codes |
| `--shop` | Battle.net storefront navigation menu | storefront games exposed by the current navigation/family pages (Blizzard, Call of Duty and third-party titles), with a `Library` column cross-referenced against your scanned library and the matching OpenBlizz id when known |
| `--shop --family SLUG` | one storefront family page | the editions/bundles of that family (`warcraft-rts`, `starcraft-remastered`, `diablo-iv`, `blizzard-arcade-collection`, ...). Slugs are printed by `--shop` |

The shop request currently uses the US storefront (`us.shop.battle.net/en-us`);
there is no `--region` selector for `--shop` yet.

Call of Duty and third-party products need separate status checks; being listed
does not mean that the current build contains downloadable game data:

- **Call of Duty** (`odin`, `zeus`, `fore`, `lazr`, `nina`, `auks`, `wlby`, ...)
  *is* published through Ribbit/NGDP: `versions`, `cdns` and the build config
  are public and readable. OpenBlizz can decode BLTE `E` chunks when a public
  KeyRing entry is present, but the complete content/key/runtime combination
  is not validated for Call of Duty. The games also require the Battle.net
  client and anti-cheat, so they remain metadata-only and `plan`/`install` are
  intentionally gated by the catalog.
  Implementing this would mean reproducing Blizzard's key delivery and the
  Battle.net client's runtime, which is out of scope for a clean-room project.
- **Third-party storefront titles** (The Witcher 3 Remastered and similar) are
  mapped to known ids when possible. The current `lyra` NGDP build is a
  metadata-only placeholder with an empty install manifest. `plan lyra` fails
  explicitly instead of reporting a successful zero-file install. If a future
  build exposes real install data, its normal NGDP plan can be tested then.

See [SCOPE.md](SCOPE.md) for the full category breakdown and the recommended
way to play each kind of title on Linux.

### `openblizz versions <product> [--region us]`

Prints the current build from the Ribbit `versions` endpoint:
`product`, `region`, `build_config`, `cdn_config`, `build_id`, `version`.

### `openblizz cdns <product> [--region us]`

Prints each CDN path and its hosts (`tpr/war3  us.cdn.blizzard.com level3.blizzard.com ...`).

### `openblizz plan <product> [options]`

```
openblizz plan w3 [--region us] [--locale enUS] [--all-locales] [--no-data] [--data-limit BYTES]
```

Resolves the metadata and content references needed by `install` **without
writing to a game directory**: downloads build/CDN config, encoding and
install manifests, archive indexes (cached under the XDG cache) and, for TVFS
products, the virtual file system; then prints the summary (version, build
config, selected files, selected bytes, encoding mappings). Use it to see the
download size before committing disk space. If the current build has an empty
install manifest (for example the public `lyra` metadata-only build), `plan`
stops before loading archive indexes and reports that the CDN build is
metadata-only.

### `openblizz vfs manifests <product> [--region us]`

Lists the TVFS manifests of the current build (`vfs-root` and the nested
`vfs-N` entries) with their encoding keys and sizes. Only meaningful for
products with a TVFS root (currently Warcraft III: Reforged).

### `openblizz vfs list <product> [--region us] [--root PATH] [--manifests-only] [--summary]`

Walks the virtual file system and prints every virtual path with its content
key and size.

| Option | Meaning |
|---|---|
| `--root PATH` | restrict to a subtree, e.g. `war3.w3mod` or `war3.w3mod:_locales:ptbr.w3mod`; nested manifest paths use `:` |
| `--manifests-only` | only show entries that are themselves nested manifests |
| `--summary` | suppresses per-file rows and prints global virtual-file, nested-manifest, content-byte and encoded-byte totals |

---

## Account session

### `openblizz login`

```
openblizz login [--browser-exe PATH|FLATPAK_ID] [--timeout 600] [--keep-browser]
                 [--cookie-jar PATH] [--profile-dir PATH] [--account-host HOST]
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
   `0600` permissions to the cookie jar, closes the browser window (unless
   `--keep-browser` was supplied) and runs `library scan`.

| Option | Default | Meaning |
|---|---|---|
| `--browser-exe` | auto | path to a browser binary, or a flatpak id (`org.mozilla.firefox`, `com.brave.Browser`) |
| `--timeout` | `600` | seconds to wait for you to finish logging in |
| `--keep-browser` | off | leave the browser window open after the session is captured (debugging) |
| `--profile-dir` | auto | override the isolated browser profile directory |
| `--account-host` | `account.battle.net` | account site host used for the login flow; intended for regional diagnostics |

### `openblizz logout [--cookie-jar PATH]`

Deletes the saved session cookies. The library file is kept (run `library
scan` after the next login to refresh it).

---

## Library (ownership)

Ownership state is a JSON file (by default under
`$XDG_STATE_HOME/openblizz/library.json`, normally
`~/.local/state/openblizz/library.json`, mode `0600`). `install`/`update`/`repair`
consult it before touching the CDN. Account-dependent commands may refresh it
automatically when older than 6 hours and a valid session exists; `verify` does
not require a session or perform an ownership check.

### `openblizz library scan [--dump PATH] [--cookie-jar PATH] [--library-file PATH] [--account-host HOST]`

Queries, with the saved session, the same internal JSON endpoints the Battle.net
account page uses:

| Endpoint | Evidence produced |
|---|---|
| `account.battle.net/api/games-and-subs` | game accounts; `titleId` is the FourCC of the NGDP code (`22323` = `W3`, `1095647827` = `ANBS`), so any NGDP product on your account is recognised, including ones absent from the curated catalog |
| `account.battle.net/api/classic-games` | classic CD keys (Diablo II, Warcraft II BNE, StarCraft Anthology, ...) |
| `account.battle.net/api/transactions?regionId=1`, `2`, and `3` | purchase history; resolves purchase-history entitlements (Warcraft I/II Remastered, Blizzard Arcade Collection, bundles). Refunded or charged-back orders are ignored |

Resulting states: `owned`, `not owned` (title absent from an endpoint that
would list it, or `Trial`), `unknown` (could not be checked), `owned (manual)`
(added with `library add`). If a purchase is not in the curated catalog,
`library scan` performs a conditional public storefront lookup. A matching
`/product/` card creates a dynamic ownership-only `thirdparty-*` entry using
the card name, slug and `appGameCode`; it is grouped under Third-party Battle.net
titles and is not assumed to be installable. Purchases without a matching card
remain under "Purchases not mapped to an installable product" so nothing is
hidden.

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
product `owned (manual)` so ownership gating does not require `--force`; a
valid session is still required by `install`, `update` and `repair`. It is
recorded as manual, not as proof of ownership. `remove` deletes the manual
entry (scanned entries are rebuilt by the next scan).

---

## Installing and maintaining a game

`install`, `update` and `repair` take `<product> --directory DIR` and require a
valid session (`openblizz login`) for account validation. `verify` takes the
same directory and does not require an account session, although it still
resolves the current build and may need network access. All four print the plan
summary first, then act.

Before downloading they check ownership: `not owned` aborts (override with
`--force`), `unknown` only warns, `owned`/`owned (manual)` proceeds.

### `openblizz install <product> --directory DIR [options]`

```
openblizz install w3 --directory ~/Games/Warcraft3 [--region us] [--locale enUS]
                  [--all-locales] [--no-data] [--jobs 4] [--limit N] [--data-limit BYTES]
                  [--cookie-jar PATH] [--library-file PATH] [--force]
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
  Data/data/*.idx, Data/data/data.NNN  local CASC storage with the TVFS content
```

Downloads are resumable: re-running `install` skips objects already present in
the local CASC index (`(already verified)` is the current progress label) and
continues where it stopped. Objects are fetched
through CDN archives using HTTP Range requests, so interrupted downloads waste
little bandwidth.

**Resume and failure handling.** `install` is idempotent: every CASC object
already present in `Data/data` (indexed by the `.idx` journals, or recovered
from the archives on start-up if a previous run was interrupted) is skipped,
journals are flushed every 256 MiB, archive range requests are retried three
times before falling back to per-object downloads, and objects that still fail
are reported at the end instead of aborting the run. Re-running the same
command finishes the job.

### `openblizz update <product> --directory DIR [--region us] [--locale enUS] [--jobs 4] [--cookie-jar PATH] [--library-file PATH]`

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

### `openblizz repair <product> --directory DIR [--region us] [--locale enUS] [--jobs 4] [--cookie-jar PATH] [--library-file PATH]`

Runs the normal (non-deep) verification pass and re-downloads install-manifest
files that fail their content check plus CASC objects that are missing or have
a size mismatch. It does not hash every CASC object; use `verify --deep` first
when you need a full content-integrity audit. It prints `Files repaired: N`.

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
| `--proton NAME` | `GE-Proton` | `PROTONPATH` for both `proton` and `umu`: `GE-Proton` (latest GE, downloaded by umu on first run), `GE-Proton9-27`, or an absolute path to a Proton install (e.g. `~/.steam/root/steamapps/common/Proton - Experimental`) |
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
