#include "gfx/backends/sdlgpu/sdlgpu_textures.h"

#include "gfx/backends/sdlgpu/sdlgpu_device.h"
#include "gfx/backends/sdlgpu/sdlgpu_targets.h"
#include "runtime/gs/ps2_gs_memory.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dq8::gfx {

namespace {

// Beyond this the cache is cleared wholesale rather than evicting cleverly.
// A GS scene addresses far fewer distinct textures than this; the cap exists
// so a pathological stream cannot exhaust GPU memory.
constexpr size_t kMaxEntries = 2048u;

uint32_t rgba5551To8888(uint32_t value) {
    const uint32_t r = (value & 0x1fu) << 3u;
    const uint32_t g = ((value >> 5u) & 0x1fu) << 3u;
    const uint32_t b = ((value >> 10u) & 0x1fu) << 3u;
    const uint32_t a = ((value >> 15u) & 0x01u) << 7u;
    return r | (g << 8u) | (b << 16u) | (a << 24u);
}

struct TexaState {
    uint8_t ta0 = 0u;
    bool aem = false;
    uint8_t ta1 = 0u;
};

TexaState unpackTexa(uint32_t packed) {
    return {static_cast<uint8_t>(packed & 0xffu),
            ((packed >> 8u) & 0x1u) != 0u,
            static_cast<uint8_t>((packed >> 16u) & 0xffu)};
}

// TEXA supplies the alpha a format has no room for: CT24 has none at all, and
// CT16's single bit selects between TA0 and TA1. AEM additionally forces alpha
// to zero when RGB is black, which is how a colour-keyed texture is done.
uint32_t applyTexa(const TexaState &texa, uint32_t psm, uint32_t texel) {
    if ((psm & 0x3fu) == GS_PSM_CT32)
        return texel;

    const bool rgbZero = (texel & 0x00ffffffu) == 0u;
    uint8_t alpha = static_cast<uint8_t>((texel >> 24u) & 0xffu);
    switch (psm & 0x3fu) {
    case GS_PSM_CT24:
        alpha = (texa.aem && rgbZero) ? 0u : texa.ta0;
        break;
    case GS_PSM_CT16:
    case GS_PSM_CT16S:
        if ((alpha & 0x80u) != 0u)
            alpha = texa.ta1;
        else
            alpha = (texa.aem && rgbZero) ? 0u : texa.ta0;
        break;
    default:
        break;
    }
    return (texel & 0x00ffffffu) | (static_cast<uint32_t>(alpha) << 24u);
}

// CSM1 swaps address bits 3 and 4 of the palette index; a 16-bit CLUT exposes
// a ninth bit through CSA[4].
uint32_t swizzleClutIndexCsm1(uint32_t index) {
    return (index & ~0x18u) | ((index & 0x08u) << 1u) | ((index & 0x10u) >> 1u);
}

uint32_t resolveClutIndex(uint32_t index, uint32_t cpsm, uint32_t csm, uint32_t csa,
                          uint32_t sourcePsm) {
    const bool fourBitSource = (sourcePsm & 0x3fu) == GS_PSM_T4 ||
                               (sourcePsm & 0x3fu) == GS_PSM_T4HH ||
                               (sourcePsm & 0x3fu) == GS_PSM_T4HL;
    // CSM2 addresses the palette directly through TEXCLUT, and CSA must be
    // zero there, so it does not offset anything.
    if (csm != 0u)
        return fourBitSource ? (index & 0x0fu) : index;

    const bool sixteenBitClut = cpsm == GS_PSM_CT16 || cpsm == GS_PSM_CT16S;
    const uint32_t csaMask = sixteenBitClut ? 0x1fu : 0x0fu;
    const uint32_t indexMask = sixteenBitClut ? 0x1ffu : 0x0ffu;
    const uint32_t base = (csa & csaMask) << 4u;

    uint32_t resolved = index;
    switch (sourcePsm & 0x3fu) {
    case GS_PSM_T4:
    case GS_PSM_T4HH:
    case GS_PSM_T4HL:
        resolved = base + (index & 0x0fu);
        break;
    case GS_PSM_T8:
    case GS_PSM_T8H:
        resolved = base + index;
        break;
    default:
        return index;
    }
    return swizzleClutIndexCsm1(resolved & indexMask);
}

// Writes an expanded texture as a PPM next to a one-line description, when
// DQ8_GFX_DUMP_TEXTURES names a directory. Expansion is where PSM, CLUT and
// TEXA all meet, so "is the texture wrong or is the shader wrong" is the first
// question every texturing bug asks.
void dumpTexture(const GsTextureKey &key,
                 const GsTextureIdentity &identity,
                 const std::vector<uint8_t> &rgba,
                 uint64_t index) {
    const char *directory = std::getenv("DQ8_GFX_DUMP_TEXTURES");
    if (!directory || *directory == '\0')
        return;

    // Named by identity, not by address: that is the name a replacement pack
    // would use, so a dump doubles as the pack's index.
    char path[512];
    std::snprintf(path, sizeof(path), "%s/%016llx_%016llx_%ux%u_psm%02x.ppm", directory,
                  static_cast<unsigned long long>(identity.texels),
                  static_cast<unsigned long long>(identity.clut), key.width, key.height,
                  key.psm);
    std::FILE *file = std::fopen(path, "wb");
    if (!file)
        return;
    std::fprintf(file, "P6\n%u %u\n255\n", key.width, key.height);
    for (size_t i = 0u; i + 3u < rgba.size(); i += 4u)
        std::fwrite(rgba.data() + i, 1u, 3u, file);
    std::fclose(file);

    uint64_t opaque = 0u;
    uint64_t nonBlack = 0u;
    for (size_t i = 0u; i + 3u < rgba.size(); i += 4u) {
        if (rgba[i] != 0u || rgba[i + 1u] != 0u || rgba[i + 2u] != 0u)
            ++nonBlack;
        if (rgba[i + 3u] != 0u)
            ++opaque;
    }
    std::fprintf(stderr,
                 "[texture %llu] id=%016llx/%016llx tbp0=%04x psm=%02x %ux%u cbp=%04x "
                 "cpsm=%02x csa=%u non-black=%llu alpha>0=%llu\n",
                 static_cast<unsigned long long>(index),
                 static_cast<unsigned long long>(identity.texels),
                 static_cast<unsigned long long>(identity.clut), key.tbp0, key.psm,
                 key.width, key.height, key.cbp, key.cpsm, key.csa,
                 static_cast<unsigned long long>(nonBlack),
                 static_cast<unsigned long long>(opaque));
}

// FNV-1a. Not cryptographic, and does not need to be: a texture pack holds
// thousands of entries, not billions.
constexpr uint64_t kHashSeed = 1469598103934665603ull;

uint64_t hashStep(uint64_t hash, uint64_t value) {
    for (int byte = 0; byte < 8; ++byte) {
        hash ^= (value >> (byte * 8)) & 0xffull;
        hash *= 1099511628211ull;
    }
    return hash;
}

// The same FNV-1a over a byte string, one machine word at a time. The
// per-pixel hashStep it replaces cost eight multiply-xor rounds per texel,
// which at 512x448 was a third of what expansion cost.
uint64_t hashBytes(uint64_t hash, const uint8_t *data, size_t size) {
    size_t i = 0;
    for (; i + 8u <= size; i += 8u) {
        uint64_t word;
        std::memcpy(&word, data + i, sizeof(word));
        hash = (hash ^ word) * 1099511628211ull;
    }
    for (; i < size; ++i)
        hash = (hash ^ data[i]) * 1099511628211ull;
    return hash;
}

} // namespace

size_t GsTextureKeyHash::operator()(const GsTextureKey &key) const {
    uint64_t hash = 1469598103934665603ull;
    auto mix = [&hash](uint64_t value) {
        hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
    };
    mix(key.tbp0);
    mix(static_cast<uint64_t>(key.tbw) | (static_cast<uint64_t>(key.psm) << 16u) |
        (static_cast<uint64_t>(key.width) << 24u) |
        (static_cast<uint64_t>(key.height) << 40u));
    mix(key.cbp);
    mix(static_cast<uint64_t>(key.cpsm) | (static_cast<uint64_t>(key.csm) << 8u) |
        (static_cast<uint64_t>(key.csa) << 16u));
    mix(key.texa);
    mix(key.texclut);
    return static_cast<size_t>(hash);
}

GsTextureCache::GsTextureCache(SdlGpuDevice &device, GsVram &vram, GsTargetCache &targets)
    : m_device(device), m_vram(vram), m_targets(targets) {
    // RG_TEXTURE_UPLOAD_MB: replacement bytes uploaded per 16 ms at most.
    if (const char *value = std::getenv("RG_TEXTURE_UPLOAD_MB"); value && std::atoi(value) > 0)
        m_hdUploadAllowance = static_cast<uint64_t>(std::atoi(value)) << 20u;
}

GsTextureCache::~GsTextureCache() {
    reset();
    releasePooled(0u);
}

void GsTextureCache::reset() {
    if (!m_entries.empty())
        beforeRewrite(nullptr);
    if (m_device.valid()) {
        for (auto &[key, entry] : m_entries) {
            (void)key;
            if (entry.texture)
                SDL_ReleaseGPUTexture(m_device.handle(), entry.texture);
            if (entry.hd && !entry.hdPooled)
                SDL_ReleaseGPUTexture(m_device.handle(), entry.hd);
        }
    }
    m_entries.clear();
    // Pooled pictures outlive the entries (the cache clears itself when
    // full; the pictures are still good).
    m_hdBytes = 0u;
    m_hdTextures = 0u;
    for (auto &[poolKey, pooled] : m_hdPool) {
        pooled.refs = 0u;
        m_hdBytes += pooled.bytes;
        ++m_hdTextures;
    }
    GsReplacementStore::instance().noteGpu(m_hdBytes, m_hdTextures);
    m_sourcePagesAll.reset();
    for (auto &entries : m_entriesByPage)
        entries.clear();
    releaseUploadPool();
}

void GsTextureCache::invalidate(const GsPageSet &pages, InvalidationSource source) {
    (void)source;
    // Draws mostly land in pages no texture reads; the walk below, over up
    // to 2048 entries, ran for every flush and every host write.
    const GsPageSet hit = m_sourcePagesAll & pages;
    if (hit.none())
        return;
    // Mapping pages to texels belongs at consumption, not at each of the
    // movie's 896 tiny writes. Bitsets merge those writes without a page walk.
    // Only the entries over the written pages, each once (Rogue Galaxy:
    // added; the walk over every entry is kept in the comment above the
    // index).
    const uint64_t generation = ++m_invalidateGeneration;
    gsForEachPage(hit, [&](uint32_t page) {
        for (Entry *entry : m_entriesByPage[page]) {
            if (entry->invalidated == generation)
                continue;
            entry->invalidated = generation;
            entry->dirtyPages |= entry->texelPages & pages;
            entry->clutDirty = entry->clutDirty || (entry->clutPages & pages).any();
        }
    });
}

void GsTextureCache::markTexelPages(const GsTextureKey &key, GsPageSet &pages) const {
    gsMarkPages(pages, key.tbp0, key.tbw, key.psm, key.width, key.height);
}

void GsTextureCache::markClutPages(const GsTextureKey &key, GsPageSet &pages) const {
    if (!gsIsIndexedPsm(key.psm))
        return;

    // The palette is a separate region and just as invalidatable: a game that
    // animates a CLUT rewrites it every frame while the indices stay put.
    const bool sixteenBitClut = key.cpsm == GS_PSM_CT16 || key.cpsm == GS_PSM_CT16S;
    const uint32_t entries = (key.csm == 0u && sixteenBitClut) ? 512u : 256u;
    const uint32_t clutWidth = (key.texclut & 0xffu) + 16u;
    const uint32_t clutHeight = ((key.texclut >> 16u) & 0xffffu) + (entries + 15u) / 16u;
    gsMarkPages(pages, key.cbp, std::max<uint32_t>(key.texclut & 0xffu, 1u), key.cpsm,
                clutWidth, clutHeight);
}

void GsTextureCache::markSourcePages(const GsTextureKey &key, GsPageSet &pages) const {
    markTexelPages(key, pages);
    markClutPages(key, pages);
}

bool GsTextureCache::evictionImminent() const {
    return m_entries.size() >= kMaxEntries;
}

GsPageSet GsTextureCache::sourcePagesFor(const GSDrawState &state) const {
    // The pages depend on the key alone, so the memo never goes stale.
    const GsTextureKey key = keyFor(state);
    if (!m_lastSourceValid || !(key == m_lastSourceKey)) {
        m_lastSourcePages.reset();
        markSourcePages(key, m_lastSourcePages);
        m_lastSourceKey = key;
        m_lastSourceValid = true;
    }
    return m_lastSourcePages;
}

