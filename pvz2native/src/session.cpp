/* The session: everything the guest needs to stay alive between frames, and
 * nothing else.
 *
 * It exists because the HOST drives the frame loop. SDL has to pump its message
 * queue and swap buffers around every Native_onDrawFrame, which is impossible
 * while one call runs the entire boot and hundreds of frames to completion --
 * the window would never pump, Windows would mark it "Not responding", and
 * nothing would ever be presented.
 *
 * This file orchestrates and delegates. The four layers below it each answer a
 * different question, and keeping them apart is what makes supporting another
 * release of the game an additive change:
 *
 *   runtime/     how to run ARM code            (no game addresses at all)
 *   game/        where things are in this build (the per-version table)
 *   engine/      what to call, and in what order
 *   diagnostics/ how to ask the running guest a question
 */

#include <atomic>
#include <pvz2native/log.h>
#include <cstdio>
#include <cstdlib>

#include <pvz2native/audio/audio_output.h>
#include <pvz2native/config.h>
#include <pvz2native/dependencies/dependency.h>
#include <pvz2native/dex/dex.h>
#include <pvz2native/diagnostics/diagnostics.h>
#include <pvz2native/engine/engine.h>
#include <pvz2native/game/patches.h>
#include <pvz2native/game/symbols.h>
#include <pvz2native/pvz2_session.h>
#include <pvz2native/runtime/dynarmic_config.h>
#include <pvz2native/runtime/guest_runtime.h>
#include <pvz2native/surface.h>

extern "C" {
#include <pvz2native/elf32/elf32_loader.h>
}

namespace {

namespace rt_ = pvz2native::runtime;
namespace engine = pvz2native::engine;
namespace diag = pvz2native::diagnostics;
namespace dex = pvz2native::dex;

/* Which frame to sample once the app has settled. The frames right after boot
 * are not representative -- the engine is still in its first-render setup -- so
 * a useful trace has to be taken well into steady state. */
constexpr int kSteadyStateSampleFrame = 300;

/* How many frames to trace instruction-by-instruction under [log] pc_sample,
 * and how few ticks to allow per JIT slice while doing it.
 *
 * This is the tool for one specific and otherwise opaque situation: a frame
 * that costs a fixed number of ticks and makes NO imports and NO JNI calls. It
 * is running real guest code and touching nothing we can see, so no shim log
 * can say where it goes -- but a small slice makes run_jit_sliced() print the
 * PC every few instructions, and the addresses can be read straight off in a
 * disassembler. Without it the only alternative is guessing at branches from
 * static disassembly, which is exactly how this kind of hunt goes wrong. */
constexpr int kPcSampleFrames = 2;
constexpr std::uint32_t kPcSampleSliceTicks = 24;

}  // namespace

struct pvz2_session {
    pvz2_elf_image_t img{};
    pvz2native::GuestRuntime rt;
    int frames_run = 0;
    int trace_frames_left = 0;  /* frames still to trace call-by-call */
    int sample_frames_left = 0; /* frames still to trace instruction-by-instruction */
};

extern "C" void pvz2_session_set_host_pump(pvz2_host_pump_fn fn) { rt_::set_host_pump(fn); }

/* "The surface changed size, tell the engine" -- a flag, not a size. It cannot be
 * acted on from the SDL event handler: re-running onSurfaceChanged is a guest
 * call and must happen on the thread that owns the JIT and GL context, between
 * frames. The new size needs no queue of its own because the host has already
 * published it through pvz2_surface_set; this only says "it moved". That also
 * coalesces for free -- a drag firing every frame costs one onSurfaceChanged per
 * frame at the latest size, never a backlog at stale ones. */
namespace {
std::atomic<bool> g_resize_pending{false};
}  // namespace

extern "C" void pvz2_session_request_resize(void) {
    g_resize_pending.store(true, std::memory_order_release);
}

extern "C" void pvz2_session_status(char *out, size_t n) { rt_::copy_status(out, n); }

