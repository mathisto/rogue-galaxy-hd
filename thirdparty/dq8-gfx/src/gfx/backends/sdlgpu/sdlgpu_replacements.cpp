#include "gfx/backends/sdlgpu/sdlgpu_replacements.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <tuple>

namespace dq8::gfx {

namespace {

using Clock = std::chrono::steady_clock;

struct KeyLess {
    bool operator()(const GsReplacementKey &a, const GsReplacementKey &b) const {
        return std::tie(a.texels, a.clut, a.psm, a.width, a.height) <
               std::tie(b.texels, b.clut, b.psm, b.width, b.height);
    }
};

struct DrawnLess {
    bool operator()(const std::pair<GsReplacementKey, uint32_t> &a,
                    const std::pair<GsReplacementKey, uint32_t> &b) const {
        if (KeyLess{}(a.first, b.first))
            return true;
        if (KeyLess{}(b.first, a.first))
            return false;
        return a.second < b.second;
    }
};

// <texels>_<clut>_<W>x<H>_psm<NN>, '_' or '-' between the fields: the name
// DQ8_GFX_DUMP_TEXTURES gives a whole texture's dump.
bool parseName(const std::string &stem, GsReplacementKey &key) {
    unsigned long long texels = 0u, clut = 0u;
    unsigned width = 0u, height = 0u, psm = 0u;
    char a = 0, b = 0, c = 0;
    if (std::sscanf(stem.c_str(), "%16llx%c%16llx%c%ux%u%cpsm%x", &texels, &a, &clut, &b, &width, &height, &c,
                    &psm) != 8)
        return false;
    for (const char separator : {a, b, c})
        if (separator != '_' && separator != '-')
            return false;
    if (texels == 0u || width == 0u || height == 0u || width > 1024u || height > 1024u)
        return false;
    key = {texels, clut, psm, width, height};
    return true;
}

std::string hex(uint64_t value) {
    char text[20];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
    return text;
}

} // namespace

struct GsReplacementStore::Impl {
    mutable std::mutex mutex;
    std::atomic<bool> enabled{true};
    std::atomic<uint64_t> generation{1u};
    GsReplacementImageLoader loader;
    std::string directory;

    struct File {
        std::string path;
        std::shared_ptr<const GsReplacementImage> image; // read on first use
        uint32_t scale = 0u;
        bool failed = false;
    };
    std::map<GsReplacementKey, File, KeyLess> files;

    struct Generated {
        std::vector<GsReplacementKey> keys;
        GsReplacementGenerator generator;
        uint32_t maxScale = 4u;
        // Drawn pictures by texture and scale, until the art changes.
        std::map<std::pair<GsReplacementKey, uint32_t>, std::shared_ptr<const GsReplacementImage>, DrawnLess> drawn;
        bool failedLogged = false;
    };
    std::map<std::string, Generated> generated;

    void bump() { generation.fetch_add(1u, std::memory_order_acq_rel); }

    size_t scanLocked() {
        files.clear();
        if (directory.empty())
            return 0u;
        std::error_code error;
        const std::filesystem::path root(directory);
        if (!std::filesystem::is_directory(root, error))
            return 0u;
        for (auto it = std::filesystem::recursive_directory_iterator(root, error);
             !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
            if (!it->is_regular_file(error))
                continue;
            std::string extension = it->path().extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (extension != ".png")
                continue;
            GsReplacementKey key;
            if (!parseName(it->path().stem().string(), key)) {
                std::fprintf(stderr, "[texrep] %s: not a replacement name (texels_clut_WxH_psmNN.png)\n",
                             it->path().string().c_str());
                continue;
            }
            files[key] = File{it->path().string()};
        }
        return files.size();
    }
};

GsReplacementStore &GsReplacementStore::instance() {
    static GsReplacementStore store;
    return store;
}

GsReplacementStore::GsReplacementStore() : m_impl(std::make_unique<Impl>()) {
    const char *log = std::getenv("RG_TEXTURE_LOG");
    m_logging = log && *log && *log != '0';
}

void GsReplacementStore::setEnabled(bool on) {
    if (m_impl->enabled.exchange(on) != on) {
        m_impl->bump();
        std::fprintf(stderr, "[texrep] replacements %s\n", on ? "on" : "off");
    }
}

bool GsReplacementStore::enabled() const {
    return m_impl->enabled.load(std::memory_order_acquire);
}

void GsReplacementStore::setImageLoader(GsReplacementImageLoader loader) {
    std::lock_guard lock(m_impl->mutex);
    m_impl->loader = std::move(loader);
}

void GsReplacementStore::setPackDirectory(const std::string &directory) {
    {
        std::lock_guard lock(m_impl->mutex);
        m_impl->directory = directory;
    }
    rescan();
}

size_t GsReplacementStore::rescan() {
    size_t count = 0u;
    {
        std::lock_guard lock(m_impl->mutex);
        count = m_impl->scanLocked();
        if (count != 0u)
            std::fprintf(stderr, "[texrep] %zu replacement file(s) in %s\n", count, m_impl->directory.c_str());
    }
    m_impl->bump();
    return count;
}

size_t GsReplacementStore::fileCount() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->files.size();
}

void GsReplacementStore::setGenerated(const std::string &name, std::vector<GsReplacementKey> keys,
                                      GsReplacementGenerator generator, uint32_t maxScale) {
    {
        std::lock_guard lock(m_impl->mutex);
        Impl::Generated &entry = m_impl->generated[name];
        entry.keys = std::move(keys);
        entry.generator = std::move(generator);
        entry.maxScale = std::clamp<uint32_t>(maxScale, 1u, 8u);
        entry.drawn.clear();
        entry.failedLogged = false;
    }
    m_impl->bump();
}