GsTextureKey GsTextureCache::keyFor(const GSDrawState &state) {
    const GSTex0Reg &tex0 = state.context.tex0;

    GsTextureKey key{};
    key.tbp0 = tex0.tbp0;
    key.tbw = std::max<uint32_t>(tex0.tbw, 1u);
    key.psm = tex0.psm;
    key.width = std::max<uint32_t>(state.textureWidth, 1u);
    key.height = std::max<uint32_t>(state.textureHeight, 1u);
    key.cbp = tex0.cbp;
    key.cpsm = tex0.cpsm;
    key.csm = tex0.csm;
    key.csa = tex0.csa;
    key.texa = static_cast<uint32_t>(state.texa.ta0) |
               (state.texa.aem ? 0x100u : 0u) |
               (static_cast<uint32_t>(state.texa.ta1) << 16u);
    key.texclut = static_cast<uint32_t>(state.texclut.cbw) |
                  (static_cast<uint32_t>(state.texclut.cou) << 8u) |
                  (static_cast<uint32_t>(state.texclut.cov) << 16u);
    return key;
}

SDL_GPUTexture *GsTextureCache::acquire(const GSDrawState &state, std::string &error, const GsRegion *sampled,
                                        uint32_t *scale, uint32_t *levels) {
    const GsTextureKey key = keyFor(state);
    if (scale)
        *scale = 1u;
    if (levels)
        *levels = 1u;
    const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
    const auto useHd = [&](Entry &entry) {
        entry.hdUse = ++m_useTick;
        entry.hdUseTime = nowUs;
        if (scale)
            *scale = entry.hdScale;
        if (levels)
            *levels = entry.hdLevels;
        return entry.hd;
    };

    ++m_stats.lookups;
    if (const uint64_t generation = GsReplacementStore::instance().generation(); generation != m_poolGeneration) {
        m_poolGeneration = generation;
        flushPool();
    }
    if (auto found = m_entries.find(key); found != m_entries.end()) {
        ++m_stats.hits;
        Entry &entry = found->second;
        if (entry.dirtyPages.any()) {
            entry.dirty.merge(regionForPages(key, entry.dirtyPages));
            entry.dirtyPages.reset();
        }
        if (entry.clutDirty) {
            if (!resolveSource(key, {}, error)) return nullptr;
            refreshClut(key, entry);
        }
        // Texels never read back join the stale ones once a draw reaches
        // them, and stale texels wait while no draw does.
        if (!entry.unresolved.empty() && (!sampled || sampled->intersects(entry.unresolved))) {
            entry.dirty.merge(entry.unresolved);
            entry.unresolved.clear();
        }
        if (!entry.dirty.empty() && (!sampled || entry.dirty.intersects(*sampled)) &&
            !update(key, entry, error, sampled))
            return nullptr;
        // The store changed (a pack read, art redrawn, replacements turned
        // off) or the render scale did: this texture's picture may too.
        if (entry.hdStamp != replacementStamp())
            matchReplacement(key, entry);
        else if (entry.hdWait && GsReplacementStore::instance().completions() != entry.hdWaitSeen)
            retryWait(key, entry);
        else if (entry.hdRetryAt != 0 && nowUs >= entry.hdRetryAt) {
            entry.hdRetryAt = 0;
            matchReplacement(key, entry);
        }
        if (entry.hd)
            return useHd(entry);
        return entry.texture;
    }

    if (m_entries.size() >= kMaxEntries) {
        reset();
        ++m_stats.evictions;
    }

    Entry entry{};
    if (!build(key, entry, error, sampled))
        return nullptr;
    markSourcePages(key, entry.pages);
    markTexelPages(key, entry.texelPages);
    markClutPages(key, entry.clutPages);
    m_sourcePagesAll |= entry.pages;
    Entry &stored = m_entries.emplace(key, std::move(entry)).first->second;
    gsForEachPage(stored.pages, [&](uint32_t page) { m_entriesByPage[page].push_back(&stored); });
    if (GsReplacementStore::instance().logging())
        std::fprintf(stderr, "[texrep] texture tbp0=%05x tbw=%u psm=%02x %ux%u built %016llx_%016llx\n", key.tbp0,
                     key.tbw, key.psm, key.width, key.height, static_cast<unsigned long long>(stored.identity.texels),
                     static_cast<unsigned long long>(stored.identity.clut));
    matchReplacement(key, stored);
    if (stored.hd)
        return useHd(stored);
    return stored.texture;
}

// The stamp a lookup holds under: the store's generation and the render
// scale, which generated pictures are drawn at.
uint64_t GsTextureCache::replacementStamp() const {
    return GsReplacementStore::instance().generation() * 16u + std::min<uint32_t>(m_targets.scale(), 15u);
}

void GsTextureCache::dropReplacement(Entry &entry) {
    entry.hdWait = false;
    entry.hdWaitAtlas = false;
    entry.hdComposed = false;
    entry.hdAtlas.reset();
    entry.hdSources.clear();
    entry.hdLast.clear();
    entry.hdLast.shrink_to_fit();
    entry.hdLastTexels = 0u;
    entry.hdAtlasClut = 0u;
    entry.hdImage.clear();
    entry.hdImage.shrink_to_fit();
    entry.hdNoVariant.clear();
    if (entry.hdMain) {
        // A composed picture and its variants are the entry's own; `hd`
        // points at one of them.
        std::vector<SDL_GPUTexture *> owned{entry.hdMain};
        for (const Entry::Variant &variant : entry.hdVariants)
            owned.push_back(variant.texture);
        for (SDL_GPUTexture *texture : owned) {
            beforeRewrite(texture);
            if (m_device.valid())
                SDL_ReleaseGPUTexture(m_device.handle(), texture);
        }
        m_hdBytes -= std::min(m_hdBytes, entry.hdBytes);
        m_hdTextures -= std::min<uint64_t>(m_hdTextures, owned.size());
        entry.hdMain = nullptr;
        entry.hdVariants.clear();
        entry.hd = nullptr;
        entry.hdScale = 1u;
        entry.hdRegion = {};
        entry.hdHash = 0u;
        entry.hdBytes = 0u;
        entry.hdLevels = 1u;
        GsReplacementStore::instance().noteGpu(m_hdBytes, m_hdTextures);
        ++m_stats.replacementsDropped;
        return;
    }
    entry.hdVariants.clear();
    if (!entry.hd)
        return;
    if (entry.hdPooled) {
        // Shared: stays in the pool for the next entry that wants it.
        if (entry.hdPool && entry.hdPool->refs)
            --entry.hdPool->refs;
        entry.hdPooled = false;
        entry.hdPool = nullptr;
        entry.hd = nullptr;
        entry.hdScale = 1u;
        entry.hdRegion = {};
        entry.hdHash = 0u;
        entry.hdBytes = 0u;
        entry.hdLevels = 1u;
        ++m_stats.replacementsDropped;
        return;
    }
    beforeRewrite(entry.hd);
    if (m_device.valid())
        SDL_ReleaseGPUTexture(m_device.handle(), entry.hd);
    entry.hd = nullptr;
    entry.hdScale = 1u;
    entry.hdRegion = {};
    entry.hdHash = 0u;
    m_hdBytes -= std::min(m_hdBytes, entry.hdBytes);
    m_hdTextures -= m_hdTextures ? 1u : 0u;
    entry.hdBytes = 0u;
    entry.hdLevels = 1u;
    GsReplacementStore::instance().noteGpu(m_hdBytes, m_hdTextures);
    ++m_stats.replacementsDropped;
}

namespace {

// Only fully transparent and (at least) opaque texels, some of each: a
// cut-out, whose HD alpha the store may harden to the same silhouettes.
bool isCutout(const std::vector<uint8_t> &rgba) {
    bool clear = false, solid = false;
    for (size_t i = 3u; i < rgba.size(); i += 4u) {
        const uint8_t a = rgba[i];
        if (a == 0u)
            clear = true;
        else if (a >= 0x7Cu) // many of the game's opaque texels are 0x7E or 0x7F (the minimaps)
            solid = true;
        else
            return false;
    }
    return clear && solid;
}

} // namespace

// Hashes the texture's top-left parts the store has replacements of that
// size for, and binds the first one found: the game's texels upscaled
// (nearest) with the picture over its part. A texture whose texels are not
// all known (built on the GPU from a target, or with unresolved target
// pages in the part) is left alone. A file the store is still reading
// leaves the game's texture until it is ready (retryWait).
void GsTextureCache::matchReplacement(const GsTextureKey &key, Entry &entry) {
    GsReplacementStore &store = GsReplacementStore::instance();
    entry.hdStamp = replacementStamp();
    entry.hdWait = false;
    const std::vector<GsReplacementShape> shapes = store.shapes(key.psm, key.width, key.height);
    if (shapes.empty() || !entry.texture || (entry.renderable && entry.identity.texels == 0u)) {
        if (matchAtlas(key, entry))
            return;
        dropReplacement(entry);
        learnTexture(key, entry);
        return;
    }
    const uint64_t clut = gsIsIndexedPsm(key.psm) ? (entry.identity.clut ? entry.identity.clut : hashClutWindow(key))
                                                  : 0u;
    const auto powerOfTwo = [](uint32_t v) { return v != 0u && (v & (v - 1u)) == 0u; };
    for (const GsReplacementShape &shape : shapes) {
        const GsRegion part{0u, 0u, shape.width, shape.height};
        if (!entry.unresolved.empty() && entry.unresolved.intersects(part))
            continue;
        const bool whole = shape.width == key.width && shape.height == key.height;
        // A part is a picture the game uploads into a larger power-of-two
        // TEX0: along each axis where it is smaller, its size is not a power
        // of two (512x416 in 512x512, 288x224 in 512x256). Hashing every
        // smaller power-of-two corner of every texture each time its texels
        // change cost more than the game's own drawing.
        if (!whole && ((shape.width != key.width && powerOfTwo(shape.width)) ||
                       (shape.height != key.height && powerOfTwo(shape.height))))
            continue;
        const uint64_t texels = whole && entry.identity.texels != 0u
                                    ? entry.identity.texels
                                    : expand(key, part, true, shape.width, shape.height);
        if (texels == 0u)
            continue;
        const GsReplacementKey wanted{texels, clut, key.psm, shape.width, shape.height};
        if (store.logging())
            std::fprintf(stderr, "[texrep] lookup tbp0=%05x psm=%02x %ux%u part %ux%u: %016llx_%016llx\n", key.tbp0,
                         key.psm, key.width, key.height, shape.width, shape.height,
                         static_cast<unsigned long long>(texels), static_cast<unsigned long long>(clut));
        if (!store.has(wanted))
            continue;
        // Already on the GPU for another entry, or for this one a moment ago.
        if (whole) {
            if (auto pooled = m_hdPool.find(wanted); pooled != m_hdPool.end() && pooled->second.texture) {
                attachPooled(entry, pooled->second, part, texels);
                return;
            }
        }
        // The part's own texels, for a generator and to tell a cut-out.
        expand(key, part);
        const bool cutout = isCutout(m_staging);
        const std::vector<uint8_t> native = m_staging;
        GsReplacementPicture picture;
        const uint64_t seen = store.completions();
        const GsPictureStatus status = store.picture(wanted, m_targets.scale(), native.data(), picture, cutout);
        if (status == GsPictureStatus::None)
            continue;
        if (status == GsPictureStatus::Pending) {
            // The old picture belongs to other texels: the game's texture
            // until the new one is read.
            if (entry.hd && entry.hdHash != texels)
                dropReplacement(entry);
            entry.hdWait = true;
            entry.hdWaitKey = wanted;
            entry.hdWaitPart = part;
            entry.hdWaitSeen = seen;
            return;
        }
        const BindResult bound = bindPicture(key, entry, part, picture, wanted);
        if (bound == BindResult::Bound)
            return;
        if (bound == BindResult::Deferred) {
            if (entry.hd && entry.hdHash != texels)
                dropReplacement(entry);
            entry.hdWait = true;
            entry.hdWaitKey = wanted;
            entry.hdWaitPart = part;
            entry.hdWaitSeen = 0u; // asks again at the next use
            return;
        }
    }
    if (matchAtlas(key, entry))
        return;
    dropReplacement(entry);
    learnTexture(key, entry);
}

void GsTextureCache::retryWait(const GsTextureKey &key, Entry &entry) {
    GsReplacementStore &store = GsReplacementStore::instance();
    if (entry.hdWaitAtlas) {
        entry.hdWait = false;
        entry.hdWaitAtlas = false;
        if (!matchAtlas(key, entry))
            dropReplacement(entry);
        return;
    }
    // The part must still hold the texels it was asked for.
    if (!entry.unresolved.empty() && entry.unresolved.intersects(entry.hdWaitPart)) {
        entry.hdWait = false;
        return;
    }
    const uint64_t seen = store.completions();
    GsReplacementPicture picture;
    // The part's own texels again, for a picture drawn over the file's.
    std::vector<uint8_t> native;
    if (store.drawsOver(entry.hdWaitKey)) {
        expand(key, entry.hdWaitPart);
        native = m_staging;
    }
    const GsPictureStatus status =
        store.picture(entry.hdWaitKey, m_targets.scale(), native.empty() ? nullptr : native.data(), picture);
    if (status == GsPictureStatus::Pending) {
        entry.hdWaitSeen = seen;
        return;
    }
    if (status == GsPictureStatus::None) {
        // The file failed: anything else (a smaller part, a drawn picture).
        matchReplacement(key, entry);
        return;
    }
    const GsRegion part = entry.hdWaitPart;
    const GsReplacementKey wanted = entry.hdWaitKey;
    const BindResult bound = bindPicture(key, entry, part, picture, wanted);
    if (bound == BindResult::Deferred) {
        entry.hdWaitSeen = 0u;
        return;
    }
    entry.hdWait = false;
    if (bound == BindResult::Failed)
        matchReplacement(key, entry);
}

