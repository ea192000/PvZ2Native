/* The per-version address tables, and the detection that picks one.
 *
 * Adding a release means adding one entry to kVersions below and nothing else:
 * no file outside this one holds an address into libPVZ2.so. To fill an entry,
 * decompile JNI_OnLoad and read its JNINativeMethod tables for the `native`
 * offsets, then take the two fingerprints with any hex viewer.
 *
 * Name every field you set (.native = {...}) and leave the rest out rather than
 * writing a 0 placeholder -- see GameSymbols for why a positional entry here is
 * a silent hazard. What an entry does NOT mention is unmapped, which is exactly
 * what 0 means to every reader of the table.
 */

#include <pvz2native/game/symbols.h>
#include <pvz2native/log.h>

#include <cstddef>
#include <cstring>

namespace pvz2native {

const char *const kGameAppClass = "com/popcap/SexyAppFramework/AndroidGameApp";

namespace {

/* Every other field value-initialises to 0/nullptr, which is exactly what
 * "nothing is mapped" means -- and stays true when a field is added. */
const GameSymbols kUnknown{.version = "unknown"};

/* Element count of a class-name array, so no entry below has to spell out the
 * sizeof/sizeof and risk dividing by the wrong array. */
template <std::size_t N>
constexpr std::uint32_t kCount(const char *const (&)[N]) {
    return (std::uint32_t)N;
}

/* Native_GameAppInitialize's declared objects for 1.6.10 and 4.5.2, which share
 * the same eight-object signature -- the third being an AndroidFacebookDriver
 * that 9.6.1 no longer has. */
const char *const kGameAppInitArgsLegacy[] = {
    "com/popcap/SexyAppFramework/AndroidSurfaceView",
    "com/popcap/SexyAppFramework/AndroidHttpProxy",
    "com/popcap/SexyAppFramework/AndroidFacebookDriver",
    "com/popcap/SexyAppFramework/cloud/Cloud",
    "com/popcap/SexyAppFramework/GooglePlay/GooglePlayConnect",
    "com/popcap/SexyAppFramework/GooglePlay/GooglePlayAchievements",
    "com/popcap/SexyAppFramework/GooglePlay/GooglePlayLeaderboard",
    "com/popcap/SexyAppFramework/AndroidNotification",
};

/* Native_GameAppInitialize's declared objects for PvZ2 9.6.1, read off the JNI
 * signature in its JNINativeMethod entry:
 *   (Lcom/popcap/SexyAppFramework/AndroidSurfaceView;
 *    Lcom/popcap/SexyAppFramework/AndroidHttpProxy;
 *    Lcom/popcap/SexyAppFramework/cloud/Cloud;
 *    Lcom/popcap/SexyAppFramework/GooglePlay/GooglePlayConnect;
 *    Lcom/popcap/SexyAppFramework/GooglePlay/GooglePlayAchievements;
 *    Lcom/popcap/SexyAppFramework/GooglePlay/GooglePlayLeaderboard;
 *    Lcom/popcap/SexyAppFramework/AndroidNotification;)Z
 * Seven, not the eight of 1.6/4.5.2 -- AndroidFacebookDriver is gone. */
const char *const kGameAppInitArgs961[] = {
    "com/popcap/SexyAppFramework/AndroidSurfaceView",
    "com/popcap/SexyAppFramework/AndroidHttpProxy",
    "com/popcap/SexyAppFramework/cloud/Cloud",
    "com/popcap/SexyAppFramework/GooglePlay/GooglePlayConnect",
    "com/popcap/SexyAppFramework/GooglePlay/GooglePlayAchievements",
    "com/popcap/SexyAppFramework/GooglePlay/GooglePlayLeaderboard",
    "com/popcap/SexyAppFramework/AndroidNotification",
};

const GameSymbols kVersions[] = {
    /* --- 1.6.10 (2013, armeabi-v7a) ------------------------------------- */
    {
        .version = "1.6.10",
        .native = {
            .game_app_initialize                = 0x9eaf98,
            .application_will_finish_launching  = 0x9ebfb8,
            .application_did_finish_launching   = 0x9ec0d8,
            .application_will_become_foreground = 0x9ec0f4,
            .application_did_become_active      = 0x9ec100,
            .on_surface_created                 = 0x9f1878,
            .on_surface_changed                 = 0x9f1914,
            .on_draw_frame                      = 0x9f1944,
            /* No PumpMessageQueue: onDrawFrame drains the queue itself. */
        },
        .surface_changed_pad = 2,
        .global = {
            .app        = 0xd55650, /* dword_D55650 -- LawnApp          */
            .app_driver = 0xdc8fd4, /* dword_DC8FD4 -- AndroidAppDriver */
        },
        .fn = {
            .string_ctor = 0xb75560, /* std::string(const char*, allocator) */
        },
        /* jni_native.http_transaction_error: 1.6 reaches the menu, left untouched.
         * patch: not mapped -- nothing is rewritten, so its store behaves exactly
         * as it always has.
         * probe: frame-gate offsets not located for 1.6. */
        .input = {
            .driver        = 68,
            .scaler        = 696,
            .touch_begin   = 192,
            .touch_end     = 196,
            .touch_stride  = 48,
            .multitouch    = 220,
            .touch_active  = 221,
            .scaler_fields = 176,
            .vt_touch_down = 432,
            .vt_touch_up   = 436,
            .vt_touch_move = 440,
        },
        .fingerprint_draw_frame    = 0xE59F0014E92D4800ull, /* PUSH {R11,LR}; LDR R0,=...        */
        .fingerprint_game_app_init = 0xE24DD094E92D4FF0ull, /* PUSH {R4-R11,LR}; SUB SP,SP,#0x94 */
        .game_app_init_args      = kGameAppInitArgsLegacy,
        .game_app_init_arg_count = kCount(kGameAppInitArgsLegacy),
    },

    /* --- 4.5.2 (2016, armeabi-v7a, 18MB) ---------------------------------
     *
     * Same ARM32 binary shape as 1.6 and the same eight natives with the same
     * JNI signatures, so nothing in runtime/ or engine/ needed to change to
     * reach it -- which was the whole point of splitting this table out.
     *
     * Only three natives are registered from JNI_OnLoad here (GameAppInitialize,
     * GameAppTeardown, getGooglePlayAPIKey); the rest live in two more
     * JNINativeMethod arrays at 0x10A6054 (lifecycle, 11 entries) and 0x10A610C
     * (surface, 3 entries), reached by following a data xref from each method
     * NAME string. That is the reliable way to find them in a stripped build --
     * do not try to recognise the functions themselves.
     *
     * Globals and the diagnostic field offsets are NOT mapped yet: they drive
     * only diagnostics/, which checks for 0 and stays quiet, so the boot does
     * not depend on them. */
    {
        .version = "4.5.2",
        .native = {
            .game_app_initialize                = 0xcc033c,
            .application_will_finish_launching  = 0xcc131c,
            .application_did_finish_launching   = 0xcc1420,
            .application_will_become_foreground = 0xcc142c,
            .application_did_become_active      = 0xcc1430,
            .on_surface_created                 = 0xcc7cf0,
            .on_surface_changed                 = 0xcc7d8c,
            .on_draw_frame                      = 0xcc7e60,
            .pump_message_queue                 = 0xcc7cd0,
        },
        .surface_changed_pad = 1, /* body reads r2/r3 -- plain static native */
        .global = {
            /* LawnApp is not located yet; the driver is the pointer
             * onSurfaceCreated and onDrawFrame dereference (1.6's dword_DC8FD4). */
            .app_driver = 0x117a734,
        },
        .jni_native = {
            /* 5-entry HTTP array at 0x10a5978 -- previous map pointed Error at
             * Complete (index 2); the real Error is index 3. */
            .http_received_response     = 0xcdeb28,
            .http_received_data         = 0xcdebcc,
            .http_transaction_complete  = 0xcded8c,
            .http_transaction_error     = 0xcdee30,
        },
        .patch = {
            /* purchase_online_gate -- the four `BL sub_247084` inside the
             * PurchaseBroker (sub_247084 is "GetNetworkStatus() is 1 or 2").
             * Every one of them gates a purchase step on connectivity and shows
             * [PURCHASE_ERROR_SERVICE_UNAVAILABLE_HEADER] when it fails; the
             * first is the one a player hits by pressing Buy.
             * The SAME helper is called from a dozen non-store places, which is
             * exactly why the call sites are patched and not the helper. */
            .purchase_online_gate = {
                0x72d320, /* in sub_72D264 -- start a single purchase       */
                0x72dc14, /* in sub_72DB98 -- "is the store offline?"        */
                0x72e0d8, /* in sub_72E0C0 -- restore purchases              */
                0x72e38c, /* in sub_72E384 -- any purchase still in flight?  */
            },
            /* LoD / CDN apply: every BL to sub_247084 on that path. Omits
             * boot/snapshot sites (0x47434c, 0x248ccc) that hang the loading
             * screen when forced online.
             * 0x323058/174/1d4 -- sub_322F84 (CDN package apply)
             * 0x2f373c         -- sub_2F3240 (CDN companion)
             * 0x2f10b0/1e8c    -- nearby CDN helpers (same module)
             * 0x32c240/3f7748  -- same "online then touch singleton" pattern;
             *                     without these, schedule can evaluate once
             *                     offline, stash NextScheduleTime, and never
             *                     pick up halloween/LoD until that timestamp. */
            .lod_cdn_online_gate = {
                0x323058,
                0x323174,
                0x3231d4,
                0x2f373c,
                0x2f10b0,
                0x2f1e8c,
                0x32c240,
                0x3f7748,
            },
            /* LoD apply never reached Active-UID writer: stub BX LR + tick gate
             * + property-gate BNEs that exit before schedule matching.
             * 0x3487ac: real apply (sub_3487AC).
             * 0x3469f8: BNE skips stub_call when toggle gate != 1 -> NOP.
             * 0x346a00: BL sub_40F440 -> BL sub_3487AC (real apply).
             * 0x3485f0: BLEQ sub_3487AC -> BL (unconditional).
             * 0x3487bc: BNE (ready!=1) -> NOP.
             * 0x40f4e0: BNE (match-gate!=1) -> NOP.
             * 0x40f44c: schedule match (sub_40F44C); BL sub_69F8DC at 0x40f664.
             * 0x40f58c: BNE when +0x178 < +0x184 -> NOP.
             * 0x40f640: BLS when schedule_uid <= +0x178 -> NOP.
             * 0x81e978: BEQ into float clock -> NOP (use time()). */
            .lod_apply_fn = 0x3487ac,
            .lod_apply_stub_call = 0x346a00,
            .lod_apply_stub_skip_bne = 0x3469f8,
            .lod_apply_tick_gate = 0x3485f0, /* observed only; not rewritten */
            .lod_apply_ready_bne = 0x3487bc,
            .lod_apply_match_bne = 0x40f4e0,
            .lod_match_fn = 0x40f44c,
            .lod_match_progress_bne = 0x40f58c,
            .lod_match_uid_bls = 0x40f640,
            .lod_time_float_beq = 0x81e978,
            .lod_uid_writer_fn = 0x69f8dc,
            /* The `MOV R1,#1` in sub_737DB8, whose only caller is
             * PurchaseBroker::OnPaymentComplete (sub_72EE30). R1=1 means "POST
             * this receipt to the validation server and wait for $.validation";
             * R1=0 takes sub_737DC0's finish-now branch. */
            .purchase_local_receipt = 0x737db8,
            /* The `MOV R0,#0` at the head of that finish-now branch
             * (loc_737EA4), whose STRB writes the record's "validated" byte
             * before it sets the state to 4=finished. 0 there is what makes the
             * broker (sub_7305D8) show "Unable to contact store" instead of
             * delivering; 1 is what sub_738F3C writes when a real server replies
             * "passed". */
            .purchase_receipt_verdict = 0x737ea4,
        },
        /* probe: the frame-gate field offsets are 9.6.1's; reading a 4.5.2 driver
         * at them printed plausible garbage, which is worse than printing
         * nothing. input: not mapped either. */
        .fingerprint_draw_frame    = 0xE59F1010E59F0010ull, /* LDR R0,[PC,#0x10]; LDR R1,[PC,#0x10] */
        .fingerprint_game_app_init = 0xE24DD084E92D4FF0ull, /* PUSH {R4-R11,LR}; SUB SP,SP,#0x84    */
        .game_app_init_args      = kGameAppInitArgsLegacy,
        .game_app_init_arg_count = kCount(kGameAppInitArgsLegacy),
    },

    /* --- 9.6.1 (PvZ2 Reflourished 1.3.1, armeabi-v7a, 30MB) ---------------
     *
     * The last Reflourished release that ships an armeabi-v7a build at all --
     * 1.4.2 is arm64-only, which this emulator cannot run. Both APKs are
     * versionCode 675, so main.675.com.ea.game.pvz2_rfl.obb pairs with either.
     *
     * First build with real DT_NEEDED dependencies: libc++_shared.so and
     * libNimble.so supply 119 of its 514 imports, so the loader maps them beside
     * libPVZ2.so rather than shimming them (see pvz2_elf_load).
     *
     * The natives were located by decoding the JNINativeMethod arrays whole. A
     * 12-byte table of pointers is self-similar under a 4-byte shift, so a
     * pattern scan reports every array three times with three different
     * apparent field orders and three different sets of function addresses --
     * the correct one is the one whose START ADDRESS is referenced by the code
     * that passes it to RegisterNatives. Three arrays, all {name, sig, fn},
     * separated by NULL words:
     *   0x1d0e960 (3) GameAppInitialize, GameAppTeardown, getGooglePlayAPIKey
     *   0x1d0e988 (1) createNativeApplicationLifecycleObserver
     *   0x1d0e998 (15) the lifecycle family
     *   0x1d0ea80 (6) the surface family + PumpMessageQueue
     *   0x1d10138 (5) the HTTP callbacks
     * Several lifecycle natives are one-instruction thunks -- 0x1277934 and
     * 0x1277938 are a bare `BX LR` -- which is why they sit 4 bytes apart and
     * looks like a misread but is not. */
    {
        .version = "9.6.1",
        .native = {
            .game_app_initialize                = 0x12768c4,
            .application_will_finish_launching  = 0x1277854,
            .application_did_finish_launching   = 0x1277938, /* a bare BX LR */
            .application_will_become_foreground = 0x1277944,
            .application_did_become_active      = 0x1277948,
            .on_surface_created                 = 0x127cc34,
            .on_surface_changed                 = 0x127ccd0,
            .on_draw_frame                      = 0x127cdcc,
            .pump_message_queue                 = 0x127cc24,
            .create_lifecycle_observer          = 0x1277000,
            .notify_surface_change              = 0x1277ad4, /* -> driver+326 */
            .notify_app_running                 = 0x1277ac0, /* -> driver+325 */
            .notify_focus_change                = 0x1277aac, /* -> driver+324 */
        },
        .surface_changed_pad = 1, /* body does MOV R5,R2 / MOV R4,R3 -- plain
                                   * static native, confirmed by disassembly,
                                   * as does Native_onOrientationChanged which
                                   * forwards r2/r3 straight into r0/r1 */
        .global = {
            /* LawnApp: the pointer sub_FC2A74 (the body of
             * Native_applicationWillFinishLaunching) `operator new`s and stores
             * before running the framework. Worth having beyond diagnostics:
             * the AndroidAppDriver lives at *(app + 8) -- SexyAppBase's
             * constructor puts it there (0x10d144c: BL ctor; STR r0,[r4,#8]) --
             * so this global reaches the driver even while the driver's OWN
             * global is still unpublished. */
            .app = 0x1d92a94,
            /* AndroidAppDriver: onDrawFrame and all three Native_Notify* natives
             * reach it through this one GOT slot (LDR r0,=off; LDR r0,[pc,r0]). */
            .app_driver = 0x1d9ce44,
        },
        .jni_native = {
            /* HttpTransactionError, 4th of the 5-entry HTTP array. */
            .http_transaction_error = 0x129eb68,
        },
        /* patch: not mapped -- the purchase-broker rewrites are 4.5.2 addresses
         * and mean nothing here. The store simply behaves as the engine's own
         * offline path dictates until they are found for this build. */
        .probe = {
            /* Byte flags read off onDrawFrame's body (0x12853e0) and
             * HandleApplicationDidBecomeActive (0x1281914). */
            .gate_skip_frame = 316,
            .gate_focus      = 324,
            .gate_running    = 325,
            .gate_surface    = 326,
            /* SexyAppBase's ctor: BL <driver ctor>; STR r0,[r4,#8] at 0x10d144c. */
            .app_driver_field = 8,
            /* From the {offset_to_top, typeinfo} header preceding it, typeinfo
             * RTTI name "N4Sexy16AndroidAppDriverE". */
            .driver_vtable = 0x1d0eb90,
            .publish_slot  = 0x1d0ecdc,
            .publish_thunk = 0x128277c, /* stores `this` into the driver global */
            /* Sexy::AndroidAsyncIOFileDriver, created and stored by SexyAppBase's
             * constructor ~20 lines before it builds the app driver. Named from
             * the RTTI its vtable (0x1d0fddc) points at. */
            .file_driver = 0x1d96db0,
        },
        /* input: not mapped for this build. */
        .fingerprint_draw_frame    = 0xE79F0000E59F0004ull, /* LDR R0,[PC,#4]; LDR R0,[PC,R0]     */
        .fingerprint_game_app_init = 0xE28DB01CE92D4FF0ull, /* PUSH {R4-R11,LR}; ADD R11,SP,#0x1C */
        .game_app_init_args      = kGameAppInitArgs961,
        .game_app_init_arg_count = kCount(kGameAppInitArgs961),
    },
};

const GameSymbols *g_active = &kUnknown;

/* Reads the 8 bytes at a .so offset as a little-endian u64, or 0 if the offset
 * lies outside the loaded image. */
std::uint64_t fingerprint_at(const pvz2_elf_image_t *img, std::uint32_t offset) {
    const std::uint64_t addr = (std::uint64_t)img->so_base + offset;
    if (offset == 0 || addr + 8 > img->mem_size) return 0;
    std::uint64_t v = 0;
    std::memcpy(&v, &img->mem[addr], 8);
    return v;
}

bool matches(const pvz2_elf_image_t *img, const GameSymbols &v) {
    return fingerprint_at(img, v.native.on_draw_frame) == v.fingerprint_draw_frame &&
           fingerprint_at(img, v.native.game_app_initialize) == v.fingerprint_game_app_init;
}

}  // namespace

const GameSymbols &sym() { return *g_active; }

bool game_symbols_detect(const pvz2_elf_image_t *img) {
    if (img == nullptr) return false;

    for (const GameSymbols &v : kVersions) {
        if (!matches(img, v)) continue;
        g_active = &v;
        pvz2_log("[version] libPVZ2.so identified as %s (onDrawFrame at 0x%x, verified)",
                 v.version, v.native.on_draw_frame);
        return true;
    }

    /* Not a version we know. Say exactly what was expected and what is
     * actually there, because that difference is the whole of the work needed
     * to add the build -- and booting on a guess would call whatever function
     * happens to live at another release's offsets. */
    pvz2_log("[version] this libPVZ2.so matches no known build (%u MB image, %u imports)",
             (unsigned)(img->so_span >> 20), img->trampoline_count);
    for (const GameSymbols &v : kVersions) {
        pvz2_log("[version]   tried %-8s: onDrawFrame 0x%x expected %016llx, found %016llx",
                 v.version, v.native.on_draw_frame,
                 (unsigned long long)v.fingerprint_draw_frame,
                 (unsigned long long)fingerprint_at(img, v.native.on_draw_frame));
    }
    pvz2_log("[version] add an entry to kVersions in src/game/symbols.cpp to support it");
    return false;
}

}  // namespace pvz2native
