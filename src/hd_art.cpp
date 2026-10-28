#include "hd_art.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

// Dear ImGui's copy of stb_truetype (public domain / MIT), private to this
// file as imgui_draw.cpp keeps its own.
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"

namespace rg::hdart
{
namespace
{

// The game's own glyphs, measured on its pages (research 08): 'H' from row
// 2 to 17 of a normal cell, 2 to 16 of a system cell, 1 to 13 of a name cell.
constexpr CellGrid kGrids[kFontTypes] = {
    {16, 18, 32, 14.0f, 13.0f}, // name
    {18, 22, 28, 18.0f, 16.0f}, // normal
    {16, 20, 32, 17.0f, 15.0f}, // system
};

// GS alpha for a coverage of 0..1.
uint8_t gsAlpha(float coverage)
{
    return static_cast<uint8_t>(std::lround(std::clamp(coverage, 0.0f, 1.0f) * 128.0f));
}

bool isAscii(uint8_t character)
{
    return character > 0x20u && character < 0x7Fu;
}

} // namespace

struct Font::Info
{
    stbtt_fontinfo font{};
};

const CellGrid &grid(FontType type)
{
    return kGrids[static_cast<int>(type)];
}

void upscale(const uint8_t *native, uint32_t width, uint32_t height, uint32_t scale, std::vector<uint8_t> &out)
{
    const uint32_t w = width * scale, h = height * scale;
    out.resize(static_cast<size_t>(w) * h * 4u);
    for (uint32_t y = 0u; y < h; ++y)
    {
        const uint8_t *source = native + static_cast<size_t>(y / scale) * width * 4u;
        uint8_t *row = out.data() + static_cast<size_t>(y) * w * 4u;
        for (uint32_t x = 0u; x < w; ++x)
        {
            std::memcpy(row + static_cast<size_t>(x) * 4u, source + static_cast<size_t>(x / scale) * 4u, 4u);
        }
    }
}

Font::~Font() = default;

bool Font::draws(int glyph) const
{
    return glyph > 0 && glyph < kGlyphs && isAscii(m_table.character[glyph]) &&
           m_layouts[1].glyphs[glyph].scaleX > 0.0f;
}

std::shared_ptr<const Font> Font::load(std::vector<uint8_t> data, const std::string &name, const GlyphTable &table,
                                       std::string &error)
{
    std::shared_ptr<Font> font(new Font());
    font->m_info = std::make_unique<Info>();
    font->m_data = std::move(data);
    font->m_name = name;
    font->m_table = table;
    font->m_widths = table;
    stbtt_fontinfo &info = font->m_info->font;
    const int offset = stbtt_GetFontOffsetForIndex(font->m_data.data(), 0);
    if (font->m_data.size() < 12u || offset < 0 || !stbtt_InitFont(&info, font->m_data.data(), offset))
    {
        error = name + " is not a TrueType or OpenType font stb_truetype can read";
        return nullptr;
    }
    const float unitsPerEm = 1.0f / stbtt_ScaleForMappingEmToPixels(&info, 1.0f);

    struct Box
    {
        int glyph = 0, advance = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    };
    std::array<Box, kGlyphs> boxes{};
    for (int index = 1; index < kGlyphs; ++index)
    {
        if (!isAscii(table.character[index]))
            continue;
        Box &box = boxes[index];
        box.glyph = stbtt_FindGlyphIndex(&info, table.character[index]);
        if (box.glyph == 0)
            continue;
        int lsb = 0;
        stbtt_GetGlyphHMetrics(&info, box.glyph, &box.advance, &lsb);
        if (!stbtt_GetGlyphBox(&info, box.glyph, &box.x0, &box.y0, &box.x1, &box.y1))
            box.x0 = box.y0 = box.x1 = box.y1 = 0; // blank
    }
    const auto boxOf = [&](char c) -> const Box * {
        for (int index = 1; index < kGlyphs; ++index)
            if (table.character[index] == static_cast<uint8_t>(c) && boxes[index].glyph != 0)
                return &boxes[index];
        return nullptr;
    };
    // Letters and digits set the size; a font without them is no use here.
    float capUnits = 0.0f, up = 0.0f, down = 0.0f;
    int letters = 0;
    for (int index = 1; index < kGlyphs; ++index)
    {
        const uint8_t c = table.character[index];
        if (!std::isalnum(c) || boxes[index].glyph == 0)
            continue;
        ++letters;
        up = std::max(up, static_cast<float>(boxes[index].y1));
        down = std::max(down, static_cast<float>(-boxes[index].y0));
    }
    if (const Box *h = boxOf('H'))
        capUnits = static_cast<float>(h->y1);
    if (letters < 52 || capUnits <= 0.0f || up <= 0.0f)
    {
        error = name + " lacks the Latin letters and digits the game's text needs";
        return nullptr;
    }
    (void)unitsPerEm;

    for (int type = 0; type < kFontTypes; ++type)
    {
        const CellGrid &cell = kGrids[type];
        Layout &layout = font->m_layouts[type];
        // As tall as the game's capitals, unless the ascenders and
        // descenders would then leave the cell.
        const float fit = (static_cast<float>(cell.cellHeight) - 0.5f) / (up + down);
        const float scale = std::min(cell.capHeight / capUnits, fit);
        layout.scaleY = scale;
        layout.baseline = std::clamp(cell.baseline, up * scale + 0.25f,
                                     static_cast<float>(cell.cellHeight) - 0.25f - down * scale);
        // As wide as the game's letters on average, or narrower: a wider
        // face is condensed to the game's widths, so lines wrap as before
        // and nothing outgrows its box.
        float gameSum = 0.0f, fontSum = 0.0f;
        for (int index = 1; index < kGlyphs; ++index)
        {
            const uint8_t c = table.character[index];
            if (!std::isalpha(c) || boxes[index].glyph == 0)
                continue;
            gameSum += static_cast<float>(table.width[type][index]) - (type == 0 ? 1.0f : 0.0f);
            fontSum += static_cast<float>(boxes[index].advance) * scale;
        }
        const float condense = fontSum > gameSum && fontSum > 0.0f ? gameSum / fontSum : 1.0f;
        for (int index = 1; index < kGlyphs; ++index)
        {
            const Box &box = boxes[index];
            if (!isAscii(table.character[index]) || box.glyph == 0)
                continue;
            Placement &place = layout.glyphs[index];
            float scaleX = scale * condense;
            const float advance = static_cast<float>(box.advance) * scaleX;
            place.advance = std::max(1, static_cast<int>(std::lround(advance)));
            // Centred in its rounded advance.
            float pen = (static_cast<float>(place.advance) - advance) * 0.5f;
            if (box.x1 > box.x0)
            {
                float left = pen + static_cast<float>(box.x0) * scaleX;
                if (left < 0.25f)
                {
                    pen += 0.25f - left;
                    left = 0.25f;
                }
                const float right = pen + static_cast<float>(box.x1) * scaleX;
                const float limit = static_cast<float>(cell.cellWidth) - 0.25f;
                if (right > limit)
                {
                    // Too wide for the cell (a W, an @): narrower still.
                    const float squeeze = (limit - left) / (right - left);
                    scaleX *= squeeze;
                    pen = left - static_cast<float>(box.x0) * scaleX;
                }
            }
            place.penX = pen;
            place.scaleX = scaleX;
            uint8_t &stored = font->m_widths.width[type][index];
            stored = static_cast<uint8_t>(std::clamp(place.advance + (type == 0 ? 1 : 0), 1, 255));
        }
    }
    return font;
}

bool Font::drawPage(FontType type, uint32_t scale, const uint8_t *native, uint32_t width, uint32_t height,
                    std::vector<uint8_t> &out) const
{
    const CellGrid &cell = kGrids[static_cast<int>(type)];
    const Layout &layout = m_layouts[static_cast<int>(type)];
    if (scale == 0u || width < static_cast<uint32_t>(cell.columns * cell.cellWidth))
        return false;
    // Over a pack's picture of the page when there is one (its symbols stay).
    if (out.size() != static_cast<size_t>(width) * scale * height * scale * 4u)
        upscale(native, width, height, scale, out);
    const uint32_t outWidth = width * scale;
    const stbtt_fontinfo &info = m_info->font;
    std::vector<uint8_t> bitmap;
    for (int index = 1; index < kGlyphs; ++index)
    {
        const Placement &place = layout.glyphs[index];
        if (!isAscii(m_table.character[index]) || place.scaleX <= 0.0f)
            continue;
        const int glyph = stbtt_FindGlyphIndex(&info, m_table.character[index]);
        const int cellX = (index % cell.columns) * cell.cellWidth;
        const int cellY = (index / cell.columns) * cell.cellHeight;
        if (static_cast<uint32_t>(cellY + cell.cellHeight) > height)
            continue;
        // The cell, cleared to transparent white (the game's palette).
        const int x0 = cellX * static_cast<int>(scale), y0 = cellY * static_cast<int>(scale);
        const int x1 = x0 + cell.cellWidth * static_cast<int>(scale);
        const int y1 = y0 + cell.cellHeight * static_cast<int>(scale);
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x)
            {
                uint8_t *texel = out.data() + (static_cast<size_t>(y) * outWidth + x) * 4u;
                texel[0] = texel[1] = texel[2] = 0xFFu;
                texel[3] = 0u;
            }
        // A glyph too tall for the cell (a bracket, a brace) shrinks about
        // its baseline.
        float scaleY = layout.scaleY;
        float scaleX = place.scaleX;
        int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
        if (stbtt_GetGlyphBox(&info, glyph, &bx0, &by0, &bx1, &by1))
        {
            const float above = static_cast<float>(by1) * scaleY, below = static_cast<float>(-by0) * scaleY;
            float shrink = 1.0f;
            if (above > layout.baseline - 0.25f)
                shrink = std::min(shrink, (layout.baseline - 0.25f) / above);
            if (below > static_cast<float>(cell.cellHeight) - 0.25f - layout.baseline)
                shrink = std::min(shrink, (static_cast<float>(cell.cellHeight) - 0.25f - layout.baseline) / below);
            scaleY *= shrink;
            scaleX *= shrink;
        }
        const float originX = (static_cast<float>(cellX) + place.penX) * static_cast<float>(scale);
        const float originY = (static_cast<float>(cellY) + layout.baseline) * static_cast<float>(scale);
        const float floorX = std::floor(originX), floorY = std::floor(originY);
        const float sx = scaleX * static_cast<float>(scale), sy = scaleY * static_cast<float>(scale);
        int ix0 = 0, iy0 = 0, ix1 = 0, iy1 = 0;
        stbtt_GetGlyphBitmapBoxSubpixel(&info, glyph, sx, sy, originX - floorX, originY - floorY, &ix0, &iy0, &ix1,
                                        &iy1);
        const int bw = ix1 - ix0, bh = iy1 - iy0;
        if (bw <= 0 || bh <= 0)
            continue;
        bitmap.assign(static_cast<size_t>(bw) * bh, 0u);
        stbtt_MakeGlyphBitmapSubpixel(&info, bitmap.data(), bw, bh, bw, sx, sy, originX - floorX, originY - floorY,
                                      glyph);
        const int left = static_cast<int>(floorX) + ix0, top = static_cast<int>(floorY) + iy0;
        for (int y = 0; y < bh; ++y)
        {
            const int py = top + y;
            if (py < y0 || py >= y1)
                continue;
            for (int x = 0; x < bw; ++x)
            {
                const int px = left + x;
                if (px < x0 || px >= x1)
                    continue;
                uint8_t *texel = out.data() + (static_cast<size_t>(py) * outWidth + px) * 4u;
                texel[3] = std::max(texel[3], gsAlpha(bitmap[static_cast<size_t>(y) * bw + x] / 255.0f));
            }
        }
    }
    return true;
}