GsTextureCache::BindResult GsTextureCache::bindPicture(const GsTextureKey &key, Entry &entry, const GsRegion &part,
                                                       const GsReplacementPicture &picture,
                                                       const GsReplacementKey &wanted) {
    const uint64_t texels = wanted.texels;
    const uint32_t s = picture.scale;
    const uint64_t hdWidth = static_cast<uint64_t>(key.width) * s;
    const uint64_t hdHeight = static_cast<uint64_t>(key.height) * s;
    if (hdWidth > 8192u || hdHeight > 8192u || hdWidth * hdHeight * 4u > (256ull << 20u)) {
        std::fprintf(stderr, "[texrep] %s: %llux%llu is too large; the game's texture stays\n",
                     picture.label.c_str(), static_cast<unsigned long long>(hdWidth),
                     static_cast<unsigned long long>(hdHeight));
        return BindResult::Failed;
    }
    const bool whole = part.width() == key.width && part.height() == key.height;
    const GsReplacementImage &image = *picture.image;
    const uint32_t levels = whole ? 1u + static_cast<uint32_t>(image.mips.size()) : 1u;
    const uint64_t bytes = whole ? image.bytes() : hdWidth * hdHeight * 4u;

    // No room under the budget: the game's texture for a few seconds more.
    if (!makeRoom(bytes, &entry)) {
        entry.hdRetryAt = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count() +
                          5000000;
        ++m_hdDenied;
        if (GsReplacementStore::instance().logging())
            std::fprintf(stderr, "[texrep] budget full: tbp0=%05x stays the game's for now\n", key.tbp0);
        return BindResult::Failed;
    }

    // A scene's first frame can ask for hundreds of pictures: spread their
    // uploads over frames (the game's texture is drawn until then).
    const int64_t now = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    if (now - m_hdWindowStart > 16000) {
        m_hdWindowStart = now;
        m_hdWindowBytes = 0u;
    }
    if (m_hdWindowBytes != 0u && m_hdWindowBytes + bytes > m_hdUploadAllowance)
        return BindResult::Deferred;
    m_hdWindowBytes += bytes;

    std::vector<uint8_t> composed;
    if (!whole) {
        expand(key, {0u, 0u, key.width, key.height});
        composed.resize(static_cast<size_t>(hdWidth * hdHeight * 4u));
        for (uint32_t y = 0u; y < hdHeight; ++y) {
            uint8_t *row = composed.data() + static_cast<size_t>(y) * hdWidth * 4u;
            const uint32_t sourceY = y / s;
            if (sourceY < part.height())
                std::memcpy(row, image.rgba.data() + static_cast<size_t>(y) * part.width() * s * 4u,
                            static_cast<size_t>(part.width()) * s * 4u);
            const uint32_t firstX = sourceY < part.height() ? part.width() : 0u;
            const uint8_t *source = m_staging.data() + static_cast<size_t>(sourceY) * key.width * 4u;
            for (uint32_t x = firstX * s; x < hdWidth; ++x)
                std::memcpy(row + static_cast<size_t>(x) * 4u, source + static_cast<size_t>(x / s) * 4u, 4u);
        }
    }

    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.width = static_cast<uint32_t>(hdWidth);
    info.height = static_cast<uint32_t>(hdHeight);
    info.layer_count_or_depth = 1u;
    info.num_levels = levels;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    SDL_GPUTexture *texture = SDL_CreateGPUTexture(m_device.handle(), &info);
    std::string error;
    if (!texture) {
        std::fprintf(stderr, "[texrep] SDL_CreateGPUTexture: %s; the game's texture stays\n", SDL_GetError());
        return BindResult::Failed;
    }
    bool ok = uploadRect(texture, 0u, 0u, info.width, info.height, whole ? image.rgba.data() : composed.data(), error);
    for (uint32_t level = 1u; ok && level < levels; ++level)
        ok = uploadRect(texture, 0u, 0u, info.width >> level, info.height >> level, image.mips[level - 1u].data(),
                        error, level);
    if (!ok) {
        std::fprintf(stderr, "[texrep] upload: %s; the game's texture stays\n", error.c_str());
        SDL_ReleaseGPUTexture(m_device.handle(), texture);
        return BindResult::Failed;
    }
    m_hdBytes += bytes;
    ++m_hdTextures;
    ++m_stats.replacements;
    if (whole) {
        PooledHd &pooled = m_hdPool[wanted];
        if (pooled.texture) {
            // A picture nobody showed any more, read again.
            for (auto &[otherKey, other] : m_entries)
                if (other.hdPooled && other.hdPool == &pooled)
                    dropReplacement(other);
            beforeRewrite(pooled.texture);
            SDL_ReleaseGPUTexture(m_device.handle(), pooled.texture);
            m_hdBytes -= std::min(m_hdBytes, pooled.bytes);
            --m_hdTextures;
        }
        pooled = {texture, bytes, s, levels, 0u, 0u};
        attachPooled(entry, pooled, part, texels);
    } else {
        dropReplacement(entry);
        entry.hd = texture;
        entry.hdScale = s;
        entry.hdRegion = part;
        entry.hdHash = texels;
        entry.hdBytes = bytes;
        entry.hdLevels = levels;
        entry.hdUse = ++m_useTick;
        entry.hdUseTime = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
    }
    if (GsReplacementStore::instance().logging())
        std::fprintf(stderr, "[texrep] tbp0=%05x psm=%02x %ux%u: %s at %ux over %ux%u%s\n", key.tbp0, key.psm,
                     key.width, key.height, picture.label.c_str(), s, part.width(), part.height(),
                     levels > 1u ? " (mipmapped)" : "");
    GsReplacementStore::instance().noteGpu(m_hdBytes, m_hdTextures);
    return BindResult::Bound;
}

void GsTextureCache::attachPooled(Entry &entry, PooledHd &pooled, const GsRegion &part, uint64_t texels) {
    if (entry.hdPooled && entry.hdPool == &pooled) {
        entry.hdHash = texels;
        entry.hdWait = false;
        return;
    }
    dropReplacement(entry);
    entry.hd = pooled.texture;
    entry.hdPooled = true;
    entry.hdPool = &pooled;
    entry.hdScale = pooled.scale;
    entry.hdLevels = pooled.levels;
    entry.hdRegion = part;
    entry.hdHash = texels;
    entry.hdBytes = 0u;
    entry.hdUse = ++m_useTick;
        entry.hdUseTime = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
    ++pooled.refs;
    pooled.use = m_useTick;
}

