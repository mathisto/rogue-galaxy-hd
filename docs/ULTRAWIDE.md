# Ultrawide (21:9 and 32:9)

What the game does on a window wider than 16:9, what each option changes, how it is done, and what was checked.
Source: `src/ultrawide.cpp` (the HUD), `src/camera_view.cpp` (the view cap), `src/widescreen.cpp` (the wide mode this builds
on). Written 2026-11-04 on `feature/ultrawide`; the research it follows is `research/15-32x9-spike.md` and
`research/17-ultrawide-prior-art.md` in the project's notes.

## The problem

The 3D world was already fine at any shape: with Widescreen on, the camera and its culling follow the window (Hor+), so a
5120x1440 window shows more of the scene to each side and nothing is missing at the edges. The 2D was not. The game draws
everything into one 512x448 picture and the presenter stretches that over the window, so at 32:9 every sprite and every glyph
came out 2.67 times as wide as it is tall, and the HUD sat at the far sides of a screen five times as wide as a TV.

## The options (Game > Camera and view, in settings.ini under `[game]`)

| Option (`key`) | Values | What it does |
|---|---|---|
| HUD area (`hud_area`) | **16:9** (0), 21:9 (1), Full width (2) | The shape of the box the HUD is laid into, in the middle of the window. 16:9 is what the game's wide mode was made for. 21:9 spreads the same layout over a wider box; Full width over the whole window. |
| HUD placement (`hud_placement`) | **Centre** (0), Edges (1) | Centre keeps the HUD one block, its parts keeping their places in the area. Edges keeps the compass, the minimap and the battle gauges at the sides of the screen (as far from the edge as they are at 16:9), and the messages and prompts in the middle. |
| Ultrawide field of view (`ultrawide_fov`) | **No limit** (0), Up to 21:9 (1), Up to 16:9 (2) | No limit is plain Hor+. A limit stops the sideways angle at what that shape shows, and gives a little less above and below instead. The Field of view slider (degrees across a 4:3 picture, the game's 52) still sets how wide the view starts. |

Nothing changes at 16:9, or in any window narrower than that (4:3 included). The defaults give a centred 16:9 HUD on a wider
window. Environment variables for a run: `RG_HUD_AREA`, `RG_HUD_PLACEMENT`, `RG_ULTRAWIDE_FOV` (0, 1, 2 as above).

## How the HUD is placed

With a wide window the game's own wide mode narrows the HUD sprites by 3/4, right for 16:9 only. Here the window's shape is A
(width over height) and the area's shape is S (16/9, 21/9, or A for Full):

- **Positions.** x' = 256 + (x - 256) * S/A about the middle of the frame (a centred area), or x' = x * (16/9)/A for a part
  kept to the left edge, 512 - (512 - x) * (16/9)/A for the right (Edges).
- **Widths.** Every sprite is made (16/9)/A as wide, so it keeps the proportions it has at 16:9 whatever the area. At the
  default area the two are one affine map. At 21:9 and Full they differ, so a HUD picture made of many sprites (the minimap is
  strips) cannot be moved sprite by sprite: the sprites one place in the game's code draws are grouped by what they touch (a
  gap over 12 pixels ends a group), and a group moves as one, its middle by the position rule, its width by the width rule.
- **What is HUD** is decided by the task that draws it. The game runs a table of tasks each frame (`TaskTableRun`, 0x1F5340),
  calling each draw function after `TaskTestFlag` (0x1F57A0); the hook there remembers which. The ones mapped: the minimap and
  the bars under the prompts (`MinimapDraw`, 0x1FFBC0), the status and battle windows (`TaskDrawStatus`, `BattleMenuDraw`,
  `TaskDrawMenuBtlChg`), the compass (`CompassHudDraw`), and, as text kept in a 16:9 box whatever the area, messages,
  captions and treasure-box messages (`TaskDrawMessage`, `TaskDrawCaption`, `TaskDrawTBox`). Everything else, the scenery, the
  characters, the effects, is left alone. The table is `g_classes` in `ultrawide.cpp`, and `RG_UW_TASKS=ADDR:H,ADDR:T`
  replaces it for a run (H the HUD, T text, N none).
- **How the vertices are changed.** The game writes each vertex of a 2D draw through six small functions
  (`GsPktSetXYZ`, `SetXYZf`, `SetXYZPixel` and their vector forms, 0x13EEB0 to 0x13F110) into a GS packet. They are replaced here
  by writers that keep the x to a sixteenth of a pixel, and note where each went; when the task's draw group ends
  (`DrawListEndPrim`, 0x141A20) the vertices are placed. Draws into any other target than the 512 wide picture (the small
  buffers a treasure box's message and the map's tiles are made in) are not touched, and neither are sprites as wide as the
  frame (fades, filters, pictures of the screen).
- **Scissor.** The clip rectangle of a HUD draw (SCISSOR_1, through `GsPktWriteAD`) is placed by the same rules from its edges,
  rounded outward to whole pixels, so a clipped panel (the radar) shows what it did.
