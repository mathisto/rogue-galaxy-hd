# Session notes: HD texture packs (2026-10-28)

Branch `feature/hd-textures` (from `feature/hd-font-prompts`), local commits
only, nothing pushed. User-facing description: [TEXTURE-PACKS.md](TEXTURE-PACKS.md).

## What was done

1. **Pack infrastructure** (`sdlgpu_replacements.{h,cpp}`, `sdlgpu_textures.*`,
   `gs_draw.frag`, `src/graphics.cpp`):
   - the folder is listed on a worker (names only; 11k files in 15-36 ms);
   - PNGs are read on two workers on first use (`GsPictureStatus::Pending`;
     the cache keeps the game's texture and asks again when the store's
     completion counter moves); 8-11 ms per 4x picture;
   - decoded pictures in an LRU under half the budget; uploaded whole-texture
     pictures pooled on the GPU by key (`m_hdPool`) and shared by entries, so
     the game's re-uploads of other textures into the same pages cost a
     pointer swap (before the pool: 6,282 re-uploads in one walk and 23 fps
     with logging);
   - budget (`RG_TEXTURE_BUDGET_MB`, menu "Texture pack memory", 2 GB): room
     is made before an upload (idle pool items, then textures not drawn for
     2 s); if only pictures in use are left the new texture stays native and
     retries after 5 s (no thrashing: 15 fps -> 60 fps at a 64 MB budget);
   - uploads spread over frames (32 MB per 16 ms);
   - F8 / `RG_TEXTURE_POLL` reload; unchanged files keep their decoded picture;
   - generated mip levels for power-of-two scales, chosen in the shader by
     footprint (3D draws only), cut-out alpha hardened at half;
   - matching made cheap: a shape index instead of a walk over all files,
     `has()` before any expansion, parts only for non-power-of-two sides;
   - learn mode (`RG_TEXTURE_LEARN`), `RG_TEXTURE_PACK`, input-file
     `pack on|off` and `key NAME` for tests.
2. **Key mapping**: `tools\rg_texkeys.py` (research folder) computes the
   renderer's identity for every TIM2 picture; `tools\rg_texpack.py` builds
   and validates the pack; `tools\rg_texlearn.py` completes it from learn
   mode. The hash reproduces the cache bit for bit (first runtime check: 101
   of 102 disc textures with a texel match also matched the palette hash).
3. **Sampling**: REPEAT that does not wrap clamps like CLAMP for replaced
   sprites (dots between the item menu's slots); see TEXTURE-PACKS.md.

## Measurements

See TEXTURE-PACKS.md (Performance) and the final report numbers below.

## Known issues / not done

- Item icon atlas (1024x1024 T8, refilled as menus open) and `work_icon`
  (CPU-filled) stay native; a hook on the icon copy (0x28DF20) would be the
  way.
- Text pages: left to the modern font; learn mode and the learn tool skip
  them.
- Palette-animated textures: exact palettes only (3 of 177 in the walk).
- Faces whose eyes and mouth the game paints in: one state per learned file.
- BC7/BC3 not done (no encoder in the build; budget not reached in the
  measured scenes).
- Numerals (TTF digits for the pause menu) not done: the digit sheet
  (`5fd0e1dbd5ef4f1c_829bf24ac2eba6cc_256x128_psm14`, confirmed at runtime)
  is covered by its AI upscale instead.
- The other agents' manifests write PSM in decimal (`psm19`); the builder
  converts (`--extra-psm-hex` if a manifest is hex).
- `build\game\src\roguegalaxy.exe` (the player's) was relinked once by
  `ninja` early in this session (04:31) after the player had closed the game;
  it then held an early state of this branch. Relink it from the final
  branch (`cmake --build build\game`) before playing.

## Running the tests

Hidden runs from a copy of the exe (`build\game-dev\rg-hdtex-a.exe`), scratch
settings (scale 2, 1280x960), a copy of the memory card, `RG_INPUT_FILE`
driven by a PowerShell script: title, load File 01, Salgin, walk, menus,
with `pack off` / `pack on` before/after pairs taken a moment apart.