void GsTextureCache::releasePooled(uint64_t target) {
    std::vector<std::pair<uint64_t, GsReplacementKey>> idle;
    for (auto &[poolKey, pooled] : m_hdPool)
        if (pooled.refs == 0u || target == 0u)
            idle.push_back({pooled.use, poolKey});
    std::sort(idle.begin(), idle.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    for (const auto &[use, poolKey] : idle) {
        if (target != 0u && m_hdBytes <= target)
            break;
        auto found = m_hdPool.find(poolKey);
        if (found->second.texture) {
            beforeRewrite(found->second.texture);
            if (m_device.valid())
                SDL_ReleaseGPUTexture(m_device.handle(), found->second.texture);
        }
        m_hdBytes -= std::min(m_hdBytes, found->second.bytes);
        m_hdTextures -= m_hdTextures ? 1u : 0u;
        m_hdPool.erase(found);
    }
    GsReplacementStore::instance().noteGpu(m_hdBytes, m_hdTextures);
}

void GsTextureCache::flushPool() {
    if (m_hdPool.empty())
        return;
    for (auto &[key, entry] : m_entries)
        if (entry.hdPooled) {
            dropReplacement(entry);
            entry.hdStamp = 0u;
        }
    releasePooled(0u);
}

bool GsTextureCache::makeRoom(uint64_t incoming, const Entry *keep) {
    const uint64_t budget = GsReplacementStore::instance().budgetBytes();
    if (incoming > budget)
        return false;
    if (m_hdBytes + incoming <= budget)
        return true;
    // Down to an eighth below the budget, so the next pictures fit too.
    const uint64_t target = budget - std::min(budget, incoming + budget / 8u);
    // Pictures nobody shows first, then those of entries not drawn for two
    // seconds, least recently drawn first.
    releasePooled(target);
    const int64_t now = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    uint64_t dropped = 0u;
    if (m_hdBytes > target) {
        std::vector<Entry *> bound;
        for (auto &[key, entry] : m_entries)
            if (entry.hd && &entry != keep && now - entry.hdUseTime > 2000000)
                bound.push_back(&entry);
        std::sort(bound.begin(), bound.end(), [](const Entry *a, const Entry *b) { return a->hdUse < b->hdUse; });
        for (Entry *entry : bound) {
            if (m_hdBytes <= target)
                break;
            const bool pooled = entry->hdPooled;
            dropReplacement(*entry);
            // Looked up again at its next use.
            entry->hdStamp = 0u;
            ++dropped;
            if (pooled)
                releasePooled(target);
        }
    }
    if (dropped && GsReplacementStore::instance().logging())
        std::fprintf(stderr, "[texrep] budget: dropped %llu replacement texture(s), %.0f MB on the GPU\n",
                     static_cast<unsigned long long>(dropped), double(m_hdBytes) / 1048576.0);
    GsReplacementStore::instance().noteGpu(m_hdBytes, m_hdTextures);
    return m_hdBytes + incoming <= budget;
}

void GsTextureCache::learnTexture(const GsTextureKey &key, Entry &entry) {
    GsReplacementStore &store = GsReplacementStore::instance();
    // Not while the pack is off: everything would look unmatched.
    if (!store.learning() || !store.enabled() || !store.filesEnabled() || entry.hdRetryAt != 0 ||
        entry.identity.texels == 0u || entry.renderable || entry.hd ||
        !entry.unresolved.empty() || !entry.texture)
        return;
    // Direct-colour textures of 256x256 and more are the frame copied
    // (bloom, the menu's backdrop, movies): a new one every frame.
    if (!gsIsIndexedPsm(key.psm) && key.width >= 256u && key.height >= 256u)
        return;
    const uint64_t name = entry.identity.texels ^ (entry.identity.clut * 0x9e3779b97f4a7c15ull) ^
                          (static_cast<uint64_t>(key.width) << 40u) ^ (static_cast<uint64_t>(key.height) << 52u);
    if (!m_learnSeen.emplace(name, true).second)
        return;
    expand(key, {0u, 0u, key.width, key.height});
    // Nothing to learn from a texture of one colour.
    bool varied = false;
    for (size_t i = 4u; i < m_staging.size() && !varied; i += 4u)
        varied = std::memcmp(m_staging.data(), m_staging.data() + i, 4u) != 0;
    if (!varied)
        return;
    store.learn({entry.identity.texels, entry.identity.clut, key.psm, key.width, key.height}, key.tbp0, m_staging);
}

void GsTextureCache::followReplacement(const GsTextureKey &key, Entry &entry, const GsRegion &region,
                                       bool uploaded) {
    if (entry.hdComposed) {
        if (uploaded)
            patchAtlas(key, entry, region);
        return;
    }
    if (!entry.hd) {
        // A texture with no picture yet: worth a lookup only when the
        // store has one of its format and size, and texels it would hash
        // changed.
        if (!uploaded)
            return;
        for (const GsReplacementShape &shape : GsReplacementStore::instance().shapes(key.psm, key.width, key.height))
            if (region.intersects(GsRegion{0u, 0u, shape.width, shape.height})) {
                matchReplacement(key, entry);
                return;
            }
        // Icons copied into a texture of a size no file has (work_icon).
        if (gsIsIndexedPsm(key.psm) && !currentAtlases().empty())
            matchReplacement(key, entry);
        return;
    }
    if (entry.renderable) {
        dropReplacement(entry);
        return;
    }
    if (GsReplacementStore::instance().logging())
        std::fprintf(stderr, "[texrep] follow tbp0=%05x region %u,%u-%u,%u uploaded=%d\n", key.tbp0, region.x0,
                     region.y0, region.x1, region.y1, uploaded ? 1 : 0);
    if (region.intersects(entry.hdRegion)) {
        const GsRegion part = entry.hdRegion;
        if (!entry.unresolved.empty() && entry.unresolved.intersects(part)) {
            dropReplacement(entry);
            return;
        }
        if (expand(key, part, true, part.width(), part.height()) != entry.hdHash) {
            matchReplacement(key, entry);
            return;
        }
    }
    if (!uploaded)
        return;
    // The texels outside the picture are the game's: m_staging holds
    // `region` as just expanded, upscaled here in up to two strips (right
    // of the picture, and below it).
    const uint32_t s = entry.hdScale;
    std::vector<uint8_t> strip;
    std::string error;
    const auto put = [&](uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
        if (x0 >= x1 || y0 >= y1)
            return;
        const uint32_t w = (x1 - x0) * s, h = (y1 - y0) * s;
        strip.resize(static_cast<size_t>(w) * h * 4u);
        for (uint32_t y = 0u; y < h; ++y) {
            const uint8_t *source =
                m_staging.data() + (static_cast<size_t>(y0 + y / s - region.y0) * region.width() + (x0 - region.x0)) * 4u;
            uint8_t *row = strip.data() + static_cast<size_t>(y) * w * 4u;
            for (uint32_t x = 0u; x < w; ++x)
                std::memcpy(row + static_cast<size_t>(x) * 4u, source + static_cast<size_t>(x / s) * 4u, 4u);
        }
        beforeRewrite(entry.hd);
        if (!uploadRect(entry.hd, x0 * s, y0 * s, w, h, strip.data(), error))
            std::fprintf(stderr, "[texrep] upload: %s\n", error.c_str());
    };
    const GsRegion &part = entry.hdRegion;
    put(std::max(region.x0, part.x1), region.y0, region.x1, std::min(region.y1, part.y1));
    put(region.x0, std::max(region.y0, part.y1), region.x1, region.y1);
}

// ---- Atlases (Rogue Galaxy: added) -----------------------------------------
//
// Rogue Galaxy's item icon sheet (1024x960 in a 1024x1024 TEX0) never hashes
// to its pack file: a TEX_ANIME script copies one of three frames of an
// animated icon into a 40x40 slot of it, and a line of other texels lands in
// it besides. work_icon (256x128) is filled on the CPU with the party's weapon
// icons copied out of the sheet. Both are composed here from the sheet's HD
// picture: a texel the same as the disc's (the atlas's native picture) at its
// place takes the HD picture there; a 32x32 cell equal to one of the atlas's
// icons takes that icon's; any other changed block found in an atlas (a
// TEX_ANIME frame) takes the HD picture of where it was found; the rest is
// the game's texel, enlarged. The game's later changes re-compose only the
// 32-texel cells that changed.

namespace {

// RGBA8 with the GS's alpha (0x80 opaque) to 255 opaque, as files and the
// atlases' native pictures have it (the learn writer's conversion).
void toFileAlpha(const uint8_t *gs, size_t texels, std::vector<uint8_t> &out) {
    out.assign(gs, gs + texels * 4u);
    for (size_t i = 3u; i < out.size(); i += 4u)
        out[i] = static_cast<uint8_t>(std::min<uint32_t>(255u, out[i] * 255u / 128u));
}

uint64_t blockHash(const uint8_t *rgba, uint32_t stride, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    uint64_t hash = 1469598103934665603ull ^ (uint64_t(w) << 32u) ^ h;
    for (uint32_t row = 0u; row < h; ++row) {
        const uint8_t *p = rgba + (static_cast<size_t>(y + row) * stride + x) * 4u;
        for (uint32_t i = 0u; i < w * 4u; ++i)
            hash = (hash ^ p[i]) * 1099511628211ull;
    }
    return hash;
}

GsRegion alignToCells(const GsRegion &region, uint32_t width, uint32_t height) {
    const uint32_t c = GsReplacementAtlas::kCell;
    return {region.x0 / c * c, region.y0 / c * c, std::min(width, (region.x1 + c - 1u) / c * c),
            std::min(height, (region.y1 + c - 1u) / c * c)};
}

} // namespace

const std::vector<std::shared_ptr<const GsReplacementAtlas>> &GsTextureCache::currentAtlases() {
    GsReplacementStore &store = GsReplacementStore::instance();
    const uint64_t generation = store.generation();
    if (generation != m_atlasGeneration) {
        m_atlasGeneration = generation;
        auto atlases = store.atlases();
        bool same = atlases.size() == m_atlases.size();
        for (size_t i = 0u; same && i < atlases.size(); ++i)
            same = atlases[i] == m_atlases[i];
        if (!same) {
            m_atlases = std::move(atlases);
            m_atlasSearch.clear();
        }
    }
    return m_atlases;
}

bool GsTextureCache::matchAtlas(const GsTextureKey &key, Entry &entry) {
    GsReplacementStore &store = GsReplacementStore::instance();
    if (!entry.texture || entry.renderable || !entry.unresolved.empty() || !gsIsIndexedPsm(key.psm))
        return false;
    const auto &atlases = currentAtlases();
    if (atlases.empty())
        return false;
    const uint64_t clut = entry.identity.clut ? entry.identity.clut : hashClutWindow(key);
    // Drawn with another palette for now: the picture waits for the
    // atlas's palette to come back (while its atlases are still there).
    if (entry.hdComposed && entry.hdMain && clut != entry.hdAtlasClut &&
        std::all_of(entry.hdSources.begin(), entry.hdSources.end(), [&](const AtlasSource &source) {
            return std::find(atlases.begin(), atlases.end(), source.atlas) != atlases.end();
        })) {
        showAtlasVariant(key, entry, clut);
        entry.hdStamp = replacementStamp();
        return true;
    }
    std::vector<std::shared_ptr<const GsReplacementAtlas>> usable;
    for (const auto &atlas : atlases)
        if (atlas->key.psm == key.psm && (atlas->key.clut == 0u || atlas->key.clut == clut))
            usable.push_back(atlas);
    if (usable.empty())
        return false;

    const uint32_t width = key.width, height = key.height;
    const GsRegion whole{0u, 0u, width, height};
    expand(key, whole);
    const std::vector<uint8_t> native = m_staging;
    std::vector<uint8_t> png;
    toFileAlpha(native.data(), static_cast<size_t>(width) * height, png);

    // The atlas the texture is: its top-left part holds the atlas's texels,
    // a few changed (at most 1 in 32).
    std::shared_ptr<const GsReplacementAtlas> self;
    for (const auto &atlas : usable) {
        if (atlas->width > width || atlas->height > height)
            continue;
        uint64_t differing = 0u;
        const uint64_t area = uint64_t(atlas->width) * atlas->height, limit = area / 32u;
        for (uint32_t y = 0u; y < atlas->height && differing <= limit; ++y)
            for (uint32_t x = 0u; x < atlas->width; ++x)
                differing += std::memcmp(png.data() + (size_t(y) * width + x) * 4u,
                                         atlas->rgba.data() + (size_t(y) * atlas->width + x) * 4u, 4u) != 0;
        if (differing <= limit) {
            self = atlas;
            break;
        }
    }
    if (!self) {
        // A texture filled with an atlas's icons: at least half of its
        // 32x32 cells that are not one colour are icons of an atlas.
        const uint32_t c = GsReplacementAtlas::kCell;
        if (width % c != 0u || height % c != 0u)
            return false;
        uint32_t varied = 0u, icons = 0u;
        for (uint32_t y = 0u; y < height; y += c)
            for (uint32_t x = 0u; x < width; x += c) {
                const uint8_t *origin = png.data() + (size_t(y) * width + x) * 4u;
                bool differs = false;
                for (uint32_t row = 0u; row < c && !differs; ++row)
                    for (uint32_t i = 0u; i < c && !differs; ++i)
                        differs = std::memcmp(origin, origin + (size_t(row) * width + i) * 4u, 4u) != 0;
                if (!differs)
                    continue;
                ++varied;
                const uint64_t hash = GsReplacementAtlas::cellHash(origin, width);
                for (const auto &atlas : usable)
                    if (atlas->cells.count(hash)) {
                        ++icons;
                        break;
                    }
            }
        if (icons == 0u || icons * 2u < varied)
            return false;
    }

    // The atlases' HD pictures (the self atlas's first).
    if (self) {
        auto at = std::find(usable.begin(), usable.end(), self);
        std::rotate(usable.begin(), at, at + 1);
    }
    const uint64_t seen = store.completions();
    std::vector<AtlasSource> sources;
    bool waiting = false;
    for (const auto &atlas : usable) {
        GsReplacementPicture picture;
        // The icons are cut-outs; the HD alpha is hardened like any cut-out's.
        const GsPictureStatus status = store.picture(atlas->key, m_targets.scale(), nullptr, picture, true);
        if (status == GsPictureStatus::Pending)
            waiting = true;
        else if (status == GsPictureStatus::Ready && picture.image)
            sources.push_back({atlas, picture.image, picture.scale});
        else if (atlas == self)
            return false;
    }
    if (waiting) {
        entry.hdWait = true;
        entry.hdWaitAtlas = true;
        entry.hdWaitSeen = seen;
        return true;
    }
    if (sources.empty())
        return false;
    const uint32_t s = sources.front().scale;
    sources.erase(std::remove_if(sources.begin(), sources.end(),
                                 [&](const AtlasSource &source) { return source.scale != s; }),
                  sources.end());

    // Bound to the same atlases already: only the cells that changed.
    const bool sameSources = entry.hdComposed && entry.hdMain && entry.hdScale == s &&
                             entry.hdAtlas == self && entry.hdAtlasClut == clut &&
                             entry.hdSources.size() == sources.size() &&
                             std::equal(sources.begin(), sources.end(), entry.hdSources.begin(),
                                        [](const AtlasSource &a, const AtlasSource &b) {
                                            return a.atlas == b.atlas && a.image == b.image;
                                        });
    entry.hdStamp = replacementStamp();
    if (sameSources) {
        patchAtlas(key, entry, whole);
        return true;
    }

    const uint64_t hdWidth = uint64_t(width) * s, hdHeight = uint64_t(height) * s;
    const uint64_t bytes = hdWidth * hdHeight * 4u;
    if (hdWidth > 8192u || hdHeight > 8192u || bytes > (256ull << 20u))
        return false;
    if (!makeRoom(bytes, &entry)) {
        entry.hdRetryAt = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count() +
                          5000000;
        ++m_hdDenied;
        return false;
    }
    const int64_t now = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    if (now - m_hdWindowStart > 16000) {
        m_hdWindowStart = now;
        m_hdWindowBytes = 0u;
    }
    if (m_hdWindowBytes != 0u && m_hdWindowBytes + bytes > m_hdUploadAllowance) {
        entry.hdWait = true;
        entry.hdWaitAtlas = true;
        entry.hdWaitSeen = 0u; // asks again at the next use
        return true;
    }
    m_hdWindowBytes += bytes;

    const auto start = std::chrono::steady_clock::now();
    dropReplacement(entry);
    entry.hdComposed = true;
    entry.hdAtlas = self;
    entry.hdAtlasClut = clut;
    entry.hdSources = std::move(sources);
    std::vector<uint8_t> composed;
    composeAtlas(entry, whole, png.data(), native.data(), s, composed);

    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.width = static_cast<uint32_t>(hdWidth);
    info.height = static_cast<uint32_t>(hdHeight);
    info.layer_count_or_depth = 1u;
    info.num_levels = 1u;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    SDL_GPUTexture *texture = SDL_CreateGPUTexture(m_device.handle(), &info);
    std::string error;
    if (!texture || !uploadRect(texture, 0u, 0u, info.width, info.height, composed.data(), error)) {
        std::fprintf(stderr, "[texrep] atlas texture: %s; the game's texture stays\n",
                     texture ? error.c_str() : SDL_GetError());
        if (texture)
            SDL_ReleaseGPUTexture(m_device.handle(), texture);
        entry.hdComposed = false;
        entry.hdAtlas.reset();
        entry.hdSources.clear();
        return false;
    }
    entry.hd = texture;
    entry.hdMain = texture;
    entry.hdScale = s;
    entry.hdRegion = whole;
    entry.hdHash = 0u;
    entry.hdBytes = bytes;
    entry.hdLevels = 1u;
    entry.hdUse = ++m_useTick;
    entry.hdUseTime = now;
    entry.hdLast = std::move(png);
    entry.hdImage = std::move(composed);
    entry.hdLastTexels = entry.identity.texels;
    m_hdBytes += bytes;
    ++m_hdTextures;
    ++m_stats.replacements;
    GsReplacementStore::instance().noteGpu(m_hdBytes, m_hdTextures);
    std::fprintf(stderr, "[texrep] tbp0=%05x psm=%02x %ux%u: composed at %ux from %s %016llx in %.1f ms\n",
                 key.tbp0, key.psm, width, height, s, self ? "its atlas" : "the icons of",
                 static_cast<unsigned long long>((self ? self->key : entry.hdSources.front().atlas->key).texels),
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    return true;
}

void GsTextureCache::composeAtlas(const Entry &entry, const GsRegion &box, const uint8_t *png, const uint8_t *native,
                                  uint32_t s, std::vector<uint8_t> &out) {
    const uint32_t bw = box.width(), bh = box.height();
    const size_t outStride = size_t(bw) * s * 4u;
    out.resize(outStride * bh * s);
    // The game's texels, enlarged.
    for (uint32_t y = 0u; y < bh * s; ++y) {
        uint8_t *row = out.data() + y * outStride;
        const uint8_t *source = native + size_t(y / s) * bw * 4u;
        for (uint32_t x = 0u; x < bw * s; ++x)
            std::memcpy(row + size_t(x) * 4u, source + size_t(x / s) * 4u, 4u);
    }
    // `w` x `h` texels of a source's HD picture at (sx, sy) to box texel (dx, dy).
    const auto copy = [&](const AtlasSource &source, uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy, uint32_t w,
                          uint32_t h) {
        const GsReplacementImage &image = *source.image;
        for (uint32_t y = 0u; y < h * s; ++y)
            std::memcpy(out.data() + (size_t(dy) * s + y) * outStride + size_t(dx) * s * 4u,
                        image.rgba.data() + ((size_t(sy) * s + y) * image.width + size_t(sx) * s) * 4u,
                        size_t(w) * s * 4u);
    };
    std::vector<uint8_t> done(size_t(bw) * bh, 0u);
    const GsReplacementAtlas *self = entry.hdAtlas.get();
    // Texels that are the atlas's own.
    if (self) {
        const AtlasSource &source = entry.hdSources.front();
        for (uint32_t y = box.y0; y < std::min(box.y1, self->height); ++y) {
            uint32_t x = box.x0;
            const uint32_t x1 = std::min(box.x1, self->width);
            while (x < x1) {
                // Runs of equal texels, copied a run at a time.
                const auto same = [&](uint32_t at) {
                    return std::memcmp(png + (size_t(y - box.y0) * bw + (at - box.x0)) * 4u,
                                       self->rgba.data() + (size_t(y) * self->width + at) * 4u, 4u) == 0;
                };
                if (!same(x)) {
                    ++x;
                    continue;
                }
                uint32_t end = x + 1u;
                while (end < x1 && same(end))
                    ++end;
                copy(source, x, y, x - box.x0, y - box.y0, end - x, 1u);
                std::fill(done.begin() + size_t(y - box.y0) * bw + (x - box.x0),
                          done.begin() + size_t(y - box.y0) * bw + (end - box.x0), 1u);
                x = end;
            }
        }
    }
    const auto pending = [&](uint32_t x, uint32_t y) {
        const size_t at = size_t(y) * bw + x;
        if (done[at])
            return false;
        // Outside the atlas's part of the texture, the game's texels stay;
        // in a filled texture, transparent texels need nothing.
        if (self)
            return box.x0 + x < self->width && box.y0 + y < self->height;
        return png[at * 4u + 3u] != 0u;
    };
    // Cells that are an atlas's icon.
    const uint32_t c = GsReplacementAtlas::kCell;
    for (uint32_t y = (box.y0 + c - 1u) / c * c; y + c <= box.y1; y += c)
        for (uint32_t x = (box.x0 + c - 1u) / c * c; x + c <= box.x1; x += c) {
            const uint32_t lx = x - box.x0, ly = y - box.y0;
            bool any = false;
            for (uint32_t row = 0u; row < c && !any; ++row)
                for (uint32_t i = 0u; i < c && !any; ++i)
                    any = pending(lx + i, ly + row);
            if (!any)
                continue;
            const uint8_t *origin = png + (size_t(ly) * bw + lx) * 4u;
            const uint64_t hash = GsReplacementAtlas::cellHash(origin, bw);
            for (const AtlasSource &source : entry.hdSources) {
                const auto found = source.atlas->cells.find(hash);
                if (found == source.atlas->cells.end())
                    continue;
                const GsReplacementAtlas &atlas = *source.atlas;
                const uint32_t ax = found->second % (atlas.width / c) * c, ay = found->second / (atlas.width / c) * c;
                bool equal = true;
                for (uint32_t row = 0u; row < c && equal; ++row)
                    equal = std::memcmp(origin + size_t(row) * bw * 4u,
                                        atlas.rgba.data() + (size_t(ay + row) * atlas.width + ax) * 4u, c * 4u) == 0;
                if (!equal)
                    continue;
                copy(source, ax, ay, lx, ly, c, c);
                for (uint32_t row = 0u; row < c; ++row)
                    std::fill(done.begin() + size_t(ly + row) * bw + lx, done.begin() + size_t(ly + row) * bw + lx + c,
                              1u);
                break;
            }
        }
    // Other changed blocks (a TEX_ANIME frame): grouped by 4x4 tiles that
    // touch, and looked for in the atlases texel for texel.
    const uint32_t t = 4u, tw = (bw + t - 1u) / t, th = (bh + t - 1u) / t;
    std::vector<uint8_t> tile(size_t(tw) * th, 0u);
    bool anyPending = false;
    for (uint32_t y = 0u; y < bh; ++y)
        for (uint32_t x = 0u; x < bw; ++x)
            if (pending(x, y)) {
                tile[size_t(y / t) * tw + x / t] = 1u;
                anyPending = true;
            }
    if (!anyPending)
        return;
    std::vector<uint32_t> stack;
    for (uint32_t start = 0u; start < tile.size(); ++start) {
        if (tile[start] != 1u)
            continue;
        uint32_t tx0 = tw, ty0 = th, tx1 = 0u, ty1 = 0u;
        stack.assign(1u, start);
        tile[start] = 2u;
        while (!stack.empty()) {
            const uint32_t at = stack.back();
            stack.pop_back();
            const uint32_t tx = at % tw, ty = at / tw;
            tx0 = std::min(tx0, tx), ty0 = std::min(ty0, ty), tx1 = std::max(tx1, tx), ty1 = std::max(ty1, ty);
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = int(tx) + dx, ny = int(ty) + dy;
                    if (nx < 0 || ny < 0 || nx >= int(tw) || ny >= int(th))
                        continue;
                    const uint32_t next = uint32_t(ny) * tw + uint32_t(nx);
                    if (tile[next] == 1u) {
                        tile[next] = 2u;
                        stack.push_back(next);
                    }
                }
        }
        // The pending texels' own bounds within those tiles.
        uint32_t x0 = bw, y0 = bh, x1 = 0u, y1 = 0u, opaque = 0u;
        for (uint32_t y = ty0 * t; y < std::min(bh, (ty1 + 1u) * t); ++y)
            for (uint32_t x = tx0 * t; x < std::min(bw, (tx1 + 1u) * t); ++x)
                if (pending(x, y)) {
                    x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x + 1u), y1 = std::max(y1, y + 1u);
                    opaque += png[(size_t(y) * bw + x) * 4u + 3u] != 0u;
                }
        if (x0 >= x1 || x1 - x0 < 4u || y1 - y0 < 4u || opaque < 16u)
            continue;
        const uint32_t w = x1 - x0, h = y1 - y0;
        const uint64_t hash = blockHash(png, bw, x0, y0, w, h);
        AtlasHit hit;
        if (const auto known = m_atlasSearch.find(hash); known != m_atlasSearch.end()) {
            hit = known->second;
        } else {
            // The row with the most opaque texels is compared first.
            uint32_t anchor = 0u, best = 0u;
            for (uint32_t row = 0u; row < h; ++row) {
                uint32_t count = 0u;
                for (uint32_t i = 0u; i < w; ++i)
                    count += png[(size_t(y0 + row) * bw + x0 + i) * 4u + 3u] != 0u;
                if (count > best)
                    best = count, anchor = row;
            }
            const uint8_t *anchorRow = png + (size_t(y0 + anchor) * bw + x0) * 4u;
            for (const AtlasSource &source : entry.hdSources) {
                const GsReplacementAtlas &atlas = *source.atlas;
                if (w > atlas.width || h > atlas.height)
                    continue;
                // Its own rows first (an animation's frames sit side by side).
                const uint32_t ownY = box.y0 + y0;
                std::vector<uint32_t> rows;
                if (ownY + h <= atlas.height)
                    rows.push_back(ownY);
                for (uint32_t sy = 0u; sy + h <= atlas.height; ++sy)
                    if (sy != ownY)
                        rows.push_back(sy);
                for (const uint32_t sy : rows) {
                    const uint8_t *line = atlas.rgba.data() + size_t(sy + anchor) * atlas.width * 4u;
                    for (uint32_t sx = 0u; sx + w <= atlas.width; ++sx) {
                        if (std::memcmp(anchorRow, line + size_t(sx) * 4u, size_t(w) * 4u) != 0)
                            continue;
                        bool equal = true;
                        for (uint32_t row = 0u; row < h && equal; ++row)
                            equal = std::memcmp(png + (size_t(y0 + row) * bw + x0) * 4u,
                                                atlas.rgba.data() + (size_t(sy + row) * atlas.width + sx) * 4u,
                                                size_t(w) * 4u) == 0;
                        if (equal) {
                            hit = {&atlas, sx, sy};
                            break;
                        }
                    }
                    if (hit.atlas)
                        break;
                }
                if (hit.atlas)
                    break;
            }
            m_atlasSearch[hash] = hit;
            if (GsReplacementStore::instance().logging())
                std::fprintf(stderr, "[texrep] atlas block %ux%u at %u,%u: %s %u,%u\n", w, h, box.x0 + x0, box.y0 + y0,
                             hit.atlas ? "found at" : "not in the atlases", hit.x, hit.y);
        }
        if (!hit.atlas)
            continue;
        for (const AtlasSource &source : entry.hdSources)
            if (source.atlas.get() == hit.atlas) {
                copy(source, hit.x, hit.y, x0, y0, w, h);
                break;
            }
    }
}

