/* The surface size -- see include/pvz2native/surface.h for why it is one store
 * and not the four it replaced. */

#include <pvz2native/surface.h>

#include <atomic>
#include <cstdint>

namespace {

/* 960x540 is the historical fixed size the port ran at before [video] existed,
 * and the only sensible answer if a reader somehow gets here before the host has
 * set anything. gfx/video_mode.c's own launch-size fallback reads these same
 * atomics (via pvz2_surface_width/height) rather than keeping a second literal,
 * since it always runs before the first pvz2_surface_set.
 *
 * Atomic because the setter runs on the SDL thread (a window resize) while the
 * readers run on the frame thread, mid-guest-call. */
std::atomic<std::uint32_t> g_width{960};
std::atomic<std::uint32_t> g_height{540};

std::atomic<std::uint32_t> g_touch_width{960};
std::atomic<std::uint32_t> g_touch_height{540};
std::atomic<bool> g_touch_frozen{false};

}  // namespace

extern "C" void pvz2_surface_set(std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0) return; /* a zero-size surface is never meant */
    g_width.store(width, std::memory_order_relaxed);
    g_height.store(height, std::memory_order_relaxed);

    /* First call wins, permanently -- see pvz2_touch_space_width. */
    if (!g_touch_frozen.exchange(true, std::memory_order_relaxed)) {
        g_touch_width.store(width, std::memory_order_relaxed);
        g_touch_height.store(height, std::memory_order_relaxed);
    }
}

extern "C" std::uint32_t pvz2_surface_width(void) {
    return g_width.load(std::memory_order_relaxed);
}
extern "C" std::uint32_t pvz2_surface_height(void) {
    return g_height.load(std::memory_order_relaxed);
}
extern "C" std::uint32_t pvz2_touch_space_width(void) {
    return g_touch_width.load(std::memory_order_relaxed);
}
extern "C" std::uint32_t pvz2_touch_space_height(void) {
    return g_touch_height.load(std::memory_order_relaxed);
}
