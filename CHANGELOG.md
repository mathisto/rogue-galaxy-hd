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
