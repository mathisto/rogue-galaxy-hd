# Session notes: the setup window, the release, the Setup and HD page (phase 2, 2026-11-01)

Branch `feature/setup-window` (from `feature/setup`, phase 1), local commits only, nothing pushed, no remote touched.
User docs: [INSTALLER.md](INSTALLER.md) ("The window", "The release"), the README's Install section, [SHARING.md](SHARING.md)
("The release zip"), [MENU-DESIGN.md](MENU-DESIGN.md) ("Setup and HD"). Phase 1's notes: [SESSION-NOTES-setup.md](SESSION-NOTES-setup.md).
The design source for the window and the release builder is the sibling project's installer (`totk-pc-recomp`, MIT, same
author); THIRD-PARTY-NOTICES.md says what was adapted.

## What was built

- **The window**: `tools/setup/rgsetup/gui.py` (tkinter, ttk `clam`, per-monitor DPI aware) over `guilogic.py` (catalog, form,
  event model, the CLI child with Ctrl+Break cancel). Form, consent (every download), run, finish. About 110 `ui.*` ids in
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
- **F1 > Setup and HD** (`ui_pages.cpp`, `src/setup_info.cpp`, `HostServices::setupInfo` / `runSetup`).
- **Two phase-1 faults the new work found**: a game running from *another* folder (the owner was playing) made the build refuse
  (`hostcheck.game_processes` now counts the game or launcher under the install folder, and a process of the game's name whose
  path it cannot read; test added); and the build step recorded its key before the build patched the sources, so the next run
  rebuilt (it records the key of the tree it leaves now; test added).

## Decisions

- **No separate "runtime" or toolchain step.** The release unpacks the pinned tools into `toolchain/` with the stamps the setup
  writes, so `ToolchainStep.current` is true and nothing in `layout.py` changed. numpy and Pillow are bundled in
  `toolchain/pylibs` (not a download: the owner's list of downloads was Real-ESRGAN, FFmpeg and chdman).
- **Install folder = the folder the release is in**, as in phase 1 (the build happens where the program is). The window's
  **Change...** copies the program (1.3 GiB, never user data) to a new empty folder and opens the setup from the copy.
- **Update = unpack the newer zip over the folder, run the setup**; the build runs again (every source file is newer), the
  extraction and the HD packs are kept.
- **Time claims** are always "on the author's PC (Ryzen 9 7900X, RTX 5070 Ti)".
- Copy was written in the impeccable skill's clarify mode (Operate surface, no PRODUCT.md, nothing invented beyond measured
  numbers). `impeccable detect` returns nothing for a Tk file, so the checks were measured (item 3 below).

## Verified (this PC; the owner's game was running from 3:50 PM, so no window was shown after that, and `build\game` was never relinked)

