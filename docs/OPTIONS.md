# Options

<p align="center"><img src="images/menu-game.jpg" width="70%" alt="The F1 menu's Game page"></p>

**F1** (or the Guide button, or Back+Start) opens the menu. Its pages, in the
order of the page list (L1 and R1 turn them): Display, Sound, Controls, Game,
Interface, Cheats. Every page is one row pattern: the setting's name, its
control, one line of help; a longer explanation shows when you hover a control
or focus it with the keyboard or controller. A gold dot after a name means
"not the default"; every page ends with a **Restore ... defaults** button
(press it twice: the second press, within four seconds, does it). Things few
players touch are in a folded **Advanced** group on the page. How and why it
is laid out: [MENU-DESIGN.md](MENU-DESIGN.md).


- **Display** (Window, Picture, and a folded Advanced group with Auto
  resolution's oversampling and highest scale, Integer scaling and Sharper
  picture): window or fullscreen, vertical sync, internal resolution
  (1x to 8x, or **Auto**: the scale that gives at least the window's height
  in lines, times an oversampling of 1.0, 1.25 or 1.5, capped at 2x-8x,
  default 6x; 1080 lines take 3x, 1440 take 4x, 2160 take 5x; it follows
  the window when resized, moved or made fullscreen, with a toast;
  `RG_GS_SCALE=auto` for one run; a very wide window costs more at the same
  height, so lower the cap on 32:9),
  aspect ratio, scaling filter (Sharp, Smooth, Nearest, **FSR**, AMD's
  edge-aware upscaling for a window larger than the internal resolution, or
  **Supersample**, a Lanczos average of every rendered pixel for an internal
  resolution larger than the window, 4x to 8x),
  **Sharpening** (AMD FSR 1's, off by default; see
  [GRAPHICS-UPSCALING.md](GRAPHICS-UPSCALING.md)), and **Borders**, what fills the sides of a
  4:3 picture (black, the picture blurred, or your own image from the
  `borders` folder).
- **Sound**: volume and muting.
- **Controls**: Devices (keyboard, controller, Back + Start opening the
  menu), Sticks and triggers (dead zone, trigger press point, camera
  inversion), and Button bindings (controller and keyboard remapping). The
  controller drawing there is a PlayStation (DualShock 4 / DualSense) or an
  Xbox (One / Series) one: the connected controller's, PlayStation when there
  is none, or whichever **Controller shown** (Auto, PlayStation, Xbox; saved as
  `layout` under `[controls]`) picks. The table reads *In the game* (the input
  as that controller names it, the game's own PlayStation name small beside it
  when it differs), then **Main** and **Second**, the two buttons or keys that
  do it; the pad lights up with what you press, and the row you point at or
  focus rings its button.
- **Game**: the game options below, in sections.
- **Interface**: the menu's size (a small window caps it so the menu fits),
  notifications, pause and fast-forward (its speed: 1.5x, 2x, 3x, 4x, 8x or
  Max, as fast as the PC goes; and whether L2 and R2 control it), the
  frame-rate counter.
- **Cheats**: trainer-style helpers behind a master switch, below.

| Key | Controller | |
|---|---|---|
| F2 | | Pause |
| F3 | | Frame-rate counter |
| F4 | L2 | Fast-forward on or off |
| F5 | | Cheats on or off, the master switch ([CHEATS.md](CHEATS.md)) |
| ` (left of 1), held | R2, held | Fast-forward while held |
| F6 | Start, to leave | Free camera |
| F8 | | Reload the texture pack |
| F11 or Alt+Enter | | Fullscreen |
| F12 | | Screenshot |

## Fast-forward

The whole game runs faster: F4 or L2 switches it on and off, R2 or ` held
runs it only while held. The speed is set under Interface > Pause and speed,
or in the menu bar's Game > Fast-forward speed. Music and sounds keep their normal tempo
(the sound driver runs in step with the audio device), and movies play at
normal speed even with fast-forward on, so their sound stays with the
picture; it picks up again after the movie. It adds to the quick scenes
(Fast transporters, Fast Revelation Flow): 2x fast-forward in a quick scene
is 4x. How fast Max goes depends on the PC.

