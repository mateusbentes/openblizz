# Running games: Proton, umu, Wine and Steam

OpenBlizz downloads Windows builds (there are no native Linux builds of
Blizzard games). `openblizz launch` runs them through one of four backends:

| Backend | Command executed | When to use |
|---|---|---|
| `proton` (default) / `umu` | `WINEPREFIX=… PROTONPATH=… GAMEID=umu-openblizz umu-run GAME.exe …` | recommended: Proton (GE) with the Steam Runtime container, no Steam client needed |
| `wine` | `WINEPREFIX=… wine GAME.exe …` | system Wine (wine-staging recommended) |
| `native` | `GAME.exe …` | the executable runs by itself (macOS/Windows builds of OpenBlizz, or tests) |

## Installing umu-launcher

[umu](https://github.com/Open-Wine-Components/umu-launcher) (Unified Launcher
for Windows Games on Linux) runs any Proton build outside Steam, inside the
same pressure-vessel container Steam uses. `--proton GE-Proton` (the default)
makes umu download and keep the latest GE-Proton automatically on first use
(stored under `~/.local/share/Steam/compatibilitytools.d/`; the Steam Runtime container itself goes to `~/.local/share/umu`).

| Distribution | Install |
|---|---|
| Arch, Manjaro, EndeavourOS, CachyOS, Garuda | `sudo pacman -S umu-launcher` (official `multilib` repository; enable `[multilib]` in `/etc/pacman.conf`) |
| Fedora 41+, Nobara | `sudo dnf install umu-launcher` |
| NixOS / Nix (25.05+) | `environment.systemPackages = [ pkgs.umu-launcher ];` or `nix-shell -p umu-launcher`; older channels: the project flake (`github:Open-Wine-Components/umu-launcher?dir=packaging/nix`) |
| Debian, Ubuntu, Mint, Pop!_OS, openSUSE, Void, Gentoo, Solus, ... | download the **zipapp** (`umu-launcher-<ver>-zipapp.tar`) or the `.deb`/`.rpm` assets from the [latest release](https://github.com/Open-Wine-Components/umu-launcher/releases); for the zipapp: `tar xf umu-launcher-*-zipapp.tar && sudo install -m755 umu/umu-run /usr/local/bin/umu-run` (needs only Python 3.10+) |
| SteamOS / Steam Deck | zipapp into `~/.local/bin` (survives SteamOS updates, Python is preinstalled), or `pacman -S umu-launcher` inside the Arch Distrobox used for building |
| From source | `git clone --recurse-submodules https://github.com/Open-Wine-Components/umu-launcher && cd umu-launcher && ./configure.sh --user-install && make install` (installs `~/.local/bin/umu-run`) |

Make sure `~/.local/bin` is in `PATH` when using a user installation.

Check: `umu-run --version`.

### Ubuntu 26.04 (resolute)

Use the `ubuntu-resolute` assets from the umu release, not the `ubuntu-noble`
packages. The architecture-specific package contains the compiled `pyzstd`
module; using the noble package on resolute can fail with
`pyzstd module: Can't import compiled .so/.pyd file`.

```bash
cd ~/Downloads
wget https://github.com/Open-Wine-Components/umu-launcher/releases/download/1.4.4/python3-umu-launcher_1.4.4-1_amd64_ubuntu-resolute.deb
wget https://github.com/Open-Wine-Components/umu-launcher/releases/download/1.4.4/umu-launcher_1.4.4-1_all_ubuntu-resolute.deb
sudo dpkg -i python3-umu-launcher_1.4.4-1_amd64_ubuntu-resolute.deb \
  umu-launcher_1.4.4-1_all_ubuntu-resolute.deb
umu-run --version
```

The package may install `bwrap-userns-restrict-umu` and trigger an AppArmor
reload. If `apparmor.service` fails, identify the offending profile before
removing anything; keep Ubuntu's `bwrap-userns-restrict` profile enabled. Do
not copy the noble `.deb` to resolute.

On the first launch, umu downloads GE-Proton and Steam Runtime 4 under
`~/.local/share/Steam/compatibilitytools.d/` and `~/.local/share/umu/`.
Downloads are resumable; leave `.parts` files in place after a timeout. A
successful setup ends with lines similar to `Using steamrt4 (latest)` and
`Running 'GE-Proton...`.

## Choosing a Proton build

`--proton` is passed as `PROTONPATH`:

| Value | Meaning |
|---|---|
| `GE-Proton` (default) | latest GE-Proton release, auto-downloaded and auto-updated by umu |
| `GE-Proton9-27`, `GE-Proton10-4`, ... | a specific GE release (downloaded if missing) |
| `UMU-Proton` | umu's default: the latest stable Valve Proton with umu compatibility added |
| absolute path | any Proton directory, e.g. `"$HOME/.steam/root/steamapps/common/Proton - Experimental"` or `~/.local/share/Steam/compatibilitytools.d/GE-Proton9-27` |

## Linux AArch64: OpenBlizz versus Proton+FEX

These are two different layers:

1. **AArch64 OpenBlizz** is the native CLI binary published as
   `openblizz-linux-aarch64`. It handles login, account scanning, NGDP/TACT
   downloads, installation and verification directly on an ARM64 Linux host.
2. **Proton ARM64 + FEX** is the game runtime. FEX translates x86/x86-64
   Linux or Windows processes for ARM64, while Proton/Wine supplies the
   Windows compatibility layer. It is what can make an x86-64 Windows game
   such as Warcraft III run on an ARM64 Linux machine.

OpenBlizz does **not** bundle Proton, FEX, an x86-64 root filesystem, Steam
Runtime or game files. Its `launch` command only delegates to `umu-run` and
passes `WINEPREFIX`, `PROTONPATH`, `GAMEID` and the executable. Therefore an
ARM64 system needs an ARM64-capable umu/Proton+FEX stack installed separately.
The ordinary `GE-Proton` auto-download path is not a promise of an ARM64 build;
on ARM, pass the actual ARM64 Proton directory explicitly when required:

```bash
openblizz launch --backend umu \
  --proton "/path/to/Proton ARM64" \
  --directory "$HOME/Games/Warcraft3/x86_64" \
  --exe "Warcraft III.exe" \
  --prefix "$HOME/Games/openblizz/warcraft3" -- -launch
```

The exact Proton ARM64/FEX installation and rootfs setup depends on the ARM
distribution and the platform (for example, an ARM64 Steam/SteamOS stack).
Valve's Proton documentation requires an ARM64 build machine for ARM64 Proton
and FEX's documentation describes the x86/x86-64 translation layer. OpenBlizz
has not yet validated Warcraft III end-to-end on ARM64, so the AArch64 Release
means **native OpenBlizz support**, not a guarantee that every Windows game
runs under Proton+FEX.

## Warcraft III: Reforged

```bash
openblizz install w3 --directory ~/Games/Warcraft3 --locale ptBR
openblizz launch --directory ~/Games/Warcraft3/x86_64 --exe "Warcraft III.exe" \
  --prefix ~/Games/openblizz/warcraft3 -- -launch
```

- `-launch` is mandatory: without it the executable tries to hand over to the
  Battle.net app and exits.
- First launch takes longer (GE-Proton download, Steam Runtime 4 setup,
  prefix creation and shader cache). Login inside the game uses Blizzard's own
  in-game login; online play works as with the official installation.
- Verified on Ubuntu 26.04/KDE Wayland with umu-launcher 1.4.4,
  GE-Proton 11-7 and Steam Runtime 4: umu reported `Running 'GE-Proton11-7-x86_64'`
  and the process reached `fsync: up and running`.
- `--locale ptBR` stores the Brazilian Portuguese assets in addition to the
  mandatory enUS set; switch the language in the game's options.
- Saved games and settings: `<prefix>/drive_c/users/<user>/Documents/Warcraft III/`.

## Other products

The executable name differs per product; after `install` look inside the game
directory (`ls DIR DIR/x86_64`). StarCraft: Remastered also accepts `-launch`
to skip the Battle.net hand-off. Warcraft I/II Remastered are licence-only
titles (ownership comes from the purchase history). Diablo II / Lord of
Destruction (`d2-classic`, `d2-lod`) are classic CD-key games that are not
distributed through NGDP: use the legacy installer from your account page and
run it with `launch --backend wine` or umu.

## Adding a game to Steam (Steam Deck / Big Picture)

1. Install the game with OpenBlizz and run it once with `openblizz launch` so
   the prefix exists.
2. Steam → *Games* → *Add a Non-Steam Game to My Library* → *Browse* → pick
   `openblizz` (`/usr/local/bin/openblizz` or the build directory binary).
3. Right-click the new entry → *Properties*:
   - **Target**: path to `openblizz`
   - **Start in**: the game directory
   - **Launch options**:
     ```
     launch --directory /home/deck/Games/Warcraft3/x86_64 --exe "Warcraft III.exe" --prefix /home/deck/Games/openblizz/warcraft3 -- -launch
     ```
   - Leave *Compatibility* **unchecked** (umu provides Proton; forcing Steam's
     Proton on top would run Proton inside Proton).
4. Optional: rename the entry, add artwork with SGDBoop/Decky, map controller
   layouts.

Alternative without OpenBlizz in the loop — point Steam at the executable
directly and let Steam's Proton run it. *Target*: `"Warcraft III.exe"`,
*Launch options*: `-launch`, *Compatibility*: force a Proton version. Use a
different prefix than umu's or the game will see an unknown prefix layout.

## Desktop launcher (.desktop file)

```ini
[Desktop Entry]
Type=Application
Name=Warcraft III: Reforged
Exec=openblizz launch --directory /home/USER/Games/Warcraft3/x86_64 --exe "Warcraft III.exe" --prefix /home/USER/Games/openblizz/warcraft3 -- -launch
Path=/home/USER/Games/Warcraft3/x86_64
Icon=/home/USER/Games/Warcraft3/x86_64/Warcraft III.exe
Categories=Game;
```

Save as `~/.local/share/applications/openblizz-w3.desktop`.

## Performance and debugging

Variables in your environment are passed through to the game:

| Variable | Effect |
|---|---|
| `MANGOHUD=1` | FPS overlay (MangoHud installed) |
| `DXVK_HUD=fps,gpuload` | DXVK built-in overlay |
| `PROTON_LOG=1` | writes `steam-umu-openblizz.log` in `$HOME`; `UMU_LOG=1` for verbose umu output |
| `WINEDEBUG=-all` | silence Wine messages |
| `PROTON_USE_WINED3D=1` | OpenGL fallback when Vulkan is unavailable |
| `gamemoderun`, `gamescope -f -- ` | prefix the whole `openblizz launch` command |

Example: `MANGOHUD=1 gamemoderun openblizz launch ... -- -launch`.
