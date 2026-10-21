#ifndef PVZ2NATIVE_RUNTIME_DYNARMIC_CONFIG_H
#define PVZ2NATIVE_RUNTIME_DYNARMIC_CONFIG_H

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <vector>

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>

#include <pvz2native/dependencies/dependency.h>
#include <pvz2native/elf32/elf32_loader.h>
#include <pvz2native/pvz2_session.h>
#include <pvz2native/runtime/guest_runtime.h>

/* The emulator itself: the dynarmic configuration, the per-guest-thread JIT
 * contexts, and the primitives that enter guest code.
 *
 * Deliberately free of every address into libPVZ2.so. This layer implements
 * Android -- a flat address space, SVC-dispatched imports, threads, an ARM
 * calling convention -- none of which changes between two releases of the game.
 * Everything that does change lives in game/symbols.h, and everything that
 * knows the game's boot ORDER lives in engine/. Keeping those three apart is
 * what makes a second version an additive change.
 */

namespace pvz2native {
namespace runtime {

/* --- guest address-space map ------------------------------------------------
 *
 * The heap used to be 16MB, and that is what kept the screen black. Loading the
 * boot resource groups exhausts it: `operator new` of a 266KB block returned 0,
 * the guest threw std::bad_alloc, the unwinder walked the stack to the bottom
 * without finding a handler (our call frames have none -- LR is the halt
 * sentinel) and called abort(). Every symptom above it -- no state machine, no
 * splash, nothing drawn -- was downstream of that one failed allocation.
 *
 * calloc() commits the range but the OS only backs pages that are actually
 * touched, so a generous reservation costs nothing until the game uses it. */
constexpr std::uint32_t kThreadStackBase = 0x15000000;
constexpr std::uint32_t kThreadStackSize = 0x00100000; /* 1MB each, 8 slots -> 0x15800000 */

/* The main stack grows down from here into the 8MB gap above the thread
 * stacks; kAddressSpaceSize must exceed it. */
constexpr std::uint32_t kStackTop = 0x16000000;
constexpr std::uint32_t kAddressSpaceSize = 0x18000000; /* 384MB */

/* The surface size lives in <pvz2native/surface.h>, not here. This layer used to
 * keep its own copy (window_width/window_height/set_window_size) alongside two
 * others that always held the same number -- see that header. */

/* Where the guest heap starts, and how big it is.
 *
 * Derived from the image rather than fixed, because "the .so ends below
 * 0x01000000" was only ever true of 1.6. It loads at 0x00100000 and spans
 * ~14.6MB, ending at 0x00EF9700 -- just under the old hardcoded base, by 700KB.
 * 4.5.2 spans 18.7MB and ends at 0x012D53F4, so a fixed 0x01000000 base put the
 * heap **three megabytes inside the game's own code**. The first allocation
 * overwrote instructions, and executing them aborted the process in dynarmic's
 * translator with `assertion failed: reg != A32::Reg::PC` -- garbage decoding as
 * an instruction that writes r15. Nothing in that message points at the heap,
 * which is why it is computed here now and logged. */
struct HeapLayout {
    std::uint32_t base;
    std::uint32_t size;
};
HeapLayout heap_layout_for(const pvz2_elf_image_t *img);

/* --- execution limits -------------------------------------------------------
 *
 * The tick budget exists only to stop a runaway loop from hanging the window; it
 * is not a statement about how much work a legitimate call may do, and it has
 * been mistaken for one more than once.
 *
 * applicationWillFinishLaunching sweeps the full BMP (0x0000-0xFFFF) once to
 * build a font glyph-metrics table -- a real, finite, 65536-iteration loop --
 * which alone broke the original 20000 import-call cap. ResourceManager::Init
 * then broke the 500000 one: it initialises the resource tables field by field,
 * and the compiler emits an __aeabi_memset for every 1-to-4-byte field, so a few
 * MB of structures costs hundreds of thousands of import calls. Each is a JIT
 * exit, which is why this is slow rather than wrong. That history is why there
 * is no longer a cap on import calls at all: every ceiling tried was a false
 * positive on real work, and handle_import_call now only counts them for
 * diagnostics. */
constexpr std::uint64_t kTickBudget = 20'000'000'000;

/* Per-call tracing is what makes the boot log readable, but at ~80 bytes a line
 * it cannot survive tens of millions of calls. Trace the part of the boot a
 * human actually reads, then go quiet and keep running. */
constexpr std::uint32_t kTraceCallLimit = 300000;

/* GetTicksRemaining() normally hands dynarmic the *entire* remaining budget in
 * one go, so a tight guest loop with no SVC/host calls can run start to finish
 * inside a single jit.Run() without ever returning control to us -- AddTicks()
 * then fires once at the very end with the full count, useless for seeing
 * whether a stuck loop is spinning at one PC or slowly progressing. Capping the
 * slice forces Run() to return every ~1M ticks so the host loop can sample the
 * PC in between. */
constexpr std::uint64_t kPcSampleSlice = 1'000'000;

/* --- diagnostics the JIT itself has to serve --------------------------------
 *
 * These two live here, rather than in diagnostics/, because they can only be
 * implemented from inside the memory callbacks and the Run() loop. What to
 * point them AT is a caller's decision. */

/* Guest memory watchpoint: every write landing in [lo, hi) is logged with the
 * PC and LR that produced it. Needed because some fields are written through a
 * base register that is not the object pointer (e.g. `r4 = app+0x400` then
 * `STR r0,[r4,#0x15C]`), which no scan for a literal offset can find. */
void watch_arm(std::uint32_t lo, std::uint32_t hi, std::uint32_t budget);
void watch_disarm();

/* Ticks per Run() slice while non-zero, overriding kPcSampleSlice. Forcing a
 * small slice makes the PC sampler emit a sample every few instructions, which
 * is how you find where a short call bails out -- a steady-state frame costs
 * ~1363 ticks against a 1,000,000-tick slice, so otherwise there is exactly one
 * slice boundary, at the entry PC.
 *
 * Reaches only the main context: it works through GetTicksRemaining(), and
 * worker and nested contexts no longer count cycles (see GuestThreadCtx), so
 * they run a callback or a whole thread in one uninterrupted slice. */
void set_slice_override(std::uint32_t ticks);

/* --- the JIT ---------------------------------------------------------------- */

class Pvz2Env final : public Dynarmic::A32::UserCallbacks {
public:
    pvz2_elf_image_t *img = nullptr;
    GuestRuntime *rt = nullptr;
    Dynarmic::A32::Jit *jit = nullptr;
    std::uint64_t ticks_used = 0;
    std::uint32_t import_calls = 0;
    std::uint32_t jni_calls = 0;
    bool halted_by_guest_return = false;
    bool quiet_call = false; /* per-frame call: no halt-sentinel banner even under verbose */
    bool exited_early = false; /* set by pthread_exit */
    std::uint32_t exit_retval = 0;

