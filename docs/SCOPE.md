# Scope: what OpenBlizz installs, what it only lists, and where to play the rest

OpenBlizz exists to fill one gap: **Battle.net-exclusive games cannot be
obtained on Linux without the Battle.net desktop app**, which has no Linux
build. Everything that is also sold on Steam or GOG already runs on Linux
through Steam's Proton or Heroic/Lutris, so OpenBlizz deliberately does not try
to replace those stores. This page explains, category by category, what the
tool does, why some products are listed but not installable, and what to do
instead. All statements below were checked against the public Ribbit/NGDP
endpoints; see [IMPLEMENTATION.md](IMPLEMENTATION.md) for the protocol details.

## Summary table

| Category | Examples | `products` | `library scan` | `install` | `launch` | What to do |
|---|---|---|---|---|---|---|
| Battle.net exclusives (PC) | Warcraft I/II/III, StarCraft I/II, Diablo II: R / III / IV, Blizzard Arcade Collection, Hearthstone, Heroes of the Storm, Overwatch, World of Warcraft | yes | yes | **yes** (NGDP/TACT) | yes (umu/Proton) | use OpenBlizz |
| Mobile-origin games, PC build | Diablo Immortal (`anbs`), Warcraft Rumble (`gryphon`), Hearthstone (`hsb`) | yes | yes | **yes** (Windows build) | yes, experimental | use OpenBlizz for the PC version |
| Mobile builds (APK / IPA) | Diablo Immortal, Rumble, Hearthstone on a phone | no | no | **no** | no | Google Play / App Store; not distributed through NGDP at all |
| Call of Duty | `odin`, `zeus`, `fore`, `lazr`, `nina`, `auks`, `wlby` | yes (Call of Duty group) | yes | **no** (TACT-encrypted, KeyRing) | no | official Battle.net client, or buy the Steam version |
| Third-party storefront titles | The Witcher 3: Wild Hunt Remastered and other non-Blizzard games sold on the Battle.net shop | `--shop` only | purchase shown as unmapped | **no** (placeholder NGDP entries) | yes, if installed from elsewhere | buy on Steam/GOG; optionally run through `openblizz launch` |
| Classic CD-key games | Diablo II (`d2-classic`), Lord of Destruction (`d2-lod`) | yes (legacy installer only) | yes | **no** | yes (`--backend wine` or umu) | legacy installer from the account page, then `launch` |
| DLC, services, in-game bundles | expansions, battle passes, cosmetics, WoW game time | `--shop` only | purchase shown as unmapped | n/a | n/a | activated inside the game / account, nothing to download separately |

## Battle.net exclusives: the reason OpenBlizz exists

These titles are only distributed by Blizzard, through the NGDP protocol
(Ribbit `summary`/`versions`/`cdns`, TACT build configs, BLTE-encoded content on
`*.cdn.blizzard.com`). OpenBlizz speaks that protocol directly:

```bash
openblizz login                                  # browser window, once
openblizz library list                           # what you own
openblizz install w3 --directory ~/Games/Warcraft3 --locale ptBR
openblizz launch --directory ~/Games/Warcraft3/x86_64 --exe "Warcraft III.exe" \
  --prefix ~/Games/openblizz/warcraft3 -- -launch
```

Login inside the game uses Blizzard's own in-game login screen (Warcraft III,
StarCraft: Remastered, Diablo II: Resurrected and the Arcade Collection all
have one), so the Battle.net app is not needed at runtime either. Pass
`-launch` to Warcraft III and StarCraft: Remastered to stop them from handing
over to the Battle.net app.

Online titles that rely on the Battle.net app for authentication at startup
(World of Warcraft, Overwatch, Diablo IV, Hearthstone) download fine but may
not reach their login screen without the app; they are catalogued as
installable because the content is, and the runtime outcome is reported as
experimental until verified. Test reports are welcome.

## Mobile-origin games

Diablo Immortal, Warcraft Rumble and Hearthstone started on phones but also
ship a Windows build through Battle.net. That Windows build is a normal NGDP
product (for example `anbs` resolves 2,431 files for version 5.1.0.x), so
OpenBlizz installs it like any other PC game. The phone builds are published on
Google Play and the App Store, never on NGDP, and are therefore out of reach by
design — the same is true of the official Battle.net app.

## Call of Duty: on NGDP, but encrypted

Call of Duty titles *are* published through the same Ribbit/NGDP endpoints:
`openblizz versions auks` and `openblizz cdns auks` work, and the build config
on the CDN is plain text (`build-name = release_..._cod25_season6_signed_bnet_r_ship`).
Two things make them non-installable anyway:

