# Session notes: polish round 2 (2026-10-28)

Branch `feature/polish-2` (from `feature/hd-icons`), local commits only,
nothing pushed. Screenshots and before/after sheets:
`research\assets\polish-2\` (the author's notes, outside the repository).
Test build `build\game-dev\rg-polish2.exe` (`rg-polish2-base.exe` = the
branch's start); the player's `build\game\src\roguegalaxy.exe` was running
throughout and was not relinked.

## 1. Effects look and stone floors (done)

- **Default per-class models.** `tools\rg_texpack.py` (research folder) now
  applies `flagged-classes.csv`'s recommended set by default (was
  `--fallback-for-flagged`): animevideov3 for `effect_additive` (research 14:
  x4plus-anime made glows greener/bigger/starrier, sparks bigger),
  `foliage_cutout` and `smooth_cloud`. Effects among the newly extracted
  pictures (`all3`, 133, which flagged-classes.csv does not list) now take
  `animevideov3-extra` by the same path rule. `--no-flagged` builds the old
  pack.
- **stone_floor class.** 88 opaque Salgin (`map\m01`) tiles picked by eye
  from contact sheets of the 333 opaque m01 pictures: paving, cobbles,
  flagstones, brick. Compared native / x4plus-anime / animevideov3 /
  realesrgan-x4plus crops (`01-stone-*.png`): x4plus-anime draws dark
  "crack" lines and smooths mortar; animevideov3 is soft and smeary;
  **x4plus** (general model) keeps stone shapes, mortar and the native
  colours. Upscaled with the local RealESRGAN (`rg_upscale_batch.py --model
  realesrgan-x4plus --only`, 10 s) into `work\hd\upscaled\x4plus\`;
  list in `upscaled\stone-floor.txt`; `rg_flag_classes.py` reads it
  (backups `*.orig-2026-10-28`, `flagged-classes.csv.bak-2026-10-28`).
- **The player's pack** rebuilt by the builder (hard links): 11,055 files,
  3,069 changed (2,836 + 12 + 133 animevideov3, 88 x4plus), learned files
  untouched. Old index kept as `textures\SCUS-97490\pack-manifest.before-polish2.csv`.
  **Switch back**: `RG_TEXTURE_PACK=textures/SCUS-97490/x4plus-anime/replacements`
  (the old pack, built with `--no-flagged`, learned files linked in).
- In game: jungle foliage before/after (`01-jungle-foliage-old-vs-new-pack.png`,
  pack swapped live with F8); the side street with the new paving
  (`01-ingame-side-street-new-pack.png`). An effects pair in battle was not
  captured cleanly (battles are real time; the tutorial boards and a game over
  got in the way); the effect change is the same pictures research 14 judged.

## 2. Remaining PlayStation art (done for the boards and pause)

`hdart::drawButtonPatches` gained kinds: `Pad` (the controls boards'
DualShock drawing replaced by an Xbox-style pad drawn by code, parts placed
where the DualShock's were so the boards' lines still land; the DualShock's
cable and grip tips are filled from the board, the boards' lines kept over
the body, never drawn over the boards' figures or lettering), `Menu`, `View`,
`LS`, `RS`, `DPad` (icons on a cleared disc) and `Word` (a word of the
picture's lettering redrawn in Roboto in its colours: "START" -> "MENU",
"L3"/"R3" -> "LS"/"RS", "Press the START button" -> "Press the Menu button").
Boxes: `rg_psart_scan.py` with `EXTRA=` templates (start/select/dpad/stick
cut from Loading_TGS, kept in `the author's work folder (templates cut from the game's pictures, not in the repository)`, not
in the repo; scan csv beside them) plus `tools/rg_psart_manual.txt`;
`rg_psart_emit.py --base` merges into the existing table (149 boxes, 46
pictures). Offline checks with a small harness compiling `hd_art.cpp` alone.

Verified in game (Xbox): Help > Controls -Movement- and -Battle-, the field
pause ("MENU button: Continue", X), the tutorial boards' "Press the Menu
button", the event pause. PlayStation setting: boards unchanged
(`04-sanity-playstation-prompts-board-unchanged.png`).

Not done / known:
- The FMV pause ("START button: Continue" over a movie) still says START: it
  is drawn from another picture than the battle sheet; not traced (movie code
  is another agent's area).
- The boards keep the game's text "LS button: ..." in the redrawn word only;
  hb0011's large R1, hb0312's tabs, Fac_ope01a, the Japanese copies' words
  stay as before. The in-board screenshots (Suggestions board's "SKIP: L3
  button") are photos and excluded.
- Tried and reverted: binding the pack's upscale of the tutorial boards (they
  stay native in battle because their 512x512 TEX0 is expanded on the GPU
  from a target's pages and 28 of the part's pages are target-held, so no
  name). Generated art still draws on them where the texture is named.

## 3. Battle digits (done; results screen not seen)

`kDigitSheets` gained btl_st02 (damage popups, `{0,0,25,20,"/",0.2,outline 2}`),
btl_st01 (cyan row y 89, big orange row y 227), b_m_base (HUD HP/AP, big y 80
and small y 98 with '/'), b_end_base (results Exp/Skills y 2 with '/', levels
y 23). `DigitRow` has `outline` (texels) and `widen`; thick-outlined rows
grow the glyph strokes. Sheets: `03-*-before-after.png`.
Verified in game (new game, Baphu tutorial battle, scratch card): damage
popups and HP/AP in Roboto (`03-ingame-*`). Not seen: critical/heal popups,
combo counter, results screen (the tutorial battle ends in a scene; the save
in the jungle met no battle with `RG_FORCE_ENCOUNTER`).

## 4. Sanity pass

Items menu (icons HD, counts and money redrawn), Strategy menu, field and
battle pause, two battles' HUD, PlayStation prompts: no regressions seen.
Shops not reached. The movie and its pause unchanged.

## Test method

Hidden runs (`DQ8_GFX_HIDDEN_WINDOW=2`) launched through `cmd /c` so stdout
lands in a log, `RG_INPUT_FILE` steps, `F12` shots, scratch settings
(1280x960, 2x) and a copy of `disc\mc0` in the session scratchpad; pack
before/after by hard-link swaps in a scratch pack folder
(`textures\SCUS-97490\_cmp`, removed afterwards) and F8.
