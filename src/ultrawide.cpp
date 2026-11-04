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
#include <tuple>

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

std::atomic<int> g_area{0};
std::atomic<int> g_placement{0};
std::atomic<int> g_fovCap{0};

// The draw function of the task being run by a task table, 0 outside one. The
// game's thread alone writes it.
uint32_t g_task = 0u;
bool g_trace = false;
int g_debugMode = 0; // RG_UW_MODE: 1 no sprite pairs, 2 modify the arguments and call the original

// The tasks that are screen-anchored HUD. Everything else is left as it is.
std::map<uint32_t, Cls> g_classes = {
    {0x002058B0u, Cls::Hud},  // TaskDrawStatus
    {0x0027A8B0u, Cls::Text}, // TaskDrawMessage
    {0x001FFB00u, Cls::Text}, // TaskDrawCaption
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

void taskTableDone(uint8_t *, R5900Context *, PS2Runtime *, uint32_t outer)
{
    g_task = outer;
}

void taskTableRun(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    rg::hooks::callThen(g_taskTableRun, rdram, ctx, runtime, &taskTableDone, g_task);
}

// A table run starts outside any task. (Not wrapped: the run goes on at an
// indirect call, and a wrapper that waits for its end stalled the game.)
void taskTableEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
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
    if (g_task == 0u || !active() || originOf(rdram, packet) != kPictureOrigin)
    {
        return Cls::None;
    }
    return classOf(g_task);
}

// The sprite being written: its first corner's x, and where that corner's
// quadword is, to be moved once the second is known.
uint32_t g_corner = 0u;
uint32_t g_cornerPacket = 0u;
float g_cornerX = 0.0f;
uint32_t g_vertexIndex = 0u;

// Writes one vertex with its x mapped, in the packet at `packet`; the
// writers' own work (an A+D quadword with the XYZ2 register), and the
// sprite's first corner moved when the second arrives.
void emit(uint8_t *rdram, uint32_t packet, float x, int32_t y16, int32_t z, Cls cls)
{
    const uint32_t pointer = read<uint32_t>(rdram, packet + kPacketWrite);
    const int32_t originX = read<int32_t>(rdram, packet + kPacketOriginX);
    const int32_t originY = read<int32_t>(rdram, packet + kPacketOriginY);
    const uint32_t prim = read<uint8_t>(rdram, packet + kPacketPrim) & 7u;
    float mapped;
    if (prim == kPrimSprite && g_debugMode != 1)
    {
        if ((g_vertexIndex++ & 1u) == 0u)
        {
            g_corner = pointer;
            g_cornerPacket = packet;
            g_cornerX = x;
            mapped = mapPoint(x, cls);
        }
        else if (g_cornerPacket == packet)
        {
            float first, second;
            mapSpan(g_cornerX, x, cls, first, second);
            const uint64_t data = read<uint64_t>(rdram, g_corner);
            const uint32_t x16 = static_cast<uint32_t>(std::lround(first * 16.0f) + originX) & 0xFFFFu;
            write<uint64_t>(rdram, g_corner, (data & ~0xFFFFull) | x16);
            mapped = second;
        }
        else
        {
            mapped = mapPoint(x, cls);
        }
    }
    else
    {
        mapped = mapPoint(x, cls);
    }
    const int32_t x16 = static_cast<int32_t>(std::lround(mapped * 16.0f)) + originX;
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
    g_vertexIndex = 0u;
    g_beginPrimAd(rdram, ctx, runtime);
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
                 static_cast<int32_t>(getRegU32(ctx, 7)), cls);
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
                 static_cast<int32_t>(ctx->f[14]), cls);
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
                 static_cast<int32_t>(getRegU32(ctx, 7)), cls);
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
                 cls);
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
                 read<int32_t>(rdram, vector + 8u), cls);
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
                 static_cast<int32_t>(read<float>(rdram, vector + 8u)), cls);
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
    if (g_task != 0u && getRegU32(ctx, 5) == 0x40u)
    {
        uint64_t value = static_cast<uint64_t>(_mm_cvtsi128_si64(ctx->r[6]));
        const int32_t x0 = static_cast<int32_t>(value & 0xFFFFu);
        const int32_t x1 = static_cast<int32_t>((value >> 16) & 0xFFFFu);
        if (g_trace)
        {
            note(6, getRegU32(ctx, 31), static_cast<float>(x0 * 1000 + x1));
        }
        const Cls cls = current(rdram, getRegU32(ctx, 4));
        if (cls != Cls::None && !(x0 <= 0 && x1 >= 511))
        {
            float first, second;
            mapSpan(static_cast<float>(x0), static_cast<float>(x1 + 1), cls, first, second);
            const int32_t left = static_cast<int32_t>(std::floor(first + 0.001f));
            const int32_t right = static_cast<int32_t>(std::ceil(second - 0.001f)) - 1;
            value = (value & ~0xFFFFFFFFull) | static_cast<uint64_t>(left & 0xFFFF) |
                    (static_cast<uint64_t>(right & 0xFFFF) << 16);
            ctx->r[6] = _mm_set_epi64x(0, static_cast<int64_t>(value));
        }
    }
    g_writeAd(rdram, ctx, runtime);
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
    const int hooks = mask ? std::atoi(mask) : 10;
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
                   rg::timing::hookFunction(runtime, kGsPktWriteAd, &writeAd, g_writeAd);
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
