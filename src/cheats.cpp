#include "cheats.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "ps2_runtime.h"
#include "scene_timing.h"

namespace rg::cheats
{
namespace
{

constexpr uint32_t kGameFrameUpdate = 0x0027C4C0u;
constexpr uint32_t kCharaSetHp = 0x003B8040u;
// The save screen's state machine (McSaveStep, a step a frame until it returns
// 1), whose result word (+0x4F0 of its manager) the screen reads: 8 is the one
// the screen answers "Save to file N failed." to, and the card is not touched
// (0xC, which the step writes when it has no game data, makes it say "File N
// saved.").
constexpr uint32_t kMcSaveStep = 0x00374110u;
constexpr uint32_t kSaveResult = 0x4F0u;
constexpr uint16_t kSaveFailed = 8u;
constexpr uint32_t kItemConsume = 0x003D41B0u;
// ItemUseMaybe's two calls of ItemConsume (jal at 0x2EF078 and 0x2EF538; the return addresses).
constexpr uint32_t kItemUseReturnA = 0x002EF080u;
constexpr uint32_t kItemUseReturnB = 0x002EF540u;

constexpr uint32_t kGameData = 0x004B8B30u;
constexpr uint32_t kCharaManager = 0x14u;
constexpr uint32_t kPartyField = 0x18u;
constexpr uint32_t kUnitManager = 0x1Cu;
// Both managers hold a pool of fixed-size records; the characters' manager
// has it at +0x8, the units' at +0x10.
constexpr uint32_t kCharaPoolAt = 0x8u;
constexpr uint32_t kUnitPoolAt = 0x10u;
constexpr uint32_t kPoolBase = 0x8u;
constexpr uint32_t kPoolStride = 0x10u;
constexpr uint32_t kPoolCapacity = 0x14u;
constexpr uint32_t kPoolUsedBits = 0x20u;

constexpr uint32_t kId = 0xCu;
constexpr uint32_t kLevel = 0x10u;
constexpr uint32_t kHp = 0x14u;
constexpr uint32_t kAp = 0x18u;
constexpr uint32_t kMaxHp = 0x20u;
constexpr uint32_t kMaxAp = 0x26u;
constexpr uint32_t kActionGauge = 0x1C4u;
constexpr uint32_t kExperience = 0x4B8u;
constexpr uint32_t kMoney = 0x160u;
constexpr uint32_t kMoneyCap = 0x004659E0u;
constexpr uint32_t kGaugeMax = 0x00465CD0u;
constexpr uint32_t kPartyCount = 0x18Cu;
constexpr uint32_t kPartyIds = 0x190u;
constexpr uint32_t kPartyGlobal = 0x00483820u;

// The memory card's writing calls.
constexpr uint32_t kMcOpen = 0x0012CA58u;
constexpr uint32_t kMcMkdir = 0x0012CB80u;
constexpr uint32_t kMcWrite = 0x0012CF20u;
constexpr uint32_t kMcFormat = 0x0012D6C8u;
constexpr uint32_t kMcDelete = 0x0012D798u;
constexpr uint32_t kMcSetFileInfo = 0x0012D978u;
constexpr uint32_t kMcRename = 0x0012DB48u;
constexpr uint32_t kMcUnformat = 0x0012DCA0u;
// PS2 file flags that write: O_WRONLY (2, also in O_RDWR), O_CREAT, O_TRUNC.
constexpr uint32_t kWriteFlags = 0x2u | 0x200u | 0x400u;

using Fn = PS2Runtime::RecompiledFunction;
Fn g_mcSaveStep = nullptr;
Fn g_frame = nullptr, g_setHp = nullptr, g_itemConsume = nullptr;
Fn g_mcOpen = nullptr, g_mcMkdir = nullptr, g_mcWrite = nullptr, g_mcFormat = nullptr, g_mcDelete = nullptr,
   g_mcSetFileInfo = nullptr, g_mcRename = nullptr, g_mcUnformat = nullptr;

std::atomic<bool> g_master{false};
std::atomic<bool> g_hp{false}, g_ap{false}, g_gauge{false}, g_items{false}, g_onehit{false},
    g_moneyHold{false};
std::atomic<int> g_moneyAmount{99999999};
std::atomic<bool> g_fireMaxMoney{false};
// Test aid (RG_CHEATS_CMD_FILE "hurt"): every character to half HP.
std::atomic<bool> g_hurt{false};
std::atomic<unsigned> g_refused{0};
bool g_log = false;

// The characters' pool, as the frame saw it (written by the game thread,
// read by the hooks that run on it too: the damage and item hooks are the
// game thread's, the memory card's are not and do not read it).
struct Pool
{
    uint32_t base = 0, stride = 0, capacity = 0, bits = 0;
    bool valid() const { return base != 0u && stride != 0u && capacity != 0u && bits != 0u; }
};
uint32_t g_charaBase = 0, g_charaEnd = 0;

template <typename T>
bool rd(const uint8_t *rdram, uint32_t address, T &out)
{
    address &= 0x1FFFFFFFu;
    if (address == 0u || address + sizeof(T) > PS2_RAM_SIZE)
        return false;
    std::memcpy(&out, rdram + address, sizeof(T));
    return true;
}

template <typename T>
bool wr(uint8_t *rdram, uint32_t address, const T &value)
{
    address &= 0x1FFFFFFFu;
    if (address == 0u || address + sizeof(T) > PS2_RAM_SIZE)
        return false;
    std::memcpy(rdram + address, &value, sizeof(T));
    return true;
}

uint32_t rd32(const uint8_t *rdram, uint32_t address)
{
    uint32_t v = 0;
    rd(rdram, address, v);
    return v;
}

bool poolAt(const uint8_t *rdram, uint32_t managerField, uint32_t poolField, Pool &pool)
{
    const uint32_t manager = rd32(rdram, kGameData + managerField);
    const uint32_t at = manager ? rd32(rdram, manager + poolField) : 0u;
    if (!at)
        return false;
    pool.base = rd32(rdram, at + kPoolBase);
    pool.stride = rd32(rdram, at + kPoolStride);
    pool.capacity = rd32(rdram, at + kPoolCapacity);
    pool.bits = rd32(rdram, at + kPoolUsedBits);
    return pool.valid() && pool.capacity <= 512u && pool.stride >= 0x200u && pool.stride <= 0x2000u;
}

template <typename F>
void forEachRecord(const uint8_t *rdram, const Pool &pool, F &&visit)
{
    for (uint32_t i = 0; i < pool.capacity; ++i)
    {
        uint32_t word = 0;
        if (!rd(rdram, pool.bits + (i / 32u) * 4u, word) || !((word >> (i % 32u)) & 1u))
            continue;
        visit(pool.base + i * pool.stride);
    }
}

bool isCharacter(uint32_t obj)
{
    return g_charaBase != 0u && obj >= g_charaBase && obj < g_charaEnd;
}

bool anyCheatOn()
{
    return g_hp.load() || g_ap.load() || g_gauge.load() || g_items.load() || g_onehit.load() ||
           g_moneyHold.load();
}

void frameWork(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    Pool chara;
    if (!poolAt(rdram, kCharaManager, kCharaPoolAt, chara))
    {
        g_charaBase = g_charaEnd = 0u;
        return;
    }
    g_charaBase = chara.base;
    g_charaEnd = chara.base + chara.capacity * chara.stride;
    const uint32_t party = rd32(rdram, kGameData + kPartyField);

    // The writes below are the master switch's; with RG_CHEATS_LOG the frame
    // work also runs for its log lines alone.
    const bool master = g_master.load(std::memory_order_relaxed);
    const bool hp = g_hp.load(), ap = g_ap.load(), gauge = g_gauge.load();
    if (master && (hp || ap || gauge))
    {
        int32_t gaugeMax = 1000;
        rd(rdram, kGaugeMax, gaugeMax);
        forEachRecord(rdram, chara, [&](uint32_t obj) {
            if (hp)
            {
                int32_t now = 0, max = 0;
                rd(rdram, obj + kHp, now);
                rd(rdram, obj + kMaxHp, max);
                // A character who is down stays down: the game revives with its
                // own items.
                if (now > 0 && now < max)
                    wr(rdram, obj + kHp, max);
            }
            if (ap)
            {
                uint16_t max = 0;
                rd(rdram, obj + kMaxAp, max);
                wr(rdram, obj + kAp, max);
            }
            if (gauge)
                wr(rdram, obj + kActionGauge, static_cast<float>(gaugeMax));
        });
    }

    if (party && master)
    {
        int64_t money = 0;
        int32_t cap = 99999999;
        rd(rdram, party + kMoney, money);
        rd(rdram, kMoneyCap, cap);
        if (g_fireMaxMoney.exchange(false))
        {
            wr(rdram, party + kMoney, static_cast<int64_t>(cap));
            std::fprintf(stderr, "[cheats] money %lld -> %d\n", static_cast<long long>(money), cap);
        }
        else if (g_moneyHold.load())
        {
            const int64_t amount = std::clamp<int64_t>(g_moneyAmount.load(), 0, cap);
            if (money != amount)
                wr(rdram, party + kMoney, amount);
        }
    }
    if (g_hurt.exchange(false))
        forEachRecord(rdram, chara, [&](uint32_t obj) {
            int32_t max = 0;
            rd(rdram, obj + kMaxHp, max);
            wr(rdram, obj + kHp, max / 2);
        });

    if (g_log)
    {
        static unsigned frame = 0;
        if (++frame % 90u == 0u)
        {
            int64_t money = 0;
            rd(rdram, party + kMoney, money);
            std::fprintf(stderr, "[cheats] money %lld\n", static_cast<long long>(money));
            forEachRecord(rdram, chara, [&](uint32_t obj) {
                int32_t id = 0, hpNow = 0, hpMax = 0, exp = 0;
                uint16_t level = 0, apNow = 0, apMax = 0;
                float gaugeNow = 0.0f;
                rd(rdram, obj + kId, id);
                rd(rdram, obj + kLevel, level);
                rd(rdram, obj + kHp, hpNow);
                rd(rdram, obj + kMaxHp, hpMax);
                rd(rdram, obj + kAp, apNow);
                rd(rdram, obj + kMaxAp, apMax);
                rd(rdram, obj + kActionGauge, gaugeNow);
                rd(rdram, obj + kExperience, exp);
                std::fprintf(stderr, "[cheats]  chara %d lv %u hp %d/%d ap %u/%u gauge %.0f exp %d\n", id, level,
                             hpNow, hpMax, apNow, apMax, gaugeNow, exp);
            });
            Pool units;
            if (poolAt(rdram, kUnitManager, kUnitPoolAt, units))
                forEachRecord(rdram, units, [&](uint32_t obj) {
                    int32_t id = 0, hpNow = 0, hpMax = 0;
                    rd(rdram, obj + kId, id);
                    rd(rdram, obj + kHp, hpNow);
                    rd(rdram, obj + kMaxHp, hpMax);
                    std::fprintf(stderr, "[cheats]  unit %d hp %d/%d\n", id, hpNow, hpMax);
                });
        }
    }
}

// Test aid: RG_CHEATS_CMD_FILE names a file whose lines are run as they come
// ("master 1", "switch hp 1", "number money 5000", "fire max_money"), read
// twice a second and emptied.
void pollCommands()
{
    static const char *path = std::getenv("RG_CHEATS_CMD_FILE");
    static unsigned frame = 0;
    if (!path || !*path || ++frame % 15u != 0u)
        return;
    std::string text;
    {
        std::ifstream in(path);
        if (!in)
            return;
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    if (text.empty())
        return;
    std::ofstream(path, std::ios::trunc).close();
    size_t at = 0;
    while (at < text.size())
    {
        size_t end = text.find('\n', at);
        if (end == std::string::npos)
            end = text.size();
        char verb[16] = {}, name[32] = {};
        int value = 0;
        const std::string line = text.substr(at, end - at);
        at = end + 1u;
        const int count = std::sscanf(line.c_str(), "%15s %31s %d", verb, name, &value);
        if (count >= 2 && std::string(verb) == "switch")
            setSwitch(name, value != 0);
        else if (count >= 2 && std::string(verb) == "number")
            setNumber(name, value);
        else if (count >= 2 && std::string(verb) == "fire")
            fire(name);
        else if (count >= 2 && std::string(verb) == "master")
            setMaster(std::atoi(name) != 0);
        else if (std::string(verb) == "hurt")
            g_hurt.store(true);
    }
}

void gameFrameUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    pollCommands();
    if (g_master.load(std::memory_order_relaxed) || g_log)
        frameWork(rdram, ctx, runtime);
    g_frame(rdram, ctx, runtime);
}

// Damage lands here: CharaSetHp(unit, hp). With the party protected, a
// character's HP does not go down; with one-hit kills, anything that is not
// a character goes to 0 when it would go down at all.
void charaSetHp(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (g_master.load(std::memory_order_relaxed))
    {
        const uint32_t obj = getRegU32(ctx, 4);
        int32_t now = 0;
        const int32_t wanted = static_cast<int32_t>(getRegU32(ctx, 5));
        if (rd(rdram, obj + kHp, now) && wanted < now)
        {
            if (isCharacter(obj))
            {
                if (g_hp.load(std::memory_order_relaxed) && now > 0)
                {
                    ctx->r[5] = _mm_set_epi64x(0, static_cast<int64_t>(now));
                    if (g_log)
                        std::fprintf(stderr, "[cheats] damage refused: character %08X hp %d would be %d\n", obj, now,
                                     wanted);
                }
            }
            else if (g_onehit.load(std::memory_order_relaxed))
            {
                ctx->r[5] = _mm_set_epi64x(0, 0);
            }
        }
    }
    g_setHp(rdram, ctx, runtime);
}

// An item used in the field or in battle is not taken from the bag. Selling,
// discarding, combining and the Revelation Flow consume through other calls
// and still do.
void itemConsume(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t returnPc = getRegU32(ctx, 31);
    if (g_master.load(std::memory_order_relaxed) && g_items.load(std::memory_order_relaxed) &&
        (returnPc == kItemUseReturnA || returnPc == kItemUseReturnB))
    {
        if (g_log)
            std::fprintf(stderr, "[cheats] item use: not consumed (%d of %08X)\n", static_cast<int>(getRegU32(ctx, 5)),
                         getRegU32(ctx, 4));
        setReturnS32(ctx, 1);
        ctx->pc = returnPc;
        return;
    }
    g_itemConsume(rdram, ctx, runtime);
}

// The memory card: while the master switch is on, writing commands are sent
// to a port that does not exist, which the runtime's card answers with an
// error, as a card pulled out would (the game's own save screen then says it
// could not save).
bool refuseWrite(R5900Context *ctx, int argument = 4)
{
    if (!g_master.load(std::memory_order_relaxed))
        return false;
    ctx->r[argument] = _mm_set_epi64x(0, 0x7FFFF);
    if (g_refused.fetch_add(1u, std::memory_order_relaxed) < 8u)
        std::fprintf(stderr, "[cheats] a memory card write was refused (cheats are on)\n");
    return true;
}

// The game's save step ends at once with the failure it reports when it
// cannot save, before any card call: the screen then says so itself.
void mcSaveStep(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (g_master.load(std::memory_order_relaxed))
    {
        const uint32_t manager = getRegU32(ctx, 4);
        wr(rdram, manager + kSaveResult, kSaveFailed);
        if (g_refused.fetch_add(1u, std::memory_order_relaxed) < 8u)
            std::fprintf(stderr, "[cheats] the save was refused (cheats are on)\n");
        setReturnS32(ctx, 1);
        ctx->pc = getRegU32(ctx, 31);
        return;
    }
    g_mcSaveStep(rdram, ctx, runtime);
}

void mcOpen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if ((getRegU32(ctx, 7) & kWriteFlags) != 0u)
        refuseWrite(ctx);
    g_mcOpen(rdram, ctx, runtime);
}
void mcMkdir(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    refuseWrite(ctx);
    g_mcMkdir(rdram, ctx, runtime);
}
void mcWrite(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    // The file handle is the first argument; a bad one finds no file.
    refuseWrite(ctx);
    g_mcWrite(rdram, ctx, runtime);
}
void mcFormat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    refuseWrite(ctx);
    g_mcFormat(rdram, ctx, runtime);
}
void mcDelete(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    refuseWrite(ctx);
    g_mcDelete(rdram, ctx, runtime);
}
void mcSetFileInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    refuseWrite(ctx);
    g_mcSetFileInfo(rdram, ctx, runtime);
}
void mcRename(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    refuseWrite(ctx);
    g_mcRename(rdram, ctx, runtime);
}
void mcUnformat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    refuseWrite(ctx);
    g_mcUnformat(rdram, ctx, runtime);
}

