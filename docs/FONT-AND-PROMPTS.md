# Xbox button prompts, a modern font, and texture replacements

Three options under F1 > Game > **Text and prompts** (and on the last of the
added pages of the game's own System > Settings, L1/R1 to turn):

| Option | Choices | Default | Variable for one run |
|---|---|---|---|
| Button prompts | PlayStation / Xbox / Auto | Auto | `RG_BUTTON_PROMPTS=playstation\|xbox\|auto` |
| Font | Original / Modern | Original | `RG_FONT_STYLE=original\|modern`, `RG_FONT=path.ttf` |
| Texture replacements | on / off | on | `RG_TEXTURE_REPLACEMENTS=0` |

Saved in `settings.ini` as `button_prompts` (0, 1, 2), `font_style` (0, 1) and
`texture_replacements`.

## Button prompts

The game writes its button prompts as tags in its text (`(X)`, `(O)`, `(#)`,
`(A)`, `[l1]`, `[r1]`, `[l2]`, `[r2]`) and draws them from one 256x256 sheet,
`gaiji2`. With **Xbox**, that sheet's eight controller icons are redrawn as an
Xbox controller's, by **position**:

| PlayStation | Xbox |
|---|---|
| Cross (bottom) | A, green |
| Circle (right) | B, red |
| Square (left) | X, blue |
| Triangle (top) | Y, yellow |
| L1 / R1 | LB / RB |
| L2 / R2 | LT / RT |

The art is drawn by the program (`src/hd_art.cpp`), at the internal
resolution, in the same cells as the game's icons, so line widths and
wrapping do not change. Nothing of the original art is copied.

**Auto** shows the PlayStation icons while the controller playing is a
PlayStation one (PS3, PS4, PS5 as SDL reports them) and the Xbox icons
otherwise, including with no controller at all: the keyboard's keys match
neither, and Xbox is the convention PC players know. A Nintendo controller
gets the Xbox icons too, which match it by position, not by letter. The
choice is checked about once a second, so plugging another controller in
switches it.

With a texture pack, the pack's upscale of `gaiji2` (its icons PlayStation
ones) is the canvas and the Xbox icons are drawn over it; before this the
pack's file replaced the drawn sheet, so field prompts ("Use transporter")
stayed PlayStation with a pack installed whatever the setting.

Which set is showing, and why, is logged once per change and shown as a
toast: `[font] Button prompts: Xbox (Auto: XInput Controller #1 is not a
PlayStation controller)`.

### Pictures with buttons painted in

