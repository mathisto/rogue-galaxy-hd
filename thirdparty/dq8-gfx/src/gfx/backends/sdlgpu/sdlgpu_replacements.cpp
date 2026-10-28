#include "gfx/backends/sdlgpu/sdlgpu_replacements.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <thread>
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

std::string envString(const char *name) {
    const char *value = std::getenv(name);
    return value ? value : "";
}

bool isPowerOfTwo(uint32_t value) {
    return value != 0u && (value & (value - 1u)) == 0u;
}

// One level half the size of `source` (w x h, both even): alpha-weighted
// colour, so transparent texels' colour does not darken the edges.
std::vector<uint8_t> halve(const std::vector<uint8_t> &source, uint32_t w, uint32_t h, bool cutout) {
    const uint32_t hw = w / 2u, hh = h / 2u;
    std::vector<uint8_t> out(static_cast<size_t>(hw) * hh * 4u);
    for (uint32_t y = 0u; y < hh; ++y) {
        for (uint32_t x = 0u; x < hw; ++x) {
            uint32_t sum[3] = {0u, 0u, 0u}, plain[3] = {0u, 0u, 0u}, alpha = 0u;
            for (uint32_t dy = 0u; dy < 2u; ++dy)
                for (uint32_t dx = 0u; dx < 2u; ++dx) {
                    const uint8_t *p = source.data() + ((static_cast<size_t>(y) * 2u + dy) * w + x * 2u + dx) * 4u;
                    for (int c = 0; c < 3; ++c) {
                        sum[c] += p[c] * static_cast<uint32_t>(p[3]);
                        plain[c] += p[c];
                    }
                    alpha += p[3];
                }
            uint8_t *q = out.data() + (static_cast<size_t>(y) * hw + x) * 4u;
            for (int c = 0; c < 3; ++c)
                q[c] = static_cast<uint8_t>(alpha ? (sum[c] + alpha / 2u) / alpha : (plain[c] + 2u) / 4u);
            uint32_t a = (alpha + 2u) / 4u;
            // A cut-out keeps a hard edge at every level (GS alpha 0x80
            // opaque; the game's alpha tests sit near half of that).
            if (cutout)
                a = a >= 0x40u ? 0x80u : 0u;
            q[3] = static_cast<uint8_t>(a);
        }
    }
    return out;
}

} // namespace

struct GsReplacementStore::Impl {
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::atomic<bool> enabled{true};
    std::atomic<bool> filesEnabled{true};
    std::atomic<uint64_t> generation{1u};
    std::atomic<uint64_t> completions{1u};
    GsReplacementImageLoader loader;
    GsReplacementImageWriter writer;
    std::string directory;
    std::atomic<uint64_t> budget{2048ull << 20u};
    std::atomic<bool> mipmaps{true};
    std::atomic<int> cutoutAlpha{static_cast<int>(GsCutoutAlpha::Threshold)};

    enum class State { Idle, Queued, Ready, Failed };
    struct File {
        std::string path;
        uintmax_t size = 0u;
        int64_t mtime = 0;
        State state = State::Idle;
        std::shared_ptr<const GsReplacementImage> image; // while in the decoded cache
        uint32_t scale = 0u;
        bool cutout = false;  // asked for with a cut-out's alpha
        std::list<GsReplacementKey>::iterator lru;
        bool inLru = false;
    };
    std::map<GsReplacementKey, File, KeyLess> files;
    // Decoded pictures, most recently used first.
    std::list<GsReplacementKey> lru;
    size_t cacheBytes = 0u;
    // (psm, width, height) of every file and generated key: shapes() looks
    // here, not through thousands of files.
    std::set<std::tuple<uint32_t, uint32_t, uint32_t>> shapeIndex;

    struct Generated {
        std::vector<GsReplacementKey> keys;
        GsReplacementGenerator generator;
        uint32_t maxScale = 4u;
        // Drawn over a pack file's picture too (setGenerated's overFiles),
        // and over the other generated pictures of the texture.
        bool overFiles = false;
    };
    std::map<std::string, Generated> generated;
    // What the generated pictures made of a texture at a scale: drawn from
    // its texels, or over `base` (a file's picture); cleared when they change.
    struct Composed {
        std::shared_ptr<const GsReplacementImage> base;
        std::shared_ptr<const GsReplacementImage> image;
        std::string label;
    };
    std::map<std::pair<GsReplacementKey, uint32_t>, Composed, DrawnLess> composed;

