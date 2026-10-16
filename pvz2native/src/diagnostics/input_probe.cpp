/* Where a touch actually lands in app space -- see diagnostics.h. */

#include <pvz2native/diagnostics/diagnostics.h>
#include <pvz2native/log.h>

#include <cstdio>
#include <cstring>

#include <pvz2native/game/symbols.h>
#include <pvz2native/runtime/dynarmic_config.h>
#include <pvz2native/surface.h>

namespace pvz2native {
namespace diagnostics {
namespace {

std::uint32_t read32(pvz2_elf_image_t *img, std::uint32_t addr) {
    std::uint32_t v = 0;
    std::memcpy(&v, &img->mem[addr], 4);
    return v;
}

}  // namespace

void dump_touch_scaler(pvz2_elf_image_t *img) {
    const GameSymbols &s = sym();
    if (s.global.app_driver == 0) return; /* not mapped for this version */

    const std::uint32_t app = read32(img, img->so_base + s.global.app_driver);
    if (app == 0) {
        /* Worth its own line even with no input offsets mapped: this pointer is
         * what every frame dereferences, so a null here means onDrawFrame has
         * been running on nothing. */
        pvz2_log("[input] app driver object is NULL -- frames have nothing to drive");
        return;
    }
    if (s.input.driver == 0) {
        pvz2_log("[input] app driver = 0x%08x (input field offsets not mapped for %s)",
                    app, s.version);
        return;
    }

    const std::uint32_t driver = read32(img, app + s.input.driver);
    const std::uint32_t scaler = driver != 0 ? read32(img, driver + s.input.scaler) : 0;

    /* The driver's own touch entry points, so they can be decompiled by name.
     * Dispatch is `(**(app+driver) + N)(driver, &touch)`, but which functions
     * those slots hold depends on the driver's concrete class, which only
     * exists at runtime. Printing them as .so offsets makes them greppable. */
    if (driver != 0 && s.input.vt_touch_down != 0) {
        const std::uint32_t vt = read32(img, driver);
        if (vt != 0) {
            pvz2_log("[input] driver=0x%08x vtable=0x%08x  TouchDown=sub_%X TouchUp=sub_%X "
                        "TouchMove=sub_%X",
                        driver, vt, read32(img, vt + s.input.vt_touch_down) - img->so_base,
                        read32(img, vt + s.input.vt_touch_up) - img->so_base,
                        read32(img, vt + s.input.vt_touch_move) - img->so_base);
        }
    }

    const std::uint32_t tb = read32(img, app + s.input.touch_begin);
    const std::uint32_t te = read32(img, app + s.input.touch_end);
    const std::uint32_t stride = s.input.touch_stride != 0 ? s.input.touch_stride : 1;
    pvz2_log("[input] app=0x%08x multitouch=%u active=%u touches=%d", app,
                img->mem[app + s.input.multitouch], img->mem[app + s.input.touch_active],
                (tb != 0 && te >= tb) ? (int)((te - tb) / stride) : -1);

    if (scaler == 0) {
        pvz2_log("[input] scaler is null -- no coordinate mapping at all");
        return;
    }
    std::int32_t f[8];
    for (int i = 0; i < 8; ++i) {
        std::memcpy(&f[i], &img->mem[scaler + s.input.scaler_fields + 4 * i], 4);
    }
    const bool skipped = (f[6] == 0 || f[7] == 0);
    pvz2_log("[input] scaler@0x%08x out_origin=(%d,%d) num=(%d,%d) in_origin=(%d,%d) "
                "den=(%d,%d)%s",
                scaler, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7],
                skipped ? "  <-- DIVISOR ZERO: transform SKIPPED, raw pixels used" : "");
    if (!skipped) {
        /* Where a click in the middle of the window actually lands. */
        const int mx = (int)pvz2_surface_width() / 2, my = (int)pvz2_surface_height() / 2;
        pvz2_log("[input] window centre (%d,%d) maps to app (%d,%d)", mx, my,
                    f[0] + (mx - f[4]) * f[2] / f[6], f[1] + (my - f[5]) * f[3] / f[7]);
    }
    std::fflush(stdout);
}

}  // namespace diagnostics
}  // namespace pvz2native
