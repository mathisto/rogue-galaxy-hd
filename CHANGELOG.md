# Changelog

## Unreleased

- Setup window and a release that needs no download for the game:
  `RogueGalaxy-Setup.exe` (a small no-console launcher) opens a window to
  choose the disc (ISO, BIN/CUE, CHD or a folder; drop it on the window),
  the install folder and Faithful, Recommended or Everything, or each part
  with its disk space and time; HD parts this PC cannot do (no Vulkan card)
  are greyed out with the reason. A review page lists every download with its
  address, size, SHA-256 and licence before anything starts; then a step list
  with progress, the free space, a log you can open, Cancel (Ctrl+Break to the
  setup, which keeps what is finished), and a finish page with Play and Open
  folder. The window draws only the events of `python -m rgsetup`; its words
  are ids of the same text catalog. `tools/make_release.py` builds the release
  folder and zip: the pinned build tools (MinGit, Python with tkinter, CMake,
  Ninja, llvm-mingw), numpy and Pillow, and the sources CMake would download
  (`deps/`, `setup.py` builds from them without the network) come in the zip;
  Real-ESRGAN, FFmpeg and chdman stay on-demand, pinned downloads.
  `tools/check_content.py` fails the build when disc-derived bytes or personal
  paths are in it. Updating is unpacking the newer zip over the folder and
  running the setup again. F1 > **Setup and HD** shows what is installed (HD
  textures and how many, HD movies N of 89, the font and button prompts) and
  opens the setup window.

- Setup program: `python -m rgsetup` (tools/setup, docs/INSTALLER.md) is the
  one engine behind `Install.bat` and `Build-HD-Pack.bat`, which are now thin
  wrappers (old options still work or print their new equivalent). It prints
  one JSON event per line (start, plan, step, progress, space, warning, error,
  end), keeps a log in words, a state file (`rg-install.json`) and exit codes
  0 done / 1 failed / 2 bad command line / 3 cancelled. Presets: Faithful
  (the game), Recommended (+ HD textures + modern font and Xbox prompts),
  Everything (+ HD movies), each broken down per component with its disk and
  time. Disc inputs: ISO, BIN/CUE, CHD (converted with a pinned,
  SHA-256-checked chdman downloaded only after a yes), or an extracted folder
  validated by the hashes of its files, with one clear message for a wrong or
  partial dump and a plain statement for another region (only USA SCUS-97490).
  Steps carry keys, so a re-run skips what is finished and intact, and running
  a newer setup on the same folder is an update that keeps saves, settings and
  finished HD work; a folder made by the old Install.bat is adopted as it is.
  A PC without a usable Vulkan card gets the game and is told why the HD parts
  were left out. `tools/hd/build_hd_pack.py` gained `--events`.
  Pillow 13.0.0 or later is now required for the HD tools. Unit tests are in
  tests/ (`python -m unittest discover -s tests -p "test_setup_*.py"`).

- HD pack builder: `Build-HD-Pack.bat` builds the HD texture pack and the
  89 HD movies from your own disc in one go (docs/HD-BUILD.md). The tools that
  used to live outside the repository are in `tools/hd/` (the archive
  extractor with the 512-byte offset fix, the TIM2 and IM3 exporters, the
  renderer-key computation, the per-class upscaler, the pack builder, the
  movie batch with a stall watchdog, the PlayStation-art scanner), without
  any user path. The builder shows a menu (textures, movies, both), the
  disk space and time it needs, a `--gentle` mode, resumes where it stopped,
  keeps a log, and downloads Real-ESRGAN (and FFmpeg when there is none) only
  after showing the address and asking, checking a pinned SHA-256.
  `--limit N` / `--limit-movies N` make a quick trial. Publishing: THIRD-PARTY-NOTICES.md,
  docs/SHARING.md (what may and may not be shared), README notice and FAQ.

