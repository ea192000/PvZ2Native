/* libc.so -- <stdlib.h>: the allocator, numeric conversion, sorting, exit.
 *
 * The allocator is GuestHeap (src/runtime/), which hands out addresses inside
 * the emulated address space; the host heap is never exposed to the guest.
 */

#include <pvz2native/dependencies/dependency.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace pvz2native {
namespace {

/* --------------------------------------------------------------- memory */

void c_malloc(GuestCall &c) {
    std::uint32_t size = c.arg(0);
    std::uint32_t addr = c.rt->heap.alloc(size, c.lr());
    c.on_alloc(addr, size);
    c.set_result(addr);
}

void c_calloc(GuestCall &c) {
    std::uint32_t n = c.arg(0) * c.arg(1);
    std::uint32_t addr = c.rt->heap.alloc(n, c.lr());
    if (addr != 0 && c.in_bounds(addr, n)) std::memset(&c.img->mem[addr], 0, n);
    c.on_alloc(addr, n);
    c.set_result(addr);
}

void c_realloc(GuestCall &c) {
    std::uint32_t old_addr = c.arg(0), new_size = c.arg(1);
    if (old_addr == 0) {
        c.set_result(c.rt->heap.alloc(new_size, c.lr()));
        return;
    }
    if (new_size == 0) {
        c.rt->heap.free_ptr(old_addr, c.lr());
        c.set_result(0);
        return;
    }
    std::uint32_t old_size = c.rt->heap.size_of(old_addr);
    std::uint32_t new_addr = c.rt->heap.alloc(new_size, c.lr());
    if (new_addr != 0 && c.in_bounds(new_addr, new_size) && c.in_bounds(old_addr, old_size)) {
        std::memcpy(&c.img->mem[new_addr], &c.img->mem[old_addr], std::min(old_size, new_size));
    }
    c.rt->heap.free_ptr(old_addr, c.lr());
    c.set_result(new_addr);
}

void c_free(GuestCall &c) {
    c.rt->heap.free_ptr(c.arg(0), c.lr());
}

void c_memalign(GuestCall &c) {
    c.set_result(c.rt->heap.alloc_aligned(c.arg(1), c.arg(0), c.lr()));
}

void c_posix_memalign(GuestCall &c) {
    /* (void **memptr, size_t alignment, size_t size) -- note the result comes
     * back through the out-parameter, and the RETURN value is an errno code. */
    std::uint32_t addr = c.rt->heap.alloc_aligned(c.arg(2), c.arg(1), c.lr());
    if (c.arg(0) != 0) c.write32(c.arg(0), addr);
    c.set_result(addr != 0 ? 0u : 12u /* ENOMEM */);
}

void c_valloc(GuestCall &c) {
    c.set_result(c.rt->heap.alloc_aligned(c.arg(0), 4096, c.lr()));
}

/* ------------------------------------------------------------ conversion */

void c_atoi(GuestCall &c) {
    c.set_result((std::uint32_t)std::strtol(c.cstr(c.arg(0), 64).c_str(), nullptr, 10));
}

void c_atof(GuestCall &c) {
    c.set_resultd(std::strtod(c.cstr(c.arg(0), 128).c_str(), nullptr));
}

void strto_integer(GuestCall &c, bool is_signed) {
    std::uint32_t nptr = c.arg(0), endptr = c.arg(1);
    int base = (int)(std::int32_t)c.arg(2);
    std::string s = c.cstr(nptr, 128);
    char *end = nullptr;
    unsigned long v = is_signed ? (unsigned long)std::strtol(s.c_str(), &end, base)
                                : std::strtoul(s.c_str(), &end, base);
    if (endptr != 0) {
        std::uint32_t consumed = end ? (std::uint32_t)(end - s.c_str()) : 0;
        c.write32(endptr, nptr + consumed);
    }
    c.set_result((std::uint32_t)v);
}

void c_strtol(GuestCall &c) { strto_integer(c, true); }
void c_strtoul(GuestCall &c) { strto_integer(c, false); }

/* The 64-bit pair. Separate from strto_integer because the result needs r1 as
 * well as r0: `long` is 32 bits on ARM32 but `long long` is 64, so truncating
 * through the 32-bit path would silently lose the top half of every large
 * value -- and these are exactly what a save file's timestamps go through. */
void strto_integer64(GuestCall &c, bool is_signed) {
    const std::uint32_t nptr = c.arg(0), endptr = c.arg(1);
    const int base = (int)(std::int32_t)c.arg(2);
    const std::string s = c.cstr(nptr, 128);
    char *end = nullptr;
    const unsigned long long v = is_signed
                                     ? (unsigned long long)std::strtoll(s.c_str(), &end, base)
                                     : std::strtoull(s.c_str(), &end, base);
    if (endptr != 0) {
        const std::uint32_t consumed = end ? (std::uint32_t)(end - s.c_str()) : 0;
        c.write32(endptr, nptr + consumed);
    }
    c.set_result64((std::uint64_t)v);
}

void c_strtoll(GuestCall &c) { strto_integer64(c, true); }
void c_strtoull(GuestCall &c) { strto_integer64(c, false); }

void c_strtod(GuestCall &c) {
    std::string s = c.cstr(c.arg(0), 128);
    char *end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (c.arg(1) != 0) c.write32(c.arg(1), c.arg(0) + (std::uint32_t)(end - s.c_str()));
    c.set_resultd(v);
}

void c_strtof(GuestCall &c) {
    std::string s = c.cstr(c.arg(0), 128);
    char *end = nullptr;
    float v = std::strtof(s.c_str(), &end);
    if (c.arg(1) != 0) c.write32(c.arg(1), c.arg(0) + (std::uint32_t)(end - s.c_str()));
    c.set_resultf(v);
}

void c_abs(GuestCall &c) {
    std::int32_t v = (std::int32_t)c.arg(0);
    c.set_result((std::uint32_t)(v < 0 ? -v : v));
}

/* --------------------------------------------------------------- random */

void c_rand(GuestCall &c) { c.set_result((std::uint32_t)std::rand()); }
void c_srand(GuestCall &c) { std::srand(c.arg(0)); }
/* lrand48 is specified to return a non-negative 31-bit value. */
void c_lrand48(GuestCall &c) { c.set_result((std::uint32_t)(std::rand() & 0x7FFFFFFF)); }
void c_srand48(GuestCall &c) { std::srand(c.arg(0)); }
void c_drand48(GuestCall &c) { c.set_resultd((double)std::rand() / ((double)RAND_MAX + 1.0)); }

/* ----------------------------------------------------------------- sort
 *
 * qsort's comparator is GUEST code. Calling it means running the JIT from
 * inside a JIT callback, which call_guest() does on a fresh Jit instance. That
 * is expensive per comparison, so the array is sorted with std::stable_sort
 * (n log n, no worse than qsort would do) and the two elements under comparison
 * are staged into a pair of scratch blocks the comparator can legally read
 * through -- the guest is handed real guest addresses, exactly as it expects. */
void c_qsort(GuestCall &c) {
    std::uint32_t base = c.arg(0), nmemb = c.arg(1), size = c.arg(2), compar = c.arg(3);
    if (base == 0 || nmemb < 2 || size == 0 || compar == 0) return;
    if (!c.in_bounds(base, nmemb * size)) {
        c.log("[libc] qsort(0x%08x, %u, %u) is out of guest bounds -- ignored", base, nmemb, size);
        return;
    }

    std::uint32_t lhs = c.rt->heap.alloc(size);
    std::uint32_t rhs = c.rt->heap.alloc(size);
    if (lhs == 0 || rhs == 0) {
        c.log("[libc] qsort could not allocate comparison scratch -- array left unsorted");
        return;
    }

    /* Work on a host-side copy of the element bytes so the comparator never
     * observes a half-permuted array. */
    std::vector<std::vector<std::uint8_t>> items(nmemb);
    for (std::uint32_t i = 0; i < nmemb; ++i) {
        items[i].assign(&c.img->mem[base + i * size], &c.img->mem[base + (i + 1) * size]);
    }

    std::stable_sort(items.begin(), items.end(),
                     [&](const std::vector<std::uint8_t> &a, const std::vector<std::uint8_t> &b) {
                         std::memcpy(&c.img->mem[lhs], a.data(), size);
                         std::memcpy(&c.img->mem[rhs], b.data(), size);
                         const std::uint32_t args[2] = {lhs, rhs};
                         return (std::int32_t)c.call_guest(compar, args, 2) < 0;
                     });

    for (std::uint32_t i = 0; i < nmemb; ++i) {
        std::memcpy(&c.img->mem[base + i * size], items[i].data(), size);
    }
    c.rt->heap.free_ptr(lhs);
    c.rt->heap.free_ptr(rhs);
}

void c_bsearch(GuestCall &c) {
    std::uint32_t key = c.arg(0), base = c.arg(1), nmemb = c.arg(2), size = c.arg(3);
    std::uint32_t compar = c.arg(4);
    if (base == 0 || nmemb == 0 || size == 0 || compar == 0) { c.set_result(0); return; }
    std::uint32_t lo = 0, hi = nmemb;
    while (lo < hi) {
        std::uint32_t mid = lo + (hi - lo) / 2;
        const std::uint32_t args[2] = {key, base + mid * size};
        std::int32_t r = (std::int32_t)c.call_guest(compar, args, 2);
        if (r == 0) { c.set_result(base + mid * size); return; }
        if (r < 0) hi = mid;
        else lo = mid + 1;
    }
    c.set_result(0);
}

/* ----------------------------------------------------------- environment */

void c_getenv(GuestCall &c) { c.set_result(0); }   /* an Android app has no environment worth exposing */
void c_setenv(GuestCall &c) { c.set_result(0); }
void c_unsetenv(GuestCall &c) { c.set_result(0); } /* nothing was ever set */
void c_putenv(GuestCall &c) { c.set_result(0); }

void c_mktemp(GuestCall &c) { c.set_result(c.arg(0)); }

/* ------------------------------------------------------------------ exit */

void c_exit(GuestCall &c) {
    c.log("[libc] guest called exit(%d)", (std::int32_t)c.arg(0));
    c.halt("exit");
}

void c_abort(GuestCall &c) {
    c.log("[libc] guest called abort()");
    c.halt("abort");
}

/* Registered by libstdc++'s ABI but implemented by libc: the guest's static
 * destructors never run, because the process ends with the host's. */
void c_cxa_atexit(GuestCall &c) { c.set_result(0); }

}  // namespace