void Font::drawLabel(const std::string &text, float cx, float cy, float capHeight, float widthLimit,
                     std::vector<float> &mask, int w, int h, float squeeze) const
{
    const stbtt_fontinfo &info = m_info->font;
    int hx0 = 0, hy0 = 0, hx1 = 0, hy1 = 0;
    const int hGlyph = stbtt_FindGlyphIndex(&info, 'H');
    if (!stbtt_GetGlyphBox(&info, hGlyph, &hx0, &hy0, &hx1, &hy1) || hy1 <= 0)
        return;
    float scale = capHeight / static_cast<float>(hy1);
    // The label's width at that size, tightened to the box.
    float total = 0.0f;
    for (size_t i = 0; i < text.size(); ++i)
    {
        int advance = 0, lsb = 0;
        const int glyph = stbtt_FindGlyphIndex(&info, static_cast<unsigned char>(text[i]));
        stbtt_GetGlyphHMetrics(&info, glyph, &advance, &lsb);
        total += static_cast<float>(advance);
        if (i + 1 < text.size())
            total += static_cast<float>(stbtt_GetGlyphKernAdvance(
                &info, glyph, stbtt_FindGlyphIndex(&info, static_cast<unsigned char>(text[i + 1]))));
    }
    float scaleX = scale * squeeze;
    if (total * scaleX > widthLimit && total > 0.0f)
        scaleX = widthLimit / total;
    float pen = cx - total * scaleX * 0.5f;
    const float baseline = cy + capHeight * 0.5f;
    std::vector<uint8_t> bitmap;
    for (size_t i = 0; i < text.size(); ++i)
    {
        const int glyph = stbtt_FindGlyphIndex(&info, static_cast<unsigned char>(text[i]));
        int advance = 0, lsb = 0;
        stbtt_GetGlyphHMetrics(&info, glyph, &advance, &lsb);
        const float fx = std::floor(pen), fy = std::floor(baseline);
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetGlyphBitmapBoxSubpixel(&info, glyph, scaleX, scale, pen - fx, baseline - fy, &x0, &y0, &x1, &y1);
        const int bw = x1 - x0, bh = y1 - y0;
        if (bw > 0 && bh > 0)
        {
            bitmap.assign(static_cast<size_t>(bw) * bh, 0u);
            stbtt_MakeGlyphBitmapSubpixel(&info, bitmap.data(), bw, bh, bw, scaleX, scale, pen - fx, baseline - fy,
                                          glyph);
            for (int y = 0; y < bh; ++y)
                for (int x = 0; x < bw; ++x)
                {
                    const int px = static_cast<int>(fx) + x0 + x, py = static_cast<int>(fy) + y0 + y;
                    if (px < 0 || py < 0 || px >= w || py >= h)
                        continue;
                    float &m = mask[static_cast<size_t>(py) * w + px];
                    m = std::min(1.0f, m + bitmap[static_cast<size_t>(y) * bw + x] / 255.0f);
                }
        }
        pen += static_cast<float>(advance) * scaleX;
        if (i + 1 < text.size())
            pen += static_cast<float>(stbtt_GetGlyphKernAdvance(
                       &info, glyph, stbtt_FindGlyphIndex(&info, static_cast<unsigned char>(text[i + 1])))) *
                   scaleX;
    }
}

