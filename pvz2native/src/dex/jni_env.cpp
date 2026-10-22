/* The fake JNIEnv and JavaVM: everything that is JNI itself rather than a
 * particular Java class. The per-class behaviour lives in hooks/.
 *
 * The object model is the one simplification the whole layer rests on: a
 * "jobject" (jstring, jclass, jbyteArray, ...) IS the raw guest address of its
 * payload, and every reference-management call is an identity passthrough over
 * that address. Real traces confirm the engine treats them that way -- it calls
 * NewGlobalRef() directly on a jstring address we fabricated, stores it, and
 * hands it back later.
 *
 * Class and method identity is NOT faked, though: FindClass returns a stable
 * per-name handle and GetMethodID a stable per-(class, method) handle, both from
 * GuestRuntime's tables. That is what lets CallXxxMethod resolve which real Java
 * method the engine is invoking instead of returning 0 for all of them.
 */

#include <pvz2native/dex/dex.h>
#include <pvz2native/diagnostics/diagnostics.h>
#include <pvz2native/diagnostics/verbose.h>
#include <pvz2native/log.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace pvz2native {
namespace dex {
namespace {

/* JNINativeInterface_ slot order, straight from the real jni.h (OpenJDK, which
 * only ever appends to this table -- so indices up through GetObjectRefType
 * (232) match what 2013-era Android/ART actually exposes; anything after that
 * this binary could never have been compiled against). */
const char *const kSlotNames[] = {
    "reserved0", "reserved1", "reserved2", "reserved3", "GetVersion",
    "DefineClass", "FindClass", "FromReflectedMethod", "FromReflectedField",
    "ToReflectedMethod", "GetSuperclass", "IsAssignableFrom", "ToReflectedField",
    "Throw", "ThrowNew", "ExceptionOccurred", "ExceptionDescribe", "ExceptionClear",
    "FatalError", "PushLocalFrame", "PopLocalFrame", "NewGlobalRef", "DeleteGlobalRef",
    "DeleteLocalRef", "IsSameObject", "NewLocalRef", "EnsureLocalCapacity", "AllocObject",
    "NewObject", "NewObjectV", "NewObjectA", "GetObjectClass", "IsInstanceOf",
    "GetMethodID", "CallObjectMethod", "CallObjectMethodV", "CallObjectMethodA",
    "CallBooleanMethod", "CallBooleanMethodV", "CallBooleanMethodA", "CallByteMethod",
    "CallByteMethodV", "CallByteMethodA", "CallCharMethod", "CallCharMethodV",
    "CallCharMethodA", "CallShortMethod", "CallShortMethodV", "CallShortMethodA",
    "CallIntMethod", "CallIntMethodV", "CallIntMethodA", "CallLongMethod",
    "CallLongMethodV", "CallLongMethodA", "CallFloatMethod", "CallFloatMethodV",
    "CallFloatMethodA", "CallDoubleMethod", "CallDoubleMethodV", "CallDoubleMethodA",
    "CallVoidMethod", "CallVoidMethodV", "CallVoidMethodA", "CallNonvirtualObjectMethod",
    "CallNonvirtualObjectMethodV", "CallNonvirtualObjectMethodA", "CallNonvirtualBooleanMethod",
    "CallNonvirtualBooleanMethodV", "CallNonvirtualBooleanMethodA", "CallNonvirtualByteMethod",
    "CallNonvirtualByteMethodV", "CallNonvirtualByteMethodA", "CallNonvirtualCharMethod",
    "CallNonvirtualCharMethodV", "CallNonvirtualCharMethodA", "CallNonvirtualShortMethod",
    "CallNonvirtualShortMethodV", "CallNonvirtualShortMethodA", "CallNonvirtualIntMethod",
    "CallNonvirtualIntMethodV", "CallNonvirtualIntMethodA", "CallNonvirtualLongMethod",
    "CallNonvirtualLongMethodV", "CallNonvirtualLongMethodA", "CallNonvirtualFloatMethod",
    "CallNonvirtualFloatMethodV", "CallNonvirtualFloatMethodA", "CallNonvirtualDoubleMethod",
    "CallNonvirtualDoubleMethodV", "CallNonvirtualDoubleMethodA", "CallNonvirtualVoidMethod",
    "CallNonvirtualVoidMethodV", "CallNonvirtualVoidMethodA", "GetFieldID", "GetObjectField",
    "GetBooleanField", "GetByteField", "GetCharField", "GetShortField", "GetIntField",
    "GetLongField", "GetFloatField", "GetDoubleField", "SetObjectField", "SetBooleanField",
    "SetByteField", "SetCharField", "SetShortField", "SetIntField", "SetLongField",
    "SetFloatField", "SetDoubleField", "GetStaticMethodID", "CallStaticObjectMethod",
    "CallStaticObjectMethodV", "CallStaticObjectMethodA", "CallStaticBooleanMethod",
    "CallStaticBooleanMethodV", "CallStaticBooleanMethodA", "CallStaticByteMethod",
    "CallStaticByteMethodV", "CallStaticByteMethodA", "CallStaticCharMethod",
    "CallStaticCharMethodV", "CallStaticCharMethodA", "CallStaticShortMethod",
    "CallStaticShortMethodV", "CallStaticShortMethodA", "CallStaticIntMethod",
    "CallStaticIntMethodV", "CallStaticIntMethodA", "CallStaticLongMethod",
    "CallStaticLongMethodV", "CallStaticLongMethodA", "CallStaticFloatMethod",
    "CallStaticFloatMethodV", "CallStaticFloatMethodA", "CallStaticDoubleMethod",
    "CallStaticDoubleMethodV", "CallStaticDoubleMethodA", "CallStaticVoidMethod",
    "CallStaticVoidMethodV", "CallStaticVoidMethodA", "GetStaticFieldID",
    "GetStaticObjectField", "GetStaticBooleanField", "GetStaticByteField",
    "GetStaticCharField", "GetStaticShortField", "GetStaticIntField", "GetStaticLongField",
    "GetStaticFloatField", "GetStaticDoubleField", "SetStaticObjectField",
    "SetStaticBooleanField", "SetStaticByteField", "SetStaticCharField",
    "SetStaticShortField", "SetStaticIntField", "SetStaticLongField", "SetStaticFloatField",
    "SetStaticDoubleField", "NewString", "GetStringLength", "GetStringChars",
    "ReleaseStringChars", "NewStringUTF", "GetStringUTFLength", "GetStringUTFChars",
    "ReleaseStringUTFChars", "GetArrayLength", "NewObjectArray", "GetObjectArrayElement",
    "SetObjectArrayElement", "NewBooleanArray", "NewByteArray", "NewCharArray",
    "NewShortArray", "NewIntArray", "NewLongArray", "NewFloatArray", "NewDoubleArray",
    "GetBooleanArrayElements", "GetByteArrayElements", "GetCharArrayElements",
    "GetShortArrayElements", "GetIntArrayElements", "GetLongArrayElements",
    "GetFloatArrayElements", "GetDoubleArrayElements", "ReleaseBooleanArrayElements",
    "ReleaseByteArrayElements", "ReleaseCharArrayElements", "ReleaseShortArrayElements",
    "ReleaseIntArrayElements", "ReleaseLongArrayElements", "ReleaseFloatArrayElements",
    "ReleaseDoubleArrayElements", "GetBooleanArrayRegion", "GetByteArrayRegion",
    "GetCharArrayRegion", "GetShortArrayRegion", "GetIntArrayRegion", "GetLongArrayRegion",
    "GetFloatArrayRegion", "GetDoubleArrayRegion", "SetBooleanArrayRegion",
    "SetByteArrayRegion", "SetCharArrayRegion", "SetShortArrayRegion", "SetIntArrayRegion",
    "SetLongArrayRegion", "SetFloatArrayRegion", "SetDoubleArrayRegion", "RegisterNatives",
    "UnregisterNatives", "MonitorEnter", "MonitorExit", "GetJavaVM", "GetStringRegion",
    "GetStringUTFRegion", "GetPrimitiveArrayCritical", "ReleasePrimitiveArrayCritical",
    "GetStringCritical", "ReleaseStringCritical", "NewWeakGlobalRef", "DeleteWeakGlobalRef",
    "ExceptionCheck", "NewDirectByteBuffer", "GetDirectBufferAddress",
    "GetDirectBufferCapacity", "GetObjectRefType",
    /* Everything below postdates 2013 Android/ART; kept only so the table size
     * comfortably covers the real one -- this binary cannot call these. */
    "GetModule", "IsVirtualThread", "GetStringUTFLengthAsLong",
};
constexpr std::uint32_t kSlotCount = sizeof(kSlotNames) / sizeof(kSlotNames[0]);
/* The layout in guest_memmap.h reserves room for kJniTableSlots entries in both
 * the table and the stub block. Appending past that would silently overwrite the
 * next region rather than fail. */
static_assert(kSlotCount <= runtime::memmap::kJniTableSlots,
              "the JNI slot table outgrew the space reserved for it in guest_memmap.h");

/* (class, method) -> guest address of the engine native RegisterNatives bound.
 * Written during JNI_OnLoad, read when a hook calls a native back. */
std::mutex g_natives_lock;
std::map<std::pair<std::string, std::string>, std::uint32_t> g_native_methods;

/* Element counts for object arrays a hook builds and hands back to the engine.
 *
 * GetArrayLength answers heap.size_of(), which is the allocation in BYTES. That
 * is correct for a byte array (one byte per element) but not for an object array
 * (four bytes each): a hook that fabricates a jobject[] of n entries would have
 * the engine iterate 4n times and read 3n words of garbage past the end. So an
 * array registered here reports its real element count; everything else keeps
 * the byte-size answer, which is what the existing byte-array users rely on. */
std::mutex g_arraylen_lock;
std::map<std::uint32_t, std::uint32_t> g_array_lengths;

/* --------------------------------------------------------------- census */

struct CensusRow {
    std::uint64_t calls = 0;
    std::uint64_t reported = 0; /* value of `calls` at the last report */
    char ret_kind = 'v';
    bool hooked = false;
};

std::mutex g_census_lock;
std::map<std::pair<std::string, std::string>, CensusRow> g_census;
/* Calls whose method id resolves to nothing. Not a "method we failed to hook"
 * but a handle we never issued, which is a different and worse problem: the
 * engine is calling through a jmethodID that did not come from GetMethodID. */
std::uint64_t g_unresolved = 0;
std::uint64_t g_unresolved_reported = 0;

void census_record(const std::string &class_name, const std::string &method_name, char ret_kind,
                   bool hooked) {
    std::lock_guard<std::mutex> lk(g_census_lock);
    CensusRow &row = g_census[std::make_pair(class_name, method_name)];
    ++row.calls;
    row.ret_kind = ret_kind;
    row.hooked = hooked;
}

/* What an unhooked method of each return kind hands back, spelled out -- the
 * whole question when a stall is suspected is whether that value is one the
 * engine can proceed from. */
const char *unhooked_answer(char ret_kind) {
    switch (ret_kind) {
        case 'o': return "null";
        case 'b': return "false";
        case 'l': return "0L";
        case 'v': return "nothing";
        default: return "0";
    }
}

/* --------------------------------------------------------- CallXxxMethod */

/* [log] verbose: one timeline line per Java call the engine makes.
 *
 * Throttled per method, because the engine polls a few of them every frame and a
 * line per frame would bury everything else: the first calls are always shown,
 * after that at most one line per second per method, saying how many were left
 * out. UI_ProcessEvents is skipped outright -- the [input] lines already
 * cover it, and it is called every frame.
 *
 * Arguments are RAW: the va_list is read three words deep without a signature, so
 * a word may be a pointer, an int, half a long, or leftover memory when the
 * method takes fewer. Anything that points at printable text is shown as that
 * text, which is what makes URLs and event names readable. */
struct JavaTraceRow {
    std::uint64_t calls = 0;
    std::uint64_t suppressed = 0;
    std::chrono::steady_clock::time_point last_logged{};
};
std::mutex g_java_trace_lock;
std::map<std::pair<std::string, std::string>, JavaTraceRow> g_java_trace;

std::string describe_word(GuestCall &c, std::uint32_t w) {
    char buf[24];
    if (w >= 0x1000 && c.in_bounds(w, 4)) {
        std::string text;
        for (std::uint32_t i = 0; i < 96 && c.in_bounds(w + i, 1); ++i) {
            const std::uint8_t ch = c.read8(w + i);
            if (ch == 0) {
                if (text.size() >= 2) return "\"" + text + "\"";
                break;
            }
            if (ch < 0x20 || ch > 0x7e) {
                text.clear();
                break;
            }
            text.push_back((char)ch);
            if (i == 95) return "\"" + text + "...\"";
        }
    }
    std::snprintf(buf, sizeof(buf), "0x%x", w);
    return buf;
}

/* The engine narrates its own state machine through Crashlytics.setXxx(key, value):
 * "Board State", "WorldMap Shown", "GameStateMgr State", "Narration State",
 * "GameStateTransitionController TransitionState", "Resgroup Preload Refcounts"...
 * These are the closest thing to a debugger on the engine's screens and modals, and
 * the port otherwise throws them away. They are set every frame, so only a CHANGE of
 * a key's value is logged; the per-frame timers (FrameStart_*) are dropped. */
bool trace_crashlytics(const DexCall &call, const std::string &class_name,
                       const std::string &method_name) {
    if (class_name != "com/crashlytics/android/Crashlytics") return false;

    /* Breadcrumb text from the engine's own decision points ("GameStateMgr::
     * handleEndOfLevel - Showing world map because ..."). Rare, so every one is
     * logged in full, with where in the engine it came from. */
    if (method_name == "log") {
        diagnostics::vlog("engine-log", "%s  [callers:%s]", call.string_arg(0).c_str(),
                          diagnostics::guess_callers(call.c).c_str());
        return true;
    }
    if (method_name.compare(0, 3, "set") != 0) return false;

    const std::string key = call.string_arg(0);
    if (key.compare(0, 11, "FrameStart_") == 0) return true;

    char value[1100];
    if (method_name == "setString") {
        std::snprintf(value, sizeof(value), "%s", call.string_arg(1).c_str());
    } else if (method_name == "setBool") {
        std::snprintf(value, sizeof(value), "%s", (call.arg(1) & 0xff) ? "true" : "false");
    } else if (method_name == "setInt" || method_name == "setLong") {
        std::snprintf(value, sizeof(value), "%d", (int)call.arg(1));
    } else if (method_name == "setFloat" || method_name == "setDouble") {
        const std::uint64_t bits = ((std::uint64_t)call.arg(2) << 32) | call.arg(1);
        double d;
        std::memcpy(&d, &bits, sizeof(d));
        std::snprintf(value, sizeof(value), "%g", d);
    } else {
        return false;
    }

    static std::mutex lock;
    static std::map<std::string, std::string> last;
    {
        std::lock_guard<std::mutex> lk(lock);
        auto it = last.find(key);
        if (it != last.end() && it->second == value) return true;
        last[key] = value;
    }
    diagnostics::vlog("engine", "%s = %s", key.c_str(), value);
    /* The moments worth a census: a map that just became usable, a narration that
     * just ended, a level just won. */
    if ((key == "WorldMap State" && std::strcmp(value, "WM_Ready") == 0) ||
        (key == "Narration State" && std::strcmp(value, "CNM_Finished") == 0) ||
        (key == "Board State" && (std::strcmp(value, "PlayerWonOutro") == 0 ||
                                  std::strcmp(value, "EndLevel") == 0))) {
        const std::string why = key + " = " + value;
        diagnostics::request_census(why.c_str());
    }
    return true;
}

void trace_java_call(GuestCall &c, const DexCall &call, const std::string &class_name,
                     const std::string &method_name, bool hooked) {
    if (method_name == "UI_ProcessEvents") return;
    if (trace_crashlytics(call, class_name, method_name)) return;

    constexpr std::uint64_t kAlwaysShow = 12;
    constexpr auto kMinGap = std::chrono::seconds(1);
    const auto now = std::chrono::steady_clock::now();
    std::uint64_t skipped = 0;
    {
        std::lock_guard<std::mutex> lk(g_java_trace_lock);
        JavaTraceRow &row = g_java_trace[std::make_pair(class_name, method_name)];
        ++row.calls;
        if (row.calls > kAlwaysShow && now - row.last_logged < kMinGap) {
            ++row.suppressed;
            return;
        }
        skipped = row.suppressed;
        row.suppressed = 0;
        row.last_logged = now;
    }

    std::string args;
    if (call.va != 0) {
        for (int i = 0; i < 3; ++i) {
            if (i) args += ", ";
            args += describe_word(c, c.read32(call.va + (std::uint32_t)i * 4));
        }
    } else {
        args = "n/a";
    }
    const std::string short_class = class_name.substr(class_name.rfind('/') + 1);
    diagnostics::vlog("java", "%s.%s(%s) %s%s", short_class.c_str(), method_name.c_str(),
                      args.c_str(), hooked ? "" : "UNHOOKED ",
                      skipped ? ("(+" + std::to_string(skipped) + " calls not shown)").c_str() : "");
}

std::mutex g_board_level_lock;
std::string g_board_level;

/* Remembers Crashlytics.setString("Board Level", ...) -- the one breadcrumb the port
 * acts on, so it is kept even when verbose is off. */
void track_board_level(const DexCall &call, const std::string &class_name,
                       const std::string &method_name) {
    if (method_name != "setString" || class_name != "com/crashlytics/android/Crashlytics") return;
    if (call.string_arg(0) != "Board Level") return;
    const std::string level = call.string_arg(1);
    std::lock_guard<std::mutex> lk(g_board_level_lock);
    g_board_level = level;
}

/* Resolves the (class, method) the engine is invoking and runs its hook. */
void dispatch_call(GuestCall &c, char ret_kind, std::uint32_t va) {
    std::uint32_t method_id = c.regs[2];
    std::string class_name, method_name;

    DexCall call{c, class_name, method_name};
    call.ret_kind = ret_kind;
    call.thiz = c.regs[1];
    call.va = va;

    if (c.rt->lookup_method(method_id, class_name, method_name)) {
        MethodHook hook = hook_table().find(class_name, method_name);
        census_record(class_name, method_name, ret_kind, hook != nullptr);
        track_board_level(call, class_name, method_name);
        if (diagnostics::verbose()) trace_java_call(c, call, class_name, method_name, hook != nullptr);
        if (hook) {
            hook(call);
        } else if (ret_kind == 'o') {
            /* An unhooked OBJECT-returning method hands the engine null, and
             * some call sites feed that straight into std::string(const char *),
             * which aborts the guest with
             *   "basic_string::_S_construct null not valid".
             * A null return is legitimate for plenty of methods, so this is not
             * an error -- but it is the only place the offending method can be
             * named, and object-returning defaults are rare enough that saying
             * so unconditionally costs nothing. Capped so a per-frame caller
             * cannot flood the log. */
            static std::atomic<int> reported{0};
            if (reported.fetch_add(1, std::memory_order_relaxed) < 40) {
                c.log("[dex] %s.%s -> NULL (unhooked, returns an object)", class_name.c_str(),
                      method_name.c_str());
            }
        } else if (trace::enabled()) {
            /* Not an error: most callbacks legitimately want the Java default
             * (0 / false / null), which is what an unhooked method returns. */
            c.log("[dex] %s.%s -> default (%c)", class_name.c_str(), method_name.c_str(), ret_kind);
        }
    } else {
        std::lock_guard<std::mutex> lk(g_census_lock);
        ++g_unresolved;
    }

    if (ret_kind == 'l') {
        c.set_result64(call.result64);
    } else if (ret_kind != 'v') {
        c.set_result(call.result);
    }
}

/* -------------------------------------------------------------- the env */

bool name_is(const char *name, const char *want) { return std::strcmp(name, want) == 0; }

bool starts_with(const char *name, const char *prefix) {
    return std::strncmp(name, prefix, std::strlen(prefix)) == 0;
}

bool ends_with(const char *name, const char *suffix) {
    std::size_t n = std::strlen(name), s = std::strlen(suffix);
    return n >= s && std::strcmp(name + n - s, suffix) == 0;
}

void handle_jni(GuestCall &c, std::uint32_t slot) {
    std::uint32_t *regs = c.regs;
    const char *name = slot < kSlotCount ? kSlotNames[slot] : "<beyond-known-table>";

    if (trace::enabled()) {
        c.log("[jni] -> %s (slot %u) (env=0x%08x, r1=0x%08x, r2=0x%08x, r3=0x%08x)", name, slot,
              regs[0], regs[1], regs[2], regs[3]);
    }

    if (name_is(name, "GetVersion")) {
        regs[0] = 0x00010006; /* JNI_VERSION_1_6 */
    } else if (name_is(name, "GetJavaVM")) {
        c.write32(regs[1], kJavaVmPtrAddr);
        regs[0] = 0; /* JNI_OK */
    }
    /* --- strings ---
     *
     * Our jstring IS the UTF-8 byte pointer, so NewStringUTF and
     * GetStringUTFChars are both the identity. Aliasing the caller's buffer
     * rather than copying is safe because the engine's pattern is always
     * NewStringUTF -> Call...Method -> DeleteLocalRef with the source alive
     * throughout; copying would leak, since DeleteLocalRef is a no-op here. */
    else if (name_is(name, "NewStringUTF") || name_is(name, "NewString")) {
        regs[0] = regs[1];
    } else if (name_is(name, "GetStringUTFChars") || name_is(name, "GetStringChars")) {
        if (regs[2] != 0) c.write32(regs[2], 0); /* *isCopy = JNI_FALSE */
        regs[0] = regs[1];
    } else if (name_is(name, "ReleaseStringUTFChars") || name_is(name, "ReleaseStringChars") ||
               name_is(name, "ReleaseStringCritical")) {
        regs[0] = 0; /* nothing to release: we never copied */
    } else if (name_is(name, "GetStringLength") || name_is(name, "GetStringUTFLength")) {
        regs[0] = (std::uint32_t)c.cstr(regs[1], 4096).size();
    } else if (name_is(name, "GetStringUTFRegion") || name_is(name, "GetStringRegion")) {
        /* (str, start, len, buf) */
        std::uint32_t start = regs[2], len = regs[3], buf = c.arg(4);
        for (std::uint32_t i = 0; i < len; ++i) c.write8(buf + i, c.read8(regs[1] + start + i));
        regs[0] = 0;
    }
    /* --- classes and members --- */
    else if (name_is(name, "FindClass")) {
        regs[0] = c.rt->find_or_alloc_class(c.cstr(regs[1], 256));
    } else if (name_is(name, "GetObjectClass")) {
        std::string cn = c.rt->class_name_of_object(regs[1]);
        /* An untagged object gets a generic identity, not a specific bridge
         * class -- guessing one would send its method calls to the wrong hook. */
        if (cn.empty()) cn = "java/lang/Object";
        regs[0] = c.rt->find_or_alloc_class(cn);
    } else if (name_is(name, "GetSuperclass")) {
        regs[0] = c.rt->alloc_fake_handle(); /* nothing dispatches on superclass identity */
    } else if (name_is(name, "IsInstanceOf") || name_is(name, "IsAssignableFrom")) {
        regs[0] = 1;
    } else if (name_is(name, "GetMethodID") || name_is(name, "GetStaticMethodID")) {
        regs[0] = c.rt->find_or_alloc_method(regs[1], c.cstr(regs[2], 256));
    } else if (name_is(name, "GetFieldID") || name_is(name, "GetStaticFieldID")) {
        regs[0] = c.rt->alloc_fake_handle(); /* no field access appears in any real trace */
    }
    /* --- construction ---
     *
     * These used to fall through to the catch-all default and return 0, so
     * EVERY Java object the engine constructed came back null. The engine
     * stores that null and only complains much later, at use:
     *   "JavaMethod: no jobject to call <Class>.<method>"
     * which is how GooglePlayPurchaseDriver.Refresh() silently did nothing.
     *
     * The constructor itself is dispatched through the normal hook table (the
     * method id in r2 names it), so a hook on "<init>" can capture the
     * arguments -- the native-side handle these bridge classes are built with
     * is passed that way and is needed to call their callbacks back. */
    else if (name_is(name, "NewObject") || name_is(name, "NewObjectV") ||
             name_is(name, "NewObjectA") || name_is(name, "AllocObject")) {
        const std::uint32_t obj = c.rt->new_object(regs[1]);
        if (!name_is(name, "AllocObject")) { /* AllocObject deliberately skips <init> */
            const std::uint32_t saved_r1 = regs[1];
            regs[1] = obj; /* dispatch_call reads `thiz` from r1 */
            dispatch_call(c, 'v', ends_with(name, "V") || ends_with(name, "A") ? regs[3] : 0);
            regs[1] = saved_r1;
        }
        regs[0] = obj;
    }
    /* --- reference management: identity over the raw pointer --- */
    else if (name_is(name, "NewGlobalRef") || name_is(name, "NewLocalRef") ||
             name_is(name, "NewWeakGlobalRef")) {
        regs[0] = regs[1];
    } else if (name_is(name, "DeleteGlobalRef") || name_is(name, "DeleteLocalRef") ||
               name_is(name, "DeleteWeakGlobalRef") || name_is(name, "PushLocalFrame") ||
               name_is(name, "PopLocalFrame") || name_is(name, "EnsureLocalCapacity")) {
        regs[0] = 0;
    } else if (name_is(name, "IsSameObject")) {
        regs[0] = (regs[1] == regs[2]) ? 1u : 0u;
    } else if (name_is(name, "MonitorEnter") || name_is(name, "MonitorExit")) {
        /* Java monitors on our objects would guard nothing: the engine's own
         * pthread mutexes already protect everything shared. */
        regs[0] = 0;
    }
    /* --- arrays: the array object IS its element buffer --- */
    else if (name_is(name, "NewByteArray") || name_is(name, "NewBooleanArray")) {
        regs[0] = c.rt->heap.alloc(regs[1]);
    } else if (name_is(name, "NewCharArray") || name_is(name, "NewShortArray")) {
        regs[0] = c.rt->heap.alloc(regs[1] * 2u);
    } else if (name_is(name, "NewIntArray") || name_is(name, "NewFloatArray") ||
               name_is(name, "NewObjectArray")) {
        regs[0] = c.rt->heap.alloc(regs[1] * 4u);
    } else if (name_is(name, "NewLongArray") || name_is(name, "NewDoubleArray")) {
        regs[0] = c.rt->heap.alloc(regs[1] * 8u);
    } else if (name_is(name, "GetArrayLength")) {
        std::lock_guard<std::mutex> lk(g_arraylen_lock);
        auto it = g_array_lengths.find(regs[1]);
        regs[0] = (it != g_array_lengths.end()) ? it->second : c.rt->heap.size_of(regs[1]);
    } else if ((starts_with(name, "Get") && ends_with(name, "ArrayElements")) ||
               name_is(name, "GetPrimitiveArrayCritical") || name_is(name, "GetStringCritical")) {
        if (regs[2] != 0) c.write32(regs[2], 0); /* *isCopy = JNI_FALSE */
        regs[0] = regs[1];                       /* the array object already IS the buffer */
    } else if (starts_with(name, "Release")) {
        regs[0] = 0; /* every Release* form: nothing was ever copied */
    } else if (name_is(name, "SetByteArrayRegion") || name_is(name, "SetBooleanArrayRegion")) {
        /* (array, start, len, buf) -- buf is the 5th JNI argument, so it spills
         * onto the guest stack past r3. */
        std::uint32_t array = regs[1], start = regs[2], len = regs[3], buf = c.arg(4);
        for (std::uint32_t i = 0; i < len; ++i) c.write8(array + start + i, c.read8(buf + i));
        regs[0] = 0;
    } else if (name_is(name, "GetByteArrayRegion") || name_is(name, "GetBooleanArrayRegion")) {
        std::uint32_t array = regs[1], start = regs[2], len = regs[3], buf = c.arg(4);
        for (std::uint32_t i = 0; i < len; ++i) c.write8(buf + i, c.read8(array + start + i));
        regs[0] = 0;
    } else if (name_is(name, "GetObjectArrayElement")) {
        regs[0] = c.read32(regs[1] + regs[2] * 4);
    } else if (name_is(name, "SetObjectArrayElement")) {
        c.write32(regs[1] + regs[2] * 4, regs[3]);
        regs[0] = 0;
    }
    /* --- exceptions: nothing throws here --- */
    else if (name_is(name, "ExceptionCheck") || name_is(name, "ExceptionOccurred") ||
             name_is(name, "ExceptionClear") || name_is(name, "ExceptionDescribe") ||
             name_is(name, "Throw") || name_is(name, "ThrowNew")) {
        regs[0] = 0;
    }
    /* --- direct byte buffers ---
     *
     * Our object model already says "the object IS its payload address", so
     * wrapping and unwrapping are both the identity. Unimplemented, this
     * returned garbage and UI_ProcessEvents could not find its buffer. */
    else if (name_is(name, "NewDirectByteBuffer") || name_is(name, "GetDirectBufferAddress")) {
        regs[0] = regs[1];
    } else if (name_is(name, "GetDirectBufferCapacity")) {
        regs[0] = c.rt->heap.size_of(regs[1]);
    }
    /* --- registered natives ---
     *
     * RegisterNatives(clazz, JNINativeMethod methods[], nMethods). Each
     * JNINativeMethod is three pointers -- name, signature, fnPtr -- so 12 bytes
     * apart. Recording (class, name) -> fnPtr is what lets a hook call one of the
     * engine's own natives back (the purchase driver's FirePaymentComplete); left
     * unhooked this returned JNI_OK and silently threw the addresses away. */
    else if (name_is(name, "RegisterNatives")) {
        const std::string cn = c.rt->class_name_of_handle(regs[1]);
        const std::uint32_t methods = regs[2];
        const std::uint32_t count = regs[3];
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t entry = methods + i * 12;
            const std::uint32_t name_ptr = c.read32(entry + 0);
            const std::uint32_t fn_ptr = c.read32(entry + 8);
            if (name_ptr == 0 || fn_ptr == 0) continue;
            register_native_method(cn, c.cstr(name_ptr, 256), fn_ptr);
        }
        regs[0] = 0; /* JNI_OK */
    }
    /* --- calls into Java ---
     *
     * The V/A variants pass the arguments through a va_list in r3 (on AAPCS
     * that is just a pointer to the argument block), which is where a method
     * like Resources_GetAssetFileInfo reads them from. The plain variant
     * spreads them across r3 + the stack instead. */
    else if (starts_with(name, "Call")) {
        /* Decode the slot name rather than list all 60 Call* variants: strip
         * "Call", then the "Static"/"Nonvirtual" qualifier, and what remains
         * starts with the Java return type. */
        const char *tail = name + 4;
        if (starts_with(tail, "Static")) tail += 6;
        if (starts_with(tail, "Nonvirtual")) tail += 10;

        char kind = 'v';
        if (starts_with(tail, "Boolean")) kind = 'b';
        else if (starts_with(tail, "Object")) kind = 'o';
        else if (starts_with(tail, "Long")) kind = 'l';
        else if (starts_with(tail, "Void")) kind = 'v';
        else kind = 'i'; /* Int/Short/Byte/Char/Float/Double all come back in r0 */

        /* The V and A suffixes are what say the arguments arrive as a list. */
        bool has_va_list = ends_with(name, "V") || ends_with(name, "A");
        dispatch_call(c, kind, has_va_list ? regs[3] : 0);
    } else {
        regs[0] = 0; /* every remaining slot: the JNI default */
    }
}

/* ------------------------------------------------------------- the JavaVM */

void handle_javavm(GuestCall &c, std::uint32_t slot) {
    /* JNIInvokeInterface layout: 0-2 reserved, 3 DestroyJavaVM,
     * 4 AttachCurrentThread, 5 DetachCurrentThread, 6 GetEnv,
     * 7 AttachCurrentThreadAsDaemon.
     *
     * GetEnv/AttachCurrentThread* all take (JavaVM*, void** outEnv, ...) and
     * MUST actually write our fake JNIEnv* into *outEnv. Callers that re-derive
     * env through GetEnv() -- real engine code does this off the main thread and
     * from C++ singletons, not just at JNI_OnLoad -- otherwise get NULL and
     * dispatch through it. That exact gap made sub_9EB684's GetEnv() hand
     * sub_9EB77C a null env, which then called IsSameObject through it and
     * landed on garbage: the "exception 0 at pc=0x6090" during
     * applicationWillFinishLaunching. */
    if (slot == 4 || slot == 6 || slot == 7) {
        c.write32(c.regs[1], kJniEnvPtrAddr);
    }
    c.set_result(0); /* JNI_OK for all of them */
}

}  // namespace

