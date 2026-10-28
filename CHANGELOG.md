# Changelog

## Unreleased

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
