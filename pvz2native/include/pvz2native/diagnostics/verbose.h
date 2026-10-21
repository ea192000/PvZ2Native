#ifndef PVZ2NATIVE_DIAGNOSTICS_VERBOSE_H
#define PVZ2NATIVE_DIAGNOSTICS_VERBOSE_H

#include <cstdint>

#include <pvz2native/elf32/elf32_loader.h>
#include <pvz2native/runtime/guest_runtime.h>

/* [log] verbose=1 -- the "why does the engine ignore me" trace.
 *
 * Built to chase the dead-UI-after-an-epic-quest bug (TODO-epic-freeze.md, solved),
 * but generic: every line carries the frame number (f=) so the Java calls, the engine's
 * states and the periodic summary can be read as one timeline. Everything here is a
 * no-op when verbose is 0, so a normal run pays nothing.
 *
 * Line tags:
 *   [engine] the engine's own state keys (Board State, WorldMap Shown, ...), logged on change
 *   [engine-log] the engine's decision breadcrumbs, with the code addresses that made them
 *   [screen] create / destroy of the reward-screen singleton (game/patches.cpp)
 *   [java]   a Java method the engine called (throttled per method, with raw args)
 *   [census] live engine objects grouped by vtable (see census() in verbose.cpp)
 *   [state]  every few seconds: fps, import/JNI/draw rates, input age, heap
 *
 * The boot banners (.init_array, lifecycle calls) are the older half of verbose
 * and are still printed from dynarmic_config.cpp. */

namespace pvz2native {
namespace diagnostics {

/* [log] verbose. */
bool verbose();

/* Per-event touch/key logging and the frame heartbeat: [log] input OR verbose. */
bool input_trace();

/* "[tag f=<frame>] message", only when verbose. */
void vlog(const char *tag, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

/* Frame number as of the last completed frame; what vlog stamps lines with. */
int current_frame();

/* Counts a GL draw call for the [state] line; a no-op unless verbose is on. */
void note_gl_draw();

/* The engine just consumed `events` input records (UI_ProcessEvents). */
void note_input_delivered(unsigned events, bool had_down, bool had_up);

/* Asks for an object census on the next frame (safe from any thread). */
void request_census(const char *why);

/* Called once per frame, right after onDrawFrame returns. */
void after_frame(pvz2_elf_image_t *img, GuestRuntime *rt, int frame);

}  // namespace diagnostics
}  // namespace pvz2native

#endif
