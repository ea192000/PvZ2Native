/* The emulator: dynarmic's configuration, one JIT per guest thread, and the
 * primitives that enter guest code. See runtime/dynarmic_config.h for why this
 * layer holds no address into libPVZ2.so.
 */

#include <pvz2native/runtime/dynarmic_config.h>
#include <pvz2native/diagnostics/verbose.h>
#include <pvz2native/log.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <pvz2native/config.h>
#include <pvz2native/dex/dex.h>

namespace pvz2native {
namespace runtime {
namespace {

namespace dex = pvz2native::dex;
namespace guest_tls = pvz2native::guest_tls;

/* Ticks per Run() slice while non-zero -- see set_slice_override. */
std::atomic<std::uint32_t> g_slice_override{0};

/* Host callback run between JIT slices on the main thread. */
std::atomic<pvz2_host_pump_fn> g_host_pump{nullptr};

/* guest_tls::self_id is 1 on the thread driving the synchronous top-level calls and 2+ inside spawned guest
 * threads, so this keeps the pump on the window's own thread. The callback rate-limits itself. */
void pump_host_from_main_thread() {
    if (guest_tls::self_id != 1) return;
    if (pvz2_host_pump_fn pump = g_host_pump.load(std::memory_order_relaxed)) pump();
}

/* While booting, how many import calls pass between two host pumps (see handle_import_call). */
constexpr std::uint32_t kBootPumpEveryCalls = 0x4000;

std::mutex g_status_lock;
char g_status[160] = "starting";

/* Memory watchpoint state -- see watch_arm. */
std::uint32_t g_watch_lo = 0;
std::uint32_t g_watch_hi = 0;
std::uint32_t g_watch_budget = 0;

/* Imported-symbol handlers, cached by trampoline id.
 *
 * Every import arrives as "SVC #idx", and idx is stable for the life of the
 * process -- but the dispatcher used to re-resolve the NAME through a string
 * hash map on every single call. That is affordable at a few thousand calls and
 * absurd at the real volume: ResourceManager::Init issues millions of
 * __aeabi_memset calls (the compiler emits one per struct field), each of which
 * was re-hashing the same fourteen characters before doing four bytes of work.
 *
 * Sized once, before any guest code runs, and never resized: guest threads
 * index it concurrently, so growing it under them would be a data race on the
 * buffer itself. */
std::vector<ImportHandler> g_import_handlers;

pvz2_elf_image_t *g_image = nullptr;
GuestRuntime *g_runtime = nullptr;

CallStats g_last_call_stats;

ImportHandler resolve_import(std::uint32_t idx) {
    return idx < g_import_handlers.size() ? g_import_handlers[idx] : nullptr;
}

/* --- direct guest memory, without a host call per access -------------------
 *
 * Left unconfigured, dynarmic reaches guest memory only through the
 * UserCallbacks MemoryRead/MemoryWrite entry points -- a devirtualized host
 * call for every single LDR/STR the guest executes. Since img->mem is one flat
 * buffer covering the whole address space, an identity page table lets the JIT
 * inline those accesses as `page_table[vaddr >> 12][vaddr & 0xFFF]` instead,
 * and the callbacks then only serve what the table leaves unmapped (everything
 * at or above mem_size), which is exactly the range in_bounds() rejects.
 *
 * Entries are only installed for whole pages inside mem_size; the loader
 * allocates PVZ2_MEM_GUARD_SLACK past it so an unaligned wide access in the
 * final page stays inside the allocation. */
using Pvz2PageTable = std::array<std::uint8_t *, Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>;
std::unique_ptr<Pvz2PageTable> g_page_table;

std::unique_ptr<GuestThreadCtx> g_main_ctx;

Dynarmic::A32::UserConfig make_arm_user_config(GuestRuntime *rt, Pvz2Env *env,
                                               size_t processor_id, bool count_cycles) {
    Dynarmic::A32::UserConfig config;
    config.callbacks = env;
    config.global_monitor = &rt->monitor;
    config.processor_id = processor_id;
    /* Inline guest memory accesses -- see build_page_table(). Null (the config
     * opt-out) simply leaves every access on the callback path. */
    config.page_table = g_page_table.get();
    /* Every entry is img->mem, so the offset within the "page" is the whole
     * guest address. Saves a `mov`+`and` and, more usefully, the second scratch
     * register the JIT would otherwise reserve for the masked offset in every
     * load and store it emits. Set together with build_page_table(); see there. */
    config.absolute_offset_page_table = true;

    /* Tick accounting, and whether this context is worth its price -- see
     * GuestThreadCtx. Off means AddTicks/GetTicksRemaining are never called, so
     * Run() returns only when something halts it, which is exactly how
     * run_nested() drives worker threads and nested callbacks anyway. */
    config.enable_cycle_counting = count_cycles;

    /* All of dynarmic's SAFE optimizations: block linking, the return-stack
     * buffer, the fast dispatcher, and the IR passes. It is already the library
     * default, but stating it here makes it a decision rather than an accident,
     * and it is the biggest lever this build has for a weak host CPU -- every one
     * cuts either dispatcher lookups or emitted instructions. Unsafe opts (which
     * trade FP accuracy for speed) are deliberately NOT enabled: they are off by
     * default in dynarmic for a reason and a plants-vs-zombies frame is not worth
     * a wrong NaN. */
    config.optimizations = Dynarmic::all_safe_optimizations;

    /* The engine derives all its timing from the wall clock (gettimeofday), never
     * from the ARM generic timer, so tell the translator a wall clock backs
     * CNTPCT. That lets it skip the extra code it would otherwise emit around
     * cycle-timer reads -- a free reduction in generated code with no behavioural
     * change here. */
    config.wall_clock_cntpct = true;

    return config;
}

}  // namespace

/* --- small shared services -------------------------------------------------- */

void set_slice_override(std::uint32_t ticks) {
    g_slice_override.store(ticks, std::memory_order_relaxed);
}

void set_host_pump(pvz2_host_pump_fn fn) { g_host_pump.store(fn, std::memory_order_relaxed); }

void set_status(const char *fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lk(g_status_lock);
    std::memcpy(g_status, buf, sizeof(buf));
}

void copy_status(char *out, size_t n) {
    if (out == nullptr || n == 0) return;
    std::lock_guard<std::mutex> lk(g_status_lock);
    std::snprintf(out, n, "%s", g_status);
}

bool verbose_boot() { return pvz2_config()->verbose != 0; }

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void watch_arm(std::uint32_t lo, std::uint32_t hi, std::uint32_t budget) {
    g_watch_lo = lo;
    g_watch_hi = hi;
    g_watch_budget = budget;
    /* The page table bypasses the write callbacks entirely, so the target's
     * pages have to go back on the slow path or nothing is ever reported. */
    for (std::uint32_t p = lo; p < hi; p += 1u << Dynarmic::A32::UserConfig::PAGE_BITS) {
        unmap_page_for_watch(p);
    }
    if (hi > lo) unmap_page_for_watch(hi - 1);
}

void watch_disarm() {
    g_watch_lo = g_watch_hi = 0;
    g_watch_budget = 0;
}

void build_page_table(pvz2_elf_image_t *img) {
    /* The memory watchpoint only works through the callbacks, so keep a way to
     * turn the table off wholesale. */
    if (pvz2_config()->no_page_table) {
        pvz2_log("[runtime] no_page_table set -- guest memory goes through the slow "
                    "callback path");
        return;
    }
    constexpr std::uint32_t kPageSize = 1u << Dynarmic::A32::UserConfig::PAGE_BITS;
    g_page_table = std::make_unique<Pvz2PageTable>();
    g_page_table->fill(nullptr);
    std::uint32_t mapped = 0;
    for (std::uint64_t addr = 0; addr + kPageSize <= img->mem_size; addr += kPageSize) {
        /* img->mem, NOT img->mem + addr: absolute_offset_page_table is on, so
         * the JIT adds the full guest address to whatever it finds here. The
         * entry still doubles as the mapped/unmapped flag, which is what keeps
         * unmap_page_for_watch() and the out-of-bounds fallback working. */
        (*g_page_table)[addr >> Dynarmic::A32::UserConfig::PAGE_BITS] = img->mem;
        ++mapped;
    }
    pvz2_log("[jit] guest memory mapped directly into the JIT (%u pages, %u MB)", mapped,
                (unsigned)(img->mem_size >> 20));
}

void unmap_page_for_watch(std::uint32_t vaddr) {
    if (!g_page_table) return;
    (*g_page_table)[vaddr >> Dynarmic::A32::UserConfig::PAGE_BITS] = nullptr;
}

void build_import_handler_cache(pvz2_elf_image_t *img) {
    const ImportTable &table = import_table();
    g_import_handlers.assign(img->trampoline_capacity, nullptr);
    std::uint32_t resolved = 0;
    for (std::uint32_t i = 0; i < img->trampoline_count; ++i) {
        g_import_handlers[i] = table.find(img->trampoline_names[i]);
        if (g_import_handlers[i] != nullptr) ++resolved;
    }
    pvz2_log("[jit] %u/%u imported symbols bound to a dependency module", resolved,
                img->trampoline_count);
}

void set_session_image(pvz2_elf_image_t *img, GuestRuntime *rt) {
    g_image = img;
    g_runtime = rt;
}

HeapLayout heap_layout_for(const pvz2_elf_image_t *img) {
    /* Kept as a floor purely so 1.6 lands on exactly the address it always
     * has: its image ends at 0x00EF9700, which rounds up to below this. */
    constexpr std::uint32_t kFloor = 0x01000000;
    constexpr std::uint32_t kGranularity = 0x00100000; /* 1MB */

    /* Past the LAST module, not the main image: dependencies (libc++_shared,
     * libNimble) are mapped after libPVZ2, so so_base + so_span would put the
     * heap on top of them. images_end is 0 only for an image that was never
     * loaded, in which case the old expression is still the right answer. */
    const std::uint32_t image_end =
        img->images_end != 0 ? img->images_end : img->so_base + img->so_span;
    std::uint32_t base = (image_end + kGranularity - 1) & ~(kGranularity - 1);
    if (base < kFloor) base = kFloor;

    if (base >= kThreadStackBase) {
        pvz2_log("[runtime] image ends at 0x%08x, past the thread stacks at 0x%08x -- "
                    "no room for a guest heap", image_end, kThreadStackBase);
        return {base, 0};
    }

    HeapLayout layout{base, kThreadStackBase - base};
    pvz2_log("[runtime] image 0x%08x..0x%08x, guest heap 0x%08x..0x%08x (%u MB)",
                img->so_base, image_end, layout.base, kThreadStackBase, layout.size >> 20);
    return layout;
}

/* --- Pvz2Env ---------------------------------------------------------------- */

void Pvz2Env::begin_call() {
    ticks_used = 0;
    import_calls = 0;
    jni_calls = 0;
    halted_by_guest_return = false;
    quiet_call = false;
    exited_early = false;
    exit_retval = 0;
    should_halt = false;
    hot_lo = hot_hi = 0;
    hot_hits = 0;
    hot_seen_lrs.clear();
}

void Pvz2Env::note_watch(std::uint32_t vaddr, std::uint32_t size, std::uint32_t value) {
    if (g_watch_lo == g_watch_hi || g_watch_budget == 0) return;
    if (vaddr + size <= g_watch_lo || vaddr >= g_watch_hi) return;
    --g_watch_budget;
    auto &regs = jit->Regs();
    std::lock_guard<std::mutex> lg(rt->log_lock);
    pvz2_log("[watch] write%u 0x%08x = 0x%08x (%d)  pc=0x%08x lr=0x%08x "
                "r0=0x%08x r1=0x%08x r4=0x%08x",
                size * 8, vaddr, value, (int)value, regs[15], regs[14], regs[0], regs[1], regs[4]);
}

std::uint8_t Pvz2Env::MemoryRead8(std::uint32_t vaddr) {
    return in_bounds(vaddr, 1) ? img->mem[vaddr] : 0;
}
std::uint16_t Pvz2Env::MemoryRead16(std::uint32_t vaddr) {
    std::uint16_t v = 0;
    if (in_bounds(vaddr, 2)) std::memcpy(&v, &img->mem[vaddr], 2);
    return v;
}
std::uint32_t Pvz2Env::MemoryRead32(std::uint32_t vaddr) {
    std::uint32_t v = 0;
    if (in_bounds(vaddr, 4)) std::memcpy(&v, &img->mem[vaddr], 4);
    return v;
}
std::uint64_t Pvz2Env::MemoryRead64(std::uint32_t vaddr) {
    std::uint64_t v = 0;
    if (in_bounds(vaddr, 8)) std::memcpy(&v, &img->mem[vaddr], 8);
    return v;
}
void Pvz2Env::MemoryWrite8(std::uint32_t vaddr, std::uint8_t value) {
    note_watch(vaddr, 1, value);
    if (in_bounds(vaddr, 1)) img->mem[vaddr] = value;
}
void Pvz2Env::MemoryWrite16(std::uint32_t vaddr, std::uint16_t value) {
    note_watch(vaddr, 2, value);
    if (in_bounds(vaddr, 2)) std::memcpy(&img->mem[vaddr], &value, 2);
}
void Pvz2Env::MemoryWrite32(std::uint32_t vaddr, std::uint32_t value) {
    note_watch(vaddr, 4, value);
    if (in_bounds(vaddr, 4)) std::memcpy(&img->mem[vaddr], &value, 4);
}
void Pvz2Env::MemoryWrite64(std::uint32_t vaddr, std::uint64_t value) {
    note_watch(vaddr, 8, (std::uint32_t)value);
    if (in_bounds(vaddr, 8)) std::memcpy(&img->mem[vaddr], &value, 8);
}

bool Pvz2Env::MemoryWriteExclusive8(std::uint32_t vaddr, std::uint8_t value, std::uint8_t) {
    MemoryWrite8(vaddr, value);
    return true;
}
bool Pvz2Env::MemoryWriteExclusive16(std::uint32_t vaddr, std::uint16_t value, std::uint16_t) {
    MemoryWrite16(vaddr, value);
    return true;
}
bool Pvz2Env::MemoryWriteExclusive32(std::uint32_t vaddr, std::uint32_t value, std::uint32_t) {
    MemoryWrite32(vaddr, value);
    return true;
}
bool Pvz2Env::MemoryWriteExclusive64(std::uint32_t vaddr, std::uint64_t value, std::uint64_t) {
    MemoryWrite64(vaddr, value);
    return true;
}

/* See the declaration for why this is worth answering. Translation-time only,
 * so the cost of computing it per call does not matter; correctness does, and
 * the range comes from the ELF program headers rather than a guessed constant. */
bool Pvz2Env::IsReadOnlyMemory(std::uint32_t vaddr) {
    if (img == nullptr) return false;
    /* One range per mapped module. A union across modules would be wrong -- the
     * gap between two modules is unmapped, and folding a load from it would turn
     * a wild read into a silent constant instead of a zero. */
    for (std::uint32_t i = 0; i < img->module_count; ++i) {
        const pvz2_elf_module_t &m = img->modules[i];
        if (m.text_size == 0) continue;
        const std::uint32_t lo = m.base + m.text_vaddr;
        if (vaddr >= lo && vaddr < lo + m.text_size) return true;
    }
    return false;
}

/* See the header for why this exists: without it, a call through a null
 * function pointer NOP-slides from address 0 into the "$halt" sentinel and is
 * reported as a clean return. */
std::optional<std::uint32_t> Pvz2Env::MemoryReadCode(std::uint32_t vaddr) {
    if (img != nullptr && vaddr < img->trampoline_base) return std::nullopt;
    return MemoryRead32(vaddr);
}

void Pvz2Env::InterpreterFallback(std::uint32_t pc, size_t num_instructions) {
    {
        std::lock_guard<std::mutex> lg(rt->log_lock);
        pvz2_log("[jit] InterpreterFallback at pc=0x%08x (%zu instr) -- halting", pc,
                    num_instructions);
    }
    should_halt = true;
    jit->HaltExecution();
}

GuestCall Pvz2Env::make_guest_call() {
    GuestCall call;
    call.img = img;
    call.rt = rt;
    call.regs = jit->Regs().data();
    call.env = this;
    call.halt_fn = &Pvz2Env::hook_halt;
    call.note_blocked_fn = &Pvz2Env::hook_note_blocked;
    call.spawn_thread_fn = &Pvz2Env::hook_spawn_thread;
    call.join_thread_fn = &Pvz2Env::hook_join_thread;
    call.call_guest_fn = &Pvz2Env::hook_call_guest;
    return call;
}

/* A blocking sync primitive reports here that the thread is about to wait.
 *
 * The runaway tick budget (kTickBudget) exists to catch a guest loop that never
 * yields; for the MAIN thread it is bounded per top-level call by begin_call().
 * A spawned worker, though, runs one entry point for the whole session and never
 * calls begin_call, so its ticks_used only ever grows -- and Wwise's audio
 * threads legitimately execute billions of instructions over a few minutes, so
 * they hit the budget and got HALTED mid-session. That killed the thread that
 * posts the audio semaphores, and the game deadlocked (tid=1/tid=7 stuck in
 * sem_wait) -- the real cause behind "freezes after a long while".
 *
 * Blocking on a semaphore or condition is proof the thread is making progress,
 * not spinning, so reset its budget here. A genuine spinlock never blocks and
 * still trips the cap. Only spawned threads need it; the main thread keeps its
 * per-call accounting so the heartbeat's tick figures stay honest. */
void Pvz2Env::hook_note_blocked(void *env) {
    if (guest_tls::self_id == 1) return; /* main thread: begin_call owns its budget */
    static_cast<Pvz2Env *>(env)->ticks_used = 0;
}

void Pvz2Env::hook_halt(void *env, const char *why) {
    auto *self = static_cast<Pvz2Env *>(env);
    /* pthread_exit is the one halt that carries a value back to whoever joins
     * this thread, and the only one after which resuming at the return address
     * would be wrong. */
    if (std::strcmp(why, "pthread_exit") == 0) {
        self->exited_early = true;
        self->exit_retval = self->jit->Regs()[0];
    }
    self->should_halt = true;
    self->jit->HaltExecution();
}

std::uint32_t Pvz2Env::hook_spawn_thread(void *env, std::uint32_t start, std::uint32_t arg) {
    return spawn_guest_thread_impl(static_cast<Pvz2Env *>(env)->rt, start, arg);
}

std::uint32_t Pvz2Env::hook_join_thread(void *env, std::uint32_t id) {
    return join_guest_thread_impl(static_cast<Pvz2Env *>(env)->rt, id);
}

void Pvz2Env::return_to_caller() {
    auto &regs = jit->Regs();
    const std::uint32_t lr = regs[14];
    regs[15] = lr & ~1u;
    if ((lr & 1u) != 0) jit->SetCpsr(jit->Cpsr() | 0x20u); /* resume in Thumb */
}

void Pvz2Env::handle_import_call(std::uint32_t idx) {
    auto &regs = jit->Regs();
    const char *name = (idx < img->trampoline_count) ? img->trampoline_names[idx] : "<unknown>";
    ++import_calls;
    /* Run() only returns to run_jit_sliced when its tick slice runs out, but every import call hands the JIT a
     * fresh slice. A boot phase that is mostly import calls (onSurfaceCreated: ~16M of them in ~20 s) therefore
     * keeps the JIT running for seconds, with no host pump: the window is not repainted and the loading bar
     * freezes. Pumping from here, before the handler runs, is the same as the pump at a slice boundary. Boot
     * only: once frames run, every frame ends in a guest return and the pump already runs between frames. */
    static thread_local std::uint32_t calls_since_pump = 0; /* import_calls restarts with every guest call */
    if ((++calls_since_pump & (kBootPumpEveryCalls - 1)) == 0 && diagnostics::current_frame() == 0) {
        pump_host_from_main_thread();
    }
    if (trace::enabled()) {
        if (import_calls == kTraceCallLimit) {
            std::lock_guard<std::mutex> lg(rt->log_lock);
            trace::set(false);
            pvz2_log("[jit] reached the trace limit (%u calls in this guest call) -- tracing "
                        "off, execution continues", kTraceCallLimit);
            std::fflush(stdout);
        } else {
            std::lock_guard<std::mutex> lg(rt->log_lock);
            pvz2_log("[jit] import call #%u -> %s(r0=0x%08x, r1=0x%08x, r2=0x%08x, r3=0x%08x) "
                        "lr=0x%08x",
                        import_calls, name, regs[0], regs[1], regs[2], regs[3], regs[14]);
        }
    }

    /* No import-call cap: import_calls is kept purely as a diagnostic counter.
     * A legitimate boot issues hundreds of millions of import calls (per-field
     * __aeabi_memset in ResourceManager::Init, the 65536-iteration font-metrics
     * sweep, etc.), and long-lived worker threads run indefinitely, so any fixed
     * ceiling here is a guaranteed false positive rather than a runaway guard. */

    if (ImportHandler fn = resolve_import(idx)) {
        GuestCall call = make_guest_call();
        fn(call);
        /* See gl_strict_enabled(): names the GL call that produced an error,
         * instead of letting it surface at the next draw. */
        if (gl_strict_enabled() && name[0] == 'g' && name[1] == 'l') {
            gl_check_error_after(call, name);
        }
        /* A handler that terminates the thread (pthread_exit) or redirects it
         * (longjmp writes its target into lr) must not be followed by the
         * ordinary return-to-caller. */
        if (call.returns) return_to_caller();
        return;
    }

    /* No implementation: return 0 and resume as if the call trivially
     * succeeded -- but SAY SO, once per distinct name.
     *
     * This silence has been the single most expensive bug class in the project.
     * An unimplemented import used to fall through the whole dispatch chain
     * leaving r0 untouched, so the guest read garbage and the symptom always
     * surfaced far away, looking like corrupt engine data rather than a missing
     * shim. wmemcpy, NewStringUTF, CallLongMethodV and Resources_GetAssetFileSize
     * were each found only after a long hunt; every one would have been a single
     * line in a dependency module. */
    {
        static std::mutex seen_lock;
        static std::set<std::string> seen;
        bool first = false;
        {
            std::lock_guard<std::mutex> lk(seen_lock);
            first = seen.insert(name).second;
        }
        if (first) {
            std::lock_guard<std::mutex> lg(rt->log_lock);
            pvz2_log("[MISSING IMPORT] %s -- unimplemented, returning 0 (lr=0x%08x)",
                        name, regs[14]);
            std::fflush(stdout);
        }
    }
    regs[0] = 0;
    return_to_caller();
}

void Pvz2Env::CallSVC(std::uint32_t swi) {
    if (swi == 0) {
        /* Boot only: after that every audio callback and nested call ends here too. */
        if (verbose_boot() && !quiet_call && diagnostics::current_frame() == 0) {
            std::lock_guard<std::mutex> lg(rt->log_lock);
            pvz2_log("[jit] guest function returned to host (halt sentinel)");
        }
        halted_by_guest_return = true;
        should_halt = true;
        jit->HaltExecution();
        return;
    }

    /* JNI and JavaVM slots belong to the DEX layer (src/dex/), which owns the
     * fake JNIEnv it installed at startup. */
    if (dex::owns_svc(swi)) {
        ++jni_calls;
        GuestCall call = make_guest_call();
        dex::dispatch_svc(call, swi);
        if (call.returns) return_to_caller();
        return;
    }

    handle_import_call(swi);
}

void Pvz2Env::ExceptionRaised(std::uint32_t pc, Dynarmic::A32::Exception exception) {
    auto &regs = jit->Regs();
    {
        std::lock_guard<std::mutex> lg(rt->log_lock);
        /* NoExecuteFault below the trampoline table is MemoryReadCode refusing
         * the null page, and it has exactly one cause worth naming: the guest
         * branched through a function pointer that was never filled in --
         * almost always a vtable slot on an object that is still zeroed, or an
         * object that was never constructed at all. LR is the call site, so it
         * names the caller directly; pc is null plus whatever field offset the
         * dispatch added, which identifies the slot. */
        if (exception == Dynarmic::A32::Exception::NoExecuteFault && img != nullptr &&
            pc < img->trampoline_base) {
            /* One line per distinct call site. A dead vtable slot is almost
             * always reached from onDrawFrame, so without this the first
             * occurrence -- the one during boot that explains the rest -- is
             * pushed out of the console by thousands of identical frames. */
            static std::set<std::uint64_t> seen;
            if (seen.insert(((std::uint64_t)pc << 32) | regs[14]).second) {
                pvz2_log("[NULL CALL] the guest branched to 0x%08x -- a null/uninitialised "
                            "function pointer, called from lr=0x%08x (offset 0x%x). r0=0x%08x "
                            "r1=0x%08x r2=0x%08x r3=0x%08x -- halting (this call site is "
                            "reported once)",
                            pc, regs[14],
                            regs[14] >= img->so_base ? regs[14] - img->so_base : regs[14],
                            regs[0], regs[1], regs[2], regs[3]);
            }
        } else {
            pvz2_log("[jit] exception %d raised at pc=0x%08x lr=0x%08x r0=0x%08x r1=0x%08x "
                        "r2=0x%08x r3=0x%08x -- halting",
                        static_cast<int>(exception), pc, regs[14], regs[0], regs[1], regs[2],
                        regs[3]);
        }
    }
    should_halt = true;
    jit->HaltExecution();
}

void Pvz2Env::AddTicks(std::uint64_t ticks) {
    ticks_used += ticks;
    if (ticks_used < kTickBudget) return;

    /* The budget is a runaway-loop guard for the WINDOW thread only. If the main
     * thread spins without yielding, the window stops pumping and the app hangs,
     * so halting it is the right call. A spawned worker crossing the budget is a
     * different story: Wwise's audio threads legitimately run billions of
     * instructions over minutes, and halting one kills whatever it feeds (the
     * audio semaphores) -- which is the real cause behind "audio dies / the game
     * freezes after ~5 minutes". note_blocked already rescues a worker that
     * blocks on a primitive; this is the backstop for one that computes straight
     * through without blocking. So a worker is never halted here: its budget is
     * reset (rearming the guard) and it keeps running. A true worker spinlock
     * then merely pins a core instead of deadlocking the game -- the lesser evil,
     * and far rarer than this false positive was. */
    if (guest_tls::self_id != 1) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) {
            std::lock_guard<std::mutex> lg(rt->log_lock);
            pvz2_log("[thread %u] crossed the %llu-tick mark -- long-lived worker, "
                        "budget not enforced (normal for audio/streaming threads)",
                        guest_tls::self_id, (unsigned long long)kTickBudget);
        }
        ticks_used = 0;
        return;
    }

