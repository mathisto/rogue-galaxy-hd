# Menu design

Why the F1 menu (the PC port's own menu, `thirdparty/dq8-gfx/src/ui`, Dear
ImGui) is laid out the way it is. Written from what the repository, its docs
and the running menu show; there is no PRODUCT.md, and nothing here is a
claim about who plays it beyond that. The game's own menus and art are not
touched by any of this.

## What it is for

A settings surface used with keyboard, mouse and controller, mostly once or
twice a session: switch something, check a value, close it. So it is an
Operate surface: scannable, predictable, the same controls everywhere. It
sits over a running game (usually paused behind it), in any window from
960 x 540 up to a 4K fullscreen, at a menu size the player can raise to 200%.

## What was kept

The look: the navy window with the white double edge, gold for what is
chosen or switched on, the triangle cursor, Roboto, the HD font. The
behaviour of every setting, and every `settings.ini` key and value: nothing
was renamed or removed, only labels, grouping, order and layout. Environment
variables and `RG_UI_SCRIPT` commands still work (the page names `display`,
`sound`, `controls`, `game`, `interface`, `cheats` are as before).

## Critique of the menu as it was (2026-10-29)

Method: single-context, no sub-agent tool. Screenshots of the build before
this pass (`rg-menuux-base.exe`) at 1280x720, 1920x1080, 1280x960, 960x540
and 1280x720 with Menu size 200%, every page scrolled through; the code read
alongside. Heuristic scores are one reviewer's judgment, not a measurement.

| Severity | Finding | Evidence |
|---|---|---|
| P0 | A choice's label is cut off: "Supersample" reads "upersampl" in the scaling filter row | 1280x720 and 960x540; the segment width was width / count with a clip |
| P0 | The Shortcuts window is taller than the screen at 960x540 and 1280x720: the Controller section and the Esc row cannot be reached, and it cannot scroll | automatic-size window, no height limit |
| P0 | At Menu size 200% in a 1280x720 window the Game page is 9,394 px long in a 399 px view (24 screens); the page list takes a quarter of the width | `pageinfo` on the old build |
| P1 | The Game page is 2,728 px long at 1280x720 (5 screens): help of 3 to 6 lines wrapped inside a 38% column, 35 options in one scroll | `pageinfo`, screenshots |
| P1 | The custom controls (switches, segmented choices, page list) draw no focus mark, so with a controller or keyboard you cannot see where you are; and the d-pad stops at the page's frame, needing an extra A to go in | `InvisibleButton` without a focus ring; page child not nav-flattened |
| P1 | Help text is long and written as documentation ("Draws the game at a multiple of the PS2's own resolution. Above 1x is slower for now."); the same fact in two places in two words ("Aspect ratio" / "Display > Aspect ratio") | copy |
| P2 | Toasts stack over the window's close button and cannot wrap: a long message at a large menu size leaves the screen | `drawToasts` |
| P2 | The cheats notice is five paragraphs; its default focus lands on "Turn cheats on" | copy, focus order |
| P2 | Shortcuts list: two rows for Fullscreen, no F6 (free camera), no F8 (texture pack reload) | list |
| P2 | Grouping: "Back + Start opens the menu" under Interface, "Auto: at most" with no hint it is about Internal resolution, Developers a section of one, texture memory beside fonts | structure |
| P2 | No way back to defaults; the default is shown only on number rows | no reset anywhere |
| P3 | At Menu size 75% the small text (help, keycaps, chips) drops to 9 to 10 px | `FontSizeBase * 0.72` and friends |
| P3 | Jargon: "Trigger threshold", "Auto: oversampling", "Developers", "Alternate", "PS2" as a column title | copy |

| # | Heuristic | Before | After | Note |
|---|---|---|---|---|
| 1 | Visibility of system status | 3 | 3 | pending resolution moved from the combo text to the help line |
| 2 | Match with the real world | 2 | 3 | plain labels, units on every number |
| 3 | User control and freedom | 2 | 3 | restore defaults per page, Esc / B everywhere, nothing cut off or unreachable |
| 4 | Consistency and standards | 2 | 3 | one row pattern, one help style, one vocabulary |
| 5 | Error prevention | 3 | 3 | cheats notice defaults to "Keep cheats off" |
| 6 | Recognition rather than recall | 2 | 3 | gold dot for changed settings, defaults in tooltips, focus ring |
| 7 | Flexibility and efficiency | 3 | 3 | unchanged: hotkeys, L1/R1, fast-forward |
| 8 | Aesthetic and minimalist design | 2 | 3 | folded Advanced groups, one-line help |
| 9 | Error recovery | 2 | 3 | restore defaults, two steps |
| 10 | Help and documentation | 2 | 3 | one line visible, the rest on hover or focus |
| | Total | 23/40 | 30/40 | |

## The information architecture, old to new

Pages, in the order of the page list (the ones used most first, Cheats, an
opt-in, last; L1 and R1 turn them in this order):

