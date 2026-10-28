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

struct GsReplacementShape {
    uint32_t width = 0u;
    uint32_t height = 0u;
};

struct GsReplacementPicture {
    std::shared_ptr<const GsReplacementImage> image;
    uint32_t scale = 1u;
    std::string label; // the file or generator, for the log
};

class GsReplacementStore {
public:
    static GsReplacementStore &instance();

    // Off: every texture is the game's (the cache drops what it bound at its
    // next use).
    void setEnabled(bool on);
    bool enabled() const;

    // The pack folder, scanned now (recursively) and again by rescan().
    void setImageLoader(GsReplacementImageLoader loader);
    void setPackDirectory(const std::string &directory);
    size_t rescan();
    size_t fileCount() const;

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
    // RG_TEXTURE_LOG: every lookup and its hash, for naming files.
    bool logging() const { return m_logging; }

    // For the cache: the sizes replacements of this format come in that fit
    // a width x height texture.
    std::vector<GsReplacementShape> shapes(uint32_t psm, uint32_t width, uint32_t height) const;
    // The picture for a texture, made or read now if need be; false when
    // there is none (or it failed, which is logged once).
    bool picture(const GsReplacementKey &key, uint32_t renderScale, const uint8_t *native,
                 GsReplacementPicture &out);

private:
    GsReplacementStore();
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    bool m_logging = false;
};

} // namespace dq8::gfx
