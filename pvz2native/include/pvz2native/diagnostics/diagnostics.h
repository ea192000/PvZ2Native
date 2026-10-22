#ifndef PVZ2NATIVE_DIAGNOSTICS_DIAGNOSTICS_H
#define PVZ2NATIVE_DIAGNOSTICS_DIAGNOSTICS_H

#include <cstdint>
#include <string>

#include <pvz2native/dependencies/dependency.h>
#include <pvz2native/elf32/elf32_loader.h>
#include <pvz2native/runtime/guest_runtime.h>

/* Tools for asking the running guest a question, kept apart from the code that
 * boots it.
 *
 * This directory has been pruned twice and should be pruned again: a probe
 * written to chase one bug is dead weight the day that bug is understood, and
 * it is dead weight that still has to be ported to every new game version,
 * because every probe is a pile of struct offsets. What survives here is only
 * what has proved useful more than once.
 *
 * The generic instruments -- the memory watchpoint and the PC sampler -- live
 * in runtime/dynarmic_config.h instead, because they can only be implemented
 * from inside the memory callbacks and the Run() loop.
 */

namespace pvz2native {
namespace diagnostics {

/* The screen->app coordinate transform every touch goes through, plus the two
 * flags that gate touch dispatch.
 *
 * This is the one thing the input log cannot show. We can prove we delivered
 * DOWN and UP at sensible window pixels and that the engine kept running
 * frames -- and still not know WHERE in app space those pixels landed. The
 * transform is
 *     x_app = out_origin.x + (x_win - in_origin.x) * num.x / den.x
 * and, critically, the engine SKIPS it entirely when either denominator is
 * zero, passing raw window pixels straight through. A zero there would also
 * explain the zero-width viewport seen on the composite draw: same size state,
 * same zero. */
void dump_touch_scaler(pvz2_elf_image_t *img);

/* Dumps a guest global by raw .so offset -- for empirically checking which
 * lifecycle call writes which global, instead of guessing from static
 * disassembly. */
void dump_dword(pvz2_elf_image_t *img, const char *label, std::uint32_t offset);

/* Guessed call chain of the guest code running now, as " 0x<offset>" .so offsets (at most `max_frames`).
 *
 * The engine is stripped and has no RTTI, so a log line is often the only name we get for a function -- but
 * the addresses of the functions that were running are enough to disassemble them offline. Walks the guest
 * stack and keeps every word that points into the code right after a BL/BLX, i.e. a plausible return
 * address. Heuristic: it can include stale entries. */
std::string guess_callers(GuestCall &c, int max_frames = 10);

/* The four bytes that decide whether onDrawFrame does anything (9.6.1 only,
 * quiet on any version whose app_driver is unmapped).
 *
 * onDrawFrame skips the whole frame when driver[316] is non-zero, and the only
 * code that clears it is HandleApplicationDidBecomeActive -- behind a guard on
 * driver[326] && (driver[325] || driver[324]), the three bytes the
 * Native_Notify* natives write. Printing all four says which half of that is
 * failing, which static reading cannot: either the writes are not landing on
 * the object the handler reads, or they are and the handler is never
 * dispatched. Also follows the dispatcher's own path to the object it passes,
 * so the two pointers can be compared directly. */
void dump_frame_gate(pvz2_elf_image_t *img, const char *when);

/* Builds a guest std::string and returns its address, for calling engine
 * functions that take one by reference.
 *
 * Kept even with no caller: it is the only way to pass a string into the
 * engine, and rebuilding it means rediscovering that the right entry point is
 * libstdc++'s `string(const char*, const allocator&)` and that it needs a dummy
 * allocator argument. The blocks are never freed -- these are built once and
 * reused for the life of the process. Returns 0 if the heap is exhausted or the
 * detected version does not map the constructor. */
std::uint32_t make_guest_string(pvz2_elf_image_t *img, GuestRuntime *rt, const char *text);

}  // namespace diagnostics
}  // namespace pvz2native

#endif
