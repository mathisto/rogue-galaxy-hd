#pragma once

#include <string>

class PS2Runtime;

// The game's text and button prompts, redrawn through the texture
// replacements of thirdparty/dq8-gfx (sdlgpu_replacements.h):
//
// - Button prompts: the eight controller icons of the gaiji sheet the text
//   tags (X) (O) (#) (A) [l1] [r1] [l2] [r2] draw, as an Xbox controller's
//   (A B X Y, LB RB LT RT, by position), or the game's PlayStation ones.
//   Auto follows the controller playing: a PlayStation pad gets the game's
//   own, any other pad, and the keyboard alone, the Xbox ones.
// - Font: the three font pages (normal, system, name) redrawn from a
//   TrueType font at the render scale, and the game's width table
//   (D_0044E850) given that font's advances, so text reflows. The font is
//   RG_FONT's file, else the first .ttf/.otf in fonts\ (in the working
//   directory, beside borders\), else Roboto Medium, built in.
//
// Menu: Game > Text and prompts (and the game's own Settings screen). For a
// run: RG_BUTTON_PROMPTS=playstation|xbox|auto, RG_FONT_STYLE=original|modern,
// RG_FONT=path, RG_TEXTURE_REPLACEMENTS=0 (no replacement at all, packs
// included). See docs/FONT-AND-PROMPTS.md.
namespace rg::hdtext
{

enum class Prompts : int
{
    PlayStation = 0,
    Xbox = 1,
    Auto = 2,
};

// After loadELF(): reads the width table and this run's variables.
void install(PS2Runtime &runtime);

// The menu's settings (any thread; before install() they wait for it).
void setPrompts(int choice);
void setModernFont(bool on);
void setReplacementsEnabled(bool on);

// The main thread, now and then: whether the controller playing is a
// PlayStation one (false for any other, or none), for Auto.
void notePlayStationPad(bool playStation);

// The font in use and where it came from, for the menu's help.
std::string fontDescription();

} // namespace rg::hdtext
