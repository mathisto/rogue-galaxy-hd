#include "hd_text.h"

#include "hd_art.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "gfx/backends/sdlgpu/sdlgpu_replacements.h"
#include "ps2_runtime.h"
#include "settings_pages.h"

// Roboto Medium (Apache 2.0), which dq8-gfx builds in for its menu.
extern const unsigned char kUiFontData[];
extern const unsigned int kUiFontData_size;

namespace rg::hdtext
{
namespace
{

using dq8::gfx::GsReplacementImage;
using dq8::gfx::GsReplacementKey;
using dq8::gfx::GsReplacementStore;
using hdart::FontType;

// D_0044E850: [character, width as a name, as normal text, as system text]
// for each of the 106 glyphs (research 08).
constexpr uint32_t kGlyphTable = 0x0044E850u;

constexpr uint32_t kPsmT4 = 0x14u;
constexpr uint32_t kPsmT8 = 0x13u;

// The pictures, as the texture cache names them: the hash of their texels
// (the font pages' first 512x416, which the game uploads into a 512x512
// TEX0), any palette. Measured with RG_TEXTURE_LOG=1 on SCUS-97490: the
// main game's pages (fontset_main_en.pak) and the launcher's
// (fontset_launcher_en.pak) where they differ.
// The normal page is one of the "fixed textures" MesFontLoad keeps in the
// high nibbles of a 32-bit buffer (T4HL/T4HH at 0x1C00); the system and name
// pages are plain T4 uploaded to 0x2A00 when drawn.
constexpr uint32_t kPsmT4HL = 0x24u;
constexpr uint32_t kPsmT4HH = 0x2Cu;
struct Page
{
    uint64_t texels;
    uint32_t psm;
    FontType type;
    uint32_t rows = 416u; // the part named: 512 x rows
};
constexpr Page kFontPages[] = {
    {0x2bad8e99ac5c8351ull, kPsmT4HL, FontType::Normal}, // FontTex_normal_EN_0, T4HL at 0x1C00
    {0x3e781ae048c61d81ull, kPsmT4, FontType::System},   // FontTex_system_EN_0, T4 at 0x2A00
    // FontTex_name_EN_0 follows the system page in memory (0x2BA0); the TEX0
    // that draws it reaches past it into memory a render target holds, so
    // only its first 72 rows, the four rows of cells ASCII needs, are named.
    {0x785b309d00337092ull, kPsmT4, FontType::Name, 72u},
};
// RG_TEXTURE_LOG=1 also asks for the hashes of every 512x416 4-bit part,
// for finding pages not listed above.
constexpr uint32_t kProbePsms[] = {kPsmT4, kPsmT4HL, kPsmT4HH};
constexpr uint64_t kGaijiTexels = 0xef8062d131c0ea57ull; // gaiji2, 256x256 T8, both packs
// The sheets whose digit rows UiDrawSpritePixelMaybe (0x292300) draws numbers
// from (research 11), by their texels as RG_TEXTURE_LOG names them, any
// palette; each row's cells, measured on the exported sheets. A second key is
// the copy with Japanese labels (menu_static_img2_en.pac), same layout.
struct DigitSheet
{
    uint64_t texels[2];
    uint32_t psm;
    uint32_t width, height;
    const hdart::DigitRow *rows;
    size_t rowCount;
};
// topmenu_02a: play time, item counts, levels, stats (grey, blue, small).
constexpr hdart::DigitRow kTopMenuRows[] = {
    {0, 0, 16, 12, "/:"},
    {0, 16, 16, 12, "/:"},
    {0, 32, 14, 10, "/"},
};
// Itm_05: the money in the item menu and shops, italic.
constexpr hdart::DigitRow kItemMoneyRows[] = {{0, 105, 22, 14, "", 0.25f}};
// TP_save02: the file numbers of the save and load screens.
constexpr hdart::DigitRow kSaveFileRows[] = {{0, 0, 18, 14, ""}};
// TP_top_02a: the levels on the save and load screens.
constexpr hdart::DigitRow kSaveLevelRows[] = {{0, 0, 14, 12, ""}};
constexpr DigitSheet kDigitSheets[] = {
    {{0x5fd0e1dbd5ef4f1cull, 0x1f1ea76d537d92ceull}, kPsmT4, 256u, 128u, kTopMenuRows, std::size(kTopMenuRows)},
    {{0x394b14f4a10e95a0ull, 0x6a9f43a50179bff6ull}, kPsmT8, 256u, 128u, kItemMoneyRows, std::size(kItemMoneyRows)},
    {{0x4919d4a889d9698dull, 0u}, kPsmT8, 256u, 128u, kSaveFileRows, std::size(kSaveFileRows)},
    {{0x68940e5ee5d6687bull, 0u}, kPsmT8, 256u, 64u, kSaveLevelRows, std::size(kSaveLevelRows)},
};

std::mutex g_mutex;
uint8_t *g_rdram = nullptr;
hdart::GlyphTable g_original;
bool g_haveTable = false;
std::shared_ptr<const hdart::Font> g_font;   // the text's, when modern
std::shared_ptr<const hdart::Font> g_labels; // Roboto, for the icons' letters
std::string g_fontDescription = "Roboto Medium (built in)";
int g_prompts = static_cast<int>(Prompts::Auto);
bool g_modern = false;
bool g_playStationPad = false;
bool g_installed = false;
// What is registered with the store now.
bool g_xboxShown = false;
bool g_fontShown = false;

std::vector<uint8_t> readFile(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
}

std::vector<uint8_t> builtIn()
{
    return std::vector<uint8_t>(kUiFontData, kUiFontData + kUiFontData_size);
}

// RG_FONT, else the first font in fonts\, else Roboto Medium.
std::shared_ptr<const hdart::Font> loadTextFont()
{
    std::vector<std::filesystem::path> candidates;
    if (const char *path = std::getenv("RG_FONT"); path && *path)
    {
        candidates.emplace_back(path);
    }
    std::error_code error;
    std::vector<std::filesystem::path> folder;
    for (const auto &entry : std::filesystem::directory_iterator("fonts", error))
    {
        std::string extension = entry.path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (entry.is_regular_file(error) && (extension == ".ttf" || extension == ".otf"))
        {
            folder.push_back(entry.path());
        }
    }
    std::sort(folder.begin(), folder.end());
    candidates.insert(candidates.end(), folder.begin(), folder.end());
    for (const std::filesystem::path &path : candidates)
    {
        std::string why;
        std::vector<uint8_t> data = readFile(path);
        if (data.empty())
        {
            std::fprintf(stderr, "[font] %s: cannot be read\n", path.string().c_str());
            continue;
        }
        if (auto font = hdart::Font::load(std::move(data), path.filename().string(), g_original, why))
        {
            g_fontDescription = path.string();
            std::printf("[font] text font: %s\n", path.string().c_str());
            return font;
        }
        std::fprintf(stderr, "[font] %s\n", why.c_str());
    }
    std::string why;
    auto font = hdart::Font::load(builtIn(), "Roboto Medium", g_original, why);
    if (!font)
    {
        std::fprintf(stderr, "[font] built-in font: %s\n", why.c_str());
    }
    g_fontDescription = "Roboto Medium (built in)";
    return font;
}

void writeWidths(const hdart::GlyphTable &table)
{
    if (!g_rdram)
    {
        return;
    }
    for (int glyph = 0; glyph < hdart::kGlyphs; ++glyph)
    {
        // Only the types whose page is redrawn: the others keep the game's
        // glyphs, which the game's widths fit.
        for (const Page &page : kFontPages)
        {
            const int type = static_cast<int>(page.type);
            g_rdram[kGlyphTable + glyph * 4u + 1u + type] = table.width[type][glyph];
        }
    }
}

// Registers or withdraws the pictures to match the settings (g_mutex held).
void apply()
{
    if (!g_installed)
    {
        return;
    }
    GsReplacementStore &store = GsReplacementStore::instance();

    const bool xbox = static_cast<Prompts>(g_prompts) == Prompts::Xbox ||
                      (static_cast<Prompts>(g_prompts) == Prompts::Auto && !g_playStationPad);
    if (xbox && g_labels && !g_xboxShown)
    {
        std::shared_ptr<const hdart::Font> labels = g_labels;
        store.setGenerated("xbox-prompts", {GsReplacementKey{kGaijiTexels, 0u, kPsmT8, 256u, 256u}},
                           [labels](uint32_t scale, const uint8_t *native, uint32_t width, uint32_t height,
                                    GsReplacementImage &out) {
                               out.width = width * scale;
                               out.height = height * scale;
                               return hdart::drawXboxIcons(*labels, scale, native, width, height, out.rgba);
                           });
        g_xboxShown = true;
        std::printf("[font] button prompts: Xbox\n");
    }
    else if (!xbox && g_xboxShown)
    {
        store.removeGenerated("xbox-prompts");
        g_xboxShown = false;
        std::printf("[font] button prompts: PlayStation\n");
    }

    // The widths follow the pictures: with replacements off, the game's.
    const bool modern = g_modern && store.enabled();
    if (modern && !g_font && g_haveTable)
    {
        g_font = loadTextFont();
    }
    if (modern && g_font && !g_fontShown)
    {
        // One picture a page, each knowing its grid.
        for (size_t index = 0; index < std::size(kFontPages); ++index)
        {
            const Page page = kFontPages[index];
            std::shared_ptr<const hdart::Font> font = g_font;
            store.setGenerated("font-" + std::to_string(index), {GsReplacementKey{page.texels, 0u, page.psm, 512u, page.rows}},
                               [font, page](uint32_t scale, const uint8_t *native, uint32_t width, uint32_t height,
                                            GsReplacementImage &out) {
                                   out.width = width * scale;
                                   out.height = height * scale;
                                   return font->drawPage(page.type, scale, native, width, height, out.rgba);
                               });
        }
        writeWidths(g_font->widths());
        // The menus' numerals (play time, money, counts, levels, stats) are
        // digit strips in menu sheets, not text: their digit rows redrawn in
        // the same font, over a pack's upscale of the sheet when there is one.
        std::shared_ptr<const hdart::Font> font = g_font;
        for (size_t index = 0; index < std::size(kDigitSheets); ++index)
        {
            const DigitSheet &sheet = kDigitSheets[index];
            std::vector<GsReplacementKey> keys;
            for (const uint64_t texels : sheet.texels)
                if (texels)
                    keys.push_back({texels, 0u, sheet.psm, sheet.width, sheet.height});
            store.setGenerated(
                "numerals-" + std::to_string(index), std::move(keys),
                [font, &sheet](uint32_t scale, const uint8_t *native, uint32_t width, uint32_t height,
                               GsReplacementImage &out) {
                    if (out.rgba.empty())
                    {
                        out.width = width * scale;
                        out.height = height * scale;
                    }
                    return hdart::drawDigitRows(*font, scale, native, width, height, sheet.rows, sheet.rowCount,
                                                out.rgba);
                },
                8u, true);
        }
        g_fontShown = true;
        std::printf("[font] text: %s\n", g_fontDescription.c_str());
    }
    else if (!modern && g_fontShown)
    {
        for (size_t index = 0; index < std::size(kFontPages); ++index)
        {
            store.removeGenerated("font-" + std::to_string(index));
        }
        for (size_t index = 0; index < std::size(kDigitSheets); ++index)
        {
            store.removeGenerated("numerals-" + std::to_string(index));
        }
        writeWidths(g_original);
        g_fontShown = false;
        std::printf("[font] text: the game's own\n");
    }
}

} // namespace

void install(PS2Runtime &runtime)
{
    std::lock_guard lock(g_mutex);
    g_rdram = runtime.memory().getRDRAM();
    for (int glyph = 0; glyph < hdart::kGlyphs; ++glyph)
    {
        g_original.character[glyph] = g_rdram[kGlyphTable + glyph * 4u];
        for (int type = 0; type < hdart::kFontTypes; ++type)
        {
            g_original.width[type][glyph] = g_rdram[kGlyphTable + glyph * 4u + 1u + type];
        }
    }
    // The table's first letters, as a check that this is the game's.
    g_haveTable = g_original.character[32] == 'A' && g_original.character[61] == 'a';
    if (!g_haveTable)
    {
        std::fprintf(stderr, "[font] the glyph table is not where it was expected; the game's font stays\n");
    }
    std::string why;
    g_labels = hdart::Font::load(builtIn(), "Roboto Medium", g_original, why);
    if (!g_labels)
    {
        std::fprintf(stderr, "[font] built-in font: %s; the game's prompts stay\n", why.c_str());
    }
    const auto env = [](const char *name) {
        const char *value = std::getenv(name);
        return std::string(value ? value : "");
    };
    if (const std::string prompts = env("RG_BUTTON_PROMPTS"); !prompts.empty())
    {
        g_prompts = prompts == "playstation" || prompts == "ps" ? 0 : prompts == "xbox" ? 1 : 2;
    }
    if (const std::string style = env("RG_FONT_STYLE"); !style.empty())
    {
        g_modern = style == "modern" || style == "1";
    }
    if (env("RG_TEXTURE_REPLACEMENTS") == "0")
    {
        GsReplacementStore::instance().setEnabled(false);
    }
    if (GsReplacementStore::instance().logging())
    {
        // Never matches (no texture hashes to 1): only makes the cache hash
        // and log the parts a font page would be.
        std::vector<GsReplacementKey> probes;
        for (const uint32_t psm : kProbePsms)
        {
            probes.push_back({1u, 0u, psm, 512u, 416u});
            probes.push_back({1u, 0u, psm, 512u, 72u});
        }
        GsReplacementStore::instance().setGenerated(
            "probe", probes, [](uint32_t, const uint8_t *, uint32_t, uint32_t, GsReplacementImage &) { return false; });
    }
    // What the game's Settings screen shows, this run's variables included.
    rg::settingspages::noteNumber("button_prompts", g_prompts);
    rg::settingspages::noteNumber("font_style", g_modern ? 1 : 0);
    g_installed = true;
    apply();
}

void setPrompts(int choice)
{
    std::lock_guard lock(g_mutex);
    g_prompts = std::clamp(choice, 0, 2);
    apply();
}

void setModernFont(bool on)
{
    std::lock_guard lock(g_mutex);
    g_modern = on;
    apply();
}

void setReplacementsEnabled(bool on)
{
    if (const char *value = std::getenv("RG_TEXTURE_REPLACEMENTS"); value && std::strcmp(value, "0") == 0)
    {
        on = false;
    }
    std::lock_guard lock(g_mutex);
    GsReplacementStore::instance().setEnabled(on);
    apply();
}

void notePlayStationPad(bool playStation)
{
    std::lock_guard lock(g_mutex);
    if (playStation == g_playStationPad)
    {
        return;
    }
    g_playStationPad = playStation;
    apply();
}

std::string fontDescription()
{
    std::lock_guard lock(g_mutex);
    return g_fontDescription;
}

} // namespace rg::hdtext
