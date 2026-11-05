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

See the end of this file (filled in with the run).

## Known gaps

- The picture is still one 512-column frame stretched over the window, so at 32:9 the 2D has about a third of the
  horizontal detail it has at 16:9. Placing it right does not make it sharper. See "Window-resolution HUD layer" below.
- The wide decision (is this frame wide or 4:3?) is read when a frame is drawn and again when it is shown, so for a frame at a
  menu or a scene change the two can disagree; it is the same behaviour as before this change.
- Events and cutscenes keep the letterbox the game draws (full width, a strip of about 4.7:1 at 32:9) and its captions go
  through the message path, so they sit in the 16:9 box.
- The bloom and focus-blur kernels work in frame pixels and are 2.67 times as wide on screen as tall at 32:9: a softer,
  sideways glow. Not changed.
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
