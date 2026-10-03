# Troubleshooting

Every error is printed as `OpenBlizz error: <reason>` on stderr with exit
code 1. This page lists the common ones, grouped by phase.

## Prebuilt installer

| Symptom | Cause / fix |
|---|---|
| `release asset not found` | no GitHub Release exists yet, the tag has no Linux asset, or the asset name was changed; build from source or ask a maintainer to push a `v*` tag and wait for the Linux workflow |
| `the release has no SHA256SUMS asset` | the Release was published manually or incompletely; do not bypass verification — republish it with `.github/workflows/release-linux.yml` |
| `SHA256SUMS does not contain a checksum` / `SHA-256 mismatch` | wrong asset, corrupted download, or a tampered Release; the script intentionally leaves the existing `~/.local/bin/openblizz` untouched |
| `~/.local/bin` is not in `PATH` | add the export printed by the script to `~/.bashrc`, `~/.zshrc` or the profile used by your shell, then open a new terminal |
| `unsupported CPU architecture` | the current public Release contains Linux x86_64 only; use the source build or pass a separately published asset with `--asset` |

## Build

| Symptom | Cause / fix |
|---|---|
| `Could NOT find CURL` / `OpenSSL` / `ZLIB` | development packages missing; see the per-distribution list in [BUILDING.md](BUILDING.md) |
| `A required package was not found ... liblz4` | install `liblz4-dev` / `lz4-devel` / `lz4` and make sure `pkg-config` is installed |
| `Could not find NLOHMANN_JSON_INCLUDE_DIR` | install `nlohmann-json3-dev` / `json-devel` / `nlohmann-json`, or pass `-DNLOHMANN_JSON_INCLUDE_DIR=/path/containing/nlohmann` |
| `CMake 3.20 or higher is required` | use your distribution's backports, `pip install cmake`, or the Kitware APT repository |
| errors about `std::ranges`, `<span>`, `consteval`, designated initialisers | compiler too old; GCC ≥ 11 or Clang ≥ 14 (`-DCMAKE_CXX_COMPILER=g++-12`) |
| linker errors mentioning `OPENSSL_1_1` or `CRYPTO_` | mixed OpenSSL 1.1 / 3.x headers and libraries; `cmake --fresh -S . -B build` after fixing packages |

## Login

| Symptom | Cause / fix |
|---|---|
| `no supported browser found` | install Firefox or a Chromium-family browser, or point `--browser-exe PATH` / `OPENBLIZZ_BROWSER=/path` to one (flatpak id also accepted) |
| `timed out waiting for the Battle.net login to complete` (default 600 s) | finish the login in that window (password, MFA, captcha); increase with `--timeout 1200`. If the page is stuck on a captcha, retry: Battle.net sometimes serves a second Arkose challenge |
| `the browser exited before the login completed` / `the browser closed the automation connection` | you closed the window, or a sandboxed browser could not use the profile directory. Try `--browser-exe firefox` (native package) or delete `~/.config/openblizz/browser-profile` |
| `could not connect to the browser automation port` / `timed out waiting for the browser automation endpoint` / `the browser refused the WebSocket upgrade` | another instance of the same browser is running with remote debugging disabled by policy (common on managed machines), or a security tool blocks localhost sockets. Use a different browser family (`--browser-exe chromium`) |
| snap Firefox: window never appears | snaps cannot read `~/.config`; OpenBlizz already uses `~/snap/firefox/common/openblizz-profile`. If it still fails, `snap connect firefox:system-observe` or install the Mozilla `.deb` |
| flatpak browser exits at once | `flatpak override --user --filesystem=~/.var/app/<id> <id>` |
| login succeeds but `the account.battle.net session cookies were rejected` | the account page completed on a regional host (`eu.account.battle.net`) before cookies for `account.battle.net` were set; run `openblizz login` again — the second pass is fast because "remember me" is still active |
| headless servers (no display) | run `login` on a desktop machine and copy `~/.config/openblizz/battlenet-cookies.txt` (0600) to the server; every other command works without a display |

## Library / ownership

