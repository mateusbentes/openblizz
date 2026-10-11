# Troubleshooting

Command exceptions are printed as `OpenBlizz error: <reason>` on stderr with
exit code 1. `verify` returns 2 when it finds mismatches; `launch` can return a
non-zero status from the child process. This page lists the common problems,
grouped by phase.

## Prebuilt installer

| Symptom | Cause / fix |
|---|---|
| `release asset not found` | no GitHub Release exists yet, the tag has no Linux asset, or the asset name was changed; build from source or ask a maintainer to push a `v*` tag and wait for the Linux workflow |
| `the release has no SHA256SUMS asset` | the Release was published manually or incompletely; do not bypass verification — republish it with `.github/workflows/release-linux.yml` |
| `SHA256SUMS does not contain a checksum` / `SHA-256 mismatch` | wrong asset, corrupted download, or a tampered Release; the script intentionally leaves the existing `~/.local/bin/openblizz` untouched |
| `~/.local/bin` is not in `PATH` | add the export printed by the script to `~/.bashrc`, `~/.zshrc` or the profile used by your shell, then open a new terminal |
| `no prebuilt asset for CPU architecture` / `unsupported CPU architecture` | the current public Release contains Linux x86_64 and AArch64 only; use the source build or pass a separately published asset with `--asset` |

## Build

| Symptom | Cause / fix |
|---|---|
| `Could NOT find CURL` / `OpenSSL` / `ZLIB` | development packages missing; see the per-distribution list in [BUILDING.md](BUILDING.md) |
| `A required package was not found ... liblz4` | install `liblz4-dev` / `lz4-devel` / `lz4` and make sure `pkg-config` is installed |
| `Could not find NLOHMANN_JSON_INCLUDE_DIR` | install `nlohmann-json3-dev` / `json-devel` / `nlohmann-json`, or pass `-DNLOHMANN_JSON_INCLUDE_DIR=/path` where `/path/nlohmann/json.hpp` exists |
| `CMake 3.20 or higher is required` | use your distribution's backports, `pip install cmake`, or the Kitware APT repository |
| errors about `std::ranges`, `<span>`, `consteval`, designated initialisers | recommended compiler: GCC ≥ 11 or Clang ≥ 14; GCC 10 may also work on distributions with sufficient C++20 support (`-DCMAKE_CXX_COMPILER=g++-12`) |
| linker errors mentioning `OPENSSL_1_1` or `CRYPTO_` | mixed OpenSSL 1.1 / 3.x headers and libraries; remove the old build directory and reconfigure with `rm -rf build && cmake -S . -B build` after fixing packages |

## Login

| Symptom | Cause / fix |
|---|---|
| `no supported browser found` | install Firefox or a Chromium-family browser, or point `--browser-exe PATH` / `OPENBLIZZ_BROWSER=/path` to one (flatpak id also accepted) |
| `timed out waiting for the Battle.net login to complete` (default 600 s) | finish the login in that window (password, MFA, captcha); increase with `--timeout 1200`. If the page is stuck on a captcha, retry: Battle.net sometimes serves a second Arkose challenge |
| `the browser exited before the login completed` / `the browser closed the automation connection` | you closed the window, or a sandboxed browser could not use the profile directory. Try `--browser-exe firefox` (native package) or delete the default profile under `${XDG_CONFIG_HOME:-$HOME/.config}/openblizz/browser-profile` (snap/Flatpak profiles use their sandbox-accessible paths) |
| `could not connect to the browser automation port` / `timed out waiting for the browser automation endpoint` / `the browser refused the WebSocket upgrade` | another instance of the same browser is running with remote debugging disabled by policy (common on managed machines), or a security tool blocks localhost sockets. Use a different browser family (`--browser-exe chromium`) |
| snap Firefox: window never appears | snaps cannot read `~/.config`; OpenBlizz already uses `~/snap/firefox/common/openblizz-profile`. If it still fails, `snap connect firefox:system-observe` or install the Mozilla `.deb` |
| flatpak browser exits at once | `flatpak override --user --filesystem=~/.var/app/<id> <id>` |
| login succeeds but `the account.battle.net session cookies were rejected` | the account page completed on a regional host (`eu.account.battle.net`) before cookies for `account.battle.net` were set; run `openblizz login` again. Regional redirects and cookie issuance can differ between attempts, so do not assume a second pass is fast |
| headless servers (no display) | `openblizz login` requires a graphical browser window for the official login flow. Use a desktop session, remote desktop, or a machine with a display; OpenBlizz does not provide a manual cookie-import authentication flow |

## Library / ownership

