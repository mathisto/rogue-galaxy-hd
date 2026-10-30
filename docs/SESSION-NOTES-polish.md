# Session notes: polish batch (2026-10-28)

Branch `feature/polish` (from `feature/hd-textures`), local commits only,
nothing pushed. Screenshots (before/after): 
`research\assets\polish\` (the author's notes, outside the repository).

## 1. Numerals (done, verified)

The menus' numbers are digit sprites (research 11), in four sheets found by
`RG_TEXTURE_LOG=1` in the pause menu, main menu, items, allies and load
screens:

| Sheet | Texels (key) | Rows redrawn | Numbers |
|---|---|---|---|
| `topmenu_02a` (EN, and the JP-label copy) | `5fd0e1dbd5ef4f1c`, `1f1ea76d537d92ce`, T4 256x128 | y 0/16/32, pitch 12/12/10, plus `/` `:` | play time, item counts, levels, HP/AP, stats |
| `Itm_05` | `394b14f4a10e95a0`, `6a9f43a50179bff6`, T8 | y 105, pitch 14, italic | money (items, shops) |
| `TP_save02` | `4919d4a889d9698d`, T8 | y 0, pitch 14 | save/load file numbers, play time |
| `TP_top_02a` | `68940e5ee5d6687b`, T8 256x64 | y 0, pitch 12 | save/load levels |

`hdart::drawDigitRows` measures the ink box, outline colour and per-row fill
colours of each row from the game's texels, then draws Roboto (or the
player's font) digits in the same boxes with a texel-wide outline. Drawn
over the pack's 4x picture of the sheet when there is one
(`setGenerated(..., overFiles=true)`, new in the store). Only with Font:
Modern. Not done: battle damage popups and the battle HUD digits
(`btl_st01/02`, `frame`), the factory's `Fac_ope01a`, shop sheets not
visited. Screenshots: `01-numerals-*`.

## 2. Painted-in PlayStation buttons (done for 114 buttons in 45 pictures)

`tools/rg_psart_scan.py` matches gaiji2's eight icons (inner part, nine
scales, NCC) against the 529 exported UI textures; the annotated contact
sheets were checked by eye, false hits excluded in
`tools/rg_psart_exclude.txt`, and `tools/rg_psart_emit.py` writes
`src/button_art_table.inc` (boxes only). With prompts Xbox, `hdart::
drawButtonPatches` draws the Xbox button over each box, over the pack's
picture. Verified in game: loading recap Press (A), pause screen, field
prompt, help boards hb0290 (controller diagram) and hb0010; PlayStation
setting leaves them as they were.

Remain PlayStation: "START"/"SELECT" button pictograms and words, the
controller drawing itself (hb0290/hb0300/Loading_TGS), L3/R3 stick art,
icons in the unusual styles the match misses (hb0011's large R1, hb0312's
L2/R2 tabs below the 0.82 cut, `Fac_ope01a`), `hbstart` (text only), any
picture not in the UI export (textures outside the menu/rc folders).

### Compass, prompts, minimap (the user's reports)

- **Transporter prompt PlayStation with Xbox selected**: the player has the
  HD pack, whose `meswin\ef8062d131c0ea57_..._psm13.png` (gaiji2's upscale,
  PlayStation icons) won over the generated Xbox sheet ("a file wins").
  Same for the normal font page (`2bad8e99ac5c8351`): with the pack the
  Modern font never showed in dialogue. Both now draw over the file. The
  player's `settings.ini` does hold `button_prompts = 1` and
  `font_style = 1` (written 04:08 today; the menu saves 0.75 s after a
  change and at exit), and their log said `button prompts: Xbox`: the
  setting was right, the pack hid it. Auto's choice is now logged and
  toasted (`[font] Button prompts: Xbox (Auto: XInput Controller #1 is not a
  PlayStation controller)`). Not tested with a real PlayStation pad (none
  here); `RG_BUTTON_PROMPTS=playstation` shows the PlayStation set.
- **Compass pixelated**: its textures (`rc/compass.chr` compas, hari, moji)
  are T8 uploads into pages an off-screen target owns, so the cache built
  them on the GPU from the target and never named them; no pack picture
  could bind. Now named from local memory when the CPU wrote every word
  (`nameFromLocalMemory`); the ring takes its 4x upscale, the letters
  (`moji`, not in the pack) are redrawn in the modern font. Other HUD icons
  in those pages bind too (log: `from a target's pages, named ...`).
