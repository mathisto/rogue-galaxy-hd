#include "ultrawide.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <numeric>
#include <iterator>
#include <tuple>
#include <utility>
#include <vector>

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "return_hook.h"
#include "scene_timing.h"
#include "widescreen.h"

namespace rg::ultrawide
{
namespace
{

constexpr uint32_t kTaskTableRun = 0x001F5340u;
constexpr uint32_t kTaskTableRunEnd = 0x001F53CCu;
constexpr uint32_t kTaskTestFlag = 0x001F57A0u;
constexpr uint32_t kGsPktSetXyzPixel = 0x0013F110u;
constexpr uint32_t kGsPktSetXyz = 0x0013EF10u;
constexpr uint32_t kGsPktSetXyzf = 0x0013F060u;
constexpr uint32_t kGsPktSetXyzVecI = 0x0013EEB0u;
constexpr uint32_t kGsPktSetXyzPixelVec = 0x0013EF70u;
constexpr uint32_t kGsPktSetXyzfVec = 0x0013EFE0u;
constexpr uint32_t kGsPktBeginPrimAd = 0x0013F6B0u;
constexpr uint32_t kGsPktWriteAd = 0x0013EC80u;
constexpr uint32_t kGsPktClose = 0x0013F5B0u;
// DrawListEndPrim(list): ends a draw group, in which a HUD element is drawn whole.
constexpr uint32_t kDrawListEndPrim = 0x00141A20u;
// ViewSetScreenMatrix(view, params): params +0x24 is the x the view's picture
// is centred on, in GS coordinates. CompassHudRender (0x370FB0 to 0x371470)
// draws the compass ring as a small 3D view.
// The pause screen's text (PauseReasonDrawMaybe) is not drawn by a task of the
// table; it is scoped from the entry of this. (PauseOverlayDraw, which
// draws the dimming, is left alone: it runs every frame and draws sprites that
// cover the screen.)
constexpr uint32_t kPauseFunctions[] = {0x002F5C40u};
// Functions that draw marks following something in the world, inside HUD
// tasks: the names over characters and enemies and the markers (ScreenMarkerDrawAll),
// the damage numbers, the door cursors. What they draw keeps the x the scene gives it.
constexpr uint32_t kWorldFunctions[] = {0x0031AF50u, 0x002D2EA0u, 0x002E7E10u, 0x00304AA0u};
// Where the minimap's tiles are written (MinimapDraw's 16x16 strips): not onto the screen
// where they end up but onto a part of the picture the game uses to build the map, which a
// polygon then reads from by its position. Moved, they would be read from the wrong place.
constexpr uint32_t kScratchSites[] = {0x0032909Cu, 0x003290D0u};
constexpr uint32_t kViewSetScreenMatrix = 0x001562B0u;
constexpr uint32_t kCompassStart = 0x00370FB0u;
constexpr uint32_t kCompassEnd = 0x00371470u;
constexpr uint32_t kScreenMatrixOriginX = 0x24u;

// The packet object (GsPkt*): +0x70 the PRIM register's value being drawn
// with, +0xF4 where the next quadword goes, +0x120 and +0x124 the origin
// added to x and y (sixteenths of a pixel).
constexpr uint32_t kPacketPrim = 0x70u;
constexpr uint32_t kPacketWrite = 0xF4u;
constexpr uint32_t kPacketOriginX = 0x120u;
constexpr uint32_t kPacketOriginY = 0x124u;
constexpr uint32_t kPrimSprite = 6u;
constexpr uint32_t kRegisterXyz2 = 5u;

constexpr uint32_t kRamMask = 0x1FFFFFFFu;
constexpr float kFrameWidth = 512.0f;
// A sprite from within this of both edges of the frame covers the screen.
constexpr float kWholeMargin = 2.0f;
// A sprite this much of the frame wide is a backdrop (a dimming, a band behind an emblem), not a part of a picture.
constexpr float kBackdropWidth = 0.9f * 512.0f;
constexpr float kFrameHalf = 256.0f;
constexpr float kSixteenNine = 16.0f / 9.0f;
constexpr float kTwentyOneNine = 21.0f / 9.0f;
// Edges placement: the third of the frame at each side belongs to its edge.
constexpr float kLeftZone = kFrameWidth / 3.0f;
constexpr float kRightZone = kFrameWidth * 2.0f / 3.0f;

// What a task's draws are, for the mapping.
enum class Cls : uint8_t
{
    None, // not touched: the world, full-screen sprites, everything unlisted
    Hud,  // laid out for 16:9 in the frame (the game's wide mode): spreads over the HUD area
    Text, // laid out for 4:3 (windows, captions, prompts): stays in a 16:9 box
};

PS2Runtime::RecompiledFunction g_taskTableRun = nullptr;
PS2Runtime::RecompiledFunction g_taskTestFlag = nullptr;
PS2Runtime::RecompiledFunction g_setXyzPixel = nullptr;
PS2Runtime::RecompiledFunction g_setXyz = nullptr;
PS2Runtime::RecompiledFunction g_setXyzf = nullptr;
PS2Runtime::RecompiledFunction g_setXyzVecI = nullptr;
PS2Runtime::RecompiledFunction g_setXyzPixelVec = nullptr;
PS2Runtime::RecompiledFunction g_setXyzfVec = nullptr;
PS2Runtime::RecompiledFunction g_beginPrimAd = nullptr;
PS2Runtime::RecompiledFunction g_writeAd = nullptr;
PS2Runtime::RecompiledFunction g_close = nullptr;
PS2Runtime::RecompiledFunction g_drawListEndPrim = nullptr;
PS2Runtime::RecompiledFunction g_viewSetScreenMatrix = nullptr;

std::atomic<int> g_area{0};
std::atomic<int> g_placement{0};
std::atomic<int> g_fovCap{0};

// The draw function of the task being run by a task table, 0 outside one. The
// game's thread alone writes it.
uint32_t g_task = 0u;
// How many of kWorldFunctions are running.
uint32_t g_world = 0u;
// The scissor last written (x edges, in frame pixels, the right one exclusive), 0..0 when it is the
// frame's: what a picture is clipped to says what it belongs to.
float g_clip0 = 0.0f;
float g_clip1 = 0.0f;
// Where that scissor sits in its packet, and whether it has been placed (a scissor written before the
// HUD draw that uses it is placed when that draw begins).
uint32_t g_clipQuad = 0u;
bool g_clipPlaced = true;
bool g_trace = false;
int g_debugMode = 0; // RG_UW_MODE: 1 no sprite pairs, 2 modify the arguments and call the original

constexpr uint32_t kCompassTask = 0x001F6F60u; // CompassHudDraw

// The tasks that are screen-anchored HUD. Everything else is left as it is.
std::map<uint32_t, Cls> g_classes = {
    {0x001F6F60u, Cls::Hud},  // CompassHudDraw
    {0x001FFBC0u, Cls::Hud},  // MinimapDraw (and the bar the prompts sit on)
    {0x00203E70u, Cls::Hud},  // BattleMenuDraw
    {0x00204AB0u, Cls::Hud},  // TaskDrawMenuBtlChg
    {0x002058B0u, Cls::Hud},  // TaskDrawStatus
    {0x0027A8B0u, Cls::Text}, // TaskDrawMessage
    {0x001FFB00u, Cls::Text}, // TaskDrawCaption
    {0x002F5C40u, Cls::Text}, // PauseReasonDrawMaybe
    {0x003387A0u, Cls::Text}, // TaskDrawTBox
};

Cls classOf(uint32_t task)
{
    const auto found = g_classes.find(task);
    return found == g_classes.end() ? Cls::None : found->second;
}

float areaAspect(float windowAspect)
{
    switch (g_area.load(std::memory_order_relaxed))
    {
    case 1:
        return kTwentyOneNine;
    case 2:
        return windowAspect;
    default:
        return kSixteenNine;
    }
}

// What a span's centre is moved by about the middle of the frame: the HUD
// area's share of the window for the HUD, a 16:9 box for text.
float positionScale(Cls cls)
{
    const float window = rg::widescreen::aspect();
    const float shape = cls == Cls::Text ? kSixteenNine : std::min(areaAspect(window), window);
    return std::min(1.0f, shape / window);
}

// What a span's width is multiplied by: the shape of the game's wide mode
// (16:9) over the window's, so a sprite keeps its look at any width.
float widthScale()
{
    return std::min(1.0f, kSixteenNine / rg::widescreen::aspect());
}

bool active()
{
    return rg::widescreen::wide() && rg::widescreen::aspect() > kSixteenNine + 0.005f;
}

bool edges(Cls cls)
{
    return cls == Cls::Hud && g_placement.load(std::memory_order_relaxed) == 1;
}

// Where the middle of something drawn from x0 to x1 (frame pixels) goes.
float centre(float middle, Cls cls)
{
    if (edges(cls))
    {
        if (middle < kLeftZone)
        {
            return middle * widthScale();
        }
        if (middle > kRightZone)
        {
            return kFrameWidth - (kFrameWidth - middle) * widthScale();
        }
    }
    return kFrameHalf + (middle - kFrameHalf) * positionScale(cls);
}

// A sprite (two corners) or a scissor: the middle moves, the width scales.
// Spans as wide as the frame are fades, filters and bars: they stay.
void mapSpan(float x0, float x1, Cls cls, float &out0, float &out1)
{
    if (x0 <= 0.0f && x1 >= kFrameWidth)
    {
        out0 = x0;
        out1 = x1;
        return;
    }
    const float half = (x1 - x0) * 0.5f * widthScale();
    const float middle = centre((x0 + x1) * 0.5f, cls);
    out0 = middle - half;
    out1 = middle + half;
}

// One corner of something that is not a sprite: the same without a width.
float mapPoint(float x, Cls cls)
{
    return centre(x, cls);
}

// The tracing of what draws what: one line per (task, caller, writer).
struct Stat
{
    uint32_t count = 0u;
    float minX = 1e9f;
    float maxX = -1e9f;
};
std::map<std::tuple<uint32_t, uint32_t, int>, Stat> g_stats;
std::chrono::steady_clock::time_point g_lastDump = std::chrono::steady_clock::now();

void note(int writer, uint32_t ra, float x, int origin = -1)
{
    Stat &stat = g_stats[{g_task, ra, writer + 100 * origin}];
    ++stat.count;
    stat.minX = std::min(stat.minX, x);
    stat.maxX = std::max(stat.maxX, x);
    const auto now = std::chrono::steady_clock::now();
    if (now - g_lastDump > std::chrono::seconds(6))
    {
        g_lastDump = now;
        std::fprintf(stderr, "[uw] -----\n");
        for (const auto &[key, value] : g_stats)
        {
            std::fprintf(stderr, "[uw] task=%08X ra=%08X w=%d n=%u x=%.1f..%.1f\n", std::get<0>(key),
                         std::get<1>(key), std::get<2>(key), value.count, value.minX, value.maxX);
        }
        g_stats.clear();
    }
}

// Guest memory the way the generated code reaches it: the scratchpad and
// the I/O registers are not in the RDRAM block (the game builds some of its
// packets in the scratchpad).
PS2Runtime *g_runtime = nullptr;
R5900Context *g_context = nullptr;

template <typename T>
T read(uint8_t *rdram, uint32_t address)
{
    static_assert(sizeof(T) == 1 || sizeof(T) == 4 || sizeof(T) == 8);
    const bool special = PS2Runtime::isSpecialAddress(address);
    if constexpr (sizeof(T) == 1)
    {
        const uint8_t value = special ? g_runtime->Load8(rdram, g_context, address) : Ps2FastRead8(rdram, address);
        T out;
        std::memcpy(&out, &value, sizeof(T));
        return out;
    }
    else if constexpr (sizeof(T) == 4)
    {
        const uint32_t value = special ? g_runtime->Load32(rdram, g_context, address) : Ps2FastRead32(rdram, address);
        T out;
        std::memcpy(&out, &value, sizeof(T));
        return out;
    }
    else
    {
        const uint64_t value = special ? g_runtime->Load64(rdram, g_context, address) : Ps2FastRead64(rdram, address);
        T out;
        std::memcpy(&out, &value, sizeof(T));
        return out;
    }
}

template <typename T>
void write(uint8_t *rdram, uint32_t address, T value)
{
    static_assert(sizeof(T) == 4 || sizeof(T) == 8);
    const bool special = PS2Runtime::isSpecialAddress(address);
    if constexpr (sizeof(T) == 4)
    {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        if (special)
        {
            g_runtime->Store32(rdram, g_context, address, bits);
        }
        else
        {
            Ps2FastWrite32(rdram, address, bits);
        }
    }
    else
    {
        uint64_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        if (special)
        {
            g_runtime->Store64(rdram, g_context, address, bits);
        }
        else
        {
            Ps2FastWrite64(rdram, address, bits);
        }
    }
}

void flushBatch(uint8_t *rdram);
void dropBatch();

// The table run is over: the HUD it drew is placed, as one picture of all its
// tasks, while its packets are still the frame's (the list is not sent until
// the run is done).
uint32_t g_tableDepth = 0u;
void taskTableDone(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t outer)
{
    g_runtime = runtime;
    g_context = ctx;
    g_task = outer;
    if (g_tableDepth != 0u && --g_tableDepth == 0u)
    {
        flushBatch(rdram);
        g_world = 0u;
        // The scissor written in this frame's packets is the frame's: the next frame writes its own.
        g_clipQuad = 0u;
        g_clipPlaced = true;
        g_clip0 = g_clip1 = 0.0f;
    }
}

void taskTableRun(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_runtime = runtime;
    g_context = ctx;
    if (g_tableDepth == 0u)
    {
        // What an earlier run left unplaced is not placed: its packets are the game's again.
        dropBatch();
        g_world = 0u;
    }
    if (g_tableDepth > 8u)
    {
        g_tableDepth = 0u; // a run that never reported its end
    }
    ++g_tableDepth;
    rg::hooks::callThen(g_taskTableRun, rdram, ctx, runtime, &taskTableDone, g_task);
}

// A table run starts outside any task. (Not wrapped: the run goes on at an
// indirect call, and a wrapper that waits for its end stalled the game.)
void taskTableEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_runtime = runtime;
    g_context = ctx;
    // What a frame left unplaced is not placed: its packets are the game's again.
    dropBatch();
    g_world = 0u;
    g_task = 0u;
    g_taskTableRun(rdram, ctx, runtime);
}

void taskTestFlag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_runtime = runtime;
    g_context = ctx;
    const uint32_t ra = getRegU32(ctx, 31);
    if (ra >= kTaskTableRun && ra < kTaskTableRunEnd && getRegU32(ctx, 5) == 0u)
    {
        g_task = read<uint32_t>(rdram, getRegU32(ctx, 4) + 4u);
    }
    g_taskTestFlag(rdram, ctx, runtime);
}

