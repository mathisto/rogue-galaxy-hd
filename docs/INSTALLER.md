# The setup (`python -m rgsetup`)

One program takes a player from "I have the game's disc" to a working game with the HD parts they chose. It has no
window of its own: it prints one JSON event per line, and a log in words. `Install.bat` and `Build-HD-Pack.bat` are thin
wrappers over it, and the window installer (phase 2) will be another. The code is `tools/setup/rgsetup/`; the sibling
project's installer (`totk-pc-recomp/installer`, also MIT and by this project's author) is where the structure comes
from: JSON events, step keys, resume as the same rule as update, a text catalog, exit codes.

```
python -m rgsetup --dest DIR [--source PATH] [--preset faithful|recommended|everything]
                  [--components textures,movies,prompts_font] [--yes] [--estimate] [--verify] [--gentle]
                  [--temp DIR] [--lang en] [--format json|text] [--settings-file FILE]
```

Run it from `tools/setup` (or with `PYTHONPATH=tools/setup`); it needs Python 3.11 or later and nothing else (the HD
builder needs numpy and Pillow, which the setup installs into `toolchain/pylibs` after a yes).

## What the player chooses

| Preset | Components | What it is |
|---|---|---|
| Faithful | none | The game as it was. |
| Recommended (default) | `textures`, `prompts_font` | The game, the HD textures, the modern font and Xbox button prompts. |
| Everything | `textures`, `movies`, `prompts_font` | Recommended and the 89 HD movies. |

A preset is only a set of components; the window expands it to per-component choices, each with its disk and time
(`plan.disk.per_component`). There are no 3D models in v1.

| Component | Needs | Disk while building / afterwards | Time on the author's PC | Downloads (asked first) |
|---|---|---|---|---|
| `textures` | a Vulkan graphics card | 11 GB / 6 GB | 20 to 30 min | Real-ESRGAN 45 MB, numpy and Pillow 40 MB |
| `movies` | a Vulkan graphics card | 11 GB / 7 GB | about 90 min | Real-ESRGAN 45 MB, FFmpeg 105 MB (not if on PATH) |
| `prompts_font` | nothing | none | none | none |

