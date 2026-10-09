# Files, directories and environment variables

OpenBlizz follows the XDG Base Directory specification for its default paths.
Explicit output options and the default launch prefix are exceptions; see the
notes below.

## Per-user state

| Path | Permissions | Content | Created by | Removed by |
|---|---|---|---|---|
| `$XDG_CONFIG_HOME/openblizz/battlenet-cookies.txt` (default `~/.config/openblizz/battlenet-cookies.txt`) | `0600` | Netscape-format `battle.net` session jar captured by `login`; renewal may update it during account-library refresh, including when an explicit `--cookie-jar` path is used | `login` | `logout` |
| `$XDG_CONFIG_HOME/openblizz/browser-profile/` | `0700` | isolated browser profile used only for the login window (no history or passwords from your normal profile). For snap/flatpak browsers the profile is placed inside the directory the sandbox can reach (`~/snap/<name>/common/openblizz-profile`, `~/.var/app/<id>/openblizz-profile`) | `login` | you may delete it any time |
| `$XDG_STATE_HOME/openblizz/library.json` (default `~/.local/state/openblizz/library.json`) | `0600` | JSON schema 1 with `products[]` records containing `product_id`, `name`, `ownership`, `source`, `reason`, and `updated_at` | `login`, `library scan`, `library add/remove`, and stale-session auto-refresh from account-dependent commands | `library remove` or delete the file |
| `$XDG_CACHE_HOME/openblizz/` (default `~/.cache/openblizz/`) | umask-dependent | decoded content objects under `objects/` and archive indexes under `indices/`, keyed by hash; safe to delete and re-downloaded on demand | `plan`, `vfs`, `install`, `update`, `verify`, `repair` | delete freely |

Override them with `--cookie-jar PATH`, `--library-file PATH`,
`library scan --dump PATH` or `login --profile-dir PATH`; those explicit paths
may be anywhere. The cache follows `XDG_CACHE_HOME` only. A launch without
`--prefix` creates `./.openblizz-prefix` relative to the current directory.

## Game directory layout

Created by `install <product> --directory DIR`. Example for Warcraft III:
Reforged:

```
DIR/
├── .build.info                  # BPSV table: build key, CDN key, product, region, version
├── x86_64/
│   ├── Warcraft III.exe         # install-manifest files, MD5-verified
│   ├── World Editor.exe
│   ├── war3_loader.dll, ClientSdk.dll, ...
└── Data/
    ├── config/xx/yy/<hash>      # build config, cdn config, patch config
    ├── indices/<hash>.index     # CDN archive indexes
    └── data/
        ├── 00000000NN.idx       # 16 local CASC index buckets
        └── data.000 ... data.NNN # local CASC archives (≤ 1 GiB each)
```

The game opens `Data/` as a local CASC storage exactly like the Battle.net
installer's output, so it is also recognised by third-party CASC tools.
The directory can be moved freely; nothing in it is tied to an absolute path.

Products whose install manifest is self-contained (no TVFS root) only get the
plain files.

## Proton / Wine prefix

`launch --prefix PATH` sets `WINEPREFIX`. If omitted, `./.openblizz-prefix`
relative to the current working directory is used — pass an explicit, stable
path (e.g. `~/Games/openblizz/warcraft3`) so the game always finds its saved
settings. umu creates and updates the prefix on first run.

Warcraft III stores documents inside the prefix at
`drive_c/users/<user>/Documents/Warcraft III/` (campaign saves, custom maps,
settings).

## Environment variables

| Variable | Set/read by | Effect |
|---|---|---|
| `OPENBLIZZ_BROWSER` | `login` | path to the browser binary (or flatpak id) to use, checked before the desktop default |
| `XDG_CONFIG_HOME`, `XDG_STATE_HOME`, `XDG_CACHE_HOME` | all | relocate the directories above |
| `HOME` | all | fallback base for the XDG directories |
| `http_proxy`, `https_proxy`, `no_proxy`, `CURL_CA_BUNDLE`, `SSL_CERT_FILE` | all network commands (via libcurl) | standard proxy and TLS trust settings |
| `WINEPREFIX`, `PROTONPATH`, `GAMEID` | `launch` | set **by** OpenBlizz for the child process on Proton/umu (`WINEPREFIX`, `PROTONPATH`, `GAMEID=umu-openblizz`) and `WINEPREFIX` on Wine; native launch sets none. Other variables (`DXVK_HUD`, `MANGOHUD`, `PROTON_LOG`, `WINEDEBUG`, `WINEDLLOVERRIDES`, ...) are inherited unchanged |

## Network endpoints contacted

| Host | Purpose | Authentication |
|---|---|---|
| `{region}.version.battle.net` (Ribbit over HTTPS, `/v2/summary`, `/v2/products/<p>/versions`, `/cdns`) | product catalog, builds, CDN hosts | none |
| `*.cdn.blizzard.com`, `level3.blizzard.com`, `blzddist1-a.akamaihd.net`, ... | TACT content (configs, archives, loose objects) | none |
| `account.battle.net`, `{eu,us,kr,tw}.account.battle.net`, `oauth.battle.net` | browser login redirect chain, session renewal, `/api/games-and-subs`, `/api/classic-games`, `/api/transactions` | your browser session cookies |
| `https://us.shop.battle.net/en-us` | `products --shop` storefront menu and family pages (currently hard-coded to the US storefront) | none |

OpenBlizz makes no direct telemetry requests and does not use the Battle.net
desktop app, Agent, or Blizzard's public/developer OAuth API. Browser login and
session renewal can follow the account site's own redirects and resources.