// The packet's x origin in pixels (what the target of the draw puts at x = 0).
int originOf(uint8_t *rdram, uint32_t packet)
{
    return read<int32_t>(rdram, packet + kPacketOriginX) / 16;
}

// The origin of a draw into the 512 wide frame (the picture): 2048 less half
// its width, in pixels. Draws into the smaller targets some tasks make first
// (a treasure box's message, the map's tiles) are not the picture.
constexpr int kPictureOrigin = 2048 - 256;

// Whether the draw being written into `packet` is mapped, and as what.
Cls current(uint8_t *rdram, uint32_t packet)
{
    if (g_task == 0u || g_world != 0u || !active() || originOf(rdram, packet) != kPictureOrigin)
    {
        return Cls::None;
    }
    return classOf(g_task);
}

// The vertices drawn by a task, placed once the task is done. The HUD's
// pieces are not alone: a minimap is fifty strips side by side, a gauge a
// plate and its bar. Moving each sprite by itself and scaling it by itself
// pulls such a picture apart (or overlaps it) as soon as the width scale and
// the move differ. So the sprites (and points) drawn from one place in the
// game's code are grouped by what they touch (a gap of a few pixels ends a
// group), and a group moves as one: its middle goes where the area puts it,
// its width scales by the window's shape, and what is inside keeps its place
// in it. Text, whose width and place scale alike, and the HUD at a 16:9
// area, move vertex by vertex.
struct BatchVertex
{
    uint32_t quad;
    int32_t originX;
    float x;
};
struct BatchSpan
{
    float x0, x1;
    size_t first, last;
    uint32_t site;
    float y0, y1;
    float clip0, clip1; // the scissor it is drawn under, when that is less than the frame
};
std::vector<BatchVertex> g_batch;
std::vector<BatchSpan> g_spans;
Cls g_batchClass = Cls::None;
size_t g_cornerIndex = 0u;
bool g_cornerOpen = false;
uint32_t g_cornerSite = 0u;
float g_cornerY = 0.0f;
float g_cornerClip0 = 0.0f;
float g_cornerClip1 = 0.0f;
// The primitive that is not a sprite (triangles, strips, lines): one span.
bool g_polygonOpen = false;
BatchSpan g_polygon{};
constexpr float kGroupGap = 12.0f;
// A sprite this wide (a bar, a window) is not part of a picture of small ones.
constexpr float kLoneWidth = 150.0f;