    {
        std::lock_guard<std::mutex> lg(rt->log_lock);
        pvz2_log("[jit] tick budget exhausted (%llu), halting",
                    (unsigned long long)ticks_used);
    }
    should_halt = true;
    jit->HaltExecution();
}

std::uint64_t Pvz2Env::GetTicksRemaining() {
    /* Only the main thread is bounded by the budget (see AddTicks); a worker
     * always gets a full slice, and its ticks_used is reset before it could ever
     * reach kTickBudget, so it never falls into the "return 0" that would stop
     * it dead. */
    std::uint64_t remaining = kPcSampleSlice;
    if (guest_tls::self_id == 1) {
        if (ticks_used >= kTickBudget) return 0;
        remaining = kTickBudget - ticks_used;
    }
    if (std::uint32_t slice = g_slice_override.load(std::memory_order_relaxed)) {
        return std::min<std::uint64_t>(remaining, slice);
    }
    if (hot_lo != hot_hi && hot_hits < kHotHitCap) {
        std::uint32_t pc = jit->Regs()[15];
        if (pc >= hot_lo && pc < hot_hi) return std::min<std::uint64_t>(remaining, 16);
    }
    return std::min<std::uint64_t>(remaining, kPcSampleSlice);
}

/* --- contexts and call setup ------------------------------------------------ */

