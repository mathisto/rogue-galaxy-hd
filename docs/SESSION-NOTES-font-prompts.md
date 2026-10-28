# Session notes: Xbox prompts, modern font, texture replacements (2026-10-28)

Branch `feature/hd-font-prompts` (from `feature/speedup-quicksave`), local
commits only, nothing pushed. User-facing description:
[FONT-AND-PROMPTS.md](FONT-AND-PROMPTS.md).

## Status per stage

1. **Replacement backend: done, tested.** `GsReplacementStore`
   (`sdlgpu_replacements.*`) keyed by the cache's texel hash (of the whole
   texture or of a top-left part) + palette hash or any palette; PNG packs in
   `textures\SCUS-97490\replacements` (dump names) and pictures registered
   from code, drawn at the render scale. The cache composes an HD texture
   (game texels upscaled, picture over its part), follows later uploads, drops
   it when the part changes or texels are unknown. The backend binds it with
   `misc.y` = scale: texel for texel at the render scale, otherwise filtered
   with a per-sprite REGION_CLAMP (no batching for those sprites). Menu
   switch + `RG_TEXTURE_REPLACEMENTS=0`; `RG_TEXTURE_LOG=1`. Tested: a 3x
   red-tinted gaiji PNG at 2x render scale was read, bound, and drawn without
   bleeding (`14-file-pack-test-3x-red-tint.png`); a misnamed file is logged
   and skipped.
2. **Xbox prompts: done, tested.** Gaiji sheet's 8 icons redrawn
   procedurally (`hd_art.cpp`), same cells. PlayStation / Xbox / Auto (Auto:
   PlayStation pad -> PlayStation, anything else or none -> Xbox). Seen in
   the field prompt (`04-field-prompt.png`). Still PlayStation (baked art):
   help page diagrams and their "Press (X)", the loading recap's "Press (X)".
3. **Modern font: done, tested.** All three pages redrawn from Roboto
   Medium (or `fonts\*.ttf`, `RG_FONT`) at the render scale; widths patched
   in `D_0044E850` for the redrawn types. Found: the normal page is T4HL at
   0x1C00, system T4 at 0x2A00, name T4 at 0x2BA0 (matched by its first 72
   rows). Switching Original/Modern live from the game's Settings works.
   Wrapping: the recap's lines break in the same places as with the game's
   font (`03-loading-recap.png`).

Screenshots (before | after): `C:\Users\mathisto\projects\rogue-galaxy\research\assets\font-prompts\`
(01 to 14; the "after" of 06, 07 and 09 predates the name page, see 13 for
the name font modern). Taken in hidden runs at 2x, scratch settings, a copy
of the card.

## Not tested / known issues

- Render scales other than 2x by eye (generated art follows the scale up to
  4x; 5x-8x use the filtered path). Widescreen with the new font (still
  stretched, as with the game's).
- The launcher font pak (`fontset_launcher_en.pak`) pages: not met; if they
  differ they stay original. Battle text, shops, Revelation Flow, name
  entry: not visited.
- A texture pack's files are read once; no hot reload, no async decode, no
  VRAM budget (design doc 07 items 5-6 not done).
- The game's Settings screen shows option values from the settings file; a
  variable given for one run is shown there too (fixed), but the F1 menu
  shows the file's.
- Speed: the field ran at 54-57 fps in a hidden run at 2x while the machine
  was otherwise busy; no regression seen against the base build's boot
  timing. Not benchmarked with `tools/bench`.

## Running it

The build is `build\game-dev\roguegalaxy.exe` (the objects are in
`build\game`; `build\game\src\roguegalaxy.exe`, the player's, was not
relinked). To play this branch: `cmake --build build\game` (only relinks),
then start `Rogue Galaxy.exe` as usual and pick the options under F1 > Game >
Text and prompts. Testing aids added: `RG_INPUT_FILE` (drive a hidden run
step by step), `RG_UI_SCRIPT scrollto KEY`.
