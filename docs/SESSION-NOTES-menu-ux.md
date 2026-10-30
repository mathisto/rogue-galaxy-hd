# Session notes: menu UX (2026-10-29)

Branch `feature/menu-ux` (from `feature/cheats`), local commits only, nothing
pushed. Why and what: [MENU-DESIGN.md](MENU-DESIGN.md) (critique, old-to-new
map, rules). Test build `build\game-dev\rg-menuux.exe` (the new menu; the
"before" screenshots came from `rg-menuux-base.exe`, the old menu plus the test
aids, which is this branch's first commit, `ce7de3e`, and was deleted
afterwards). `build\game` was relinked from this branch (no `roguegalaxy.exe` was
running when it was). Screenshots, `before_` and `after_`:
`research\assets\menu-ux\` (the author's notes, outside the repository).

## What changed (code)

- `thirdparty/dq8-gfx/src/ui/ui_widgets.{h,cpp}`: `segmented` fits its words
  (equal segments, by word width, or a drop-down: never clipped); `navFocused`
  and `focusRing` (a white ring on a focused switch, segment, page entry or
  section); `beginSection` (a folded group); `settingRow` takes a `changed`
  flag (gold dot), stacks under 24 em of width, and its help runs the table's
  width (`ImGuiTableFlags_NoClip`); `rowDetail` (tooltip on hover or focus);
  `navItem` has a compact, icon-only form.
- `ui_pages.cpp`: the page list (icons below 44 em of window width, children
  `NavFlattened` so the d-pad crosses into the page), every page rebuilt on
  the row pattern, per-page `pageFooter` (two-step restore, the gold-dot
  legend), Display's Advanced group, Controls reordered, the Game page's
  foldable sections, Shortcuts (side by side when the window is wide, never
  taller than it, F6 and F8, alternatives on one row), About and the cheats
  notice scroll when short of room, the notice defaults to "Keep cheats off",
  new copy for the cheats page, notice and badge.
- `ui_overlay.cpp`: Menu size capped so the settings window keeps 40 x 24 of
  its font sizes (stepped by 0.05), toasts wrap to 30 em or the window,
  `kSettingsPageNames` shared by the menu bar and the page list (Interface
  before Cheats), "Cheats (master switch)" in the Game menu.
- `ui_style.h`: `pushScaledFont`, small text never under 11 px on screen.
- `src/graphics.cpp`: the option list regrouped and reordered, `help` cut to
  one line with the old text kept as `gameDetails`, `gameFolded` for the
  folded sections. Keys, defaults, ranges and steps are untouched (checked:
  the diff only moves and rewords).
- `ui_script.cpp`: `scrollpage N|end`, `pageinfo`, `unfold`, `nav` test aids.
- Docs: OPTIONS.md, README, CHANGELOG, DEVELOPMENT.md (the new script
  commands), MENU-DESIGN.md, `docs/images/menu-game.jpg` and `menu-cheats.jpg`
  (new menu), the research cheat sheet (also copied to
  `research\06-controls-cheatsheet.md` (the author's notes, outside the repository)).

## Test method

`build\game-dev\rg-menuux*.exe` from the repository folder,
`DQ8_GFX_HIDDEN_WINDOW=2`, `RG_MUTE=1`, `RG_GS_SCALE=2`, a scratch
`settings.ini` per size (`[window] fullscreen=0, width, height` and
`[interface] ui_scale`), a copy of `disc\mc0` as `RG_MC0_DIR` (the user's
settings and card were never touched), `RG_UI_SCRIPT` with wall times:
`menu on`, `settings <page>`, `scrollpage N`, `pageinfo`, `screenshot menu`,
`show Game`, `shortcuts`, `about`, `key F5` (the first-time notice),
`cheatsnotice accept` (badge), `notify ...` (toasts), `nav down ...` (focus).
The same script ran on the old and new executables at 1280x720, 1920x1080,
1280x960 (4:3), 960x540, and 1280x720 at Menu size 200% (and the new one at
960x540 at 200%). The menu was over the game's boot (the memory card check
screen), so what shows behind it is not a bug of the menu. Screenshots are BMP
in this SDL: converted to PNG with ffmpeg. Each shot also carries the
"Screenshot saved" toast of the previous one, which is the real toast.

Measured, per page, with `pageinfo` (scroll range of the page / visible
height, in pixels, at 1280x720 and 100%; and at 200%):

| Page | 1280x720, 100%: before | after | 960x540, 100%: before | after | 1280x720, Menu size 200%: before | after |
|---|---|---|---|---|---|---|
| Display | 210 | 58 | 367 | 238 | 1,609 | 634 |
| Sound | 0 | 0 | 0 | 0 | 41 | 11 |
| Controls | 1,125 | 1,208 | 1,282 | 1,388 | 3,110 | 2,623 |
| Game | 2,728 | 1,263 | 2,885 | 1,443 | 9,394 | 2,720 |
| Interface | 270 | 151 | 427 | 331 | 1,596 | 752 |
| Cheats | 501 | 304 | 658 | 484 | 2,304 | 1,127 |

The view was 537 px (560 after: a taller window) at 1280x720 and 100%, 380 at
960x540, and 399 (438 after) at Menu size 200%, where the new build shows the
menu at 176% because a 1280x720 window caps it. At 1920x1080 and 1280x960 the
window is 54 x 46 font sizes, so every page but Controls and Game fits whole
(Game 1,133 px, Controls 1,078 in a 690 px view). Controls grew a little:
its Back + Start row came from Interface and the table is the same length.

Sizes: the smallest text is 11 px or more at any Menu size (12.2 px chips,
14.3 px help at 100%); controls are one frame tall (29 px at 100%), the
compact page list entries 34 px; both counted from the style, not measured on
the screen.

## Not verified

- A physical controller; the injected d-pad path is the same ImGui
  navigation, not the SDL gamepad events.
- The game's own Settings screen (System > Settings) after the regrouping:
  its rows are drawn by `src/settings_pages.cpp` from the same option keys and
  were not changed, but they were not looked at in a loaded game.
- 4K, and a desktop display scale above 100%; fullscreen.
