#ifndef PVZ2NATIVE_GAME_SYMBOLS_H
#define PVZ2NATIVE_GAME_SYMBOLS_H

#include <cstdint>

#include <pvz2native/elf32/elf32_loader.h>

namespace pvz2native {

/* Everything about the game binary that is not the same in every build.
 *
 * The harness itself -- the JIT, the libc/GLES/JNI shims, the heap, the audio
 * bridge -- is version-agnostic: it implements Android, and Android does not
 * change between two PvZ2 releases. What DOES change is every address inside
 * libPVZ2.so, and those used to be hex literals inlined at their use sites, so
 * supporting a second release meant editing the middle of the boot logic. They
 * all live here now, and adding a version is adding one entry to kVersions.
 *
 * The lifecycle natives are the reason raw offsets are unavoidable at all:
 * JNI_OnLoad registers them with RegisterNatives, so unlike the Java_* exports
 * they never appear in the dynamic symbol table and cannot be looked up by name.
 *
 * 0 means "not mapped for this version". Callers must treat it as such:
 * `natives` are required and a missing one is a hard error, while a missing
 * diagnostic offset just turns that diagnostic off.
 *
 * kVersions fills this in with DESIGNATED initialisers (.native = {...}), and a
 * new entry must too. Every field here is an interchangeable uint32 offset, so a
 * positional aggregate that omits one block -- or a member added in the middle of
 * one -- shifts everything after it with nothing for the compiler to complain
 * about; the symptom is a branch into the middle of an unrelated function. Naming
 * each field makes an omitted one simply absent, and absent means 0, which is
 * already the contract above. */
struct GameSymbols {
    const char *version;

    /* Registered natives -- offsets into the .so, called by run_at_offset. */
    struct {
        std::uint32_t game_app_initialize;
        std::uint32_t application_will_finish_launching;
        std::uint32_t application_did_finish_launching;
        std::uint32_t application_will_become_foreground;
        std::uint32_t application_did_become_active;
        std::uint32_t on_surface_created;
        std::uint32_t on_surface_changed;
        std::uint32_t on_draw_frame;

        /* PumpMessageQueue: drains the lifecycle event queue and dispatches it.
         *
         * 0 when the version has no such native. 1.6 does not: it drains the
         * queue at the top of onDrawFrame itself. 4.5.2 split the two apart, and
         * its onDrawFrame ONLY draws -- so failing to call this leaves every
         * applicationDidBecomeActive/WillBecomeForeground sitting in the queue
         * forever, the driver stuck in its paused state, and every frame taking
         * a fixed-cost branch that updates nothing. The window stays black with
         * no error anywhere; the giveaway is a frame that costs the same ticks
         * every time and makes zero imports. */
        std::uint32_t pump_message_queue;

        /* Native_createNativeApplicationLifecycleObserver: new in 9.6.1, 0 for
         * 1.6 and 4.5.2. It is registered in a JNINativeMethod array of its own,
         * so the Java side calls it explicitly during startup, and this build
         * moved work there -- its Native_applicationDidFinishLaunching is now a
         * bare `BX LR`, where 4.5.2's did something. Called right after
         * GameAppInitialize, which is where Java constructs the observer.
         *
         * If a 9.6.1 boot misbehaves in the lifecycle, this speculative call is
         * the first thing to try removing: the ORDER is inferred from the name
         * and from what the natives around it became, not read off a trace. */
        std::uint32_t create_lifecycle_observer;

        /* Native_NotifySurfaceChange / NotifyAppRunning / NotifyFocusChange,
         * each `(Z)V`. New in 9.6.1; 0 for 1.6 and 4.5.2, which have no such
         * natives. On Android the activity calls them from its surface and
         * focus callbacks -- so this port, which IS that activity, must too.
         *
         * They are not optional, and skipping them fails SILENTLY. Each writes
         * one byte into the app driver (surface -> +326, running -> +325,
         * focus -> +324, all zero from the driver's constructor).
         * HandleApplicationDidBecomeActive -- the handler PumpMessageQueue
         * dispatches for applicationDidBecomeActive -- opens with
         *
         *     if (surface == 0) return;
         *     if (running == 0 && focus == 0) return;
         *
         * and only past that guard does it clear the driver's "skip the frame"
         * byte at +316. onDrawFrame tests exactly that byte and, when set, takes
         * a branch that updates and draws nothing. So without these three the
         * engine boots perfectly, drains its queue, and then runs a fixed-cost
         * empty frame forever: measured as `0 imports, 0 jni, 1052 ticks`,
         * identical every frame, with a black window and no error anywhere.
         * The giveaway in the log is a MISSING line -- "HandleApplicationWill-
         * BecomeForeground" appears, "HandleApplicationDidBecomeActive" does
         * not, because that debug print sits after the guard. */
        std::uint32_t notify_surface_change;
        std::uint32_t notify_app_running;
        std::uint32_t notify_focus_change;
    } native;