// The 16:9 layout laid into the area, vertex by vertex: right for text (whose
// width and place scale alike), and where no group is needed.
float affine(float x, Cls cls)
{
    return kFrameHalf + (x - kFrameHalf) * positionScale(cls);
}

bool grouped(Cls cls)
{
    return cls == Cls::Hud && (g_placement.load(std::memory_order_relaxed) == 1 ||
                               g_area.load(std::memory_order_relaxed) != 0);
}

void setVertexX(uint8_t *rdram, const BatchVertex &vertex, float x)
{
    const uint64_t data = read<uint64_t>(rdram, vertex.quad);
    const uint32_t x16 = static_cast<uint32_t>(std::lround(x * 16.0f) + vertex.originX) & 0xFFFFu;
    write<uint64_t>(rdram, vertex.quad, (data & ~0xFFFFull) | x16);
}

// A new primitive: a sprite's first corner left open has no second.
void endPrimitive()
{
    if (g_cornerOpen)
    {
        g_spans.push_back({g_batch[g_cornerIndex].x, g_batch[g_cornerIndex].x, g_cornerIndex, g_cornerIndex,
                           g_cornerSite, g_cornerY, g_cornerY, g_cornerClip0, g_cornerClip1});
        g_cornerOpen = false;
    }
    if (g_polygonOpen)
    {
        g_spans.push_back(g_polygon);
        g_polygonOpen = false;
    }
}

