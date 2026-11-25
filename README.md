# HMMMGC-NX

Native Nintendo Switch port of **Harvest Moon: Magical Melody** (GameCube, NTSC-U `G4AEE9`) via static recompilation.

> [!IMPORTANT]
> hmmmgc-nx does *not* provide any copyrighted assets. You must provide your own copy of the original game.

> [!NOTE]
> The Nintendo Switch porting code, patches, and build integration in this repository were written entirely by AI, and tested and supervised by a human (built on top of the open-source upstream projects listed in [Upstream Projects & Credits](#upstream-projects--credits)).

## Features

- **Native AArch64 static recompilation** -- game code is translated to C++ ahead of time and runs natively on Switch with no emulator in between
- **Native 720p (1280x720)** rendering via **deko3d** (`Aurora` GX backend)
- **Native 30 FPS**
- **Native AX DSP audio**
- **GameCube Memory Card (`.gci`)** save & load support (compatible with Dolphin `.gci` saves)
- **Direct disc loading** from uncompressed `.iso` / `.gcm` images or extracted disc folders

## Hardware & Tested On

- **Tested on Nintendo Switch Lite (Handheld mode)** only.
- Docked mode and other Nintendo Switch revisions have not been tested yet.

## In-Game Options Menu & Mods

Press **`-` (Minus)** during gameplay to open or close the in-game Options Menu overlay (Dear ImGui). While the menu is open, the game does not receive controller input. Changes are saved automatically to `sdmc:/switch/hmmmgc-nx/settings.json`.

Available options include **Picture** (widescreen, FPS counter), **Volume**, **Language** (English, Spanish, French, German, or Italian dialogue from the disc), **Controls**, and **Mods**.

All compiled-in mods from [RodoLy/HMMMGC](https://github.com/RodoLy/HMMMGC) are off by default and can be toggled from the menu:
- **Story / Marriage**: *Marry Jamie*, *Marry Anyone* (experimental)
- **Quality of Life**: *Faster character*, *No slowdown when carrying*, *Farther camera*, *Longer days*, *Gentler stamina*, *Faster tools*
- **Cheats** (via `settings.json`): *infinite stamina*, *infinite money*, *stop time*, *maximum friendship*

## Installation on Nintendo Switch

1. Download the latest release `.zip` from the GitHub Releases page and extract it to the root of your Switch SD card.
2. Place your own legally dumped **uncompressed** GameCube `.iso` or `.gcm` (`G4AEE9`, NTSC-U) inside `sdmc:/switch/hmmmgc-nx/` (or an extracted `game/` folder containing `sys/` and `files/`).
   - Compressed formats (`.rvz`, `.gcz`, `.wia`, `.ciso`) are not supported directly.

Example SD card layout:

```text
sdmc:/
`-- switch/
    `-- hmmmgc-nx/
        |-- hmmmgc-nx.nro
        `-- game.iso        # any .iso/.gcm, or an extracted game/ folder
```

3. Launch Homebrew Menu using **Title Override (Full RAM)** by holding `R` while launching any installed title. **Do not launch in Applet Mode** (Album).
4. Runtime logs are written to `sdmc:/switch/hmmmgc-nx/native.log`.

## Building from Source

Requires **devkitPro** (`devkitA64`, `libnx`, `switch-zlib`, `switch-libpng`, `switch-tools`), **CMake 3.28+**, **Python 3**, **.NET 8 SDK**, and your own extracted `G4AEE9` disc at `src/hmmmgc/orig/G4AEE9/`.

```bash
# 1. Load devkitPro Switch environment & translate your main.dol
source env_switch.sh
./run_translate.sh

# 2. Configure & build stages in order using $DEVKITPRO/cmake/Switch.cmake:
#    stage4b_uam -> stage4c_dawn -> stage4d_gfxtest (cos_switch_deko) ->
#    stage5_sdltest -> stage6_hmmmgc -> stage6b_runtime -> stage6c_nro (hmmmgc_nro)
cmake -S <stage_dir> -B <stage_dir>/build -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake"
cmake --build <stage_dir>/build -j 4

# Final NRO target:
cmake --build stage6c_nro/build --target hmmmgc_nro -j 4
# Output: stage6c_nro/build/hmmmgc-nx.nro
```

## Support & Issues

- Reported performance is based solely on the device currently used for testing (**Nintendo Switch Lite**).
- This is a hobby/community port provided **as-is**; updates are only made when there is feedback and free time.
- If you open an Issue, please attach `sdmc:/switch/hmmmgc-nx/native.log` along with your Switch model, CFW/firmware version, and game region.

## Upstream Projects & Credits

This port is built on top of existing open-source projects:
- **[RodoLy/HMMMGC](https://github.com/RodoLy/HMMMGC)** -- Native PC runtime and mods for *Harvest Moon: Magical Melody* (GPL-3.0)
- **[patchzyy/wiicompiled](https://github.com/patchzyy/wiicompiled)** -- PowerPC-to-C++ static translator and runtime ABI (GPL-3.0)
- **[encounter/aurora](https://github.com/encounter/aurora)** -- GameCube/Wii GX, CARD, and PAD implementation (MIT)
- **`deko` backend (adapted from SwitchWakerHD)** + **[deko3d](https://github.com/devkitPro/deko3d)** & **[uam](https://github.com/devkitPro/uam)** -- Native Switch GPU rendering & DKSH shader compilation
- **[encounter/dawn](https://github.com/encounter/dawn)** (Tint WGSL -> GLSL), **SDL3** (with custom libnx shim), **Dear ImGui**, **xxHash**, **SQLite3**, and **devkitPro / libnx**

See `CREDITS.md` for full attribution, pinned upstream commits, and third-party license notices.

## License

Licensed under the **GNU General Public License v3.0** -- see `LICENSE` and `CREDITS.md`.

This project is an unofficial community port and is **not affiliated with, endorsed by, or associated with Marvelous Inc., Natsume Inc., or Nintendo**. All trademarks, copyrights, and game assets belong to their respective owners.
