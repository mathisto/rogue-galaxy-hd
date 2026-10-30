# HD texture packs

The renderer can draw any of the game's textures from a PNG of your own, at
up to 8x its size. A pack made from the AI-upscaled set (4x) replaces the
textures of the maps, characters, items, effects and menus; this page says
how to build one, install it, tune it and complete it.

A pack is made from the game's own pictures, so it is yours to use and not
to share or commit: the repository ignores `/textures/` and `/work/`
([SHARING.md](SHARING.md)). The repository holds the tools that build it from
your own disc, and nothing of the game.

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

**One click:** run `Build-HD-Pack.bat` (after `Install.bat`) and choose **[1]
textures**. It extracts the archives from `disc\`, decodes the pictures,
computes the renderer's keys, upscales, builds the pack into
`textures\SCUS-97490\replacements` and checks it. Options, time and disk, and
what to do when it stops: [HD-BUILD.md](HD-BUILD.md). The rest of this page is
what those steps do and how to run them one by one (Python 3.10 or later with
numpy and Pillow; every tool is in `tools\hd\`).

```
py -3 -X utf8 tools\hd\extract.py disc work\hd\extracted
py -3 -X utf8 tools\hd\tim2_export.py work\hd\extracted work\hd\tim2
py -3 -X utf8 tools\hd\im3_export.py disc work\hd\im3 --scope all
py -3 -X utf8 tools\hd\rg_texkeys.py work\hd\extracted work\hd\keys.csv
    (build_hd_pack.py then merges tim2\ and im3\ into work\hd\all, classifies the
     pictures with rg_flag_classes.py and upscales each class with rg_upscale_batch.py)
py -3 -X utf8 tools\hd\rg_texpack.py build --keys work\hd\keys.csv ^
    --out textures\SCUS-97490\replacements --upscaled work\hd\upscaled ^
    --stubs work\hd\tim2\stubs.csv ^
    --extra work\hd\im3\unique.csv=work\hd\upscaled --native item_icon --clean
```

- `extract.py` reads `DATA0_0/1.DAT` through their `.HD6` indexes. The index
  stores some files 512 bytes late (`--legacy` reproduces the old, wrong
  offsets); with the fixed offsets it writes 18,931 entries, 2,240,810,404
  bytes. `tim2_export.py` leaves out the ~1,130 "pictures" that are only
  64-byte headers of runtime texture slots, and decodes the 8-bit pictures
  stored linear (not GS-swizzled) the right way; `im3_export.py` reads the
  menu art (IM3 containers) straight from the archive and gives each picture
  the renderer's key.
- `rg_texkeys.py` reads every TIM2 picture on the disc and computes the key
  the renderer gives it once the game has uploaded it, bit for bit:
  FNV-1a over the deswizzled indices (8-bit) or the packed nibbles (4-bit),
  seeded with width, height and format, plus the palette window's hash
  (CSM1 order). 8-bit pictures stored linear rather than GS-swizzled get
  both keys; the runtime decides which one exists.
- `rg_texpack.py build` names each upscale by those keys
  (`<texels>_<clut>_<W>x<H>_psm<NN>.png`) and hard-links it into the pack
  (no space used; `--copy` to copy). It skips the pictures that are not images
  (`--stubs`), merges the IM3 menu art and the pictures found inside IM3
  containers by their `gs_key` (`--extra`), and writes `pack-manifest.csv`
  beside the folder. `--clean` removes only the files the previous manifest
  lists, so learned files stay.
- **Per-class models (the default since 2026-10-28).** `rg_flag_classes.py`
  gives each picture a class and `build_hd_pack.py` upscales it with the model
  that suits it: animevideov3 for effects (additive glows, sparks, slash
  trails: x4plus-anime made them greener, bigger and starrier), foliage
  cut-outs and smooth clouds, and the general `realesrgan-x4plus` model for
  the **stone_floor** class (paving, cobbles, flagstones and brick:
  x4plus-anime drew a "cracked mosaic" of invented lines on the side streets
  and flattened the mortar), the rest x4plus-anime. The stone tiles are a
  hand-picked list (`tools/hd/data/stone-floor.txt`, 88 Salgin pictures
  chosen from contact sheets of native against the three models; hashes only).
  IM3 pictures under `effect\` take animevideov3 by the same rule.
  `rg_texpack.py build --no-flagged` builds the all-x4plus-anime pack of
  before. The reference build (2026-10-28) had 5,182 x4plus-anime,
  2,836 + 12 + 133 animevideov3 and 88 x4plus pictures, the rest menu art.
- **fp16 blackouts.** Under ncnn's fp16 math x4plus-anime returns black or
  dark pictures for about 130 bright textures whatever the tile size. The
  builder finds them by colour drift (`rg_upscale_colorcheck.py`, over 25)
  and uses the animevideov3 upscale for those.
- Atlases: for the TIM2 names given with `--native NAME` (default
  `item_icon`, the icon sheet), the builder links the disc's own picture
  beside the upscale as `native_<key>.png` (see Atlases below). Older builds
  of the game skip the file (a misnamed file, logged once).
- `rg_texpack.py validate --keys keys.csv --pack DIR LOG...` reads logs made
  with `RG_TEXTURE_LOG=1` and reports how many of the textures the game built
  the pack names, with a list of the misses and why.

The reference build (2026-10-28, from the primary set minus the decoder's
rejects, the fixed set, the menu art and the newly extracted pictures) had
11,055 files (8,106 primary, 102 fixed, 2,509 newly extracted, 338 menu art),
built in 2 s once the upscales existed, plus 29 learned. The IM3 manifests
write the PSM in decimal (`psm19` is T8): the builder converts their
`gs_key`s to the renderer's hex names.

### Going back to the old look

To compare with the x4plus-anime-everywhere look of before the per-class
models, build into another folder with `rg_texpack.py build --no-flagged
--out textures\SCUS-97490\x4plus-anime\replacements ...` (it hard-links the
same upscales) and start the game with

```
set RG_TEXTURE_PACK=textures/SCUS-97490/x4plus-anime/replacements
```

Without the variable the default pack is used.

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
| Item icon atlas (12 states as icons load) | composed (Atlases, since 2026-10-28 afternoon) |
| Text pages (font, names: the modern font) | 7, left out |
| Pseudo-textures over the frame buffer (one palette, noise) | 7 |

So every disc texture the route showed has its HD picture, apart from the
text pages. The keys are exact: of the first 102 textures
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
py -3 -X utf8 tools\hd\rg_texlearn.py C:\somewhere\learned textures\SCUS-97490\replacements ^
    --source work\hd\all\png=work\hd\upscaled\x4plus-anime,work\hd\upscaled\animevideov3 ^
    --realesrgan toolchain\realesrgan
```

