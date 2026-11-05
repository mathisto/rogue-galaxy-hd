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
`sound`, `controls`, `game`, `interface`, `setup`, `cheats` are as before).

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

**Setup and HD** (added with the setup window, 2026-11-01) sits between Interface and Cheats: Display, Sound, Controls, Game,
Interface, Setup and HD, Cheats. It follows the same rules, with two sections of `settingRow`s and no setting of its own (it
changes nothing in `settings.ini`):

| Section | Row | Control | Where the value comes from |
|---|---|---|---|
| What is installed | HD textures | "11,072 pictures", or "Not installed" | the replacement store's file count (`GsReplacementStore::fileCount`), what the game itself found in `textures/SCUS-97490/replacements` |
| | HD movies | "89 of 89", or "Not installed" | `rg::hdmovie::fileCount()` of `textures/SCUS-97490/movies` against the disc's 89 |
| | Font, Button prompts | the current choice (Original / Modern; PlayStation / Xbox / Auto) | the Game page's own options |
| | Installed by | "Setup 0.1.0", or "Not recorded" | `release` in `rg-install.json` (absent for a folder the old `Install.bat` made) |
| Add or change | Run the setup | button **Open the setup**; disabled, with a tooltip, when `RogueGalaxy-Setup.exe` is not in the game's folder (a developer's checkout) | starts the exe detached; a toast says it is opening |

Each row's help is one line that says where to change the thing ("Switch them on or off under Game > Texture
replacements"); the long note (the setup opens in its own window, it keeps saves and settings, HD parts need Vulkan) is the
button's tooltip. The page reads the folder again every two seconds while it is shown, so it follows the setup finishing
behind it. `RG_UI_SCRIPT` names the page `setup`. Checked at 1280x720, 1920x1080, 1280x960 and 960x540 with `rowcheck`: 0 of 6
rows overlap at each (docs/SESSION-NOTES-setup-window.md).

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
| Camera and view (8) | Camera and view (11): **HUD area**, **HUD placement** and **Ultrawide field of view** added after Widescreen menus (2026-11-04, below) |
| Convenience (7, HD movies among them) | Convenience (6) |
| Battles and rewards (6) | Battles and rewards (6) |
| Text and prompts: Button prompts, Font, Texture replacements, Texture pack memory | Text and HD content: Button prompts, Font, **HD movies**, Texture replacements |
| Developers: Developer tools | **Advanced** (folded): Texture pack memory, Developer tools |

### Game: the ultrawide rows (2026-11-04)

Three rows in Camera and view, after Widescreen menus, which they follow in meaning: Widescreen turns the wide picture on,
the next rows say how the 2D sits on it and how far the view widens. Each is a segmented choice (the existing `choices`
control, a value from its minimum, as Widescreen menus and Button prompts are), so the three states are in view and one
press changes them; none needs a number. New `settings.ini` keys only (`hud_area`, `hud_placement`, `ultrawide_fov`, under
`[game]`); nothing was renamed.

| Row | Choices | One-line help (kept to a line at 960 wide) | Why this wording |
|---|---|---|---|
| HUD area | 16:9, 21:9, Full width | How wide the HUD spreads on a window wider than 16:9. | The choices are the shapes themselves; "area" is the word the help uses again, so the three rows read as one idea (an area, its placement, the view). "Full width" says what 32:9 shows on a 32:9 window without naming a shape the player may not have. |
| HUD placement | Centre, Edges | Where the HUD sits inside that area. | "Centre" and "Edges" are positions, not styles; the long help (tooltip) says which parts go to the edges. |
| Ultrawide field of view | No limit, Up to 21:9, Up to 16:9 | How far the view may widen on a window wider than 16:9. | Not "Hor+" and not "Widest": the player sees a limit or none, in the shapes they know. It sits beside the Field of view slider and says "field of view" in full so the two are found together. |

