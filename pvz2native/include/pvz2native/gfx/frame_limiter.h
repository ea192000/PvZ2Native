#ifndef PVZ2NATIVE_GFX_FRAME_LIMITER_H
#define PVZ2NATIVE_GFX_FRAME_LIMITER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Owns how fast the host presents frames: the swap interval AND the pacing, so
 * there is one place that answers "why is it running at N FPS".
 *
 * Only the presentation loop is paced. The engine's simulation advances from the
 * wall clock and its audio runs on its own guest thread (see libopensles's
 * buffer-queue pump), so waiting here slows nothing down but the redraw. */

/* Reads [video] fps_limit and applies it. Requires a current GL context (it
 * sets the swap interval). Safe to call again after changing fps_limit at
 * runtime:
 *   fps_limit > 0 -> vsync on (adaptive if the driver has it) plus a wait that
 *                    holds the loop to that many frames per second; the display's
 *                    refresh still caps it further.
 *   fps_limit = 0 -> vsync off and no wait at all. */
void pvz2_frame_limit_init(void);

/* Turns vsync off / back on (as fps_limit says) without touching the pacing. The loading screen is repainted from
 * inside the boot many times a second; with vsync each of those presents would wait for the display and slow the
 * boot down. */
void pvz2_frame_limit_vsync_suspend(void);
void pvz2_frame_limit_vsync_resume(void);

/* Waits out whatever is left of the current frame's budget. Call once per
 * iteration of the frame loop, after the swap. A no-op when uncapped. */
void pvz2_frame_limit_wait(void);

#ifdef __cplusplus
}
#endif

#endif