void register_libc_stdlib(ImportTable &t) {
    t.add("malloc", c_malloc);
    t.add("calloc", c_calloc);
    t.add("realloc", c_realloc);
    t.add("free", c_free);
    t.add("memalign", c_memalign);
    t.add("posix_memalign", c_posix_memalign);
    t.add("valloc", c_valloc);

    t.add("atoi", c_atoi);
    t.add("atol", c_atoi);
    t.add("atoll", c_atoi);
    t.add("atof", c_atof);
    t.add("strtol", c_strtol);
    t.add("strtoul", c_strtoul);
    t.add("strtod", c_strtod);
    t.add("strtof", c_strtof);
    t.add("abs", c_abs);
    t.add("labs", c_abs);

    t.add("rand", c_rand);
    t.add("srand", c_srand);
    t.add("lrand48", c_lrand48);
    t.add("srand48", c_srand48);
    t.add("drand48", c_drand48);

    t.add("qsort", c_qsort);
    t.add("bsearch", c_bsearch);

    t.add("strtoll", c_strtoll);
    t.add("strtoull", c_strtoull);
    /* intmax_t is 64 bits on armeabi-v7a, so these ARE strtoll/strtoull -- the
     * same function under the <inttypes.h> spelling. Routing them to the 32-bit
     * strtol instead would silently halve any large value. */
    t.add("strtoimax", c_strtoll);
    t.add("strtoumax", c_strtoull);
    t.add("getenv", c_getenv);
    t.add("setenv", c_setenv);
    t.add("unsetenv", c_unsetenv);
    t.add("putenv", c_putenv);
    t.add("mktemp", c_mktemp);

    t.add("exit", c_exit);
    t.add("_exit", c_exit);
    t.add("abort", c_abort);
    t.add("__cxa_atexit", c_cxa_atexit);
    t.add("__cxa_finalize", c_cxa_atexit);
}

}  // namespace pvz2native
