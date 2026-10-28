// Replacement textures: pictures that stand in for a guest texture, found by
// its content (Rogue Galaxy: added).
//
// GsTextureCache names every texture by the hash of its raw texels and of the
// palette window it reads (GsTextureIdentity). A replacement covers the
// top-left width x height texels of a texture of one format whose texels
// there hash to a value, seeded the way the cache seeds a whole texture's
// hash, so a replacement the size of the texture matches the cache's
// identity itself, and a smaller one matches a picture the game uploads into
// part of a larger TEX0 (Rogue Galaxy's 512x416 font pages, sampled as
// 512x512). The picture is an integer multiple of that size; the cache binds
// it with the multiple as the shader's physical-texels-per-GS-texel factor,
// so UVs, wrapping and alpha stay the guest's.
//
// Two sources:
// - files in a pack folder (textures\SCUS-97490\replacements), named
//   <texels>_<clut>_<W>x<H>_psm<NN>.png (or with '-' between the fields; a
//   clut of 16 zeros matches any palette), alpha 255 meaning opaque;
// - pictures the program makes (setGenerated), drawn at the render scale
//   from the texture's own texels, in the GS's alpha range (0x80 opaque).
// A file wins over a generated picture for the same texture.
//
// Packs can hold thousands of 4x pictures (gigabytes of RGBA), so files are
// handled in the background:
// - the folder is listed (names only) on a worker thread at start and on a
//   reload; nothing is decoded then;
// - a file is decoded on a worker the first time a texture asks for it;
//   picture() answers Pending until then and the game's texture is drawn
//   meanwhile, so the render thread never waits for a PNG;
// - decoded pictures stay in a least-recently-used cache of
//   cacheBudgetBytes() (half of the budget); the cache in the GPU texture
//   cache (GsTextureCache) holds the uploaded ones under the whole budget;
// - reload() lists the folder again (F8, the menu, or RG_TEXTURE_POLL);
//   pictures of files that changed are read again.
// Anything that fails (unreadable file, wrong size) leaves the game's texture
// and is logged once.
//
// Everything here is safe to call from any thread.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dq8::gfx {

struct GsReplacementImage {
    uint32_t width = 0u;
    uint32_t height = 0u;
    // RGBA8, alpha in the GS's range (0x80 opaque).
    std::vector<uint8_t> rgba;
    // Smaller levels, each half the one before (box filter), down to the
    // texture's own size; empty unless mipmaps are on and the scale is a
    // power of two.
    std::vector<std::vector<uint8_t>> mips;
    size_t bytes() const {
        size_t total = rgba.size();
        for (const auto &level : mips)
            total += level.size();
        return total;
    }
};

struct GsReplacementKey {
    uint64_t texels = 0u;
    uint64_t clut = 0u; // 0: any palette
    uint32_t psm = 0u;
    uint32_t width = 0u;
    uint32_t height = 0u;

    bool operator==(const GsReplacementKey &other) const = default;
};

// Makes the picture at `scale` times the key's size from the texture's own
// texels there (`native`, RGBA8 in the GS's alpha range, width x height of
// the key). False leaves the texture as the game has it.
using GsReplacementGenerator =
    std::function<bool(uint32_t scale, const uint8_t *native, uint32_t width, uint32_t height, GsReplacementImage &out)>;

// Reads a picture file to RGBA8 (alpha 255 opaque).
using GsReplacementImageLoader =
    std::function<bool(const std::string &path, uint32_t &width, uint32_t &height, std::vector<uint8_t> &rgba)>;
// Writes RGBA8 (alpha 255 opaque) as a PNG.
using GsReplacementImageWriter =
    std::function<bool(const std::string &path, uint32_t width, uint32_t height, const uint8_t *rgba)>;

struct GsReplacementShape {
    uint32_t width = 0u;
    uint32_t height = 0u;
};

struct GsReplacementPicture {
    std::shared_ptr<const GsReplacementImage> image;
    uint32_t scale = 1u;
    std::string label; // the file or generator, for the log
};

enum class GsPictureStatus {
    None,    // no replacement (or it failed)
    Pending, // a file being read; ask again once completions() moves
    Ready,
};

// How a soft-edged HD alpha is treated where the game's texture has only
// "transparent" and "opaque" texels (a cut-out: foliage, hair, grates).
enum class GsCutoutAlpha {
    Soft,      // as the file has it
    Threshold, // opaque above half, transparent below: the game's silhouettes
};