// The vertices of a frame that never got placed are forgotten.
void dropBatch()
{
    g_batch.clear();
    g_spans.clear();
    g_cornerOpen = false;
    g_polygonOpen = false;
}

// The task is done: its vertices go where they belong.
void flushBatch(uint8_t *rdram)
{
    endPrimitive();
    if (g_batch.empty())
    {
        return;
    }
    std::vector<BatchSpan> spans;
    spans.swap(g_spans);
    const Cls cls = g_batchClass;
    // Sprites as wide as the frame (a fade, a filter, a picture of the screen)
    // stay whole.
    std::vector<BatchSpan> parts;
    for (const BatchSpan &span : spans)
    {
        if ((span.x0 <= kWholeMargin && span.x1 >= kFrameWidth - kWholeMargin) || span.x1 - span.x0 >= kBackdropWidth)
        {
            for (size_t v = span.first; v <= span.last; ++v)
            {
                setVertexX(rdram, g_batch[v], g_batch[v].x);
            }
        }
        else
        {
            parts.push_back(span);
            if (g_trace && (span.y1 > 440.0f || span.y0 < 0.0f))
            {
                static std::map<std::pair<uint32_t, uint32_t>, int> seen;
                if (seen[{g_task, span.site}]++ == 0)
                {
                    std::fprintf(stderr, "[uw] span off the picture task=%08X site=%08X x=%.1f..%.1f y=%.1f..%.1f\n", g_task,
                                 span.site, span.x0, span.x1, span.y0, span.y1);
                }
            }
            if (g_trace && span.x1 - span.x0 >= 200.0f)
            {
                std::fprintf(stderr, "[uw] wide span task=%08X site=%08X x=%.1f..%.1f y=%.1f..%.1f\n", g_task, span.site, span.x0,
                             span.x1, span.y0, span.y1);
            }
        }
    }
    if (g_trace)
    {
        static int flushes = 0;
        if (++flushes % 400 == 0)
        {
            std::fprintf(stderr, "[uw] ---- flush %d, %zu spans, class %d\n", flushes, parts.size(), static_cast<int>(cls));
            for (size_t i = 0; i < parts.size() && i < 70u; ++i)
            {
                const BatchSpan &sp = parts[i];
                std::fprintf(stderr, "[uw]   site %08X x %.1f..%.1f y %.1f..%.1f clip %.0f..%.0f -> %.1f\n", sp.site, sp.x0, sp.x1,
                             sp.y0, sp.y1, sp.clip0, sp.clip1, affine(sp.x0, cls));
            }
        }
    }
    if (!grouped(cls))
    {
        for (const BatchSpan &span : parts)
        {
            for (size_t v = span.first; v <= span.last; ++v)
            {
                setVertexX(rdram, g_batch[v], affine(g_batch[v].x, cls));
            }
        }
        g_batch.clear();
        return;
    }
    // Groups: what touches (in x and in y, within a few pixels) is one picture,
    // wherever in the game's code it was drawn: a map's strips, its cursor and
    // the marks on it. A sprite as wide as a bar or a window is a thing of its
    // own.
    struct Group
    {
        float x0, x1, y0, y1;
        std::vector<size_t> members;
        bool alone;
        bool clipped; // x0..x1 is the scissor
    };
    std::vector<Group> groups;
    const auto touches = [](const Group &g, const BatchSpan &s) {
        return s.x0 <= g.x1 + kGroupGap && s.x1 >= g.x0 - kGroupGap && s.y0 <= g.y1 + kGroupGap &&
               s.y1 >= g.y0 - kGroupGap;
    };
    // First what is clipped: what is clipped together moves together.
    for (size_t i = 0; i < parts.size(); ++i)
    {
        const BatchSpan &span = parts[i];
        if (span.clip1 <= span.clip0)
        {
            continue;
        }
        bool found = false;
        for (Group &group : groups)
        {
            if (group.clipped && group.x0 == span.clip0 && group.x1 == span.clip1)
            {
                group.members.push_back(i);
                group.y0 = std::min(group.y0, span.y0);
                group.y1 = std::max(group.y1, span.y1);
                found = true;
                break;
            }
        }
        if (!found)
        {
            groups.push_back({span.clip0, span.clip1, span.y0, span.y1, {i}, true, true});
        }
    }
    for (size_t i = 0; i < parts.size(); ++i)
    {
        const BatchSpan &span = parts[i];
        if (span.clip1 > span.clip0)
        {
            continue;
        }
        // A mark inside a clipped picture (a map's cursor) is part of it.
        bool inside = false;
        for (Group &group : groups)
        {
            const float middle = (span.x0 + span.x1) * 0.5f;
            const float centreY = (span.y0 + span.y1) * 0.5f;
            if (group.clipped && middle >= group.x0 && middle <= group.x1 && centreY >= group.y0 &&
                centreY <= group.y1)
            {
                group.members.push_back(i);
                inside = true;
                break;
            }
        }
        if (inside)
        {
            continue;
        }
        if (span.x1 - span.x0 >= kLoneWidth)
        {
            groups.push_back({span.x0, span.x1, span.y0, span.y1, {i}, true, false});
            continue;
        }
        constexpr size_t kNone = static_cast<size_t>(-1);
        size_t home = kNone;
        for (size_t g = 0; g < groups.size(); ++g)
        {
            if (groups[g].alone || groups[g].clipped || !touches(groups[g], span))
            {
                continue;
            }
            if (home == kNone)
            {
                home = g;
                Group &target = groups[home];
                target.members.push_back(i);
                target.x0 = std::min(target.x0, span.x0);
                target.x1 = std::max(target.x1, span.x1);
                target.y0 = std::min(target.y0, span.y0);
                target.y1 = std::max(target.y1, span.y1);
            }
            else
            {
                // The span joins two groups: they are one.
                Group &target = groups[home];
                const Group &other = groups[g];
                target.members.insert(target.members.end(), other.members.begin(), other.members.end());
                target.x0 = std::min(target.x0, other.x0);
                target.x1 = std::max(target.x1, other.x1);
                target.y0 = std::min(target.y0, other.y0);
                target.y1 = std::max(target.y1, other.y1);
                groups.erase(groups.begin() + static_cast<std::ptrdiff_t>(g));
                if (g < home)
                {
                    --home;
                }
                --g;
            }
        }
        if (home == kNone)
        {
            groups.push_back({span.x0, span.x1, span.y0, span.y1, {i}, false, false});
        }
    }
    const float scale = widthScale();
    for (const Group &group : groups)
    {
        const float middle = (group.x0 + group.x1) * 0.5f;
        const float moved = centre(middle, cls);
        for (size_t member : group.members)
        {
            for (size_t v = parts[member].first; v <= parts[member].last; ++v)
            {
                setVertexX(rdram, g_batch[v], moved + (g_batch[v].x - middle) * scale);
            }
        }
    }
    g_batch.clear();
}