GuestThreadCtx::GuestThreadCtx(pvz2_elf_image_t *img, GuestRuntime *rt, size_t processor_id,
                               bool count_cycles)
    : jit(make_arm_user_config(rt, &env, processor_id, count_cycles)) {
    env.img = img;
    env.rt = rt;
    env.jit = &jit;
}

GuestThreadCtx &main_ctx(pvz2_elf_image_t *img, GuestRuntime *rt) {
    if (!g_main_ctx) g_main_ctx = std::make_unique<GuestThreadCtx>(img, rt, 0);
    g_main_ctx->env.begin_call();
    return *g_main_ctx;
}

void release_main_ctx() { g_main_ctx.reset(); }

void apply_call_setup(Dynarmic::A32::Jit &jit, Pvz2Env &env, pvz2_elf_image_t *img,
                      const GuestCallSetup &setup) {
    env.hot_lo = setup.hot_lo;
    env.hot_hi = setup.hot_hi;
    for (size_t i = 0; i < setup.stack_args.size(); ++i) {
        std::uint32_t val = setup.stack_args[i];
        std::memcpy(&img->mem[setup.sp + (std::uint32_t)i * 4], &val, 4);
    }
    auto &regs = jit.Regs();
    regs[0] = setup.r0;
    regs[1] = setup.r1;
    regs[2] = setup.r2;
    regs[3] = setup.r3;
    regs[13] = setup.sp;
    regs[14] = setup.lr;

    /* ARM/Thumb interworking, which every entry into guest code has to honour.
     *
     * A function POINTER carries the instruction set in its low bit: odd means
     * Thumb, and the real entry address is the value with that bit cleared.
     * Forcing ARM mode here regardless -- which is what this did -- makes the
     * JIT decode Thumb halfwords as 32-bit ARM words. That garbage decodes into
     * arbitrary instructions, and the process dies inside dynarmic's translator
     * on `assertion failed: reg != A32::Reg::PC` (a32_ir_emitter.cpp) when one
     * of them happens to name r15 as a destination -- a message that says
     * nothing about the actual cause.
     *
     * 1.6 hid this completely: every one of its 619 .init_array entries is ARM.
     * 4.5.2 has exactly ONE Thumb entry out of 1030, at index 1006, so it ran
     * 1005 constructors and then aborted. It applies to every entry point, not
     * just constructors -- a Thumb qsort comparator or OpenSL callback would
     * have failed the same way.
     *
     * Interworking WITHIN guest code needs nothing from us: BX/BLX carry the
     * bit themselves and dynarmic already tracks the T flag across them. */
    std::uint32_t entry_pc = setup.entry_pc;
    std::uint32_t cpsr = setup.cpsr;
    constexpr std::uint32_t kCpsrThumbBit = 0x20;
    if ((entry_pc & 1u) != 0) {
        entry_pc &= ~1u;
        cpsr |= kCpsrThumbBit;
    } else {
        cpsr &= ~kCpsrThumbBit;
    }
    regs[15] = entry_pc;
    jit.SetCpsr(cpsr);
}

