# Session notes: speed-up, quick save, upscaling (2026-10-27)

Branch `feature/speedup-quicksave`, local commits only (nothing pushed).

## Status per goal

1. **Speed-up hotkeys: done, tested.** R2 held = fast-forward while held,
   L2 = on/off; keyboard ` (left of 1) held, F4 on/off as before. Speeds
   1.5x, 2x, 3x, 4x, 8x, Max (unpaced) in F1 > Interface and the menu bar's
   Game menu, saved (`fast_forward_speed`, `trigger_fast_forward`);
   `RG_FAST_FORWARD_SPEED`, `RG_FAST_FORWARD_TRIGGERS` for one run. Composes
   with quick scenes (menu x scene); movies are held at 1x. Audio: the IOP
   runs in step with the audio device, so music and sounds keep their tempo
   and nothing queues up; only movie sound could drift (its packets come from
   the EE), hence the 1x hold. Tested hidden: every key path, free-camera
   pass-through, 4x at the logos gave 100-115 fps against 30, movie held at
   30 fps.
2. **Quick save/load: not shipped.** A whole-machine snapshot is not feasible
   (10 of 11 guest threads sit suspended inside host call chains on fiber
   stacks); the game's own save only holds save-point state and loads only
   through the title. No save on this PC's card to test with. Details and a
   way forward: `docs/QUICKSAVE.md`.
3. **Graphics: FSR 1 added, DLSS ruled out.** Display > Scaling filter "FSR"
   (EASU, when enlarging) and Display > Sharpening Off/Low/Medium/High
   (RCAS), off by default. Checked on the title screen only. Why not DLSS or
   FSR 2+: `docs/GRAPHICS-UPSCALING.md`.

Also: `DQ8_GFX_HIDDEN_WINDOW=2` composes a hidden window's frames off screen
so menu scripts and screenshots work unattended; `RG_INPUT` `HOST` steps
press host-side buttons; `RG_UI_SCRIPT` gains `keydown`, `keyup`,
`fastforwardheld`, `sharpening`, `filter fsr`; `[rg] speed` log lines.

## Files

New: `src/hotkeys.{h,cpp}`, `shaders/present_{easu,rcas}.frag`,
`docs/QUICKSAVE.md`, `docs/GRAPHICS-UPSCALING.md`, this file. Changed:
`src/graphics.cpp`, `src/scene_timing.{h,cpp}`, `src/debug/input_script.*`,
`src/CMakeLists.txt`, dq8-gfx `ui_overlay.*`, `ui_pages.cpp`,
`ui_script.cpp`, `ui_settings.*`, `sdlgpu_backend.cpp`, `sdlgpu_device.*`,
`sdlgpu_display.cpp`, `sdlgpu_window.h`, `CMakeLists.txt`, `README.md`;
README, OPTIONS, DEVELOPMENT, CHANGELOG.

## Build

`toolchain\` on PATH (cmake, ninja, llvm-mingw, python), then
`cmake --build build/game` (or `python setup.py build`). A change to dq8-gfx's
headers rebuilds most of the runtime (about 9 minutes here).

## Known issues and untested

- While L2/R2 drive fast-forward the game never sees L2/R2 (or Left/Right
  Shift). If some game function needs them, turn the option off.
- Max speed's gain depends on the PC; under the CPU load from other jobs
  during this session the title could not even hold 60 guest frames a second.
- FSR and sharpening were not looked at in a 3D scene, in widescreen or with
  Blurred/Image borders (no save on the card to get in game).
- The menu copy was not run through the impeccable skill's audit.

## Recommendations

- Play-test L2/R2 in battle and the field to confirm the game does not need
  them; if it does, default the option off.
- For quick save, start with the "open the game's own save screen anywhere"
  hotkey described in `docs/QUICKSAVE.md`, tested with a real save.