void GsTextureCache::patchAtlas(const GsTextureKey &key, Entry &entry, const GsRegion &region) {
    const uint32_t width = key.width, height = key.height;
    if (entry.hdLast.size() != size_t(width) * height * 4u || !entry.hdMain)
        return;
    const GsRegion area{std::min(region.x0, width), std::min(region.y0, height), std::min(region.x1, width),
                        std::min(region.y1, height)};
    if (area.empty())
        return;
    const bool whole = area.x0 == 0u && area.y0 == 0u && area.x1 == width && area.y1 == height;
    const uint64_t clut = entry.identity.clut ? entry.identity.clut : hashClutWindow(key);
    if (clut != entry.hdAtlasClut) {
        // Another palette. Texels it changed meanwhile are composed when
        // the atlas's palette is back (they cannot be told apart in it).
        if (!whole || entry.identity.texels != entry.hdLastTexels)
            entry.hdLastTexels = 0u;
        showAtlasVariant(key, entry, clut);
        return;
    }
    entry.hd = entry.hdMain;
    // The palette came back over texels already composed.
    if (whole && entry.identity.texels != 0u && entry.identity.texels == entry.hdLastTexels)
        return;
    entry.hdLastTexels = whole ? entry.identity.texels : 0u;
    // Which texels changed since the picture was composed.
    expand(key, area);
    std::vector<uint8_t> png;
    toFileAlpha(m_staging.data(), size_t(area.width()) * area.height(), png);
    // The 32-texel cells with a changed texel.
    const uint32_t c = GsReplacementAtlas::kCell, cw = (width + c - 1u) / c, ch = (height + c - 1u) / c;
    std::vector<uint8_t> changed(size_t(cw) * ch, 0u);
    bool any = false;
    for (uint32_t y = area.y0; y < area.y1; ++y) {
        const uint8_t *now = png.data() + size_t(y - area.y0) * area.width() * 4u;
        const uint8_t *last = entry.hdLast.data() + (size_t(y) * width + area.x0) * 4u;
        if (std::memcmp(now, last, size_t(area.width()) * 4u) == 0)
            continue;
        for (uint32_t x = area.x0; x < area.x1; ++x)
            if (std::memcmp(now + size_t(x - area.x0) * 4u, last + size_t(x - area.x0) * 4u, 4u) != 0) {
                changed[size_t(y / c) * cw + x / c] = 1u;
                any = true;
            }
    }
    if (!any)
        return;
    // Composed and uploaded a run of cells along a row at a time, into the
    // picture and its variants.
    const uint32_t s = entry.hdScale;
    const size_t imageStride = size_t(width) * s * 4u;
    std::vector<uint8_t> composed, recolored;
    std::string error;
    beforeRewrite(entry.hdMain);
    for (const Entry::Variant &variant : entry.hdVariants)
        beforeRewrite(variant.texture);
    for (uint32_t cy = 0u; cy < ch; ++cy)
        for (uint32_t cx = 0u; cx < cw; ++cx) {
            if (!changed[size_t(cy) * cw + cx])
                continue;
            uint32_t end = cx + 1u;
            while (end < cw && changed[size_t(cy) * cw + end])
                ++end;
            const GsRegion box{cx * c, cy * c, std::min(width, end * c), std::min(height, (cy + 1u) * c)};
            cx = end;
            expand(key, box);
            const std::vector<uint8_t> native = m_staging;
            toFileAlpha(native.data(), size_t(box.width()) * box.height(), png);
            composeAtlas(entry, box, png.data(), native.data(), s, composed);
            const size_t rowBytes = size_t(box.width()) * s * 4u;
            for (uint32_t y = 0u; y < box.height() * s; ++y)
                std::memcpy(entry.hdImage.data() + (size_t(box.y0) * s + y) * imageStride + size_t(box.x0) * s * 4u,
                            composed.data() + y * rowBytes, rowBytes);
            if (!uploadRect(entry.hdMain, box.x0 * s, box.y0 * s, box.width() * s, box.height() * s, composed.data(),
                            error))
                std::fprintf(stderr, "[texrep] atlas upload: %s\n", error.c_str());
            for (const Entry::Variant &variant : entry.hdVariants) {
                recolorAtlas(entry, variant, box, recolored);
                if (!uploadRect(variant.texture, box.x0 * s, box.y0 * s, box.width() * s, box.height() * s,
                                recolored.data(), error))
                    std::fprintf(stderr, "[texrep] atlas upload: %s\n", error.c_str());
            }
            for (uint32_t y = box.y0; y < box.y1; ++y)
                std::memcpy(entry.hdLast.data() + (size_t(y) * width + box.x0) * 4u,
                            png.data() + size_t(y - box.y0) * box.width() * 4u, size_t(box.width()) * 4u);
            if (GsReplacementStore::instance().logging())
                std::fprintf(stderr, "[texrep] tbp0=%05x %ux%u: atlas cells %u,%u-%u,%u composed again\n", key.tbp0,
                             width, height, box.x0, box.y0, box.x1, box.y1);
        }
}