void run_nested(Dynarmic::A32::Jit &jit, Pvz2Env &env) {
    while (!env.should_halt) jit.Run();
}

Dynarmic::HaltReason run_jit_sliced(Dynarmic::A32::Jit &jit, Pvz2Env &env) {
    Dynarmic::HaltReason hr{};
    for (;;) {
        hr = jit.Run();
        pump_host_from_main_thread();
        if (env.should_halt) break; /* covers every real stop condition */
        if (!pvz2_config()->pc_sample) continue;
        std::uint32_t pc = jit.Regs()[15];
        if (env.hot_lo != env.hot_hi && pc >= env.hot_lo && pc < env.hot_hi) {
            env.hot_hits++;
            bool new_lr = env.hot_seen_lrs.insert(jit.Regs()[14]).second;
            if (env.hot_hits <= 40 || new_lr) {
                std::lock_guard<std::mutex> lg(env.rt->log_lock);
                pvz2_log("[HOT #%u%s] tid=%u pc=0x%08x lr=0x%08x r0=0x%08x r1=0x%08x "
                            "r4=0x%08x sp=0x%08x",
                            env.hot_hits, new_lr ? " NEW-LR" : "", guest_tls::self_id, pc,
                            jit.Regs()[14], jit.Regs()[0], jit.Regs()[1], jit.Regs()[4],
                            jit.Regs()[13]);
            } else if (env.hot_hits == env.kHotHitCap) {
                std::lock_guard<std::mutex> lg(env.rt->log_lock);
                pvz2_log("[HOT] cap reached (%u hits, %zu distinct LRs) -- reverting to "
                            "coarse sampling", env.hot_hits, env.hot_seen_lrs.size());
            }
            continue;
        }
        {
            std::lock_guard<std::mutex> lg(env.rt->log_lock);
            pvz2_log("[pc-sample] tid=%u ticks=%llu pc=0x%08x lr=0x%08x sp=0x%08x",
                        guest_tls::self_id, (unsigned long long)env.ticks_used, jit.Regs()[15],
                        jit.Regs()[14], jit.Regs()[13]);
        }
    }
    return hr;
}

