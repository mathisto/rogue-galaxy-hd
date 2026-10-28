// A video picture that stands in for a guest texture (Rogue Galaxy: added).
//
// The game's movie player puts each decoded picture into GS memory (16x16
// macroblocks at a fixed TBP) and draws it as one textured sprite. A host
// decoder can offer a better picture of the same frame here: while one is
// published, a draw whose TEX0 points at its tbp0 samples it instead of the
// texture built from local memory.
//
// The picture is `scale` times the guest texture's size (width x height
// physical texels covering the guest's `guestWidth` x `guestHeight` texels
// the movie fills), so UVs, clamping and the shader's physical-texels-per-
// texel factor are the guest's own, as with a texture-pack replacement. Pass
// the frame already resampled to that size.
//
// Any thread may publish; the backend's worker reads. Frames are immutable
// once published (shared_ptr), the newest one is the one a draw sees.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace dq8::gfx {

struct GsVideoFrame {
    uint32_t tbp0 = 0u;
    uint32_t guestWidth = 0u;
    uint32_t guestHeight = 0u;
    uint32_t scale = 1u;
    uint32_t width = 0u;  // guestWidth * scale
    uint32_t height = 0u; // guestHeight * scale
    // RGBA8 in the GS's alpha range (0x80 opaque), as a guest texture's.
    std::vector<uint8_t> rgba;
    uint64_t serial = 0u;
};

class GsVideoOverride {
public:
    static GsVideoOverride &instance() {
        static GsVideoOverride self;
        return self;
    }
    void publish(std::shared_ptr<const GsVideoFrame> frame) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_frame = std::move(frame);
        m_active.store(m_frame != nullptr, std::memory_order_release);
    }
    // A cheap test for the draw path: whether a picture is published.
    bool active() const { return m_active.load(std::memory_order_acquire); }
    void clear() { publish(nullptr); }
    std::shared_ptr<const GsVideoFrame> current() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_frame;
    }
    // Counters for the log (the draws that took the picture / the uploads).
    std::atomic<uint64_t> draws{0u};
    std::atomic<uint64_t> uploads{0u};

private:
    std::atomic<bool> m_active{false};
    mutable std::mutex m_mutex;
    std::shared_ptr<const GsVideoFrame> m_frame;
};

} // namespace dq8::gfx