void GsTextureCache::recolorAtlas(const Entry &entry, const Entry::Variant &variant, const GsRegion &box,
                                  std::vector<uint8_t> &out) const {
    const uint32_t s = entry.hdScale, w = box.width() * s, h = box.height() * s;
    const size_t imageStride = size_t(entry.hdRegion.width()) * s * 4u;
    out.resize(size_t(w) * h * 4u);
    for (uint32_t y = 0u; y < h; ++y) {
        const uint8_t *source = entry.hdImage.data() + (size_t(box.y0) * s + y) * imageStride + size_t(box.x0) * s * 4u;
        uint8_t *row = out.data() + size_t(y) * w * 4u;
        for (uint32_t x = 0u; x < w; ++x) {
            row[x * 4u + 0u] = variant.rgba[0];
            row[x * 4u + 1u] = variant.rgba[1];
            row[x * 4u + 2u] = variant.rgba[2];
            // The picture's coverage at the palette's opacity.
            row[x * 4u + 3u] = static_cast<uint8_t>((uint32_t(source[x * 4u + 3u]) * variant.rgba[3] + 0x40u) / 0x80u);
        }
    }
}

void GsTextureCache::showAtlasVariant(const GsTextureKey &key, Entry &entry, uint64_t clut) {
    for (const Entry::Variant &variant : entry.hdVariants)
        if (variant.clut == clut) {
            entry.hd = variant.texture;
            return;
        }
    entry.hd = nullptr;
    if (std::find(entry.hdNoVariant.begin(), entry.hdNoVariant.end(), clut) != entry.hdNoVariant.end() ||
        entry.hdImage.empty())
        return;
    // A palette of one colour: every texel the composed picture has opaque
    // is that colour, every transparent one transparent (a few may differ:
    // a TEX_ANIME slot changed meanwhile).
    const uint32_t width = key.width, height = key.height;
    const GsReplacementAtlas *self = entry.hdAtlas.get();
    const GsRegion area{0u, 0u, self ? self->width : width, self ? self->height : height};
    expand(key, area);
    uint8_t colour[4] = {};
    bool found = false, flat = true;
    uint64_t differing = 0u;
    for (uint32_t y = 0u; y < area.y1 && flat; ++y)
        for (uint32_t x = 0u; x < area.x1; ++x) {
            const uint8_t *now = m_staging.data() + (size_t(y) * area.width() + x) * 4u;
            const bool wasOpaque = entry.hdLast[(size_t(y) * width + x) * 4u + 3u] != 0u;
            if ((now[3] != 0u) != wasOpaque) {
                ++differing;
                continue;
            }
            if (now[3] == 0u)
                continue;
            if (!found) {
                std::memcpy(colour, now, 4u);
                found = true;
            } else if (std::memcmp(colour, now, 4u) != 0) {
                flat = false;
                break;
            }
        }
    if (!flat || !found || differing > uint64_t(area.width()) * area.height() / 256u) {
        entry.hdNoVariant.push_back(clut);
        if (GsReplacementStore::instance().logging())
            std::fprintf(stderr, "[texrep] tbp0=%05x %ux%u: palette %016llx is not one colour; the game's texture\n",
                         key.tbp0, width, height, static_cast<unsigned long long>(clut));
        return;
    }
    const uint32_t s = entry.hdScale;
    const uint64_t bytes = uint64_t(width) * s * height * s * 4u;
    if (!makeRoom(bytes, &entry))
        return;
    Entry::Variant variant;
    variant.clut = clut;
    std::memcpy(variant.rgba, colour, 4u);
    std::vector<uint8_t> recolored;
    recolorAtlas(entry, variant, {0u, 0u, width, height}, recolored);
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.width = width * s;
    info.height = height * s;
    info.layer_count_or_depth = 1u;
    info.num_levels = 1u;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    variant.texture = SDL_CreateGPUTexture(m_device.handle(), &info);
    std::string error;
    if (!variant.texture || !uploadRect(variant.texture, 0u, 0u, info.width, info.height, recolored.data(), error)) {
        if (variant.texture)
            SDL_ReleaseGPUTexture(m_device.handle(), variant.texture);
        entry.hdNoVariant.push_back(clut);
        return;
    }
    entry.hdVariants.push_back(variant);
    entry.hdBytes += bytes;
    m_hdBytes += bytes;
    ++m_hdTextures;
    GsReplacementStore::instance().noteGpu(m_hdBytes, m_hdTextures);
    entry.hd = variant.texture;
    std::fprintf(stderr,
                 "[texrep] tbp0=%05x %ux%u: palette %016llx is one colour (%02x %02x %02x %02x): the composed "
                 "picture in it\n",
                 key.tbp0, width, height, static_cast<unsigned long long>(clut), colour[0], colour[1], colour[2],
                 colour[3]);
}

// Expands one texel rectangle into m_staging as tightly packed RGBA8 rows of
// region.width() pixels, and returns the hash of the raw texels it read.
//
// Three fast paths carry everything DQ8 draws during boot: CT32/Z32 store
// straight into staging (the raw word is the RGBA byte sequence), P8 resolves
// the palette once and indexes it per texel, and the P8H/P4H* family reads
// its row through the CT32 layout and shifts the index out of each word. The
// per-pixel path below remains for every other PSM; it is correct for all of
// them and none of them is hot on this game.
void GsTextureCache::buildPalette(const GsTextureKey &key) {
    const TexaState texa = unpackTexa(key.texa);
    const uint32_t clutWidth = std::max<uint32_t>(key.texclut & 0xffu, 1u);
    const uint32_t clutOriginU = (key.texclut >> 8u) & 0xffu;
    const uint32_t clutOriginV = (key.texclut >> 16u) & 0xffffu;
    const uint32_t psm = key.psm & 0x3fu;
    const bool fourBit = psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
    m_clutRgba.resize(fourBit ? 16u : 256u);
    for (uint32_t raw = 0u; raw < m_clutRgba.size(); ++raw) {
        const uint32_t clutIndex = resolveClutIndex(raw, key.cpsm, key.csm, key.csa, key.psm);
        const uint32_t clutX = clutOriginU + (clutIndex & 0x0fu);
        const uint32_t clutY = clutOriginV + (clutIndex >> 4u);
        const uint32_t entryValue = m_vram.read(key.cpsm, key.cbp, clutWidth, clutX, clutY);
        uint32_t color = 0u;
        switch (key.cpsm & 0x3fu) {
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            color = applyTexa(texa, key.cpsm, rgba5551To8888(entryValue));
            break;
        default:
            color = applyTexa(texa, key.cpsm, entryValue);
            break;
        }
        m_clutRgba[raw] = color;
    }
}

uint64_t GsTextureCache::expand(const GsTextureKey &key, const GsRegion &region, bool hashOnly, uint32_t seedWidth,
                                uint32_t seedHeight) {
    const TexaState texa = unpackTexa(key.texa);
    const uint32_t clutWidth = std::max<uint32_t>(key.texclut & 0xffu, 1u);
    const uint32_t clutOriginU = (key.texclut >> 8u) & 0xffu;
    const uint32_t clutOriginV = (key.texclut >> 16u) & 0xffffu;
    const bool indexed = gsIsIndexedPsm(key.psm);
    const uint32_t regionWidth = region.width();
    const uint32_t regionHeight = region.height();
    const uint32_t psm = key.psm & 0x3fu;

    if (!hashOnly)
        m_staging.resize(static_cast<size_t>(regionWidth) * regionHeight * 4u);
    uint64_t texelHash = hashStep(hashStep(hashStep(kHashSeed, seedWidth ? seedWidth : key.width),
                                           seedHeight ? seedHeight : key.height),
                                  key.psm);

    // The palette resolved to final RGBA once per expand, so the texel loop is
    // a load and a store. A per-pixel swizzled palette read here was the
    // single most expensive thing in the old loop.
    if (indexed && !hashOnly)
        buildPalette(key);

    const bool rowIsWords = psm == GS_PSM_CT32 || psm == GS_PSM_Z32 ||
                            psm == GS_PSM_T8H || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
    m_rawRow.resize(static_cast<size_t>(regionWidth) * (rowIsWords ? 4u : 1u));
    m_idxRow.resize(regionWidth);

    for (uint32_t y = region.y0; y < region.y1; ++y) {
        // Hashing only, the texels are read and hashed exactly as below and
        // nothing is written (Rogue Galaxy: added).
        uint8_t *row = hashOnly ? nullptr
                                : m_staging.data() + static_cast<size_t>(y - region.y0) * regionWidth * 4u;
        switch (psm) {
        case GS_PSM_CT32:
        case GS_PSM_Z32: {
            // The raw word is the RGBA byte sequence; TEXA does not apply.
            const auto readRow = psm == GS_PSM_Z32 ? GSMem::ReadRowZ32 : GSMem::ReadRowCT32;
            uint8_t *out = hashOnly ? m_rawRow.data() : row;
            readRow(m_vram.data(), key.tbp0, key.tbw, region.x0, y, regionWidth, out);
            texelHash = hashBytes(texelHash, out, static_cast<size_t>(regionWidth) * 4u);
            continue;
        }
        case GS_PSM_T8: {
            uint8_t *raw = m_rawRow.data();
            GSMem::ReadRowP8(m_vram.data(), key.tbp0, key.tbw, region.x0, y,
                             regionWidth, raw);
            texelHash = hashBytes(texelHash, raw, regionWidth);
            if (hashOnly)
                continue;
            for (uint32_t x = 0u; x < regionWidth; ++x)
                std::memcpy(row + static_cast<size_t>(x) * 4u, &m_clutRgba[raw[x]], 4u);
            continue;
        }
        case GS_PSM_T4: {
            // Rogue Galaxy: added. Its textures are mostly 4-bit, and the
            // general loop below re-read every texel's palette entry from
            // local memory although buildPalette has already resolved them.
            uint8_t *raw = m_idxRow.data();
            m_vram.readRowT4(key.tbp0, key.tbw, region.x0, y, regionWidth, raw);
            if (!hashOnly)
                for (uint32_t x = 0u; x < regionWidth; ++x)
                    std::memcpy(row + static_cast<size_t>(x) * 4u, &m_clutRgba[raw[x] & 0x0fu], 4u);
            // Hashed as two nibbles per byte, low first, like T4HL/T4HH.
            const uint32_t pairs = (regionWidth + 1u) / 2u;
            for (uint32_t p = 0u; p < pairs; ++p)
                raw[p] = static_cast<uint8_t>(raw[p * 2u] |
                                              ((p * 2u + 1u < regionWidth ? raw[p * 2u + 1u] : 0u) << 4u));
            texelHash = hashBytes(texelHash, raw, pairs);
            continue;
        }
        case GS_PSM_T8H:
        case GS_PSM_T4HL:
        case GS_PSM_T4HH: {
            // Indices ride in a 32-bit layout: read the row as CT32 words and
            // shift each index out. All the index bits are inside the pixel's
            // own word, so the +3-byte read offset the per-pixel path uses is
            // not needed here.
            uint32_t *words = reinterpret_cast<uint32_t *>(m_rawRow.data());
            uint8_t *raw = m_idxRow.data();
            GSMem::ReadRowCT32(m_vram.data(), key.tbp0, key.tbw, region.x0, y,
                               regionWidth, reinterpret_cast<uint8_t *>(words));
            const uint32_t shift = psm == GS_PSM_T4HH ? 28u : 24u;
            const uint32_t mask = psm == GS_PSM_T8H ? 0xffu : 0x0fu;
            for (uint32_t x = 0u; x < regionWidth; ++x)
                raw[x] = static_cast<uint8_t>((words[x] >> shift) & mask);
            if (psm == GS_PSM_T8H) {
                texelHash = hashBytes(texelHash, raw, regionWidth);
            } else {
                // Two nibbles per byte, low first, so the packed stream stays
                // the hash input.
                const uint32_t pairs = (regionWidth + 1u) / 2u;
                for (uint32_t p = 0u; p < pairs; ++p)
                    raw[p] = static_cast<uint8_t>(raw[p * 2u] |
                                                  ((p * 2u + 1u < regionWidth
                                                        ? raw[p * 2u + 1u]
                                                        : 0u)
                                                   << 4u));
                texelHash = hashBytes(texelHash, raw, pairs);
            }
            if (hashOnly)
                continue;
            for (uint32_t x = 0u; x < regionWidth; ++x) {
                const uint32_t index = (words[x] >> shift) & mask;
                std::memcpy(row + static_cast<size_t>(x) * 4u, &m_clutRgba[index], 4u);
            }
            continue;
        }
        default:
            break;
        }

        // Formats with no fast path: one swizzled read and one hash word per
        // pixel, exactly as before.
        for (uint32_t x = region.x0; x < region.x1; ++x) {
            const uint32_t raw = m_vram.read(key.psm, key.tbp0, key.tbw, x, y);
            texelHash = hashStep(texelHash, raw);
            if (hashOnly)
                continue;
            uint32_t color = 0u;

            if (indexed) {
                const uint32_t clutIndex =
                    resolveClutIndex(raw, key.cpsm, key.csm, key.csa, key.psm);
                const uint32_t clutX = clutOriginU + (clutIndex & 0x0fu);
                const uint32_t clutY = clutOriginV + (clutIndex >> 4u);
                const uint32_t entryValue =
                    m_vram.read(key.cpsm, key.cbp, clutWidth, clutX, clutY);
                switch (key.cpsm & 0x3fu) {
                case GS_PSM_CT16:
                case GS_PSM_CT16S:
                    color = applyTexa(texa, key.cpsm, rgba5551To8888(entryValue));
                    break;
                default:
                    color = applyTexa(texa, key.cpsm, entryValue);
                    break;
                }
            } else {
                switch (psm) {
                case GS_PSM_CT16:
                case GS_PSM_CT16S:
                case GS_PSM_Z16:
                case GS_PSM_Z16S:
                    color = applyTexa(texa, GS_PSM_CT16, rgba5551To8888(raw));
                    break;
                default:
                    color = applyTexa(texa, key.psm, raw);
                    break;
                }
            }

            const size_t offset = static_cast<size_t>(x - region.x0) * 4u;
            row[offset] = static_cast<uint8_t>(color);
            row[offset + 1u] = static_cast<uint8_t>(color >> 8u);
            row[offset + 2u] = static_cast<uint8_t>(color >> 16u);
            row[offset + 3u] = static_cast<uint8_t>(color >> 24u);
        }
    }
    return texelHash;
}