namespace
{

// A small premultiplied RGBA canvas in physical pixels, for the icons.
struct Canvas
{
    int w = 0, h = 0;
    std::vector<float> rgba; // premultiplied

    Canvas(int width, int height) : w(width), h(height), rgba(static_cast<size_t>(width) * height * 4u, 0.0f) {}

    // Paints `color` (straight RGB 0..1) over the canvas where `coverage`
    // says, with `alpha` overall.
    template <typename Coverage>
    void paint(Coverage coverage, const float (&top)[3], const float (&bottom)[3], float alpha = 1.0f)
    {
        for (int y = 0; y < h; ++y)
        {
            const float t = (static_cast<float>(y) + 0.5f) / static_cast<float>(h);
            const float color[3] = {top[0] + (bottom[0] - top[0]) * t, top[1] + (bottom[1] - top[1]) * t,
                                    top[2] + (bottom[2] - top[2]) * t};
            for (int x = 0; x < w; ++x)
            {
                const float a = std::clamp(coverage(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f), 0.0f,
                                           1.0f) *
                                alpha;
                if (a <= 0.0f)
                    continue;
                float *p = rgba.data() + (static_cast<size_t>(y) * w + x) * 4u;
                for (int c = 0; c < 3; ++c)
                    p[c] = color[c] * a + p[c] * (1.0f - a);
                p[3] = a + p[3] * (1.0f - a);
            }
        }
    }
};

// Coverage of a signed distance (negative inside), one pixel of ramp.
float edge(float distance)
{
    return std::clamp(0.5f - distance, 0.0f, 1.0f);
}

float circle(float x, float y, float cx, float cy, float r)
{
    return std::hypot(x - cx, y - cy) - r;
}

// A box with its top corners rounded by rTop and its bottom ones by rBottom.
float roundedBox(float x, float y, float x0, float y0, float x1, float y1, float rTop, float rBottom)
{
    const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    const float hx = (x1 - x0) * 0.5f, hy = (y1 - y0) * 0.5f;
    const float r = y < cy ? rTop : rBottom;
    const float qx = std::abs(x - cx) - hx + r, qy = std::abs(y - cy) - hy + r;
    return std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - r;
}

// The mask grown by `radius` pixels, for an outline around a label.
std::vector<float> grow(const std::vector<float> &mask, int w, int h, float radius)
{
    std::vector<float> out(mask.size(), 0.0f);
    const int reach = static_cast<int>(std::ceil(radius));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            float best = 0.0f;
            for (int dy = -reach; dy <= reach; ++dy)
                for (int dx = -reach; dx <= reach; ++dx)
                {
                    const int sx = x + dx, sy = y + dy;
                    if (sx < 0 || sy < 0 || sx >= w || sy >= h)
                        continue;
                    const float d = std::hypot(static_cast<float>(dx), static_cast<float>(dy));
                    const float weight = std::clamp(radius + 0.5f - d, 0.0f, 1.0f);
                    best = std::max(best, mask[static_cast<size_t>(sy) * w + sx] * weight);
                }
            out[static_cast<size_t>(y) * w + x] = best;
        }
    return out;
}