/* ------------------------------------------------------------- interface */

std::string current_board_level() {
    std::lock_guard<std::mutex> lk(g_board_level_lock);
    return g_board_level;
}

std::uint32_t jni_slot_count() { return kSlotCount; }

const char *jni_slot_name(std::uint32_t slot) {
    return slot < kSlotCount ? kSlotNames[slot] : "<beyond-known-table>";
}

void register_native_method(const std::string &class_name, const std::string &method,
                            std::uint32_t fn_addr) {
    std::lock_guard<std::mutex> lk(g_natives_lock);
    g_native_methods[std::make_pair(class_name, method)] = fn_addr;
}
std::uint32_t find_native_method(const std::string &class_name, const std::string &method) {
    std::lock_guard<std::mutex> lk(g_natives_lock);
    auto it = g_native_methods.find(std::make_pair(class_name, method));
    return it == g_native_methods.end() ? 0 : it->second;
}

void set_array_length(std::uint32_t array, std::uint32_t count) {
    std::lock_guard<std::mutex> lk(g_arraylen_lock);
    g_array_lengths[array] = count;
}

std::uint32_t DexCall::arg(int i) const {
    if (va == 0) {
        /* The plain CallXxxMethod variant: arguments start at r3 and spill onto
         * the stack. Reading them there is possible but has never been needed,
         * and silently reading the wrong word is exactly the failure mode this
         * layer exists to avoid -- so say so. */
        c.log("[dex] %s.%s wants argument %d but was called without a va_list",
              class_name.c_str(), method_name.c_str(), i);
        return 0;
    }
    return c.read32(va + (std::uint32_t)i * 4);
}

