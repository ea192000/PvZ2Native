/* Reading a guest global, and handing the guest a std::string -- see
 * diagnostics.h. */

#include <pvz2native/diagnostics/diagnostics.h>
#include <pvz2native/log.h>

#include <cstdio>
#include <cstring>

#include <pvz2native/game/symbols.h>
#include <pvz2native/runtime/dynarmic_config.h>

namespace pvz2native {
namespace diagnostics {

void dump_dword(pvz2_elf_image_t *img, const char *label, std::uint32_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, &img->mem[img->so_base + offset], 4);
    pvz2_log("[dbg] %s (offset 0x%x) = 0x%08x", label, offset, value);
}

namespace {

std::uint32_t rd32(pvz2_elf_image_t *img, std::uint32_t addr) {
    std::uint32_t v = 0;
    if (addr != 0 && (std::uint64_t)addr + 4 <= img->mem_size) std::memcpy(&v, &img->mem[addr], 4);
    return v;
}

std::uint8_t rd8(pvz2_elf_image_t *img, std::uint32_t addr) {
    return (addr != 0 && addr < img->mem_size) ? img->mem[addr] : 0;
}

/* Everything this probe needs to know about the loaded build comes from
 * sym().probe -- see GameSymbols. It used to be a block of 9.6.1 literals right
 * here, which was both a .so address outside game/ and an active misreport: 4.5.2
 * has a non-zero app_driver global, so the probe ran on it and printed 9.6.1's
 * field offsets against a 4.5.2 object. Plausible numbers, all meaningless. */

}  // namespace

void dump_frame_gate(pvz2_elf_image_t *img, const char *when) {
    const std::uint32_t driver_global = sym().global.app_driver;
    if (img == nullptr || driver_global == 0) return; /* not mapped for this build */

    const std::uint32_t driver = rd32(img, img->so_base + driver_global);

    /* Said loudly, because a NULL driver makes the byte columns LIE in a way
     * that looks like success: `driver + 326` degenerates to absolute address
     * 326, so a write and a read both land in guest low memory and agree with
     * each other. The engine does the same thing -- it dereferences NULL, walks
     * a vtable of zeros and BLXes to address 0, where zeroed memory decodes as
     * ANDEQ r0,r0,r0 (a NOP) until it reaches the trampoline table at 0x1000 and
     * halts. That is what a frame costing a fixed ~1052 ticks with zero imports
     * actually is: not a short-circuit branch, an executed void. */
    if (driver == 0) {
        pvz2_log("[gate] %-22s driver=[0x%08x] is NULL -- nothing has published the "
                    "AndroidAppDriver; the byte columns below read absolute low memory, NOT the "
                    "object", when, img->so_base + driver_global);
    }
    /* The byte columns only when this build's offsets are actually known. An
     * unmapped build would read driver+0 and print four bytes of the vptr as if
     * they were the gate flags. */
    const auto &p = sym().probe;
    if (p.gate_skip_frame != 0) {
        pvz2_log("[gate] %-22s driver=[0x%08x]=0x%08x  skip_frame(+%u)=%u  "
                    "surface(+%u)=%u running(+%u)=%u focus(+%u)=%u",
                    when, img->so_base + driver_global, driver,
                    p.gate_skip_frame, rd8(img, driver + p.gate_skip_frame),
                    p.gate_surface, rd8(img, driver + p.gate_surface),
                    p.gate_running, rd8(img, driver + p.gate_running),
                    p.gate_focus, rd8(img, driver + p.gate_focus));
    } else {
        pvz2_log("[gate] %-22s driver=[0x%08x]=0x%08x  (gate offsets not mapped for %s)",
                    when, img->so_base + driver_global, driver, sym().version);
    }

    /* The app, and the driver hanging off it. These two split the failure in
     * half at a glance:
     *   app == 0        -- the framework constructor never finished, so nothing
     *                      downstream of it ran either;
     *   app != 0, drv 0 -- SexyAppBase's constructor died before building the
     *                      driver;
     *   both != 0, but the driver GLOBAL above still 0 -- the object exists and
     *                      simply was never published, i.e. AndroidAppDriver::
     *                      SexyAppRun (which ends in that store) was not reached. */
    /* How far SexyAppBase's constructor got, which is the question a NULL driver
     * global cannot answer on its own -- see GameSymbols::probe::file_driver for
     * how to read the pair. Printed before the app line because when the answer
     * is "it died in the base chain" the app object does not exist yet either. */
    if (p.file_driver != 0) {
        const std::uint32_t fdrv = rd32(img, img->so_base + p.file_driver);
        pvz2_log("[gate] %-22s file_driver=[0x%08x]=0x%08x  (%s)", when,
                    img->so_base + p.file_driver, fdrv,
                    fdrv != 0 ? "SexyAppBase's ctor reached its file-driver line"
                              : "NOT created -- the ctor died at or above it, and the "
                                "resources.xml probe that runs next will deref this NULL");
    }

    const std::uint32_t app_global = sym().global.app;
    if (app_global == 0 || p.app_driver_field == 0) return;
    const std::uint32_t app = rd32(img, img->so_base + app_global);
    const std::uint32_t dispatch_obj = rd32(img, app + p.app_driver_field);
    pvz2_log("[gate] %-22s app=[0x%08x]=0x%08x  *(app+%u)=0x%08x  (%s the driver global)",
                when, img->so_base + app_global, app, p.app_driver_field, dispatch_obj,
                dispatch_obj == driver ? "SAME as" : "DIFFERENT from");

    if (dispatch_obj == 0 || p.driver_vtable == 0) return;

    /* Is that object really an AndroidAppDriver, and is the vtable base right?
     * Checked against the live vptr, because the base was derived by reading
     * .data.rel.ro by hand and a plausible-but-wrong base would silently
     * misidentify every slot. */
    const std::uint32_t vptr = rd32(img, dispatch_obj);
    const std::uint32_t expected_vtable = img->so_base + p.driver_vtable;
    const std::uint32_t slot_off = p.publish_slot - p.driver_vtable;
    const std::uint32_t slot_value = rd32(img, vptr + slot_off);
    const std::uint32_t expected_thunk = img->so_base + p.publish_thunk;
    pvz2_log("[gate] %-22s vptr=0x%08x (expected AndroidAppDriver vtable 0x%08x, %s); "
                "slot +%u = 0x%08x (publish thunk 0x%08x, %s)\n",
                when, vptr, expected_vtable, vptr == expected_vtable ? "MATCH" : "mismatch",
                slot_off, slot_value, expected_thunk,
                slot_value == expected_thunk ? "MATCH" : "mismatch");
}

std::uint32_t make_guest_string(pvz2_elf_image_t *img, GuestRuntime *rt, const char *text) {
    const std::uint32_t ctor = sym().fn.string_ctor;
    if (ctor == 0) return 0;

    const std::uint32_t len = (std::uint32_t)std::strlen(text) + 1;
    const std::uint32_t cstr = rt->heap.alloc(len);
    const std::uint32_t dest = rt->heap.alloc(8);  /* the string object itself */
    const std::uint32_t alloc = rt->heap.alloc(8); /* dummy allocator argument */
    if (cstr == 0 || dest == 0 || alloc == 0) return 0;
    std::memcpy(&img->mem[cstr], text, len);
    runtime::call_guest_quiet(img, rt, ctor, dest, cstr, alloc);
    return dest;
}

std::string guess_callers(GuestCall &c, int max_frames) {
    pvz2_elf_image_t *img = c.rt->img;
    if (img == nullptr) return "";
    const std::uint32_t lo = img->so_base + img->text_vaddr, hi = lo + img->text_size;
    std::string out;
    int found = 0;
    const std::uint32_t sp = c.regs[13];
    for (std::uint32_t a = sp; a < sp + 4096 && found < max_frames; a += 4) {
        if (!c.in_bounds(a, 4)) break;
        const std::uint32_t w = c.read32(a);
        if (w < lo + 4 || w >= hi || (w & 1) || !c.in_bounds(w - 4, 4)) continue;
        const std::uint32_t prev = c.read32(w - 4);
        if ((prev & 0x0F000000) != 0x0B000000 && (prev & 0x0FFFFFF0) != 0x012FFF30) continue;
        char buf[24];
        std::snprintf(buf, sizeof(buf), " 0x%x", w - img->so_base);
        out += buf;
        ++found;
    }
    return out;
}

}  // namespace diagnostics
}  // namespace pvz2native