1. The `versions` row carries a **KeyRing** column and the game content is
   TACT-encrypted with keys that only the Battle.net client receives after an
   entitlement check. OpenBlizz could download the encrypted blobs, but it
   cannot decrypt them, so `plan`/`install` refuse early with an explicit
   message instead of filling your disk with unusable data.
2. The games require the Battle.net client and the Ricochet anti-cheat at
   runtime, which do not run under Proton without the official stack.

Reproducing the key delivery would mean re-implementing Blizzard's client
authentication and DRM, which is out of scope for a clean-room project. The
titles are catalogued under the *Call of Duty* group so that `library list`
shows them correctly when owned; to play them on Linux, use the Steam version
(Proton, if the anti-cheat allows it) or the official client on Windows.

## Third-party games on the Battle.net shop

The Battle.net shop sells some non-Blizzard games (`openblizz products --shop`
lists them, e.g. The Witcher 3: Wild Hunt Remastered). Their NGDP entries are
**placeholders**: an empty root (`root = 00000000000000000000000000000000`), a
39-byte install manifest and `client-version = 9.99.99`. The actual game data is
not delivered through the Blizzard CDN, so there is nothing OpenBlizz could
download, and such a purchase appears under `Purchases not mapped to an
installable product` after `library scan`.

None of these games is a Battle.net exclusive — they are also sold on Steam and
GOG, where they run on Linux through Steam's Proton, Heroic or Lutris. That is
the recommended route. If you already have such a game installed from another
store and want a single launcher, `openblizz launch` is a generic
umu/Proton/Wine runner and works for any Windows executable:

```bash
openblizz launch --directory "$HOME/Games/SomeGame" --exe "Game.exe" \
  --prefix "$HOME/Games/openblizz/somegame" --backend umu --proton GE-Proton
```

## Classic CD-key games (Diablo II, Lord of Destruction)

`d2-classic` and `d2-lod` are listed by the account page as classic licences
but are not on NGDP. Download the legacy installer from
<https://account.battle.net/games> (Blizzard's own download, not redistributed
by OpenBlizz), then run the installer and the game with the same runner:

```bash
# 1. run the installer you downloaded (file name as provided by Blizzard)
openblizz launch --directory ~/Downloads --exe "<installer>.exe" \
  --prefix ~/Games/openblizz/diablo2 --backend wine
# 2. run the game from where the installer put it inside the prefix
openblizz launch --directory "$HOME/Games/openblizz/diablo2/drive_c/Program Files (x86)/Diablo II" \
  --exe "Game.exe" --prefix ~/Games/openblizz/diablo2 --backend wine
```

## The runtime stack: umu, Proton, Wine

Every installed game is a Windows build; the runtime is provided by tools
OpenBlizz does **not** bundle:

| Layer | Provided by | Installed how |
|---|---|---|
| Steam Runtime container (pressure-vessel) | [umu-launcher](https://github.com/Open-Wine-Components/umu-launcher) | distribution package, zipapp, `.deb`/`.rpm` or Flatpak — see [RUNTIME.md](RUNTIME.md#installing-umu-launcher) |
| Proton (Wine + DXVK + vkd3d) | GE-Proton, UMU-Proton or any Proton directory | auto-downloaded by umu on first use (`--proton GE-Proton`, the default) |
| Plain Wine | your distribution's `wine` / `wine-staging` | `--backend wine` |
| x86-64 on ARM64 | Proton ARM64 + FEX | separate install, see [RUNTIME.md](RUNTIME.md#linux-aarch64-openblizz-versus-protonfex) |

`openblizz launch` sets `WINEPREFIX`, `PROTONPATH` and `GAMEID` for the
Proton/umu backends, `WINEPREFIX` for Wine, and runs the executable directly
for the native backend. Steam itself is not required; when you *do* want the
game inside Steam (Steam Deck, Big Picture, controller layouts), add
`openblizz` as a non-Steam game with the `launch ...` arguments as launch
options and leave Steam's compatibility setting off, as described in
[RUNTIME.md](RUNTIME.md#optional-adding-a-game-to-steam-steam-deck--big-picture).

## Reporting a wrong classification

If a game you own shows up in the wrong group, or `products --all` lists an
NGDP code under "Other Battle.net products" that you can identify, open an
issue with the output of `openblizz library list` and
`openblizz library scan --dump /tmp/scan.json` (remove personal data first).
The classification lives in `src/catalog.cpp` and is easy to extend.