constexpr float kOutline[3] = {0.06f, 0.06f, 0.07f};
constexpr float kWhite[3] = {1.0f, 1.0f, 1.0f};
constexpr float kLabelEdge[3] = {0.08f, 0.08f, 0.09f};

void label(Canvas &canvas, const Font &font, const std::string &text, float cx, float cy, float cap, float limit,
           float scale)
{
    std::vector<float> mask(static_cast<size_t>(canvas.w) * canvas.h, 0.0f);
    font.drawLabel(text, cx, cy, cap, limit, mask, canvas.w, canvas.h);
    const std::vector<float> outline = grow(mask, canvas.w, canvas.h, 0.85f * scale);
    canvas.paint([&](float x, float y) { return outline[static_cast<size_t>(y) * canvas.w + static_cast<int>(x)]; },
                 kLabelEdge, kLabelEdge, 0.85f);
    canvas.paint([&](float x, float y) { return mask[static_cast<size_t>(y) * canvas.w + static_cast<int>(x)]; },
                 kWhite, kWhite);
}

// A face button: a disc in the button's colour, darker below, with a dark
// rim like the game's own icons, and its letter in white.
void faceButton(Canvas &canvas, const Font &font, char letter, const float (&color)[3], float scale)
{
    const float cx = 11.0f * scale, cy = 11.0f * scale;
    canvas.paint([&](float x, float y) { return edge(circle(x, y, cx, cy, 10.0f * scale)); }, kOutline, kOutline);
    const float top[3] = {std::min(1.0f, color[0] * 1.12f + 0.06f), std::min(1.0f, color[1] * 1.12f + 0.06f),
                          std::min(1.0f, color[2] * 1.12f + 0.06f)};
    const float bottom[3] = {color[0] * 0.72f, color[1] * 0.72f, color[2] * 0.72f};
    canvas.paint([&](float x, float y) { return edge(circle(x, y, cx, cy, 8.9f * scale)); }, top, bottom);
    // A soft highlight across the top, as on the pad's domed buttons.
    canvas.paint(
        [&](float x, float y) {
            return edge(circle(x, y, cx, cy - 3.2f * scale, 5.6f * scale)) *
                   edge(circle(x, y, cx, cy, 8.0f * scale)) * 0.22f;
        },
        kWhite, kWhite);
    label(canvas, font, std::string(1, letter), cx, cy + 0.15f * scale, 10.0f * scale, 14.0f * scale, scale);
}

// A shoulder button: the game's pill for the bumpers, a trigger's shape
// (rounder on top) for the triggers, dark grey, its name in white.
void shoulderButton(Canvas &canvas, const Font &font, const std::string &name, bool trigger, float scale)
{
    const float x0 = 1.0f * scale, y0 = 1.0f * scale, x1 = 31.0f * scale, y1 = 19.0f * scale;
    const float rTop = (trigger ? 8.5f : 5.0f) * scale, rBottom = (trigger ? 3.0f : 5.0f) * scale;
    canvas.paint([&](float x, float y) { return edge(roundedBox(x, y, x0, y0, x1, y1, rTop, rBottom)); }, kOutline,
                 kOutline);
    const float top[3] = {0.47f, 0.47f, 0.50f};
    const float bottom[3] = {0.24f, 0.24f, 0.26f};
    const float inset = 1.1f * scale;
    canvas.paint(
        [&](float x, float y) {
            return edge(roundedBox(x, y, x0 + inset, y0 + inset, x1 - inset, y1 - inset, rTop - inset * 0.7f,
                                   std::max(rBottom - inset * 0.7f, 0.5f * scale)));
        },
        top, bottom);
    label(canvas, font, name, 16.0f * scale, 10.3f * scale, 9.5f * scale, 22.0f * scale, scale);
}

void blit(const Canvas &canvas, std::vector<uint8_t> &out, uint32_t outWidth, int left, int top)
{
    for (int y = 0; y < canvas.h; ++y)
        for (int x = 0; x < canvas.w; ++x)
        {
            const float *p = canvas.rgba.data() + (static_cast<size_t>(y) * canvas.w + x) * 4u;
            uint8_t *texel = out.data() + (static_cast<size_t>(top + y) * outWidth + left + x) * 4u;
            const float a = p[3];
            for (int c = 0; c < 3; ++c)
                texel[c] = a > 0.0f ? static_cast<uint8_t>(std::lround(std::clamp(p[c] / a, 0.0f, 1.0f) * 255.0f))
                                    : 0u;
            texel[3] = gsAlpha(a);
        }
}

struct Rgb
{
    float c[3] = {0.0f, 0.0f, 0.0f};
};

float luminance(const uint8_t *p)
{
    return 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
}

// A glyph's ink drawn by drawLabel centred on (0, 0) at `size` (its 'H'
// height) and `squeeze` across: its size and its centre's offset.
struct Ink
{
    bool any = false;
    float w = 0.0f, h = 0.0f, dx = 0.0f, dy = 0.0f;
};