    // With the lock held, which it drops while drawing (a font takes some
    // milliseconds). True with `out` set when something was drawn: the
    // pictures drawn from the texture's own texels first, then the ones that
    // draw over (over `base` alone when it is given).
    bool composeLocked(std::unique_lock<std::mutex> &lock, const GsReplacementKey &key, uint32_t scale,
                       const uint8_t *native, const std::shared_ptr<const GsReplacementImage> &base,
                       GsReplacementPicture &out) {
        GsReplacementKey any = key;
        any.clut = 0u;
        const auto cacheKey = std::make_pair(key, scale);
        if (const auto done = composed.find(cacheKey); done != composed.end() && done->second.base == base) {
            if (!done->second.image)
                return false;
            out = {done->second.image, scale, done->second.label};
            return true;
        }
        std::vector<std::pair<std::string, GsReplacementGenerator>> steps;
        for (int pass = 0; pass < 2; ++pass)
            for (const auto &[name, entry] : generated) {
                if (entry.overFiles != (pass == 1) || (base && !entry.overFiles))
                    continue;
                if (std::any_of(entry.keys.begin(), entry.keys.end(), [&](const GsReplacementKey &listedKey) {
                        return listedKey == key || listedKey == any;
                    }))
                    steps.push_back({name, entry.generator});
            }
        if (steps.empty() || !native)
            return false;
        const uint64_t seenGeneration = generation.load();
        lock.unlock();
        const auto start = Clock::now();
        auto image = std::make_shared<GsReplacementImage>();
        if (base) {
            image->width = base->width;
            image->height = base->height;
            image->rgba = base->rgba;
        }
        std::string label;
        for (const auto &[name, generator] : steps) {
            bool ok = false;
            try {
                ok = generator && generator(scale, native, key.width, key.height, *image) &&
                     image->width == key.width * scale && image->height == key.height * scale &&
                     image->rgba.size() == static_cast<size_t>(image->width) * image->height * 4u;
            } catch (const std::exception &e) {
                std::fprintf(stderr, "[texrep] %s: %s\n", name.c_str(), e.what());
            }
            if (ok)
                label += (label.empty() ? "" : "+") + name;
            else
                std::fprintf(stderr, "[texrep] %s: not drawn for %s\n", name.c_str(), hex(key.texels).c_str());
        }
        lock.lock();
        const bool drew = !label.empty();
        if (drew)
            std::fprintf(stderr, "[texrep] drew %s for %s at %ux in %.1f ms\n", label.c_str(), hex(key.texels).c_str(),
                         scale, std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        std::shared_ptr<const GsReplacementImage> shared;
        if (drew)
            shared = std::move(image);
        if (generation.load() == seenGeneration)
            composed[cacheKey] = {base, shared, label};
        if (!drew)
            return false;
        out = {shared, scale, label};
        return true;
    }

    // Work for the threads.
    std::deque<GsReplacementKey> decodeQueue;
    bool scanWanted = false;
    struct LearnJob {
        std::string name;
        uint32_t width = 0u, height = 0u, tbp0 = 0u;
        std::vector<uint8_t> rgba;
    };
    std::deque<LearnJob> learnQueue;
    std::set<std::string> learnedNames;
    std::string learnDirectory;
    uint64_t learnDropped = 0u;
    bool stopping = false;
    std::vector<std::thread> threads;
    std::thread poller;

    // Counters.
    uint64_t reads = 0u;
    double readMs = 0.0;
    uint64_t evicted = 0u;
    uint64_t requests = 0u;
    uint64_t misses = 0u;
    uint64_t learned = 0u;
    size_t failed = 0u;
    std::atomic<uint64_t> gpuBytes{0u};
    std::atomic<uint64_t> gpuTextures{0u};

    void bump() { generation.fetch_add(1u, std::memory_order_acq_rel); }

    void rebuildShapesLocked() {
        shapeIndex.clear();
        for (const auto &[key, file] : files)
            shapeIndex.insert({key.psm, key.width, key.height});
        for (const auto &[name, entry] : generated)
            for (const GsReplacementKey &key : entry.keys)
                shapeIndex.insert({key.psm, key.width, key.height});
    }

    void dropImageLocked(File &file) {
        if (file.inLru) {
            lru.erase(file.lru);
            file.inLru = false;
        }
        if (file.image)
            cacheBytes -= std::min(cacheBytes, file.image->bytes());
        file.image.reset();
    }

    void evictLocked(const GsReplacementKey *keep) {
        const uint64_t limit = budget.load() / 2u;
        while (cacheBytes > limit && !lru.empty()) {
            const GsReplacementKey victim = lru.back();
            if (keep && victim == *keep && lru.size() == 1u)
                break;
            auto found = files.find(victim);
            if (found == files.end()) {
                lru.pop_back();
                continue;
            }
            dropImageLocked(found->second);
            found->second.state = State::Idle;
            ++evicted;
        }
    }

    // The folder listed (names only); runs on a worker, the map swapped in
    // under the lock. Files that did not change keep their decoded picture.
    void scan() {
        std::string root;
        {
            std::lock_guard lock(mutex);
            root = directory;
        }
        const auto start = Clock::now();
        std::map<GsReplacementKey, File, KeyLess> found;
        size_t misnamed = 0u;
        std::error_code error;
        if (!root.empty() && std::filesystem::is_directory(root, error)) {
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
                    if (++misnamed <= 8u)
                        std::fprintf(stderr, "[texrep] %s: not a replacement name (texels_clut_WxH_psmNN.png)\n",
                                     it->path().string().c_str());
                    continue;
                }
                File file;
                file.path = it->path().string();
                file.size = it->file_size(error);
                file.mtime = static_cast<int64_t>(it->last_write_time(error).time_since_epoch().count());
                found[key] = std::move(file);
            }
        }
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        {
            std::lock_guard lock(mutex);
            size_t kept = 0u;
            for (auto &[key, file] : files) {
                auto again = found.find(key);
                if (again != found.end() && again->second.path == file.path && again->second.size == file.size &&
                    again->second.mtime == file.mtime && file.state != State::Queued) {
                    // Unchanged: keep what was read.
                    again->second.state = file.state == State::Failed ? State::Failed : file.state;
                    again->second.image = file.image;
                    again->second.scale = file.scale;
                    again->second.cutout = file.cutout;
                    if (file.image)
                        ++kept;
                }
            }
            // The decoded cache follows the new map.
            lru.clear();
            cacheBytes = 0u;
            failed = 0u;
            files = std::move(found);
            for (auto &[key, file] : files) {
                if (file.state == State::Failed)
                    ++failed;
                if (!file.image) {
                    if (file.state == State::Ready)
                        file.state = State::Idle;
                    continue;
                }
                lru.push_front(key);
                file.lru = lru.begin();
                file.inLru = true;
                cacheBytes += file.image->bytes();
            }
            decodeQueue.clear();
            rebuildShapesLocked();
            if (!files.empty() || misnamed)
                std::fprintf(stderr, "[texrep] %zu replacement file(s) in %s, listed in %.0f ms (%zu kept read%s)\n",
                             files.size(), directory.c_str(), ms, kept,
                             misnamed ? ", some files misnamed" : "");
        }
        bump();
    }

    void decode(const GsReplacementKey key) {
        std::string path;
        bool cutout = false;
        GsReplacementImageLoader load;
        {
            std::lock_guard lock(mutex);
            auto found = files.find(key);
            if (found == files.end() || found->second.state != State::Queued)
                return;
            path = found->second.path;
            cutout = found->second.cutout;
            load = loader;
        }
        const auto start = Clock::now();
        auto image = std::make_shared<GsReplacementImage>();
        uint32_t w = 0u, h = 0u, scale = 0u;
        bool ok = false;
        std::string why;
        try {
            ok = load && load(path, w, h, image->rgba) && w != 0u && h != 0u;
            if (!ok)
                why = "could not be read";
        } catch (const std::exception &e) {
            ok = false;
            why = e.what();
        }
        if (ok) {
            scale = w / key.width;
            if (scale < 1u || scale > 8u || w != key.width * scale || h != key.height * scale) {
                char text[160];
                std::snprintf(text, sizeof(text), "%ux%u is not 1x to 8x of the texture's %ux%u", w, h, key.width,
                              key.height);
                why = text;
                ok = false;
            }
        }
        if (ok) {
            const bool threshold = cutout && cutoutAlpha.load() == static_cast<int>(GsCutoutAlpha::Threshold);
            // Files are drawn with 255 opaque; the GS's opaque is 0x80.
            for (size_t i = 3u; i < image->rgba.size(); i += 4u) {
                const uint32_t a = image->rgba[i];
                image->rgba[i] = threshold ? static_cast<uint8_t>(a >= 128u ? 0x80u : 0u)
                                           : static_cast<uint8_t>((a * 128u + 127u) / 255u);
            }
            image->width = w;
            image->height = h;
            if (mipmaps.load() && scale >= 2u && isPowerOfTwo(scale)) {
                uint32_t lw = w, lh = h;
                const std::vector<uint8_t> *previous = &image->rgba;
                for (uint32_t s = scale; s > 1u; s /= 2u) {
                    image->mips.push_back(halve(*previous, lw, lh, threshold));
                    lw /= 2u;
                    lh /= 2u;
                    previous = &image->mips.back();
                }
            }
        }
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        {
            std::lock_guard lock(mutex);
            ++reads;
            readMs += ms;
            auto found = files.find(key);
            if (found != files.end() && found->second.path == path && found->second.state == State::Queued) {
                File &file = found->second;
                if (ok) {
                    file.image = std::move(image);
                    file.scale = scale;
                    file.state = State::Ready;
                    lru.push_front(key);
                    file.lru = lru.begin();
                    file.inLru = true;
                    cacheBytes += file.image->bytes();
                    evictLocked(&key);
                } else {
                    file.state = State::Failed;
                    ++failed;
                    std::fprintf(stderr, "[texrep] %s: %s; the game's texture stays\n", path.c_str(), why.c_str());
                }
            }
        }
        if (ok && std::getenv("RG_TEXTURE_LOG"))
            std::fprintf(stderr, "[texrep] read %s (%ux%u, %ux%s) in %.1f ms\n", path.c_str(), w, h, scale,
                         cutout ? ", cut-out" : "", ms);
        completions.fetch_add(1u, std::memory_order_acq_rel);
    }

    void writeLearned(LearnJob &job) {
        GsReplacementImageWriter write;
        std::string dir;
        {
            std::lock_guard lock(mutex);
            write = writer;
            dir = learnDirectory;
        }
        if (!write)
            return;
        for (size_t i = 3u; i < job.rgba.size(); i += 4u)
            job.rgba[i] = static_cast<uint8_t>(std::min<uint32_t>(255u, job.rgba[i] * 255u / 128u));
        const std::string path = dir + "/" + job.name;
        if (!write(path, job.width, job.height, job.rgba.data())) {
            std::fprintf(stderr, "[texrep] learn: could not write %s\n", path.c_str());
            return;
        }
        if (std::FILE *index = std::fopen((dir + "/index.tsv").c_str(), "ab")) {
            std::fprintf(index, "%s\t%05x\n", job.name.c_str(), job.tbp0);
            std::fclose(index);
        }
        std::lock_guard lock(mutex);
        ++learned;
    }

    void work() {
        for (;;) {
            std::unique_lock lock(mutex);
            wake.wait(lock, [&] {
                return stopping || scanWanted || !decodeQueue.empty() || !learnQueue.empty();
            });
            if (stopping)
                return;
            if (scanWanted) {
                scanWanted = false;
                lock.unlock();
                scan();
                continue;
            }
            if (!decodeQueue.empty()) {
                const GsReplacementKey key = decodeQueue.front();
                decodeQueue.pop_front();
                lock.unlock();
                decode(key);
                continue;
            }
            LearnJob job = std::move(learnQueue.front());
            learnQueue.pop_front();
            lock.unlock();
            writeLearned(job);
        }
    }

    void startThreadsLocked() {
        if (!threads.empty())
            return;
        int count = 2;
        if (const std::string value = envString("RG_TEXTURE_THREADS"); !value.empty())
            count = std::clamp(std::atoi(value.c_str()), 1, 8);
        for (int i = 0; i < count; ++i)
            threads.emplace_back([this] { work(); });
        // RG_TEXTURE_POLL=N: the folder is checked every N seconds and
        // listed again when a file was added, removed or changed.
        if (const int seconds = std::atoi(envString("RG_TEXTURE_POLL").c_str()); seconds > 0)
            poller = std::thread([this, seconds] { poll(seconds); });
    }

    uint64_t signature() {
        std::string root;
        {
            std::lock_guard lock(mutex);
            root = directory;
        }
        uint64_t hash = 1469598103934665603ull;
        std::error_code error;
        if (root.empty() || !std::filesystem::is_directory(root, error))
            return 0u;
        for (auto it = std::filesystem::recursive_directory_iterator(root, error);
             !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
            const std::string name = it->path().string();
            for (const char ch : name)
                hash = (hash ^ static_cast<uint8_t>(ch)) * 1099511628211ull;
            hash = (hash ^ static_cast<uint64_t>(it->last_write_time(error).time_since_epoch().count())) *
                   1099511628211ull;
        }
        return hash;
    }

    void poll(int seconds) {
        uint64_t last = signature();
        for (;;) {
            {
                std::unique_lock lock(mutex);
                if (wake.wait_for(lock, std::chrono::seconds(seconds), [&] { return stopping; }))
                    return;
            }
            const uint64_t now = signature();
            if (now != last) {
                last = now;
                std::fprintf(stderr, "[texrep] the pack folder changed: listing it again\n");
                std::lock_guard lock(mutex);
                scanWanted = true;
                wake.notify_all();
            }
        }
    }
};