- Menu: the F1 menu was reviewed and regrouped (docs/MENU-DESIGN.md has the
  critique and the old-to-new map; no setting or `settings.ini` key changed).
  Nothing is cut off or out of reach any more, down to a 960 x 540 window
  and Menu size 200%: a scaling-filter word was cut ("Supersample"), the
  Shortcuts window ran off the screen, and the Game page was five to
  twenty-four screens long. Pages: Display, Sound, Controls, Game,
  Interface, Cheats (Cheats and Interface swapped). Settings are in sections
  with one line of help under each, the long explanation on hover or focus,
  the rarely used ones in a folded Advanced group (Display: auto-resolution
  tuning, integer scaling, line blending; Game: texture pack memory,
  developer tools); Back + Start moved to Controls; HD movies sits with the
  text and HD options. A gold dot marks what is not the default and every
  page ends with a two-step Restore defaults. Keyboard and controller: a
  visible focus ring on every control, the d-pad goes from the page list
  into the page, Esc/B backs out. A narrow window shows the page list as
  icons, a small window caps Menu size so the menu fits, dialogs scroll,
  toasts wrap, small text stays at 11 px or more. Shortcuts: F6 and F8 added,
  Fullscreen one row. Cheats notice shorter and defaulting to "Keep cheats
  off". Test aids: `RG_UI_SCRIPT` `scrollpage`, `pageinfo`, `unfold`, `nav`, `rowcheck`.
  Fixed: a row with the "changed" dot ran its help inline under the control,
  and the Experimental badge overlapped the label.

