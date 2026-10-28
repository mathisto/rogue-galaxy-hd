#pragma once

#include <cstdint>
#include <functional>

// The controller's speed shortcuts: R2 held runs the game at the
// fast-forward speed for as long as it is held, L2 switches fast-forward on
// and off (as F4 does). They read the pad as the player's bindings make it,
// so the keyboard's L2 and R2 (Left and Right Shift by default) do the same.
//
// While they are on (the menu's Interface > Pause and speed > "L2 and R2
// control fast-forward", RG_FAST_FORWARD_TRIGGERS=0 or 1 for a run) the game
// no longer sees L2 and R2: holding R2 to go faster would otherwise also do
// whatever the game does with it. The free camera keeps them (it moves the
// camera up and down and nearer with them), so they pass through while it
// is on. The speed itself is the menu's (graphics.cpp), which multiplies
// with a quick scene's (src/quick_scenes.h).
namespace rg::hotkeys
{

// The PS2 pad report's bits, as the host pad state carries them.
constexpr uint32_t kL2 = 0x0100u;
constexpr uint32_t kR2 = 0x0200u;

struct Handlers
{
    // R2 pressed (true) or let go (false).
    std::function<void(bool held)> holdFastForward;
    // L2 pressed.
    std::function<void()> toggleFastForward;
};
// From the main thread, before the first filterPad().
void setHandlers(Handlers handlers);

void setTriggersEnabled(bool on);
bool triggersEnabled();

// The host pad before it is published to the game, once a pump: acts on L2
// and R2 and takes them out of what the game gets. `passThrough` (the free
// camera on) leaves them to the game, and lets go of a held fast-forward.
// The main thread's.
void filterPad(uint32_t &held, uint32_t &pressed, bool passThrough);

// Buttons a test adds to the host pad as if held on the controller
// (RG_INPUT's HOST steps, src/debug/input_script.h); any thread.
void setInjectedButtons(uint32_t held);
uint32_t injectedButtons();

} // namespace rg::hotkeys