Ink measure(const Font &font, char c, float size, float squeeze)
{
    const int tw = static_cast<int>(std::ceil(size * 4.0f)) + 8, th = tw;
    std::vector<float> trial(static_cast<size_t>(tw) * th, 0.0f);
    font.drawLabel(std::string(1, c), tw * 0.5f, th * 0.5f, size, 1e9f, trial, tw, th, squeeze);
    int x0 = tw, y0 = th, x1 = -1, y1 = -1;
    for (int y = 0; y < th; ++y)
        for (int x = 0; x < tw; ++x)
            if (trial[static_cast<size_t>(y) * tw + x] > 0.02f)
            {
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
                y0 = std::min(y0, y);
                y1 = std::max(y1, y);
            }
    Ink ink;
    if (x1 < 0)
        return ink;
    ink.any = true;
    ink.w = static_cast<float>(x1 - x0 + 1);
    ink.h = static_cast<float>(y1 - y0 + 1);
    ink.dx = (static_cast<float>(x0) + static_cast<float>(x1) + 1.0f) * 0.5f - tw * 0.5f;
    ink.dy = (static_cast<float>(y0) + static_cast<float>(y1) + 1.0f) * 0.5f - th * 0.5f;
    return ink;
}

// One size and squeeze for all of `chars`, so their ink fits maxW x maxH:
// as tall as `cap` allows, condensed across down to 72% before it shrinks.
void fitGlyphs(const Font &font, const char *chars, float cap, float maxW, float maxH, float &size, float &squeeze)
{
    float inkW = 0.0f, inkH = 0.0f;
    for (const char *c = chars; *c; ++c)
    {
        const Ink ink = measure(font, *c, cap, 1.0f);
        inkW = std::max(inkW, ink.w);
        inkH = std::max(inkH, ink.h);
    }
    size = cap;
    squeeze = 1.0f;
    if (inkW <= 0.0f || inkH <= 0.0f)
        return;
    const float fitH = std::min(1.0f, maxH / inkH);
    size = cap * fitH;
    const float fitW = maxW / (inkW * fitH);
    if (fitW < 1.0f)
    {
        squeeze = std::max(0.72f, fitW);
        if (fitW < 0.72f)
            size *= fitW / 0.72f;
    }
}

// One glyph's coverage at `size` and `squeeze`, its ink centred on (cx, cy)
// of a w x h mask, in physical pixels. `byCap`: centred vertically as a
// capital would be (digits, so they share a baseline).
void glyphMask(const Font &font, char c, float cx, float cy, float size, float squeeze, bool byCap,
               std::vector<float> &mask, int w, int h)
{
    const Ink ink = measure(font, c, size, squeeze);
    if (!ink.any)
        return;
    std::vector<float> drawn(static_cast<size_t>(w) * h, 0.0f);
    font.drawLabel(std::string(1, c), cx - ink.dx, byCap ? cy : cy - ink.dy, size, 1e9f, drawn, w, h, squeeze);
    for (size_t i = 0; i < mask.size(); ++i)
        mask[i] = std::min(1.0f, mask[i] + drawn[i]);
}

// A cell redrawn: the glyph's mask with an outline around it, the outline in
// `edge` (alpha `edgeAlpha`), the fill in `fill[y]` by native row from `top`.
void paintGlyph(std::vector<uint8_t> &out, uint32_t outWidth, int left, int top, int w, int h,
                const std::vector<float> &mask, float outlinePx, const Rgb &edgeColor, float edgeAlpha,
                const std::vector<Rgb> &fill, int nativeTop, float scale)
{
    const std::vector<float> outline = grow(mask, w, h, outlinePx);
    for (int y = 0; y < h; ++y)
    {
        // The fill's colour here, between the measured rows.
        const float ny = (static_cast<float>(y) + 0.5f) / scale - 0.5f;
        const int n = static_cast<int>(fill.size());
        const float clamped = std::clamp(ny, 0.0f, static_cast<float>(n - 1));
        const int i0 = static_cast<int>(clamped), i1 = std::min(i0 + 1, n - 1);
        const float t = clamped - static_cast<float>(i0);
        (void)nativeTop;
        for (int x = 0; x < w; ++x)
        {
            const size_t at = static_cast<size_t>(y) * w + x;
            const float aF = mask[at];
            const float aO = outline[at] * edgeAlpha;
            const float a = aF + aO * (1.0f - aF);
            uint8_t *texel = out.data() + (static_cast<size_t>(top + y) * outWidth + left + x) * 4u;
            if (a <= 0.0f)
            {
                texel[0] = static_cast<uint8_t>(edgeColor.c[0]);
                texel[1] = static_cast<uint8_t>(edgeColor.c[1]);
                texel[2] = static_cast<uint8_t>(edgeColor.c[2]);
                texel[3] = 0u;
                continue;
            }
            for (int c = 0; c < 3; ++c)
            {
                const float f = fill[i0].c[c] + (fill[i1].c[c] - fill[i0].c[c]) * t;
                const float v = (f * aF + edgeColor.c[c] * aO * (1.0f - aF)) / a;
                texel[c] = static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 255.0f)));
            }
            texel[3] = gsAlpha(a);
        }
    }
}

