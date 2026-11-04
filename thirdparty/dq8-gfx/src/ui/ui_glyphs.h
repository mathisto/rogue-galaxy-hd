// Vector drawings for the menu: icons, the PS2 pad's button marks, what a
// binding asks the player to press, and a DualShock 2 lit by live input.
// Drawn rather than loaded, so they stay sharp at any scale and ship nothing.
#pragma once

#include "gfx/backends/sdlgpu/sdlgpu_input.h"

#include <imgui.h>
#include <string>

namespace dq8::ui {

enum class Icon : uint8_t {
    Display, Sound, SoundOff, Controls, Interface, Crown, Camera, Pause, Play,
    Keyboard, Info, Chart, Close, Folder, Link, Speed, Check, Warning, Reset, Disc, Wrench, Gear, Copy
};

void drawIcon(ImDrawList *list, Icon icon, ImVec2 center, float size, ImU32 color);
// Lays out an icon-sized item and draws the icon in it.
void icon(Icon icon, float size, ImU32 color);

// Which controller the Controls page draws and names buttons after.
enum class PadBody : uint8_t { PlayStation, Xbox };

// A PS2 input as the shown controller marks it: Cross is a cross on a
// PlayStation pad, the A button on an Xbox one. The game itself still reads
// the PS2 pad, so `gameInputOriginal` gives the PS2 name when it differs.
void drawPadGlyph(ImDrawList *list, gfx::PadInput input, ImVec2 center, float size,
                  PadBody body = PadBody::PlayStation);
const char *gameInputName(gfx::PadInput input, PadBody body);
const char *gameInputOriginal(gfx::PadInput input, PadBody body); // nullptr when the same

// What a binding asks the player to press, as their keyboard or controller
// type labels it. Chips are `height` tall; the width depends on the label.
float bindingChipWidth(const gfx::PadBinding &binding, SDL_GamepadType type, float height);
void drawBindingChip(ImDrawList *list, const gfx::PadBinding &binding, SDL_GamepadType type,
                     ImVec2 min, float height, float alpha = 1.0f);
std::string bindingLabel(const gfx::PadBinding &binding, SDL_GamepadType type);
const char *controllerFamilyName(SDL_GamepadType type);

// A PlayStation (DualShock 4 / DualSense) or Xbox (One / Series) controller,
// drawn in code and fitted into the box, lit by `pad` (what the game reads, so
// the PS2 Cross lights the A button). `highlight` rings one input, or none
// when it is PadInput::Count.
void drawPadDiagram(ImDrawList *list, ImVec2 min, ImVec2 size, const gfx::PadSnapshot &pad,
                    gfx::PadInput highlight, PadBody body);

} // namespace dq8::ui
