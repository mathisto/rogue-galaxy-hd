# HD texture packs

The renderer can draw any of the game's textures from a PNG of your own, at
up to 8x its size. A pack made from the AI-upscaled set (4x) replaces the
textures of the maps, characters, items, effects and menus; this page says
how to build one, install it, tune it and complete it.

A pack is made from the game's own pictures, so it is yours to use and not
to share or commit: the repository ignores `/textures/`.

## Turning it on

1. Put the pack in `textures\SCUS-97490\replacements` in the game's folder
   (the repository folder, beside `Rogue Galaxy.exe`). Sub-folders are fine.
2. Start the game. F1 > Game > Text and prompts > **Texture replacements**
   must be on (it is by default).
3. That is all: the folder is listed at start, each picture is read the first
   time its texture is drawn.

While a picture is being read (8 to 11 ms for a 512x512 4x PNG, on one of two
background threads), the game's own texture is drawn; a scene's textures
turn HD over its first frames. Nothing waits for a PNG.

## Options

| Where | What | Default |
|---|---|---|
| F1 > Game > Texture pack memory | Video memory the pack's pictures may take; as much again of main memory keeps pictures ready to upload. `RG_TEXTURE_BUDGET_MB` for one run | 2048 MB |
| F8 | Lists the pack folder again (hot reload): new and changed files are read at their next use | |
| `RG_TEXTURE_PACK=dir` | Another pack folder for one run | `textures/SCUS-97490/replacements` |
| `RG_TEXTURE_POLL=N` | Checks the folder every N seconds and reloads when a file changed (for pack makers) | off |
| `RG_TEXTURE_MIPMAPS=0` | No smaller levels (see Sampling) | on |
| `RG_TEXTURE_CUTOUT=soft` | Keep the PNG's soft alpha on cut-outs (see Sampling) | hard edges |
| `RG_TEXTURE_THREADS=N` | Threads reading PNGs | 2 |
| `RG_TEXTURE_UPLOAD_MB=N` | HD bytes uploaded per 16 ms at most (spreads a scene's uploads over frames) | 32 |
| `RG_TEXTURE_LOG=1` | Every texture's key as it is built, every lookup and picture bound, and every 10 s a line on the pack: files, read, waiting, memory, GPU | |
| `RG_TEXTURE_LEARN=dir` | Learn mode (below) | |
| `RG_TEXTURE_REPLACEMENTS=0` | No replacement at all (no pack, no modern font, no Xbox prompts) | |

Memory: a 128x128 texture at 4x is 1 MB of RGBA (1.33 MB with its smaller
levels), a 256x256 one 5.3 MB. The Salgin walk, title and menus measured
200 MB of HD textures on the GPU at most and 290 MB of decoded pictures, so
the 2 GB default leaves room for big areas. If a scene needs more than the
budget, textures not drawn for two seconds give way first; when only
textures in use are left, the newest stays the game's picture and asks again
five seconds later (no thrashing: 60 fps in Salgin even at a 64 MB budget,
with part of it in native resolution).

## Building the pack

The tools are in the research folder's `tools\` (Python 3.12, numpy, Pillow):

```
py -3.12 -I -X utf8 tools\rg_texkeys.py extracted2 keys.csv
py -3.12 -I -X utf8 tools\rg_texpack.py build --keys keys.csv ^
    --out Q:\projects\rogue-galaxy-recomp\textures\SCUS-97490\replacements ^
    --extra Q:\work\rg-textures\ui\all\unique.csv=Q:\work\rg-textures\ui\upscaled ^
    --extra "Q:\work\rg-textures\all3\manifest.csv=Q:\work\rg-textures\upscaled|x4plus-anime-extra|animevideov3-extra" ^
    --clean
```

- `rg_texkeys.py` reads every TIM2 picture on the disc and computes the key
  the renderer gives it once the game has uploaded it, bit for bit:
  FNV-1a over the deswizzled indices (8-bit) or the packed nibbles (4-bit),
  seeded with width, height and format, plus the palette window's hash
  (CSM1 order). 8-bit pictures stored linear rather than GS-swizzled get
  both keys; the runtime decides which one exists.
- `rg_texpack.py build` names each upscale by those keys
  (`<texels>_<clut>_<W>x<H>_psm<NN>.png`) and hard-links it into the pack
  (no space used; `--copy` to copy). It applies the decoder fixes
  (`all2\old-set-actions.csv`: no header-only stubs, no mis-decoded linear
  pictures), prefers the fixed and newly exported sets, merges the IM3 menu
  art and the newly extracted pictures by their `gs_key`, and writes
  `pack-manifest.csv` beside the folder. `--clean` removes only the files the
  previous manifest lists, so learned files stay.
- `--fallback-for-flagged` takes the animevideov3 upscale for the classes
  `flagged-classes.csv` recommends it for (foliage cut-outs, effects, smooth
  clouds): softer, fewer posterised leaves and streaks. A pack built so is in
  `textures\SCUS-97490\flagged-animevideov3\replacements`; try it with
  `RG_TEXTURE_PACK=textures/SCUS-97490/flagged-animevideov3/replacements`.
- `rg_texpack.py validate --keys keys.csv --pack DIR LOG...` reads logs made
  with `RG_TEXTURE_LOG=1` and reports how many of the textures the game built
  the pack names, with a list of the misses and why.

Built 2026-10-28 from all four sources (primary set minus the decoder's
rejects, the fixed set, the menu art, the newly extracted pictures): 11,055
files (8,106 primary, 102 fixed, 2,509 newly extracted, 338 menu art), in
2 s; plus 29 learned. The other agents' manifests write the PSM in decimal
(`psm19` is T8): the builder converts their `gs_key`s to the renderer's hex
names.

### Validation (2026-10-28)

Logged runs (`RG_TEXTURE_LOG=1`) over the title, the load screen, Salgin's
streets, the main, item and allies menus: the game built 210 distinct
texture identities, of which 27 are frame copies and render targets. Of the
183 indexed textures:

| | |
|---|---|
| Named by keys computed from the disc | 119 (65%) |
| Named by learn mode (from an earlier run of the same route) | 28 (15%) |
| Matched as a part (UI pages in a larger TEX0) | 6 |
| Item icon atlas (12 states as icons load) | not replaceable |
| Text pages (font, names: the modern font) | 7, left out |
| Pseudo-textures over the frame buffer (one palette, noise) | 7 |

So every disc texture the route showed has its HD picture, apart from the
icon atlas and the text pages. The keys are exact: of the first 102 textures
whose texel hash the disc reproduced, 101 also matched the palette hash (the
other was a palette variant).

## Completing it by playing (learn mode)

Some textures cannot be keyed from the disc: character faces whose eyes and
mouth the game paints in, sheets the game assembles, pictures in formats the
export does not read. Play with

```
set RG_TEXTURE_LEARN=C:\somewhere\learned
```

and every texture the pack did not cover is written there once, as a PNG
named by its key (frame copies and textures of one colour are skipped). Then

```
py -3.12 -I -X utf8 tools\rg_texlearn.py C:\somewhere\learned textures\SCUS-97490\replacements
```

links the upscale of any disc picture with the same pixels, and upscales the
rest with the same model and alpha handling as the main set
(`rg_upscale_batch.py`, about a second per picture), into
`replacements\learned\`. F8 in the game reads them. Text pages (4-bit
512-wide sheets: the modern font draws those), atlases over 512 (the item
icon sheet the game refills) and noise are left out.

## Sampling

- **Scale.** A picture is an integer multiple of the texture (1x to 8x);
  wrapping, UVs and alpha stay the game's, in GS texels. At the render
  scale a picture is sampled texel for texel; at any other scale it is
  filtered, and a UI sprite is clamped to its own texels so the next cell of
  its sheet does not bleed in (REPEAT that does not wrap counts as CLAMP;
  without that the item menu showed dots between its slots). Such sprites
  are drawn one at a time; the item and status menus stayed at 60 fps.
- **Alpha.** PNGs use 255 for opaque; the renderer stores the GS's 0x80.
  Where the game's texture has only transparent and opaque texels (a
  cut-out: hair, grilles, foliage), the upscale's soft Lanczos edge would
  move the alpha test's silhouette, so its alpha is made hard at half
  (`RG_TEXTURE_CUTOUT=soft` keeps it).
- **Mipmaps.** No disc texture has smaller levels and the GS path ignores
  them. A 4x picture is given its own (2x and 1x, alpha-weighted box filter,
  cut-outs kept hard) when its scale is a power of two, and the shader picks
  the level whose texels are about a pixel apart, never below the texture's
  own size, for 3D draws. Without them, ground and walls at a distance
  sparkle as the camera moves.
- **Palettes.** A file is for one palette (exact key) or any (16 zeros). A
  texture whose palette the game animates or fades changes key each frame;
  it shows the HD picture only for the palettes named. Rogue Galaxy's walks
  measured 3 such textures of 177.
- **Parts.** A picture uploaded into a larger power-of-two TEX0 (UI pages of
  288x224 in 512x256, 512x416 font pages in 512x512) is keyed by its own
  size and matched as the top-left part; only parts whose smaller sides are
  not powers of two are looked for (hashing every corner of every texture
  cost more than the game's drawing).

## Not covered

- **The item icon sheet** (1024x1024, 8-bit): the game copies each icon into
  it as menus open, so its content (and key) changes all the time; the icons
  stay native. Same for `work_icon`, filled by the CPU (0x28DF20).
- **Text pages**: the modern font redraws them; an AI upscale of letters is
  not used.
- **Frame copies and render targets** (bloom, the menu's backdrop, shadows):
  not textures from the disc.
- Partly updated textures (an animated 40x40 slot in the item icons) lose
  their picture after the first update.

## Performance

Measured in hidden runs at 2x render scale, Salgin (sand, walls, NPCs),
60 fps mode, the same scripted walk:

| | fps | GS worker busy |
|---|---|---|
| No pack | 59.9 | 63-64% |
| Pack (4x, mipmaps) | 59.8-59.9 | 42-54% |
| Pack, mipmaps off | 59.9 | 47% |
| Pack, 64 MB budget (too small) | 59.9 | 40-49% |

The frame rate is capped at 60; the worker's share is the comparable number
and did not rise (it varies with what is on screen; the runs differ a few
points). Matching costs one hash probe per texture change; pictures already
on the GPU are shared by key, so the game's habit of uploading different
textures into the same pages every frame costs no new upload.

BC7/BC3 compression of uploaded pictures was not done: there is no encoder
in the build, a BC7 encode of a 512x512 picture takes tens of milliseconds
on the CPU, and the measured scenes stay far under the budget. Compression
would cut video memory 4x (BC7) at some loss on the AI pictures' fine edges;
an offline step producing DDS files is the way if big areas need it.