// Which texels of a texture a set of local-memory pages covers. Same geometry
// as GsTargetCache::regionForPages, applied to a texture's own base and width.
GsRegion GsTextureCache::regionForPages(const GsTextureKey &key,
                                        const GsPageSet &pages) const {
    GsRegion region{};
    const GsPageExtent extent = gsPageExtent(key.psm);
    if (!extent.valid())
        return {0u, 0u, key.width, key.height};

    const uint32_t pixelsPerRow = std::max<uint32_t>(key.tbw, 1u) * 64u;
    const uint32_t pagesPerRow = std::max<uint32_t>(pixelsPerRow / extent.width, 1u);
    const uint32_t basePage = key.tbp0 >> 5u;
    const uint32_t pageRows = (key.height + extent.height - 1u) / extent.height;
    const uint32_t pageColumns = (key.width + extent.width - 1u) / extent.width;

    for (uint32_t row = 0u; row < pageRows; ++row) {
        for (uint32_t column = 0u; column < pageColumns; ++column) {
            const uint32_t page = (basePage + row * pagesPerRow + column) % kGsPageCount;
            const bool tailHit = (key.tbp0 & 31u) != 0u &&
                                 pages.test((page + 1u) % kGsPageCount);
            if (!pages.test(page) && !tailHit)
                continue;
            GsRegion cell{column * extent.width, row * extent.height,
                          std::min((column + 1u) * extent.width, key.width),
                          std::min((row + 1u) * extent.height, key.height)};
            region.merge(cell);
        }
    }
    return region;
}

// Transfer buffers come back once the submission that read them completes.
// Reaping happens on take; the common case never waits because by the time
// a buffer is wanted again the previous upload has long finished.
SDL_GPUTransferBuffer *GsTextureCache::takeUploadBuffer(size_t bytes) {
    std::string error;
    return m_uploadPool.take(m_device, bytes, error);
}

void GsTextureCache::recycleUploadBuffer(SDL_GPUTransferBuffer *buffer) {
    m_uploadPool.used(buffer, m_device.pendingSerial());
}

void GsTextureCache::releaseUploadPool() {
    m_uploadPool.release(m_device);
}

bool GsTextureCache::uploadRegion(SDL_GPUTexture *texture, const GsRegion &region,
                                  std::string &error) {
    return uploadRect(texture, region.x0, region.y0, region.width(), region.height(), m_staging.data(), error);
}

bool GsTextureCache::uploadRect(SDL_GPUTexture *texture, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                                const uint8_t *rgba, std::string &error, uint32_t level) {
    SDL_GPUDevice *device = m_device.handle();
    const size_t bytes = static_cast<size_t>(width) * height * 4u;

    SDL_GPUTransferBuffer *upload = takeUploadBuffer(bytes);
    if (!upload) {
        error = std::string("SDL_CreateGPUTransferBuffer(texture cache): ") + SDL_GetError();
        return false;
    }
    void *mapped = SDL_MapGPUTransferBuffer(device, upload, false);
    if (!mapped) {
        error = std::string("SDL_MapGPUTransferBuffer(texture cache): ") + SDL_GetError();
        return false;
    }
    std::memcpy(mapped, rgba, bytes);
    SDL_UnmapGPUTransferBuffer(device, upload);

    SDL_GPUCommandBuffer *commands = m_device.pendingCommands(error);
    if (!commands)
        return false;
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
    if (!copy) {
        error = std::string("SDL_BeginGPUCopyPass(texture cache): ") + SDL_GetError();
        m_device.cancelPending();
        return false;
    }
    SDL_GPUTextureTransferInfo source{};
    source.transfer_buffer = upload;
    source.pixels_per_row = width;
    source.rows_per_layer = height;
    SDL_GPUTextureRegion destination{};
    destination.texture = texture;
    destination.mip_level = level;
    destination.x = x;
    destination.y = y;
    destination.w = width;
    destination.h = height;
    destination.d = 1u;
    // cycle stays false: a cycled destination would come back with its
    // untouched texels undefined, which a partial update must not do. The
    // buffer side is guarded by the pool fence instead.
    SDL_UploadToGPUTexture(copy, &source, &destination, false);
    SDL_EndGPUCopyPass(copy);
    recycleUploadBuffer(upload);
    return true;
}

// Hash of the palette window this texture's indices resolve into: only the
// entries the CLUT resolution can actually reach, CSA window included.
uint64_t GsTextureCache::hashClutWindow(const GsTextureKey &key) const {
    const uint32_t clutWidth = std::max<uint32_t>(key.texclut & 0xffu, 1u);
    const uint32_t clutOriginU = (key.texclut >> 8u) & 0xffu;
    const uint32_t clutOriginV = (key.texclut >> 16u) & 0xffffu;
    const bool sixteenBitClut = key.cpsm == GS_PSM_CT16 || key.cpsm == GS_PSM_CT16S;
    const uint32_t entries = ((key.psm & 0x3fu) == GS_PSM_T4 ||
                              (key.psm & 0x3fu) == GS_PSM_T4HL ||
                              (key.psm & 0x3fu) == GS_PSM_T4HH)
                                 ? 16u
                                 : (sixteenBitClut ? 512u : 256u);
    uint64_t clutHash = hashStep(hashStep(kHashSeed, key.cpsm), key.csm);
    for (uint32_t index = 0u; index < entries; ++index) {
        const uint32_t resolved =
            resolveClutIndex(index, key.cpsm, key.csm, key.csa, key.psm);
        const uint32_t clutX = clutOriginU + (resolved & 0x0fu);
        const uint32_t clutY = clutOriginV + (resolved >> 4u);
        clutHash = hashStep(clutHash, m_vram.read(key.cpsm, key.cbp, clutWidth, clutX, clutY));
    }
    return clutHash;
}

// A palette-page write landed. Re-hash the window: when it is unchanged --
// games re-upload the same CLUT every frame -- this is the whole cost of the
// write. When it moved, every expanded colour is stale, so the whole texture
// re-expands on next use.
void GsTextureCache::refreshClut(const GsTextureKey &key, Entry &entry) {
    entry.clutDirty = false;
    if (!gsIsIndexedPsm(key.psm))
        return;
    const uint64_t clutHash = hashClutWindow(key);
    if (clutHash == entry.identity.clut)
        return;
    entry.identity.clut = clutHash;
    entry.clutUploadPending = true;
    entry.dirty.merge(GsRegion{0u, 0u, key.width, key.height});
}

// Patches the texels a host write touched, instead of discarding a texture and
// re-expanding all of it. On the DQ8 movie path the written region is a small
// fraction of a 512x512 texture, and rebuilding it was the dominant cost.
GsPageSet GsTextureCache::staleTexelPages(const GsTextureKey &key, const GsRegion &region) const {
    GsPageSet pages;
    if (region.empty())
        return pages;
    if (key.psm == GS_PSM_CT32 || key.psm == GS_PSM_CT24 ||
        key.psm == GS_PSM_T8H || key.psm == GS_PSM_T4HL || key.psm == GS_PSM_T4HH) {
        m_targets.markReadPagesCt32(pages, key.tbp0, key.tbw, region);
    } else {
        // Through the words the CPU wrote where it can tell (T8, T4, CT16):
        // a texture uploaded into pages a target still owns needs no
        // readback (Rogue Galaxy: markReadPages).
        m_targets.markReadPages(pages, key.tbp0, key.tbw, key.psm, region);
    }
    return pages;
}

bool GsTextureCache::resolveSource(const GsTextureKey &key, const GsRegion &region,
                                  std::string &error, const GsPageSet *exclude) {
    GsPageSet conservative;
    markSourcePages(key, conservative);
    if (!m_targets.ownsAny(conservative)) return true;

    GsPageSet pages = staleTexelPages(key, region);
    if (exclude)
        pages &= ~*exclude;
    markClutReads(key, pages);
    return m_targets.resolve(pages, error);
}

void GsTextureCache::markClutReads(const GsTextureKey &key, GsPageSet &pages) const {
    if (!gsIsIndexedPsm(key.psm))
        return;
    if (key.cpsm != GS_PSM_CT32 && key.cpsm != GS_PSM_CT24) {
        markClutPages(key, pages);
        return;
    }
    // Palette words the CPU wrote itself are already right in local memory.
    const bool fourBit = key.psm == GS_PSM_T4 || key.psm == GS_PSM_T4HL || key.psm == GS_PSM_T4HH;
    const uint32_t count = fourBit ? 16u : 256u;
    const uint32_t bw = std::max<uint32_t>(key.texclut & 0xffu, 1u);
    for (uint32_t i = 0u; i < count; ++i) {
        const uint32_t index = resolveClutIndex(i, key.cpsm, key.csm, key.csa, key.psm);
        const uint32_t x = ((key.texclut >> 8u) & 0xffu) + (index & 15u);
        const uint32_t y = (key.texclut >> 16u) + (index >> 4u);
        m_targets.markReadPagesCt32(pages, key.cbp, bw, {x, y, x + 1u, y + 1u});
    }
}

// A texture expanded from a target's pages has no name of its own, so no pack
// picture could stand for it (the field's compass, uploaded into pages an
// off-screen target owns). When the CPU wrote every word of it there (no page
// of it is stale), local memory holds its texels: they name it as a build
// would, and a replacement can bind (Rogue Galaxy: added).
void GsTextureCache::nameFromLocalMemory(const GsTextureKey &key, Entry &entry) {
    const GsRegion whole{0u, 0u, key.width, key.height};
    if (!staleTexelPages(key, whole).none())
        return;
    entry.identity.texels = expand(key, whole, true);
    entry.hdStamp = 0u; // looked up again at its next use
    if (GsReplacementStore::instance().logging())
        std::fprintf(stderr, "[texrep] texture tbp0=%05x psm=%02x %ux%u from a target's pages, named %016llx_%016llx\n",
                     key.tbp0, key.psm, key.width, key.height, static_cast<unsigned long long>(entry.identity.texels),
                     static_cast<unsigned long long>(entry.identity.clut));
}