bool flagEnv(const char *name)
{
    const char *value = std::getenv(name);
    return value && *value && *value != '0';
}

} // namespace

void install(PS2Runtime &runtime)
{
    g_log = flagEnv("RG_CHEATS_LOG");
    // RG_CHEATS=hp,ap,gauge,items,onehit,money_hold: the master on
    // and these cheats for this run (tests); RG_CHEATS_MONEY=N sets the
    // amount held, RG_CHEATS_FIRE=max_money runs the action once the game is
    // up.
    if (const char *list = std::getenv("RG_CHEATS"); list && *list)
    {
        setMaster(true);
        std::string text = list;
        for (size_t at = 0; at < text.size();)
        {
            const size_t end = std::min(text.find(',', at), text.size());
            setSwitch(text.substr(at, end - at), true);
            at = end + 1u;
        }
    }
    if (const char *money = std::getenv("RG_CHEATS_MONEY"))
        setNumber("money", std::atoi(money));
    if (const char *fireList = std::getenv("RG_CHEATS_FIRE"); fireList && *fireList)
    {
        std::string text = fireList;
        for (size_t at = 0; at < text.size();)
        {
            const size_t end = std::min(text.find(',', at), text.size());
            fire(text.substr(at, end - at));
            at = end + 1u;
        }
    }
    bool ok = true;
    ok &= rg::timing::hookFunction(runtime, kGameFrameUpdate, &gameFrameUpdate, g_frame);
    ok &= rg::timing::hookFunction(runtime, kCharaSetHp, &charaSetHp, g_setHp);
    ok &= rg::timing::hookFunction(runtime, kItemConsume, &itemConsume, g_itemConsume);
    ok &= rg::timing::hookFunction(runtime, kMcSaveStep, &mcSaveStep, g_mcSaveStep);
    ok &= rg::timing::hookFunction(runtime, kMcOpen, &mcOpen, g_mcOpen);
    ok &= rg::timing::hookFunction(runtime, kMcMkdir, &mcMkdir, g_mcMkdir);
    ok &= rg::timing::hookFunction(runtime, kMcWrite, &mcWrite, g_mcWrite);
    ok &= rg::timing::hookFunction(runtime, kMcFormat, &mcFormat, g_mcFormat);
    ok &= rg::timing::hookFunction(runtime, kMcDelete, &mcDelete, g_mcDelete);
    ok &= rg::timing::hookFunction(runtime, kMcSetFileInfo, &mcSetFileInfo, g_mcSetFileInfo);
    ok &= rg::timing::hookFunction(runtime, kMcRename, &mcRename, g_mcRename);
    ok &= rg::timing::hookFunction(runtime, kMcUnformat, &mcUnformat, g_mcUnformat);
    if (!ok)
        std::fprintf(stderr, "[rg] cheats: some hooks are missing, see above; the cheats stay off\n");
    (void)kPartyGlobal;
}

void setMaster(bool on)
{
    if (g_master.exchange(on) != on)
        std::fprintf(stderr, "[cheats] master %s\n", on ? "on" : "off");
}

bool master()
{
    return g_master.load(std::memory_order_relaxed);
}

bool active()
{
    return master() && anyCheatOn();
}

bool blockingSaves()
{
    return master();
}

unsigned refusedSaves()
{
    return g_refused.load(std::memory_order_relaxed);
}

void setSwitch(const std::string &name, bool on)
{
    if (name == "hp")
        g_hp.store(on);
    else if (name == "ap")
        g_ap.store(on);
    else if (name == "gauge")
        g_gauge.store(on);
    else if (name == "items")
        g_items.store(on);
    else if (name == "onehit")
        g_onehit.store(on);
    else if (name == "money_hold")
        g_moneyHold.store(on);
}

void setNumber(const std::string &name, int value)
{
    if (name == "money")
        g_moneyAmount.store(std::clamp(value, 0, 99999999));
}

void fire(const std::string &name)
{
    if (name == "max_money")
        g_fireMaxMoney.store(true);
}

} // namespace rg::cheats
