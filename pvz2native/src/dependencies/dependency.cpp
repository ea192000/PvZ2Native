#include <pvz2native/dependencies/dependency.h>

#include <pvz2native/config.h>
#include <pvz2native/log.h>

#include <atomic>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace pvz2native {

/* --- GuestCall accessors ---------------------------------------------------
 *
 * AAPCS: the first four words are r0..r3, the rest are on the stack above sp.
 * Under softfp a float occupies one word and a double an even-aligned pair,
 * exactly like the integer types of the same size -- so the same indexing
 * works for both. */

std::uint32_t GuestCall::arg(int i) const {
    if (i < 4) return regs[i];
    std::uint32_t sp = regs[13];
    return read32(sp + (std::uint32_t)(i - 4) * 4);
}

float GuestCall::argf(int i) const {
    std::uint32_t bits = arg(i);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

double GuestCall::argd(int i) const {
    /* 64-bit arguments are 8-byte aligned, so they start on an even index. */
    int idx = (i + 1) & ~1;
    std::uint64_t bits = (std::uint64_t)arg(idx) | ((std::uint64_t)arg(idx + 1) << 32);
    double d;
    std::memcpy(&d, &bits, 8);
    return d;
}

void GuestCall::set_resultf(float v) {
    std::uint32_t bits;
    std::memcpy(&bits, &v, 4);
    regs[0] = bits;
}

void GuestCall::set_resultd(double v) {
    std::uint64_t bits;
    std::memcpy(&bits, &v, 8);
    set_result64(bits);
}

bool GuestCall::in_bounds(std::uint32_t addr, std::uint32_t size) const {
    return addr < img->mem_size && (std::uint64_t)addr + size <= img->mem_size;
}

std::uint8_t GuestCall::read8(std::uint32_t addr) const {
    return in_bounds(addr, 1) ? img->mem[addr] : 0;
}
std::uint16_t GuestCall::read16(std::uint32_t addr) const {
    std::uint16_t v = 0;
    if (in_bounds(addr, 2)) std::memcpy(&v, &img->mem[addr], 2);
    return v;
}
std::uint32_t GuestCall::read32(std::uint32_t addr) const {
    std::uint32_t v = 0;
    if (in_bounds(addr, 4)) std::memcpy(&v, &img->mem[addr], 4);
    return v;
}
void GuestCall::write8(std::uint32_t addr, std::uint8_t v) {
    if (in_bounds(addr, 1)) img->mem[addr] = v;
}
void GuestCall::write16(std::uint32_t addr, std::uint16_t v) {
    if (in_bounds(addr, 2)) std::memcpy(&img->mem[addr], &v, 2);
}
void GuestCall::write32(std::uint32_t addr, std::uint32_t v) {
    if (in_bounds(addr, 4)) std::memcpy(&img->mem[addr], &v, 4);
}

std::string GuestCall::cstr(std::uint32_t addr, std::size_t max) const {
    std::string s;
    for (std::size_t i = 0; i < max; ++i) {
        if (!in_bounds(addr + (std::uint32_t)i, 1)) break;
        char c = (char)img->mem[addr + i];
        if (c == '\0') break;
        s.push_back(c);
    }
    return s;
}

void GuestCall::put_cstr(std::uint32_t addr, const std::string &s) {
    if (addr == 0 || !in_bounds(addr, (std::uint32_t)s.size() + 1)) return;
    std::memcpy(&img->mem[addr], s.data(), s.size());
    img->mem[addr + s.size()] = 0;
}

std::uint32_t GuestCall::dup_cstr(const std::string &s) {
    std::uint32_t addr = rt->heap.alloc((std::uint32_t)s.size() + 1);
    put_cstr(addr, s);
    return addr;
}

void *GuestCall::ptr(std::uint32_t addr, std::uint32_t size) const {
    return in_bounds(addr, size) ? (void *)&img->mem[addr] : nullptr;
}

std::FILE *GuestCall::file(std::uint32_t token) const {
    std::lock_guard<std::mutex> lk(rt->files_lock);
    auto it = rt->host_files.find(token);
    return it == rt->host_files.end() ? nullptr : it->second;
}

int GuestCall::fd(std::uint32_t token) const {
    std::lock_guard<std::mutex> lk(rt->files_lock);
    auto it = rt->host_fds.find(token);
    return it == rt->host_fds.end() ? -1 : it->second;
}

void GuestCall::log(const char *fmt, ...) const {
    /* Shared host logger: timestamp + [log] enable gate. rt->log_lock is no
     * longer needed here -- pvz2_log serialises itself. */
    (void)rt;
    va_list ap;
    va_start(ap, fmt);
    pvz2_logv(fmt, ap);
    va_end(ap);
}

std::uint32_t GuestCall::errno_addr() const {
    if (guest_tls::errno_slot == 0) guest_tls::errno_slot = rt->heap.alloc(4);
    return guest_tls::errno_slot;
}

void GuestCall::set_errno(std::uint32_t value) {
    std::uint32_t slot = errno_addr();
    if (slot != 0) write32(slot, value);
}

/* --- tracing --------------------------------------------------------------- */

namespace trace {
namespace {
/* Tracing the boot writes ~57MB of stdout under a lock. The [log] trace switch
 * in config.ini is the master gate; g_dynamic is the runtime toggle the
 * steady-state frame sample flips around specific frames. enabled() ANDs the
 * two, and reads the config live (not at static-init time, which runs before
 * config.ini is loaded). g_dynamic starts on so that when the master is set,
 * tracing covers the boot from the first call, exactly as PVZ2_TRACE=1 did. */
std::atomic<bool> g_dynamic{true};
}  // namespace
void set(bool on) { g_dynamic.store(on, std::memory_order_relaxed); }
bool enabled() {
    return pvz2_config()->trace != 0 && g_dynamic.load(std::memory_order_relaxed);
}
}  // namespace trace

/* --- per-guest-thread libc state ------------------------------------------ */

namespace guest_tls {
thread_local std::uint32_t self_id = 1; /* overwritten inside spawned threads */
thread_local std::uint32_t strtok_next = 0;
thread_local std::uint32_t errno_slot = 0;

std::unordered_map<std::uint32_t, std::uint32_t> &values() {
    static thread_local std::unordered_map<std::uint32_t, std::uint32_t> map;
    return map;
}
}  // namespace guest_tls

/* --- the table ------------------------------------------------------------ */

void register_libc(ImportTable &t) {
    register_libc_string(t);
    register_libc_wchar(t);
    register_libc_stdio(t);
    register_libc_stdlib(t);
    register_libc_time(t);
    register_libc_unistd(t);
    register_libc_pthread(t);
    register_libc_locale(t);
    register_libc_ctype(t);
    register_libc_socket(t);
    register_libc_misc(t);
}

const ImportTable &import_table() {
    static const ImportTable table = [] {
        ImportTable t;
        register_libc(t);
        register_libm(t);
        register_libz(t);
        register_liblog(t);
        register_libgles(t);
        register_libstdcxx(t);
        register_libopensles(t);
        register_libdl(t);
        return t;
    }();
    return table;
}

}  // namespace pvz2native