// The canvas laid over the picture at (left, top), its alpha over what is
// there (the GS's alpha range in `out`).
void blend(const Canvas &canvas, std::vector<uint8_t> &out, uint32_t outWidth, uint32_t outHeight, int left, int top)
{
    for (int y = 0; y < canvas.h; ++y)
        for (int x = 0; x < canvas.w; ++x)
        {
            const int px = left + x, py = top + y;
            if (px < 0 || py < 0 || px >= static_cast<int>(outWidth) || py >= static_cast<int>(outHeight))
                continue;
            const float *p = canvas.rgba.data() + (static_cast<size_t>(y) * canvas.w + x) * 4u;
            const float a = p[3];
            if (a <= 0.0f)
                continue;
            uint8_t *texel = out.data() + (static_cast<size_t>(py) * outWidth + px) * 4u;
            const float under = std::min(1.0f, texel[3] / 128.0f);
            const float total = a + under * (1.0f - a);
            for (int c = 0; c < 3; ++c)
            {
                const float v = (p[c] * 255.0f + texel[c] * under * (1.0f - a)) / total;
                texel[c] = static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 255.0f)));
            }
            texel[3] = gsAlpha(total);
        }
}

} // namespace

bool drawButtonPatches(const Font &labels, uint32_t scale, const uint8_t *native, uint32_t width, uint32_t height,
                       const ButtonPatch *patches, size_t count, std::vector<uint8_t> &out)
{
    if (scale == 0u || !native)
        return false;
    const uint32_t outWidth = width * scale, outHeight = height * scale;
    if (out.size() != static_cast<size_t>(outWidth) * outHeight * 4u)
        upscale(native, width, height, scale, out);
    const float s = static_cast<float>(scale);
    for (size_t i = 0; i < count; ++i)
    {
        const ButtonPatch &patch = patches[i];
        const float rw = static_cast<float>(patch.w) * s, rh = static_cast<float>(patch.h) * s;
        const float cx = (static_cast<float>(patch.x) + static_cast<float>(patch.w) * 0.5f) * s;
        const float cy = (static_cast<float>(patch.y) + static_cast<float>(patch.h) * 0.5f) * s;
        const bool face = patch.button <= Button::Y;
        // The icon's own unit: a face icon is 22 units across, a shoulder 32 x 20,
        // grown a little so the game's rim is covered.
        const float unit = face ? std::max(rw, rh) / 20.0f : std::max(rw / 30.0f, rh / 18.0f);
        const int cw = static_cast<int>(std::ceil((face ? 22.0f : 32.0f) * unit));
        const int ch = static_cast<int>(std::ceil((face ? 22.0f : 20.0f) * unit));
        Canvas canvas(cw, ch);
        switch (patch.button)
        {
        case Button::A:
            faceButton(canvas, labels, 'A', {0.36f, 0.70f, 0.20f}, unit);
            break;
        case Button::B:
            faceButton(canvas, labels, 'B', {0.86f, 0.22f, 0.20f}, unit);
            break;
        case Button::X:
            faceButton(canvas, labels, 'X', {0.20f, 0.45f, 0.90f}, unit);
            break;
        case Button::Y:
            faceButton(canvas, labels, 'Y', {0.93f, 0.72f, 0.10f}, unit);
            break;
        case Button::LB:
            shoulderButton(canvas, labels, "LB", false, unit);
            break;
        case Button::RB:
            shoulderButton(canvas, labels, "RB", false, unit);
            break;
        case Button::LT:
            shoulderButton(canvas, labels, "LT", true, unit);
            break;
        case Button::RT:
            shoulderButton(canvas, labels, "RT", true, unit);
            break;
        }
        blend(canvas, out, outWidth, outHeight, static_cast<int>(std::lround(cx - cw * 0.5f)),
              static_cast<int>(std::lround(cy - ch * 0.5f)));
    }
    return true;
}

bool drawLetterBoxes(const Font &font, uint32_t scale, const uint8_t *native, uint32_t width, uint32_t height,
                     const LetterBox *boxes, size_t count, float outline, std::vector<uint8_t> &out)
{
    if (scale == 0u || !native)
        return false;
    const uint32_t outWidth = width * scale;
    if (out.size() != static_cast<size_t>(outWidth) * height * scale * 4u)
        upscale(native, width, height, scale, out);
    const float s = static_cast<float>(scale);
    const auto at = [&](int x, int y) { return native + (static_cast<size_t>(y) * width + x) * 4u; };
    for (size_t i = 0; i < count; ++i)
    {
        const LetterBox &box = boxes[i];
        // The letter's colours: its outline the darkest texels, its fill by
        // row the lightest.
        float lo = 1e9f, hi = -1e9f;
        for (int y = box.y; y < box.y + box.h; ++y)
            for (int x = box.x; x < box.x + box.w; ++x)
                if (at(x, y)[3] >= 0x40u)
                {
                    lo = std::min(lo, luminance(at(x, y)));
                    hi = std::max(hi, luminance(at(x, y)));
                }
        if (hi < lo)
            continue;
        Rgb edgeColor;
        float edgeAlpha = 0.0f;
        int edgeCount = 0;
        std::vector<Rgb> fill(static_cast<size_t>(box.h));
        std::vector<int> fillCount(static_cast<size_t>(box.h), 0);
        for (int y = 0; y < box.h; ++y)
            for (int x = 0; x < box.w; ++x)
            {
                const uint8_t *p = at(box.x + x, box.y + y);
                if (p[3] < 0x40u)
                    continue;
                const float l = luminance(p);
                if (l <= lo + 0.3f * (hi - lo))
                {
                    for (int c = 0; c < 3; ++c)
                        edgeColor.c[c] += p[c];
                    edgeAlpha += std::min(1.0f, p[3] / 128.0f);
                    ++edgeCount;
                }
                else if (l >= lo + 0.55f * (hi - lo))
                {
                    for (int c = 0; c < 3; ++c)
                        fill[y].c[c] += p[c];
                    ++fillCount[y];
                }
            }
        if (edgeCount == 0)
            continue;
        for (int c = 0; c < 3; ++c)
            edgeColor.c[c] /= static_cast<float>(edgeCount);
        edgeAlpha /= static_cast<float>(edgeCount);
        int first = -1;
        for (int y = 0; y < box.h; ++y)
            if (fillCount[y])
            {
                for (int c = 0; c < 3; ++c)
                    fill[y].c[c] /= static_cast<float>(fillCount[y]);
                if (first < 0)
                    first = y;
            }
        if (first < 0)
            continue;
        for (int y = 0; y < box.h; ++y)
            if (!fillCount[y])
            {
                int best = first;
                for (int k = 0; k < box.h; ++k)
                    if (fillCount[k] && std::abs(k - y) < std::abs(best - y))
                        best = k;
                fill[y] = fill[best];
            }
        const float outlinePx = outline * s;
        const int w = box.w * static_cast<int>(scale), h = box.h * static_cast<int>(scale);
        const char one[2] = {box.letter, 0};
        float size = 0.0f, squeeze = 1.0f;
        fitGlyphs(font, one, static_cast<float>(h) - 2.0f * outlinePx, static_cast<float>(w) - 2.0f * outlinePx,
                  static_cast<float>(h) - 2.0f * outlinePx, size, squeeze);
        std::vector<float> mask(static_cast<size_t>(w) * h, 0.0f);
        glyphMask(font, box.letter, w * 0.5f, h * 0.5f, size, squeeze, false, mask, w, h);
        paintGlyph(out, outWidth, box.x * static_cast<int>(scale), box.y * static_cast<int>(scale), w, h, mask,
                   outlinePx, edgeColor, edgeAlpha, fill, box.y, s);
    }
    return true;
}

