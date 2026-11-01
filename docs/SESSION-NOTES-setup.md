# Session notes: the setup program, phase 1 (2026-11-01)

Branch `feature/setup` (from `feature/share-prep`), local commits only, nothing
pushed, no remote touched. User docs: [INSTALLER.md](INSTALLER.md). Phase 2 (the
window installer, the bundled Python / compiler / runtime layout) is not started.
The design source is the sibling project's installer (`totk-pc-recomp/installer`,
same author, MIT); see THIRD-PARTY-NOTICES.md.

## What was built

- `tools/setup/rgsetup/` (stdlib only): `events.py` (JSON events + the log in words
  from `text/en.json`), `state.py` (`rg-install.json`), `runner.py` (step keys, transient
  steps, verify of kept output, cancel), `layout.py` (the one module that knows folders and
  toolchain), `plan.py` (presets, components, disk and time), `disc.py` (ISO, folder,
  BIN/CUE validation; serial and region), `hostcheck.py` (Vulkan probe through
  `vulkan-1.dll`, SSE4.1, free space, game process by path), `download.py` (pinned,
  SHA-256, chdman), `settings.py`, `proc.py` (children, cancel), `steps.py`, `cli.py`.
- `tools/hd/build_hd_pack.py` gained `--events` (`@@RG {json}` lines for stages, downloads
  and errors); nothing else in it changed. `tools/hd/requirements.txt` now says
  `Pillow>=13.0.0` (owner's note of 2026-10-29 about the older ones' critical CVEs; I did not
  look the advisories up again). `tools/disc/iso9660.py` closes the file when the image is
  not a disc (Windows could not delete the open file in a test).
- `Install.bat` / `tools/install/install.ps1` and `Build-HD-Pack.bat` /
  `tools/hd/build-hd-pack.ps1` are thin wrappers (old flags kept or mapped; the line they run
  is printed; builder-only flags run `build_hd_pack.py` directly). The toolchain pins moved to
  `rgsetup/data/toolchain.json`, read by both Python and PowerShell (for Python itself).
- `tests/`: 140 unit tests (`python -m unittest discover -s tests -p "test_setup_*.py"`,
  about 10 s), and `tests/e2e_setup.py` (real disc, clean clone, 27 checks).

## Decisions worth knowing

- **Dest is the program's own folder in phase 1.** The build, `toolchain/`, `disc/`,
  `textures/` and `work/` sit where Install.bat always put them. Another dest is accepted by
  `--estimate` with a warning and refused otherwise (`err.dest_not_program_folder`); an empty
  or foreign folder is classified (`layout.classify_dest`) and tested for phase 2.
- **Keys over inputs, not over the input's path.** `extract`'s key is the known disc hashes,
  so an ISO, a CHD, a BIN/CUE and a folder that are the same disc are the same extraction, and a
  second run needs only `--dest`. `build`'s key has the release and the sources' paths, sizes
  and times; the HD steps' keys have the extraction key, the HD tools' code and data lists, and the
  trial limits (so a trial never counts as the real run).
