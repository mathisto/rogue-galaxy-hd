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
                     std::vector<float> &mask, int w, int h) const
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
    float scaleX = scale;
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

} // namespace

bool drawXboxIcons(const Font &labels, uint32_t scale, const uint8_t *native, uint32_t width, uint32_t height,
                   std::vector<uint8_t> &out)
{
    if (width < 128u || height < 42u || scale == 0u)
        return false;
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
