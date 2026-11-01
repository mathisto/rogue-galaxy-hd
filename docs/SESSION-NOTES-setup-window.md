# Session notes: the setup window, the release, the Setup and HD page (phase 2, 2026-11-01)

Branch `feature/setup-window` (from `feature/setup`, phase 1), local commits only, nothing pushed, no remote touched.
User docs: [INSTALLER.md](INSTALLER.md) ("The window", "The release"), the README's Install section, [SHARING.md](SHARING.md)
("The release zip"), [MENU-DESIGN.md](MENU-DESIGN.md) ("Setup and HD"). Phase 1's notes: [SESSION-NOTES-setup.md](SESSION-NOTES-setup.md).
The design source for the window and the release builder is the sibling project's installer (`totk-pc-recomp`, MIT, same
author); THIRD-PARTY-NOTICES.md says what was adapted.

## What was built

- **The window**: `tools/setup/rgsetup/gui.py` (tkinter, ttk `clam`, per-monitor DPI aware) over `guilogic.py` (catalog, form,
  event model, the CLI child with Ctrl+Break cancel). Form, consent (every download), run, finish. 107 `ui.*` ids in
  `text/en.json`, the same catalog as the log. Drag and drop of the disc (`WM_DROPFILES`), Enter does the main thing, the form
  scrolls inside the window on a small screen, an unexpected error in the window is written to
  `%TEMP%\rg-setup-logs\window-error.log` and explained. `--selftest [--shots DIR] [--scale 1.5]`, and a developer driver
  `--live review|run|cancel` that drives the real window against the real CLI.
- **The launcher**: `RogueGalaxy-Setup.exe` (`tools/setup/launcher/setup_launcher.c`, 60 lines of C, static, no console;
  icon and manifest through `src/launcher/make_resources.py`, because windres is quarantined by some antivirus programs). Icon:
  the game launcher's ringed planet with a download arrow, drawn by `tools/setup/make_icon.py` from nothing.
- **The release**: `tools/make_release.py [--zip]` and `tools/check_content.py`; `tools/release/source_zip.py` kept working and
  now exposes `iter_source` (the release builder uses it). `setup.py` builds from `deps/` when a release has it
  (`FETCHCONTENT_FULLY_DISCONNECTED`). `rgsetup/data/deps.json` pins the nine sources CMake would download.
- **F1 > Setup and HD** (`ui_pages.cpp`, `src/setup_info.cpp`, `HostServices::setupInfo`/`runSetup`).
- **A phase-1 fix found by the window**: `hostcheck.game_processes` counted a game running from *another* folder (the owner was
  playing) and refused the build. It now counts the game or launcher under the install folder, and a process of the game's name
  whose path it cannot read; a copy elsewhere is not in the way (test added).

## Decisions

- **No separate "runtime" or toolchain step.** The release unpacks the pinned tools into `toolchain/` with the stamps the setup
  writes, so `ToolchainStep.current` is true and nothing in `layout.py` changed. numpy and Pillow are bundled in
  `toolchain/pylibs` (not a download: the owner's list of downloads was Real-ESRGAN, FFmpeg and chdman).
- **Install folder = the folder the release is in**, as in phase 1 (the build happens where the program is). The window's
  **Change...** copies the program (1.3 GiB, never user data) to a new empty folder and opens the setup from the copy.
- **Update = unpack the newer zip over the folder, run the setup**; the build runs again (every source file is newer), the
  extraction and the HD packs are kept.
- **Window title / product name**: "Rogue Galaxy Recompiled Setup" (`ui.title`); the owner may want another.
- **Time claims** are always "on the author's PC (Ryzen 9 7900X, RTX 5070 Ti)"; the build figure in the README (20 to 25
  minutes) is measured below.
- Copy was written in the impeccable skill's clarify mode (Operate surface, no PRODUCT.md, nothing invented: no claims beyond the
  measured numbers); `impeccable detect` returns nothing for a Tk file, so the checks were measured (below).

## Verified (this PC; the owner's game was running from 3:50 PM, so no window was shown after that and nothing in `build\game` was relinked)

PENDING_VERIFIED

## Not verified

PENDING_NOT_VERIFIED

## Cleaned up

PENDING_CLEAN
