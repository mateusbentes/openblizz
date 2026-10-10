# Scope: downloads, ownership and launching

OpenBlizz provides a Linux CLI for supported Windows builds delivered through Battle.net's public NGDP/TACT/CASC protocols. Its original goal is to install Blizzard games and launch them as non-Steam games through Proton. Steam integration is optional: a terminal command with umu or Wine is enough to attempt a launch.

OpenBlizz also recognizes purchases and can attempt downloads of non-Blizzard NGDP games. **A library entry, a complete downloadable build, and a working game runtime are three different outcomes.** A storefront card is not proof of ownership, and a successful download is not proof of Proton compatibility. OpenBlizz does not replace Steam, GOG, Google Play or the App Store.

## Summary table

| Category | Examples | Ownership/catalog | Download | Runtime |
|---|---|---|---|---|
| Supported Blizzard NGDP Windows builds | Warcraft I/II/III, StarCraft I/II, Diablo II: Resurrected/III/IV, Blizzard Arcade Collection, WoW, Overwatch, Heroes of the Storm | curated catalog and account scan | existing IN/EN and supported TVFS/CASC pipeline | generic umu/Proton/Wine hand-off; most titles not gameplay-tested |
| Mobile-origin titles with a PC build | Diablo Immortal (`anbs`), Warcraft Rumble (`gryphon`), Hearthstone (`hsb`) | known PC products | Windows build only | experimental, not a phone runtime |
| Mobile packages | APK/IPA on phones | not a supported package source | no | Google Play/App Store; outside OpenBlizz |
| Crash Bandicoot 4 (`wlby`) | non-Blizzard PC game | Third-party Battle.net titles | experimental; public plan verified on 2026-10-10 | not gameplay-tested; online requirements remain |
| Other third-party NGDP games | known `lyra`, scanned `thirdparty-*` entries | curated or account purchase + matching public card | requires explicit code and usable supported manifests; inspected `lyra` build is metadata-only | not guaranteed |
| Call of Duty | `odin`, `zeus`, `fore`, `lazr`, `nina`, `auks` | catalog/ownership/metadata | gated: complete content/key/runtime combination not validated | this project does not implement anti-cheat or authentication bypass |
| Classic CD-key games | Diablo II (`d2-classic`), Lord of Destruction (`d2-lod`) | ownership-only | obtain Blizzard's legacy installer from your account | run installer and game with the same Wine/umu prefix |
| DLC, services, cosmetic bundles | battle passes, game time, upgrades | known mappings may resolve the base title; otherwise reported unmapped | not promoted to standalone games by generic discovery | activated by the game/account |

## Blizzard PC builds

`install` uses Ribbit build/CDN metadata, TACT configs, IN/EN manifests, BLTE content and archive ranges. If the build has a supported TVFS root, it also populates local CASC storage. A catalog's install command describes downloader support, not a verified launch of every title.

```bash
openblizz login
openblizz library list
openblizz install w3 --directory "$HOME/Games/Warcraft3" --locale ptBR
openblizz launch --directory "$HOME/Games/Warcraft3" \
  --exe "x86_64/Warcraft III.exe" --prefix "$HOME/Games/openblizz/warcraft3" -- -launch
```

The manually verified gameplay path is Warcraft III Legacy/TFT `1.29.2.9232-legacy-tft` with umu-launcher 1.4.4 and GE-Proton10-10. Reforged's installation and Proton hand-off were exercised, but performance/input/embedded-login problems were also reported on the test machine. Reaching `fsync: up and running` alone is not proof that the game opened successfully. See [RUNTIME.md](RUNTIME.md) for the full commands and tested conditions.

Reforged uses `-launch` to avoid handing over to the Battle.net desktop app. Games may still require their own in-game login, runtime tokens, registry entries, redistributables or online services. OpenBlizz's browser login establishes the **account-library session**, not an authentication token for the launched Windows game. It does not copy those cookies into the game.

## Mobile-origin versus mobile-only games

`anbs`, `gryphon` and `hsb` refer to the Windows builds known to the catalog. The downloader does not fetch Android APKs or iOS IPAs, and installing the native OpenBlizz AArch64 binary does not create an Android environment. An ARM64 Linux host still needs a compatible Proton+FEX/umu stack for Windows x86/x86-64 binaries. Hearthstone also has PC origins; its mobile version is a separate distribution target.

