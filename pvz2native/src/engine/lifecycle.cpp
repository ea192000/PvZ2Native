/* The RegisterNatives-only lifecycle natives, in the order AndroidGameApp.java
 * drives them. None of these appear in the dynamic symbol table -- JNI_OnLoad
 * registers them at runtime -- which is why they are reached by offset. */

#include <pvz2native/engine/engine.h>

#include <cstring>
#include <vector>

#include <pvz2native/diagnostics/diagnostics.h>
#include <pvz2native/dex/dex.h>
#include <pvz2native/game/symbols.h>
#include <pvz2native/log.h>
#include <pvz2native/runtime/dynarmic_config.h>
#include <pvz2native/surface.h>

namespace pvz2native {
namespace engine {

namespace rt_ = pvz2native::runtime;

/* Native_GameAppInitialize.
 *
 * It is declared in AndroidGameApp.java as an INSTANCE native, so JNI hands it
 * (JNIEnv*, jobject thiz, arg0..arg7) -- NINE values, not eight. Passing only
 * the eight declared arguments shifted every object down one register: the
 * engine took our "surfaceView" as `thiz` and our "httpProxy" as the surface
 * view, so Graphics_GetScreenSizeInPixels was invoked on the wrong fake object
 * and left LawnApp's mOrigScreenWidth/Height reading uninitialised array
 * memory. thiz goes first.
 *
 * The class names are ground-truthed against the native method declaration in
 * classesdex/com/popcap/SexyAppFramework/AndroidGameApp.java and tagged with
 * tag_object_class(), so GetObjectClass/GetMethodID/CallXxxMethod on these
 * objects resolve to real (class, method) pairs instead of opaque handles. */
void run_game_app_initialize(pvz2_elf_image_t *img, GuestRuntime *rt) {
    /* Which objects this build declares is per-version data, so it lives in the
     * version table with every other per-version fact -- see GameSymbols. This
     * file used to keep the 1.6/4.5.2 list of eight as a local fallback, which
     * both duplicated the 9.6.1 list it differs from by one entry and broke
     * symbols.cpp's rule that adding a release touches nothing outside it. */
    const char *const *arg_classes = sym().game_app_init_args;
    const std::uint32_t arg_count = sym().game_app_init_arg_count;

    /* Refusing loudly, not silently. GameAppInitialize is the call that gives the
     * engine its surface view and http proxy; running it with the wrong number of
     * objects shifts every one down a register and corrupts the screen size
     * instead of failing, which is precisely the bug this list exists to prevent.
     * A version table entry without a list is a mistake in that entry. */
    if (arg_classes == nullptr || arg_count == 0) {
        pvz2_log("[engine] %s has no game_app_init_args -- refusing to call "
                 "Native_GameAppInitialize with a guessed signature",
                 sym().version);
        return;
    }
    if (arg_count + 1 > rt_::memmap::kFakeObjMax) {
        pvz2_log("[engine] %s declares %u objects, more than the %u the memory map "
                 "reserves -- raise kFakeObjMax in runtime/guest_memmap.h",
                 sym().version, arg_count, rt_::memmap::kFakeObjMax);
        return;
    }

    /* thiz first, then the declared objects. Each gets its own scratch block and
     * is tagged with the class it stands for, so GetObjectClass/GetMethodID on it
     * resolve to a real (class, method) pair instead of an opaque handle. */
    std::vector<std::uint32_t> args;
    args.reserve(arg_count + 1);
    for (std::uint32_t i = 0; i <= arg_count; ++i) {
        const std::uint32_t addr = rt_::memmap::kFakeObjBase + i * rt_::memmap::kFakeObjStride;
        const char *cls = (i == 0) ? kGameAppClass : arg_classes[i - 1];
        /* The label is only for logging; the last path component reads well. */
        const char *slash = std::strrchr(cls, '/');
        rt_::make_fake_jstring(img, addr, slash ? slash + 1 : cls);
        rt->tag_object_class(addr, cls);
        args.push_back(addr);
    }
    rt_::run_at_offset(img, rt, "Native_GameAppInitialize", sym().native.game_app_initialize, args);

    /* It returns jboolean, and on a device AndroidGameApp.java refuses to start
     * when that is false -- this port used to discard it and carry on.
     *
     * What false means, read off the body: it registers the natives, then caches
     * ~50 jmethodIDs in one short-circuiting chain (Config_*, Device_*, UI_*,
     * Resources_*, Info_Sys*, Play_*), and gives up at the FIRST link that comes
     * back 0, leaving every later slot in that cache zeroed. Nothing announces
     * it. The engine then calls through those zeroed IDs much later and from
     * somewhere else entirely -- AndroidAsyncIOFileDriver::Init asks for
     * Resources_GetUserDataFolder while SexyAppBase is still constructing -- so
     * the symptom surfaces with no trace of this call in it.
     *
     * Not fatal here on purpose: which links matter differs per build, and
     * refusing to boot on a build that merely lacks one would be worse than the
     * silence it replaces. */
    if (rt_::last_call_stats().result == 0) {
        pvz2_log("[engine] Native_GameAppInitialize returned FALSE -- it abandoned its "
                 "jmethodID cache part-way, so some Java calls the engine makes later will go "
                 "through a zeroed id. Booting anyway; expect a failure far from here");
    }
}

/* Native_applicationWillFinishLaunching builds argv[] and runs the whole
 * SexyAppFramework constructor, which `operator new`s the LawnApp into the
 * global at sym().global.app. Its r1 (thiz) is never dereferenced, so 0 is
 * safe; a null startUrl in r2 is handled gracefully (argv becomes
 * {"Game", NULL}).
 *
 * Then, in real Java call order: onActivityStart ->
 * Native_applicationDidFinishLaunching (a nullsub, kept for fidelity),
 * onActivityResume -> Native_applicationWillBecomeForeground. The latter does
 * NOT do the work itself: it allocates an AndroidAppEvent and appends it to a
 * global std::list, which is drained and dispatched at the top of every
 * onDrawFrame. */
void run_application_launch(pvz2_elf_image_t *img, GuestRuntime *rt) {
    diagnostics::dump_frame_gate(img, "before WillFinishLaunching");
    rt_::run_at_offset(img, rt, "Native_applicationWillFinishLaunching",
                       sym().native.application_will_finish_launching, {0, 0});
    diagnostics::dump_frame_gate(img, "after WillFinishLaunching");

    /* Native_createNativeApplicationLifecycleObserver -- new in 9.6.1, 0
     * elsewhere. It runs HERE, after the framework constructor, and not right
     * after GameAppInitialize where it was first tried: the AndroidAppDriver
     * does not exist until WillFinishLaunching has built it, so anything that
     * attaches or publishes that object has nothing to work with earlier. */
    if (sym().native.create_lifecycle_observer != 0) {
        rt_::run_at_offset(img, rt, "Native_createNativeApplicationLifecycleObserver",
                           sym().native.create_lifecycle_observer, {0});
        diagnostics::dump_frame_gate(img, "after LifecycleObserver");
    }

    rt_::run_at_offset(img, rt, "Native_applicationDidFinishLaunching",
                       sym().native.application_did_finish_launching, {0});
    rt_::run_at_offset(img, rt, "Native_applicationWillBecomeForeground",
                       sym().native.application_will_become_foreground, {0});
    diagnostics::dump_frame_gate(img, "after WillBecomeForeground");
}

/* onWindowFocusChanged(true) -> Native_applicationDidBecomeActive bookends the
 * surface setup.
 *
 * onSurfaceChanged reads its size from its THIRD and FOURTH arguments, because
 * as a JNI static method the first two are (JNIEnv*, jclass). Passing
 * {width, height} put them in r0/r1, so the engine read r2/r3 and got stack
 * garbage -- hence the "mOrigScreenWidth = 17533344" and "Resize: 334x1536" in
 * the logs. The size also feeds the projection matrix, so a bogus width poisons
 * rendering as well. */
void run_surface_changed(pvz2_elf_image_t *img, GuestRuntime *rt, std::uint32_t width,
                         std::uint32_t height) {
    /* The dummy words the JNI preamble eats before (width, height) differ per
     * build -- see GameSymbols::surface_changed_pad. per_frame=true keeps it
     * from logging a banner, which matters because a window drag fires this
     * every frame. */
    std::vector<std::uint32_t> size_args(sym().surface_changed_pad, 0u);
    size_args.push_back(width);
    size_args.push_back(height);
    rt_::run_at_offset(img, rt, "Native_onSurfaceChanged", sym().native.on_surface_changed,
                       size_args, true);
}

/* The three `(Z)V` state natives 9.6.1 added -- see GameSymbols::native for what
 * goes silently wrong without them. Static natives, so run_at_offset's implicit
 * JNIEnv* lands in r0, the jclass placeholder in r1, and the jboolean in r2:
 * that is the shape the bodies confirm, each doing `MOV R1, R2` before tailing
 * into its one-line setter.
 *
 * They must precede applicationDidBecomeActive, because it is the handler for
 * THAT message which reads all three and decides whether to start drawing. */
void notify_app_state(pvz2_elf_image_t *img, GuestRuntime *rt) {
    struct { std::uint32_t offset; const char *name; } kNotifies[] = {
        {sym().native.notify_surface_change, "Native_NotifySurfaceChange"},
        {sym().native.notify_app_running,    "Native_NotifyAppRunning"},
        {sym().native.notify_focus_change,   "Native_NotifyFocusChange"},
    };
    for (const auto &n : kNotifies) {
        if (n.offset == 0) continue; /* 1.6 and 4.5.2 have no such native */
        rt_::run_at_offset(img, rt, n.name, n.offset, {0 /* jclass */, 1 /* JNI_TRUE */});
    }
}

void run_surface_lifecycle(pvz2_elf_image_t *img, GuestRuntime *rt) {
    rt_::run_at_offset(img, rt, "Native_onSurfaceCreated", sym().native.on_surface_created, {});
    run_surface_changed(img, rt, pvz2_surface_width(), pvz2_surface_height());
    /* Surface is live, the app is running and the window has focus -- assert all
     * three BEFORE the message that consults them is enqueued. */
    diagnostics::dump_frame_gate(img, "before Notify*");
    notify_app_state(img, rt);
    diagnostics::dump_frame_gate(img, "after Notify*");
    rt_::run_at_offset(img, rt, "Native_applicationDidBecomeActive",
                       sym().native.application_did_become_active, {0});
    diagnostics::dump_frame_gate(img, "after DidBecomeActive");
    /* Stay offline through onSurfaceCreated: wifi there reopens the loading-screen
     * hang (PvZ2Web / content waits). Promote after the menu path is live so
     * LoD/map online gates can open against OBB-served CDN bodies. */
    dex::net_mark_boot_finished();
}

}  // namespace engine
}  // namespace pvz2native