GsReplacementStore &GsReplacementStore::instance() {
    static GsReplacementStore store;
    return store;
}

GsReplacementStore::GsReplacementStore() : m_impl(std::make_unique<Impl>()) {
    const char *log = std::getenv("RG_TEXTURE_LOG");
    m_logging = log && *log && *log != '0';
    if (const std::string budget = envString("RG_TEXTURE_BUDGET_MB"); !budget.empty())
        setBudgetMegabytes(static_cast<uint32_t>(std::max(0, std::atoi(budget.c_str()))));
    if (envString("RG_TEXTURE_MIPMAPS") == "0")
        m_impl->mipmaps = false;
    if (envString("RG_TEXTURE_CUTOUT") == "soft")
        m_impl->cutoutAlpha = static_cast<int>(GsCutoutAlpha::Soft);
    if (const std::string learn = envString("RG_TEXTURE_LEARN"); !learn.empty()) {
        std::error_code error;
        std::filesystem::create_directories(learn, error);
        m_impl->learnDirectory = learn;
        m_learning = true;
        // Seeded with what an earlier session wrote, so a re-run adds only
        // new textures.
        for (auto it = std::filesystem::directory_iterator(learn, error);
             !error && it != std::filesystem::directory_iterator(); it.increment(error))
            m_impl->learnedNames.insert(it->path().filename().string());
    }
}