bool drawDigitRows(const Font &font, uint32_t scale, const uint8_t *native, uint32_t width, uint32_t height,
                   const DigitRow *rows, size_t rowCount, std::vector<uint8_t> &out)
{
    if (scale == 0u || !native)
        return false;
    const uint32_t outWidth = width * scale;
    if (out.size() != static_cast<size_t>(outWidth) * height * scale * 4u)
        upscale(native, width, height, scale, out);
    const float s = static_cast<float>(scale);
    const auto at = [&](int x, int y) { return native + (static_cast<size_t>(y) * width + x) * 4u; };
    for (size_t r = 0; r < rowCount; ++r)
    {
        const DigitRow &row = rows[r];
        if (row.x < 0 || row.y < 0 || row.x + row.pitch * 10 > static_cast<int>(width) ||
            row.y + row.band > static_cast<int>(height))
            continue;
        // The digits' ink box in their cells, and their colours.
        int ix0 = row.pitch, ix1 = -1, iy0 = row.band, iy1 = -1;
        float lo = 1e9f, hi = -1e9f;
        for (int d = 0; d < 10; ++d)
            for (int y = 0; y < row.band; ++y)
                for (int x = 0; x < row.pitch; ++x)
                {
                    const uint8_t *p = at(row.x + d * row.pitch + x, row.y + y);
                    if (p[3] <= 8u)
                        continue;
                    ix0 = std::min(ix0, x);
                    ix1 = std::max(ix1, x);
                    iy0 = std::min(iy0, y);
                    iy1 = std::max(iy1, y);
                    if (p[3] >= 0x40u)
                    {
                        lo = std::min(lo, luminance(p));
                        hi = std::max(hi, luminance(p));
                    }
                }
        if (ix1 < 0 || hi < lo)
            continue;
        Rgb edgeColor;
        float edgeAlpha = 0.0f;
        int edgeCount = 0;
        std::vector<Rgb> fill(static_cast<size_t>(row.band));
        std::vector<int> fillCount(static_cast<size_t>(row.band), 0);
        const float darkLimit = lo + 0.3f * (hi - lo), lightLimit = lo + 0.6f * (hi - lo);
        for (int d = 0; d < 10; ++d)
            for (int y = 0; y < row.band; ++y)
                for (int x = 0; x < row.pitch; ++x)
                {
                    const uint8_t *p = at(row.x + d * row.pitch + x, row.y + y);
                    if (p[3] < 0x40u)
                        continue;
                    const float l = luminance(p);
                    if (l <= darkLimit)
                    {
                        for (int c = 0; c < 3; ++c)
                            edgeColor.c[c] += p[c];
                        edgeAlpha += std::min(1.0f, p[3] / 128.0f);
                        ++edgeCount;
                    }
                    else if (l >= lightLimit)
                    {
                        for (int c = 0; c < 3; ++c)
                            fill[y].c[c] += p[c];
                        ++fillCount[y];
                    }
                }
        if (edgeCount == 0)
            continue;
        for (int c = 0; c < 3; ++c)
            edgeColor.c[c] /= static_cast<float>(edgeCount);
        edgeAlpha /= static_cast<float>(edgeCount);
        // Rows with no light texels take the nearest measured one.
        int firstMeasured = -1;
        for (int y = 0; y < row.band; ++y)
            if (fillCount[y] > 0)
            {
                for (int c = 0; c < 3; ++c)
                    fill[y].c[c] /= static_cast<float>(fillCount[y]);
                if (firstMeasured < 0)
                    firstMeasured = y;
            }
        if (firstMeasured < 0)
            continue;
        for (int y = 0; y < row.band; ++y)
            if (fillCount[y] == 0)
            {
                int best = firstMeasured;
                for (int k = 0; k < row.band; ++k)
                    if (fillCount[k] > 0 && std::abs(k - y) < std::abs(best - y))
                        best = k;
                fill[y] = fill[best];
            }

        // The game's outline is a texel wide.
        const float outlinePx = 1.0f * s;
        const float inkW = static_cast<float>(ix1 - ix0 + 1) * s, inkH = static_cast<float>(iy1 - iy0 + 1) * s;
        const float cap = inkH - 2.0f * outlinePx;
        const float cy = (static_cast<float>(iy0) + static_cast<float>(iy1) + 1.0f) * 0.5f * s;
        const int cellW = row.pitch * static_cast<int>(scale), bandH = row.band * static_cast<int>(scale);
        float size = cap, squeeze = 1.0f;
        fitGlyphs(font, "0123456789", cap, inkW - 2.0f * outlinePx - row.slant * cap, cap, size, squeeze);
        for (int d = 0; d < 10; ++d)
        {
            std::vector<float> mask(static_cast<size_t>(cellW) * bandH, 0.0f);
            const float cx = (static_cast<float>(ix0) + static_cast<float>(ix1) + 1.0f) * 0.5f * s;
            glyphMask(font, static_cast<char>('0' + d), cx, cy, size, squeeze, true, mask, cellW, bandH);
            if (row.slant != 0.0f)
            {
                // Leaned like the game's italic digits, about their middle.
                std::vector<float> leaned(mask.size(), 0.0f);
                for (int y = 0; y < bandH; ++y)
                {
                    const float shift = row.slant * (cy - (static_cast<float>(y) + 0.5f));
                    for (int x = 0; x < cellW; ++x)
                    {
                        const float sx = static_cast<float>(x) - shift;
                        const int x0 = static_cast<int>(std::floor(sx));
                        const float t = sx - static_cast<float>(x0);
                        const auto sample = [&](int at) {
                            return at < 0 || at >= cellW ? 0.0f : mask[static_cast<size_t>(y) * cellW + at];
                        };
                        leaned[static_cast<size_t>(y) * cellW + x] = sample(x0) * (1.0f - t) + sample(x0 + 1) * t;
                    }
                }
                mask.swap(leaned);
            }
            paintGlyph(out, outWidth, (row.x + d * row.pitch) * static_cast<int>(scale), row.y * static_cast<int>(scale),
                       cellW, bandH, mask, outlinePx, edgeColor, edgeAlpha, fill, row.y, s);
        }
        // The glyphs after the digits, each found by its ink.
        int x = row.x + row.pitch * 10;
        for (const char *extra = row.extra; extra && *extra; ++extra)
        {
            const auto inked = [&](int column) {
                for (int y = 0; y < row.band; ++y)
                    if (at(column, row.y + y)[3] > 8u)
                        return true;
                return false;
            };
            while (x < static_cast<int>(width) && !inked(x))
                ++x;
            const int a = x;
            while (x < static_cast<int>(width) && inked(x))
                ++x;
            const int b = x; // one past
            if (a >= static_cast<int>(width))
                break;
            int gy0 = row.band, gy1 = -1;
            for (int column = a; column < b; ++column)
                for (int y = 0; y < row.band; ++y)
                    if (at(column, row.y + y)[3] > 8u)
                    {
                        gy0 = std::min(gy0, y);
                        gy1 = std::max(gy1, y);
                    }
            const int boxW = (b - a) * static_cast<int>(scale);
            std::vector<float> mask(static_cast<size_t>(boxW) * bandH, 0.0f);
            const float gx = boxW * 0.5f;
            const float gcy = (static_cast<float>(gy0) + static_cast<float>(gy1) + 1.0f) * 0.5f * s;
            const char one[2] = {*extra, 0};
            float extraSize = size, extraSqueeze = squeeze;
            fitGlyphs(font, one, size, static_cast<float>(boxW) - 2.0f * outlinePx,
                      static_cast<float>(gy1 - gy0 + 1) * s - 2.0f * outlinePx, extraSize, extraSqueeze);
            glyphMask(font, *extra, gx, gcy, extraSize, extraSqueeze, false, mask, boxW, bandH);
            paintGlyph(out, outWidth, a * static_cast<int>(scale), row.y * static_cast<int>(scale), boxW, bandH, mask,
                       outlinePx, edgeColor, edgeAlpha, fill, row.y, s);
        }
    }
    return true;
}