- Cheats: F1 > Settings > Cheats, behind a master switch (F5, Game > Cheats)
  that is off at every start, whatever settings.ini says. Infinite HP, infinite
  AP, a full action gauge, one-hit kills, items that are not used up, the
  money held at an amount or set to the maximum; each a native hook or a
  per-frame write found in the game's code and checked in the running game
  (docs/CHEATS.md lists what and where, and what was left out as unverified:
  setting levels, weapon charge). A notice asks for a yes the first time. While
  the master switch is on the game cannot save (the save screen says "Save to
  file N failed." and the memory card is not touched), and a "CHEATS ON"
  badge shows in the picture.

- HD movies: the cutscenes can play from upscaled 1080p files (HEVC in
  Matroska, one per movie, built from your disc: docs/HD-MOVIES.md). The game
  keeps playing its own movie, with its sound, clock, subtitles, pause and
  skip; only the picture it draws is swapped for the file's frame of the same
  number, at the right 16:9 / 4:3 shape (Widescreen included). A missing,
  unreadable or too slow file falls back to the original picture, mid-movie
  too. Game > Convenience > "HD movies" (and the game's Settings screen),
  `hd_movies` in settings.ini, on by default only when the folder holds
  files; `RG_HD_MOVIES=dir|0` for a run.

- HD texture packs: the pack builder now picks the upscale model per class by
  default: animevideov3 for effects (glows and sparks were greener, bigger and
  starrier), foliage and clouds, and the general x4plus model for Salgin's
  paving, cobbles and brick (no more invented "cracked mosaic" lines). The old
  look stays as `RG_TEXTURE_PACK=textures/SCUS-97490/x4plus-anime/replacements`.
- Xbox prompts: the controls boards' controller drawing is an Xbox-style pad;
  START/SELECT pictograms are Menu/View, the sticks LS/RS, the d-pad Xbox's;
  "START button", "Press the START button" and "L3/R3 button" read Menu, LS, RS.
- Modern font: battle damage numbers, the HUD's HP/AP digits and the results
  screen's digits are redrawn in it too.

- HD texture packs: every item icon is HD. The icon sheet the menus draw
  from (items, equipment, allies, shops, the developers' item grid) had
  stayed native: the game animates one of its icons in place and draws the
  glow behind equipped items with a one-colour palette, so the sheet never
  matched its pack file. The renderer now composes it from the pack's
  picture and the disc's own (`native_<key>.png` beside it, written by the
  pack builder), follows the animation, and draws the glow from the HD
  icons. Icons the game copies into other textures (the party's weapons in
  `work_icon`) are composed from the same picture.

- Display > Internal resolution: Auto picks the scale from the window's
  height in pixels (1080: 3x, 1440: 4x, 2160: 5x; oversampling 1.0 / 1.25 /
  1.5, at most 6x by default) and follows the window as it changes.

- Modern font now covers the menus' numbers too (play time, money, item
  counts, levels, stats, the save screens' file numbers) and the compass's
  N E S W, in the game's own colours and outline.
- Xbox prompts reach the pictures with PlayStation buttons painted in: the
  help pages, the loading recap's Press (X), the pause screen, the battle
  and menu sheets' L1/R1/L2 pills (114 buttons in 45 pictures). With an HD
  texture pack installed the field prompts ("Use transporter") and the
  modern font had stayed the pack's PlayStation/original art; fixed. A toast
  says which prompt set Auto picked and why.
- HD texture packs: the field compass and other HUD textures the game
  builds in off-screen memory now take their HD pictures; the minimap's AI
  upscale no longer spills grey blotches over the map (hard cut-out edges).
- Display > Scaling filter: Supersample, a Lanczos average of every
  rendered pixel for 4x to 8x internal resolution.
- Widescreen: full-screen menus are shown at 4:3 with the Borders fill
  beside them (Game > Widescreen menus: 4:3 pillarboxed, the default, or
  Stretched).
- ReShade and RenderDoc can be let in with `RG_ALLOW_VK_LAYERS=1`
  (colour shaders only for ReShade); captures carry pass labels.

- Fast-forward from the controller: R2 held runs the game faster while
  held, L2 switches fast-forward on and off; on the keyboard, ` (left of 1)
  held, and F4 as before. The game no longer gets L2 and R2 while they do
  this (F1 > Interface > Pause and speed turns it off).
- Fast-forward speeds 1.5x and 8x besides 2x, 3x, 4x and Max; movies keep
  normal speed so their sound stays in step.
- Display: an FSR scaling filter (AMD FSR 1's upscale, for a window larger
  than the internal resolution) and Sharpening (FSR 1's RCAS: off, low,
  medium, high). DLSS and temporal upscalers are not possible here:
  docs/GRAPHICS-UPSCALING.md.
- Quick save and quick load were looked into and are not included:
  docs/QUICKSAVE.md says why.
- Xbox button prompts: the game's text shows A, B, X, Y, LB, RB, LT and RT
  (by position) instead of the PlayStation buttons; Auto (the default) picks
  them unless a PlayStation controller plays. The help pages' controller
  pictures keep the PlayStation buttons.
- Modern font: the game's text redrawn sharp at the internal resolution in
  Roboto, or a .ttf of your own in `fonts\`; text rewraps to its widths. Off
  by default (F1 > Game > Text and prompts).
- Texture replacements: packs of PNGs in
  `textures\SCUS-97490\replacements`, named by texture hash
  (`RG_TEXTURE_LOG=1`); docs/FONT-AND-PROMPTS.md.
- HD texture packs: a pack of the AI-upscaled (4x) textures can be built
  from your disc with the tools in `tools/hd` and dropped in that folder
  (docs/TEXTURE-PACKS.md). Pictures are read in the background on first use
  (the game's texture shows meanwhile), kept under a memory budget (F1 >
  Game > Texture pack memory, 2 GB by default), shared on the GPU, given
  smaller levels so distant ground does not sparkle, and cut-outs keep the
  game's silhouettes. F8 reads the folder again. Learn mode
  (`RG_TEXTURE_LEARN`) collects the textures a pack misses while you play.
  Known gaps: the item icon sheet, text pages (the modern font covers them),
  textures whose palette the game animates.

## 0.1.0 (pre-release)

First release: the source plus an installer that builds the game on your PC
from your own disc image. No game data is included.

**Requires:** Windows 10/11 (64-bit), about 16 GB free, and your own image
of Rogue Galaxy (USA, SCUS-97490).

### Install

1. Download **RogueGalaxy-0.1.0.zip** (not "Source code").
2. Extract it to a short folder, such as `C:\RogueGalaxy`.
3. Run `Install.bat` and pick your disc image.
4. Wait about 20 minutes on a fast PC (up to an hour on a slower one), then
   play with `Rogue Galaxy.exe`.

### Highlights

- 60 fps, or the original 30
- Widescreen, up to 32:9
- Up to 8x internal resolution
- In-game menu (F1): graphics, sound, controls
- Game options: camera, field of view, draw distance, instant text,
  encounter rate, experience, money, item drops, shop prices and more

### Known issues

- Only the USA version is supported.
- The heaviest scenes can dip to 50 fps.
- In widescreen, menus and text are stretched.
- An antivirus may block the compiler during the install: restore it and
  run `Install.bat` again.