- **Adoption.** A folder with no `rg-install.json` (the user's own) has its intact outputs
  recorded as done (`adopted`), the build only when `roguegalaxy.exe` is newer than the sources.
  On the user's folder the build is therefore *not* adopted: `src/hd_text.cpp` and
  `button_art_table.inc` are newer than `build/game/src/roguegalaxy.exe` (the relink the older
  notes say is waiting). A real run there would rebuild, and refuses while the game runs.
- **settings.ini**: only absent keys are written, so the user's `button_prompts = 1`,
  `font_style = 1` (already there) mean nothing is written. `finish` writes no settings: HD
  movies are already on by default when the movie folder has files, texture replacements are on, and
  Internal resolution Auto is *not* the repo's default (`autoScale = false`), so it is left alone.
- **AVX2 is not needed** (the runtime is built with `-msse4.1`); the check requires SSE4.1.
- **Consent.** Every download needs `--yes` (or a yes on a console with `--format text`); the
  plan lists address, size, SHA-256, licence and destination for each. `Install.bat` passes
  `--yes`, as running it always meant "download the build tools".
- **chdman** is MAME 0.289's Windows release (`mame0289b_x64.exe`, 87,626,249 bytes, SHA-256
  `a1aa7912...6346`, matches GitHub's asset digest), read with the Windows `tar` without
  running it; only `chdman.exe` and `COPYING` are kept. chdman.cpp is BSD-3-Clause (header read
  from the tag).

## Verified (this PC: Ryzen 9 7900X, RTX 5070 Ti, NVMe; the game was not running)

- Unit tests, 140, all pass: events (schema, throttle, log, text mode, catalog fallback), presets
  and the disk/time math (also against `GB_TEXTURES`/`GB_MOVIES` in the builder), step keys,
  resume, update, transient, cancel, verify, force, only/skip, estimate writes nothing; disc
  validation on synthetic ISOs and folders (a partial dump gives one message with counts, another
  region names serial and region), BIN/CUE (`MODE1/2048`, `MODE1/2352`, `MODE2/2352`, audio after
  the data, damaged sector), settings edits (byte-exact otherwise), the CLI on a stand-in
  program folder (exit codes 0/1/2/3, consent, no Vulkan, foreign folder, adoption), real Ctrl+Break
  to a subprocess (exit 3, `end` cancelled), the build step's commands with a fake process runner,
  the game-process check with a copied `ping.exe` under `build\game-dev` (found) and under
  `toolchain\python` (not found), and catalog completeness (every id the code can emit, with the
  placeholders its call site passes, and the builder's stage names).
- Read only on the user's real folder and ISO: `--estimate --preset everything --source <iso>`
  and `--estimate --verify` (re-hashed all 124 files, 6.67 GB, 50 s: all match; the ISO's key files
  checked in 1.3 s). Nothing was written (no `rg-install.json`, no `work/`, log in the system temp).
  Adoption recognised `extract`, `toolchain`, `hd_textures` and `hd_movies`.
- `tests/e2e_setup.py --iso <real> --chd <real CHD> --cancel --cleanup`, clean `git clone` of
  this branch, 27 checks, all pass:
  - estimate+verify writes nothing; the run (check, source, extract, trial HD textures with
    `--limit 6`, trial HD movie with `--limit-movies 1`, prompts_font into a scratch settings file)
    exits 0 in 5.7 to 7.1 min over three runs: ISO check and source 1.3 s, **extract 16.5 s**
    (6.67 GB, NVMe), **textures trial 332 s** (almost all of it the builder's preparation steps; 4
    pictures upscaled), the smallest movie (`GAMEOVER`) 38 s. Real-ESRGAN downloaded and
    SHA-256-checked, numpy 2.5.4 and **Pillow 13.0.0** installed by pip into the clone's
    `toolchain/pylibs`, and the HD tools ran on them.
  - `--estimate --verify` on the clone: the unpacked disc re-hashes clean.
  - a second run with only `--dest` (plus the same trial flags) starts nothing but check and finish.
  - Ctrl+Break during a forced re-extraction: exit 3, `end` cancelled, the state keeps the earlier
    steps; the next run finishes and the disc is whole.
  - a CHD of the real disc (made here with `chdman createdvd`, 4.2 GB, at idle priority): chdman
    downloaded by the setup (10 s) and unpacked (3 s), `extractdvd` + validation 83 s, extract 10 s;
    the disc in the clone re-hashes identical to hashes.json.
- `Install.bat`'s script with `-Estimate -Yes` and `Build-HD-Pack.bat`'s with `--mode 3 --estimate`
  on the real folder (read only; install.log left alone); the latter with `--mode 1 --limit 3
  --gentle --yes` in the clone (the HD step finished; see the mistake below).
- The Vulkan probe on this PC: `[{'name': 'NVIDIA GeForce RTX 5070 Ti', 'type': 'discrete'}]`.

Cleaned: the clone and its disc/work files (9.4 GiB), the CHD (4.2 GB), the MAME download
(87 MB), chdman copies, logs in the system temp; kept: nothing outside the repository.

## Not verified

- **A game build through the new CLI.** The toolchain downloads in Python (`ToolchainStep`), the
  environment, `setup.py tools / recompile / build` driven with progress, the submodule fetch, and
  the whole `Install.bat` on a clean folder (including `Install-Python` when the PC has no Python, the
  folder-too-long copy offer, the shortcut) were exercised only as far as unit tests with a fake
  process runner and the PowerShell scripts' parse and estimate runs. A real clean install is 20 to 60
  minutes and about 9 GB; I did not run it. `build\game` was not relinked (no game process was
  running, but nothing needed it).
- BIN/CUE with real disc data (synthetic only); a CD-type CHD (`extractcd` path); a CHD of another
  release; a real disc of another region (synthetic serials only).
- The full upscale of textures or movies (deliberately; the trials upscale 4 pictures and one movie).
  The movie progress poll was not watched on a long run.
- A PC without Vulkan, or with only an integrated GPU: simulated with `RG_SETUP_FAKE_VULKAN`.
- Weaker graphics cards, hard disks, antivirus interference (the patience loops are copied from
  install.ps1, not exercised), OneDrive and long-path rejection beyond the unit-level function.
- The time figures for BIN/CUE or CHD conversion (`plan.CONVERT_MINUTES` = 3 to 10) are a guess; the
  measured CHD conversion was about 80 s for `extractdvd` on this PC.
- The build time (20 to 60 min) and HD times are the README's and HD-BUILD.md's figures, not remeasured.

## A mistake caught

While testing the wrapper in the clone I passed `--mode 1` but the wrapper ran `--only
...hd_textures,hd_movies`; because the clone's state already held `movies` (from a trial with
`--limit-movies 1`) and the trial key differs from a no-limit run, the *full* movie upscale started.
I stopped it (the processes were mine, `taskkill /T` on the wrapper's setup process) after a few
minutes, and the wrapper now passes only the step of its mode. A second earlier slip of the same kind
(my e2e's "second run" lacked the trial flags and started the full texture upscale) was stopped the
same way; the e2e now repeats the flags. Lesson, in the docs: trial flags are per run and change
the keys.

## For phase 2

See "For the window" in INSTALLER.md. Interface: `python -m rgsetup ... --format json` (default),
events as in INSTALLER.md, `plan.disk.per_component` for the per-component page, `plan.downloads`
for the consent page, `--estimate` before the page, `--yes` after it, Ctrl+Break to cancel, exit
codes 0/1/2/3, `events.load_catalog(lang)` + `events.render` to show ids. Replace the toolchain by
changing `layout.py` only (`python()`, `build_path()`, `setup_py_command()`, `hd_command()`) and make
`steps.ToolchainStep.wanted` return False.
