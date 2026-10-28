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
                   std::vector<float> &mask, int w, int h, float squeeze = 1.0f) const;

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

// A row of digit cells in a menu sprite sheet (UiDrawSpritePixelMaybe,
// 0x292300, draws digit d from (x + pitch * d, y)): `band` rows from `y`
// hold the digits' ink and nothing else; `extra` names the glyphs drawn after
// the ten digits ("/:", left to right, each found by its ink), which are
// redrawn too.
struct DigitRow
{
    int x;
    int y;
    int band;
    int pitch;
    const char *extra;
    // Italic digits: how far the top leans right, per pixel of height.
    float slant = 0.0f;
    // The outline's width in texels (the battle's big digits have a thick
    // one), and how much wider than the font's own the digits may be drawn
    // to fill their cells as the game's bold ones did.
    float outline = 1.0f;
    float widen = 1.0f;
};

// The digit rows of a sheet redrawn with `font`'s digits: each in the box the
// game's digit inked (so widths and centring stay), with the game's outline
// colour and width and its fill's colours from top to bottom, measured from
// `native`. `out` holds the picture to draw over (a pack's upscale, scale
// times the sheet) or is empty, then the game's texels upscaled are.
bool drawDigitRows(const Font &font, uint32_t scale, const uint8_t *native, uint32_t width, uint32_t height,
                   const DigitRow *rows, size_t rowCount, std::vector<uint8_t> &out);

// Letters of a picture (the compass's N E S W) redrawn with `font`, each in
// its box (texels of the picture, the letter's ink with its outline), with
// the game's outline colour and its fill's colours from top to bottom.
struct LetterBox
{
    int16_t x, y, w, h;
    char letter;
};
bool drawLetterBoxes(const Font &font, uint32_t scale, const uint8_t *native, uint32_t width, uint32_t height,
                     const LetterBox *boxes, size_t count, float outline, std::vector<uint8_t> &out);

// PlayStation button art baked into a picture, and the Xbox button drawn over
// it (by position: Cross A, Circle B, Square X, Triangle Y, L1/R1 LB/RB,
// L2/R2 LT/RT).
enum class Button : uint8_t
{
    A,
    B,
    X,
    Y,
    LB,
    RB,
    LT,
    RT,
    // The pictograms the gaiji sheet has no cell for (polish 2): START and
    // SELECT become the Menu and View buttons, the sticks' mushrooms the
    // thumbsticks LS and RS, the d-pad's four arrows Xbox's cross.
    Menu,
    View,
    LS,
    RS,
    DPad,
    // The DualShock drawn on the controls boards (its box: the drawing's
    // dark body): an Xbox-style pad over it, its parts where the
    // DualShock's are so the boards' lines still point at them.
    Pad,
    // A word in the picture's own lettering ("START", "L3") redrawn as
    // `text` ("MENU", "LS") in the label font, in its box and colours.
    Word,
};
struct ButtonPatch
{
    int16_t x, y, w, h; // the game's icon, texels of the picture
    Button button;
    const char *text = nullptr; // Button::Word
};

// The Xbox buttons drawn over `patches`, each filling the game's icon's box
// (so it covers it), over `out` (a pack's upscale, scale times the picture)
// or, when it is empty, the game's texels upscaled.
bool drawButtonPatches(const Font &labels, uint32_t scale, const uint8_t *native, uint32_t width, uint32_t height,
                       const ButtonPatch *patches, size_t count, std::vector<uint8_t> &out);

// The game's texels upscaled `scale` times, nearest.
void upscale(const uint8_t *native, uint32_t width, uint32_t height, uint32_t scale, std::vector<uint8_t> &out);

} // namespace rg::hdart
