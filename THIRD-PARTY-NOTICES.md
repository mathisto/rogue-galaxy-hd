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
| **numpy**, **Pillow** | `Build-HD-Pack.bat` (pip), into `toolchain/pylibs`, for the HD pack tools | BSD-3-Clause, MIT-CMU (HPND) | Used by the Python tools in `tools/hd/`; not distributed with the project. |

## Downloaded by the HD pack builder (`Build-HD-Pack.bat`), on request

The builder shows each address and asks before it downloads, checks the
SHA-256 printed in `tools/hd/build_hd_pack.py`, and puts the result in
`toolchain/`. Neither is redistributed by this project.

| Component | Licence | Notes |
|---|---|---|
| **Real-ESRGAN ncnn-vulkan** 20220424, Windows portable build, from <https://github.com/xinntao/Real-ESRGAN/releases/tag/v0.2.5.0> | Real-ESRGAN (the models and training code): BSD-3-Clause, Copyright (c) 2021 Xintao Wang. Real-ESRGAN-ncnn-vulkan: MIT, Copyright (c) 2021 Xintao Wang. ncnn: BSD-3-Clause, Copyright (C) 2017 Tencent. | The zip holds `realesrgan-ncnn-vulkan.exe`, the models `realesr-animevideov3`, `realesrgan-x4plus` and `realesrgan-x4plus-anime`, and Microsoft's `vcomp140.dll` (the Visual C++ OpenMP runtime, redistributable). The zip carries no licence file of its own, so the licences above are those of the three projects' repositories. The upscaled pictures it produces are derived from your disc and are not part of this project. |
| **FFmpeg** 8.1.2 "essentials" build by gyan.dev, from the `GyanD/codexffmpeg` release 8.1.2, only if no `ffmpeg` is on your PATH | GPL-3.0 build (includes x265 and NVENC support) of FFmpeg, which is LGPL-2.1-or-later with GPL parts | A separate program the builder runs; it is not linked into the game and is not redistributed here. |

## Modified code and how to get the source

The changes to the vendored and patched projects are in this repository:
`thirdparty/dq8-gfx` (modified copy), `patches/ps2recomp/*.patch`, and
`src/` for the game itself. The GPL-3.0 text is in [`LICENSE`](LICENSE). If you
distribute a build of the game, give the source of exactly that build
(`tools/release/source_zip.py` makes the source zip of a release).