bool drawXboxIcons(const Font &labels, uint32_t scale, const uint8_t *native, uint32_t width, uint32_t height,
                   std::vector<uint8_t> &out)
{
    if (width < 128u || height < 42u || scale == 0u)
        return false;
    // Over a pack's picture of the sheet when there is one.
    if (out.size() != static_cast<size_t>(width) * scale * height * scale * 4u)
        upscale(native, width, height, scale, out);
    const float s = static_cast<float>(scale);
    const uint32_t outWidth = width * scale;
    // The cells of gaiji2's metric table (D_0044E680): the shoulder buttons
    // 32x20 along the top, the face buttons 22x22 below.
    struct Shoulder
    {
        int x;
        const char *name;
        bool trigger;
    };
    for (const Shoulder &shoulder : {Shoulder{0, "LT", true}, Shoulder{32, "LB", false}, Shoulder{64, "RB", false},
                                     Shoulder{96, "RT", true}})
    {
        Canvas canvas(32 * static_cast<int>(scale), 20 * static_cast<int>(scale));
        shoulderButton(canvas, labels, shoulder.name, shoulder.trigger, s);
        blit(canvas, out, outWidth, shoulder.x * static_cast<int>(scale), 0);
    }
    struct Face
    {
        int x;
        char letter;
        float color[3];
    };
    // By position: Circle (east) B, Triangle (north) Y, Square (west) X,
    // Cross (south) A.
    const Face faces[] = {
        {0, 'B', {0.86f, 0.22f, 0.20f}},
        {22, 'Y', {0.93f, 0.72f, 0.10f}},
        {44, 'X', {0.20f, 0.45f, 0.90f}},
        {66, 'A', {0.36f, 0.70f, 0.20f}},
    };
    for (const Face &face : faces)
    {
        Canvas canvas(22 * static_cast<int>(scale), 22 * static_cast<int>(scale));
        faceButton(canvas, labels, face.letter, face.color, s);
        blit(canvas, out, outWidth, face.x * static_cast<int>(scale), 20 * static_cast<int>(scale));
    }
    return true;
}

} // namespace rg::hdart
