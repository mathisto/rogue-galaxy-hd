#pragma once

// Pictures drawn for the replacement textures of src/hd_text.h: the game's
// font pages redrawn from a TrueType font, and the controller icons of its
// gaiji sheet redrawn as Xbox buttons. Pure drawing, no game state, so a
// test can call it alone. Every picture is RGBA8 in the GS's alpha range
// (0x80 opaque), `scale` times the game's texels.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rg::hdart
{

// The game's three font types (TextStyleGetFontType, 0x26EDE0), in its
// numbering: 0 names, 1 messages and menus, 2 the system font.
enum class FontType : int
{
    Name = 0,
    Normal = 1,
    System = 2,
};
constexpr int kFontTypes = 3;

// A glyph cell grid: cells of cellWidth x cellHeight, `columns` a row, the
// glyph drawn from the cell's top-left (FontGetGlyphRect, 0x26E040), and
// where the game's own glyphs put their baseline and cap height.
struct CellGrid
{
    int cellWidth;
    int cellHeight;
    int columns;
    float baseline;
    float capHeight;
};
const CellGrid &grid(FontType type);

// The game's glyph table, D_0044E850: 106 entries of the character and its
// width for each type (name, normal, system); index 0 is the space and
// 89..105 symbols a tag draws (arrows, stars, shapes), '&' at 95.
constexpr int kGlyphs = 106;
struct GlyphTable
{
    std::array<uint8_t, kGlyphs> character{};
    // [type][glyph], the table's bytes as the game stores them (a name width
    // is one more than the advance; FontGetCharWidth, 0x26F4C0).
    std::array<std::array<uint8_t, kGlyphs>, kFontTypes> width{};
};

// A TrueType font laid out over the game's grids: per type a size, a
// baseline and per glyph where its pen starts in the cell, how wide it is
// drawn and the advance it gets, chosen so the glyphs fit their cells and
// the letters take the room the game's did on average (narrower faces are
// tracked out, wider ones condensed), so text wraps about as before.
class Font
{
public:
    // False when the data is no font stb_truetype reads, or lacks letters.
    static std::shared_ptr<const Font> load(std::vector<uint8_t> data, const std::string &name,
                                            const GlyphTable &table, std::string &error);

    const std::string &name() const { return m_name; }
    // The table with this font's advances for the ASCII glyphs, symbols and
    // the space as the game has them.
    const GlyphTable &widths() const { return m_widths; }
    // True for the glyphs this font draws (ASCII), false for the game's
    // own symbols, which are kept.
    bool draws(int glyph) const;

    // The page `native` (the game's texels, width x height) redrawn at
    // `scale`: the font's glyphs over the ASCII cells, the rest the game's
    // texels upscaled.
    bool drawPage(FontType type, uint32_t scale, const uint8_t *native, uint32_t width, uint32_t height,
                  std::vector<uint8_t> &out) const;

    struct Placement
    {
        float penX = 0.0f;   // in the cell, game pixels
        float scaleX = 0.0f; // font units to game pixels, across
        int advance = 0;     // game pixels
    };
    struct Layout
    {
        float scaleY = 0.0f; // font units to game pixels, down
        float baseline = 0.0f;
        std::array<Placement, kGlyphs> glyphs{};
    };
    const Layout &layout(FontType type) const { return m_layouts[static_cast<int>(type)]; }

    // A label (button names), bold-ish white text centred on (cx, cy) in
    // physical pixels, `height` its cap height there: the coverage, added to
    // `mask` (w x h).
    void drawLabel(const std::string &text, float cx, float cy, float capHeight, float widthLimit,
                   std::vector<float> &mask, int w, int h) const;

    ~Font();

private:
    Font() = default;
    struct Info;
    std::unique_ptr<Info> m_info;
    std::vector<uint8_t> m_data;
    std::string m_name;
    GlyphTable m_table;
    GlyphTable m_widths;
    std::array<Layout, kFontTypes> m_layouts{};
};

// The gaiji sheet (gaiji2, 256x256) with its eight controller icons
// ((X) (O) (#) (A) [l1] [r1] [l2] [r2]) redrawn as an Xbox controller's,
// by position: Cross A, Circle B, Square X, Triangle Y, L1 and R1 LB and RB,
// L2 and R2 LT and RT. Same cells, so the advances stay; the rest of the
// sheet is the game's, upscaled. `labels` draws the letters.
bool drawXboxIcons(const Font &labels, uint32_t scale, const uint8_t *native, uint32_t width, uint32_t height,
                   std::vector<uint8_t> &out);

// The game's texels upscaled `scale` times, nearest.
void upscale(const uint8_t *native, uint32_t width, uint32_t height, uint32_t scale, std::vector<uint8_t> &out);

} // namespace rg::hdart