links the upscale of any disc picture with the same pixels, and upscales the
rest with the same model and alpha handling as the main set
(`rg_upscale_batch.py`, about a second per picture), into
`replacements\learned\`. F8 in the game reads them. Text pages (4-bit
512-wide sheets: the modern font draws those), atlases over 512 (the item
icon sheet: an atlas, below) and noise are left out.

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

## Atlases: the item icons

The item icon sheet (`item_icon`, 1024x960 T8 in a 1024x1024 TEX0, every
item's 32x32 icon) never hashes to its pack file once the game runs:

- a TEX_ANIME script (`icon_all2.img`'s `#texanime`) copies one of three
  40x40 frames of an animated icon into the slot at (512,0) (the script's
  y 920 counts from the bottom), from x 552 and 592 of the same sheet, every
  8 to 112 frames;
- a short line of other texels lands in row 798 (x 508-679, under six icons
  of the bottom block) on the GS side; the EE copy of the sheet has the
  disc's texels there;
- in the item menu every other draw of the sheet uses a palette of one
  colour (`cbba0287...`, all entries 0x80 0xFF 0xFF 0xFF): the cyan glow
  behind equipped items.

`work_icon` (256x128) is filled on the CPU (`WorkIconCopyItemIcon`,
0x28DF20) with 32x32 icons copied out of the EE copy of the sheet: the
party's weapons (`WorkIconBuildPartyWeaponsMaybe`, at each close of the main
menu) and the icon of an event's message window (`UiGetWorkIconSprite`).

A pack file with the disc's picture beside it (`native_<key>.png`, RGBA,
alpha 255 opaque, the key's size) is an **atlas**. When a texture of the
atlas's format and palette matches no file, the renderer
(`GsTextureCache::matchAtlas`) composes its HD picture:

1. If its top-left part holds the atlas's texels (at most 1 in 32 differ),
   the texture is the atlas: each texel equal to the disc's takes the HD
   picture at its place.
2. Each 32x32 cell (at multiples of 32) equal to one of the atlas's icons
   takes that icon's HD picture: this composes `work_icon` (a texture at
   least half of whose non-empty cells are an atlas's icons), whatever the
   party carries.
3. Other changed texels are grouped (4x4 tiles that touch) and each group's
   box is looked for in the atlases texel for texel (its own rows first); a
   TEX_ANIME frame is found where the script copied it from.
4. What is left (the stray line in row 798) is the game's texel, enlarged.

The game's later changes re-compose and upload only the 32-texel cells whose
texels changed (the animation's 64x32 runs). A palette of one colour gets a
variant of the composed picture in that colour (the glow, from the HD
icons' coverage); any other palette shows the game's texture. Costs: 36 ms
on the GS worker the first time the sheet is composed (64 MB uploaded, 4096
square at 4x), nothing at a palette flip back (the raw texels' hash is
kept), 64 MB more on the GPU for the glow variant and 64 MB of main memory
for the composed texels. The item menu measured the same frame rate as
before (49.4 against 49.9 fps, hidden window at 4x beside another game).

**The icons' look.** The pack's sheet is the x4plus-anime upscale, like the
rest of the menus: crisp, bold outlines, but metal blades and coins lose
their shading to flat black and white, colours get more saturated and a few
fine icons are redrawn (a snake staff's coils). Two other upscales of the
sheet can be made for taste: with the `x4plus` model (the general one: keeps
the shading and colours, sharpest, a little grain) and with `animevideov3`
(softest, closest to the native colours, some blur) -- run
`tools\hd\rg_upscale_batch.py --model realesrgan-x4plus --only LIST ...` on the
sheet's picture (its pixel hash is `59f235aa3f748836`; the picture is in
`work\hd\all\png\`). To try one, replace the pack file's link (the key stays
the same): `del replacements\menu\a3d85cd21042528c_8cfb075b4730706f_1024x960_psm13.png`
then `mklink /h` it to the new upscale, and F8 in the game.

`RG_TEXTURE_LOG=1` logs `composed at 4x from its atlas`, `atlas block WxH at
X,Y: found at X,Y`, `atlas cells ... composed again` and the palette
variants.

## Not covered

- **Text pages**: the modern font redraws them; an AI upscale of letters is
  not used.
- **Frame copies and render targets** (bloom, the menu's backdrop, shadows):
  not textures from the disc.
- Other partly updated textures without a native picture lose their HD
  picture after the first update (the other `#texanime` destinations are
  runtime-filled slots; `--native NAME` makes any picture an atlas).
- Palettes other than the atlas's and one-colour ones (none seen) show the
  game's icons.

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
