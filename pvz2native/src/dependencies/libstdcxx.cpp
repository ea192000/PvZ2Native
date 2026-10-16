/* libstdc++.so / libgcc -- the ARM C++ runtime support the engine imports.
 *
 * Only the unwinder entry point needs real behaviour. libPVZ2.so is built with
 * exceptions, and when one is thrown the personality routine asks
 * __gnu_Unwind_Find_exidx for the ARM exception index table covering the
 * throwing PC. Returning 0 (the old silent stub) means "no unwind information
 * here", which turns every throw into an immediate terminate -- and since the
 * engine catches its own exceptions for things like missing resources, that
 * failure would look like an unexplained abort far from its cause.
 *
 * The table is a real segment of the loaded image: PT_ARM_EXIDX, which the ELF
 * loader now records (26962 entries in this build).
 */

#include <pvz2native/dependencies/dependency.h>
#include <pvz2native/log.h>

#include <cstdio>

namespace pvz2native {
namespace {

/* const uint32_t *__gnu_Unwind_Find_exidx(uint32_t pc, int *pcount)
 *
 * Returns the base of the exidx table covering `pc` and writes its entry count.
 * Entries are 8 bytes.
 *
 * The pc argument is load-bearing as soon as more than one module is mapped:
 * with libc++_shared.so present, a throw in the engine unwinds through libc++
 * frames and back, and each frame's personality routine asks about its OWN pc.
 * Answering with the main image's table for a libc++ pc finds no entry for that
 * address, which the unwinder reports as a missing handler -- i.e. terminate,
 * far from the throw. Older builds map one module and every pc lands in it, so
 * this reduces to exactly the previous behaviour. */
void unwind_find_exidx(GuestCall &c) {
    const std::uint32_t pc = c.arg(0);
    const std::uint32_t pcount = c.arg(1);

    const pvz2_elf_module_t *m = pvz2_elf_module_for_pc(c.img, pc);
    if (m == nullptr || m->exidx_size == 0) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            pvz2_log("[libstdc++] no PT_ARM_EXIDX covering pc=0x%08x (%s) -- "
                        "C++ throws from there cannot unwind",
                        pc, m ? m->name : "outside every module");
        }
        if (pcount != 0) c.write32(pcount, 0);
        c.set_result(0);
        return;
    }
    if (pcount != 0) c.write32(pcount, m->exidx_size / 8);
    c.set_result(m->base + m->exidx_vaddr);
}

/* int __cxa_thread_atexit_impl(void (*dtor)(void*), void *obj, void *dso_handle)
 *
 * Registers a destructor for a `thread_local` object with a non-trivial one --
 * libc++_shared.so emits a call to this for every such variable it defines, so
 * 9.6.1 reaches it where 1.6 and 4.5.2 never did.
 *
 * Accepted and dropped, matching what __cxa_atexit already does for static
 * destructors (libc_stdlib.cpp). Running them would mean re-entering guest code
 * from thread teardown, after that thread's Jit is gone -- and the process is
 * ending anyway. Returning non-zero would make libc++'s wrapper call
 * std::terminate on a perfectly healthy thread. */
void cxa_thread_atexit(GuestCall &c) { c.set_result(0); }

}  // namespace

void register_libstdcxx(ImportTable &t) {
    t.add("__cxa_thread_atexit_impl", cxa_thread_atexit);
    t.add("__gnu_Unwind_Find_exidx", unwind_find_exidx);
    /* The same function under bionic's own name -- identical contract
     * (pc, int *pcount) -> table base. 1.6 imports the __gnu_ spelling and
     * 4.5.2 this one, and without it every C++ `throw` in 4.5.2 would fail to
     * find an unwind table and terminate instead of reaching its handler. */
    t.add("dl_unwind_find_exidx", unwind_find_exidx);
}

}  // namespace pvz2native
