#include "movie_hd.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libswscale/swscale.h>
}

#include "ps2_runtime.h"
#include "scene_timing.h"
#if defined(RG_HAS_SDLGPU)
#include "gfx/backends/sdlgpu/sdlgpu_video.h"
#endif

namespace rg::hdmovie
{
namespace
{

namespace fs = std::filesystem;

// MoviePlayerOpen(player, params): the game's callers (MoviePlayModal, the
// title overlay's TitleMoviePlay) call this, which jumps into MovieOpen
// (0x1D5CF0, never entered through the function table) with the source name
// at params + 0x80.
constexpr uint32_t kMoviePlayerOpen = 0x001D61A0u;
constexpr uint32_t kSourceName = 0x80u;
constexpr uint32_t kMovieDrawFrame = 0x001D5870u;    // MovieDrawFrame(player)
constexpr uint32_t kMoviePlayerShutdown = 0x001D56C0u;
// The game's libmpeg picture call (sceMpegGetPicture, whose body the runtime
// supplies) from VideoDecDecodeBs (0x1D29E0): when it leaves the mpeg object's
// word at +8 zero a new picture has been served, and the game uploads it to
// the movie texture next. The pictures served so far number the game's frames
// (MovieGetFrameCount's word, 0x4B3218, stays 0 here).
constexpr uint32_t kMpegGetPicture = 0x00111148u;
constexpr uint32_t kMpegNoPicture = 0x8u;
// The texture MovieDrawFrame draws from, 512 wide: the picture's bottom edge
// stays put in GS memory, so a 336-line movie sits at TBP 0x2E00 and a
// 448-line one (the title's, the credits') at 0x2A00.
constexpr uint32_t kMovieTbp336 = 0x2E00u;
constexpr uint32_t kMovieTbp448 = 0x2A00u;
constexpr uint32_t kGuestWidth = 512u;

// A frame that is late: how long the game may be showing the HD picture of an
// older frame (or none, once HD has been shown) before this movie falls back
// to its own picture for good.
constexpr std::chrono::milliseconds kLateLimit{500};
// How long MovieOpen waits for the decoder's first picture, so the first
// frame is already HD; and how long the very first picture may take at all.
constexpr std::chrono::milliseconds kFirstFrameWait{400};
constexpr std::chrono::milliseconds kFirstFrameLimit{3000};
// Pictures decoded ahead (each 512*scale x 336..448*scale RGBA, ~11-15 MB).
constexpr size_t kAhead = 6u;

std::atomic<bool> g_enabled{true};
std::atomic<bool> g_debug{false};
// The folder is resolved once at install; the menu's count is a scan of it.
std::string g_folder;
std::atomic<bool> g_forcedOff{false};
std::atomic<int> g_fileCount{-1};

PS2Runtime::RecompiledFunction g_openOriginal = nullptr;
PS2Runtime::RecompiledFunction g_drawOriginal = nullptr;
PS2Runtime::RecompiledFunction g_shutdownOriginal = nullptr;
PS2Runtime::RecompiledFunction g_getPictureOriginal = nullptr;
// Pictures the game has taken from the decoder since the movie opened.
std::atomic<uint32_t> g_served{0u};
// Counting only while an HD movie is open; the first call after the open.
std::atomic<bool> g_counting{false};
std::atomic<bool> g_entered{false};
// Debugging: the pictures to take a screenshot at (RG_HD_MOVIES_SHOT_AT).
std::vector<uint32_t> g_shotAt;
size_t g_shotNext = 0u;
bool g_shotActive = false; // for the movie in play

using Clock = std::chrono::steady_clock;

#if defined(RG_HAS_SDLGPU)

// Decodes one HD movie ahead of the game on its own thread. Pictures come out
// numbered from 0 in display order, already the size the backend wants.
class Player
{
  public:
    explicit Player(std::string path) : m_path(std::move(path)) {}

    ~Player()
    {
        stop();
    }

    bool start()
    {
        if (!open())
        {
            return false;
        }
        m_thread = std::thread([this] { run(); });
        return true;
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }
        m_wake.notify_all();
        if (m_thread.joinable())
        {
            m_thread.join();
        }
        close();
    }