Pictures with the buttons drawn into them cannot be reached by the sheet
swap. With Xbox prompts, an Xbox button is drawn over each PlayStation one
found in them, in the same box, at the internal resolution (or over the
pack's upscale of the picture): the loading recap's "Press (X)", the pause
screen's "(#): Go back to title", the battle sheet's (#)/(^) and L1/L2
pills, the item menu's and save screens' L1/R1, and the help boards' buttons
(System > Help). Where they are is a table, `src/button_art_table.inc`,
written by `tools/rg_psart_scan.py` (a template match of `gaiji2`'s own
icons against the UI textures exported by `im3_export.py`; boxes only, no
art). See [SESSION-NOTES-polish.md](SESSION-NOTES-polish.md) for what it
covers.

Since polish 2 ([SESSION-NOTES-polish-2.md](SESSION-NOTES-polish-2.md)) the
same table also holds:

- **the controller drawing** of the controls boards (Help > Controls
  -Movement- and -Battle-, the loading screen's Game Controls): an
  Xbox-style pad drawn by the program over the DualShock, its d-pad,
  sticks, View/Menu and A B X Y where the DualShock's were, so the boards'
  lines still end on them;
- **START and SELECT** pictograms as the Menu (three bars) and View (two
  windows) buttons, the **d-pad** as Xbox's cross, the **stick** mushrooms
  as LS/RS thumbsticks;
- **words in the pictures' lettering**: the pause screen's "START button"
  reads "MENU button", "Press the START button" reads "Press the Menu
  button", "L3 button"/"R3 button" read "LS"/"RS", redrawn in the label
  font in the word's colours.

The extra boxes come from the scanner with more templates (`EXTRA=`, cut
from the game's pictures and kept outside the repository) and from
`tools/rg_psart_manual.txt` (measured by hand); `tools/rg_psart_emit.py
--base` adds them to the table. Still the game's: the movie pause's "START
button: Continue" (drawn from another picture), icons in styles the match
misses (hb0011's large R1, hb0312's tabs, the factory's `Fac_ope01a`), the
Japanese copies' words.

## Numerals

The menus' numbers (play time, money, item counts, levels, HP and stats, the
save screens' file numbers and levels) are not text but digit sprites cut
from four menu sheets (`topmenu_02a`, `Itm_05`, `TP_save02`, `TP_top_02a`;
research 11). With Font **Modern** their digit rows are redrawn in the same
font, in the box each of the game's digits inked (so widths, centring and
the colon stay the game's), with the game's outline colour and its fill's
colours top to bottom, measured from the sheet; the money digits lean like
the game's italic ones. Over a pack's upscale of the sheet when there is
one, so the rest of the sheet stays HD.

The battle's digits too (polish 2): the damage popups (`btl_st02`, drawn by
DamagePopupDraw 0x2D3240: white, italic, a two-texel outline), the cyan and
big orange rows of `btl_st01`, the HUD's HP/AP digits big and small with
their '/' (`b_m_base`), and the results screen's Exp/Skills and level
digits (`b_end_base`). The thick-outlined rows get the font's strokes grown
to match the game's bold digits (`DigitRow::outline`). The pictures are
keyed for any palette, so popups the game tints by palette or vertex colour
(heal, critical) take the redrawn digits too; those were not seen in a test.

The field compass's letters (N E S W, `rc/compass.chr` "moji", which no
pack holds) are redrawn the same way with Font Modern.

## Font

**Modern** redraws the game's three font pages (normal: dialogue, the
loading recap, field prompts; system: menus, descriptions, the save screens;
name: speaker names, list and settings labels) from a TrueType font, at the
internal resolution: sharp at 2x to 4x and beyond. The font is, in order:

1. `RG_FONT=path` to a .ttf or .otf;
2. the first .ttf or .otf, by name, in a `fonts` folder in the game's
   working directory (the repository folder for `Rogue Galaxy.exe`, beside
   `borders`);
3. Roboto Medium, built in (Apache 2.0, from Dear ImGui's fonts).

How it fits the game's glyph cells (18x22, 16x20, 16x18 game pixels):

- the font is sized so its capitals are as tall as the game's, or smaller if
  its ascenders and descenders would leave the cell, and sits on a baseline
  inside it; a glyph too tall or too wide for its cell (brackets, W, @) is
  shrunk alone;
- if the font is wider than the game's on average, it is condensed to the
  game's average width, so text fits the boxes it was written for; a narrower
  font keeps its own widths;
- each letter's advance, rounded to whole game pixels, replaces the game's in
  its width table (`D_0044E850`), so lines wrap and boxes reflow for the new
  letters (there is no kerning, as in the game);
- only the ASCII letters, digits and punctuation are redrawn; arrows, stars
  and shapes (`[ar-*]` and friends) and the Japanese glyphs stay the game's;
- the game's outlines and shadows (the same glyph drawn several times, offset)
  keep working, in the new shapes.

Text already laid out on screen keeps its old positions until its next page.

**Widescreen** still stretches text horizontally, as it does with the game's
font: the 2D layer is drawn for 4:3. Narrowing each glyph would make the
letters right but leave the gaps between them stretched; not done.

### Your own font

Put one .ttf or .otf in `fonts\` (or point `RG_FONT` at one) and pick Modern.
It needs the Latin letters and digits; the log says `[font] text font: ...`
or why it was refused. Check its license before sharing screenshots or
recordings.

## Texture replacements

The prompts and the font are pictures put in place of the game's textures in
the renderer's texture cache (`thirdparty/dq8-gfx/.../sdlgpu_replacements.h`).
The same mechanism reads **texture packs**:

```
textures\SCUS-97490\replacements\     (in the working directory; sub-folders allowed)
    <texels>_<clut>_<W>x<H>_psm<NN>.png
```

- `<texels>`: the 16-hex-digit hash of the texture's raw texels as the cache
  computes it (FNV-1a, seeded with W, H, PSM); `<clut>`: its palette window's
  hash, or 16 zeros for any palette. `-` works as a separator too. These are
  this renderer's hashes, not PCSX2's: PCSX2 packs do not match.
- The names are those `DQ8_GFX_DUMP_TEXTURES=<dir>` gives its dumps (as
  .ppm); `RG_TEXTURE_LOG=1` logs every texture's hash as it is built or
  changes, and every lookup.
- A picture is 1x to 8x the texture's size, RGBA, alpha 255 opaque (converted
  to the GS's 0x80). A picture of the top-left WxH part of a larger texture
  is allowed (the game's 512x416 font pages live in 512x512 textures).
- The prompts, the font, the numerals and the painted-in buttons are drawn
  over a file for the same texture (the file is their canvas); with Font
  Original and PlayStation prompts the file shows as it is.
- Read in the background on first use; F8 reads the folder again. Memory,
  mipmaps, learn mode and a pack of the whole game:
  [TEXTURE-PACKS.md](TEXTURE-PACKS.md).

Drawing: a picture at the internal resolution is sampled texel for texel; at
any other scale it is filtered, clamped to each sprite's own texels so a
sheet's neighbours do not bleed in (such sprites are drawn one at a time).
If anything fails (unreadable file, wrong size, a texture whose texels are
not all known yet) the game's texture is used and the log says why.

## For developers

| Piece | Where |
|---|---|
| Replacement store (files, generated pictures, lookup) | `thirdparty/dq8-gfx/src/gfx/backends/sdlgpu/sdlgpu_replacements.{h,cpp}` |
| Binding in the cache (part hashes, HD texture, follow-up uploads) | `sdlgpu_textures.cpp`: `matchReplacement`, `followReplacement` |
| Sampling (scale factor, filter, region clamp) | `sdlgpu_backend.cpp`, the full path's texture section |
| Art (font layout and pages, Xbox icons) | `src/hd_art.{h,cpp}` (no game state; stb_truetype from Dear ImGui) |
| Game side (page hashes, width table, options) | `src/hd_text.{h,cpp}` |

The pages and the sheet as the cache sees them (SCUS-97490):

| Picture | Where | Format | Part named | Texel hash |
|---|---|---|---|---|
| `FontTex_normal_EN_0` | 0x1C00 | T4HL (in a 32-bit buffer) | 512x416 | `2bad8e99ac5c8351` |
| `FontTex_system_EN_0` | 0x2A00 | T4 | 512x416 | `3e781ae048c61d81` |
| `FontTex_name_EN_0` | 0x2BA0 | T4 | 512x72 (its TEX0 reaches a live target) | `785b309d00337092` |
| `gaiji2` | 0x2D80 | T8 | 256x256 | `ef8062d131c0ea57` |

To find a texture's hash: `RG_TEXTURE_LOG=1` (with Font Modern, the log also
probes every 512x416 and 512x72 part of 4-bit textures). `RG_INPUT_FILE`
lets a hidden test run be driven step by step (DEVELOPMENT.md).

## Licenses

- Roboto Medium: Apache License 2.0 (Google), shipped inside Dear ImGui
  (`thirdparty/imgui/misc/fonts`), already compiled into the menu.
- stb_truetype: public domain / MIT, the copy in Dear ImGui (`imstb_truetype.h`).
- The Xbox-style icons are drawn by this program's code; they copy no art.
  Xbox is a trademark of Microsoft; the icons only follow the controller's
  button colours and letters so prompts match the pad in hand.
