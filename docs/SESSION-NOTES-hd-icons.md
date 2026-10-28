# Session notes: HD item icons (2026-10-28)

Branch `feature/hd-icons` (from `feature/polish`), local commits only,
nothing pushed. User-facing description: [TEXTURE-PACKS.md](TEXTURE-PACKS.md)
(Atlases). Screenshots: `C:\Users\mathisto\projects\rogue-galaxy\research\assets\hd-icons\`.

## What was found (hidden runs, `RG_TEXTURE_LOG=1`, learn mode)

- The item icon sheet is `tbp0=02a00 tbw=16 psm=13 1024x1024`, palette
  `8cfb075b4730706f`. Its 1024x960 part hashed `19738f4c...`, `3739af1c...`,
  `7850d41b...`, `a61800eb...`, `3097f8ff...` over a session, never the pack
  file's `a3d85cd2...`. A learn-mode dump against the disc's picture: 700
  texels differ, in two places:
  - the TEX_ANIME slot, 40x40 at (512,0), holding the frame from (552,0) or
    (592,0) (the script's y 920 counts from the bottom of the 960 rows);
  - row 798, x 508-679: a line of other colours (frog purple among them)
    under six icons of the bottom block, written on the GS side and changing
    every frame in the menus. The EE copy has the disc's texels there
    (`work_icon` copies of those icons match the disc).
- Every other draw of the sheet in the item menu uses palette
  `cbba028790f4ded8`: 255 entries of 0x80 0xFF 0xFF, alpha 0xFF (the cyan
  glow behind equipped items, drawn from the same texels). It flips each
  frame, so the cache re-expands the whole sheet twice a frame (as before).
- `work_icon` was not drawn on any route reachable from the only save (a town:
  no field HUD, no battles); a state learned in an earlier session
  (`replacements\learned\8ca57e9dbdebc6fe_8cfb075b4730706f_256x128_psm13.png`)
  holds three weapon icons, all three found among the sheet's 876 icon cells.

## What was done

- **Atlases** (`sdlgpu_replacements.*`, `sdlgpu_textures.*`): a pack file
  with `native_<key>.png` beside it; the cache composes a texture from the
  atlas's HD picture where its texels are the disc's, from icon cells and
  found blocks where they moved, the game's texels elsewhere; patches by
  32-texel cells; one-colour palette variants. Details in TEXTURE-PACKS.md.
- **Pack tool** (`tools\rg_texpack.py`, research folder): `--native NAME`
  (default `item_icon`) links the disc picture beside the upscale. The
  user's pack got the file by hand (`keys.csv` of the last build is gone; a
  test build of the menu-art set into scratch produced the same file).
- **Alternative upscales** of the sheet for taste: `Q:\work\rg-textures\ui\upscaled\x4plus\`
  and `animevideov3\` (`rg_upscale_batch.py --only`), 2-4 s of GPU each.

## Verified

- Items menu, allies (equipment slots), the developers' icon grid
  (`RG_DEV_TOOLS=1`, R3 in the main menu: every icon, five pages of weapons
  and armour), the menu closed and opened again, pack off/on: HD every time
  (`01`-`05` in the screenshots). The animated slot's frames were found at
  (559,3) and (599,1) and re-composed as they came.
- The glow: native silhouette before, HD variant after (`04`).
- Frame rate in the item menu, 20 s, no logging: 49.9 fps before
  (`rg-polish.exe`), 49.4 fps after; first composition 35-37 ms on the GS
  worker (once per pack load), no re-composition when the menu reopens.

## Not verified / known issues

- `work_icon` in the game (field HUD outside towns, battle, event message
  windows): the composition path is the same as the sheet's cells, and its
  premise holds on the learned state, but no run drew it. With a save
  outside a town: `RG_TEXTURE_LOG=1`, close the main menu, look for
  `composed at 4x from the icons of a3d85cd21042528c`.
- Shops, the battle item menu and field pickup pop-ups were not reached
  (town save: no battles; the shop markers were not reached by the scripted
  walk). They draw from the same sheet and `work_icon`.
- Event item pictures (43 32x32 pictures, `event\item`): in the pack by key,
  upscales checked by eye (`08`); not seen in a run.
- Row 798's stray texels stay as the game draws them (enlarged), as on the
  console's GS presumably; not checked against PCSX2.
- Look: x4plus-anime flattens metal shading on weapons and coins
  (`06`, `07`); x4plus keeps it. The pack keeps x4plus-anime (the menus'
  style); swapping is one link (TEXTURE-PACKS.md).
- Memory: 64 MB GPU for the sheet at 4x, 64 MB for the glow variant, 64 MB
  of main memory for the composed texels.

## Running the tests

`build\game-dev\rg-icons.exe`, hidden window (`DQ8_GFX_HIDDEN_WINDOW=2`),
scratch settings (4x, 1280x960), a copy of `disc\mc0`, `RG_INPUT_FILE`, the
scene trace to know when the field runs (`mode 6 phase 100`), `pack off` /
`pack on` for before/after pairs.