    /* Set alongside every jit->HaltExecution() call in this class. Needed
     * because run_jit_sliced() drives Run() in a host-side loop (see
     * kPcSampleSlice) -- HaltExecution() only stops the CURRENT Run() call, it
     * doesn't by itself tell that outer loop to stop re-entering. Without this,
     * hitting any halt condition OTHER than the guest-return sentinel,
     * pthread_exit, or the tick budget (the only 3 the loop used to check) made
     * it immediately call Run() again, re-trigger the same halt at the same PC,
     * and spam forever -- exactly what happened the first time this shipped. */
    bool should_halt = false;

    /* Instruction-level sampling window, as absolute guest addresses. While PC
     * is inside it, GetTicksRemaining() shrinks the slice to a handful of ticks
     * so run_jit_sliced() can log LR/r0/r1/r4 on nearly every pass -- distinct
     * LR values mean multiple call sites, one repeating LR confirms a tight
     * loop. Capped by hot_hits so a genuinely non-terminating loop does not
     * grind the run to a halt on slice overhead once there are enough samples. */
    std::uint32_t hot_lo = 0, hot_hi = 0;
    std::uint32_t hot_hits = 0;
    static constexpr std::uint32_t kHotHitCap = 300;
    std::set<std::uint32_t> hot_seen_lrs;

    bool in_bounds(std::uint32_t vaddr, std::uint32_t size) const {
        return vaddr < img->mem_size && (std::uint64_t)vaddr + size <= img->mem_size;
    }

    /* Clears the per-call bookkeeping so one long-lived Env (and the Jit bound
     * to it) can serve call after call -- see GuestThreadCtx. Everything reset
     * here describes a single guest call; img/rt/jit describe the thread and
     * must survive. */
    void begin_call();