GsReplacementStore::~GsReplacementStore() {
    {
        std::lock_guard lock(m_impl->mutex);
        m_impl->stopping = true;
        m_impl->decodeQueue.clear();
    }
    m_impl->wake.notify_all();
    for (std::thread &thread : m_impl->threads)
        if (thread.joinable())
            thread.join();
    if (m_impl->poller.joinable())
        m_impl->poller.join();
}

void GsReplacementStore::setEnabled(bool on) {
    if (m_impl->enabled.exchange(on) != on) {
        m_impl->bump();
        std::fprintf(stderr, "[texrep] replacements %s\n", on ? "on" : "off");
    }
}

void GsReplacementStore::setFilesEnabled(bool on) {
    if (m_impl->filesEnabled.exchange(on) != on) {
        m_impl->bump();
        std::fprintf(stderr, "[texrep] pack files %s\n", on ? "on" : "off");
    }
}

bool GsReplacementStore::filesEnabled() const {
    return m_impl->filesEnabled.load();
}

bool GsReplacementStore::enabled() const {
    return m_impl->enabled.load(std::memory_order_acquire);
}

void GsReplacementStore::setImageLoader(GsReplacementImageLoader loader) {
    std::lock_guard lock(m_impl->mutex);
    m_impl->loader = std::move(loader);
}