| Symptom | Cause / fix |
|---|---|
| `no valid Battle.net account session; run openblizz login first` / `library scan needs the Battle.net session saved by openblizz login` | no cookie jar, or the session expired and could not be renewed (password change, "log out everywhere", > 30 days idle). `openblizz login` again |
| a game you own shows `not owned` | it is not attached to a game account and was not found in the purchase history (gifts, very old orders, other region). Run `library scan --dump /tmp/bnet.json` and open an issue with the `titleId`/`productTitle`; the dump contains raw account responses and sensitive account/purchase data, so redact it before sharing. `library add <id>` can override ownership metadata, but a valid session is still required by install/update/repair |
| a game you own shows `unknown` | ownership could not be determined from the available account responses, such as missing or unsupported records, regional gaps, or a failed/expired check; same as above |
| products appear under "Other Battle.net products" | the NGDP code was found on your account but is not in the curated catalog yet; it is currently listable/ownable, not necessarily installable. Please report the code so it can be evaluated and catalogued |
| `Purchases not mapped to an installable product` lists a game | DLC and services have no separate download. The Witcher 3 Remastered is mapped to `lyra` when its purchase title matches, but the current public build is metadata-only. Call of Duty remains catalogued-only because its complete protected content/runtime is not validated |
| `plan lyra` says `the current CDN build is metadata-only` | the inspected public build returns a valid but empty install manifest. This is independent of the recognized license, not a login failure. It does not prove the official Battle.net distribution is unavailable; its alternative file mapping remains unimplemented. No new purchase is needed; see [THIRD_PARTY.md](THIRD_PARTY.md#the-witcher-3-remastered-lyra) |
| `encrypted BLTE ... key not found` | the object references a KeyRing id that is not present in the public KeyRing loaded for that build. OpenBlizz does not guess or bundle private keys; the product remains unavailable until the provider publishes a usable key |
| `Call of Duty remains metadata-only: its complete content/key/authentication/runtime combination has not been validated` | the curated Call of Duty downloader gate remains in place; `versions`/`cdns` and ownership are separate from a validated installation |
| `N of M CASC objects could not be downloaded ... Run the same install command again` | some objects failed (CDN 404/5xx or network drop) after 3 range retries; everything else was stored and journaled. Re-run the identical `install` command: only the missing objects are fetched |
| `all CDN hosts failed for <hash>: HTTP GET returned status 404 .../data/xx/yy/<hash>` | a loose object, archive request or fallback failed; metadata may be stale or the object may be archive-only. This does not establish a specific root cause. CASC per-object failures are collected at the end; install-manifest file failures can still abort. Retry the identical command and report persistent product/hash failures |
| `Recovered N CASC objects left unindexed by an interrupted run.` | informational: a previous run was killed (Ctrl+C, power loss, OOM) between journal flushes; the objects already in `Data/data/data.NNN` were re-indexed instead of downloaded again |
| after an interruption `already stored` is much lower than expected | current builds salvage complete unindexed records on reopen and flush journals periodically. Reuse the same game directory, product and locale; partial or invalid records may still need downloading |
| scan says `Account session expired; renewing it through the site login flow` and then fails | the renewal redirect ended on the password page: the long-lived `remember.auth.permit` cookie is gone. `openblizz login` again |

## Catalog, plan, install

| Symptom | Cause / fix |
|---|---|
| `product is not in the public catalog` / `HTTP GET returned status 404 ... /versions` | unknown product code or a product with no public build (internal/PTR codes from `products --all`) |
| `HTTP GET returned status 404 ... /data/xx/yy/<hash>` while downloading | an archive-range request or direct-object fallback failed, or archive metadata is missing/stale. Re-run first; delete `${XDG_CACHE_HOME:-$HOME/.cache}/openblizz` only if metadata appears corrupt. If it persists on a fresh cache, report the product and hash |
| `all CDN hosts failed for <hash>` | network/ISP issue or an upstream CDN outage. Retry later or try `--region eu` (different host set); downloads resume |
| `your Battle.net account does not own <id> (library state: not_owned)` | the older curated Blizzard path permits `--force` for this state, not as a replacement for login; third-party downloads require account-detected `owned` and do not accept that override |
| very slow download | increase `--jobs 8`; Blizzard CDNs are fast but per-connection throttled. Check `ulimit -n` if you see `Too many open files` |
| `No space left on device` | W3 Reforged needs ~35 GB (enUS) + ~3 GB per extra locale + the cache (~0.5 GB). `--no-data` installs only the executables (not playable) |
| install finished but game shows "install incomplete / update required" | run `openblizz update` (new build published) then `verify --deep`; make sure `.build.info` exists in the game root |
| `verify` exits 2 with `content hash mismatch` | re-run `repair`; if the same file fails again the build changed between the two commands, run `update` |

## Third-party downloads

| Symptom | Cause / fix |
|---|---|
| `dynamic product ... has no explicit NGDP appGameCode` | only a storefront slug was available, or the entry predates structured metadata. Rerun `library scan`; if the code is still absent, the entry remains ownership-only. Do not edit `reason` or guess a code from the title |
| `dynamic product ... needs account-derived ownership` / `third-party downloads require account-derived ownership` | the local entry is unknown, manual, absent or no longer owned. Refresh the account scan. `--force` and manual `library add` do not authorize third-party downloads |
| `invalid NGDP product code` | the explicit code contains unsupported characters or exceeds the length limit; report the redacted storefront metadata, not cookies |
| `install manifest content hash mismatch` / `encoding manifest content hash mismatch` | decoded manifest differs from the build-config CKey. The plan refuses it before downloading game files; retry after checking the selected build/CDN. Do not disable hash validation |
| `manifest ... is unavailable as a loose object or in the advertised archives` | all announced hosts failed the loose request and the loaded indexes contained no exact EKey. This may reflect stale/missing public distribution metadata, not a license failure. Report product, build, locale and hash; do not erase game files or bypass validation |
| `HTTP Range GET ... expected 206` / `missing or mismatched Content-Range` | the host/proxy did not return the exact requested archive interval. OpenBlizz rejects it instead of accepting the wrong bytes; host failover and applicable direct-object fallback still run. Report a persistent product/build/hash failure |
| `unsafe or inaccessible filesystem path` / `symlink inside CASC storage` | installation/cache/CASC writes do not follow symlinks; check the path and permissions and use the actual directory instead. CASC in-place writes reject hard-linked archives too. Do not erase downloaded data as a first step |
| `unsafe Windows manifest path` / `reserved Windows manifest path` | the manifest contains a drive/ADS, device, trailing-dot/space or other unsafe Windows name. Report product/build/locale; do not bypass path validation |
| `conflicting install entries for ...` | the selected manifest gives different contents to the same normalized Windows path. OpenBlizz refuses concurrent writes instead of racing over `.part` files; report product, locale and build |
| `product ... has no selected Windows install files` | the non-empty manifest has no applicable Windows/Release/locale entries. Check the product/locale; OpenBlizz does not claim a zero-file install succeeded |

`wlby` is Crash Bandicoot 4, not Call of Duty. Its public plan has been tested,
but its full download and online runtime are still experimental. `lyra`'s
empty-manifest failure describes the public build inspected on 2026-10-10,
not all official distribution paths. See [THIRD_PARTY.md](THIRD_PARTY.md).

## Launch

| Symptom | Cause / fix |
|---|---|
| `sh: 1: umu-run: not found` | install umu-launcher ([RUNTIME.md](RUNTIME.md)) or use `--backend wine` |
| `pyzstd module: Can't import compiled .so/.pyd file` | the `ubuntu-noble` umu package was installed on Ubuntu 26.04; replace it with the matching `ubuntu-resolute` architecture-specific `.deb` |
| `umu has not been setup for the user` / `Could not find steamrt4_platform_*` | the first umu launch was interrupted before Steam Runtime 4 finished. Run the same launch again; umu resumes `.parts` downloads and stores the runtime in `~/.local/share/umu/` |
| `apparmor.service` fails after installing umu | identify the failing profile with `systemctl status apparmor` and `journalctl -xeu apparmor`; retain Ubuntu's `bwrap-userns-restrict` profile and fix/disable only the offending umu profile. The umu executable can still be tested with `umu-run --version` |
| `fsync: up and running` appears after the Proton command | this indicates that Proton reached the game process; it does not by itself prove that the game window or embedded browser rendered correctly. If no window appears, continue with the black-screen/Vulkan and game-specific diagnostics below |
| Warcraft III Legacy/TFT exits immediately with one Proton build | use a fresh prefix and try the verified route with GE-Proton10-10: see [RUNTIME.md](RUNTIME.md#warcraft-iii-legacy--tft). Legacy/TFT does not need `-launch` |
| `steamrt3 validation failed` or `Could not find sniper_platform_*` on the first Classic launch | umu has not finished setting up its Sniper Steam Runtime. Run the same command again and allow umu to download and verify `steamrt3` under `~/.local/share/umu/` |
| many GStreamer warnings ending in `wrong ELF class` appear while Classic starts | these warnings can be non-fatal from the Proton runtime; if the game opens, no action is required. If it exits, retry with a fresh prefix and GE-Proton10-10 before changing game files |
| ARM64 OpenBlizz installs correctly but the Windows game does not start | the AArch64 CLI and the game runtime are separate; install an ARM64-capable Proton+FEX/umu stack, configure its x86-64 rootfs, and pass that Proton directory with `--proton`. The default x86_64 GE-Proton download is not automatically an ARM64 runtime |
| `game executable does not exist` | pass the directory that contains the `.exe` (for W3: `--directory DIR/x86_64`) or an absolute `--exe` path |
| first launch "hangs" for minutes | umu is downloading GE-Proton and the Steam Runtime (~1 GB); run `UMU_LOG=1 openblizz launch ...` to watch progress |
| Warcraft III Reforged opens then exits immediately | add `-launch` after `--`; for the separate Legacy/TFT client, do not add `-launch` and see the GE-Proton10-10 route above |
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