    std::uint8_t MemoryRead8(std::uint32_t vaddr) override;
    std::uint16_t MemoryRead16(std::uint32_t vaddr) override;
    std::uint32_t MemoryRead32(std::uint32_t vaddr) override;
    std::uint64_t MemoryRead64(std::uint32_t vaddr) override;
    void MemoryWrite8(std::uint32_t vaddr, std::uint8_t value) override;
    void MemoryWrite16(std::uint32_t vaddr, std::uint16_t value) override;
    void MemoryWrite32(std::uint32_t vaddr, std::uint32_t value) override;
    void MemoryWrite64(std::uint32_t vaddr, std::uint64_t value) override;

    /* UserCallbacks::MemoryWriteExclusiveN defaults to `return false` when left
     * unoverridden, meaning every STREX in the guest binary was unconditionally
     * failing regardless of contention. The global monitor has already verified
     * the LDREX/STREX reservation by the time we are called, so we just perform
     * the write and confirm it -- the same pattern Citra and yuzu use. This was
     * root-causing what looked like an infinite construction loop but was
     * actually ANY refcounted object's atomic release spinning forever. */
    bool MemoryWriteExclusive8(std::uint32_t vaddr, std::uint8_t value, std::uint8_t) override;
    bool MemoryWriteExclusive16(std::uint32_t vaddr, std::uint16_t value, std::uint16_t) override;
    bool MemoryWriteExclusive32(std::uint32_t vaddr, std::uint32_t value, std::uint32_t) override;
    bool MemoryWriteExclusive64(std::uint32_t vaddr, std::uint64_t value, std::uint64_t) override;

    /* "Will a read from here always give the same answer?" Asked once per
     * translated instruction, never at run time, and answering it truthfully
     * for the game's R+X segment costs nothing and pays well.
     *
     * dynarmic's ConstProp pass (already on) folds any memory read whose
     * address is a compile-time constant into the value itself when this says
     * yes. On ARM32 that is not a corner case: the ISA has no 32-bit immediate,
     * so every constant, every string address and every function pointer is
     * fetched with `LDR Rd, [PC, #imm]` from a literal pool sitting in .text --
     * and the translator knows PC, so those reads arrive here with a literal
     * address. Each one turns from a page-table lookup into a `mov reg, imm32`,
     * which then feeds further constant propagation downstream.
     *
     * Only img->text_vaddr's range qualifies, and the exclusions matter: the
     * R+W segment holds .data.rel.ro and the GOT, which the loader rewrites
     * during relocation. The one thing that DOES write into this range is
     * game/patches.cpp, and it is safe because it runs before the first Jit
     * exists -- no block can have been translated yet. Anything that patched
     * guest code later would have to invalidate the JIT's cache anyway. */
    bool IsReadOnlyMemory(std::uint32_t vaddr) override;

    /* Instruction fetch, overridden for ONE reason: to make a call through a
     * null function pointer fail loudly instead of looking like a success.
     *
     * Guest low memory is zeroed, and a zero word decodes as `ANDEQ r0,r0,r0`
     * -- a NOP. So `BLX r3` with r3 == 0 does not fault: it runs from address 0
     * through ~1024 NOPs until it reaches the trampoline table at 0x1000, whose
     * index 0 is the "$halt" sentinel, and CallSVC(0) then reports the call as
     * having RETURNED NORMALLY. The whole remainder of the guest function is
     * skipped and every log line says success.
     *
     * That is not a hypothetical: it is how a dead vtable dispatch inside the
     * 9.6.1 app constructor hid for an entire debugging session -- a
     * `Native_applicationWillFinishLaunching returned, halt_reason=0x01000000`
     * that had in fact died somewhere in the middle, leaving the app driver
     * unpublished and every later frame drawing nothing. The fixed ~1052 ticks
     * of an "empty" onDrawFrame are the same slide, counted.
     *
     * Nothing legitimate executes below trampoline_base (0x1000): the SVC stubs
     * start there, the fake JNIEnv is at 0x7000 and the guest stack at
     * kStackTop. So refusing to fetch code from that page costs nothing and
     * turns the silent slide into a NoExecuteFault carrying the faulting PC and
     * the LR of the call site. Returning nullopt rather than an undefined
     * instruction keeps the distinction: this address holds no code at all.
     *
     * Only code fetch is refused -- the data callbacks still treat the page as
     * ordinary zeroed memory, because reading through a null pointer is common
     * in guest code that then checks the result, and turning those into faults
     * would break booting. */
    std::optional<std::uint32_t> MemoryReadCode(std::uint32_t vaddr) override;