bool GsTextureCache::expandFromTarget(const GsTextureKey &key, Entry &entry, GsRegion region,
                                      bool &handled, std::string &error) {
    handled = false;
    if ((key.psm & 0x3fu) != GS_PSM_T8 || (key.tbp0 & 31u) != 0u)
        return true;
    GsPageSet texelPages;
    markTexelPages(key, texelPages);
    GsSurface *target = m_targets.nativeOwner(texelPages, true);
    if (!target)
        return true;

    // The palette is still read on the CPU; only the texels come from the
    // target, so only their cells of it need be current (its other queued
    // patches, the textures uploaded into its other pages, can wait: Rogue
    // Galaxy, refreshRegion).
    GsPageSet clutPages;
    markClutReads(key, clutPages);
    if (!m_targets.resolve(clutPages, error) ||
        !m_targets.refreshRegion(*target, m_targets.regionForPages(*target, texelPages), error))
        return false;
    buildPalette(key);

    if (!entry.renderable) {
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.width = key.width;
        info.height = key.height;
        info.layer_count_or_depth = 1u;
        info.num_levels = 1u;
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        SDL_GPUTexture *texture = SDL_CreateGPUTexture(m_device.handle(), &info);
        if (!texture) {
            error = std::string("SDL_CreateGPUTexture(indexed from target): ") + SDL_GetError();
            return false;
        }
        if (entry.texture) {
            beforeRewrite(entry.texture);
            SDL_ReleaseGPUTexture(m_device.handle(), entry.texture);
        }
        entry.texture = texture;
        entry.renderable = true;
        region = {0u, 0u, key.width, key.height};
    } else {
        beforeRewrite(entry.texture);
    }

    const SDL_Rect rect{int(region.x0), int(region.y0), int(region.width()), int(region.height())};
    const std::array<uint32_t, 4> pages{key.tbp0 >> 5u, std::max<uint32_t>(key.tbw >> 1u, 1u),
                                        target->base >> 5u,
                                        target->bufferWidth | (std::max(target->scale, 1u) << 16u)};
    if (!m_device.expandIndexed8(target->texture, m_clutRgba.data(), entry.texture, key.width,
                                 key.height, rect, pages, error))
        return false;
    ++m_stats.expandedFromTargets;
    handled = true;
    checkAgainstCpu(key, entry, region);
    return true;
}

// DQ8_GFX_CHECK_GPU_TEXTURES=1 rebuilds every texture expandFromTarget made
// the old way, reading the target back, and reports texels that differ. It
// stalls on every readback, so it is for checking changes, not for play.
void GsTextureCache::checkAgainstCpu(const GsTextureKey &key, Entry &entry, const GsRegion &region) {
    static const bool enabled = std::getenv("DQ8_GFX_CHECK_GPU_TEXTURES") != nullptr;
    if (!enabled)
        return;
    std::vector<uint8_t> gpu;
    std::string error;
    if (!m_device.submitPending(error) ||
        !m_device.downloadTexture(entry.texture, key.width, key.height, gpu, error) ||
        !resolveSource(key, region, error)) {
        std::fprintf(stderr, "[texture-check] %s\n", error.c_str());
        return;
    }
    expand(key, region);
    uint64_t differing = 0u;
    for (uint32_t y = region.y0; y < region.y1; ++y)
        for (uint32_t x = region.x0; x < region.x1; ++x)
            differing += std::memcmp(gpu.data() + (size_t(y) * key.width + x) * 4u,
                                     m_staging.data() + (size_t(y - region.y0) * region.width() + (x - region.x0)) * 4u,
                                     4u) != 0;
    std::fprintf(stderr, "[texture-check] tbp=%05x %ux%u from target: %llu of %llu texels differ\n", key.tbp0,
                 key.width, key.height, static_cast<unsigned long long>(differing),
                 static_cast<unsigned long long>(region.width()) * region.height());
}

void GsTextureCache::restrictToSampled(const GsTextureKey &key, const GsRegion &region, const GsRegion *sampled,
                                       GsPageSet &exclude, GsRegion &unresolved) const {
    exclude.reset();
    if (!sampled || region.empty() || sampled->contains(region))
        return;
    // Only pages a target holds matter: stale words nobody owns resolve to
    // nothing anyway.
    GsPageSet stale = staleTexelPages(key, region);
    for (uint32_t page = 0u; page < kGsPageCount; ++page) {
        if (!stale.test(page))
            continue;
        GsPageSet one;
        one.set(page);
        if (!m_targets.ownsAny(one))
            stale.reset(page);
    }
    if (stale.none())
        return;
    GsRegion staleRegion = regionForPages(key, stale);
    staleRegion.x0 = std::max(staleRegion.x0, region.x0);
    staleRegion.y0 = std::max(staleRegion.y0, region.y0);
    staleRegion.x1 = std::min(staleRegion.x1, region.x1);
    staleRegion.y1 = std::min(staleRegion.y1, region.y1);
    if (staleRegion.empty() || sampled->intersects(staleRegion))
        return;
    exclude = stale;
    unresolved.merge(staleRegion);
}

bool GsTextureCache::update(const GsTextureKey &key, Entry &entry, std::string &error, const GsRegion *sampled) {
    const GsRegion region = entry.dirty;
    if (region.empty() || !entry.texture)
        return true;

    bool expanded = false;
    if (!expandFromTarget(key, entry, region, expanded, error))
        return false;
    if (expanded) {
        dropReplacement(entry);
        entry.identity.texels = 0u;
        entry.clutUploadPending = false;
        entry.dirty.clear();
        nameFromLocalMemory(key, entry);
        ++m_stats.partialUpdates;
        m_stats.texelsUpdated += static_cast<uint64_t>(region.width()) * region.height();
        return true;
    }

    // Pages of the target the draw reaches no texel of stay there; their
    // texels are re-expanded from local memory as it stands and stay
    // unresolved.
    GsPageSet exclude;
    restrictToSampled(key, region, sampled, exclude, entry.unresolved);
    if (!resolveSource(key, region, error, &exclude)) {
        return false;
    }

    const bool wholeTexture = region.x0 == 0u && region.y0 == 0u &&
                              region.x1 >= key.width && region.y1 >= key.height;
    // A whole texture dirty again with no palette change waiting is most
    // often back to what it held: another texture passed through its pages
    // and this one was uploaded again (Rogue Galaxy: added; 86% of the
    // re-expands in its streets). Hashing the raw texels first costs their
    // read; when they match, the expansion and the upload are skipped.
    // DQ8_GFX_NO_UNCHANGED_SKIP=1 expands them anyway, as before.
    static const bool skipUnchanged = std::getenv("DQ8_GFX_NO_UNCHANGED_SKIP") == nullptr;
    if (skipUnchanged && wholeTexture && !entry.clutUploadPending && entry.identity.texels != 0u &&
        expand(key, region, true) == entry.identity.texels) {
        entry.dirty.clear();
        ++m_stats.unchangedUpdates;
        return true;
    }
    const uint64_t texelHash = expand(key, region);
    // A whole-texture re-expand whose raw texels hash to what we already hold,
    // with no palette change waiting, is the title screen re-uploading its own
    // UI: the GPU texture is already right, so skip the transfer entirely.
    const bool unchanged = wholeTexture && !entry.clutUploadPending &&
                           texelHash == entry.identity.texels;
    if (!unchanged) {
        beforeRewrite(entry.texture);
        if (!uploadRegion(entry.texture, region, error))
            return false;
    }

    // The stored hash described the whole texture before this patch. A whole-
    // texture expand re-derived it; a partial one would leave it wrong, and
    // rehashing would mean reading every texel, so the identity is cleared
    // rather than left wrong -- nothing should match a partially updated
    // texture against a replacement pack.
    if (wholeTexture) {
        entry.identity.texels = texelHash;
        entry.clutUploadPending = false;
    } else {
        entry.identity.texels = 0u;
    }
    entry.dirty.clear();
    // A bound picture follows the texels it stands over (Rogue Galaxy:
    // added).
    if (!unchanged && wholeTexture && GsReplacementStore::instance().logging())
        std::fprintf(stderr, "[texrep] texture tbp0=%05x tbw=%u psm=%02x %ux%u now %016llx_%016llx\n", key.tbp0,
                     key.tbw, key.psm, key.width, key.height, static_cast<unsigned long long>(entry.identity.texels),
                     static_cast<unsigned long long>(entry.identity.clut));
    followReplacement(key, entry, region, !unchanged);
    // Learn mode: a texture whose whole texels changed is another texture.
    if (!unchanged && wholeTexture && !entry.hd && !entry.hdWait)
        learnTexture(key, entry);
    ++m_stats.partialUpdates;
    m_stats.texelsUpdated += static_cast<uint64_t>(region.width()) * region.height();
    return true;
}

bool GsTextureCache::build(const GsTextureKey &key, Entry &entry, std::string &error, const GsRegion *sampled) {
    if (!m_device.valid()) {
        error = "SDL GPU device is not available";
        return false;
    }

    // A texture may be sourced from a region a render target still owns --
    // render-to-texture, which DQ8 uses. Resolving costs the extra detail a
    // scaled target holds; sampling the target directly is the fix, and is not
    // done yet.
    GsPageSet sourcePages;
    markSourcePages(key, sourcePages);
    const bool owned = m_targets.ownsAny(sourcePages);
    if (owned)
        ++m_stats.renderTargetSources;

    bool expanded = false;
    if (!expandFromTarget(key, entry, {0u, 0u, key.width, key.height}, expanded, error))
        return false;
    if (expanded) {
        entry.identity = {};
        entry.identity.clut = hashClutWindow(key);
        nameFromLocalMemory(key, entry);
        ++m_stats.builds;
        return true;
    }

    // Pages of a target the draw reaches no texel of stay there; their
    // texels are expanded from whatever local memory holds and marked
    // unresolved (restrictToSampled).
    const GsRegion whole{0u, 0u, key.width, key.height};
    GsPageSet exclude;
    if (owned)
        restrictToSampled(key, whole, sampled, exclude, entry.unresolved);
    if (!resolveSource(key, whole, error, &exclude)) {
        return false;
    }

    const uint32_t width = key.width;
    const uint32_t height = key.height;
    m_staging.resize(static_cast<size_t>(width) * height * 4u);

    const bool indexed = gsIsIndexedPsm(key.psm);

    // Identity, computed off the same reads the expansion does. Dimensions and
    // format go into the texel hash because the same bytes read as 64x64 and as
    // 128x128 are different pictures.
    entry.identity = {};
    if (indexed) {
        // Only the palette window this texture can actually reach: CSA lets
        // many 4-bit textures share one CLUT, and folding in the whole thing
        // would make them all move together.
        entry.identity.clut = hashClutWindow(key);
    }

    entry.identity.texels = expand(key, {0u, 0u, width, height});
    dumpTexture(key, entry.identity, m_staging, m_stats.builds);

    SDL_GPUDevice *device = m_device.handle();
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1u;
    info.num_levels = 1u;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    entry.texture = SDL_CreateGPUTexture(device, &info);
    if (!entry.texture) {
        error = std::string("SDL_CreateGPUTexture(texture cache): ") + SDL_GetError();
        return false;
    }

    SDL_GPUTransferBuffer *upload = takeUploadBuffer(m_staging.size());
    if (!upload) {
        error = std::string("SDL_CreateGPUTransferBuffer(texture cache): ") + SDL_GetError();
        SDL_ReleaseGPUTexture(device, entry.texture);
        entry.texture = nullptr;
        return false;
    }
    void *mapped = SDL_MapGPUTransferBuffer(device, upload, false);
    if (!mapped) {
        error = std::string("SDL_MapGPUTransferBuffer(texture cache): ") + SDL_GetError();
        SDL_ReleaseGPUTexture(device, entry.texture);
        entry.texture = nullptr;
        return false;
    }
    std::memcpy(mapped, m_staging.data(), m_staging.size());
    SDL_UnmapGPUTransferBuffer(device, upload);

    SDL_GPUCommandBuffer *commands = m_device.pendingCommands(error);
    if (!commands) {
        SDL_ReleaseGPUTexture(device, entry.texture);
        entry.texture = nullptr;
        return false;
    }
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
    if (!copy) {
        error = std::string("SDL_BeginGPUCopyPass(texture cache): ") + SDL_GetError();
        m_device.cancelPending();
        SDL_ReleaseGPUTexture(device, entry.texture);
        entry.texture = nullptr;
        return false;
    }
    SDL_GPUTextureTransferInfo source{};
    source.transfer_buffer = upload;
    source.pixels_per_row = width;
    source.rows_per_layer = height;
    SDL_GPUTextureRegion region{};
    region.texture = entry.texture;
    region.w = width;
    region.h = height;
    region.d = 1u;
    SDL_UploadToGPUTexture(copy, &source, &region, false);
    SDL_EndGPUCopyPass(copy);
    recycleUploadBuffer(upload);

    ++m_stats.builds;
    m_stats.texelsExpanded += static_cast<uint64_t>(width) * height;
    return true;
}

} // namespace dq8::gfx