/* --- nested calls and threads ----------------------------------------------- */

std::uint32_t Pvz2Env::hook_call_guest(void *env, std::uint32_t fn, const std::uint32_t *args,
                                       int nargs) {
    auto *outer = static_cast<Pvz2Env *>(env);
    GuestRuntime *rt = outer->rt;
    if (fn == 0) return 0;

    constexpr std::uint32_t kNestedStackSize = 128 * 1024;
    static thread_local std::uint32_t nested_stack_top = 0;
    if (nested_stack_top == 0) {
        std::uint32_t base = rt->heap.alloc(kNestedStackSize);
        if (base == 0) {
            std::lock_guard<std::mutex> lg(rt->log_lock);
            pvz2_log("[call-guest] no room for a nested stack -- callback 0x%08x "
                        "skipped", fn);
            return 0;
        }
        nested_stack_top = (base + kNestedStackSize) & ~7u;
    }

    GuestCallSetup setup;
    if (nargs > 0) setup.r0 = args[0];
    if (nargs > 1) setup.r1 = args[1];
    if (nargs > 2) setup.r2 = args[2];
    if (nargs > 3) setup.r3 = args[3];
    for (int i = 4; i < nargs; ++i) setup.stack_args.push_back(args[i]);
    setup.sp = nested_stack_top;
    setup.lr = outer->img->trampoline_base; /* LR -> the "$halt" sentinel */
    setup.entry_pc = fn;

    /* A qsort comparator is called once per comparison, and building a Jit per
     * call meant reserving a 128MB code cache and re-translating the comparator
     * every single time. One reusable nested Jit per guest thread translates it
     * once. A callback that itself makes another guest callback would re-enter
     * this Jit while it is mid-Run(), so that (rare) case still gets a
     * throwaway one. */
    static thread_local std::unique_ptr<GuestThreadCtx> nested_ctx;
    static thread_local int nested_depth = 0;

    if (nested_depth == 0) {
        if (!nested_ctx) {
            /* No cycle counting: run_nested() drives this to completion in one
             * go, nothing samples it, and the per-SVC tick bookkeeping would
             * triple the host-call cost of a comparator called once per
             * comparison. See GuestThreadCtx. */
            nested_ctx = std::make_unique<GuestThreadCtx>(outer->img, rt, guest_tls::self_id, false);
        }
        Pvz2Env &inner = nested_ctx->env;
        inner.begin_call();
        apply_call_setup(nested_ctx->jit, inner, outer->img, setup);

        ++nested_depth;
        run_nested(nested_ctx->jit, inner);
        --nested_depth;

        /* The nested call's ticks are the outer call's ticks: it is the same
         * guest thread doing the work, and the budget bounds total execution.
         * Now always 0, since the nested context does not count cycles -- kept
         * so the accounting is right again the moment that is turned back on. */
        outer->ticks_used += inner.ticks_used;
        return nested_ctx->jit.Regs()[0];
    }

    GuestThreadCtx deep(outer->img, rt, guest_tls::self_id, false);
    apply_call_setup(deep.jit, deep.env, outer->img, setup);

    ++nested_depth;
    run_nested(deep.jit, deep.env);
    --nested_depth;

    outer->ticks_used += deep.env.ticks_used;
    return deep.jit.Regs()[0];
}