- **Minimap blotches and cell lines**: the minimaps' opaque texels are alpha
  0x7E, so the cache did not take them for cut-outs and the AI upscale's soft
  alpha (up to 190 of 255 where the game's is 0) showed as grey smears and
  faint tile edges. Cut-out detection now counts 0x7C-0x80 as opaque, so the
  pack picture gets the hard edge. Crops: `03-minimap-*`. Faint lines where
  two map cells meet can remain where the upscale itself drew them.

## 3. Supersample downscale (done; not the default)

`present_downscale.frag`: separable Lanczos-2 over each output pixel's
footprint. Crops at 4x and 8x (`04-downscale-*`): cleaner railings and
thin rules than Smooth, a small difference in stills. Not measured: cost on
a slow GPU, motion shimmer (needs the user's eye).

## 4. Widescreen menus (done)

`wide()` answers 4:3 in game modes 14, 16, 17, 19, 20; verified at 21:9
(2520x1080): main menu, items, allies, Settings pillarboxed; field wide
again on leaving. Screenshots record the picture only (the 4:3 part), so the
bars are not in `05-*` (`*-pillarboxed` are 1440x1080, `*-stretched`
2520x1080). Not visited: shops (mode 6 with a shop window? they may stay
wide), save menu at a transporter, factory, insect, ship menus.

## 5. Vulkan layers (done, partly verified)

`RG_ALLOW_VK_LAYERS=1` in the launcher and the batch file; GPU debug labels.
Verified that the loader inserts the implicit layers with it and the game
reaches the title; the launcher's own change was compiled with the final
build but not run (it starts the player's exe). ReShade/RenderDoc not
installed.

## 6. Auto internal resolution (done, verified in hidden runs)

`Overlay::updateAutoScale` (each frame): window height in pixels
(`SDL_GetWindowSizeInPixels`), settled for 0.5 s, gives
`ceil(h * oversample / 448)` clamped to [1, cap]; asked of the renderer
live (`requestResolutionScale`, the same path as the menu's manual change,
no restart). Settings: `scale = auto`, `manual_scale`, `auto_oversample`,
`auto_cap`; `RG_GS_SCALE=auto` for a run (not saved). Verified: 5120x1440
-> 4x (1792 lines), 1920x1080 -> 3x switched live from 2x, 2560x1440 at
1.5 -> 5x; log `[ui] internal resolution Auto: 4x, 1792 lines for 1440`.
Not verified: a real window resize, a monitor move, fullscreen toggle (the
hidden window does not resize), the menu rows by eye.

Frame rate at 5120x1440 widescreen, Salgin plaza, hidden window, 15 s
windows of `[gfx]` lines (fps, backend = GS worker's share of the wall):
3x 50.2 fps / 74% (min 34.5), 4x 52.9 fps / 69%, 5x 58.4 fps / 30%. These
are not trustworthy: the player's game (5x, 1.7 GB) ran on the same GPU and
CPU throughout, the hidden window composes off screen, and the numbers do
not rise with the scale as they must. The battle measurement did not happen:
the save stands in Salgin's town, where no battles start
(`RG_FORCE_ENCOUNTER` needs enemies on the map). To repeat with the game
closed: a hidden run with `DQ8_GFX_STATS_EVERY=60`, `key F9` marks around a
15 s field window, then (from a save outside a town, `RG_FORCE_ENCOUNTER=1`)
walk until `[encounter] battle starts` and mark a battle window.

## Test method

Hidden runs (`DQ8_GFX_HIDDEN_WINDOW=2`) of `build\game-dev\rg-polish.exe`
(`rg-polish-base.exe` the branch's start, for "before"), scratch settings,
a copy of `disc\mc0`, `RG_INPUT_FILE` steps; the battle checker's
`roguegalaxy-hd.exe` was not touched. The player's game was running
throughout (PID 7804); its frame rate competed with the test runs.
