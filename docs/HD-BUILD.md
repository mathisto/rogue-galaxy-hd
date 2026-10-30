# Building the HD textures and HD movies

`Build-HD-Pack.bat` builds, from **your own disc**, the 4x AI-upscaled texture
pack ([TEXTURE-PACKS.md](TEXTURE-PACKS.md)) and the 89 upscaled 1080p cutscenes
([HD-MOVIES.md](HD-MOVIES.md)), and puts them where the game looks for them.
Nothing it makes may be shared ([SHARING.md](SHARING.md)); that is why it is a
builder and not a download.

## Before you start

- `Install.bat` has run: the builder reads the extracted disc in `disc\` (if
  it is not there, the builder asks for your `.iso` and extracts it the way
  `Install.bat` does).
- A graphics card with **Vulkan**: NVIDIA, AMD, or Intel Arc/Xe, with a current
  driver. The upscaler runs on it; without one the builder says so and stops
  before doing anything (the game itself plays without the HD files).
- Disk: about 11 GB for the textures and 11 GB for the movies on the drive of
  the repository (the pack and the movies are hard links to the work files, so
  they take no more). `--work "D:\somewhere"` puts the work files on another
  drive.
- Python 3.10 or later: the one `Install.bat` put in `toolchain\python` is used;
  otherwise a Python on your PC. The two packages the tools need, **numpy** and
  **Pillow** (`tools\hd\requirements.txt`), are installed into
  `toolchain\pylibs` after asking (from pypi.org, about 40 MB). Nothing is
  installed in Windows or in your Python.
- Internet, once, for the downloads below.

## Running it

Double-click **`Build-HD-Pack.bat`** (or run it in a terminal with options).

```
Choose 1, 2, 3, E or Q:
  [1] HD textures only   (about 20 to 30 minutes: about 5 of preparation, then 15 to 25 of upscaling)
  [2] HD movies only     (about 1.5 hours)
  [3] Both
  [E] Show the plan and the estimates only
  [Q] Quit
```

Before it does anything it prints the plan: the folders, the graphics card it
found, the tools it has or lacks, **the disk space needed against what is
free** (it stops if there is not enough), the time it expects, and which steps
are already done. Then it asks `Start now?`. The times are those measured on
the author's PC (Ryzen 9 7900X, RTX 5070 Ti, NVMe disk); the graphics card
decides the upscale, and a laptop or older card takes several times longer.

### Downloads (always asked first)

The builder never downloads silently. For each tool it prints the address, the
size, the SHA-256 it will insist on, the licence and where it goes, and waits
for a yes:

| Tool | When | Address (pinned) |
|---|---|---|
| Real-ESRGAN ncnn-vulkan 20220424, 45 MB, into `toolchain\realesrgan` | the first time | `https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-windows.zip`, SHA-256 `abc02804e17982a3be33675e4d471e91ea374e65b70167abc09e31acb412802d` |
| FFmpeg 8.1.2 essentials, 105 MB, into `toolchain\ffmpeg` | only for the movies, and only if there is no `ffmpeg` and `ffprobe` on your PATH | `https://github.com/GyanD/codexffmpeg/releases/download/8.1.2/ffmpeg-8.1.2-essentials_build.zip`, SHA-256 `db580001caa24ac104c8cb856cd113a87b0a443f7bdf47d8c12b1d740584a2ec` |
| numpy and Pillow | if Python lacks them | pypi.org, by pip, into `toolchain\pylibs` |

A file whose SHA-256 differs is deleted and nothing is unpacked. Licences:
[THIRD-PARTY-NOTICES.md](../THIRD-PARTY-NOTICES.md). `--yes` answers yes to
everything, downloads included: use it only when you have read the table.

### Options

