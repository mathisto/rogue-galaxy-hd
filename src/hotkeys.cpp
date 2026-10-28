#include "hotkeys.h"

#include <atomic>
#include <cstdio>
#include <utility>

namespace rg::hotkeys
{
namespace
{

Handlers g_handlers;
// Off until the menu says (Overlay::applySettings): without it there is
// no fast-forward to drive, and the game keeps its triggers.
std::atomic<bool> g_triggers{false};
std::atomic<uint32_t> g_injected{0u};
// Their presses, kept until a pump takes them: one shorter than a stalled
// frame still counts, as a real button's press does (SdlPadInput keeps
// those from its events).
std::atomic<uint32_t> g_injectedPressed{0u};
// R2 is holding fast-forward: let go of it when R2 is, or when the
// triggers stop being ours.
bool g_holding = false;

void release()
{
    if (g_holding)
    {
        g_holding = false;
        if (g_handlers.holdFastForward)
        {
            g_handlers.holdFastForward(false);
        }
    }
}

} // namespace

void setHandlers(Handlers handlers)
{
    g_handlers = std::move(handlers);
}

void setTriggersEnabled(bool on)
{
    g_triggers.store(on, std::memory_order_relaxed);
}

bool triggersEnabled()
{
    return g_triggers.load(std::memory_order_relaxed);
}

void setInjectedButtons(uint32_t held)
{
    const uint32_t was = g_injected.exchange(held, std::memory_order_relaxed);
    g_injectedPressed.fetch_or(held & ~was, std::memory_order_relaxed);
}

uint32_t injectedButtons()
{
    return g_injected.load(std::memory_order_relaxed);
}

void filterPad(uint32_t &held, uint32_t &pressed, bool passThrough)
{
    held |= g_injected.load(std::memory_order_relaxed);
    pressed |= g_injectedPressed.exchange(0u, std::memory_order_relaxed);
    if (passThrough || !triggersEnabled())
    {
        release();
        return;
    }
    if ((held & kR2) != 0u && !g_holding)
    {
        g_holding = true;
        std::fprintf(stderr, "[hotkeys] R2 held: fast-forward\n");
        if (g_handlers.holdFastForward)
        {
            g_handlers.holdFastForward(true);
        }
    }
    else if ((held & kR2) == 0u && g_holding)
    {
        std::fprintf(stderr, "[hotkeys] R2 let go\n");
        release();
    }
    if ((pressed & kL2) != 0u)
    {
        std::fprintf(stderr, "[hotkeys] L2: fast-forward on or off\n");
        if (g_handlers.toggleFastForward)
        {
            g_handlers.toggleFastForward();
        }
    }
    held &= ~(kL2 | kR2);
    pressed &= ~(kL2 | kR2);
}

} // namespace rg::hotkeys