std::string DexCall::string_arg(int i) const {
    std::uint32_t p = arg(i);
    return p == 0 ? std::string() : c.cstr(p, 1024);
}

void DexCall::ret_string(const std::string &s) {
    /* Each value manufactured while answering a call needs its own address that
     * stays live for the process: the engine keeps these (it calls NewGlobalRef
     * on them), so a shared scratch buffer would alias. */
    result = c.dup_cstr(s);
}

void report_call_census() {
    /* Wall time, not frames: the question is "has anything happened lately",
     * and at three thousand frames a second a frame count is not that. */
    using clock = std::chrono::steady_clock;
    constexpr auto kPeriod = std::chrono::seconds(5);
    constexpr std::size_t kMaxLines = 8;

    static clock::time_point last = clock::now();
    const clock::time_point now = clock::now();
    if (now - last < kPeriod) return;
    const double secs = std::chrono::duration<double>(now - last).count();
    last = now;

    struct Line {
        std::uint64_t delta;
        std::uint64_t total;
        std::string text;
        bool hooked;
        char ret_kind;
    };
    std::vector<Line> lines;
    std::uint64_t unresolved_delta = 0;
    {
        std::lock_guard<std::mutex> lk(g_census_lock);
        for (auto &kv : g_census) {
            CensusRow &row = kv.second;
            if (row.calls == row.reported) continue;
            lines.push_back({row.calls - row.reported, row.calls,
                             kv.first.first + "." + kv.first.second, row.hooked, row.ret_kind});
            row.reported = row.calls;
        }
        unresolved_delta = g_unresolved - g_unresolved_reported;
        g_unresolved_reported = g_unresolved;
    }

    if (lines.empty() && unresolved_delta == 0) {
        /* The useful negative result: the engine has not asked Java for
         * anything in five seconds. Whatever it is waiting on, it is not us --
         * so look at guest threads and locks, not at the hook table. */
        pvz2_log("[jni] no Java calls in the last %.0fs -- the engine is not waiting on "
                    "this layer", secs);
        return;
    }

    std::sort(lines.begin(), lines.end(),
              [](const Line &a, const Line &b) { return a.delta > b.delta; });

    pvz2_log("[jni] %zu method(s) called in the last %.0fs:", lines.size(), secs);
    for (std::size_t i = 0; i < lines.size() && i < kMaxLines; ++i) {
        const Line &l = lines[i];
        char note[64] = "";
        if (!l.hooked) {
            std::snprintf(note, sizeof(note), "  UNHOOKED -> %s", unhooked_answer(l.ret_kind));
        }
        pvz2_log("[jni]   %-58s x%-7llu (total %llu)%s", l.text.c_str(),
                    (unsigned long long)l.delta, (unsigned long long)l.total, note);
    }
    if (lines.size() > kMaxLines) {
        pvz2_log("[jni]   ... and %zu more", lines.size() - kMaxLines);
    }
    if (unresolved_delta != 0) {
        pvz2_log("[jni]   %llu call(s) through a jmethodID we never issued",
                    (unsigned long long)unresolved_delta);
    }
    std::fflush(stdout);
}