void GsReplacementStore::setImageWriter(GsReplacementImageWriter writer) {
    std::lock_guard lock(m_impl->mutex);
    m_impl->writer = std::move(writer);
}

void GsReplacementStore::setPackDirectory(const std::string &directory) {
    {
        std::lock_guard lock(m_impl->mutex);
        m_impl->directory = directory;
    }
    reload();
}

std::string GsReplacementStore::packDirectory() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->directory;
}

void GsReplacementStore::reload() {
    {
        std::lock_guard lock(m_impl->mutex);
        m_impl->scanWanted = true;
        m_impl->startThreadsLocked();
    }
    m_impl->wake.notify_all();
}

size_t GsReplacementStore::fileCount() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->files.size();
}

void GsReplacementStore::setBudgetMegabytes(uint32_t megabytes) {
    const uint64_t bytes = static_cast<uint64_t>(std::clamp<uint32_t>(megabytes, 64u, 65536u)) << 20u;
    if (m_impl->budget.exchange(bytes) == bytes)
        return;
    std::lock_guard lock(m_impl->mutex);
    m_impl->evictLocked(nullptr);
}

uint64_t GsReplacementStore::budgetBytes() const {
    return m_impl->budget.load();
}

uint64_t GsReplacementStore::cacheBudgetBytes() const {
    return m_impl->budget.load() / 2u;
}