extern "C" pvz2_session_t *pvz2_session_start(const char *so_path) {
    auto *s = new pvz2_session();
    if (pvz2_elf_load(so_path, rt_::kAddressSpaceSize, &s->img) != 0) {
        pvz2_log("[session] failed to load '%s'", so_path);
        delete s;
        return nullptr;
    }

    /* Before anything else runs: every offset the engine layer is about to use
     * belongs to a specific build, and calling another release's addresses
     * would land in the middle of an unrelated function with no diagnosable
     * symptom. Refusing to start is the only honest outcome. */
    if (!pvz2native::game_symbols_detect(&s->img)) {
        pvz2_elf_free(&s->img);
        delete s;
        return nullptr;
    }

    /* Rewrites of the guest's own code, now that the build is known and while
     * nothing has executed yet -- dynarmic caches translated blocks, so a later
     * write would not be seen by anything already run. See game/patches.h. */
    pvz2native::game_apply_patches(&s->img);

    /* Must precede the first Jit: UserConfig captures the page table pointer at
     * construction, and the Jits are long-lived now. */
    rt_::build_page_table(&s->img);

    /* Imported STT_OBJECT symbols got real guest memory from the loader; fill
     * them in before any guest code runs (the ctype tables in particular). */
    rt_::build_import_handler_cache(&s->img);
    pvz2native::initialize_data_imports(&s->img, &s->rt);

    dex::install(&s->img);

    s->rt.img = &s->img;
    const rt_::HeapLayout heap = rt_::heap_layout_for(&s->img);
    if (heap.size == 0) {
        pvz2_elf_free(&s->img);
        delete s;
        return nullptr;
    }
    s->rt.heap.init(heap.base, heap.size);
    /* Lets make_guest_callback() and call_guest_between_frames() reach this
     * session -- the OpenSL layer mints trampolines through both. */
    rt_::set_session_image(&s->img, &s->rt);

    /* Needs the session image (make_guest_callback). Before any guest code so
     * the SVC at sub_3487AC is visible to the first JIT translation. */
    pvz2native::game_install_lod_apply_probe(&s->img);

    /* Load-time bring-up, then the registered-native lifecycle, in the exact
     * order the real Android runtime uses -- see engine/. */
    engine::boot_native_library(&s->img, &s->rt);
    engine::run_eaframework_startup(&s->img, &s->rt);
    engine::run_game_app_initialize(&s->img, &s->rt);
    engine::run_application_launch(&s->img, &s->rt);
    engine::run_surface_lifecycle(&s->img, &s->rt);

    return s;
}

