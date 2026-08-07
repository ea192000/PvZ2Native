/* Frame pacing -- see include/pvz2native/gfx/frame_limiter.h.
 *
 * Split out of main.c for the same reason as video_mode.c: the loop only wants
 * "wait until the next frame is due", and the reasoning behind the wait (why
 * vsync stays on under a finite cap, why the deadline is absolute rather than a
 * per-frame sleep) is worth keeping in one place.
 */

#include <pvz2native/gfx/frame_limiter.h>

#include <pvz2native/config.h>

#include <SDL.h>

#include <stdio.h>
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <immintrin.h>
#endif

/* Performance-counter ticks one frame is allowed to take; 0 means uncapped. */
static Uint64 g_frame_ticks = 0;
static Uint64 g_perf_freq = 1;

/* Absolute deadline for the next present, in performance-counter ticks. Kept
 * absolute and advanced by exactly one frame each time so that a wait which
 * returns early (SDL_Delay only resolves to a millisecond) is compensated by
 * the next one, and the long-run average lands on the requested rate. 0 until
 * the first wait anchors it. */
static Uint64 g_next_frame = 0;

void pvz2_frame_limit_init(void) {
    const int fps = pvz2_config()->fps_limit;

    g_perf_freq = SDL_GetPerformanceFrequency();
    if (g_perf_freq == 0) g_perf_freq = 1;
    g_next_frame = 0;

    if (fps <= 0) {
        /* "Unlimited" has to mean unlimited, so vsync goes too -- leaving it on
         * would cap the loop at the refresh rate and make 0 a synonym for
         * whatever the monitor does. */
        SDL_GL_SetSwapInterval(0);
        g_frame_ticks = 0;
        printf("video: frame limit off (vsync off, presenting as fast as the machine manages)\n");
        return;
    }

    /* Vsync stays ON under a finite cap: it is what keeps the presented frame
     * whole (no tearing), and the wait below only has to do anything when the
     * cap is BELOW the refresh rate. So the effective rate is min(cap, refresh),
     * and the default 120 changes nothing for the usual 60 Hz display -- it only
     * bites on a 120/144/240 Hz one, where the old code let the engine redraw at
     * the panel's full rate for no visible gain. Adaptive vsync first (no stutter
     * when a frame misses vblank), plain vsync if the driver lacks it. */
    if (SDL_GL_SetSwapInterval(-1) != 0) {
        SDL_GL_SetSwapInterval(1);
    }
    g_frame_ticks = g_perf_freq / (Uint64)fps;
    printf("video: frame limit %d FPS (vsync on, so the display's refresh rate can cap it lower)\n",
           fps);
}

void pvz2_frame_limit_wait(void) {
    if (g_frame_ticks == 0) return;

    Uint64 now = SDL_GetPerformanceCounter();
    if (g_next_frame == 0) { /* first frame: nothing to wait for, just anchor */
        g_next_frame = now + g_frame_ticks;
        return;
    }

    if (now < g_next_frame) {
        /* Hybrid wait for steadier frame pacing:
         *  - coarse SDL_Delay(1) while we're still clearly early
         *  - short spin in the last sub-millisecond slice for precision */
        const Uint64 fine_window = g_perf_freq / 2000; /* ~0.5 ms */
        while (now + fine_window < g_next_frame) {
            SDL_Delay(1);
            now = SDL_GetPerformanceCounter();
        }
        while (now < g_next_frame) {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
            _mm_pause();
#endif
            now = SDL_GetPerformanceCounter();
        }
    }

    g_next_frame += g_frame_ticks;

    /* Already past the new deadline: the machine is running slower than the cap,
     * or the loop just came back from a stall (a load screen, a window drag).
     * Re-anchor instead of carrying the missed deadlines forward, which would
     * make the loop sprint through a burst of unpaced frames to "catch up". */
    const Uint64 after = SDL_GetPerformanceCounter();
    if (g_next_frame < after) g_next_frame = after + g_frame_ticks;
}