struct GsReplacementStats {
    size_t files = 0u;
    size_t decoded = 0u;     // pictures in the decoded cache
    size_t cacheBytes = 0u;  // their size
    size_t pending = 0u;     // files queued for the workers
    size_t failed = 0u;
    uint64_t reads = 0u;     // files read so far
    double readMs = 0.0;     // time the workers spent reading them
    uint64_t evicted = 0u;   // decoded pictures dropped for the budget
    uint64_t requests = 0u;  // lookups that found a file
    uint64_t misses = 0u;    // lookups that found none
    uint64_t learned = 0u;   // textures written by learn mode
};

class GsReplacementStore {
public:
    static GsReplacementStore &instance();
    ~GsReplacementStore();

    // Off: every texture is the game's (the cache drops what it bound at its
    // next use).
    void setEnabled(bool on);
    bool enabled() const;
    // The pack's files alone (the generated pictures stay): for comparing
    // with and without a pack in one session.
    void setFilesEnabled(bool on);

    // The pack folder, listed now on a worker (recursively) and again by
    // reload().
    void setImageLoader(GsReplacementImageLoader loader);
    void setImageWriter(GsReplacementImageWriter writer);
    void setPackDirectory(const std::string &directory);
    std::string packDirectory() const;
    // Lists the folder again, on a worker; pictures of changed files are
    // read again at their next use.
    void reload();
    size_t fileCount() const;

    // The memory the pack may take: the GPU texture cache's replacement
    // textures stay under it, and decoded pictures waiting in memory under
    // half of it. RG_TEXTURE_BUDGET_MB, the menu's "Texture pack memory".
    void setBudgetMegabytes(uint32_t megabytes);
    uint64_t budgetBytes() const;
    uint64_t cacheBudgetBytes() const;
    // Generated smaller levels for HD pictures (RG_TEXTURE_MIPMAPS=0 off).
    bool mipmaps() const;
    void setMipmaps(bool on);
    GsCutoutAlpha cutoutAlpha() const;
    void setCutoutAlpha(GsCutoutAlpha mode);

    // A picture the program draws, for the textures `keys` names. `maxScale`
    // bounds the scale it is drawn at (the render scale otherwise). Setting a
    // name again replaces it.
    void setGenerated(const std::string &name, std::vector<GsReplacementKey> keys, GsReplacementGenerator generator,
                      uint32_t maxScale = 4u);
    void removeGenerated(const std::string &name);
    // Its art changed: drawn again at the next use.
    void refreshGenerated(const std::string &name);

    // Bumped by every change above; the cache looks its textures up again
    // when it moves.
    uint64_t generation() const;
    // Bumped each time a worker finishes reading a file: a texture waiting
    // for one asks again when it moves.
    uint64_t completions() const;
    // RG_TEXTURE_LOG: every lookup and its hash, for naming files.
    bool logging() const { return m_logging; }

    // For the cache: the sizes replacements of this format come in that fit
    // a width x height texture, the largest first.
    std::vector<GsReplacementShape> shapes(uint32_t psm, uint32_t width, uint32_t height) const;
    // Whether a file (for this palette or any) or a generated picture
    // stands for the key: a map probe, before the cache expands anything.
    bool has(const GsReplacementKey &key) const;
    // The picture for a texture. Files are read on a worker: Pending until
    // then. `cutout`: the game's texels have only transparent and opaque
    // alpha (for GsCutoutAlpha::Threshold).
    GsPictureStatus picture(const GsReplacementKey &key, uint32_t renderScale, const uint8_t *native,
                            GsReplacementPicture &out, bool cutout = false);

    // Learn mode (RG_TEXTURE_LEARN=dir): the cache hands every texture with
    // no replacement here, once per name; it is written as a PNG (alpha 255
    // opaque) on a worker, for tools/rg_texlearn.py to match against the
    // disc's pictures.
    bool learning() const { return m_learning; }
    void learn(const GsReplacementKey &key, uint32_t tbp0, std::vector<uint8_t> rgba);

    // The cache's replacement textures on the GPU, for the stats line.
    void noteGpu(uint64_t bytes, uint64_t textures);
    GsReplacementStats stats() const;
    // One line for the log: files, read, waiting, memory.
    std::string summary() const;

private:
    GsReplacementStore();
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    bool m_logging = false;
    bool m_learning = false;
};

} // namespace dq8::gfx