    void InterpreterFallback(std::uint32_t pc, size_t num_instructions) override;
    void CallSVC(std::uint32_t swi) override;
    void ExceptionRaised(std::uint32_t pc, Dynarmic::A32::Exception exception) override;
    void AddTicks(std::uint64_t ticks) override;
    std::uint64_t GetTicksRemaining() override;

    /* --- the bridge to src/dependencies/ and src/dex/ ---------------------
     *
     * Both layers are written against GuestCall and know nothing about
     * dynarmic. This packages the live register file plus the handful of
     * operations that genuinely need the executing JIT -- halting it, spawning
     * another, calling back into guest code -- into the object they receive.
     * The hooks are static so they can be taken as plain function pointers. */
    GuestCall make_guest_call();

    static void hook_halt(void *env, const char *why);
    static void hook_note_blocked(void *env);
    static std::uint32_t hook_spawn_thread(void *env, std::uint32_t start, std::uint32_t arg);
    static std::uint32_t hook_join_thread(void *env, std::uint32_t id);
    static std::uint32_t hook_call_guest(void *env, std::uint32_t fn, const std::uint32_t *args,
                                         int nargs);
    /* GuestCall::on_alloc_fn is deliberately left unset. The hook exists (the
     * malloc shim calls it on every allocation) and is the way to arm a
     * watchpoint on an object at the moment it is born, which is the only way
     * to catch a field written during construction. Nothing needs it right now,
     * and a hook installed for nothing costs every malloc. */

    static std::uint32_t spawn_guest_thread_impl(GuestRuntime *rt, std::uint32_t start_routine,
                                                 std::uint32_t arg);
    static std::uint32_t join_guest_thread_impl(GuestRuntime *rt, std::uint32_t id);

private:
    void note_watch(std::uint32_t vaddr, std::uint32_t size, std::uint32_t value);
    void handle_import_call(std::uint32_t idx);

    /* Resumes the caller after a shim has run, with BX semantics rather than a
     * plain `pc = lr`.
     *
     * The low bit of LR says which instruction set to return to, so a Thumb
     * function that called an import has to be resumed in Thumb mode. Ignoring
     * it resumes Thumb code as ARM, which is the same failure as entering a
     * Thumb function in ARM mode -- see apply_call_setup. Free for ARM callers:
     * we are executing the ARM trampoline, so the T flag is already clear and
     * only the rare Thumb case touches CPSR. */
    void return_to_caller();
};

/* One Jit per guest thread, alive for the whole session.
 *
 * A Dynarmic::A32::Jit owns a 128MB code cache and every block it translated
 * dies with it. Building one per guest call -- which every call site used to do
 * -- meant each boot frame re-translated the entire onDrawFrame call graph from
 * ARM to x64 before executing a single instruction of it. Reusing the Jit is
 * what turns the boot from minutes of retranslation into the guest actually
 * running.
 *
 * The Env has to live exactly as long: UserConfig::callbacks is captured at
 * construction and cannot be repointed, so per-call state is cleared with
 * begin_call() instead of by rebuilding the object. Declaration order matters
 * -- `env` must be constructed before `jit` takes its address.
 *
 * `count_cycles` chooses whether this context pays for tick accounting, and it
 * is not a small bill. With it on, dynarmic wraps EVERY SVC in two extra
 * devirtualized host calls (AddTicks before, GetTicksRemaining after) plus a
 * pair of MXCSR switches -- so one guest import costs three host calls instead
 * of one, on a boot that issues hundreds of millions of them -- and decrements
 * a counter at the end of every basic block.
 *
 * Only the main context needs any of that. It is the one whose Run() has to
 * return periodically so the window keeps pumping messages (see
 * run_jit_sliced), the one the PC sampler and hot-window diagnostics read, and
 * the only one the runaway tick budget still applies to. Spawned threads and
 * nested callbacks run to completion inside run_nested() without pumping
 * anything, and AddTicks() already refuses to enforce the budget on a spawned
 * thread, so for them the accounting was pure overhead.
 *
 * What is given up: a nested callback that loops forever now hangs instead of
 * being halted by the budget. That guard was a debugging convenience -- the
 * same loop hangs the game on a real device -- and flipping this argument back
 * to true restores it. */
struct GuestThreadCtx {
    Pvz2Env env;
    Dynarmic::A32::Jit jit;

