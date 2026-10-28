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

### What stays PlayStation

Pictures with the buttons drawn into them, which the sheet swap cannot reach:

- the help pages' controller diagrams (System > Help, the `help_board`
  pictures) and their "Press (X)" footer;
- the loading screen's "Press (X)" after the story recap;
- any other picture with a button painted in (the title screen's "Press the
  START button" has no icon).

These would need per-picture replacements in a texture pack (below).

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
- A file wins over a drawn picture (the prompts, the font).
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