std::uint32_t Pvz2Env::spawn_guest_thread_impl(GuestRuntime *rt, std::uint32_t start_routine,
                                               std::uint32_t arg) {
    std::uint32_t stack_top;
    std::uint32_t id;
    {
        std::lock_guard<std::mutex> lock(rt->threads_lock);
        if (rt->next_stack_slot >= kThreadStackMax) {
            std::lock_guard<std::mutex> lg(rt->log_lock);
            pvz2_log("[thread] out of guest thread stack slots (max %u)", kThreadStackMax);
            return 0;
        }
        stack_top = kThreadStackBase + (rt->next_stack_slot + 1) * kThreadStackSize;
        rt->next_stack_slot++;
        id = rt->next_thread_id++;
    }

    std::thread th([rt, start_routine, arg, stack_top, id]() {
        guest_tls::self_id = id;
        /* One Jit for the whole life of the thread -- it only ever runs this one
         * entry point, but hook_call_guest may reuse the context too.
         *
         * No cycle counting: AddTicks() already refuses to enforce the budget on
         * a spawned thread (halting one killed the audio), and nothing here
         * pumps the window or samples the PC, so the accounting bought nothing
         * while charging two extra host calls per import -- on precisely the
         * threads that run for the whole session. */
        GuestThreadCtx ctx(rt->img, rt, id, false);
        Pvz2Env &env = ctx.env;
        Dynarmic::A32::Jit &jit = ctx.jit;
        auto &regs = jit.Regs();

        GuestCallSetup setup;
        setup.r0 = arg;
        setup.sp = stack_top;
        setup.lr = rt->img->trampoline_base; /* LR -> "$halt" sentinel */
        setup.entry_pc = start_routine;
        apply_call_setup(jit, env, rt->img, setup);

        {
            std::lock_guard<std::mutex> lg(rt->log_lock);
            pvz2_log("[thread %u] starting at 0x%08x (arg=0x%08x)", id, start_routine,
                        arg);
        }
        /* Same slice trap as the nested callbacks: one Run() lasts a tick
         * slice, not a whole thread. This thread used to stop dead after
         * kPcSampleSlice ticks and report itself "finished" -- which is what a
         * worker that issues its read() and then never writes a completion
         * status looks like from the outside. */
        run_nested(jit, env);
        std::uint32_t retval = env.exited_early ? env.exit_retval : regs[0];
        {
            std::lock_guard<std::mutex> lg(rt->log_lock);
            pvz2_log("[thread %u] finished, retval=0x%08x, import_calls=%u, "
                        "jni_calls=%u", id, retval, env.import_calls, env.jni_calls);
        }

        std::lock_guard<std::mutex> lock(rt->threads_lock);
        rt->thread_retvals[id] = retval;
    });

    std::lock_guard<std::mutex> lock(rt->threads_lock);
    rt->threads[id] = std::move(th);
    return id;
}

std::uint32_t Pvz2Env::join_guest_thread_impl(GuestRuntime *rt, std::uint32_t id) {
    std::thread th;
    {
        std::lock_guard<std::mutex> lock(rt->threads_lock);
        auto it = rt->threads.find(id);
        if (it == rt->threads.end()) return 0;
        th = std::move(it->second);
        rt->threads.erase(it);
    }
    if (th.joinable()) th.join();

    std::lock_guard<std::mutex> lock(rt->threads_lock);
    auto it = rt->thread_retvals.find(id);
    return it == rt->thread_retvals.end() ? 0 : it->second;
}

/* Wake every guest sync object so threads parked in sem_wait / cond_wait can
 * see shutting_down and return. Audio shutdown already unblocks the OpenSL
 * completion wait; this covers the rest of the pthread table. */
static void wake_guest_sync_for_shutdown(GuestRuntime *rt) {
    rt->shutting_down.store(true, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lock(rt->sems_lock);
        for (auto &kv : rt->guest_sems) {
            GuestSem *gs = kv.second.get();
            {
                std::lock_guard<std::mutex> lk(gs->m);
                /* Ensure the wait predicate can succeed even if a waiter races
                 * the flag; count is harmless once the process is exiting. */
                if (gs->count < 1) gs->count = 1;
            }
            gs->cv.notify_all();
        }
    }
    {
        std::lock_guard<std::mutex> lock(rt->conds_lock);
        for (auto &kv : rt->guest_conds) {
            GuestCond *gc = kv.second.get();
            {
                std::lock_guard<std::mutex> lk(gc->m);
                gc->generation++;
            }
            gc->cv.notify_all();
        }
    }
    {
        std::lock_guard<std::mutex> lock(rt->mutexes_lock);
        for (auto &kv : rt->guest_mutexes) {
            /* Unblock anyone stuck in GuestMutex::lock after a stuck report. */
            kv.second->cv.notify_all();
        }
    }
}

bool join_leftover_guest_threads(GuestRuntime *rt) {
    using namespace std::chrono_literals;
    wake_guest_sync_for_shutdown(rt);

    std::vector<std::pair<std::uint32_t, std::thread>> leftover;
    {
        std::lock_guard<std::mutex> lock(rt->threads_lock);
        leftover.reserve(rt->threads.size());
        for (auto &kv : rt->threads) {
            leftover.emplace_back(kv.first, std::move(kv.second));
        }
        rt->threads.clear();
    }

    /* Guest workers often sit forever in sem_wait for audio/bank work that
     * never comes after audio::shutdown(). Give them a short window to unwind
     * after the wake above; anything still alive is detached and the caller
     * must not free img/rt (use _Exit). */
    constexpr auto kJoinDeadline = 2s;
    const auto deadline = std::chrono::steady_clock::now() + kJoinDeadline;
    bool all_joined = true;

    for (auto &entry : leftover) {
        const std::uint32_t id = entry.first;
        std::thread &th = entry.second;
        if (!th.joinable()) continue;

        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> lock(rt->threads_lock);
                if (rt->thread_retvals.find(id) != rt->thread_retvals.end()) break;
            }
            /* Re-poke sync in case a thread re-entered wait after a spurious
             * wake before seeing shutting_down. */
            wake_guest_sync_for_shutdown(rt);
            std::this_thread::sleep_for(20ms);
        }

        bool finished = false;
        {
            std::lock_guard<std::mutex> lock(rt->threads_lock);
            finished = rt->thread_retvals.find(id) != rt->thread_retvals.end();
        }
        if (finished) {
            th.join();
        } else {
            {
                std::lock_guard<std::mutex> lg(rt->log_lock);
                pvz2_log("[shutdown] guest thread %u still blocked after "
                            "join deadline -- detaching", id);
            }
            th.detach();
            all_joined = false;
        }
    }
    return all_joined;
}

/* --- entering guest code ---------------------------------------------------- */

std::uint32_t make_fake_jstring(pvz2_elf_image_t *img, std::uint32_t addr, const char *utf8) {
    size_t len = std::strlen(utf8) + 1;
    std::memcpy(&img->mem[addr], utf8, len);
    return addr;
}

/* The C++ static/global constructors Android's linker calls during dlopen(),
 * before JNI_OnLoad or anything else touches the .so. pvz2_elf_load() only
 * loads and relocates; it never executes guest code, so skipping this left
 * every non-trivial global C++ object as raw zeroed memory instead of properly
 * constructed -- root cause of the AndroidAppEvent list in onDrawFrame spinning
 * forever copying from a never-self-initialised sentinel. Called with
 * argc=argv=envp=0 like a minimal libc startup would. */
namespace {

/* One constructor entry, called the way a minimal libc startup would. */
void run_one_ctor(pvz2_elf_image_t *img, GuestRuntime *rt, std::uint32_t entry_addr,
                  const char *what, const char *module, std::uint32_t i) {
    GuestThreadCtx &ctx = main_ctx(img, rt);
    GuestCallSetup setup;
    setup.r0 = 0; /* argc */
    setup.r1 = 0; /* argv */
    setup.r2 = 0; /* envp */
    setup.sp = kStackTop;
    setup.lr = img->trampoline_base; /* LR -> "$halt" sentinel */
    setup.entry_pc = entry_addr;
    apply_call_setup(ctx.jit, ctx.env, img, setup);

    Dynarmic::HaltReason hr = run_jit_sliced(ctx.jit, ctx.env);
    if (verbose_boot()) {
        pvz2_log("[runtime] %s %s[%u] (0x%08x) returned, halt_reason=0x%08x, import_calls=%u",
                    module, what, i, entry_addr, static_cast<unsigned>(hr), ctx.env.import_calls);
    }
}

}  // namespace