| Symptom | Cause / fix |
|---|---|
| `no valid Battle.net account session; run openblizz login first` / `library scan needs the Battle.net session saved by openblizz login` | no cookie jar, or the session expired and could not be renewed (password change, "log out everywhere", > 30 days idle). `openblizz login` again |
| a game you own shows `not owned` | it is not attached to a game account and was not found in the purchase history (gifts, very old orders, other region). Run `library scan --dump /tmp/bnet.json` and open an issue with the `titleId`/`productTitle` (the dump contains no password, but does contain your account id and purchase history — redact before sharing). Meanwhile `library add <id>` lets you install |
| a game you own shows `unknown` | licence-only title with no purchase record in regions 1-3; same as above |
| products appear under "Other Battle.net products" | the NGDP code was found on your account but is not in the curated catalog yet; it is still installable. Please report the code |
| `Purchases not mapped to an installable product` lists a game | DLC, services, Call of Duty or third-party titles (e.g. The Witcher 3 Remastered) are not distributed through NGDP and cannot be installed by OpenBlizz |
| scan says `Account session expired; renewing it through the site login flow` and then fails | the renewal redirect ended on the password page: the long-lived `remember.auth.permit` cookie is gone. `openblizz login` again |

## Catalog, plan, install

| Symptom | Cause / fix |
|---|---|
| `product is not in the public catalog` / `HTTP GET returned status 404 ... /versions` | unknown product code or a product with no public build (internal/PTR codes from `products --all`) |
| `HTTP GET returned status 404 ... /data/xx/yy/<hash>` while downloading | the object is only inside a CDN archive and the archive index lookup failed; usually a stale cache. `rm -rf ~/.cache/openblizz` and re-run. If it persists on a fresh cache, report the product and hash |
| `all CDN hosts failed for <hash>` | network/ISP issue or an upstream CDN outage. Retry later or try `--region eu` (different host set); downloads resume |
| `your Battle.net account does not own <id> (library state: not_owned)` | see the library section; `--force` installs anyway (public CDN content), but the game may still refuse to log in |
| very slow download | increase `--jobs 8`; Blizzard CDNs are fast but per-connection throttled. Check `ulimit -n` if you see `Too many open files` |
| `No space left on device` | W3 Reforged needs ~35 GB (enUS) + ~3 GB per extra locale + the cache (~0.5 GB). `--no-data` installs only the executables (not playable) |
| install finished but game shows "install incomplete / update required" | run `openblizz update` (new build published) then `verify --deep`; make sure `.build.info` exists in the game root |
| `verify` exits 2 with `content hash mismatch` | re-run `repair`; if the same file fails again the build changed between the two commands, run `update` |

## Launch

| Symptom | Cause / fix |
|---|---|
| `sh: 1: umu-run: not found` | install umu-launcher ([RUNTIME.md](RUNTIME.md)) or use `--backend wine` |
| `game executable does not exist` | pass the directory that contains the `.exe` (for W3: `--directory DIR/x86_64`) or an absolute `--exe` path |
| first launch "hangs" for minutes | umu is downloading GE-Proton and the Steam Runtime (~1 GB); run `UMU_LOG=1 openblizz launch ...` to watch progress |
| Warcraft III opens then exits immediately | missing `-launch` after `--` |
| black screen / no Vulkan device | update Mesa/NVIDIA drivers; try `PROTON_USE_WINED3D=1` as a fallback |
| game asks to log in every time | the prefix changed; always pass the same `--prefix` |
| Steam shortcut does nothing | make sure *Compatibility* is unchecked and the *Target* points to `openblizz`, *Launch options* begin with `launch`. Test the same line in a terminal first |
| controller / Steam Input not working | add the shortcut to Steam and start it from Steam (umu inherits Steam Input when launched from Steam) |

## Collecting information for a bug report

```bash
git -C openblizz rev-parse --short HEAD   # OpenBlizz commit
cmake --system-information 2>/dev/null | grep -E 'CMAKE_CXX_COMPILER_VERSION|CMAKE_SYSTEM_NAME' 
openblizz versions <product>          # build in question
openblizz library scan --dump /tmp/bnet.json   # ownership issues (redact before sharing)
UMU_LOG=1 PROTON_LOG=1 openblizz launch ...    # runtime issues; attach ~/steam-umu-openblizz.log
```

Never attach `battlenet-cookies.txt` or the `browser-profile` directory: they
contain your live session.