- **The compass** is a 3D ring in a small view; its screen matrix (`ViewSetScreenMatrix`, 0x1562B0, from `CompassHudRender`)
  has its centre moved by the position rule. Its size is right already (`widescreen.cpp` gives it the window's aspect).
- **Marks that follow something in the world** (damage numbers, names over enemies, the lock-on arrows) are drawn with another
  origin than the picture's and are left where the scene puts them. Full-screen menus and the title are shown 4:3 between bars
  (Widescreen menus) and are not drawn wide at all.
- **The view** with a limit: the vertical field of view given to the camera is cut so that the horizontal one stays what 16:9
  (or 21:9) gives: tan(v'/2) = tan(v/2) * cap / A.

## What was checked

Hidden window, scratch settings (Display > Aspect ratio Auto, internal scale 2), a copy of the memory card (File 01, chapter 2,
Juraika: Path to Burkaqua, the party at the first transporter), a Cross script. Screenshots, looked at one by one, are in
`research/assets/ultrawide/` of the project's notes (`before_` is the branch's previous build, `after_` this one; `a16`, `a21`,
`full` the HUD area, `edges` the placement).

| Scene | 1280x960 (4:3) | 1920x1080 | 2560x1080 (21:9) | 5120x1440 (32:9) |
|---|---|---|---|---|
| Title | 4:3, as before | 4:3 between the Borders fill | same | same (checked) |
| Field: compass, minimap, prompt | unchanged | identical (below) | HUD area 16:9, 21:9, Full, and Edges: right | 16:9, 21:9, Full, Edges: right |
| Main menu | unchanged | unchanged | 4:3 between the Borders | 4:3 between the Borders (checked) |
| Pause | unchanged | identical (below) | text in the 16:9 box | same |
| Battle HUD (gauges, party panel, radar, names) | not captured | not captured | right, 16:9 area | right, 16:9 area |
| Results screen after a battle | | | in the 16:9 box (task 0x204320, Text) | in the 16:9 box |
| Field of view cap | | | | Up to 21:9, Up to 16:9: the horizontal angle held, less above and below |
| Settings menu rows | `rowcheck`: 27 rows on the Game page, 0 overlapping, at 1280x720 (not run at other sizes) | | | |

**16:9 identity.** Two runs of the old build never match pixel for pixel (the scene animates: 42% of the field frame differs
between two runs of the same exe), so a screenshot diff of a live scene proves nothing. The pause frame is frozen: the old
build's second run and the new build's first run, at 1920x1080, have **0 differing pixels** in the whole frame (compass, minimap,
dimming and the PAUSE text included); the other pairs froze the scene at another moment. That pair was taken before the
follow-up round; after it no new identical frame was captured (the runs animate), so for the later changes the evidence is the
gate: nothing in the follow-up changes runs unless the window is wider than 16:9. The code path agrees: below 16:9 every
hook calls the game's own function unchanged.

Not reached: a cutscene event (letterbox and captions; this save reaches none; the New Game prologue does, but it was not run),
the battle at 1920x1080 and 4:3, other HUD areas in battle, a window narrower than 4:3, a controller. The player's real
5120x1440 monitor is the final judge.

## Known gaps

- The picture is still one 512-column frame stretched over the window, so at 32:9 the 2D has about a third of the
  horizontal detail it has at 16:9. Placing it right does not make it sharper. See "Window-resolution HUD layer" below.
- The wide decision (is this frame wide or 4:3?) is read when a frame is drawn and again when it is shown, so for a frame at a
  menu or a scene change the two can disagree; it is the same behaviour as before this change.
- Events and cutscenes keep the letterbox the game draws (full width, a strip of about 4.7:1 at 32:9) and its captions go
  through the message path, so they sit in the 16:9 box.
- The bloom and focus-blur kernels work in frame pixels and are 2.67 times as wide on screen as tall at 32:9: a softer,
  sideways glow. Not changed.
- Anything not listed in `g_classes` is as stretched as it was (the battle results screen, task 0x204320, is now listed).
- Fixed in this round: the pause dimming seam (under the pause scope only the text's own call sites, 0x2F5C40..0x2F5FB0, are
  mapped; the filter sprites over the scene and the scissor stay as the game writes them), and the multicoloured pixels at the
  left edge of the minimap (the map is built in strips written into the picture and read back by position, and the
  whole-frame start sprite of that build, 0x3259B8/0x3259CC, was being moved, so the read-back picked up other data; all of
  these sites are in `kScratchSites` and are written unmoved). Another element built the same way would show as a missing
  or shifted piece or as such noise: add its site to `kScratchSites`.
- Two HUD tasks of the battle (the damage popups and the target marks) were judged world-anchored from their origin and not
  mapped; a few elements that follow the picture may still be missing from the table. A missing task shows up as a piece of
  the HUD left at its 16:9 position over the wide picture.

## Window-resolution HUD layer: what it would take

(A feasibility look only; nothing of it is built.) The question: could the HUD be drawn in a layer at the window's resolution
instead of into the game's 512x448 frame (as gta-vcs-recomp does), so text and gauges are sharp at 32:9 whatever the internal
resolution? Yes, it can be done, and the placement work here is its first half: the game thread already knows, per vertex,
that a draw is screen-anchored HUD (the task scope), and the groups and the 16:9 proportions are what the layer would draw.
What it needs:

1. **A tag that crosses the thread boundary.** The GS thread sees only the packets. A marker written into the packet at the
   start and the end of a HUD task (an A+D write to a register the GS does not use, say 0x7F with a value) tells the renderer
   which draws belong to the layer. About 2 hours.
2. **A second set of render targets** in `sdlgpu_targets.cpp`: an RGBA surface at the window's size, cleared to transparent at
   the start of a frame, that HUD draws go to instead of the picture's colour target, with the vertex transform of those draws
   scaled to it (the game's x, y in frame pixels over 512 and 448). The target cache is built around one `m_scale` for every
   surface (about 300 uses), so a layer with its own size is a new kind of surface. 4 to 6 hours.
3. **Blending that accumulates.** Drawing alpha-blended sprites onto a transparent target and compositing it later is not what
   the GS did (it blended into the opaque picture). Ordinary blends (source alpha) are right in a premultiplied layer; additive
   and subtractive blends, draws that test or write destination alpha, and draws that read the frame (the shader's own blend
   path, `feedback`) are not, and the HUD has some (glows, the pause dimming, the minimap's translucent panel). Each needs a
   decision: into the layer with a matching blend, or into the picture as now. 4 to 6 hours.
4. **The present step** draws the layer over the picture (before the menu overlay), in the picture's rectangle, through the
   same scaling and FSR choice, and into a capture. 2 hours.
5. **What reads the frame after the HUD is drawn.** The game's blur, focus and glare passes and its pause filter copy the
   frame buffer. If any run after a HUD draw, the HUD must be in the picture at that moment, or the layer must be composited
   first. Whether Rogue Galaxy draws its HUD before or after those reads was not checked (the HUD tasks come late in the table,
   which suggests after, and the pause screen would then dim the layer too). 2 to 4 hours of looking and a rule.
6. **Texture replacement at the layer's scale.** Replacement fonts and prompts are drawn at the internal resolution through
   `misc.y`; at the window's they sample the same textures, so that works, but the sprite clamp and the texel-per-pixel path
   need a pass. 2 hours.
7. **Tests** at six window sizes and three areas, the menus and the pause among them. 3 hours.

About **20 to 27 hours of working time**, three working days at the pace of this branch (builds are a minute; the runs
are three minutes each, four at a time). The risk is in 3 and 5. A cheaper step that gets part of the sharpness is a per-surface
x and y scale (the spike's F3), which costs more and sharpens the world as well.

## Tutorial boards and the sweep of other screens (2026-11-05)

The first-battle tutorial board ("Basic Battle Controls (1) 01/03", pictures, "Press the Menu button") is drawn by
`TaskDrawEvent1` (0x27AB50), not by the help menu's task (0x203280, which is the main menu's Help). It was unlisted, so the
whole board was stretched over the window (`tutorial_32_before.jpg`). Listed as Text it sits in the 16:9 box with its
proportions (`tutorial_32_after.jpg`, 1920x540). `0x203280` is listed too, for the help menu when menus are not pillarboxed.

Reached by a new game and a scripted run (hidden window, Cross every 2 s, 1920x540, scratch card, about 9 minutes to the
board): the prologue and the event cutscenes (letterbox), the tutorial board pages 1 to 3, the battle HUD and the field. Seen at
32:9 and fixed: the tutorial board. Seen and as before: event letterbox and captions, battle gauges.

Not reached (nothing was drawn at 32:9 for them; only reasoned from the task table): shops (`TaskDrawMenuShop` 0x203720), the
warp menu (0x1FFB60), EasyChange (0x2030A0), Think (0x203620), CompleteCheck (0x203450), 7th Mystery (0x203530), Insectron
and Disc menus, Factory, Item Creation, the world map, Save/Load, Options and the Jaws of Hell. The menu modes (14, 16, 17, 19,
20) are shown 4:3 between bars by default, so they do not stretch; the field-mode ones (shops, warp) may. To try one,
`RG_UW_TASKS=` with its address as `T` (for example `RG_UW_TASKS=<the default table>,203720:T`) and look at it; `RG_UW_TRACE=2`
logs each task the first time it draws, with its target origin and class (cheap; `RG_UW_TRACE=1` is too slow in a heavy scene).
They are not listed because a task listed blind could break a screen that is fine.

**Fixed: the picture 80% as wide as the window in the tutorial battle.** The cause was a write into memory that was no
longer ours. A vertex's x is placed when the table run ends, into the packet the vertex was written to; the minimap's draw
(`MinimapDraw`, 0x1FFBC0) in that battle had packets the game reuses before the run is over, so the late write landed on
other data (found with `RG_UW_TRACE=3`: 40 "stale vertex" lines, the quad no longer holding an XYZ2 register, the contents
floats and counters). That corrupted something the presentation reads, and the picture came out 80% wide with black at the
right. Placing a vertex (and the late scissor patch) now checks that the quad is still an XYZ2 (or SCISSOR) quad and does
nothing otherwise. Verified at 1920x540, 2560x720, 5120x1440 and 1280x720 (below).

