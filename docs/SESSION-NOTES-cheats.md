# Session notes: cheats (2026-10-28)

Branch `feature/cheats` (from `feature/hd-movies`), local commits only, nothing
pushed. User docs: [CHEATS.md](CHEATS.md). Test build
`build\game-dev\rg-cheats.exe`; no `roguegalaxy.exe` was running when
`build\game` was relinked (it now holds this branch). Screenshots:
`research\assets\cheats\` (the author's notes, outside the repository).

## What was built

- `src/cheats.{h,cpp}`: the hooks and the per-frame work (see the header for the
  address map). `GameFrameUpdate` (0x27C4C0) is the heartbeat (the obvious
  `GameDataTick` 0x1FEBD0 is only called at session start and on map warps);
  `CharaSetHp` (0x3B8040) is where damage lands; `ItemConsume` (0x3D41B0) is
  filtered by its two callers in `ItemUseMaybe` (the return address is the
  `jal` + 8: the first attempt used the `jal` address and never matched).
- The game's data: the ten playable characters live in a pool under
  `GameData+0x14` (not in the party structure at 0x483820, which only has the
  active ids), the enemies in a second pool under `GameData+0x1C`; both found by
  reading `CharaMgrPoolFindById` / `ObjFindById`. Fields: +0xC id, +0x10 level,
  +0x14 HP, +0x18 AP ("MP" in the symbols), +0x20 max HP, +0x26 max AP, +0x1C4
  action gauge, +0x4B8 experience. All checked against the battle HUD, the
  party list in the items screen and the save screen.
- Menu (`thirdparty/dq8-gfx/src/ui`): a Cheats page (the options whose key
  starts with `cheat`, drawn by the Game page's own code, `drawGameOptions`),
  `GameOption::action` (a button instead of a switch), the first-time notice,
  F5, the "CHEATS ON" badge, a toast per refused save, `settings cheats` and
  `cheatsnotice accept|decline` for `RG_UI_SCRIPT`. `src/graphics.cpp` declares
  the options and routes them to `rg::cheats`. Changing the UI headers
  rebuilt the UI library only (about two minutes), not the whole runtime.
- Saving: `McSaveStep` (0x374110) is hooked to finish at once with the result
  word 8, which the save screen reads as "Save to file N failed."; the libmc
  writing calls are sent to a port that does not exist as a second line of
  defence. The first version only refused the card calls and the game said
  "File 1 saved." and then "Data corrupted." (the card was untouched, but the
  message was wrong); writing 0xC into the result, which the step itself uses
  for "no game data", said "File 1 saved." too. 8 is the one that fails.

## Verified in the game (hidden window, scratch settings, a copy of the card)

Each in a run with the Juraika save (Jaster, Kisala, Zegram), logs from
`RG_CHEATS_LOG=1`:

- Infinite HP: damage refused in battle (167 would be 141); HUD full.
- Infinite AP, full action gauge: held (HUD, log).
- One-hit kills: the three enemies of the first battle went to 0.
- Items are not used up: Heal Potion 21 -> 20 with it off, stayed 20 with it on
  (the HP still came back).
- Money: held at 5,000,000 (items screen), set to 99,999,999 (log).
- Master off at every start even with `cheats_master = 1` in settings.ini;
  F5 on and off (toasts, badge); the notice, "accept" saved as
  `cheats_notice_seen`.
- Saving: with the master on, the save screen answers "Save to file 1
  failed." and the card's files have the same checksums; with it off, a save
  goes through and the card changes.

## Not verified, not shipped

See CHEATS.md. In short: setting levels (the game's `CharaRecalcStatsFromLevel`
and `CharaAddExp` did not work when called from a host hook: stats came out
0/0, and a character was left at 0 HP until the next run; a first version that
shipped it was removed), weapon charge (`EquipSlotDrainGauge`/`OnUse` hooks
worked but the gauge was always 0.0 in the battles tried), walking speed, unlock
flags. A bug found and fixed on the way: with `RG_CHEATS_LOG=1` the frame work
ran without the master switch and applied the cheats; the writes are now
gated on the master.

## Calling the game's own functions from a hook (lessons)

- A recompiled function that ends in `jr t9` (a tail call through a register)
  returns to its caller with the target in `ctx->pc` instead of running it;
  a host caller has to follow `ctx->pc` until it reaches the return address it
  set in `ra`. Even so, the level functions returned wrong results, which was
  not tracked down. If someone picks this up: compare with the game's own call
  of `CharaApplyLevelUps` from `BattleCalcResults` (0x3A2D38).
- `CharaAddExp` leaves `a2` alone and `CharaApplyLevelUps` writes through it
  when it is not null: a host caller must pass 0.

## Test method

`build\game-dev\rg-cheats.exe` from the repository folder (killed by image
name, which only this build has), `RG_MUTE=1`, `RG_GS_SCALE=2`, `RG_FPS=30`,
`RG_TEXTURE_REPLACEMENTS=0`, a copy of `disc\mc0`'s File 01 as `RG_MC0_DIR`,
`RG_SETTINGS_FILE` in the scratch folder, `RG_INPUT` (the bench's load script
for File 01) then `RG_INPUT_FILE` appended to step by step, a Cross after the
recap (the game waits for "Press X"), `RG_FORCE_ENCOUNTER=1`, a walk of
`LLEFT:90`, `LUP:90` for a battle; the main menu on Triangle (Items, Cross, a
potion, Use, a target); the save point's menu on Cross at the start spot.
Screenshots are BMP in this SDL: converted to PNG to read. Menu screenshots with
`RG_UI_SCRIPT` ("97s screenshot menu"): the menu pauses the game, so the
guest-frame clock stops; use wall times.