    // Blocks until the first picture is ready, failed or `wait` passed.
    bool waitFirst(std::chrono::milliseconds wait)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_wake.wait_for(lock, wait, [this] { return !m_queue.empty() || m_done || m_failed; });
        return !m_queue.empty();
    }

    // The picture of game frame `index`: older pictures are dropped. Null when
    // it is not ready (or the decoder has run out).
    std::shared_ptr<const dq8::gfx::GsVideoFrame> take(uint32_t index)
    {
        std::shared_ptr<const dq8::gfx::GsVideoFrame> found;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            while (!m_queue.empty() && m_queue.front().index < index)
            {
                m_queue.pop_front();
                ++m_dropped;
            }
            if (!m_queue.empty() && m_queue.front().index == index)
            {
                found = std::move(m_queue.front().frame);
                m_queue.pop_front();
            }
        }
        m_wake.notify_all();
        return found;
    }

    bool failed() const
    {
        return m_failed.load();
    }
    bool finished() const
    {
        return m_done.load();
    }
    uint32_t decoded() const
    {
        return m_decoded.load();
    }
    uint32_t dropped() const
    {
        return m_dropped;
    }
    // Milliseconds a picture took to resample to RGBA, on average and at most.
    double convertAverageMs() const
    {
        return m_decoded.load() ? double(m_convertNanos) / 1e6 / double(m_decoded.load()) : 0.0;
    }
    double convertMaxMs() const
    {
        return double(m_convertMax) / 1e6;
    }
    uint32_t guestHeight() const
    {
        return m_guestHeight;
    }
    uint32_t scale() const
    {
        return m_scale;
    }
    const std::string &path() const
    {
        return m_path;
    }

  private:
    struct Slot
    {
        uint32_t index = 0u;
        std::shared_ptr<const dq8::gfx::GsVideoFrame> frame;
    };

    bool open()
    {
        if (avformat_open_input(&m_format, m_path.c_str(), nullptr, nullptr) < 0)
        {
            m_format = nullptr;
            return false;
        }
        if (avformat_find_stream_info(m_format, nullptr) < 0)
        {
            close();
            return false;
        }
        const AVCodec *codec = nullptr;
        m_stream = av_find_best_stream(m_format, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
        if (m_stream < 0 || !codec)
        {
            close();
            return false;
        }
        m_codec = avcodec_alloc_context3(codec);
        if (!m_codec || avcodec_parameters_to_context(m_codec, m_format->streams[m_stream]->codecpar) < 0)
        {
            close();
            return false;
        }
        m_codec->thread_count = 4;
        m_codec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        if (avcodec_open2(m_codec, codec, nullptr) < 0)
        {
            close();
            return false;
        }
        const int w = m_codec->width, h = m_codec->height;
        if (w <= 0 || h <= 0)
        {
            close();
            return false;
        }
        // The game's picture is 512x336 for a 16:9 movie and 512x448 for a
        // 4:3 one (the title, the credits): the shape of the file says which.
        const double shape = double(w) / double(h);
        m_guestHeight = shape > 1.55 ? 336u : 448u;
        // A whole multiple of the guest texture that keeps the file's width.
        m_scale = std::clamp<uint32_t>((uint32_t(w) + kGuestWidth - 1u) / kGuestWidth, 1u, 4u);
        const double expected = m_guestHeight == 336u ? 16.0 / 9.0 : 4.0 / 3.0;
        if (std::abs(shape - expected) > 0.05)
        {
            std::fprintf(stderr, "[hdmovie] %s: %dx%d is neither 16:9 nor 4:3; not used\n", m_path.c_str(), w, h);
            close();
            return false;
        }
        m_outWidth = kGuestWidth * m_scale;
        m_outHeight = m_guestHeight * m_scale;
        m_sws = sws_getContext(w, h, m_codec->pix_fmt, int(m_outWidth), int(m_outHeight), AV_PIX_FMT_RGBA,
                               SWS_BICUBIC, nullptr, nullptr, nullptr);
        m_packet = av_packet_alloc();
        m_picture = av_frame_alloc();
        if (!m_sws || !m_packet || !m_picture)
        {
            close();
            return false;
        }
        // The file's matrix and range: HEVC from the batch is BT.709 limited.
        const int *coefficients = sws_getCoefficients(SWS_CS_ITU709);
        sws_setColorspaceDetails(m_sws, coefficients, 0, coefficients, 1, 0, 1 << 16, 1 << 16);
        return true;
    }

    void close()
    {
        if (m_sws)
        {
            sws_freeContext(m_sws);
            m_sws = nullptr;
        }
        av_frame_free(&m_picture);
        av_packet_free(&m_packet);
        if (m_codec)
        {
            avcodec_free_context(&m_codec);
        }
        if (m_format)
        {
            avformat_close_input(&m_format);
        }
    }

    // Converts the decoded picture into a published-size RGBA frame.
    bool push(uint32_t index)
    {
        auto frame = std::make_shared<dq8::gfx::GsVideoFrame>();
        frame->tbp0 = m_guestHeight == 448u ? kMovieTbp448 : kMovieTbp336;
        frame->guestWidth = kGuestWidth;
        frame->guestHeight = m_guestHeight;
        frame->scale = m_scale;
        frame->width = m_outWidth;
        frame->height = m_outHeight;
        frame->serial = ++s_serial;
        frame->rgba.resize(size_t(m_outWidth) * m_outHeight * 4u);
        uint8_t *dst[4] = {frame->rgba.data(), nullptr, nullptr, nullptr};
        const int dstStride[4] = {int(m_outWidth * 4u), 0, 0, 0};
        const auto began = Clock::now();
        sws_scale(m_sws, m_picture->data, m_picture->linesize, 0, m_codec->height, dst, dstStride);
        // The GS's alpha range: 0x80 is opaque.
        uint32_t *pixels = reinterpret_cast<uint32_t *>(frame->rgba.data());
        for (size_t i = 0, n = size_t(m_outWidth) * m_outHeight; i < n; ++i)
        {
            pixels[i] = (pixels[i] & 0x00FFFFFFu) | 0x80000000u;
        }
        const int64_t nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - began).count();
        m_convertNanos += nanos;
        m_convertMax = std::max(m_convertMax, nanos);
        if (s_delayMs > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(s_delayMs));
        }
        std::unique_lock<std::mutex> lock(m_mutex);
        m_wake.wait(lock, [this] { return m_stop || m_queue.size() < kAhead; });
        if (m_stop)
        {
            return false;
        }
        m_queue.push_back({index, std::move(frame)});
        ++m_decoded;
        lock.unlock();
        m_wake.notify_all();
        return true;
    }

    // Receives every picture the decoder has ready.
    bool drain(uint32_t &index)
    {
        for (;;)
        {
            const int status = avcodec_receive_frame(m_codec, m_picture);
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF)
            {
                return true;
            }
            if (status < 0)
            {
                return false;
            }
            const bool ok = push(index++);
            av_frame_unref(m_picture);
            if (!ok)
            {
                return false;
            }
        }
    }

    void run()
    {
        uint32_t index = 0u;
        bool ok = true;
        while (ok)
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_stop)
                {
                    break;
                }
            }
            const int status = av_read_frame(m_format, m_packet);
            if (status < 0)
            {
                // End of the file: flush what the decoder still holds.
                avcodec_send_packet(m_codec, nullptr);
                ok = drain(index);
                if (index == 0u)
                {
                    m_failed = true;
                }
                break;
            }
            if (m_packet->stream_index == m_stream)
            {
                if (avcodec_send_packet(m_codec, m_packet) < 0)
                {
                    av_packet_unref(m_packet);
                    m_failed = true;
                    break;
                }
                ok = drain(index);
            }
            av_packet_unref(m_packet);
        }
        if (!ok)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_stop)
            {
                m_failed = true;
            }
        }
        m_done = true;
        m_wake.notify_all();
    }

    static inline std::atomic<uint64_t> s_serial{0u};
    // RG_HD_MOVIES_DELAY_MS=N: a pause after each picture, for testing the
    // fallback of a decoder that cannot keep up.
    static inline const int s_delayMs = [] {
        const char *value = std::getenv("RG_HD_MOVIES_DELAY_MS");
        return value ? std::atoi(value) : 0;
    }();

    std::string m_path;
    AVFormatContext *m_format = nullptr;
    AVCodecContext *m_codec = nullptr;
    SwsContext *m_sws = nullptr;
    AVPacket *m_packet = nullptr;
    AVFrame *m_picture = nullptr;
    int m_stream = -1;
    uint32_t m_guestHeight = 336u;
    uint32_t m_scale = 4u;
    uint32_t m_outWidth = 0u, m_outHeight = 0u;

    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Slot> m_queue;
    bool m_stop = false;
    std::atomic<bool> m_failed{false};
    std::atomic<bool> m_done{false};
    std::atomic<uint32_t> m_decoded{0u};
    uint32_t m_dropped = 0u;
    int64_t m_convertNanos = 0, m_convertMax = 0;
};