The help says "on a window wider than 16:9" in all three because that is when they do anything: on a 16:9 or narrower
window nothing changes, and the rows stay visible (a hidden row would hide that the option exists). The default of each is its
first choice (16:9, Centre, No limit): nothing is capped until the player asks. `rowcheck` was run on the page with the rows
in; the sizes and the result are in SESSION-NOTES-ultrawide.md.

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
- A row never overlaps itself: the label sits after a fixed gutter that holds
  the gold dot, wraps at the label column's edge, the badge (Experimental,
  Restart needed) goes beside it when it fits and under it when not, the help
  is always its own wrapped line under both, and the control starts in its own
  column (or under the help when the page is narrower than 24 em). The layout
  is in `settingRow`; `RG_UI_SCRIPT` `rowcheck` compares each row's rectangles
  as it is drawn (134 distinct rows over every page and the About window): the
  first version overlapped in 60 rows at 1280x720 with changed settings and
  66 to 68 at Menu size 125% and 200%; now 0, at 1280x720, 1920x1080,
  1280x960, 960x540, and 1125 px wide at Menu size 100, 125 and 200%.
- Small text is never under 11 px on screen, whatever the Menu size.
- Words: plain, one noun for one thing (Cheats on, Menu size, Auto
  resolution), no internal terms, Experimental chips kept as the project had
  them.

## Controls: the controller drawing and the bindings table (2026-11-04)

The page drew a DualShock 2 whatever was plugged in, and its table read "Game
button | Button | Alternative".

**The drawing.** Two original controller bodies, drawn in code
(`drawPadDiagram`, `ui_glyphs.cpp`: cubic outlines mirrored about the middle,
a shadow, a rim, a lighter face and one outline; no picture, no third-party
art): a PlayStation one (DualShock 4 / DualSense: touchpad, Share and Options,
the PS button, sticks side by side, the Triangle, Circle, Cross and Square
marks in the menu's own colours) and an Xbox one (One / Series: View, Menu and
the Xbox button, the left stick up and the right stick down, A, B, X and Y in
their colours, LB, RB, LT and RT). Every button is lit by what the game reads
(the PS2 Cross lights A), rung in gold when its row is under the pointer **or
has the keyboard or pad focus** (new), and the sticks' knobs follow the
sticks. Which body: the controller in use (PS3, PS4 and PS5 pads: PlayStation;
Xbox 360 and One: Xbox), PlayStation when none is connected, and PlayStation
too for a Switch or unknown pad, which has no body of its own here (the
caption says so, and its buttons are named by position: Cross is the south
button). A caption beside it says which and why ("DualSense connected", "No
controller connected: showing PlayStation"). **Controller shown: Auto /
PlayStation / Xbox** (`[controls] layout = auto|playstation|xbox`, default
Auto, display only) lets a player with no pad, or one who wants the other
names, see them. The button names in the table and in the capture prompt follow
the shown layout.

**The columns: my opinion.** Keep three columns and keep both slots. The code
has two slots per input, the keyboard fills both for the faces, and players do
use a second key; dropping the column would lose a feature to tidy a table.
Showing only the plugged-in controller's column does not apply: the table is
already one device at a time (the Controller and Keyboard tabs), and
a binding is a position on any pad, so what a pad changes is the *names*,
which now follow the shown layout. What was wrong was the words and the
weight:

- "Game button" names what the game reads, a PS2 button, and the player's
  pad is often an Xbox: "Cross" next to an "A" chip read as two different
  buttons. The first column is now **In the game**: the input as the shown
  controller names it, with a glyph, and the game's own PS2 name small and
  muted beside it only when it differs ("A button  Cross", "LB  L1",
  "Menu  Start"; the game's own text says Cross, so the player can still match
  it).
- "Button" and "Game button" were near synonyms, and "Alternative" was an
  alternative to what? The two columns are now **Main** and **Second**; the
  tab says whether they are buttons or keys. A row reads "in the game: A
  button, Main: A".
- The Second column is the spare, empty on the controller by default, so it
  is quieter (dimmer chip, dimmer dash) and the first sentence beside the
  drawing says what the table is: "Each row is what the game asks for; Main
  and Second are the buttons that do it."

Rowcheck on the page: 8 rows, none overlapping, at 1280x720, 1920x1080,
960x540 and 1280x720 at Menu size 200%, for both bodies and with no pad.
Labels renamed (keys unchanged): Game button to In the game; Button, Key and
Alternative to Main and Second.

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
- The PlayStation and Xbox drawings are shown for a DualShock or DualSense,
  an Xbox pad and none; a Switch pad shows the PlayStation one. Only an Xbox
  pad was ever connected to the test PC: a PlayStation pad is simulated
  (`RG_FAKE_PAD_TYPE`).
- The help text of the stick and trigger rows still says L2 and R2.
