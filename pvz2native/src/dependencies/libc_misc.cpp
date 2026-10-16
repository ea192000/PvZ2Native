/* libc.so -- errno, non-local jumps, process introspection, the stack guard.
 * The leftovers that belong to no single header. */

#include <pvz2native/config.h>
#include <pvz2native/dependencies/dependency.h>

#include <cstring>
#include <string>

namespace pvz2native {
namespace {

/* ---------------------------------------------------------------- errno
 *
 * bionic's errno is `(*__errno())`, a per-thread int. The slot is a small guest
 * allocation made on first use, so the guest gets a real address to read and
 * write like any other. */
void c_errno(GuestCall &c) {
    c.set_result(c.errno_addr());
}

/* Only the handful of codes this port can actually produce are named. Shared by
 * strerror and strerror_r so the two can never disagree. */
const char *errno_text(std::uint32_t err) {
    switch (err) {
        case 2:   return "No such file or directory";
        case 3:   return "No such process";
        case 9:   return "Bad file descriptor";
        case 10:  return "No child processes";
        case 11:  return "Try again";
        case 12:  return "Out of memory";
        case 13:  return "Permission denied";
        case 16:  return "Device or resource busy";
        case 22:  return "Invalid argument";
        case 34:  return "Numerical result out of range";
        case 97:  return "Address family not supported by protocol";
        case 110: return "Connection timed out";
        default:  return "Unknown error";
    }
}

void c_strerror(GuestCall &c) {
    /* Returns a pointer to a static string, which must live in GUEST memory. */
    static std::uint32_t slot = 0;
    if (slot == 0) slot = c.rt->heap.alloc(64);
    c.put_cstr(slot, errno_text(c.arg(0)));
    c.set_result(slot);
}

/* ------------------------------------------------------------ setjmp/longjmp
 *
 * Real control flow, not a stub: the engine calls setjmp, and a longjmp that
 * did nothing would silently continue down the failing path instead of
 * unwinding. Both halves are ours, so the jmp_buf layout is ours too --
 * bionic's is `long[64]`, far more room than the callee-saved set needs.
 *
 * The dispatcher performs `pc = lr` after every handler returns, so longjmp
 * redirects execution by writing its target into lr rather than pc. */
constexpr std::uint32_t kJmpBufMagic = 0x4A4D5042; /* "JMPB" */

void c_setjmp(GuestCall &c) {
    std::uint32_t env = c.arg(0);
    if (env != 0 && c.in_bounds(env, 48)) {
        c.write32(env, kJmpBufMagic);
        for (int i = 0; i < 8; ++i) c.write32(env + 4 + (std::uint32_t)i * 4, c.regs[4 + i]); /* r4..r11 */
        c.write32(env + 36, c.regs[13]); /* sp */
        c.write32(env + 40, c.regs[14]); /* lr */
    }
    c.set_result(0); /* the direct call always returns 0 */
}

void c_longjmp(GuestCall &c) {
    std::uint32_t env = c.arg(0), val = c.arg(1);
    if (env == 0 || !c.in_bounds(env, 48) || c.read32(env) != kJmpBufMagic) {
        c.log("[libc] longjmp with an uninitialised jmp_buf (0x%08x) -- ignoring", env);
        c.set_result(val ? val : 1u);
        return;
    }
    for (int i = 0; i < 8; ++i) c.regs[4 + i] = c.read32(env + 4 + (std::uint32_t)i * 4);
    c.regs[13] = c.read32(env + 36);
    c.regs[14] = c.read32(env + 40); /* the dispatcher jumps here */
    c.set_result(val ? val : 1u);    /* setjmp must never appear to return 0 */
}

/* ------------------------------------------------------------ stack guard */

void c_stack_chk_fail(GuestCall &c) {
    c.log("[libc] __stack_chk_fail() -- the guest detected stack smashing at lr=0x%08x", c.lr());
    c.halt("__stack_chk_fail");
}

/* -------------------------------------------------------- process / system */

void c_sysconf(GuestCall &c) {
    /* bionic's _SC_* numbering. */
    switch (c.arg(0)) {
        case 39: c.set_result(4096); return; /* _SC_PAGESIZE          */
        case 97: c.set_result(4); return;    /* _SC_NPROCESSORS_ONLN  */
        case 96: c.set_result(4); return;    /* _SC_NPROCESSORS_CONF  */
        default: c.set_result(0); return;
    }
}

void c_getpagesize(GuestCall &c) { c.set_result(4096); }
void c_getpid(GuestCall &c) { c.set_result(1); }
void c_gettid(GuestCall &c) { c.set_result(guest_tls::self_id); }

/* A raw syscall cannot be serviced: the guest is asking the Linux kernel
 * directly, and there is none. Report the number so an unexpected one is
 * visible rather than a mystery -ENOSYS deep in the engine. */
void c_syscall(GuestCall &c) {
    static bool warned = false;
    if (!warned) {
        warned = true;
        c.log("[libc] raw syscall(%u) -- returning -ENOSYS (lr=0x%08x)", c.arg(0), c.lr());
    }
    c.set_result((std::uint32_t)-38 /* -ENOSYS */);
}

/* prctl is used for PR_SET_NAME (thread naming) and little else; accepting it
 * is exactly right. ptrace is the anti-debug check -- reporting "no tracer" is
 * both true and what the engine wants to hear. */
void c_prctl(GuestCall &c) { c.set_result(0); }
void c_ptrace(GuestCall &c) { c.set_result((std::uint32_t)-1); }

/* Names an address as <module>+0x<offset>, or "?" when it is in no module.
 * Guest code addresses in a log are useless without this: with dependencies
 * mapped, an address that looks like libPVZ2's is often libc++'s or Nimble's --
 * mistaking one for the other has already cost this project a long hunt. */
std::string where(const GuestCall &c, std::uint32_t addr) {
    const pvz2_elf_module_t *m = pvz2_elf_module_for_pc(c.img, addr & ~1u);
    char buf[128];
    if (m == nullptr) {
        std::snprintf(buf, sizeof(buf), "0x%08x (in no mapped module)", addr);
    } else {
        std::snprintf(buf, sizeof(buf), "0x%08x (%s+0x%x)", addr, m->name, (addr & ~1u) - m->base);
    }
    return buf;
}

/* raise(sig) -- and the one thing worth spending lines on is WHERE.
 *
 * SIGFPE(8) in particular is almost never a real signal: on ARM an integer
 * divide by zero calls __aeabi_idiv0, and bionic's implementation of that is a
 * raise(SIGFPE). So "guest called raise(8)" means "the engine divided by zero",
 * and the only useful question is which division -- which the bare message did
 * not answer, leaving a halt 128 words deep with nothing to grep for.
 *
 * lr is the immediate caller (usually __aeabi_idiv0 itself), so the stack is
 * scanned for the return addresses above it. It is a heuristic backtrace -- any
 * stale word that happens to point into a module shows up too -- but a wrong
 * ENTRY is obvious in a disassembler, whereas no entries at all leaves nowhere
 * to start. */
void c_raise(GuestCall &c) {
    const std::uint32_t sig = c.arg(0);
    c.log("[libc] guest called raise(%u)%s from lr=%s", sig,
          sig == 8 ? " [SIGFPE -- on ARM this is __aeabi_idiv0, i.e. an integer divide by zero]"
                   : "",
          where(c, c.lr()).c_str());

    unsigned shown = 0;
    for (std::uint32_t sp = c.sp(); shown < 12 && sp < c.sp() + 512; sp += 4) {
        const std::uint32_t w = c.read32(sp);
        const pvz2_elf_module_t *m = pvz2_elf_module_for_pc(c.img, w & ~1u);
        /* Return addresses only: a data word can point into a module too, but
         * an executable one is what a BL leaves behind. */
        if (m == nullptr || (w & ~1u) < m->base + m->text_vaddr ||
            (w & ~1u) >= m->base + m->text_vaddr + m->text_size) {
            continue;
        }
        c.log("  [raise] stack +0x%-3x -> %s", sp - c.sp(), where(c, w).c_str());
        ++shown;
    }
    c.halt("raise");
}

/* --- process control, new in 4.5.2 -----------------------------------------
 *
 * Crashlytics ships in 4.5.2 and this is its toolkit: fork a helper, exec a
 * reporter, wait for it. There is one emulated process here and no way to
 * create a second -- a guest "child" would need its own address space, and it
 * would immediately try to talk to a crash server this port has no network for
 * (see libc_socket.cpp).
 *
 * So they fail, with the errno that says the system will not make another
 * process. That is a state real Android reaches under memory pressure, so the
 * caller's error path is one the library was written to survive -- unlike
 * fork() returning 0, which would tell the guest it IS the child and send it
 * down a path that never returns. */
void c_fork(GuestCall &c) {
    static bool warned = false;
    if (!warned) {
        warned = true;
        c.log("[libc] fork() -- refused, this port is a single emulated process (lr=0x%08x)",
              c.lr());
    }
    c.set_errno(11 /* EAGAIN */);
    c.set_result((std::uint32_t)-1);
}

void c_execv(GuestCall &c) {
    c.set_errno(2 /* ENOENT */);
    c.set_result((std::uint32_t)-1);
}

/* system(cmd) returns -1 for "could not run a shell"; system(NULL) asks whether
 * one EXISTS, and the answer to that is 0 for no. */
void c_system(GuestCall &c) { c.set_result((std::uint32_t)-1); }

/* ECHILD: there are no children to wait for, which follows from fork failing. */
void c_waitpid(GuestCall &c) {
    c.set_errno(10 /* ECHILD */);
    c.set_result((std::uint32_t)-1);
}

/* getpid() already answers 1, so the parent is the traditional init. */
void c_getppid(GuestCall &c) { c.set_result(1); }

/* sigaction(sig, act, oact): accepted and ignored.
 *
 * Deliberately reported as SUCCESS with a zeroed `oact`. The guest cannot
 * receive a real signal -- it is JIT-executed code inside our process, and a
 * host fault never becomes a guest one -- so an installed handler would never
 * run either way. 4.5.2's JNI_OnLoad installs one per signal in a loop and
 * stores the old action; failing there would be a startup error path taken for
 * no reason. */
void c_sigaction(GuestCall &c) {
    const std::uint32_t oact = c.arg(2);
    if (oact != 0) {
        for (std::uint32_t i = 0; i < 16; i += 4) c.write32(oact + i, 0);
    }
    c.set_result(0);
}

/* void __assert2(const char *file, int line, const char *func, const char *msg)
 *
 * bionic's assert(). Reaching it means the guest has already decided its own
 * state is impossible, so the one useful thing is to print WHAT it was before
 * stopping -- an abort with no message here would be indistinguishable from
 * any other halt. */
void c_assert2(GuestCall &c) {
    c.log("[guest assert] %s:%u: %s: %s", c.cstr(c.arg(0), 256).c_str(), c.arg(1),
          c.cstr(c.arg(2), 128).c_str(), c.cstr(c.arg(3), 256).c_str());
    c.halt("__assert2");
}

/* int __aeabi_atexit(void *obj, void (*dtor)(void*), void *dso_handle)
 *
 * The ARM EABI spelling of __cxa_atexit, which libc_stdlib already accepts and
 * ignores: static destructors run at process exit, and this process exits by
 * ending. Registering them would mean running guest code after the JIT and heap
 * are gone. */
void c_aeabi_atexit(GuestCall &c) { c.set_result(0); }

/* --- what 9.6.1 adds -------------------------------------------------------- */

/* int strerror_r(int errnum, char *buf, size_t buflen)
 *
 * The XSI form, which is what bionic provides: writes into the caller's buffer
 * and returns 0, or ERANGE if it did not fit. NOT the GNU form that returns a
 * char* -- getting that backwards would have the caller print a pointer. */
void c_strerror_r(GuestCall &c) {
    const std::uint32_t buf = c.arg(1), buflen = c.arg(2);
    const std::string msg = errno_text(c.arg(0));
    if (buf == 0 || buflen == 0) {
        c.set_result(22 /* EINVAL */);
        return;
    }
    if (msg.size() + 1 > buflen) {
        c.set_result(34 /* ERANGE */);
        return;
    }
    c.put_cstr(buf, msg);
    c.set_result(0);
}

/* Identity. One user, one group, and not root -- claiming uid 0 would send any
 * caller that checks down a privileged path this port cannot honour. The values
 * match a typical Android app uid. */
void c_getuid(GuestCall &c) { c.set_result(10001); }
void c_getgid(GuestCall &c) { c.set_result(10001); }

/* getpwuid/getpwnam return a `struct passwd *` in GUEST memory, so the struct
 * and every string it points at are built in one heap block. bionic's layout is
 * {name, passwd, uid, gid, gecos, dir, shell} -- seven words on ARM32.
 *
 * Answering NULL instead is the tempting shortcut and the wrong one: a caller
 * that asked "who am I?" and got NULL usually treats it as a fatal
 * misconfiguration rather than as "no such user". */
constexpr std::uint32_t kPasswdSize = 7 * 4;

void build_passwd(GuestCall &c, std::uint32_t *out) {
    static std::uint32_t block = 0;
    if (block == 0) {
        block = c.rt->heap.alloc(kPasswdSize + 96);
        if (block == 0) {
            *out = 0;
            return;
        }
        const std::uint32_t strings = block + kPasswdSize;
        const std::uint32_t name = strings;
        const std::uint32_t empty = strings + 16;
        const std::uint32_t dir = strings + 32;
        const std::uint32_t shell = strings + 64;
        c.put_cstr(name, "app");
        c.put_cstr(empty, "");
        c.put_cstr(dir, std::string("/data/data/") + pvz2_config()->package_name);
        c.put_cstr(shell, "/system/bin/sh");
        c.write32(block + 0, name);
        c.write32(block + 4, empty);  /* pw_passwd */
        c.write32(block + 8, 10001);  /* pw_uid    */
        c.write32(block + 12, 10001); /* pw_gid    */
        c.write32(block + 16, empty); /* pw_gecos  */
        c.write32(block + 20, dir);
        c.write32(block + 24, shell);
    }
    *out = block;
}

void c_getpwuid(GuestCall &c) {
    std::uint32_t pw = 0;
    build_passwd(c, &pw);
    c.set_result(pw);
}

/* int getpwuid_r(uid_t, struct passwd *pwd, char *buf, size_t buflen,
 *                struct passwd **result)
 * Copies into the caller's struct and sets *result to it; 0 on success. */
void c_getpwuid_r(GuestCall &c) {
    const std::uint32_t pwd = c.arg(1), result = c.arg(4);
    std::uint32_t src = 0;
    build_passwd(c, &src);
    if (src == 0 || pwd == 0) {
        if (result != 0) c.write32(result, 0);
        c.set_result(22 /* EINVAL */);
        return;
    }
    for (std::uint32_t i = 0; i < kPasswdSize; i += 4) {
        c.write32(pwd + i, c.read32(src + i));
    }
    if (result != 0) c.write32(result, pwd);
    c.set_result(0);
}

/* Groups. `struct group` is {name, passwd, gid, members} -- four words, with
 * members a NULL-terminated char* array. */
void c_getgrgid(GuestCall &c) {
    static std::uint32_t block = 0;
    if (block == 0) {
        block = c.rt->heap.alloc(4 * 4 + 32);
        if (block != 0) {
            const std::uint32_t name = block + 16;
            const std::uint32_t empty = block + 24;
            const std::uint32_t members = block + 28; /* one NULL entry */
            c.put_cstr(name, "app");
            c.put_cstr(empty, "");
            c.write32(members, 0);
            c.write32(block + 0, name);
            c.write32(block + 4, empty);
            c.write32(block + 8, 10001);
            c.write32(block + 12, members);
        }
    }
    c.set_result(block);
}

/* --- <sys/mman.h> ------------------------------------------------------------
 *
 * mmap is refused rather than emulated. A real one would have to carve an
 * unused range out of the flat guest address space and keep it out of the
 * heap's reach; nothing here needs that, and the callers (libc++'s allocator
 * fallback, Crashlytics) all treat MAP_FAILED as "use malloc instead", which is
 * a path they take on any memory-constrained device.
 *
 * MAP_FAILED is (void*)-1, NOT NULL -- a caller comparing against 0 would
 * happily use the failed mapping. */
void c_mmap(GuestCall &c) {
    static bool warned = false;
    if (!warned) {
        warned = true;
        c.log("[libc] mmap() -- refused (MAP_FAILED/ENOMEM); callers fall back to malloc");
    }
    c.set_errno(12 /* ENOMEM */);
    c.set_result((std::uint32_t)-1);
}

/* munmap of something mmap never handed out. Succeeding is right: the caller is
 * unwinding, and an error there is noise it cannot act on. */
void c_munmap(GuestCall &c) { c.set_result(0); }

/* The whole guest address space is one flat read/write allocation, so there are
 * no protections to change and nothing to pin. Both succeed. */
void c_mprotect(GuestCall &c) { c.set_result(0); }
void c_mlock(GuestCall &c) { c.set_result(0); }

/* --- <libgen.h> --------------------------------------------------------------
 *
 * Both may modify the input buffer and return a pointer into it, which is
 * exactly what the POSIX versions do -- so the result stays valid guest memory
 * with no allocation. */
void c_basename(GuestCall &c) {
    const std::uint32_t path = c.arg(0);
    static std::uint32_t dot = 0;
    if (dot == 0) {
        dot = c.rt->heap.alloc(8);
        if (dot != 0) c.put_cstr(dot, ".");
    }
    if (path == 0 || c.read8(path) == 0) {
        c.set_result(dot);
        return;
    }
    std::uint32_t len = 0;
    while (c.read8(path + len) != 0) ++len;
    /* Trailing slashes are not part of the name. */
    while (len > 1 && c.read8(path + len - 1) == '/') --len;
    if (len == 1 && c.read8(path) == '/') {
        c.set_result(path); /* "/" is its own basename */
        return;
    }
    c.write8(path + len, 0);
    std::uint32_t start = len;
    while (start > 0 && c.read8(path + start - 1) != '/') --start;
    c.set_result(path + start);
}

void c_dirname(GuestCall &c) {
    const std::uint32_t path = c.arg(0);
    static std::uint32_t dot = 0;
    if (dot == 0) {
        dot = c.rt->heap.alloc(8);
        if (dot != 0) c.put_cstr(dot, ".");
    }
    if (path == 0 || c.read8(path) == 0) {
        c.set_result(dot);
        return;
    }
    std::uint32_t len = 0;
    while (c.read8(path + len) != 0) ++len;
    while (len > 1 && c.read8(path + len - 1) == '/') --len;
    std::uint32_t cut = len;
    while (cut > 0 && c.read8(path + cut - 1) != '/') --cut;
    if (cut == 0) {
        c.set_result(dot); /* no slash at all -> "." */
        return;
    }
    while (cut > 1 && c.read8(path + cut - 1) == '/') --cut;
    c.write8(path + cut, 0);
    c.set_result(path);
}

/* kill(pid, sig): the only reachable target is this process, and delivering a
 * signal to it is not something the guest can survive meaningfully -- see
 * c_sigaction on why handlers never run. ESRCH says "no such process", which is
 * true of every pid but our own. */
void c_kill(GuestCall &c) {
    c.log("[libc] kill(pid=%u, sig=%u) ignored", c.arg(0), c.arg(1));
    c.set_errno(3 /* ESRCH */);
    c.set_result((std::uint32_t)-1);
}

/* sighandler_t bsd_signal(int sig, sighandler_t handler) -- bionic's signal().
 * Accepted and ignored, returning SIG_DFL as the previous handler, for the same
 * reason as sigaction: the guest can never receive one. */
void c_bsd_signal(GuestCall &c) { c.set_result(0 /* SIG_DFL */); }

/* execl(path, arg0, ...) -- varargs sibling of execv, which already refuses. */
void c_execl(GuestCall &c) {
    c.set_errno(2 /* ENOENT */);
    c.set_result((std::uint32_t)-1);
}

}  // namespace

void register_libc_misc(ImportTable &t) {
    t.add("__errno", c_errno);
    t.add("__errno_location", c_errno);
    t.add("strerror", c_strerror);

    t.add("setjmp", c_setjmp);
    t.add("_setjmp", c_setjmp);
    t.add("longjmp", c_longjmp);
    t.add("_longjmp", c_longjmp);

    t.add("__stack_chk_fail", c_stack_chk_fail);

    t.add("sysconf", c_sysconf);
    t.add("getpagesize", c_getpagesize);
    t.add("getpid", c_getpid);
    t.add("gettid", c_gettid);
    t.add("syscall", c_syscall);
    t.add("prctl", c_prctl);
    t.add("ptrace", c_ptrace);
    t.add("raise", c_raise);

    t.add("fork", c_fork);
    t.add("execv", c_execv);
    t.add("system", c_system);
    t.add("waitpid", c_waitpid);
    t.add("getppid", c_getppid);
    t.add("sigaction", c_sigaction);

    t.add("__assert2", c_assert2);
    t.add("__aeabi_atexit", c_aeabi_atexit);

    /* --- added for 9.6.1 --- */
    t.add("strerror_r", c_strerror_r);

    t.add("getuid", c_getuid);
    t.add("geteuid", c_getuid);
    t.add("getgid", c_getgid);
    t.add("getegid", c_getgid);
    t.add("getpwuid", c_getpwuid);
    t.add("getpwnam", c_getpwuid); /* one user, so the name is not consulted  */
    t.add("getpwuid_r", c_getpwuid_r);
    t.add("getgrgid", c_getgrgid);
    t.add("getgrnam", c_getgrgid); /* likewise: one group                      */

    t.add("mmap", c_mmap);
    t.add("munmap", c_munmap);
    t.add("mprotect", c_mprotect);
    t.add("mlock", c_mlock);

    t.add("basename", c_basename);
    t.add("dirname", c_dirname);

    t.add("kill", c_kill);
    t.add("bsd_signal", c_bsd_signal);
    t.add("signal", c_bsd_signal);
    t.add("execl", c_execl);
}

}  // namespace pvz2native
