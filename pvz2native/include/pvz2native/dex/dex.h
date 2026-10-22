#ifndef PVZ2NATIVE_DEX_DEX_H
#define PVZ2NATIVE_DEX_DEX_H

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <pvz2native/dependencies/dependency.h>
#include <pvz2native/runtime/guest_memmap.h>

namespace pvz2native {

/* The Java side of the game -- classes.dex -- reimplemented as native hooks.
 *
 * PvZ2's C++ engine calls back into Java constantly: for the screen size, the
 * .obb path, device info, config storage, input events, achievements. On a
 * device those calls land in com.popcap.SexyAppFramework.* and are answered by
 * the Android framework. There is no Dalvik here and writing one would be
 * absurd, so instead every method the engine actually invokes is implemented
 * natively, keyed by the (class, method) pair it names through JNI.
 *
 * The layering is deliberate:
 *   dex/jni_env.cpp  -- the fake JNIEnv/JavaVM themselves: object model,
 *                       string handling, arrays, reference management. Nothing
 *                       game-specific.
 *   dex/hooks/*.cpp  -- one file per Java class, holding only that class's
 *                       methods. This is where behaviour that was
 *                       ground-truthed against the decompiled classes.dex
 *                       lives, and where a new class gets added.
 *
 * Splitting it this way is what makes the answers auditable: each hook sits
 * next to a note saying what the real Java does and why this port answers the
 * way it does, instead of being one anonymous branch in a 200-line if-chain.
 */
namespace dex {

/* --- guest-memory layout of the fake JNI --------------------------------- *
 *
 * JNIEnv* is really a `struct JNINativeInterface**`: a pointer to a pointer to
 * a table of function pointers. Both levels are built in the emulated address
 * space, with every slot wired to its own SVC trampoline, so a JNI call from
 * guest code traps straight back to us and we know exactly which slot it was.
 *
 * WHERE they sit is not decided here -- runtime/guest_memmap.h owns the whole
 * sub-image layout and checks at build time that these do not collide with the
 * trampoline table, the scratch area or each other. Only the SVC number bases,
 * which are not addresses, belong to this layer. */
using runtime::memmap::kJavaVmPtrAddr;
using runtime::memmap::kJavaVmSlots;
using runtime::memmap::kJavaVmStubsAddr;
using runtime::memmap::kJavaVmTableAddr;
using runtime::memmap::kJniEnvPtrAddr;
using runtime::memmap::kJniStubsAddr;
using runtime::memmap::kJniTableAddr;

constexpr std::uint32_t kJniSvcBase = 10000;
constexpr std::uint32_t kJavaVmSvcBase = 20000;

std::uint32_t jni_slot_count();
const char *jni_slot_name(std::uint32_t slot);

/* Writes both fake vtables and their stubs into guest memory. Call once after
 * the image is loaded, before any guest code runs. */
void install(pvz2_elf_image_t *img);

/* --- dispatch ------------------------------------------------------------ *
 *
 * The harness routes SVC numbers here; these say which ones belong to us. */
bool owns_svc(std::uint32_t swi);
void dispatch_svc(GuestCall &c, std::uint32_t swi);

/* --- registered natives -------------------------------------------------- *
 *
 * RegisterNatives records the (class, method) -> guest function address the
 * engine binds for each Java `native` method, so this port can call one back the
 * way real Java would -- the emulated purchase driver needs it to invoke
 * FirePaymentComplete / FireDidRefresh. Recorded generically; find returns 0 for
 * a pair that was never registered. */
void register_native_method(const std::string &class_name, const std::string &method,
                            std::uint32_t fn_addr);
std::uint32_t find_native_method(const std::string &class_name, const std::string &method);

/* Records the element count of an object array a hook built, so GetArrayLength
 * reports elements rather than bytes for it -- see the g_array_lengths note in
 * jni_env.cpp. Only affects the exact handle passed. */
void set_array_length(std::uint32_t array, std::uint32_t count);

/* --- what a hook receives ------------------------------------------------ */

/* One CallXxxMethod arriving from guest code, already resolved to the real
 * (class, method) pair via the handle tables in GuestRuntime. */
struct DexCall {
    GuestCall &c;
    const std::string &class_name;
    const std::string &method_name;

    /* 'b' boolean, 'i' int, 'l' long, 'o' object/jstring, 'v' void. */
    char ret_kind = 'v';

    /* The jobject (or jclass, for a static call) the method was invoked on. */
    std::uint32_t thiz = 0;

    /* For the V/A call variants the Java arguments arrive through a va_list,
     * which on AAPCS is just a pointer to the argument block. Zero means the
     * plain variant was used and the arguments are spread across r3 + the
     * stack -- no hooked method needs those yet, and reading them from the
     * wrong place is silent, so `arg` reports instead of guessing. */
    std::uint32_t va = 0;

    /* The i-th Java argument (0-based). */
    std::uint32_t arg(int i) const;
    std::string string_arg(int i) const;

