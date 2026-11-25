# Credits & Third-Party Notices

`hmmmgc-nx` is a Nintendo Switch port combining multiple open-source projects. Full credit belongs to the original authors and contributors of every upstream component listed below.

> [!IMPORTANT]
> No copyrighted Nintendo, Marvelous, or Natsume game assets or executables are distributed in this repository or its releases.

## Core Upstream Projects

| Component | Upstream Repository | Pinned Commit / Version | License |
| :--- | :--- | :--- | :--- |
| **HMMMGC** (`src/hmmmgc`) | https://github.com/RodoLy/HMMMGC | `91e43e276ba4fd1e8ce12ce678c407420ddc4dd0` | GPL-3.0 |
| **WiiCompiled** (`deps/wiicompiled`) | https://github.com/patchzyy/wiicompiled | `0d5c402d0366640a294bf9c699068299591f7eff` | GPL-3.0 (includes GPL-2.0-or-later & MIT components) |
| **Aurora** (`deps/aurora_patched`, `deps/aurora_sw`) | https://github.com/encounter/aurora | `3227d76c60e1e782ca576610bce61c9e7744d8be` | MIT (Copyright (c) 2022 Luke Street) |
| **Switch `deko` Aurora Backend** (`deps/deko`) | Adapted from SwitchWakerHD | Local Switch adaptation | MIT / MPL-2.0 |
| **deko3d** (`deps/deko3d`) | https://github.com/devkitPro/deko3d | devkitPro / fincs | Zlib (Copyright (c) 2018-2020 fincs) |
| **uam** (`deps/uam`) | https://github.com/devkitPro/uam | devkitPro / fincs | Zlib / MIT (with Mesa components) |
| **Dawn & Tint** (`deps/dawn_cfg`) | https://github.com/encounter/dawn | `266c1cf8de969a364afa4fa49311631fc99a881e` | BSD-3-Clause / Apache-2.0 / MIT |
| **SDL3** (`deps/sdl3_headers` + `deps/sdl3_shim`) | https://github.com/libsdl-org/SDL | `8e37db5e797b6167f3a00d697d816a684bd259c7` | Zlib (Copyright (c) 1997-2026 Sam Lantinga) |
| **Dear ImGui** (`deps/imgui`) | https://github.com/ocornut/imgui | `4806a1924ff6181180bf5e4b8b79ab4394118875` | MIT (Copyright (c) 2014-2025 Omar Cornut) |
| **xxHash** (`deps/xxhash`) | https://github.com/Cyan4973/xxHash | `e626a72bc2321cd320e953a0ccf1584cad60f363` | BSD-2-Clause (Copyright (c) 2012-2021 Yann Collet) |
| **SQLite3 + Horizon VFS** (`deps/sqlite_horizon.c`) | https://www.sqlite.org/ | Bundled via Aurora / Switch Horizon shim | Public Domain / MIT |
| **nlohmann/json** (`deps/nlohmann`) | https://github.com/nlohmann/json | Bundled header | MIT |
| **devkitPro / libnx** | https://github.com/switchbrew/libnx | Switch toolchain | Zlib / ISC |

## Additional Notices

- See `src/hmmmgc/packaging/native/THIRD_PARTY_NOTICES.txt` and `deps/wiicompiled/THIRD-PARTY-NOTICES.md` for additional transitive notices (including Dolphin-derived components, fmt, libpng, zlib, and DejaVu font notices).
- Switch port integration, testing, and supervision by **zxkzxx** (with Switch port code written by AI).