extern "C" int pvz2_session_frame(pvz2_session_t *s) {
    if (s == nullptr) return 0;

    /* A pending window resize, applied here on the frame thread: re-run
     * onSurfaceChanged so the engine re-lays-out and renders AT that resolution
     * rather than upscaling a fixed one. Done before the frame so this frame
     * already draws at the new size.
     *
     * The size itself is already current -- the host set it through
     * pvz2_surface_set the moment the resize arrived, and every layer reads that
     * one store. Only telling the ENGINE has to wait for the frame thread. */
    if (g_resize_pending.exchange(false, std::memory_order_acquire)) {
        engine::run_surface_changed(&s->img, &s->rt, pvz2_surface_width(),
                                    pvz2_surface_height());
    }

    /* Instruction-level trace of a settled frame -- see kPcSampleFrames. The
     * slice override has to be armed BEFORE the call and cleared after, because
     * it is what makes jit.Run() return often enough to sample at all. */
    if (pvz2_config()->pc_sample && s->frames_run == kSteadyStateSampleFrame) {
        s->sample_frames_left = kPcSampleFrames;
        pvz2_log("[trace] ==== tracing frames %d..%d instruction-by-instruction ====",
                    s->frames_run, s->frames_run + kPcSampleFrames - 1);
    }
    if (s->sample_frames_left > 0) {
        rt_::set_slice_override(kPcSampleSliceTicks);
    }

    if (pvz2_config()->trace && s->frames_run == kSteadyStateSampleFrame) {
        s->trace_frames_left = 2;
        pvz2native::trace::set(true);
        pvz2_log("[trace] ==== steady-state sample: tracing frames %d..%d ====", s->frames_run,
                    s->frames_run + s->trace_frames_left - 1);
    }
    if (s->trace_frames_left > 0 && --s->trace_frames_left == 0) {
        pvz2native::trace::set(false);
        pvz2_log("[trace] ==== end steady-state sample ====");
        std::fflush(stdout);
    }

    /* Grant any purchase the emulated store owes the engine, here between
     * top-level guest calls rather than reentrantly from inside RequestPayment
     * -- see dex::iap_deliver_pending. */
    if (pvz2_config()->emulate_iap) {
        dex::iap_deliver_pending(&s->img, &s->rt);
    }

    engine::draw_frame(&s->img, &s->rt, s->frames_run);
    ++s->frames_run;

    /* The lifecycle only ENQUEUES applicationDidBecomeActive; the handler that
     * consults the gate bytes runs inside the first PumpMessageQueue. So the
     * state that matters is the one AFTER a frame, not after the lifecycle. */
    if (s->frames_run == 2) {
        diag::dump_frame_gate(&s->img, "after first frames");
    }

    if (s->sample_frames_left > 0 && --s->sample_frames_left == 0) {
        rt_::set_slice_override(0);
        pvz2_log("[trace] ==== end instruction trace ====");
        std::fflush(stdout);
    }

    /* Fallback path only. Buffer-queue completions are normally delivered by a
     * guest thread of their own (see bq_pump_thread in libopensles.cpp), because
     * doing it here made the audio engine's progress depend on onDrawFrame
     * returning -- and onDrawFrame calls AK::SoundEngine::UnloadBank, which
     * blocks until the audio engine has finished the bank's last voice. This
     * call does nothing once that thread is running. */
    pvz2native::audio_pump_callbacks();

    /* [log] input=1 heartbeat, and the point of it: if this stops right after a
     * click, the engine hung INSIDE onDrawFrame handling that click, and the
     * button frozen mid-press is simply the last frame ever drawn. If it keeps
     * ticking, the engine is alive and the event reached it but did not act.
     * Those two have completely different fixes, and nothing else in the log
     * tells them apart. */
    if (pvz2_config()->input && s->frames_run % 600 == 0) {
        /* What that frame COST, not just that it happened. A heartbeat alone
         * cannot tell "the engine is running and drawing nothing" from "the
         * engine bails out at the top of onDrawFrame" -- both tick forever over
         * a black window, and they are unrelated bugs. A frame worth a handful
         * of imports and no ticks is the second. */
        const rt_::CallStats &f = rt_::last_call_stats();
        pvz2_log("[input] heartbeat -- frame %d  (last frame: %u imports, %u jni, "
                    "%llu ticks, %.2f ms)",
                    s->frames_run, f.import_calls, f.jni_calls, (unsigned long long)f.ticks, f.ms);

        /* Heap use and audio health over time. The point is the TREND across
         * heartbeats: the "freezes after ~5 minutes, audio loops a noise" report
         * is either the guest heap climbing to exhaustion (in-use approaching
         * total, then the guest aborts and everything stops -- the audio loop is
         * just a dead process's last DMA buffer repeating), or the audio queue
         * genuinely starving (underruns climbing while the game keeps running).
         * If this line simply STOPS printing at the freeze, the whole guest hung;
         * if it keeps printing with underruns climbing, only the audio died. */
        std::uint64_t heap_in_use = 0, heap_peak = 0, heap_total = 0;
        s->rt.heap.usage(heap_in_use, heap_peak, heap_total);
        std::uint64_t underruns = 0, enqueue_fails = 0;
        pvz2native::audio::diag(underruns, enqueue_fails);
        pvz2_log("[health] heap %llu/%llu MB (peak %llu) | audio queued %u underruns %llu "
                    "enqueue-fails %llu",
                    (unsigned long long)(heap_in_use >> 20), (unsigned long long)(heap_total >> 20),
                    (unsigned long long)(heap_peak >> 20), pvz2native::audio::queued_count(),
                    (unsigned long long)underruns, (unsigned long long)enqueue_fails);

        diag::dump_touch_scaler(&s->img);
        /* And WHICH Java calls those were. The heartbeat says the engine is
         * alive; this says what it is asking for, which is the only way to tell
         * "waiting on an answer we do not give" from "stuck in its own code".
         * Throttles itself to one report every few seconds. */
        dex::report_call_census();
    }
    return 1;
}

extern "C" void pvz2_session_end(pvz2_session_t *s) {
    if (s == nullptr) return;
    /* Stop the audio device first: its callback reads host buffers owned by the
     * audio layer, and this is also what releases the guest thread parked in
     * wait_for_completion() so it can unwind before its Jit goes away. */
    pvz2native::audio::shutdown();
    /* Any guest thread that outlived the call that spawned it holds raw
     * pointers to both img and rt, so it must be joined before either dies.
     * If a worker stays forever in sem_wait, join times out -- freeing the
     * session then races the still-running Jit (SIGSEGV). Hard-exit instead. */
    if (!rt_::join_leftover_guest_threads(&s->rt)) {
        std::fflush(stdout);
        std::_Exit(0);
    }
    /* The shared main-thread Jit points at rt->monitor and its Env points at
     * img, so it has to go before either -- see GuestThreadCtx. */
    rt_::release_main_ctx();
    pvz2_elf_free(&s->img);
    rt_::set_session_image(nullptr, nullptr);
    delete s;
}