While "L2 and R2 control fast-forward" is on (it is by default), the game
no longer gets L2 and R2, and neither do the keys bound to them (Left and
Right Shift). Turn it off for anything in the game that needs them. The
free camera keeps them either way.

## Game options

**Camera and view**

| Option | |
|---|---|
| 60 frames per second | On by default; off, the game's own 30. From the next start. |
| Widescreen | A wider view that fills the window, from 4:3 to 32:9. On a window wider than 16:9 the HUD stays in a box of the HUD area below ([ULTRAWIDE.md](ULTRAWIDE.md)). |
| Widescreen menus | 4:3 pillarboxed (default): the full-screen menus (main menu, items, equipment, status, Settings, save and transporter menus, factory, insect and ship menus) are shown at 4:3 with Display > Borders beside them. Stretched: as wide as the window, as before. |
| HUD area | 16:9 (default), 21:9 or Full width: how wide the HUD (message windows, prompts, minimap, compass, battle gauges) spreads on a window wider than 16:9. Nothing changes at 16:9 or narrower. |
| HUD placement | Centre (default): the HUD is one block in the middle of its area. Edges: its left and right parts (the compass, the minimap, the battle gauges) keep to the sides of the screen. |
| Ultrawide field of view | No limit (default), Up to 21:9 or Up to 16:9: whether the view keeps widening on a window wider than that. A cap stops the sideways angle and shows a little less above and below instead. |
| Field of view | 40 to 90 degrees (the game's is 52). |
| Camera distance | 70% to 160% of the game's. |
| Faster camera rotation | The camera turns twice as fast. |
| Draw distance | 100% to 400% of the game's, from the next map loaded. |
| Free camera (F6) | The party stands still and the controller moves the camera. Start or F6 to leave. |

**Convenience**

| Option | |
|---|---|
| Skip the opening | Straight to the title screen. |
| Show the loading recap at once | The story so far appears whole, and you can go on as soon as it has loaded. |
| Instant text | Dialogue a page at a time instead of letter by letter. |
| Chests on the map | Unopened chests show on the map in the parts explored. |
| Fast transporters | Transporter warps play up to twice as fast. |
| Fast Revelation Flow | Revelation Flow animations play up to twice as fast. |

**Battles and rewards**

| Option | |
|---|---|
| Encounter rate | 0% (no random battles) to 300%. |
| Experience from battles | 0% to 500%. |
| Money from battles | 0% to 500%. |
| Item drops | 0% to 1000%. |
| Shop prices | 10% to 200%. |
| Action gauge speed | 25% to 400%. |

**Text and HD content** ([FONT-AND-PROMPTS.md](FONT-AND-PROMPTS.md))

| Option | |
|---|---|
| Button prompts | PlayStation, Xbox (A, B, X, Y, LB, RB, LT, RT, by position: Cross is A), or Auto (the default: PlayStation buttons for a PlayStation controller, Xbox ones otherwise; a toast says which and why). Buttons painted into pictures (help pages, the loading screen, battle and menu sheets) follow too, with the controls boards' controller drawing as an Xbox-style pad, START/SELECT as Menu/View, the sticks and d-pad, and "START button" / "L3" words. |
| Font | Original, or Modern: Roboto, or your own .ttf in the `fonts` folder, drawn sharp at the internal resolution; lines rewrap to its letter widths. The menus' numbers (play time, money, counts, levels, stats), the battle's damage numbers, HP/AP and results digits, and the compass letters too. |
| HD movies | The cutscenes play from upscaled 1080p files in `textures\SCUS-97490\movies` (or the folder `RG_HD_MOVIES` names), with the game's own sound and timing; a movie without a file, or one the PC cannot decode in time, plays as the game has it. On by default only when the folder holds files (the option's help says how many of the 89 it found). [HD-MOVIES.md](HD-MOVIES.md) |
| Texture replacements | On by default. Off shows every texture as the game has it: no Xbox buttons, no modern font, no texture pack. F8 reads the pack's folder again. |