// Writes one vertex into the packet at `packet` (the writers' own work: an
// A+D quadword with the XYZ2 register), at the x the area gives it; flushBatch
// places it for good.
void emit(uint8_t *rdram, uint32_t packet, float x, int32_t y16, int32_t z, Cls cls, uint32_t site)
{
    const uint32_t pointer = read<uint32_t>(rdram, packet + kPacketWrite);
    const int32_t originX = read<int32_t>(rdram, packet + kPacketOriginX);
    const int32_t originY = read<int32_t>(rdram, packet + kPacketOriginY);
    if (!g_batch.empty() && g_batchClass != cls)
    {
        flushBatch(rdram);
    }
    g_batchClass = cls;
    if (!g_clipPlaced && g_clipQuad != 0u && g_clip1 > g_clip0)
    {
        float first, second;
        mapSpan(g_clip0, g_clip1, cls, first, second);
        const int32_t left = static_cast<int32_t>(std::floor(first + 0.001f));
        const int32_t right = static_cast<int32_t>(std::ceil(second - 0.001f)) - 1;
        const uint64_t data = read<uint64_t>(rdram, g_clipQuad);
        write<uint64_t>(rdram, g_clipQuad,
                        (data & ~0xFFFFFFFFull) | static_cast<uint64_t>(left & 0xFFFF) |
                            (static_cast<uint64_t>(right & 0xFFFF) << 16));
        g_clipPlaced = true;
    }
    const bool sprite = (read<uint8_t>(rdram, packet + kPacketPrim) & 7u) == kPrimSprite;
    const float y = static_cast<float>(y16) / 16.0f;
    for (const uint32_t scratch : kScratchSites)
    {
        if (site == scratch)
        {
            // Written as the game would write it.
            const int32_t plain = static_cast<int32_t>(std::lround(x * 16.0f)) + originX;
            const int64_t value = (static_cast<int64_t>(z) << 32) | static_cast<int64_t>(plain) |
                                  (static_cast<int64_t>(y16 + originY) << 16);
            write<int64_t>(rdram, pointer, value);
            write<int64_t>(rdram, pointer + 8u, kRegisterXyz2);
            write<uint32_t>(rdram, packet + kPacketWrite, pointer + 0x10u);
            return;
        }
    }
    const size_t index = g_batch.size();
    if (g_trace)
    {
        static std::map<uint32_t, uint32_t> owners;
        if (index == 0u)
        {
            owners.clear();
        }
        const auto found = owners.find(pointer);
        if (found != owners.end())
        {
            std::fprintf(stderr, "[uw] quad %08X written twice, sites %08X and %08X, task %08X\n", pointer, found->second,
                         site, g_task);
        }
        owners[pointer] = site;
    }
    g_batch.push_back({pointer, originX, x});
    if (sprite)
    {
        if (!g_cornerOpen)
        {
            g_cornerOpen = true;
            g_cornerIndex = index;
            g_cornerSite = site;
            g_cornerY = y;
            g_cornerClip0 = g_clip0;
            g_cornerClip1 = g_clip1;
        }
        else
        {
            g_spans.push_back({std::min(g_batch[g_cornerIndex].x, x), std::max(g_batch[g_cornerIndex].x, x),
                               g_cornerIndex, index, g_cornerSite, std::min(g_cornerY, y), std::max(g_cornerY, y),
                               g_cornerClip0, g_cornerClip1});
            g_cornerOpen = false;
        }
    }
    else if (!g_polygonOpen)
    {
        g_polygonOpen = true;
        g_polygon = {x, x, index, index, site, y, y, g_clip0, g_clip1};
    }
    else
    {
        g_polygon.x0 = std::min(g_polygon.x0, x);
        g_polygon.x1 = std::max(g_polygon.x1, x);
        g_polygon.y0 = std::min(g_polygon.y0, y);
        g_polygon.y1 = std::max(g_polygon.y1, y);
        g_polygon.last = index;
    }
    const int32_t x16 = static_cast<int32_t>(std::lround(affine(x, cls) * 16.0f)) + originX;
    const int32_t yy = y16 + originY;
    const int64_t value =
        (static_cast<int64_t>(z) << 32) | static_cast<int64_t>(x16) | (static_cast<int64_t>(yy) << 16);
    write<int64_t>(rdram, pointer, value);
    write<int64_t>(rdram, pointer + 8u, kRegisterXyz2);
    write<uint32_t>(rdram, packet + kPacketWrite, pointer + 0x10u);
}

