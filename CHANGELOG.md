# Changelog

## Unreleased

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
  from your disc with the research tools and dropped in that folder
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