bool GsReplacementStore::mipmaps() const {
    return m_impl->mipmaps.load();
}

void GsReplacementStore::setMipmaps(bool on) {
    if (m_impl->mipmaps.exchange(on) == on)
        return;
    // Pictures read with the other setting are read again.
    {
        std::lock_guard lock(m_impl->mutex);
        for (auto &[key, file] : m_impl->files)
            if (file.state == Impl::State::Ready) {
                m_impl->dropImageLocked(file);
                file.state = Impl::State::Idle;
            }
    }
    m_impl->bump();
}

GsCutoutAlpha GsReplacementStore::cutoutAlpha() const {
    return static_cast<GsCutoutAlpha>(m_impl->cutoutAlpha.load());
}

void GsReplacementStore::setCutoutAlpha(GsCutoutAlpha mode) {
    if (m_impl->cutoutAlpha.exchange(static_cast<int>(mode)) == static_cast<int>(mode))
        return;
    {
        std::lock_guard lock(m_impl->mutex);
        for (auto &[key, file] : m_impl->files)
            if (file.state == Impl::State::Ready && file.cutout) {
                m_impl->dropImageLocked(file);
                file.state = Impl::State::Idle;
            }
    }
    m_impl->bump();
}

void GsReplacementStore::setGenerated(const std::string &name, std::vector<GsReplacementKey> keys,
                                      GsReplacementGenerator generator, uint32_t maxScale, bool overFiles) {
    {
        std::lock_guard lock(m_impl->mutex);
        Impl::Generated &entry = m_impl->generated[name];
        entry.keys = std::move(keys);
        entry.generator = std::move(generator);
        entry.maxScale = std::clamp<uint32_t>(maxScale, 1u, 8u);
        entry.overFiles = overFiles;
        m_impl->composed.clear();
        m_impl->rebuildShapesLocked();
        m_impl->bump();
    }
    m_impl->bump();
}

void GsReplacementStore::removeGenerated(const std::string &name) {
    {
        std::lock_guard lock(m_impl->mutex);
        if (m_impl->generated.erase(name) == 0u)
            return;
        m_impl->composed.clear();
        m_impl->rebuildShapesLocked();
        m_impl->bump();
    }
    m_impl->bump();
}