1. **Phase 1's gap, closed: a real clean install through the new CLI.** A clean `git clone` of this branch at `Q:\work\rgsw1`,
   `python -m rgsetup --dest ... --source "<the real ISO>" --preset faithful --yes --gentle` (the PC's own Python 3.13, no HD part):
   exit 0 in **1488 s (24.8 min)** at below-normal priority while other work ran: source 0.5 s, extract 22 s, toolchain 179 s (the
   five pinned downloads, SHA-256 checked), build **1285 s** (submodules 54 s, the recompiler about 7.5 min, then the recompile and
   the compile of 908 files). Nothing broke, so nothing needed fixing there. `roguegalaxy.exe` 123,566,592 bytes, `Rogue Galaxy.exe`
   184,320. **Booted** through the real launcher (`Rogue Galaxy.exe`) with the hidden window (`DQ8_GFX_HIDDEN_WINDOW=2`), a *copy* of
   the memory card (`disc\mc0` and `mc1`, 3 files), a scratch `settings.ini`, `RG_MUTE=1`: four screenshots at 20, 35, 55 and 80 s,
   the last showing the **title screen (NEW GAME / CONTINUE / OPTIONS)**; the log has `[overlay] load #1: title`, and the scripted
   `quit` at 90 s ended it cleanly. (The screenshots are the game's pictures; they stayed in the scratch folder, which is deleted.)
2. **Unit tests**: 160 in `tests/test_setup_*.py` (about 15 s), all pass: phase 1's 140, the window's logic and text (12), the
   content scanner (6), and the build-key and game-process tests added this session. The window's `--selftest` is one of them.
3. **The window, every screen, at 100 % and 150 %** (`--selftest --shots`, then looked at: form, parts open, no usable graphics card,
   an error, an installed folder, consent with two downloads, run, run with the log open, finished, failed, cancelled). Fixes from
   looking: a cut-off "Recommended", the HD reason repeated three times (now once, under the presets), "C::" in the disk message
   (`err.space` reworded), a form taller than a laptop screen (it scrolls inside the window now). Measured, since the detector only
   reads web files: every text pair is 4.9:1 or better (muted text 5.8:1, white on the accent 8.7:1; disabled text 3.5:1, exempt), the
   smallest text is 9 pt (12 px at 100 %), buttons are about 30 px high at 100 % (above the 24 px floor, below 44), no horizontal
   overflow at 100 % or 150 %. The screenshots come from `PrintWindow` on a fully transparent, never-activated window, so the check
   never touched the player's focus.
4. **The window against the real CLI** (`--live`, from the unpacked release, with the real ISO, buttons pressed with `invoke()`):
   *review* (probe, then the consent page: the one download was Real-ESRGAN; nothing for the game); *cancel* (Cancel pressed 5 s into
   the extraction: exit 3, "Setup cancelled", the state kept); *run* with Faithful: **the whole game built from the unpacked release,
   offline** (`FETCHCONTENT_FULLY_DISCONNECTED=ON` and `FETCHCONTENT_SOURCE_DIR_*` in both CMake caches, no dependency was cloned):
   source 1.1 s, extract 44 s, **build 645 s (10.8 min)**, exit 0, "Rogue Galaxy is ready". With `RG_SETUP_FAKE_VULKAN` set to `none`
   or `software` (in a folder without HD parts), the HD parts are greyed out with the reason and the preset falls back to Faithful; with a
   fake RTX card they are offered.
5. **The release**: `python tools/make_release.py --zip` built `RogueGalaxy-Setup-0.1.0-3879af1-windows.zip`: **413.9 MiB** (433,971,987 bytes; the GitHub limit is
   2 GiB), **1347 MiB unpacked** (toolchain 1102 MiB, deps 212, thirdparty 29, the rest under 3), SHA-256 `999d1d216352c150ce86b43a9469a34415e162c15b6cd02a0d2f642719d83030`.
   `check_content` ran over all 31,597 files with the disc's 41 byte runs: no hit. It caught two real leaks on the way, the
   console-script launchers in `pylibs/bin` that embed the builder's Python path, and an e-mail address in the script itself; both
   fixed. The bundled Python (3.12.13 with tkinter 8.6) ran the window's self test and the setup's own check, and the setup recognised
   the unpacked toolchain: no download for the game in the plan.
6. **From the unpacked zip, with the real ISO**: `Setup-Console.cmd --estimate` (only Real-ESRGAN in the plan; numpy and Pillow are
   bundled); then the trial: `--components textures,movies,prompts_font --limit 6 --limit-movies 1 --gentle --yes`, exit 0 in 745 s:
   a 425 s incremental rebuild (see "Bugs"), **HD textures trial 290 s (4 pictures upscaled)**, **HD movie trial 30 s (1 movie)**,
   the only download Real-ESRGAN (FFmpeg was on PATH), the GPU busy for seconds at the lowest priority. No full upscale ran.
7. **The F1 page** (`build\game-dev\rg-setupui.exe`, linked from the objects of `build\game` without relinking
   `build\game\src\roguegalaxy.exe`; no game was running when it was linked): hidden-window runs from the repository folder with a
   scratch settings file and card, `RG_UI_SCRIPT` `settings setup`, `rowcheck`: **0 of 6 rows overlap** at 1280x720, 1920x1080,
   1280x960 and 960x540, and at 1280x720 and 960x540 with Menu size 200 %. The screenshots show "11,072 pictures", "89 of 89",
   Original, Auto, "Not recorded", and the button disabled (this checkout has no `RogueGalaxy-Setup.exe`). The one open item: a row's
   value sat 6 px above its label; `AlignTextToFramePadding` fixed it and was re-checked.
8. **Short paths**: the setup's own rule fired on my first release folder (`build\release\RogueGalaxy-Setup`, 63 characters, limit 60:
   `err.path_long`), which is why the release check ignores `err.path_*` errors and why the README says "a short folder".
9. **The final zip itself** (`...-3879af1-windows.zip`, unpacked fresh into `Q:\work\rgrel2`): the window's `--live run` with Faithful
   and the real ISO built the whole game offline, **exit 0, "Rogue Galaxy is ready"**: source 0.5 s, extract 15 s, build **826 s**
   (13.8 min, with the owner's game running and my other work competing, at below-normal priority). **Then `--estimate` on the same
   folder: `plan.run` is `check,finish`, `build=current`**, i.e. a second run does nothing (the key fix, on the real first-run path).
   The game built there, run hidden from that folder, shows F1 > Setup and HD with "Not installed" for both HD parts, Original,
   Auto, "Setup 0.1.0" (read from `rg-install.json`) and **Open the setup enabled** (`RogueGalaxy-Setup.exe` is there);
   `rowcheck`: 6 rows, 0 overlapping. Four of the window's screens are in `docs/images/setup-*.png` (INSTALLER.md).

## Bugs

- **The second run rebuilt** (found in item 6): the build applies `patches/` to `thirdparty/PS2Recomp`, and the build step's key was
  taken over the sources *before* that, so the next run (add HD parts later, update) saw a new key and ran `setup.py` again: 47
  files and 7 minutes here. `BuildStep.run` records the key of the tree it leaves now (unit test). Verified on the unpacked release
  after the patches were in place (`--estimate` then shows `build=current`); the first-run recompute is covered by the unit test.
- **A game running from another folder blocked the build** (the owner's game, while the window installed into a scratch folder):
  fixed in `hostcheck.game_processes`, with a test using a copy of `ping.exe` named `roguegalaxy.exe`.
- One slip of mine: the first `Get-Process | where Path -like '*rogue-galaxy-recomp*'` guard also matches the release builder's own
  git under `build\release`, and `*roguegalaxy*` matches `RogueGalaxy-Setup`; I used the process names after that.

## Not verified

- **Real clicks, a real mouse, a real display scale**: buttons were pressed with Tk's `invoke()` and 150 % was Tk's `scaling`, not a
  monitor at 150 %; drag and drop was a posted `WM_DROPFILES` message, not a drag from Explorer.
- **`RogueGalaxy-Setup.exe` itself, started by double-click, and its missing-Python message.** It compiled with the bundled clang and
  the window it starts (`pythonw.exe -m rgsetup.gui`) was exercised by `--live` and `--selftest` with the same interpreter, but the
  exe was never run: the owner's game was running from 3:50 PM until the end of the session, and a visible window could take its
  focus. Run it once yourself; if it shows nothing, `toolchain\python\pythonw.exe -X utf8 -m rgsetup.gui` from `tools\setup` is
  what it runs.
- **Play** and **Open folder** on the finish page, **Change...** (only the list of what it would copy was computed: 31,607 files,
  1.3 GiB, none of `disc`, `build`, `textures`, `work`, `mc0`, `mc1`, the state file), and **Open the setup** in the game (its enabled
  state was drawn and checked in item 9; pressing it was not, because it opens a visible window).
- A CHD or BIN/CUE disc in the window (phase 1 tested them in the CLI), a disc of another region in the window.
- Windows 10, a PC with no NVIDIA card (simulated only), antivirus or SmartScreen behaviour, OneDrive and network folders in the
  window, screen readers (Tk exposes little to UI Automation: the window is keyboard-reachable but was not tried with a reader),
  another display language (there is only English).
- The full HD upscales (deliberately), and the HD movie progress on a long run.
- Code signing: the exe is unsigned.
- `tests/e2e_setup.py` was not changed and not rerun; the runs above replace it for this phase.
- The licences in THIRD-PARTY-NOTICES.md for the bundled pieces (MinGit, CMake, Ninja, llvm-mingw, libdwarf and the other
  dependency sources) are from my reading of what each project says, not re-checked against its repository today.

## The owner decides

- **Code signing and the SmartScreen wording.** The release is unsigned, so Windows shows "Windows protected your PC", "Unknown
  publisher". README and SHARING.md say what to do ("More info", "Run anyway", compare the SHA-256 on the release page). A
  certificate costs money every year and a new one still warns until the file has a reputation.
- **The name on the window, the exe and the zip**: "Rogue Galaxy Recompiled Setup" / `RogueGalaxy-Setup.exe` (`ui.title`,
  `setup_launcher.c`, `make_release.py`).
- **Bundling GPL MinGit** (git is needed for `git apply` of the patches; the notice names its source), or having the build apply the
  patches itself.
- **The time claims**: the README says "20 to 25 minutes" for the build on a fast PC (measured: 10.8 min offline from the release,
  21 min with downloads in phase 1's clean clone, both at below-normal priority); the window's own estimate still says 20 to 60.
- **Zip size**: 413.9 MiB, far under 2 GiB; nothing needs cutting.

## Cleaned up

Deleted (all mine, under `Q:\work` or the system temp): the clean clone `rgsw1` (10.1 GB, with its disc, toolchain and build), the
unpacked release `rgrel2` (8.9 GB), the earlier unpacked release (11.7 GB), the stub program folder, the copy of the memory card and
scratch settings, the harness outputs and every screenshot (about 31 GB freed in all), the setup's temp logs. No process of mine is
running (each was started by this session and ended or was stopped by its PID; the owner's game was never touched).

Kept, on purpose: `build\release\` (**the zip, 413.9 MiB**, and the unpacked `RogueGalaxy-Setup\`, 1.3 GiB: delete the folder, the zip
is the deliverable), `build\release-cache\` (614 MiB of pinned downloads, so the next `make_release.py` run needs no network; safe to
delete), and `build\game-dev\rg-setupui.exe` (118 MiB, the test build of the page). **Not relinked: `build\game\src\roguegalaxy.exe`**,
but the objects behind it in `build\game` were rebuilt from the current sources and CMake was re-run there (new `setup_info.cpp`), so
`cmake --build build\game` now only relinks, as the older notes said.