`prompts_font` builds nothing: it writes `button_prompts = 1` (Xbox) and `font_style = 1` (modern) into the `[game]`
section of the game's `settings.ini`, **only if those keys are absent**. A key the player set in the menu is never
overwritten, which also makes an update safe. Nothing else is ever written to `settings.ini` (Internal resolution
Auto is not the game's default, so it is not touched; HD movies are on by default whenever the movie folder has files).

The game itself costs about 7 GB (the unpacked disc) plus 9 GB (build and toolchain) and, on a fast PC, 20 to 60
minutes. The disk and time figures are the ones measured and documented in [HD-BUILD.md](HD-BUILD.md) and the README;
the HD time depends on the graphics card, the disk figure does not.

A PC without a usable Vulkan graphics card (a CPU or virtual device does not count) still gets the game: the HD
components are left out with the warning `warn.hd_no_vulkan` or `warn.hd_no_gpu`, and the plan lists them in
`skipped_components`. An HD component already built earlier stays, whatever this PC has.

## The disc

The only supported disc is Rogue Galaxy (USA), SCUS-97490 ([DISC.md](DISC.md)). `--source` takes:

| Input | How |
|---|---|
| `.iso` | Used as it is; read only. |
| `.bin` + `.cue`, or a lone `.bin` | The first data track's 2048-byte sectors (`MODE1/2048`, `MODE1/2352`, `MODE2/2352`) are written to an ISO in the temp folder. A BIN that already is an ISO is used directly. Every raw sector's sync pattern is checked. |
| `.chd` | Converted to an ISO by `chdman` (`extractdvd`, or `extractcd` then the BIN/CUE rule). chdman is **downloaded only after the player says yes** (the plan lists it): MAME 0.289's Windows release, 87.6 MB, SHA-256 `a1aa7912...6346` (pinned in `download.py`), from github.com/mamedev/mame; only `chdman.exe` is kept, in `toolchain/chdman`. chdman is BSD-3-Clause (MAME as a whole GPL-2.0-or-later); the installer runs it as a separate program. |
| a folder | The extracted disc. It is found even one folder down or in other letter case, and **validated by hashes**: all 124 files must be present with the right size, and every file up to 32 MiB must have the right SHA-256 (the big ones are hashed while they are copied into `disc/`). A wrong or partial dump fails with **one** message (`err.disc_mismatch`) that says how many files are missing or wrong and names the first. |

Which release it is comes from the boot file named in `SYSTEM.CNF` and from the hashes of `config/SCUS_974.90/hashes.json`
(facts about the Redump dump, no game data). Another game or region fails with `err.disc_region`, naming the serial and
the region ("SCES-54567, Europe; only the NTSC-U release, SCUS-97490, is supported for now"). `--verify` also hashes the
whole image against Redump's CRC-32, MD5 and SHA-1.

## Steps and their keys

A step runs when its **key** (a SHA-256 over everything its output depends on) differs from the one recorded in
`rg-install.json`, or when its output is no longer in place; otherwise it is skipped. A step's key includes the keys of
the steps it reads from, so a change upstream reaches everything below it.

| Step | Does | Key covers | Output checked (cheap / `--verify`) |
|---|---|---|---|
| `check` | always, read only: this PC (64-bit Windows, SSE4.1; AVX2 is not needed), the folder, the disc, the Vulkan card, the space, the downloads | n/a | n/a |
| `source` | transient: BIN/CUE or CHD to an ISO in the temp folder; validates the image or folder; runs only when `extract` does | the disc's known hashes, the input kind | n/a |
| `extract` | `setup.py extract` (an ISO) or a hashed copy (a folder) into `disc/` | the known disc hashes (not the input path: any valid copy is the same disc) | `disc/manifest.json` and all 124 sizes / every file re-hashed |
| `toolchain` | the pinned build tools into `toolchain/` (git, Python, CMake, Ninja, llvm-mingw), SHA-256 checked | the pins in `data/toolchain.json` | each tool's exe and hash stamp |
| `build` | `setup.py tools`, `recompile`, `build`; refuses to run while the game is running | the release, the sources' paths, sizes and times, the extraction key, the toolchain | `roguegalaxy.exe` and `Rogue Galaxy.exe` exist / sizes |
| `hd_textures` | `tools/hd/build_hd_pack.py --mode 1` | the extraction key, a hash of `tools/hd/*.py` and its data lists, `--limit` | the pack manifest and a sample of its files / all of them |
| `hd_movies` | `build_hd_pack.py --mode 2` | the same, `--limit-movies` | the `.mkv` count (89) / non-empty |
| `prompts_font` | the two settings defaults | the two key/value pairs | none: written once, the player's later choice stands |
| `finish` | always: removes scratch from the temp folder, records the components | n/a | n/a |

Because the keys do not depend on the install folder's history, **a first install, a resume after a cancel or failure,
and an update are one rule**. An update is the newer setup run on the same folder: the extraction (key without the
release) and the HD packs (key without it unless the HD tools changed) are kept, the build (key with the release and the
sources) runs again incrementally, and the output of steps recorded by another release is hash-checked first.

A folder made before the setup existed (Install.bat, Build-HD-Pack.bat) has no `rg-install.json`. When the setup meets one it
**adopts** what is in place: a step whose own output check passes is recorded as done (`adopted`), the build only if the
executable is newer than the sources. `--redo STEP[,STEP]` runs a step again.

The HD steps resume inside as well: `build_hd_pack.py` keeps `work/hd/state.json`, finished upscales are files, and its
stall watchdog restarts a stuck movie; the setup only calls it and translates what it prints.

## Command line

| Option | Meaning |
|---|---|
| `--dest DIR` | The install folder (required). In this release it must be the program's own folder (the unpacked zip or the repository): the game is built where the program is. The folder must be that, empty, or hold `rg-install.json`; anything else is `err.dest_foreign`. |
| `--source PATH` | Disc image or folder (`--iso` is an alias). Needed when the disc is not yet unpacked in `disc/`. |
| `--preset`, `--components` | See above. `--components` replaces the preset on a first install; on a folder that has components it adds to them. The setup never removes a component. |
| `--yes` | Answer yes to every question, **downloads included**. Without it, a plan that has downloads stops with `err.consent_needed` (on a console with `--format text` it asks instead). |
| `--estimate` | Check and report; change and download nothing, and write no file in the folder (the log goes to the system temp folder). With `--verify`, a read-only hash check of everything kept. |
| `--verify` | Hash every kept output again (about a minute for the 6.7 GB disc); a mismatch makes that step run again. |
| `--gentle` | Lowest CPU and GPU priority for the HD builder and the build tools. |
| `--temp DIR` | Scratch (converted ISO, downloads, logs). Default `work/setup` in the install folder; `space` reports it separately when it is on another drive. |
| `--lang`, `--format` | Language of the log and of `--format text` (`text/<lang>.json`, English fills gaps); `json` (default) or `text` on stdout. |
| `--settings-file FILE` | Where `prompts_font` writes (default: the file the game reads, `RG_SETTINGS_FILE` or `%APPDATA%\RogueGalaxy\Recompiled\settings.ini`). |
| `--clean-work` | Delete the HD builder's work files when each HD step finishes (the pack stays). |
| `--gpu N`, `--tools-on-path`, `--redo STEPS`, `--only STEPS`, `--skip STEPS`, `--limit N`, `--limit-movies N` | A specific Vulkan device; developers' toolchain; run again; maintainers and tests (`--only` is what `Build-HD-Pack.bat` uses so it never touches the game build); trial runs of the HD builder. |

Exit codes: **0** done, **1** failed (an `error` event says why), **2** bad command line (an `err.usage` event), **3**
cancelled. Cancel by sending Ctrl+Break to the setup's process group (what the window will do), or Ctrl+C or SIGTERM:
the running child (and everything it started, and only those) is killed, an `end` event with `result: "cancelled"` is
printed, and what finished is kept in `rg-install.json`.

## Events (stdout, one JSON object per line)

Every event has `type` and `t` (seconds since the start). Text is never the payload: ids name a step (`step.<id>`), a
phase (`phase.<step>.<phase>`) or a message (`err.*`, `warn.*`); `tools/setup/rgsetup/text/en.json` has the English with
`{param}` placeholders, and a translator adds `text/<lang>.json`.

| `type` | Fields |
|---|---|
| `start` | `release`, `mode` (`install` \| `resume` \| `update`), `dest`, `log`, `steps` (ids in order), `estimate`, `verify`, `lang`, `adopted` (steps taken over from an older install) |
| `plan` | `components`, `preset` (`faithful` \| `recommended` \| `everything` \| `custom`), `skipped_components` `[{id, reason, params}]`, `region`, `source.kind`, `run` (steps that will run), `skip` `{step: reason}`, `downloads` `[{id, name, url, bytes, sha256, license, dest, needed_by}]`, `disk` `{need, game, peak_hd, final, temp_need, per_component{peak, final, minutes_min, minutes_max}}` (bytes), `time` `{minutes_min, minutes_max}` |
| `space` | `dest`, `need`, `free` (bytes on the install drive; includes the temp need when on the same drive), `temp`, `temp_need`, `temp_free`, `same_disk` |
| `step` | `step`, `state`: `started` \| `done` (`seconds`) \| `skipped` (`reason`: `current` \| `not_needed` \| `not_chosen`) |
| `progress` | `step`, `phase`, and `done`, `total`, `unit` (`bytes`, `files`, `steps`, `batches`, `movies`, `permille`, `tools`) when known; at most one every 0.5 s per phase, the first and last always |
| `warning` | `id`, `params` |
| `error` | `id`, `params`, `step` (absent outside a step) |
| `end` | `result`: `ok` \| `failed` \| `cancelled`, `seconds` |

`--estimate` prints `start`, `plan`, `space`, the `check` step and `end`. Any failure is an `error` then an `end` with
`failed`.

Example (trimmed):

```
{"type":"start","t":0.2,"release":"0.1.0","mode":"resume","dest":"C:\\RogueGalaxy","steps":["check","source","extract",...]}
{"type":"plan","t":0.4,"components":["textures","prompts_font"],"preset":"recommended","run":["check","hd_textures","finish"],...}
{"type":"step","t":0.4,"step":"hd_textures","state":"started"}
{"type":"progress","t":210.3,"step":"hd_textures","phase":"upscale","done":14,"total":120,"unit":"batches"}
{"type":"end","t":1805.2,"result":"ok","seconds":1805.2}
```

## The state file

`rg-install.json` in the install folder: `format`, `release`, `region`, `components` (everything installed so far),
`inputs.disc` (kind, file name, size, serial, `validated`; never a path to the image or a byte of it), and `steps`
(`key`, `release`, `finished`, `seconds`, the step's own fields, `adopted` when taken over). It is replaced atomically
after each step. A newer `format` than the setup knows is `err.state_newer`; an unreadable one `err.state_unreadable`
(move it away to start over; nothing else is deleted).

## What it never does

- It does not write to the disc image or change a file it did not make. `disc/mc0`, `mc1`, `textures/` and the player's
  `settings.ini` keys are never deleted or overwritten.
- It does not download without the plan having listed the address, size, SHA-256 and licence and a yes (`--yes`), and
  it throws away a file whose SHA-256 differs without unpacking it.
- It does not stop a process it did not start. Before the build replaces `roguegalaxy.exe` it looks for a running
  game by image name and by path under the install folder (`err.game_running`).
- It does not put anything from the disc in the log, the state file or an event.

## For the window (phase 2)

Start `python -m rgsetup` as a child with `CREATE_NEW_PROCESS_GROUP`, read stdout line by line, show the `plan` for
the consent page (components with `disk.per_component`, `downloads`, `skipped_components`), pass `--yes` when the player
confirms, send `CTRL_BREAK_EVENT` to cancel, and render ids through the same catalog (`events.load_catalog(lang)`,
`events.render`). `--estimate` before the page gives the numbers without side effects. The one place that knows the
folder layout and the toolchain is `layout.py` (`Layout`: `toolchain`, `python()`, `build_path()`, `setup_py_command()`,
`hd_command()`); the bundled Python, compiler and runtime replace those, and `steps.ToolchainStep` becomes a no-op.

## Tests

`python -m unittest discover -s tests -p "test_setup_*.py"`: events, presets and the disk/time math, step keys and
resume (a fake step list), disc validation on tiny synthetic ISOs and folders (no game bytes anywhere), BIN/CUE
conversion, settings, the CLI on a stand-in program folder, Ctrl+Break, and a check that every message id the code can
emit is in `text/en.json` with the placeholders it fills. `python tests/e2e_setup.py --scratch DIR --iso FILE` is the
end-to-end run with a real disc in a clean clone ([SESSION-NOTES-setup.md](SESSION-NOTES-setup.md) has its results).