// The movie in play. Only the game's thread touches it (MovieOpen, the draw
// and the shutdown run there).
struct Active
{
    std::unique_ptr<Player> player;
    std::string name;
    Clock::time_point opened;
    Clock::time_point lastGood; // when the picture last matched the game's frame
    bool shown = false;         // an HD picture has been published
    bool gaveUp = false;
    uint32_t lastIndex = ~0u;
    uint32_t drawn = 0u;
    uint32_t held = 0u; // draws with the previous picture kept
    uint32_t firstIndex = ~0u;
    uint64_t draws0 = 0u, uploads0 = 0u; // the backend's counters when it opened
};
Active g_active;

void clearOverride()
{
    dq8::gfx::GsVideoOverride::instance().clear();
}

void endMovie(const char *why)
{
    if (!g_active.player)
    {
        return;
    }
    if (g_debug.load() || g_active.gaveUp)
    {
        g_active.player->stop();
        auto &video = dq8::gfx::GsVideoOverride::instance();
        std::fprintf(stderr,
                     "[hdmovie] %s: %s after %u draws (%u held, %u pictures decoded, %u dropped, resampling %.1f ms "
                     "on average and %.1f at most, %llu draws on HD pictures, %llu uploads)\n",
                     g_active.name.c_str(), why, g_active.drawn, g_active.held, g_active.player->decoded(),
                     g_active.player->dropped(), g_active.player->convertAverageMs(),
                     g_active.player->convertMaxMs(), static_cast<unsigned long long>(video.draws.load() - g_active.draws0),
                     static_cast<unsigned long long>(video.uploads.load() - g_active.uploads0));
    }
    clearOverride();
    g_counting.store(false, std::memory_order_release);
    g_active.player.reset();
    g_active = Active{};
}