    /* How many dummy words precede (width, height) in the onSurfaceChanged
     * call -- i.e. how many of r1..r3 the JNI preamble eats before the real
     * arguments start.
     *
     * A per-version value because the two builds genuinely differ, and getting
     * it wrong is silent: the engine reads whichever registers it expects and
     * derives its whole projection matrix from them, so a shift by one register
     * produces a plausible-looking window with a poisoned transform rather than
     * a crash. 4.5.2's is the textbook static-native shape -- its body reads r2
     * and r3 -- and was read off the decompilation, not assumed. */
    std::uint32_t surface_changed_pad;

    /* Engine globals, as offsets into the .so. `app` is the LawnApp the
     * SexyAppFramework constructor stores (dword_D55650 in 1.6); `app_driver`
     * is the separate AndroidAppDriver pointer the surface/frame natives
     * actually dereference (dword_DC8FD4) -- they are NOT the same object, and
     * assuming they were cost a long hunt once already. */
    struct {
        std::uint32_t app;
        std::uint32_t app_driver;
    } global;

    /* Guest functions the harness calls directly rather than through a native. */
    struct {
        /* libstdc++ `string(const char*, const allocator&)`, for handing the
         * engine a real std::string argument. */
        std::uint32_t string_ctor;
    } fn;

    /* Engine natives the PORT calls back INTO -- the reverse of `native`. These
     * are the C++ implementations behind Java `native` methods, registered by
     * RegisterNatives, that a Java class fires as a completion callback. This
     * port IS that Java class (see dex/hooks/), so it has to invoke them itself.
     *
     * AndroidHttpTransaction's five-entry JNINativeMethod array, in order:
     *   HttpReceivedResponse / HttpReceivedData / HttpTransactionComplete /
     *   HttpTransactionError / HttpTransactionCleanup.
     * Response/Complete/Error take (JNIEnv*, jobject, jlong nativeTransaction)
     * and only ENQUEUE onto the ring buffer PumpMessageQueue drains -- cheap and
     * safe to call nested from Start(). Data also takes (jbyteArray, jint len).
     * Without Error (or a full success sequence), every HTTP request hangs
     * forever: on a device Java times out after 30s and fires Error; here Start()
     * has to do that itself. 0 when not mapped. */
    struct {
        std::uint32_t http_received_response;
        std::uint32_t http_received_data;
        std::uint32_t http_transaction_complete;
        std::uint32_t http_transaction_error;
    } jni_native;

    /* Instructions the port rewrites in the loaded image -- see game/patches.h,
     * which explains why one exists at all. Addresses only; the rewriting and
     * its verification live there. */
    struct {
        /* Every call to the engine's "is the device online?" helper made from
         * INSIDE the purchase broker, as offsets into the .so. Each is a 4-byte
         * ARM `BL` that is rewritten to `MOV R0, #1`, which is what makes an
         * emulated store usable on a port with no network. 0 ends the list, and
         * an all-zero list simply patches nothing. */
        std::uint32_t purchase_online_gate[8];

        /* Same helper (sub_247084), but call sites on the LoD / CDN-apply path.
         * Global GetNetworkStatus=wifi during onSurfaceCreated hangs the loading
         * screen; these sites alone get "online" so schedule apply can run while
         * boot stays offline. 0 ends the list. */
        std::uint32_t lod_cdn_online_gate[8];