// GsPktBeginPrimAD(packet, ...): a new primitive; its first vertex is a corner.
void beginPrimAd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    endPrimitive();
    g_beginPrimAd(rdram, ctx, runtime);
}

// DrawListEndPrim(list): the group is whole; its vertices go where they belong.
void drawListEndPrim(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_drawListEndPrim(rdram, ctx, runtime);
}

// GsPktClose(packet): the block is whole.
void close(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    endPrimitive();
    g_close(rdram, ctx, runtime);
}

// GsPktSetXYZ(packet, x, y, z): x and y in sixteenths of a pixel.
void setXyz(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_runtime = runtime;
    g_context = ctx;
    if (g_task != 0u)
    {
        const int32_t x16 = static_cast<int32_t>(getRegU32(ctx, 5));
        if (g_trace)
        {
            note(1, getRegU32(ctx, 31), static_cast<float>(x16) / 16.0f, originOf(rdram, getRegU32(ctx, 4)));
        }
        const Cls cls = current(rdram, getRegU32(ctx, 4));
        if (cls != Cls::None && g_debugMode == 2)
        {
            ctx->r[5] = _mm_set_epi64x(0, static_cast<int64_t>(std::lround(mapPoint(static_cast<float>(x16) / 16.0f, cls) * 16.0f)));
        }
        else if (cls != Cls::None)
        {
            emit(rdram, getRegU32(ctx, 4), static_cast<float>(x16) / 16.0f, static_cast<int32_t>(getRegU32(ctx, 6)),
                 static_cast<int32_t>(getRegU32(ctx, 7)), cls, getRegU32(ctx, 31));
            ctx->pc = getRegU32(ctx, 31);
            return;
        }
    }
    g_setXyz(rdram, ctx, runtime);
}

// GsPktSetXYZf(packet, f12 x, f13 y, f14 z): pixels.
void setXyzf(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_runtime = runtime;
    g_context = ctx;
    if (g_task != 0u)
    {
        if (g_trace)
        {
            note(2, getRegU32(ctx, 31), ctx->f[12], originOf(rdram, getRegU32(ctx, 4)));
        }
        const Cls cls = current(rdram, getRegU32(ctx, 4));
        if (cls != Cls::None && g_debugMode == 2)
        {
            ctx->f[12] = mapPoint(ctx->f[12], cls);
        }
        else if (cls != Cls::None)
        {
            emit(rdram, getRegU32(ctx, 4), ctx->f[12], static_cast<int32_t>(ctx->f[13] * 16.0f),
                 static_cast<int32_t>(ctx->f[14]), cls, getRegU32(ctx, 31));
            ctx->pc = getRegU32(ctx, 31);
            return;
        }
    }
    g_setXyzf(rdram, ctx, runtime);
}

// GsPktSetXYZPixel(packet, x, y, z): whole pixels.
void setXyzPixel(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_runtime = runtime;
    g_context = ctx;
    if (g_task != 0u)
    {
        const int32_t x = static_cast<int32_t>(getRegU32(ctx, 5));
        if (g_trace)
        {
            note(0, getRegU32(ctx, 31), static_cast<float>(x), originOf(rdram, getRegU32(ctx, 4)));
        }
        const Cls cls = current(rdram, getRegU32(ctx, 4));
        if (cls != Cls::None && g_debugMode == 2)
        {
            ctx->r[5] = _mm_set_epi64x(0, static_cast<int64_t>(std::lround(mapPoint(static_cast<float>(x), cls))));
        }
        else if (cls != Cls::None)
        {
            emit(rdram, getRegU32(ctx, 4), static_cast<float>(x), static_cast<int32_t>(getRegU32(ctx, 6)) * 16,
                 static_cast<int32_t>(getRegU32(ctx, 7)), cls, getRegU32(ctx, 31));
            ctx->pc = getRegU32(ctx, 31);
            return;
        }
    }
    g_setXyzPixel(rdram, ctx, runtime);
}

// The vector forms: (packet, pointer to x, y, z).
void setXyzVecI(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_runtime = runtime;
    g_context = ctx;
    if (g_task != 0u)
    {
        const uint32_t vector = getRegU32(ctx, 5);
        const float x = static_cast<float>(read<int32_t>(rdram, vector)) / 16.0f;
        if (g_trace)
        {
            note(3, getRegU32(ctx, 31), x, originOf(rdram, getRegU32(ctx, 4)));
        }
        const Cls cls = current(rdram, getRegU32(ctx, 4));
        if (cls != Cls::None)
        {
            emit(rdram, getRegU32(ctx, 4), x, read<int32_t>(rdram, vector + 4u), read<int32_t>(rdram, vector + 8u),
                 cls, getRegU32(ctx, 31));
            ctx->pc = getRegU32(ctx, 31);
            return;
        }
    }
    g_setXyzVecI(rdram, ctx, runtime);
}