void GsReplacementStore::removeGenerated(const std::string &name) {
    {
        std::lock_guard lock(m_impl->mutex);
        if (m_impl->generated.erase(name) == 0u)
            return;
    }
    m_impl->bump();
}

void GsReplacementStore::refreshGenerated(const std::string &name) {
    {
        std::lock_guard lock(m_impl->mutex);
        const auto found = m_impl->generated.find(name);
        if (found == m_impl->generated.end())
            return;
        found->second.drawn.clear();
        found->second.failedLogged = false;
    }
    m_impl->bump();
}

uint64_t GsReplacementStore::generation() const {
    return m_impl->generation.load(std::memory_order_acquire);
}

std::vector<GsReplacementShape> GsReplacementStore::shapes(uint32_t psm, uint32_t width, uint32_t height) const {
    std::vector<GsReplacementShape> result;
    if (!enabled())
        return result;
    std::lock_guard lock(m_impl->mutex);
    const auto add = [&](const GsReplacementKey &key) {
        if (key.psm != psm || key.width > width || key.height > height)
            return;
        for (const GsReplacementShape &shape : result)
            if (shape.width == key.width && shape.height == key.height)
                return;
        result.push_back({key.width, key.height});
    };
    for (const auto &[key, file] : m_impl->files)
        add(key);
    for (const auto &[name, entry] : m_impl->generated)
        for (const GsReplacementKey &key : entry.keys)
            add(key);
    // The largest first: a whole texture before a part of it.
    std::sort(result.begin(), result.end(), [](const GsReplacementShape &a, const GsReplacementShape &b) {
        return a.width * a.height > b.width * b.height;
    });
    return result;
}

bool GsReplacementStore::picture(const GsReplacementKey &key, uint32_t renderScale, const uint8_t *native,
                                 GsReplacementPicture &out) {
    if (!enabled())
        return false;
    std::unique_lock lock(m_impl->mutex);
    GsReplacementKey any = key;
    any.clut = 0u;

    // A file first, for this palette and then for any.
    for (const GsReplacementKey &probe : {key, any}) {
        const auto found = m_impl->files.find(probe);
        if (found == m_impl->files.end())
            continue;
        Impl::File &file = found->second;
        if (file.failed)
            break;
        if (!file.image) {
            const auto start = Clock::now();
            auto image = std::make_shared<GsReplacementImage>();
            uint32_t w = 0u, h = 0u;
            if (!m_impl->loader || !m_impl->loader(file.path, w, h, image->rgba) || w == 0u || h == 0u) {
                std::fprintf(stderr, "[texrep] %s: could not be read; the game's texture stays\n", file.path.c_str());
                file.failed = true;
                break;
            }
            const uint32_t scale = w / key.width;
            if (scale < 1u || scale > 8u || w != key.width * scale || h != key.height * scale) {
                std::fprintf(stderr,
                             "[texrep] %s: %ux%u is not 1x to 8x of the texture's %ux%u; the game's texture stays\n",
                             file.path.c_str(), w, h, key.width, key.height);
                file.failed = true;
                break;
            }
            // Files are drawn with 255 opaque; the GS's opaque is 0x80.
            for (size_t i = 3u; i < image->rgba.size(); i += 4u)
                image->rgba[i] = static_cast<uint8_t>((image->rgba[i] * 128u + 127u) / 255u);
            image->width = w;
            image->height = h;
            file.image = std::move(image);
            file.scale = scale;
            std::fprintf(stderr, "[texrep] read %s (%ux%u, %ux) in %.1f ms\n", file.path.c_str(), w, h, scale,
                         std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        }
        out = {file.image, file.scale, std::filesystem::path(file.path).filename().string()};
        return true;
    }

    for (auto &[name, entry] : m_impl->generated) {
        const bool listed = std::any_of(entry.keys.begin(), entry.keys.end(), [&](const GsReplacementKey &listedKey) {
            return listedKey == key || listedKey == any;
        });
        if (!listed)
            continue;
        const uint32_t scale = std::clamp<uint32_t>(renderScale, 1u, entry.maxScale);
        const auto cacheKey = std::make_pair(key, scale);
        if (const auto drawn = entry.drawn.find(cacheKey); drawn != entry.drawn.end()) {
            out = {drawn->second, scale, name};
            return true;
        }
        // Drawn outside the lock: a font takes some milliseconds.
        GsReplacementGenerator generator = entry.generator;
        const std::string label = name;
        lock.unlock();
        const auto start = Clock::now();
        auto image = std::make_shared<GsReplacementImage>();
        bool ok = false;
        try {
            ok = generator && generator(scale, native, key.width, key.height, *image) &&
                 image->width == key.width * scale && image->height == key.height * scale &&
                 image->rgba.size() == static_cast<size_t>(image->width) * image->height * 4u;
        } catch (const std::exception &e) {
            std::fprintf(stderr, "[texrep] %s: %s\n", label.c_str(), e.what());
            ok = false;
        }
        lock.lock();
        const auto again = m_impl->generated.find(label);
        if (again == m_impl->generated.end())
            return false;
        if (!ok) {
            if (!again->second.failedLogged)
                std::fprintf(stderr, "[texrep] %s: not drawn; the game's texture stays\n", label.c_str());
            again->second.failedLogged = true;
            return false;
        }
        std::fprintf(stderr, "[texrep] drew %s for %s at %ux in %.1f ms\n", label.c_str(), hex(key.texels).c_str(),
                     scale, std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        std::shared_ptr<const GsReplacementImage> shared = std::move(image);
        again->second.drawn[cacheKey] = shared;
        out = {shared, scale, label};
        return true;
    }
    return false;
}

} // namespace dq8::gfx