    void ret(std::uint32_t v) { result = v; }
    void ret_bool(bool v) { result = v ? 1u : 0u; }
    void ret64(std::uint64_t v) { result64 = v; }
    /* Allocates the text in guest memory and returns it as a jstring. Our
     * jstring IS its UTF-8 byte pointer -- see jni_env.cpp. */
    void ret_string(const std::string &s);

    std::uint32_t result = 0;
    std::uint64_t result64 = 0;
};

using MethodHook = void (*)(DexCall &);

/* Generic bodies shared by the online-service classes this port does not
 * implement (GooglePlay, Facebook, the purchase driver's ack methods): an
 * unconditional "not connected", a fire-and-forget acknowledgement, and ""
 * where Java would return null -- the engine feeds several of these straight
 * into std::string(const char *), which aborts on a real null. Kept here,
 * rather than copied per file, because the three were drifting identically in
 * google_play.cpp, facebook.cpp, purchase_driver.cpp and android_http.cpp. */
inline void hook_not_connected(DexCall &d) { d.ret_bool(false); }
inline void hook_ignored(DexCall &d) { d.ret(0); }
inline void hook_empty_string(DexCall &d) { d.ret_string(""); }

/* (class, method) -> hook. Names are the real ones from classes.dex, e.g.
 * "com/popcap/SexyAppFramework/AndroidGameApp" / "Info_SysGetPackageName". */
class HookTable {
public:
    void add(const char *class_name, const char *method, MethodHook fn) {
        map_[Key(class_name, method)] = fn;
    }
    MethodHook find(const std::string &class_name, const std::string &method) const {
        auto it = map_.find(Key(class_name, method));
        return it == map_.end() ? nullptr : it->second;
    }
    std::size_t size() const { return map_.size(); }

private:
    using Key = std::pair<std::string, std::string>;
    std::map<Key, MethodHook> map_;
};

/* --- the JNI census ------------------------------------------------------ *
 *
 * Every CallXxxMethod that arrives, counted per (class, method) and split by
 * whether a hook answered it.
 *
 * This exists for one symptom that nothing else can diagnose: the engine is
 * alive, frames tick, and it never finishes what it is doing. A stall is either
 * the engine waiting on an answer from Java that this port does not give, or it
 * is not talking to Java at all -- and those have opposite fixes. Counting the
 * calls separates them, and the DELTA between two reports is what shows a poll
 * loop: a method called four thousand times between heartbeats is the engine
 * asking the same question and never liking the answer.
 *
 * Reports the change since the last report, throttled to a few seconds, so it
 * stays readable at three thousand frames a second. "Nothing changed" is itself
 * the finding -- it means the stall is entirely inside guest code. */
void report_call_census();

/* The level the engine last announced through Crashlytics "Board Level" (e.g.
 * "goldbloom10"), or empty. Tracked whether or not verbose is on: the end-of-level
 * epic fix in game/patches.cpp needs it. */
std::string current_board_level();

/* One registrar per Java class, each in its own file under src/dex/hooks/. */
void register_android_game_app(HookTable &t);
void register_android_surface_view(HookTable &t);
void register_android_http(HookTable &t);

/* After DidBecomeActive, promote GetNetworkStatus none->wifi so LoD/map online
 * gates open. Must stay offline through onSurfaceCreated or the loading screen
 * hangs waiting on content downloads this port cannot finish. */
void net_mark_boot_finished();

/* One PACKAGES\\<NAME>.RTON from the OBB's Packages group (the files the engine itself loads), kept as
 * userdata/CDN.<tag>/<cache_name>.<tag> next to the files the emulated CDN serves, so that later launches
 * read that small file instead of inflating the group. Like those files, it is taken from the OBB only when
 * missing and on every launch with [runtime] rewrite_cdn=1; nothing else refreshes it. It is private to the
 * port: the game is not asked for it. Empty when neither the copy nor the OBB can be read. Safe before the
 * guest runs. */
std::vector<std::uint8_t> read_cdn_cached_package(GuestRuntime *rt, const char *pkg_path, const char *cache_name);

void register_framework_activity(HookTable &t);
void register_google_play(HookTable &t);
void register_facebook(HookTable &t);
void register_purchase_driver(HookTable &t);

const HookTable &hook_table();

/* Delivers any purchase results the emulated store owes the engine -- see
 * hooks/purchase_driver.cpp. Called once per frame from the session, BETWEEN
 * top-level guest calls, because completing a purchase means calling an engine
 * native and doing that reentrantly from inside RequestPayment could corrupt the
 * engine's own purchase state. A no-op when [game] emulate_iap is off or nothing
 * is pending. */
void iap_deliver_pending(pvz2_elf_image_t *img, GuestRuntime *rt);

/* The surface size the engine is told about -- everything geometric derives from
 * it (Graphics_GetScreenSizeInPixels -> LawnApp::SetWidthHeight) -- lives in
 * <pvz2native/surface.h>. This layer used to keep its own copy, defaulting to
 * 1280x720 while the other two copies defaulted to 960x540. */

}  // namespace dex
}  // namespace pvz2native

#endif