void GsReplacementStore::refreshGenerated(const std::string &name) {
    {
        std::lock_guard lock(m_impl->mutex);
        const auto found = m_impl->generated.find(name);
        if (found == m_impl->generated.end())
            return;
        m_impl->composed.clear();
        m_impl->bump();
    }
    m_impl->bump();
}

uint64_t GsReplacementStore::generation() const {
    return m_impl->generation.load(std::memory_order_acquire);
}

uint64_t GsReplacementStore::completions() const {
    return m_impl->completions.load(std::memory_order_acquire);
}

std::vector<GsReplacementShape> GsReplacementStore::shapes(uint32_t psm, uint32_t width, uint32_t height) const {
    std::vector<GsReplacementShape> result;
    if (!enabled())
        return result;
    std::lock_guard lock(m_impl->mutex);
    for (auto it = m_impl->shapeIndex.lower_bound({psm, 0u, 0u});
         it != m_impl->shapeIndex.end() && std::get<0>(*it) == psm; ++it) {
        const uint32_t w = std::get<1>(*it), h = std::get<2>(*it);
        if (w <= width && h <= height)
            result.push_back({w, h});
    }
    // The largest first: a whole texture before a part of it.
    std::sort(result.begin(), result.end(), [](const GsReplacementShape &a, const GsReplacementShape &b) {
        return a.width * a.height > b.width * b.height;
    });
    return result;
}

bool GsReplacementStore::has(const GsReplacementKey &key) const {
    if (!enabled())
        return false;
    GsReplacementKey any = key;
    any.clut = 0u;
    std::lock_guard lock(m_impl->mutex);
    for (const GsReplacementKey &probe : {key, any}) {
        if (!m_impl->filesEnabled.load())
            break;
        const auto found = m_impl->files.find(probe);
        if (found != m_impl->files.end() && found->second.state != Impl::State::Failed)
            return true;
    }
    for (const auto &[name, entry] : m_impl->generated)
        for (const GsReplacementKey &listed : entry.keys)
            if (listed == key || listed == any)
                return true;
    ++const_cast<Impl &>(*m_impl).misses;
    return false;
}

bool GsReplacementStore::drawsOver(const GsReplacementKey &key) const {
    GsReplacementKey any = key;
    any.clut = 0u;
    std::lock_guard lock(m_impl->mutex);
    for (const auto &[name, entry] : m_impl->generated)
        if (entry.overFiles)
            for (const GsReplacementKey &listed : entry.keys)
                if (listed == key || listed == any)
                    return true;
    return false;
}

GsPictureStatus GsReplacementStore::picture(const GsReplacementKey &key, uint32_t renderScale, const uint8_t *native,
                                            GsReplacementPicture &out, bool cutout) {
    if (!enabled())
        return GsPictureStatus::None;
    std::unique_lock lock(m_impl->mutex);
    GsReplacementKey any = key;
    any.clut = 0u;

    // A file first, for this palette and then for any.
    for (const GsReplacementKey &probe : {key, any}) {
        if (!m_impl->filesEnabled.load())
            break;
        const auto found = m_impl->files.find(probe);
        if (found == m_impl->files.end())
            continue;
        Impl::File &file = found->second;
        if (file.state == Impl::State::Failed)
            break;
        ++m_impl->requests;
        if (file.state == Impl::State::Ready && file.image) {
            if (file.inLru && file.lru != m_impl->lru.begin())
                m_impl->lru.splice(m_impl->lru.begin(), m_impl->lru, file.lru);
            out = {file.image, file.scale, std::filesystem::path(file.path).filename().string()};
            // Generated pictures that draw over files (the numerals, the
            // Xbox buttons over the pack's sheet) get the file's picture as
            // their canvas.
            const std::string label = out.label;
            if (m_impl->composeLocked(lock, key, file.scale, native, file.image, out))
                out.label += " over " + label;
            return GsPictureStatus::Ready;
        }
        if (file.state != Impl::State::Queued) {
            file.state = Impl::State::Queued;
            file.cutout = cutout;
            m_impl->decodeQueue.push_back(probe);
            m_impl->startThreadsLocked();
            m_impl->wake.notify_one();
        }
        return GsPictureStatus::Pending;
    }

    // No file: the generated pictures that list the texture, one over the
    // other.
    uint32_t maxScale = 0u;
    for (const auto &[name, entry] : m_impl->generated)
        if (std::any_of(entry.keys.begin(), entry.keys.end(),
                        [&](const GsReplacementKey &listedKey) { return listedKey == key || listedKey == any; }))
            maxScale = std::max(maxScale, entry.maxScale);
    if (maxScale == 0u) {
        ++m_impl->misses;
        return GsPictureStatus::None;
    }
    const uint32_t scale = std::clamp<uint32_t>(renderScale, 1u, maxScale);
    if (m_impl->composeLocked(lock, key, scale, native, nullptr, out))
        return GsPictureStatus::Ready;
    return GsPictureStatus::None;
}