**Advanced** (folded until opened)

| Option | |
|---|---|
| Texture pack memory | 512 MB to 8 GB (default 2 GB): the video memory an HD texture pack may take; the least recently seen pictures give way ([TEXTURE-PACKS.md](TEXTURE-PACKS.md)). The pack's look (per-class upscale models) is chosen when it is built; `RG_TEXTURE_PACK=textures/SCUS-97490/x4plus-anime/replacements` runs the older all-x4plus-anime look. |
| Developer tools | What the developers left in the game. In the main menu: R3 shows every item (R1 + R3 as a list, Cross gives one), R1 + Select unlocks every help and info entry, Select fills the bestiary. R2 + Triangle on the Revelation Flow gives the items a cell needs. Triangle + R1 + R2, paused in a scene, skips it (this can break the story's order). |

## Cheats

<p align="center"><img src="images/menu-cheats.jpg" width="70%" alt="The F1 menu's Cheats page"></p>

Settings > Cheats, with a master switch (**F5**, or Game > Cheats; "Cheats on"
on the page) that is off
at every start. While it is on the game cannot save (a save made with cheated
numbers is the one that gets corrupted; the save screen says "Save to file 1
failed." and the card is not touched), and a badge in the picture's corner
says so. The first time it is turned on a notice asks for a yes
([CHEATS.md](CHEATS.md): what each does, where it lives, what was verified).

| Option | |
|---|
| Infinite HP | The party takes no damage and tops up to full; someone already down stays down. |
| Infinite AP | AP stays full. |
| Full action gauge | The party's action gauge stays full. |
| One-hit kills | Anything that is not a party member goes down to its first hit, bosses included. |
| Items are not used up | Items used from the bag in the field or in battle stay in it. |
| Keep money at an amount | The money is put back to the amount below whenever it differs (0 to 99,999,999). |
| Money | A button, "Set to maximum": 99,999,999, once. |

<p align="center">
  <img src="images/salgin-above.jpg" width="49%" alt="Salgin from above">
  <img src="images/dev-items.jpg" width="49%" alt="Developer tools: the list of every item">
</p>

## In the game's own menu

The same options are on the game's Settings screen (System > Settings in
the main menu): L1 and R1 turn the page.

<p align="center">
  <img src="images/settings-switches.jpg" width="49%" alt="The game's Settings screen: the switches">
  <img src="images/settings-numbers.jpg" width="49%" alt="The game's Settings screen: the numbers">
</p>

## Good to know

- The window opens as you left it. Settings are saved in
  `%APPDATA%\RogueGalaxy\Recompiled\settings.ini`.
- On a 144 Hz monitor 60 fps judders a little; 120 Hz or variable refresh
  looks smoother.
- Each option also has an environment variable, for testing:
  [DEVELOPMENT.md](DEVELOPMENT.md). For fast-forward: `RG_FAST_FORWARD_SPEED`
  and `RG_FAST_FORWARD_TRIGGERS`; for text and prompts: `RG_BUTTON_PROMPTS`,
  `RG_FONT_STYLE`, `RG_FONT`, `RG_TEXTURE_REPLACEMENTS`; `RG_WIDESCREEN_MENUS`;
  for HD movies: `RG_HD_MOVIES`; for the cheats: `RG_CHEATS` and friends
  ([CHEATS.md](CHEATS.md#test-method)).
- ReShade and RenderDoc: set `RG_ALLOW_VK_LAYERS=1` in Windows before
  starting the game ([GRAPHICS-UPSCALING.md](GRAPHICS-UPSCALING.md#reshade-and-renderdoc)).
- Texture packs go in `textures\SCUS-97490\replacements`:
  [FONT-AND-PROMPTS.md](FONT-AND-PROMPTS.md#texture-replacements); building
  one from the AI-upscaled set, tuning and completing it:
  [TEXTURE-PACKS.md](TEXTURE-PACKS.md).