void giveUp(const char *why)
{
    std::fprintf(stderr, "[hdmovie] %s: %s; the game's own picture from here\n", g_active.name.c_str(), why);
    clearOverride();
    g_counting.store(g_shotActive, std::memory_order_release);
    g_active.gaveUp = true;
    // The thread is stopped, the memory freed; the movie plays on as it was.
    g_active.player.reset();
}

std::string readString(const uint8_t *rdram, uint32_t address)
{
    std::string text;
    const uint32_t physical = address & 0x1FFFFFFFu;
    if (physical == 0u || physical >= PS2_RAM_SIZE - 0x100u)
    {
        return text;
    }
    for (uint32_t i = 0u; i < 0xC0u && rdram[physical + i] != 0u; ++i)
    {
        const char c = static_cast<char>(rdram[physical + i]);
        if (static_cast<unsigned char>(c) < 0x20u || static_cast<unsigned char>(c) > 0x7Eu)
        {
            return {};
        }
        text.push_back(c);
    }
    return text;
}

// "cdrom0:\MOVIE\00\TITLE01.MVI;1" (either slash) -> "00/TITLE01".
std::string movieName(const std::string &source)
{
    std::string upper = source;
    for (char &c : upper)
    {
        c = c == '\\' ? '/' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    const size_t semicolon = upper.find(';');
    if (semicolon != std::string::npos)
    {
        upper.resize(semicolon);
    }
    if (const size_t movie = upper.find("MOVIE/"); movie != std::string::npos)
    {
        upper.erase(0, movie + 6u);
    }
    if (const size_t colon = upper.find(':'); colon != std::string::npos)
    {
        upper.erase(0, colon + 1u);
    }
    while (!upper.empty() && upper.front() == '/')
    {
        upper.erase(0, 1u);
    }
    if (const size_t dot = upper.rfind('.'); dot != std::string::npos && dot > upper.rfind('/') + 1u)
    {
        upper.resize(dot);
    }
    // Only plain NN/NAME names: nothing that climbs out of the folder.
    if (upper.empty() || upper.find("..") != std::string::npos)
    {
        return {};
    }
    return upper;
}

void movieOpen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    // A movie opened without the last one shut down.
    endMovie("replaced");
    g_counting.store(false, std::memory_order_release);
    g_served.store(0u, std::memory_order_release);
    g_entered.store(false, std::memory_order_release);
    g_shotNext = 0u;
    g_counting.store(false, std::memory_order_release);
    clearOverride();
    std::string source = readString(rdram, getRegU32(ctx, 5) + kSourceName);
    bool substituted = false;
    // RG_HD_MOVIES_PLAY=NN/NAME, for tests: the opening movie plays this one
    // instead (the game reads it from the disc as it would the opening), to
    // reach a movie a short run cannot, with or without HD.
    if (const char *play = std::getenv("RG_HD_MOVIES_PLAY"); play && *play && movieName(source) == "01/01_01")
    {
        std::string path = play;
        for (char &c : path)
        {
            c = c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        path += ".mvi";
        const uint32_t physical = (getRegU32(ctx, 5) + kSourceName) & 0x1FFFFFFFu;
        if (path.size() < source.size() + 8u && physical + path.size() + 1u < PS2_RAM_SIZE)
        {
            std::memcpy(rdram + physical, path.c_str(), path.size() + 1u);
            std::fprintf(stderr, "[hdmovie] playing %s in place of the opening\n", path.c_str());
            source = path;
            substituted = true;
        }
    }
    // Screenshots at pictures (debugging) are for the substituted movie, if any.
    const char *playEnv = std::getenv("RG_HD_MOVIES_PLAY");
    g_shotActive = !g_shotAt.empty() && (substituted || !playEnv || !*playEnv);
    g_counting.store(g_shotActive, std::memory_order_release);
    if (enabled() && !g_folder.empty())
    {
        const std::string name = movieName(source);
        if (g_debug.load())
        {
            std::fprintf(stderr, "[hdmovie] MoviePlayerOpen \"%s\" -> %s\n", source.c_str(), name.c_str());
        }
        if (!name.empty())
        {
            const fs::path file = fs::path(g_folder) / (name + ".mkv");
            std::error_code error;
            if (fs::is_regular_file(file, error))
            {
                auto player = std::make_unique<Player>(file.string());
                if (!player->start())
                {
                    std::fprintf(stderr, "[hdmovie] %s: cannot open %s; the game's own picture\n", name.c_str(),
                                 file.string().c_str());
                }
                else
                {
                    // The first picture ready before the game draws its first.
                    if (!player->waitFirst(kFirstFrameWait) && g_debug.load())
                    {
                        std::fprintf(stderr, "[hdmovie] %s: first picture not ready after %lld ms\n", name.c_str(),
                                     static_cast<long long>(kFirstFrameWait.count()));
                    }
                    g_active = Active{};
                    g_active.name = name;
                    g_active.draws0 = dq8::gfx::GsVideoOverride::instance().draws.load();
                    g_active.uploads0 = dq8::gfx::GsVideoOverride::instance().uploads.load();
                    g_active.opened = g_active.lastGood = Clock::now();
                    g_active.player = std::move(player);
                    g_counting.store(true, std::memory_order_release);
                    std::fprintf(stderr, "[hdmovie] %s: HD %s\n", name.c_str(), file.string().c_str());
                }
            }
            else if (g_debug.load())
            {
                std::fprintf(stderr, "[hdmovie] %s: no file %s\n", name.c_str(), file.string().c_str());
            }
        }
    }
    g_openOriginal(rdram, ctx, runtime);
}

// RG_HD_MOVIES_SHOT_AT=N,M: a debugging aid for the tests, which asks the
// scripted input (RG_INPUT_FILE) for a screenshot as the game takes its Nth
// picture, with or without HD, to compare the two at the same frame.
void checkShot()
{
    if (!g_shotActive)
    {
        return;
    }
    const uint32_t count = g_served.load(std::memory_order_acquire);
    if (g_shotNext < g_shotAt.size() && count > g_shotAt[g_shotNext])
    {
        const char *file = std::getenv("RG_INPUT_FILE");
        if (file && *file)
        {
            if (std::FILE *out = std::fopen(file, "ab"))
            {
                std::fputs("shot\n", out);
                std::fclose(out);
            }
        }
        std::fprintf(stderr, "[hdmovie] screenshot requested with %u pictures taken (picture %u)\n", count,
                     count - 1u);
        ++g_shotNext;
    }
}

void movieDrawFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (!g_shotAt.empty())
    {
        checkShot();
    }
    if (g_active.player)
    {
        const uint32_t count = g_served.load(std::memory_order_acquire);
        const auto now = Clock::now();
        ++g_active.drawn;
        if (!enabled())
        {
            giveUp("switched off");
        }
        else if (g_active.player->failed())
        {
            giveUp("the HD file failed to decode");
        }
        else
        {
            if (g_active.firstIndex == ~0u)
            {
                g_active.firstIndex = count;
            }
            // The game's counter is the number of pictures it has taken, so
            // the picture on screen is the one before it.
            const uint32_t want = count > 0u ? count - 1u : 0u;
            if (g_debug.load() && (g_active.drawn <= 12u || g_active.drawn % 150u == 0u))
            {
                std::fprintf(stderr, "[hdmovie] %s: draw %u served %u want %u shown %d queue-decoded %u\n",
                             g_active.name.c_str(), g_active.drawn, count, want, int(g_active.lastIndex),
                             g_active.player->decoded());
            }
            if (count == 0u)
            {
                // Nothing served yet: the game's own (blank) picture.
                g_active.lastGood = now;
            }
            else if (want != g_active.lastIndex)
            {
                if (auto frame = g_active.player->take(want))
                {
                    dq8::gfx::GsVideoOverride::instance().publish(std::move(frame));
                    g_active.lastIndex = want;
                    g_active.shown = true;
                    g_active.lastGood = now;
                    if (g_debug.load() && (want < 3u || want % 300u == 0u))
                    {
                        std::fprintf(stderr, "[hdmovie] %s: draw %u counter %u -> picture %u\n",
                                     g_active.name.c_str(), g_active.drawn, count, want);
                    }
                }
                else
                {
                    // Not ready (or the file ended early): the last picture
                    // stays; too long and the movie goes back to the game's.
                    ++g_active.held;
                    const auto limit = g_active.shown ? kLateLimit : kFirstFrameLimit;
                    if (g_active.player->finished() && want >= g_active.player->decoded() + 3u)
                    {
                        // The file ran out well before the movie: no point in
                        // waiting (a frame or two past its end is the movie's
                        // own last frames, and the picture simply stays).
                        giveUp("the HD file ended before the movie");
                    }
                    else if (now - g_active.lastGood > limit)
                    {
                        giveUp("decoding fell behind");
                    }
                }
            }
            else
            {
                g_active.lastGood = now;
            }
        }
    }
    g_drawOriginal(rdram, ctx, runtime);
}