    GuestThreadCtx(pvz2_elf_image_t *img, GuestRuntime *rt, size_t processor_id,
                   bool count_cycles = true);
};

/* The synchronous top-level calls (.init_array, JNI_OnLoad, every registered
 * native, every frame) all run on the window's thread, one at a time, so they
 * share a single context, created on first use. */
GuestThreadCtx &main_ctx(pvz2_elf_image_t *img, GuestRuntime *rt);
void release_main_ctx();

/* r0-r3 plus any overflow args spilled onto the guest stack past r3 (the same
 * layout AAPCS uses for a call with more than 4 words of arguments), sp/pc/lr/
 * cpsr, and the optional [hot_lo, hot_hi) sampling window. */
struct GuestCallSetup {
    std::uint32_t r0 = 0, r1 = 0, r2 = 0, r3 = 0;
    std::vector<std::uint32_t> stack_args;
    std::uint32_t sp = 0;
    std::uint32_t entry_pc = 0;
    std::uint32_t lr = 0;
    std::uint32_t cpsr = 0x000001d0;
    std::uint32_t hot_lo = 0, hot_hi = 0;
};

void apply_call_setup(Dynarmic::A32::Jit &jit, Pvz2Env &env, pvz2_elf_image_t *img,
                      const GuestCallSetup &setup);

/* Runs a call to completion.
 *
 * A single jit.Run() is NOT a whole call: GetTicksRemaining() hands out at most
 * kPcSampleSlice ticks, so Run() returns on a spent slice just as readily as on
 * the halt sentinel. Taking r0 after one Run() therefore read a half-finished
 * callback's registers whenever it needed more than a slice -- a comparator
 * returning garbage mid-qsort, silently. */
void run_nested(Dynarmic::A32::Jit &jit, Pvz2Env &env);

/* Same, but pumping the host between slices and optionally sampling the PC --
 * the variant every top-level call uses. */
Dynarmic::HaltReason run_jit_sliced(Dynarmic::A32::Jit &jit, Pvz2Env &env);

/* --- session-wide setup ----------------------------------------------------- */

/* Must precede the first Jit: UserConfig captures the page table pointer at
 * construction, and the Jits are long-lived.
 *
 * Every entry holds img->mem itself rather than img->mem + page_base, which
 * pairs with UserConfig::absolute_offset_page_table: the JIT then indexes
 * page_table[vaddr >> 12][vaddr] instead of [vaddr >> 12][vaddr & 0xFFF],
 * dropping two instructions AND a scratch register from every emitted guest
 * load and store. The two must agree -- an entry of img->mem + page_base with
 * the flag on would read one page further for every page. */
void build_page_table(pvz2_elf_image_t *img);

/* Drops a page back onto the callback path so the MemoryWrite callbacks -- and
 * therefore the watchpoint -- see accesses to it again. Costs that one page its
 * inline access; everything else keeps it. */
void unmap_page_for_watch(std::uint32_t vaddr);

/* Binds every imported symbol to its handler in src/dependencies/. Sized to the
 * FULL trampoline capacity, not just the imports actually present:
 * make_guest_callback() hands out indices past trampoline_count at startup, and
 * the table is never resized afterwards (guest threads index it concurrently,
 * so growing it under them would be a data race on the buffer itself). */
void build_import_handler_cache(pvz2_elf_image_t *img);

/* Lets make_guest_callback() and call_guest_between_frames() reach the live
 * session. Cleared at shutdown so neither can outlive it. */
void set_session_image(pvz2_elf_image_t *img, GuestRuntime *rt);

/* Joins any guest thread that outlived the call which spawned it -- their
 * lambdas hold raw pointers to both img and rt, so this must happen before
 * either dies. Wakes guest sync primitives first. Returns false if some
 * threads were still alive after the join deadline (caller should not free
 * img/rt; typically _Exit instead). */
bool join_leftover_guest_threads(GuestRuntime *rt);

/* --- entering guest code ----------------------------------------------------
 *
 * Three shapes, and the difference between them has bitten this project twice:
 *   run_export      -- an exported Java_* symbol, looked up by name
 *   run_at_offset   -- a RegisterNatives-only native: r0 is the fake JNIEnv*
 *   run_guest_call  -- an ordinary C++ method: r0 is `this`
 * Calling a C++ method through run_at_offset puts the JNI environment in `this`
 * and branches into the JNI dispatch table within a few instructions. */
void run_init_array(pvz2_elf_image_t *img, GuestRuntime *rt);
void run_jni_onload(pvz2_elf_image_t *img, GuestRuntime *rt);
void run_export(pvz2_elf_image_t *img, GuestRuntime *rt, const char *entry_name,
                std::uint32_t arg2 = 0, std::uint32_t arg3 = 0);
/* `per_frame` suppresses the banner and result line unless [log] verbose is on.
 * There are thousands of those calls and ten of the others, whose timings ARE
 * the boot report. It is a parameter rather than a guess from the label, which
 * is what it used to be -- a strncmp against "Native_onDrawFrame" that silently
 * stopped covering the per-frame calls the moment a version added another. */
void run_guest_call(pvz2_elf_image_t *img, GuestRuntime *rt, const char *label,
                    std::uint32_t offset, std::uint32_t r0,
                    const std::vector<std::uint32_t> &extra_args, bool per_frame = false);
void run_at_offset(pvz2_elf_image_t *img, GuestRuntime *rt, const char *label,
                   std::uint32_t offset, const std::vector<std::uint32_t> &extra_args,
                   bool per_frame = false);

/* What the last top-level guest call actually cost.
 *
 * The one measurement that separates the two ways a frame loop can look alive
 * and produce nothing: a frame costing a handful of imports and ~0 ticks means
 * the engine is bailing out at the top of onDrawFrame and never reaching its
 * update; a frame costing thousands means it IS running and the problem is
 * downstream, in what it draws. Both look identical from outside -- the window
 * is black and the heartbeat keeps ticking -- and they have nothing in common
 * as bugs. */
struct CallStats {
    std::uint32_t import_calls = 0;
    std::uint32_t jni_calls = 0;
    std::uint64_t ticks = 0;
    double ms = 0.0;