| Old | New |
|---|---|
| Display, Sound, Controls, Game, **Cheats, Interface** | Display, Sound, Controls, Game, **Interface, Cheats** |

Display

| Old | New |
|---|---|
| Window: Mode, Vertical sync | Window: Mode, Vertical sync |
| Picture: Internal resolution, Auto: oversampling, Auto: at most, Aspect ratio, Scaling filter, Sharpening, Borders, Integer scaling, Sharper picture | Picture: Internal resolution, Aspect ratio, Scaling filter, Sharpening, Borders |
| | **Advanced** (folded): Auto resolution: oversampling, Auto resolution: highest, Integer scaling, Sharper picture |

Controls

| Old | New |
|---|---|
| Devices: Keyboard, Controller (the connected list under it) | Devices: Keyboard, Controller, **Open the menu with Back + Start** (from Interface), the connected list |
| Buttons: pad drawing, the bindings | Sticks and triggers: Stick dead zone, Trigger press point, Invert camera x2 |
| Analog sticks: Dead zone, Trigger threshold, Invert x2 | Button bindings: pad drawing, the bindings (the longest part last) |

Game

| Old | New |
|---|---|
| Camera and view (8) | Camera and view (8), unchanged |
| Convenience (7, HD movies among them) | Convenience (6) |
| Battles and rewards (6) | Battles and rewards (6) |
| Text and prompts: Button prompts, Font, Texture replacements, Texture pack memory | Text and HD content: Button prompts, Font, **HD movies**, Texture replacements |
| Developers: Developer tools | **Advanced** (folded): Texture pack memory, Developer tools |

Interface: Menu (Menu size, Notifications, Hide the idle mouse cursor; Back +
Start moved to Controls), Pause and speed, Frame rate. Sound is unchanged
(Volume: Master volume, Mute, Mute in the background). Cheats: Master switch
(Cheats on), Battle, Items and money, as before.

Labels renamed (keys unchanged): Cheats (master switch, F5) to Cheats on
(F5); Hold the money at an amount to Keep money at an amount; Amount to hold
to Amount; Dead zone to Stick dead zone; Trigger threshold to Trigger press
point; Auto: oversampling and Auto: at most to Auto resolution: oversampling
and highest; the bindings table's PS2 and Alternate to Game button and
Alternative; the cheats notice's Not now to Keep cheats off.

## The rules the pages follow

- Common first: what most players touch is at the top of a page and open;
  what few touch (the auto-resolution tuning, line blending, texture memory,
  developer tools) is in a folded Advanced group, a click or a d-pad press
  from view. Folding is remembered for the session.
- One row pattern: label (and a chip such as Experimental) on the left, the
  control on the right, one short help line under the label, running the
  page's width. The long explanation, where there is one, is a tooltip on
  hover and on keyboard or controller focus (the help is never the only place
  a fact lives).
- Units are on the value (%, degrees, MB, a multiplication sign for scales),
  and 100% always means the game's own.
- Defaults: a gold dot after a label means "not the default"; number rows
  keep their Default button; every page ends with a two-step "Restore ...
  defaults" (the first press asks, the second, within four seconds, does it).
  Display's restores the picture only, not Windowed/Fullscreen; Controls'
  restores devices and sticks, the bindings have their own button.
- A choice is never cut: equal segments when the longest word fits, segments
  by word width when they are uneven, a drop-down when they do not fit.
- It never overflows: the page scrolls; below 44 em of window width the page
  list shrinks to icons (the name on hover or focus); below 24 em of page
  width the control goes under its label; a small window caps the Menu size
  so the settings window keeps at least 40 x 24 of its own font sizes (the
  Menu size slider says so); dialogs (Shortcuts, About, the cheats notice)
  scroll when the window is shorter than they are; toasts wrap.
- Reach and focus: every control takes the d-pad and keyboard, the page list
  crosses into the page without an extra press, the focused control has a
  white ring, the scroll follows it, and Esc or B backs out of the topmost
  thing first (cheats notice, About, Shortcuts, Settings, the menu).
- Small text is never under 11 px on screen, whatever the Menu size.
- Words: plain, one noun for one thing (Cheats on, Menu size, Auto
  resolution), no internal terms, Experimental chips kept as the project had
  them.

## Not done

- A search box: about 40 options in 6 pages with folded groups did not seem
  to need one, and the rows are drawn inline by each page, so filtering them
  is not cheap.
- The game's own Settings screen (System > Settings) was reviewed from its
  source (`src/settings_pages.cpp`) and left alone: its labels are
  Title Case and short because of the game's own font and widths, and
  changing them could not be checked without loading a save and reading that
  screen. They still match this menu's order of sections; the names differ
  where the game's field is narrow (Encounters, Experience, Money, Prompts).
- Controller navigation was exercised with injected d-pad presses
  (`RG_UI_SCRIPT` `nav`), not a physical pad.