// Each call of the game's picture function leaves the mpeg object's word at
// +8 zero when it served a picture, and the game then uploads that picture to
// the movie texture before it calls again. So the next call's entry is the
// moment the picture of the call before is on the texture, and the word it
// finds then is that call's outcome. (Not return_hook's callThen: it cannot
// follow a call that waits for the next vblank, as this one does.)
void mpegGetPicture(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (g_counting.load(std::memory_order_acquire))
    {
        if (!g_entered.exchange(true, std::memory_order_acq_rel))
        {
            // The first call after the open: the word is whatever was there.
        }
        else
        {
            uint32_t none = 1u;
            const uint32_t physical = (getRegU32(ctx, 4) + kMpegNoPicture) & 0x1FFFFFFFu;
            if (physical + sizeof(none) <= PS2_RAM_SIZE)
            {
                std::memcpy(&none, rdram + physical, sizeof(none));
            }
            if (none == 0u)
            {
                g_served.fetch_add(1u, std::memory_order_release);
            }
        }
    }
    g_getPictureOriginal(rdram, ctx, runtime);
}

void moviePlayerShutdown(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    endMovie("ended");
    g_counting.store(false, std::memory_order_release);
    g_shutdownOriginal(rdram, ctx, runtime);
}

#endif // RG_HAS_SDLGPU