| Option | Effect |
|---|---|
| `--mode 1\|2\|3` | textures, movies, both, without the menu |
| `--estimate` | the plan, disk and time; changes and downloads nothing |
| `--gentle` | the lowest CPU and GPU priority, one upscaler thread set, a pause between movies: the PC stays usable, the build is slower |
| `--yes` | no questions (downloads included) |
| `--iso "path"` | your disc image, used only if `disc\` is not extracted (an `.iso` dropped on the `.bat` does the same) |
| `--work DIR`, `--out DIR` | where the work files go (default `work\hd`, or `%RG_HD_WORK%`), and the `textures\SCUS-97490` folder written |
| `--gpu N` | the Vulkan device to use (default: the best; the builder lists the others) |
| `--redo STEP` | forget a finished step (`extract`, `tim2`, `im3`, `keys`, `merge`, or `all`) so it runs again |
| `--retry-failed` | try again the movies that failed before |
| `--stall-timeout S` | seconds without a frame moving before a movie is killed and retried (default 180) |
| `--limit N`, `--limit-movies N`, `--sample-log LOG` | a quick trial, below |
| `--clean-work` | delete the work files when done (the pack and movies stay; they are separate links) |

### Resuming

State is kept in `work\hd\state.json` (steps) and `work\hd\movies\state.json`
(movies); upscaled pictures are files, so a stopped upscale continues with the
pictures missing. Close the window, shut down the PC, run the `.bat` again: it
says `[skip] ... (done earlier)` for finished steps. The log of every run is
`work\hd\build.log` (the movies' own in `work\hd\movies\batch.log`).

## What it does

| Step | What | Where |
|---|---|---|
| extract | reads `disc\DATA0_0/1.DAT` through their `.HD6` indexes, with the 512-byte offset fix | `work\hd\extracted` (2.2 GB, 18,931 entries) |
| tim2 | decodes the TIM2 pictures; drops the 1,129 header-only stubs that are not images; decodes the pictures stored linear | `work\hd\tim2` (8,606 pictures) |
| im3 | decodes the IM3 menu art straight from the archive and gives each picture the renderer's key | `work\hd\im3` |
| keys | computes, for every TIM2 picture, the key the renderer gives the texture once the game has uploaded it | `work\hd\keys.csv` |
| merge | one folder and manifest of the 11,234 distinct pictures; classes (`rg_flag_classes.py`): effects, foliage, clouds, stone floors | `work\hd\all`, `upscaled\flagged-classes.csv` |
| upscale | `realesrgan-x4plus-anime` for most; `realesr-animevideov3` for effects, foliage and clouds; `realesrgan-x4plus` for the 88 hand-picked stone floors | `work\hd\upscaled\<model>\` (about 6 GB) |
| fp16 | finds the ~130 pictures x4plus-anime blacked out (ncnn fp16) by colour drift and redoes them with animevideov3 | same |
| pack | `rg_texpack.py build`: names each upscale by its key, hard-links it into the pack, writes the manifest | `textures\SCUS-97490\replacements`, `pack-manifest.csv` |
| verify | the manifest matches the files, the names are valid, a sample of pictures opens at 4x | |
| movies | per movie: decrypt the `.MVI`, upscale every frame 2x with animevideov3, resample to 1080p, encode HEVC Main10, keep the sound; check the frame count and the duration | `work\hd\movies\hd` |
| publish | hard-link the movies into the game's folder; check each: HEVC, 1080 high, as many frames as the disc's movie | `textures\SCUS-97490\movies` |

The **stall watchdog**: a movie is a pipe of three programs (FFmpeg decoding,
Real-ESRGAN, FFmpeg encoding). Once, Real-ESRGAN sat idle while the pipe
stalled and the batch waited for ever. Now the builder watches one progress
number (frames decoded + frames upscaled + frames encoded); if it does not
move for `--stall-timeout` seconds it stops the processes of that movie (only
those it started) and tries the movie again, up to twice, then marks it failed
and goes on. The log says `STALL: no frame moved for N s ... trying again`.

## When it stops

It prints `STOPPED at step "...": what` and `What to do: ...`, and keeps all
finished work: run `Build-HD-Pack.bat` again to continue.

| Message | Cause and fix |
|---|---|
| `the disc is not extracted` | Run `Install.bat`, or give your `.iso` |
| `only N GB free` | Free space, or `--work` on another drive |
| `no Vulkan graphics device was found` | Update the graphics driver; on a laptop with two chips make Windows use the fast one for the builder (Settings > System > Display > Graphics) or `--gpu N`; or skip the HD files |
| `only software Vulkan devices` | The same: the CPU cannot do this in a sensible time |
| `the download of ... was not accepted` | You said no; run again and answer y, or put the tool in `toolchain\<name>` yourself |
| `the file downloaded ... is not the expected one` | Something on the network changed it; run again; nothing was unpacked |
| `... is missing after unpacking` | An antivirus removed it: restore it from quarantine or allow `toolchain\` |
| `the step stopped (exit code ...)` | The last lines of the tool are printed; the log has all of them. Usual causes: no disk space, an antivirus holding files (wait, run again), a GPU driver crash (update it, try `--gentle`) |
| `N pictures failed in ...` | Listed in `_failed.txt` beside the upscaled set; the pack is built without them (those textures stay the game's) |
| `N movies failed` | `--retry-failed` (try `--gentle`); `work\hd\movies\batch.log` has the reasons |
| `the pack failed its check` | The pack step always rebuilds the pack: run again; if it repeats, the log has the pack builder's output |

## A quick trial

To see that it all works on your PC before the long run:

```
Build-HD-Pack.bat --mode 3 --limit 20 --limit-movies 2 --gentle
```

`--limit 20` upscales 20 pictures (a few of every model); `--limit-movies 2`
processes the two shortest movies. The preparation steps still read the whole
disc (about 5 minutes) and are kept for the real run. To make the 20 pictures
ones you will *see*: run the game once without a pack with `RG_TEXTURE_LOG=1`
(`set RG_TEXTURE_PACK=C:\nowhere` so no pack is used), play to the title and
the main menu, and pass `--sample-log build\last-run.log`: the 20 are then
chosen from the textures that run built. The trial's pack is tiny; run the
builder again without the limits to complete it (finished pictures are kept).

## Files the builder leaves, and the repository

`work\hd\` (extracted archives, decoded pictures, upscales, keys, state, log),
`toolchain\realesrgan`, `toolchain\ffmpeg`, `toolchain\pylibs`,
`textures\SCUS-97490\` are all outside version control (`.gitignore`) and none
may be shared. Delete `work\hd` to free about 20 GB once the pack and the
movies are built (`--clean-work`, or answer y when it asks at the end); the
pack and the movies stay, because they are separate hard links. A later change
(a new class, a different model) then needs the preparation steps again.

## FAQ

**Why do I need numpy and Pillow?** The pictures are decoded and compared with
numpy and written as PNG with Pillow. They are the only Python packages.

**How can textures take 20 minutes and not hours?** On the author's PC the
upscale of the whole set measured about 5 minutes for the 8,628 general
pictures at full speed (`x4plus-anime`, 306 s) and 2 for 2,510 more (136 s); the
preparation takes about 5 minutes, and the rest is the flagged classes. A
weaker graphics card is several times slower (an integrated one could take
hours). The movies are the long part.

**Can I change which model is used for a picture?** The classes are in
`tools\hd\rg_flag_classes.py`; the hand-picked stone floors in
`tools\hd\data\stone-floor.txt` (add a hash there to give a picture the
`x4plus` model). `--redo merge` and run again.

**I already have a pack from the old research tools.** `Build-HD-Pack.bat` writes
the same files in the same places, and `rg_texpack.py --clean` removes only the
files its previous manifest lists, so learned pictures (`replacements\learned`)
stay. To keep the old pack, copy `textures\SCUS-97490` aside first.

**The sound of a HD movie?** It is the game's own, played by the game; the
HD file only provides the picture ([HD-MOVIES.md](HD-MOVIES.md)).