    /* r0 as the call left it -- the return value, for the natives that HAVE one.
     * Most lifecycle natives are void and this is then whatever the body last
     * put in r0, so it is only meaningful where the JNI signature says so.
     *
     * Read it together with the "did NOT return" warning above: a call that
     * halted early leaves a plausible r0 that never came from a `return`
     * statement, which is the same lie that line exists to catch. */
    std::uint32_t result = 0;
};
const CallStats &last_call_stats();

/* Calls a guest function and returns r0, without the banner and status noise.
 *
 * Safe only from the host side BETWEEN top-level calls: it re-arms the shared
 * main-thread context, so calling it while that context is mid-call would
 * clobber the call in progress. */
std::uint32_t call_guest_quiet(pvz2_elf_image_t *img, GuestRuntime *rt, std::uint32_t offset,
                               std::uint32_t r0 = 0, std::uint32_t r1 = 0, std::uint32_t r2 = 0);

/* Same, but with an arbitrary argument list laid out AAPCS-style (r0-r3, then the
 * stack). For callbacks taking more than three words -- e.g. the purchase-driver
 * natives, one of which takes a jlong plus six jstrings and a jboolean. The
 * caller owns the layout: a jlong occupies an even/odd register pair, so it lands
 * in r2:r3 when it is the first argument after (JNIEnv*, jobject). */
std::uint32_t call_guest_quiet_args(pvz2_elf_image_t *img, GuestRuntime *rt, std::uint32_t offset,
                                    const std::uint32_t *args, int nargs);

/* Fabricates a fake jstring at a scratch address: a UTF-8 payload followed by a
 * NUL, so code that ends up dereferencing "the string's bytes" directly finds
 * something plausible instead of garbage. */
std::uint32_t make_fake_jstring(pvz2_elf_image_t *img, std::uint32_t addr, const char *utf8);

/* --- host-visible status ---------------------------------------------------- */

/* "What is the guest doing right now", surfaced in the window title. The boot
 * takes seconds with nothing on screen, and without this there is no way to
 * tell a working startup from a hang -- the window just sits there dark. */
void set_status(const char *fmt, ...);
void copy_status(char *out, size_t n);
void set_host_pump(pvz2_host_pump_fn fn);

/* [log] verbose=1 in config.ini restores the blow-by-blow boot log. Quiet by
 * default: the interesting output was buried under two lines per .init_array
 * entry (there are 619) and two more per frame (thousands). */
bool verbose_boot();

/* Wall-clock cost of one guest call. Ticks say how much guest work a call did;
 * only a clock says how much of the boot it actually spent, which is what
 * separates "the guest is doing a lot" from "the harness is being slow". */
double ms_since(std::chrono::steady_clock::time_point t0);

}  // namespace runtime
}  // namespace pvz2native

#endif