fs::path defaultFolder()
{
    return fs::path("textures") / "SCUS-97490" / "movies";
}

// RG_HD_MOVIES: =dir names the folder, =0 / off switches the feature off for
// this run, =1 the default folder. Read once, by whoever asks first (the menu
// builds its option before install()).
void resolve()
{
    static std::once_flag once;
    std::call_once(once, [] {
        const char *env = std::getenv("RG_HD_MOVIES");
        const std::string value = env ? env : "";
        g_debug = std::getenv("RG_HD_MOVIES_DEBUG") != nullptr;
        g_forcedOff = value == "0" || value == "off";
        if (const char *shots = std::getenv("RG_HD_MOVIES_SHOT_AT"))
        {
            const std::string list = shots;
            for (size_t at = 0u; at < list.size();)
            {
                const size_t comma = list.find(',', at);
                g_shotAt.push_back(
                    static_cast<uint32_t>(std::strtoul(list.substr(at, comma - at).c_str(), nullptr, 10)));
                at = comma == std::string::npos ? list.size() : comma + 1u;
            }
        }
        if (!value.empty() && value != "0" && value != "off" && value != "1" && value != "on")
        {
            g_folder = value;
        }
        else
        {
            g_folder = defaultFolder().string();
        }
        av_log_set_level(g_debug.load() ? AV_LOG_WARNING : AV_LOG_ERROR);
    });
}

