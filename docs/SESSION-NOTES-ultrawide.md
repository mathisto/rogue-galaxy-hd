# Session notes: ultrawide (2026-11-04)

Branch `feature/ultrawide` (from `feature/controller-art`), local commits only, nothing pushed, no remote touched. User docs:
[ULTRAWIDE.md](ULTRAWIDE.md) (what each option does, how it works, what was checked, the gaps, the HUD-layer estimate),
[OPTIONS.md](OPTIONS.md), [MENU-DESIGN.md](MENU-DESIGN.md) ("Game: the ultrawide rows"), the README's feature list, the CHANGELOG,
[DEVELOPMENT.md](DEVELOPMENT.md) (the test variables), THIRD-PARTY-NOTICES.md ("Ideas credited, no code taken").

## What was built

- `src/ultrawide.cpp`: the HUD mapping (hooks on the task table, the six GS vertex writers, `GsPktWriteAD` for scissors,
  `ViewSetScreenMatrix` for the compass, the pause text, and a few world-mark functions), `src/camera_view.cpp` (the view cap),
  `src/graphics.cpp` (three Game rows, three settings keys), `src/widescreen.cpp/.h` (`aspect()`).
- Options and defaults: HUD area 16:9 / 21:9 / Full width (default 16:9), HUD placement Centre / Edges (default Centre), Ultrawide
  field of view No limit / Up to 21:9 / Up to 16:9 (default No limit). Keys `hud_area`, `hud_placement`, `ultrawide_fov`; the
  existing `field_of_view` slider keeps its meaning (degrees across 4:3), and the cap is its own row (the slider was not
  redefined as horizontal degrees at 16:9: that would have changed what every saved value means).

## What was learned on the way (the things that cost runs)

- **A hook that replaces a function and does not call the original must set `ctx->pc = ra`.** Without it the caller sees the
  call as unwound and returns early: every HUD vertex written by my writers made the draw vanish until this was set.
- **Packets are not all in RDRAM**: the scratchpad (0x70000000) holds some. Guest memory goes through `PS2Runtime::Load/Store`
  and `Ps2FastRead/Write`, not `rdram + (address & mask)` (a crash writing 0x1effbff9b10).
- **"Stalled" in the harness was waiting for a button.** The loading recap's "Press A" needs two Cross presses (the first one
  that lands before it is ready is lost); the stall watchdog's report ("main thread has not moved for 15 s") fires there. Four
  runs were spent hooking and unhooking on that.
- **A 512-wide frame holds more than the picture.** The minimap builds its map in 16x16 strips written into the picture
  (x 128..240, y 144..336) and then reads them back with a polygon by position; the draw target's origin (1792 pixels for the
  picture, 1952 for a treasure box's message buffer, 0 for marks in world coordinates) and a list of the strips' call sites
  keep those out. A full-width sprite (a fade, a filter, a copy of the screen) must stay whole too, or it shrinks to the box
  and the scene is gone at the sides.
- **Do not patch a packet after its frame.** Vertices are placed at the end of the table run (a `callThen` on
  `TaskTableRun`); what a run left unplaced is dropped, not placed, so a stale address never writes into the next frame.
- **`aspect = stretch` in the player's settings makes every 4:3 screen stretch** (title, menus), Widescreen menus or not: it
  is the Display option "Fills the window". With Auto the title, the loading recap and the menus are 4:3 between the Borders
  fill at 21:9 and 32:9 (checked, below).

## Verified

Hidden window (`DQ8_GFX_HIDDEN_WINDOW=2`), scratch `settings.ini` (aspect Auto, internal scale 2), a copy of `disc\mc0` (File 01,
Juraika: Path to Burkaqua, chapter 2, the party at the first transporter) as `RG_MC0_DIR`, `RG_INPUT_FILE` steps (a screenshot
with `shot`), the player's `settings.ini`, card, `textures\` and `disc\` untouched. Build: the changed objects with ninja, then
`build\game-dev\rg-ultrawide.exe` linked from the objects of `build\game` (the link command of `ninja -t commands`); the player's
`build\game\src\roguegalaxy.exe` was never relinked. Every run was looked at.

See the "What was checked" section of [ULTRAWIDE.md](ULTRAWIDE.md) for the table of sizes and areas, the 16:9 identity check and
what could not be reached.

## Time

See the final report of the session (wall clock, tool calls, builds, runs).