void setXyzPixelVec(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_runtime = runtime;
    g_context = ctx;
    if (g_task != 0u)
    {
        const uint32_t vector = getRegU32(ctx, 5);
        const float x = static_cast<float>(read<int32_t>(rdram, vector));
        if (g_trace)
        {
            note(4, getRegU32(ctx, 31), x, originOf(rdram, getRegU32(ctx, 4)));
        }
        const Cls cls = current(rdram, getRegU32(ctx, 4));
        if (cls != Cls::None)
        {
            emit(rdram, getRegU32(ctx, 4), x, read<int32_t>(rdram, vector + 4u) * 16,
                 read<int32_t>(rdram, vector + 8u), cls, getRegU32(ctx, 31));
            ctx->pc = getRegU32(ctx, 31);
            return;
        }
    }
    g_setXyzPixelVec(rdram, ctx, runtime);
}

void setXyzfVec(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_runtime = runtime;
    g_context = ctx;
    if (g_task != 0u)
    {
        const uint32_t vector = getRegU32(ctx, 5);
        const float x = read<float>(rdram, vector);
        if (g_trace)
        {
            note(5, getRegU32(ctx, 31), x, originOf(rdram, getRegU32(ctx, 4)));
        }
        const Cls cls = current(rdram, getRegU32(ctx, 4));
        if (cls != Cls::None)
        {
            emit(rdram, getRegU32(ctx, 4), x, static_cast<int32_t>(read<float>(rdram, vector + 4u) * 16.0f),
                 static_cast<int32_t>(read<float>(rdram, vector + 8u)), cls, getRegU32(ctx, 31));
            ctx->pc = getRegU32(ctx, 31);
            return;
        }
    }
    g_setXyzfVec(rdram, ctx, runtime);
}

// GsPktWriteAD(packet, register, value): the scissor (SCISSOR_1, 0x40) of a
// HUD draw is mapped as its sprites are: x0 | x1 << 16 | y0 << 32 | y1 << 48,
// both x inclusive, so the edges x0 and x1 + 1 are what moves. A scissor as
// wide as the frame (the default) stays as it is.
void writeAd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_runtime = runtime;
    g_context = ctx;
    if (getRegU32(ctx, 5) == 0x40u)
    {
        uint64_t value = static_cast<uint64_t>(_mm_cvtsi128_si64(ctx->r[6]));
        const int32_t x0 = static_cast<int32_t>(value & 0xFFFFu);
        const int32_t x1 = static_cast<int32_t>((value >> 16) & 0xFFFFu);
        // A scissor of the whole frame (give or take the border the game leaves) clips nothing a
        // picture could belong to.
        const bool whole = x0 <= kWholeMargin && x1 + 1 >= kFrameWidth - kWholeMargin;
        g_clip0 = whole ? 0.0f : static_cast<float>(x0);
        g_clip1 = whole ? 0.0f : static_cast<float>(x1 + 1);
        g_clipQuad = read<uint32_t>(rdram, getRegU32(ctx, 4) + kPacketWrite);
        g_clipPlaced = whole;
        if (g_task != 0u)
        {
            if (g_trace)
            {
                note(6, getRegU32(ctx, 31), static_cast<float>(x0 * 1000 + x1));
            }
            const Cls cls = current(rdram, getRegU32(ctx, 4));
            if (cls != Cls::None && !whole)
            {
                float first, second;
                mapSpan(static_cast<float>(x0), static_cast<float>(x1 + 1), cls, first, second);
                const int32_t left = static_cast<int32_t>(std::floor(first + 0.001f));
                const int32_t right = static_cast<int32_t>(std::ceil(second - 0.001f)) - 1;
                value = (value & ~0xFFFFFFFFull) | static_cast<uint64_t>(left & 0xFFFF) |
                        (static_cast<uint64_t>(right & 0xFFFF) << 16);
                ctx->r[6] = _mm_set_epi64x(0, static_cast<int64_t>(value));
                g_clipPlaced = true;
            }
        }
    }
    g_writeAd(rdram, ctx, runtime);
}

// The compass is a 3D ring in a view of its own: where the view is centred
// moves as a HUD element does (its size is right already: widescreen.cpp
// gives it the window's aspect).
void viewSetScreenMatrix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t ra = getRegU32(ctx, 31);
    if (ra >= kCompassStart && ra < kCompassEnd && active() && classOf(kCompassTask) == Cls::Hud)
    {
        g_runtime = runtime;
        g_context = ctx;
        const uint32_t params = getRegU32(ctx, 5) + kScreenMatrixOriginX;
        const float x = read<float>(rdram, params);
        // In GS coordinates (the picture's origin added) or in pixels.
        const float origin = x > 1000.0f ? static_cast<float>(kPictureOrigin) : 0.0f;
        if (g_trace)
        {
            std::fprintf(stderr, "[uw] compass centre %.1f (origin %.0f)\n", x, origin);
        }
        write<float>(rdram, params, centre(x - origin, Cls::Hud) + origin);
    }
    g_viewSetScreenMatrix(rdram, ctx, runtime);
}

// The entry of a function that draws without a task of its own: what it draws
// is scoped as its own task, until the next task starts.
PS2Runtime::RecompiledFunction g_scoped[std::size(kPauseFunctions)] = {};

template <size_t Index>
void scopedEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_runtime = runtime;
    g_context = ctx;
    flushBatch(rdram);
    g_task = kPauseFunctions[Index];
    g_scoped[Index](rdram, ctx, runtime);
}

template <size_t... Index>
bool installScoped(PS2Runtime &runtime, std::index_sequence<Index...>)
{
    bool complete = true;
    ((complete &= rg::timing::hookFunction(runtime, kPauseFunctions[Index], &scopedEntry<Index>, g_scoped[Index])), ...);
    return complete;
}

PS2Runtime::RecompiledFunction g_worldFunctions[std::size(kWorldFunctions)] = {};

void worldDone(uint8_t *, R5900Context *, PS2Runtime *, uint32_t)
{
    if (g_world != 0u)
    {
        --g_world;
    }
}

template <size_t Index>
void worldEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_runtime = runtime;
    g_context = ctx;
    ++g_world;
    rg::hooks::callThen(g_worldFunctions[Index], rdram, ctx, runtime, &worldDone, 0u);
}