int scanFolder()
{
    int count = 0;
    std::error_code error;
    if (g_folder.empty() || !fs::is_directory(g_folder, error))
    {
        return 0;
    }
    for (fs::recursive_directory_iterator it(g_folder, fs::directory_options::skip_permission_denied, error), end;
         !error && it != end; it.increment(error))
    {
        if (it->is_regular_file(error) && it->path().extension() == ".mkv" &&
            it->path().filename().string().find(".part") == std::string::npos)
        {
            ++count;
        }
    }
    return count;
}

} // namespace

void install(PS2Runtime &runtime)
{
    resolve();
#if defined(RG_HAS_SDLGPU)
    const bool hooked =
        rg::timing::hookFunction(runtime, kMoviePlayerOpen, &movieOpen, g_openOriginal) &&
        rg::timing::hookFunction(runtime, kMovieDrawFrame, &movieDrawFrame, g_drawOriginal) &&
        rg::timing::hookFunction(runtime, kMoviePlayerShutdown, &moviePlayerShutdown, g_shutdownOriginal) &&
        rg::timing::hookFunction(runtime, kMpegGetPicture, &mpegGetPicture, g_getPictureOriginal);
    if (!hooked)
    {
        std::fprintf(stderr, "[rg] hd movies: some hooks are missing, see above\n");
        g_enabled = false;
        return;
    }
    std::fprintf(stderr, "[hdmovie] folder %s: %d file(s)%s\n", g_folder.c_str(), fileCount(),
                 g_enabled.load() ? "" : " (switched off)");
#else
    (void)runtime;
#endif
}

void setEnabled(bool on)
{
    g_enabled.store(on, std::memory_order_relaxed);
}

bool enabled()
{
    resolve();
    return g_enabled.load(std::memory_order_relaxed) && !g_forcedOff.load();
}

std::string folder()
{
    resolve();
    return g_folder;
}

int fileCount()
{
    resolve();
    int count = g_fileCount.load();
    if (count < 0)
    {
        count = scanFolder();
        g_fileCount.store(count);
    }
    return count;
}

} // namespace rg::hdmovie