## Third-party Battle.net downloads

`wlby` is **Crash Bandicoot 4**, not Call of Duty. Its public `containerless ngdp` build resolved 1,123 named files (24,980,571,164 decoded bytes) in the 2026-10-10 plan test. The downloader is enabled experimentally. No complete game download or gameplay test was performed, and its official Windows release requires an Internet connection.

For an unknown purchase, `library scan` can match a public `/product/...` storefront card and preserve a stable `thirdparty-*` id. It records the explicit `appGameCode` as `ngdp_product` and the product path as `shop_slug`. A recognized account-owned entry with a usable code can then go through the normal plan/install/update/verify/repair path. A slug-only match stays ownership-only: the installer never guesses a code from the purchase text or slug.

Third-party downloads require account-detected `owned` evidence and a valid saved session; `library add`, an unknown state and `--force` do not replace that requirement. The local library remains a cache of account responses, not a signed entitlement certificate. Curated blocks cannot be bypassed through dynamic aliases.

The Witcher 3 Remastered (`lyra`) is recognized by its purchase title, but the public build inspected on 2026-10-10 still has an empty install manifest and placeholder metadata. `plan lyra` rejects it before loading archive indexes. A public KeyRing decrypts the manifest; it does not supply missing named files. The advertised `build-file-db` mapping remains unimplemented and unvalidated.

See [THIRD_PARTY.md](THIRD_PARTY.md) for commands, dated evidence, sources and offline test coverage. Do not buy more games merely to test the generic downloader: synthetic IN/EN/BLTE fixtures already exercise it. A later live build must still be evaluated individually.

## Call of Duty

These products expose public Ribbit/NGDP metadata, but this project has not validated their complete content, keys, authentication and runtime combination. Public KeyRing support is not equivalent to a working installation or anti-cheat compatibility. They remain gated as metadata/ownership entries.

OpenBlizz does not recreate private key delivery, remove DRM, bypass a game's authentication or make anti-cheat work under Proton. Buying the Steam version does not by itself fix anti-cheat compatibility either. Use the official supported platform/client if the title cannot run on Linux.

## Games from other stores

A Steam or GOG license is not a Battle.net entitlement, and OpenBlizz does not download from those stores. Steam can provide its own Proton integration; tools such as Heroic or Lutris can manage supported external-store installations. Store availability and Linux compatibility depend on the specific edition and game; not every third-party Battle.net game is sold on every store.

An already installed Windows game may be passed to the generic runner, without any promise that an arbitrary executable will work:

```bash
openblizz launch --directory "$HOME/Games/SomeGame" --exe "Game.exe" \
  --prefix "$HOME/Games/openblizz/somegame" --backend umu --proton GE-Proton
```

OpenBlizz does not bundle umu, Proton, Wine, DXVK, Steam Runtime or FEX. See [RUNTIME.md](RUNTIME.md#installing-umu-launcher) for installation and [optional Steam integration](RUNTIME.md#optional-adding-a-game-to-steam-steam-deck--big-picture).

## Classic CD-key games

`d2-classic` and `d2-lod` track the classic licenses rather than a supported NGDP install. Download the official legacy installer from [your account page](https://account.battle.net/games), run it, and launch the installed game using the same prefix:

```bash
# Use the actual installer filename downloaded from Blizzard.
openblizz launch --directory "$HOME/Downloads" --exe "<installer>.exe" \
  --prefix "$HOME/Games/openblizz/diablo2" --backend wine
# Adjust the installed path if you chose a different location.
openblizz launch --directory "$HOME/Games/openblizz/diablo2/drive_c/Program Files (x86)/Diablo II" \
  --exe "Game.exe" --prefix "$HOME/Games/openblizz/diablo2" --backend wine
```

OpenBlizz neither downloads nor redistributes those legacy installers. A CD-key prompt, where required, belongs to the official installer/game, not the OpenBlizz terminal.

## Reporting a classification problem

Report the product id, redacted library evidence and relevant `plan` output. The curated classifications live in `src/catalog.cpp`. `library scan --dump PATH` writes raw account/purchase responses with owner-only permissions; it can contain sensitive data. Redact it before sharing and never attach session cookies, passwords, payment information or license keys. Public protocol references are in [SOURCES.md](../SOURCES.md).