template <size_t... Index>
bool installWorld(PS2Runtime &runtime, std::index_sequence<Index...>)
{
    bool complete = true;
    const char *mask = std::getenv("RG_UW_WORLD");
    const unsigned enabled = mask ? static_cast<unsigned>(std::strtoul(mask, nullptr, 0)) : 0xFFu;
    ((complete &= (enabled & (1u << Index)) == 0u ||
                  rg::timing::hookFunction(runtime, kWorldFunctions[Index], &worldEntry<Index>, g_worldFunctions[Index])),
     ...);
    return complete;
}

void parseClasses(const char *text)
{
    // "2058B0:H,27A8B0:T": function address in hex, H(ud) or T(ext), or N for none.
    g_classes.clear();
    const std::string list = text;
    size_t at = 0u;
    while (at < list.size())
    {
        size_t end = list.find(',', at);
        if (end == std::string::npos)
        {
            end = list.size();
        }
        const std::string item = list.substr(at, end - at);
        const size_t colon = item.find(':');
        if (colon != std::string::npos && colon + 1u < item.size())
        {
            const uint32_t address = static_cast<uint32_t>(std::strtoul(item.c_str(), nullptr, 16));
            const char kind = item[colon + 1];
            g_classes[address] = kind == 'H' ? Cls::Hud : kind == 'T' ? Cls::Text : Cls::None;
        }
        at = end + 1u;
    }
}

} // namespace

void install(PS2Runtime &runtime)
{
    if (const char *value = std::getenv("RG_HUD_AREA"))
    {
        setHudArea(std::atoi(value));
    }
    if (const char *value = std::getenv("RG_HUD_PLACEMENT"))
    {
        setHudPlacement(std::atoi(value));
    }
    if (const char *value = std::getenv("RG_ULTRAWIDE_FOV"))
    {
        setFovCap(std::atoi(value));
    }
    if (const char *value = std::getenv("RG_UW_TASKS"))
    {
        parseClasses(value);
    }
    if (const char *value = std::getenv("RG_UW_MODE"))
    {
        g_debugMode = std::atoi(value);
    }
    if (const char *value = std::getenv("RG_UW_TRACE"))
    {
        g_trace = *value && *value != '0';
    }
    // Bisecting: RG_UW_HOOKS is a mask, 1 the task tables, 2 the writers.
    const char *mask = std::getenv("RG_UW_HOOKS");
    const int hooks = mask ? std::atoi(mask) : 3;
    bool complete = true;
    if (hooks & 1)
    {
        complete = rg::timing::hookFunction(runtime, kTaskTableRun, &taskTableRun, g_taskTableRun) &&
                   rg::timing::hookFunction(runtime, kTaskTestFlag, &taskTestFlag, g_taskTestFlag);
    }
    if (hooks & 8)
    {
        complete = complete && rg::timing::hookFunction(runtime, kTaskTableRun, &taskTableEntry, g_taskTableRun) &&
                   rg::timing::hookFunction(runtime, kTaskTestFlag, &taskTestFlag, g_taskTestFlag);
    }
    if (hooks & 4)
    {
        complete = complete && rg::timing::hookFunction(runtime, kTaskTestFlag, &taskTestFlag, g_taskTestFlag);
    }
    if (hooks & 2)
    {
        complete = complete && rg::timing::hookFunction(runtime, kGsPktSetXyzPixel, &setXyzPixel, g_setXyzPixel) &&
                   rg::timing::hookFunction(runtime, kGsPktSetXyz, &setXyz, g_setXyz) &&
                   rg::timing::hookFunction(runtime, kGsPktSetXyzf, &setXyzf, g_setXyzf) &&
                   rg::timing::hookFunction(runtime, kGsPktSetXyzVecI, &setXyzVecI, g_setXyzVecI) &&
                   rg::timing::hookFunction(runtime, kGsPktSetXyzPixelVec, &setXyzPixelVec, g_setXyzPixelVec) &&
                   rg::timing::hookFunction(runtime, kGsPktSetXyzfVec, &setXyzfVec, g_setXyzfVec) &&
                   rg::timing::hookFunction(runtime, kGsPktBeginPrimAd, &beginPrimAd, g_beginPrimAd) &&
                   rg::timing::hookFunction(runtime, kGsPktClose, &close, g_close) &&
                   rg::timing::hookFunction(runtime, kDrawListEndPrim, &drawListEndPrim, g_drawListEndPrim) &&
                   rg::timing::hookFunction(runtime, kGsPktWriteAd, &writeAd, g_writeAd) &&
                   rg::timing::hookFunction(runtime, kViewSetScreenMatrix, &viewSetScreenMatrix,
                                            g_viewSetScreenMatrix) &&
                   installScoped(runtime, std::make_index_sequence<std::size(kPauseFunctions)>{}) &&
                   installWorld(runtime, std::make_index_sequence<std::size(kWorldFunctions)>{});
    }
    if (!complete)
    {
        std::fprintf(stderr, "[rg] ultrawide: some hooks are missing, see above\n");
    }
}

void setHudArea(int area)
{
    g_area.store(std::clamp(area, 0, 2), std::memory_order_relaxed);
}

void setHudPlacement(int placement)
{
    g_placement.store(std::clamp(placement, 0, 1), std::memory_order_relaxed);
}

void setFovCap(int cap)
{
    g_fovCap.store(std::clamp(cap, 0, 2), std::memory_order_relaxed);
}

bool fovCapped()
{
    return g_fovCap.load(std::memory_order_relaxed) != 0;
}

float cappedVerticalFov(float vertical)
{
    const int cap = g_fovCap.load(std::memory_order_relaxed);
    const float window = rg::widescreen::aspect();
    const float shape = cap == 1 ? kTwentyOneNine : kSixteenNine;
    if (cap == 0 || window <= shape + 0.005f)
    {
        return vertical;
    }
    return 2.0f * std::atan(std::tan(vertical * 0.5f) * shape / window);
}

} // namespace rg::ultrawide
