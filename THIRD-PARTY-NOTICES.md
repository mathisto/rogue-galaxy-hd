# Third-party notices

Rogue Galaxy Recompiled is released under the **GNU General Public License
version 3** (see [`LICENSE`](LICENSE)). It is built on, and uses, the work of
others, listed here with the licence each carries. Each licence below was read
from the licence file of the project itself (in this repository where it is
vendored, otherwise from the project's own repository on 2026-10-29); where a
project has no licence file of its own this says so.

This repository does not contain any of the game's data, any BIOS, or anything
made from them. The game, its name, art, music and movies belong to Level-5 and
Sony Computer Entertainment; this project is not affiliated with or endorsed
by them or by any project named below.

## In this repository

| Component | Where | Licence | Notes |
|---|---|---|---|
| **PS2Recomp** by ran-j, and the fork by Sinan Karakaya | `thirdparty/PS2Recomp` (git submodule of the fork); `patches/ps2recomp/` are patches to it | GPL-3.0 | `thirdparty/PS2Recomp/LICENSE` is the GNU GPL version 3. The built game links its runtime, so the whole is GPL-3.0. |
| **DQ8-Recompiled** (Sinan Karakaya): the `dq8-gfx` renderer, GS emulation on the GPU, and the in-game menu | `thirdparty/dq8-gfx` (vendored, with this project's changes) | GPL-3.0 | `thirdparty/dq8-gfx/LICENSE` is the GNU GPL version 3. Its window-icon file is replaced here by an empty one (`window_icon.cpp`), so DQ8's icon is not used. |
| **Dear ImGui** by Omar Cornut | `thirdparty/imgui` (git submodule, pinned commit) | MIT | `thirdparty/imgui/LICENSE.txt`: "The MIT License (MIT), Copyright (c) 2014-2026 Omar Cornut". |
| **stb_truetype** by Sean Barrett (Dear ImGui's copy, `imstb_truetype.h`, v1.26) | `thirdparty/imgui`; used by `src/hd_art.cpp` to draw the modern font | Public domain, or MIT (your choice) | The licence block at the end of the header offers both. |
| **Roboto Medium** (`Roboto-Medium.ttf`) by Christian Robertson, Google | `thirdparty/imgui/misc/fonts`; embedded in the build as the menu font and the default modern in-game font | Apache License 2.0 | The font file's own name table reads "Copyright 2011 Google Inc. All Rights Reserved. Licensed under the Apache License, Version 2.0, http://www.apache.org/licenses/LICENSE-2.0". Dear ImGui's `docs/FONTS.md` credits it. The font is only embedded in executables you build yourself. |
| **RGDecomp** by Dorgengoa | `config/SCUS_974.90/functions_*.csv` derive from its function maps and names | CC0 1.0 (public domain dedication) | The repository's `LICENSE` is "Creative Commons Legal Code, CC0 1.0 Universal". No attribution is required; it is given anyway. |
| **AMD FidelityFX Super Resolution 1.0** (EASU and RCAS) | `thirdparty/dq8-gfx/src/gfx/backends/sdlgpu/shaders/present_easu.frag`, `present_rcas.frag`: written out again in plain GLSL | MIT, Copyright (c) 2021 Advanced Micro Devices, Inc. | Stated in the headers of both shader files. |
| **PCSX2** (GameDB, widescreen and 60 fps patch notes) and **Red-tv's 60 fps pnach** for the PAL release | Facts only (addresses, field names, what the patch changes) in `docs/sixty-fps-logic.md` and comments; no PCSX2 code and no pnach text | PCSX2: GPL-3.0. The pnach: no licence stated where it was read; not checked further | Used as a reference and credited; nothing is copied. |
| **totk-pc-recomp installer** (`installer/`: `events.py`, `state.py`, `runner.py`, `cli.py`) by this project's author | `tools/setup/rgsetup/events.py`, `state.py`, `runner.py` and `cli.py` follow its structure (JSON progress events, step keys that make resume and update one rule, a text catalog of ids, exit codes 0/1/2/3 with cancel by Ctrl+Break) and adapt parts of its code; the rest is written for this project | MIT (the author's own project; the combination here is GPL-3.0) | Credited as the design source; no other code from it. |
| **totk-pc-recomp installer window and release builder** (`installer/gui.py`, `guilogic.py`, `launcher/launcher.c`, `tools/make_release.py`, `tools/check_content.py`) by this project's author | `tools/setup/rgsetup/gui.py` and `guilogic.py` (the screens, the event model, the child process with Ctrl+Break cancel, the self test with screenshots), `tools/setup/launcher/setup_launcher.c`, `tools/make_release.py` (the release folder and zip) and `tools/check_content.py` (the content scanner) adapt their structure and parts of their code; the screens, the text, the consent page, the per-component choice, the drag and drop and the scanner's disc checks are written for this project | MIT (the author's own project; the combination here is GPL-3.0) | Credited as the design source; no other code from it. |
| **Redump** | Disc size and hashes in `docs/DISC.md` and `config/SCUS_974.90/hashes.json` | Facts about a disc image | No data from the disc itself. |

The launcher's icon (`src/launcher/rogue-galaxy.ico`) is drawn by
`src/launcher/make_icon.py` "from nothing (no game art)". The Xbox-style
button prompts are drawn in code (`src/hd_art.cpp`, `drawXboxIcons`: circles,
letters in Roboto, bars and outlines; no picture file and nothing taken from
the game or from Microsoft's art), and `src/button_art_table.inc` lists only
boxes (positions in the game's own pictures), never pixels.

## Fetched or downloaded when you build or run (not in this repository)

| Component | Fetched by | Licence | Notes |
|---|---|---|---|
| **SDL3** release-3.2.24 | CMake (`thirdparty/dq8-gfx/CMakeLists.txt`) | zlib | `LICENSE.txt` of libsdl-org/SDL. |
| **FFmpeg** n7.1, LGPL shared build by System233 (`ffmpeg-msvc-prebuilt`), SHA-256 pinned | CMake (`ps2xRuntime`), DLLs end up next to the game | LGPL-2.1-or-later for FFmpeg; MIT for System233's repository | FFmpeg's `LICENSE.md`: "Most files in FFmpeg are under the GNU Lesser General Public License version 2.1 or later". The LGPL build (no `--enable-gpl`) is linked dynamically. |
| **glslang** 15.4.0 and **SPIRV-Cross** vulkan-sdk-1.4.321.0 | CMake, only when no GLSL compiler is installed | glslang: BSD-3-Clause for most files and other permissive licences for some (its `LICENSE.txt` lists them); SPIRV-Cross: Apache-2.0 | Build-time tools for the shaders; not part of the game. |
| **sse2neon** v1.9.1 | CMake, ARM builds only | MIT | Not used on Windows x86-64. |
| **raylib** 5.5 (and, with the debug UI, rlImGui) | CMake, from PS2Recomp's runtime project | zlib | Not examined further: the game's window and renderer are SDL3 and `dq8-gfx`. |
| **llvm-mingw / LLVM**, **CMake**, **Ninja**, **MinGit**, **Python** (python-build-standalone) | `Install.bat`, into `toolchain/`, SHA-256 pinned | Apache-2.0 with LLVM exceptions, BSD-3-Clause, Apache-2.0, GPL-2.0, PSF | Build tools; nothing from them is in the game. |
| **numpy**, **Pillow** (13.0.0 or later) | the setup program or `Build-HD-Pack.bat` (pip), into `toolchain/pylibs`, for the HD pack tools, after a yes | BSD-3-Clause, MIT-CMU (HPND) | Used by the Python tools in `tools/hd/`; not distributed with the project. Pillow 13.0.0 is the floor because older releases have known critical vulnerabilities. |

## Bundled in the Windows release zip (`tools/make_release.py`)

`RogueGalaxy-Setup-<version>-windows.zip` carries these pinned files so that installing the game downloads nothing. The pins
(addresses, SHA-256 of each archive, commits) are in `tools/setup/rgsetup/data/toolchain.json` and `deps.json`, and
`release.json` in the zip lists what was bundled. None of it is part of the game's own code except where a library is linked
into it (SDL3, raylib: zlib licence), which the built game's source offer below covers.

| Component | Licence | Notes |
|---|---|---|
| **MinGit** 2.56.0 (Git for Windows), from <https://github.com/git-for-windows/git/releases/tag/v2.56.0.windows.2> | GPL-2.0 (Git), with LGPL and other parts as in its own `LICENSE` | Used by the build for `git apply` of the patches. **Source for it:** the same release's source archive and <https://github.com/git-for-windows/git>, at that tag; this project makes no change to it. |
| **Python** 3.12.13, python-build-standalone 20260807 (Astral), with Tcl/Tk (tkinter) | PSF-2.0; Tcl/Tk: the Tcl licence (BSD-style) | Runs the setup, its window and the build scripts. |
| **CMake** 3.31.12, **Ninja** 1.13.2 | BSD-3-Clause; Apache-2.0 | Build tools. |
| **llvm-mingw** 20260616 (LLVM 22.1.8, mingw-w64, libc++) by Martin Storsjo | Apache-2.0 with LLVM exceptions; mingw-w64 and others permissive as in its `LICENSE.TXT` | The compiler the game is built with. |
| **numpy**, **Pillow** 13.0.0 (installed with the bundled pip into `toolchain/pylibs`) | BSD-3-Clause and others; MIT-CMU (HPND) | For the HD builder, which is where the Pillow floor of the note above comes from. |
| Dependency sources in `deps/` (no `.git`): **SDL3** release-3.2.24, **raylib** 5.5 (Zlib); **glslang** 15.4.0 (BSD-3-Clause and others); **fmt** 12.1.0, **toml11** v4.4.0, **ELFIO** Release_3.12, **nlohmann/json** v3.11.3, **rabbitizer** 1.14.3 (MIT); **libdwarf** 2.2.0 (LGPL-2.1 for the library, others per file) | as listed | What CMake would download while building; the recompiler tool (fmt, toml11, ELFIO, nlohmann/json, rabbitizer, libdwarf) is built on the player's PC and is not part of the game. libdwarf is only linked into that tool. |

## Downloaded by the HD pack builder (`Build-HD-Pack.bat`), on request

The builder shows each address and asks before it downloads, checks the
SHA-256 printed in `tools/hd/build_hd_pack.py`, and puts the result in
`toolchain/`. Neither is redistributed by this project.

| Component | Licence | Notes |
|---|---|---|
| **Real-ESRGAN ncnn-vulkan** 20220424, Windows portable build, from <https://github.com/xinntao/Real-ESRGAN/releases/tag/v0.2.5.0> | Real-ESRGAN (the models and training code): BSD-3-Clause, Copyright (c) 2021 Xintao Wang. Real-ESRGAN-ncnn-vulkan: MIT, Copyright (c) 2021 Xintao Wang. ncnn: BSD-3-Clause, Copyright (C) 2017 Tencent. | The zip holds `realesrgan-ncnn-vulkan.exe`, the models `realesr-animevideov3`, `realesrgan-x4plus` and `realesrgan-x4plus-anime`, and Microsoft's `vcomp140.dll` (the Visual C++ OpenMP runtime, redistributable). The zip carries no licence file of its own, so the licences above are those of the three projects' repositories. The upscaled pictures it produces are derived from your disc and are not part of this project. |
| **FFmpeg** 8.1.2 "essentials" build by gyan.dev, from the `GyanD/codexffmpeg` release 8.1.2, only if no `ffmpeg` is on your PATH | GPL-3.0 build (includes x265 and NVENC support) of FFmpeg, which is LGPL-2.1-or-later with GPL parts | A separate program the builder runs; it is not linked into the game and is not redistributed here. |

## Downloaded by the setup program (`python -m rgsetup`, `Install.bat`), on request

Only for a CHD disc image, and only after the plan has listed it and the player said yes (`--yes`): the address, size,
SHA-256 and licence are in `tools/setup/rgsetup/download.py`; a file whose SHA-256 differs is deleted and nothing is unpacked.

| Component | Licence | Notes |
|---|---|---|
| **chdman** from the **MAME** 0.289 Windows release (`mame0289b_x64.exe`, 87,626,249 bytes, SHA-256 `a1aa7912168c9d1b05e611906bc21b8b9be3935822aead36d12a1da363150b7d`), from <https://github.com/mamedev/mame/releases/tag/mame0289> | chdman: BSD-3-Clause (the header of `src/tools/chdman.cpp`, "license:BSD-3-Clause", Aaron Giles). MAME as a whole: GPL-2.0-or-later (and others per file) | The setup reads the self-extracting archive with the Windows `tar` without running it, keeps `chdman.exe` and `COPYING` in `toolchain/chdman`, and runs chdman as a separate program to turn a CHD into an ISO. It is not linked into the game and is not redistributed here. |

## Ideas credited, no code taken

The ultrawide HUD (`src/ultrawide.cpp`, [docs/ULTRAWIDE.md](docs/ULTRAWIDE.md)) is written for this game's own code. Its ideas
come from projects that solved the same problem for other games; **no code, no data and no file of theirs is in this
repository**, so none of their licences apply to it. They were read on their public pages (2026-11-04) and are named here because the
approach is theirs first: **gta-vcs-recomp** (GPL-3.0: a HUD squeezed toward an anchor after snapping to the console's pixels, the
scissor squeezed the same way), **KillzoneRecomp** (GPL-3.0: the 2D drawn into a centred 16:9 box, full-width rectangles kept
whole, a field of view in tangent space), **GT4Recomp** (MIT: a 16:9 HUD block, menus and movies kept 4:3, nothing written at
16:9 so the picture is byte-identical), **takedawn** (MIT: a per-frame wide flag, left and right parts anchored to the visible
edge), and **Zelda64Recomp** with **RT64** (GPL-3.0, MIT: a left, centre or right origin for each piece of the HUD).

## Modified code and how to get the source

The changes to the vendored and patched projects are in this repository:
`thirdparty/dq8-gfx` (modified copy), `patches/ps2recomp/*.patch`, and
`src/` for the game itself. The GPL-3.0 text is in [`LICENSE`](LICENSE). If you
distribute a build of the game, give the source of exactly that build
(`tools/release/source_zip.py` makes the source zip of a release).