void GsReplacementStore::learn(const GsReplacementKey &key, uint32_t tbp0, std::vector<uint8_t> rgba) {
    if (!m_learning || rgba.size() != static_cast<size_t>(key.width) * key.height * 4u)
        return;
    char name[96];
    std::snprintf(name, sizeof(name), "%016llx_%016llx_%ux%u_psm%02x.png", static_cast<unsigned long long>(key.texels),
                  static_cast<unsigned long long>(key.clut), key.width, key.height, key.psm);
    {
        std::lock_guard lock(m_impl->mutex);
        if (!m_impl->learnedNames.insert(name).second)
            return;
        // A burst (a scene load) waits in memory; past this many it is
        // dropped and tried again the next time the texture is built.
        if (m_impl->learnQueue.size() >= 512u) {
            m_impl->learnedNames.erase(name);
            ++m_impl->learnDropped;
            return;
        }
        m_impl->learnQueue.push_back({name, key.width, key.height, tbp0, std::move(rgba)});
        m_impl->startThreadsLocked();
    }
    m_impl->wake.notify_one();
}

void GsReplacementStore::noteGpu(uint64_t bytes, uint64_t textures) {
    m_impl->gpuBytes.store(bytes, std::memory_order_relaxed);
    m_impl->gpuTextures.store(textures, std::memory_order_relaxed);
}

GsReplacementStats GsReplacementStore::stats() const {
    std::lock_guard lock(m_impl->mutex);
    GsReplacementStats s;
    s.files = m_impl->files.size();
    s.decoded = m_impl->lru.size();
    s.cacheBytes = m_impl->cacheBytes;
    s.pending = m_impl->decodeQueue.size();
    s.failed = m_impl->failed;
    s.reads = m_impl->reads;
    s.readMs = m_impl->readMs;
    s.evicted = m_impl->evicted;
    s.requests = m_impl->requests;
    s.misses = m_impl->misses;
    s.learned = m_impl->learned;
    return s;
}

std::string GsReplacementStore::summary() const {
    const GsReplacementStats s = stats();
    char text[320];
    std::snprintf(text, sizeof(text),
                  "pack %zu files | read %llu (%.1f ms avg), waiting %zu, failed %zu | in memory %zu (%.0f MB), "
                  "dropped %llu | on GPU %llu (%.0f MB of %.0f) | lookups %llu found, %llu none%s",
                  s.files, static_cast<unsigned long long>(s.reads), s.reads ? s.readMs / double(s.reads) : 0.0,
                  s.pending, s.failed, s.decoded, double(s.cacheBytes) / 1048576.0,
                  static_cast<unsigned long long>(s.evicted),
                  static_cast<unsigned long long>(m_impl->gpuTextures.load()),
                  double(m_impl->gpuBytes.load()) / 1048576.0, double(budgetBytes()) / 1048576.0,
                  static_cast<unsigned long long>(s.requests), static_cast<unsigned long long>(s.misses),
                  m_learning ? (" | learned " + std::to_string(s.learned)).c_str() : "");
    return text;
}

} // namespace dq8::gfx