const HookTable &hook_table() {
    static const HookTable table = [] {
        HookTable t;
        register_android_game_app(t);
        register_android_surface_view(t);
        register_android_http(t);
        register_framework_activity(t);
        register_google_play(t);
        register_facebook(t);
        register_purchase_driver(t);
        return t;
    }();
    return table;
}

void install(pvz2_elf_image_t *img) {
    auto write32 = [&](std::uint32_t addr, std::uint32_t value) {
        std::memcpy(&img->mem[addr], &value, sizeof(value));
    };

    /* One "SVC #(base+i); BX LR" stub per slot, and a table of pointers to
     * them. Two instructions per stub, 8 bytes apart. */
    for (std::uint32_t i = 0; i < kSlotCount; ++i) {
        std::uint32_t stub = kJniStubsAddr + i * 8;
        write32(stub + 0, 0xEF000000u | (kJniSvcBase + i)); /* SVC #imm24 */
        write32(stub + 4, 0xE12FFF1Eu);                     /* BX LR      */
        write32(kJniTableAddr + i * 4, stub);
    }
    write32(kJniEnvPtrAddr, kJniTableAddr);

    for (std::uint32_t i = 0; i < kJavaVmSlots; ++i) {
        std::uint32_t stub = kJavaVmStubsAddr + i * 8;
        write32(stub + 0, 0xEF000000u | (kJavaVmSvcBase + i));
        write32(stub + 4, 0xE12FFF1Eu);
        write32(kJavaVmTableAddr + i * 4, stub);
    }
    write32(kJavaVmPtrAddr, kJavaVmTableAddr);

    pvz2_log("[dex] JNIEnv at 0x%08x (%u slots), JavaVM at 0x%08x, %zu method hooks",
                kJniEnvPtrAddr, kSlotCount, kJavaVmPtrAddr, hook_table().size());
}

bool owns_svc(std::uint32_t swi) {
    return (swi >= kJniSvcBase && swi < kJniSvcBase + kSlotCount) ||
           (swi >= kJavaVmSvcBase && swi < kJavaVmSvcBase + kJavaVmSlots);
}

void dispatch_svc(GuestCall &c, std::uint32_t swi) {
    if (swi >= kJavaVmSvcBase && swi < kJavaVmSvcBase + kJavaVmSlots) {
        handle_javavm(c, swi - kJavaVmSvcBase);
        return;
    }
    handle_jni(c, swi - kJniSvcBase);
}

}  // namespace dex
}  // namespace pvz2native