        /* LoD Active-UID writer is only reached via sub_3487AC -> sub_40F44C ->
         * sub_69F8DC. Dead ends leave alod=0 / wm=[] even with a valid
         * lod_config day:
         *   lod_apply_fn -- sub_3487AC itself (entry used by the runtime probe).
         *   lod_apply_stub_call -- in sub_3469E0, after LevelOfTheDay toggle +
         *     schedule-singleton init, a BL to sub_40F440 (three BX LR stubs).
         *     Rewritten to BL sub_3487AC so the real apply runs.
         *   lod_apply_stub_skip_bne -- in sub_3469E0, BNE that skips the
         *     stub_call entirely when sub_40D31C != 1. NOP'd so init always
         *     reaches the (rewritten) apply BL.
         *   lod_apply_tick_gate -- in sub_3484AC, BLEQ sub_3487AC (left alone;
         *     forcing it unconditional called apply with r0=predicate).
         *   lod_apply_ready_bne -- in sub_3487AC, BNE out if sub_40D31C != 1
         *     (property/toggle gate that stays false offline on this port).
         *     Rewritten to NOP so schedule matching still runs.
         *   lod_apply_match_bne -- in sub_40F44C, BNE out if a second
         *     property gate != 1. Same NOP treatment.
         *   lod_match_fn -- sub_40F44C itself (schedule match + BL sub_69F8DC).
         *     Runtime probe entry; apply calls this at 0x348814.
         *   lod_match_progress_bne -- in sub_40F44C, BNE out when
         *     (+0x178 < +0x184). That invariant breaks after a UID write that
         *     does not also refresh +0x178, leaving Active LoD sticky forever.
         *   lod_match_uid_bls -- in sub_40F44C, BLS that skips the UID writer
         *     when schedule_uid <= +0x178 (assumes UIDs rise with time; our
         *     mid-insert days and future holiday UIDs violate that).
         *   lod_uid_writer_fn -- sub_69F8DC: STR UID/state at obj+0x184/+0x190.
         * 0 = not mapped. */
        std::uint32_t lod_apply_fn;
        /* sub_81E948: "engine clock synced with the server?" (clock+0x48 != 0); the port has no server, so it is
         * rewritten to always say yes -- see force_clock_synced in patches.cpp. 0 = not mapped. */
        std::uint32_t clock_synced_fn;
        std::uint32_t lod_apply_stub_call;
        std::uint32_t lod_apply_stub_skip_bne;
        std::uint32_t lod_apply_tick_gate;
        std::uint32_t lod_apply_ready_bne;
        std::uint32_t lod_apply_match_bne;
        std::uint32_t lod_match_fn;
        std::uint32_t lod_match_progress_bne;
        std::uint32_t lod_match_uid_bls;
        std::uint32_t lod_uid_writer_fn;

        /* Create / destroy of the 0x104-byte reward-screen singleton (vtable at
         * .so+0x10C0A58) that the award, quest, prize and coin flows each spawn; see
         * game_install_reward_screen_fix. Both are `PUSH {...,lr}` at the function entry. */
        std::uint32_t scr_create;
        std::uint32_t scr_destroy;
        /* The global holding the live instance (.bss; 0 when none). Create overwrites
         * it without looking, so a second create while one is alive orphans the first,
         * and the first's own destroy call then finds 0 and does nothing. */
        std::uint32_t scr_global;
        /* Value of its vptr (an offset into the .so), used to check that a remembered
         * instance is still a live object of that class before it is destroyed. */
        std::uint32_t scr_vtable;

        /* Quest manager entry points, only read by the [log] verbose=1 quest trace
         * (game_install_quest_trace). All are `PUSH {...,lr}` at the entry; 0 = not mapped.
         *   quest_pick_fn      -- QuestMgr::Pick(this): runs the pending pick, appends the
         *                         human-readable "Pick Time / Limiters / + Add N quests from
         *                         slot X" text to a std::string at this+0x94.
         *   quest_slot_pick_fn -- (this, slotInfo, count): picks up to `count` quests of one
         *                         slot; returns how many are still missing.
         *   quest_candidate_fn -- (out, this, const std::string &id): looks an eligible
         *                         candidate up in the active list; one call per candidate.
         *   quest_add_fn       -- (out, this, questDataHandle): instantiates the quest and
         *                         appends it to the active list. */
        std::uint32_t quest_pick_fn;
        std::uint32_t quest_slot_pick_fn;
        std::uint32_t quest_candidate_fn;
        std::uint32_t quest_add_fn;