void run_init_array(pvz2_elf_image_t *img, GuestRuntime *rt) {
    /* Every mapped module, in dependency order: libc++_shared.so's single
     * constructor has to have run before libPVZ2.so's 1044 do, or the engine's
     * first use of a libc++ global (a locale, an iostream) touches uninitialised
     * memory. init_order is the loader's post-order over DT_NEEDED; for a
     * single-module image it is just {0}, which is the old behaviour exactly.
     *
     * DT_INIT runs before DT_INIT_ARRAY within a module, as the ELF spec
     * requires -- libPVZ2 has none, but the NDK's libc++_shared does. */
    std::uint32_t total = 0;
    for (std::uint32_t k = 0; k < img->module_count; ++k) {
        total += img->modules[k].init_array_count;
    }
    if (total == 0 && img->module_count <= 1) {
        pvz2_log("[runtime] no .init_array entries, skipping");
        return;
    }
    pvz2_log("[runtime] ---- running %u constructor(s) across %u module(s) ----", total,
                img->module_count);

    std::uint32_t done = 0;
    for (std::uint32_t k = 0; k < img->module_count; ++k) {
        const pvz2_elf_module_t &m = img->modules[img->init_order[k]];

        if (m.init_vaddr != 0) {
            run_one_ctor(img, rt, m.base + m.init_vaddr, "DT_INIT", m.name, 0);
        }
        for (std::uint32_t i = 0; i < m.init_array_count; ++i) {
            std::uint32_t entry_addr = 0;
            std::memcpy(&entry_addr, &img->mem[m.base + m.init_array_vaddr + i * 4], 4);
            ++done;
            if (entry_addr == 0 || entry_addr == 0xFFFFFFFFu) continue; /* some toolchains pad with -1 */
            set_status("booting: static constructors %u/%u", done, total);
            run_one_ctor(img, rt, entry_addr, ".init_array", m.name, i);
        }
    }
}

/* JNI_OnLoad(JavaVM*, void* reserved) -- a real exported symbol, but with a
 * DIFFERENT calling convention than every Java_* export (r0 is a JavaVM*, not a
 * JNIEnv*; there is no jclass in r1). Android calls this automatically right
 * after System.loadLibrary(), before any other native call. Skipping it left
 * the JavaVM and method-ID cache it allocates NULL for the whole process,
 * silently breaking every later call that re-derives a JNIEnv* via
 * JavaVM->GetEnv() instead of using the one handed to it.
 *
 * Run for EVERY module that exports one, not just libPVZ2.so. JNI_OnLoad is a
 * per-library entry point: each .so keeps its OWN JavaVM* in its OWN global, and
 * Android calls each library's as that library is loaded. Running only the main
 * image's is the same bug as skipping it entirely, just confined to the
 * dependencies -- and it cost a long hunt: libNimble.so's
 * EA::Nimble::getEnv() calls (*vm)->GetEnv(vm, &env, JNI_VERSION_1_6) with a vm
 * that only ITS JNI_OnLoad ever assigns, so the guest branched to 0 and halted
 * ~230 words deep inside applicationWillFinishLaunching. That aborted the
 * framework's main before its tail, which is the ONLY code that publishes the
 * AndroidAppDriver -- so the whole boot then ran with a NULL driver global and
 * drew nothing, several layers away from the cause.
 *
 * In init_order, i.e. dependency before dependent -- the same order the
 * constructors ran in, and the order the Java side would load them. */
void run_jni_onload(pvz2_elf_image_t *img, GuestRuntime *rt) {
    unsigned ran = 0;
    for (std::uint32_t i = 0; i < img->module_count; ++i) {
        const pvz2_elf_module_t &m = img->modules[img->init_order[i]];
        const std::uint32_t entry_addr = pvz2_elf_find_symbol_in(&m, "JNI_OnLoad");
        if (entry_addr == 0) continue; /* most libraries have none -- normal */

        pvz2_log("[runtime] ---- running JNI_OnLoad [%s] at 0x%08x ----", m.name, entry_addr);

        GuestThreadCtx &ctx = main_ctx(img, rt);
        GuestCallSetup setup;
        setup.r0 = dex::kJavaVmPtrAddr; /* JavaVM* vm */
        setup.r1 = 0;                   /* void* reserved */
        setup.sp = kStackTop;
        setup.lr = img->trampoline_base;
        setup.entry_pc = entry_addr;
        apply_call_setup(ctx.jit, ctx.env, img, setup);

        Dynarmic::HaltReason hr = run_jit_sliced(ctx.jit, ctx.env);
        const std::uint32_t ret = ctx.jit.Regs()[0];
        pvz2_log("[runtime] JNI_OnLoad [%s] returned 0x%08x, halt_reason=0x%08x, import_calls=%u, "
                    "jni_calls=%u, ticks_used=%llu",
                    m.name, ret, static_cast<unsigned>(hr), ctx.env.import_calls,
                    ctx.env.jni_calls, (unsigned long long)ctx.env.ticks_used);

        /* It returns the JNI version it wants, or JNI_ERR. A library that
         * refuses here has NOT set up its globals, and every later call into it
         * dereferences them -- so say it now rather than let it surface as a
         * null branch somewhere unrelated. */
        if ((std::int32_t)ret < 0) {
            pvz2_log("[!] %s's JNI_OnLoad failed (JNI_ERR) -- its JavaVM and any cached "
                        "classes are unset; calls into it will branch through NULL", m.name);
        }
        ++ran;
    }
    if (ran == 0) pvz2_log("[runtime] symbol 'JNI_OnLoad' not found in any module, skipping");
}

void run_export(pvz2_elf_image_t *img, GuestRuntime *rt, const char *entry_name,
                std::uint32_t arg2, std::uint32_t arg3) {
    std::uint32_t entry_addr = pvz2_elf_find_symbol(img, entry_name);
    if (entry_addr == 0) {
        pvz2_log("[runtime] symbol '%s' not found, skipping", entry_name);
        return;
    }
    pvz2_log("[runtime] ---- running %s at 0x%08x ----", entry_name, entry_addr);

    /* Synchronous top-level calls never overlap, so they share one Jit -- and
     * with it every block it has already translated. */
    GuestThreadCtx &ctx = main_ctx(img, rt);
    GuestCallSetup setup;
    setup.r0 = dex::kJniEnvPtrAddr; /* JNIEnv* env */
    setup.r1 = 0x00000200;          /* jclass clazz (dummy placeholder) */
    setup.r2 = arg2;
    setup.r3 = arg3;
    setup.sp = kStackTop;
    setup.lr = img->trampoline_base; /* LR -> "$halt" sentinel (trampoline #0) */
    setup.entry_pc = entry_addr;
    apply_call_setup(ctx.jit, ctx.env, img, setup); /* ARM mode, supervisor, IRQ/FIQ masked */

    auto t0 = std::chrono::steady_clock::now();
    Dynarmic::HaltReason hr = run_jit_sliced(ctx.jit, ctx.env);
    pvz2_log("[runtime] %s returned, halt_reason=0x%08x, import_calls=%u, jni_calls=%u, "
                "ticks_used=%llu, ms=%.1f",
                entry_name, static_cast<unsigned>(hr), ctx.env.import_calls, ctx.env.jni_calls,
                (unsigned long long)ctx.env.ticks_used, ms_since(t0));
}

