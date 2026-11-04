#pragma once

class PS2Runtime;

// The game's 2D at 21:9 and 32:9.
//
// The 3D world is Hor+ at any shape (widescreen.cpp: the camera's aspect and
// the frustum follow the window). The 2D is drawn into the game's 512x448
// frame, which the presenter stretches over the whole window, so at 32:9
// every sprite and glyph comes out 2.67 times as wide as it is tall. Here the
// screen-anchored HUD (message windows, prompts, captions, the field HUD, the
// battle HUD) has its horizontal coordinates mapped about the middle of the
// frame as the packets are written: x' = 256 + (x - 256) * S / A, A being the
// window's shape and S the shape of the "HUD area" (16:9, 21:9, or the
// window's own: Full). Marks that follow something in the world, and
// full-screen sprites (fades, filters, letterbox bars), keep their x.
//
// Which draws are HUD is decided by the task that draws them: the task tables
// (TaskTableRun) call each task's draw function after TaskTestFlag, and the
// hooks here remember which one is running. At 16:9 and below nothing is
// changed, so the picture is what it was.
//
// Settings (keys of the Game page): hud_area, hud_placement, ultrawide_fov.
namespace rg::ultrawide
{

// After loadELF().
void install(PS2Runtime &runtime);

// How far the HUD spreads: 0 a centred 16:9, 1 a centred 21:9, 2 the window's
// own width (Full).
void setHudArea(int area);
// 0 Centre: the HUD is one block in the middle of the area. 1 Edges: its left
// and right parts keep to the area's edges.
void setHudPlacement(int placement);
// 0 Hor+ (the view widens with the window), 1 capped at 21:9, 2 capped at
// 16:9 (the horizontal angle stops growing; the vertical one gives).
void setFovCap(int cap);

// The camera's vertical field of view (radians) for a window this much wider
// than the cap's shape, under the player's cap; the argument unchanged when
// there is no cap or the window is no wider.
float cappedVerticalFov(float vertical);
bool fovCapped();

} // namespace rg::ultrawide