        /* The engine's "what time is it" (sub_81E960: `PUSH {r4,lr}`, r0 = the clock object), used by the Level
         * of the Day schedule, the quest limiter and the active-event countdown. Its own clock (a float
         * counter plus a server-set offset) reads ~2030 on this port, which stuck the schedule on far-future
         * events, so game_install_engine_clock replaces it. 0 = not mapped. */
        std::uint32_t engine_clock_fn;
        /* The shuffle the pick runs over a slot's candidates (std::random_shuffle over 8-byte
         * handles, called from the slot pick and from one wrapper). 0 = not mapped. */
        std::uint32_t quest_shuffle_fn;
        /* libc++'s default shuffle generator std::__rs_default, a function-local static
         * std::mt19937 inside the .so: guard byte at +4, 624 state words at +8, index at
         * +0x9c8. Built lazily on first use from the fixed default seed 5489, so every process
         * starts the same sequence. Offset of the object itself; 0 = not mapped. */
        std::uint32_t rs_default_state;

        /* The receipt verdict, which takes two instructions because the engine
         * only ever reaches a "purchase finished" state through its validation
         * server. Both are `MOV Rd, #imm` and 0 means not mapped.
         *
         * A completed payment is put on the broker's pending list, and the broker
         * hands the item over only once that record reads (finished, validated).
         * Only the server sets validated: the HTTP reply's "$.validation" must
         * say "passed". This port has no network, so left alone the purchase
         * hangs on "Purchasing Stuff..." forever waiting for that reply.
         *
         *   local_receipt  -- the `MOV R1, #1` in the thunk whose only caller is
         *                     OnPaymentComplete, meaning "POST the receipt and
         *                     wait". Rewritten to `MOV R1, #0`: finish now.
         *   receipt_verdict-- the `MOV R0, #0` in that finish-now branch, which
         *                     writes "not validated". Rewritten to `MOV R0, #1`.
         * Together they are exactly the outcome of a server answering "passed",
         * without the round trip. Patching only the first ends at the engine's
         * "Unable to contact store" dialog, which is that branch being honest. */
        std::uint32_t purchase_local_receipt;
        std::uint32_t purchase_receipt_verdict;
    } patch;

    /* What diagnostics/guest_probe reads to explain a frame that draws nothing.
     * All optional: zeroed out, dump_frame_gate reports what it can and skips
     * the rest. These were 9.6.1 literals inside guest_probe.cpp, which put .so
     * addresses outside this file -- the one thing symbols.cpp promises not to
     * allow -- and which the probe then applied to whatever build was loaded:
     * 4.5.2 has a non-zero app_driver global, so it ran and printed 9.6.1's
     * field offsets against a 4.5.2 object. Plausible numbers, all meaningless. */
    struct {
        /* Byte flags in the AndroidAppDriver that gate whether a frame draws.
         * Read off onDrawFrame's body and HandleApplicationDidBecomeActive. */
        std::uint32_t gate_skip_frame;
        std::uint32_t gate_focus;
        std::uint32_t gate_running;
        std::uint32_t gate_surface;

        /* Where the driver sits inside the app object: SexyAppBase's constructor
         * stores it there, so *(app + this) finds the driver even while the
         * driver's own global is still unpublished -- which is exactly the state
         * the probe exists to explain. */
        std::uint32_t app_driver_field;

        /* Sexy::AndroidAppDriver's vtable, the slot holding the thunk that
         * publishes `this` into the driver global, and that thunk itself.
         *
         * The vtable base is derived by hand from the {offset_to_top, typeinfo}
         * header, and a plausible-but-wrong base makes every slot index wrong
         * while still looking right -- so the probe verifies it against the live
         * object's vptr instead of trusting the arithmetic. That check is only
         * possible because all three are given together. */
        std::uint32_t driver_vtable;
        std::uint32_t publish_slot;
        std::uint32_t publish_thunk;