void run_guest_call(pvz2_elf_image_t *img, GuestRuntime *rt, const char *label,
                    std::uint32_t offset, std::uint32_t r0,
                    const std::vector<std::uint32_t> &extra_args, bool per_frame) {
    std::uint32_t entry_addr = img->so_base + offset;
    /* verbose adds boot banners, not a banner per frame: that was 5 lines a frame
     * and buried the [java]/[engine]/[state] timeline (18 MB in five minutes). */
    const bool chatty = !per_frame;
    if (chatty) {
        pvz2_log("[runtime] ---- running %s at 0x%08x (offset 0x%x) ----", label, entry_addr,
                    offset);
        /* Only the non-per-frame calls (the boot phases) update the window-title
         * status; a per-frame onDrawFrame doing it churned a mutex and a
         * vsnprintf every frame for a string nothing shows once booted -- the
         * title is FPS by then. */
        set_status("%s", label);
    }

    GuestThreadCtx &ctx = main_ctx(img, rt);
    ctx.env.quiet_call = per_frame;
    GuestCallSetup setup;
    setup.r0 = r0;
    if (extra_args.size() > 0) setup.r1 = extra_args[0];
    if (extra_args.size() > 1) setup.r2 = extra_args[1];
    if (extra_args.size() > 2) setup.r3 = extra_args[2];
    if (extra_args.size() > 3) setup.stack_args.assign(extra_args.begin() + 3, extra_args.end());
    setup.sp = kStackTop;
    setup.lr = img->trampoline_base;
    setup.entry_pc = entry_addr;
    apply_call_setup(ctx.jit, ctx.env, img, setup);

    auto t0 = std::chrono::steady_clock::now();
    Dynarmic::HaltReason hr = run_jit_sliced(ctx.jit, ctx.env);
    const double ms = ms_since(t0);
    g_last_call_stats = {ctx.env.import_calls, ctx.env.jni_calls, ctx.env.ticks_used, ms,
                         ctx.jit.Regs()[0]};

    /* Did it actually return? A guest function that runs to its own `BX lr`
     * pops its frame first, so SP comes back to exactly the value set up here.
     * Anything else reached the halt sentinel by another route -- the classic
     * one being a branch into unmapped or uninitialised memory that slid into
     * the trampoline table (see MemoryReadCode) -- and the "returned" line
     * below would otherwise report a half-executed function as a success.
     * Said regardless of per_frame -- a frame that stops early is the single
     * most expensive thing to misread in this emulator -- but only ONCE per
     * label: onDrawFrame failing this way fails it every frame, and the flood
     * buries the boot-time occurrence that caused it. */
    if (ctx.jit.Regs()[13] != setup.sp) {
        /* Keyed on the ENTRY OFFSET, not on `label`: the per-frame callers
         * build labels like "Native_onDrawFrame[3076]", so a label-keyed set
         * never matches twice and dedupes nothing. */
        static std::set<std::uint32_t> seen;
        if (seen.insert(offset).second) {
            pvz2_log("[!] %s did NOT return -- it halted with sp=0x%08x, expected 0x%08x "
                        "(%u words deep). Any 'returned' line for it is a return that did not "
                        "happen; everything after the halt point never ran. (reported once)",
                        label, ctx.jit.Regs()[13], setup.sp,
                        (unsigned)((setup.sp - ctx.jit.Regs()[13]) / 4));
        }
    }
    if (chatty) {
        pvz2_log("[runtime] %s returned, halt_reason=0x%08x, import_calls=%u, jni_calls=%u, "
                    "ticks_used=%llu, ms=%.1f",
                    label, static_cast<unsigned>(hr), ctx.env.import_calls, ctx.env.jni_calls,
                    (unsigned long long)ctx.env.ticks_used, ms);
    }
}

const CallStats &last_call_stats() { return g_last_call_stats; }

void run_at_offset(pvz2_elf_image_t *img, GuestRuntime *rt, const char *label,
                   std::uint32_t offset, const std::vector<std::uint32_t> &extra_args,
                   bool per_frame) {
    run_guest_call(img, rt, label, offset, dex::kJniEnvPtrAddr, extra_args, per_frame);
}

std::uint32_t call_guest_quiet_args(pvz2_elf_image_t *img, GuestRuntime *rt, std::uint32_t offset,
                                    const std::uint32_t *args, int nargs) {
    GuestThreadCtx &ctx = main_ctx(img, rt);
    ctx.env.quiet_call = true;
    GuestCallSetup setup;
    if (nargs > 0) setup.r0 = args[0];
    if (nargs > 1) setup.r1 = args[1];
    if (nargs > 2) setup.r2 = args[2];
    if (nargs > 3) setup.r3 = args[3];
    for (int i = 4; i < nargs; ++i) setup.stack_args.push_back(args[i]);
    setup.sp = kStackTop;
    setup.lr = img->trampoline_base; /* LR -> "$halt" sentinel */
    setup.entry_pc = img->so_base + offset;
    apply_call_setup(ctx.jit, ctx.env, img, setup);
    run_nested(ctx.jit, ctx.env);
    return ctx.jit.Regs()[0];
}

/* The three-register form, which is the shape most callers want. It was a second
 * copy of the body above until the only difference left was how the arguments
 * arrive. */
std::uint32_t call_guest_quiet(pvz2_elf_image_t *img, GuestRuntime *rt, std::uint32_t offset,
                               std::uint32_t r0, std::uint32_t r1, std::uint32_t r2) {
    const std::uint32_t args[3] = {r0, r1, r2};
    return call_guest_quiet_args(img, rt, offset, args, 3);
}

}  // namespace runtime

/* --- the two hooks src/dependencies/ reaches back through -------------------- */

/* See dependency.h. Mints the SVC stub and binds its handler in one step, so a
 * caller building an OpenSL vtable gets back an address it can store straight
 * into guest memory. Startup only -- the handler table is never resized once
 * guest threads are indexing it. */
std::uint32_t make_guest_callback(const char *name, ImportHandler fn) {
    pvz2_elf_image_t *img = runtime::g_image;
    if (img == nullptr || fn == nullptr) return 0;
    const std::uint32_t addr = pvz2_elf_add_trampoline(img, name);
    if (addr == 0) return 0;
    const std::uint32_t idx = (addr - img->trampoline_base) / 4;
    if (idx >= runtime::g_import_handlers.size()) return 0;
    runtime::g_import_handlers[idx] = fn;
    return addr;
}

/* See dependency.h. call_guest_quiet() takes an offset and re-adds so_base, so
 * an absolute address goes in as (fn - so_base). */
std::uint32_t call_guest_between_frames(std::uint32_t fn, std::uint32_t a0, std::uint32_t a1) {
    pvz2_elf_image_t *img = runtime::g_image;
    GuestRuntime *rt = runtime::g_runtime;
    if (img == nullptr || rt == nullptr || fn < img->so_base) return 0;
    return runtime::call_guest_quiet(img, rt, fn - img->so_base, a0, a1);
}

std::uint32_t call_guest_between_frames_n(std::uint32_t fn, const std::uint32_t *args, int nargs) {
    pvz2_elf_image_t *img = runtime::g_image;
    GuestRuntime *rt = runtime::g_runtime;
    if (img == nullptr || rt == nullptr || fn < img->so_base) return 0;
    return runtime::call_guest_quiet_args(img, rt, fn - img->so_base, args, nargs);
}

}  // namespace pvz2native
