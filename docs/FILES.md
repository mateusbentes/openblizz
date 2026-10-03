# Files, directories and environment variables

OpenBlizz follows the XDG Base Directory specification. Nothing is written
outside these locations and the game directory you pass with `--directory`.

## Per-user state

| Path | Permissions | Content | Created by | Removed by |
|---|---|---|---|---|
| `$XDG_CONFIG_HOME/openblizz/battlenet-cookies.txt` (default `~/.config/openblizz/battlenet-cookies.txt`) | `0600` | Netscape-format cookie jar with the `battle.net` session captured by `login`; renewed in place by every command that uses it | `login` | `logout` |
| `$XDG_CONFIG_HOME/openblizz/browser-profile/` | `0700` | isolated browser profile used only for the login window (no history or passwords from your normal profile). For snap/flatpak browsers the profile is placed inside the directory the sandbox can reach (`~/snap/<name>/common/openblizz-profile`, `~/.var/app/<id>/openblizz-profile`) | `login` | you may delete it any time |
| `$XDG_STATE_HOME/openblizz/library.json` (default `~/.local/state/openblizz/library.json`) | `0600` | ownership state: one record per product (`product_id`, `name`, `ownership`, `source`, `reason`, `updated`, `family`) plus unmapped purchases | `login`, `library scan`, `library add` | `library remove` (one entry) or delete the file |
| `$XDG_CACHE_HOME/openblizz/` (default `~/.cache/openblizz/`) | `0755` | downloaded CDN metadata: build/cdn configs, encoding, install/download manifests, archive indexes, TVFS manifests, keyed by hash. Safe to delete; re-downloaded on demand (a few hundred MB for W3) | `plan`, `install`, `vfs` | delete freely |

Override any of them per command with `--cookie-jar PATH` or
`--library-file PATH`; the cache follows `XDG_CACHE_HOME` only.

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

| Variable | Read by | Effect |
|---|---|---|
| `OPENBLIZZ_BROWSER` | `login` | path to the browser binary (or flatpak id) to use, checked before the desktop default |
| `XDG_CONFIG_HOME`, `XDG_STATE_HOME`, `XDG_CACHE_HOME` | all | relocate the directories above |
| `HOME` | all | fallback base for the XDG directories |
| `http_proxy`, `https_proxy`, `no_proxy`, `CURL_CA_BUNDLE`, `SSL_CERT_FILE` | all network commands (via libcurl) | standard proxy and TLS trust settings |
| `WINEPREFIX`, `PROTONPATH`, `GAMEID` | `launch` | set **by** OpenBlizz for the child process (`--prefix`, `--proton`, `umu-openblizz`); any other variable in your environment (`DXVK_HUD`, `MANGOHUD`, `PROTON_LOG`, `WINEDEBUG`, `WINEDLLOVERRIDES`, ...) is inherited unchanged |

## Network endpoints contacted

| Host | Purpose | Authentication |
|---|---|---|
| `{region}.version.battle.net` (Ribbit over HTTPS, `/v2/summary`, `/v2/products/<p>/versions`, `/cdns`) | product catalog, builds, CDN hosts | none |
| `*.cdn.blizzard.com`, `level3.blizzard.com`, `blzddist1-a.akamaihd.net`, ... | TACT content (configs, archives, loose objects) | none |
| `account.battle.net`, `{eu,us,kr,tw}.account.battle.net`, `oauth.battle.net` | browser login redirect chain, session renewal, `/api/games-and-subs`, `/api/classic-games`, `/api/transactions` | your browser session cookies |
| `{region}.shop.battle.net` | `products --shop` storefront menu and family pages | none |

OpenBlizz never contacts anything else, sends no telemetry and does not use
the Battle.net desktop app, Agent, or the OAuth developer API.