        /* The Sexy::AndroidAsyncIOFileDriver global, and THE bisector for a boot
         * that never publishes the app driver.
         *
         * SexyAppBase's constructor creates this object and stores it here, then
         * -- a few instructions later, in the same constructor -- creates the
         * AndroidAppDriver at app + app_driver_field. So the two together say
         * exactly how far that constructor got, which is the one thing a black
         * first frame does not otherwise reveal:
         *
         *   this 0, driver 0 -- the constructor died BEFORE either, i.e. inside
         *                       the base-class chain above SexyAppBase;
         *   this != 0, drv 0 -- it died between the two, a span of ~20 lines;
         *   both != 0        -- the constructor finished, so the failure is
         *                       downstream of it and nothing in the app-
         *                       construction tree is to blame.
         *
         * Worth reading even when everything works: the framework's own
         * "is there a resources.xml?" probe, which runs immediately after the
         * constructor returns, dereferences this global with NO null check. A
         * constructor that died leaves it 0 and that probe then walks a vtable
         * of zeroes -- so a failure anywhere above reappears as the executed
         * void, several frames from its cause. */
        std::uint32_t file_driver;
    } probe;

    /* Field offsets for the touch diagnostic -- see diagnostics/input_probe.
     * All optional: zeroed out, the probe simply reports nothing. */
    struct {
        std::uint32_t driver;        /* app_driver value -> driver          */
        std::uint32_t scaler;        /* driver -> coordinate scaler         */
        std::uint32_t touch_begin;   /* active-touch vector begin           */
        std::uint32_t touch_end;     /* active-touch vector end             */
        std::uint32_t touch_stride;  /* bytes per entry in that vector      */
        std::uint32_t multitouch;    /* byte flag: multitouch enabled       */
        std::uint32_t touch_active;  /* byte flag: touch dispatch active    */
        std::uint32_t scaler_fields; /* first of 8 int32s in the scaler     */
        std::uint32_t vt_touch_down; /* driver vtable slots, for naming the */
        std::uint32_t vt_touch_up;   /* concrete handlers in the log        */
        std::uint32_t vt_touch_move;
    } input;

    /* First 8 bytes at native.on_draw_frame and native.game_app_initialize.
     * Detection compares these rather than trusting a file size: if they match,
     * the two most important offsets in the table are proven right on this exact
     * binary, and a mismatch is caught before we branch into the middle of some
     * unrelated function. */
    std::uint64_t fingerprint_draw_frame;
    std::uint64_t fingerprint_game_app_init;

    /* The object arguments Native_GameAppInitialize declares, in order, NOT
     * counting the leading (JNIEnv*, jobject thiz): thiz is always
     * AndroidGameApp and the caller supplies it.
     *
     * Per-version because the list genuinely changed. 1.6 and 4.5.2 declare
     * eight and pass an AndroidFacebookDriver third; 9.6.1 declares seven and
     * has no Facebook driver at all. Reusing the old list on 9.6.1 would shift
     * cloud, GooglePlay* and notification each down one register -- and this is
     * the exact mistake that once made the engine call
     * Graphics_GetScreenSizeInPixels on the wrong fake object, corrupting the
     * screen size instead of failing. Read it off the JNI signature string in
     * the JNINativeMethod table, never guessed.
     *
     * Every version must fill this in. It used to be optional, with nullptr
     * meaning "the historical eight" and engine/lifecycle.cpp holding that list
     * as a fallback -- which put per-version data outside this file, against the
     * rule at the top of symbols.cpp, and duplicated seven of the eight names. */
    const char *const *game_app_init_args;
    std::uint32_t game_app_init_arg_count;
};

/* thiz, the leading jobject every build's Native_GameAppInitialize takes and
 * which therefore is not in the per-version lists above. */
extern const char *const kGameAppClass;

/* Identifies the loaded image against the built-in table.
 *
 * Returns false when nothing matches, having logged the candidates it tried and
 * what it found instead -- which is the report someone adding a version needs.
 * Booting anyway would mean calling whatever happens to sit at another build's
 * offsets, so the caller must refuse to start. */
bool game_symbols_detect(const pvz2_elf_image_t *img);

/* The detected table. Before a successful detect it is all zeroes with
 * version "unknown", so a stray read cannot hand out a plausible-looking
 * address. */
const GameSymbols &sym();

}  // namespace pvz2native

#endif
