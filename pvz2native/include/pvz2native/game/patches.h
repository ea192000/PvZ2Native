#ifndef PVZ2NATIVE_GAME_PATCHES_H
#define PVZ2NATIVE_GAME_PATCHES_H

#include <pvz2native/elf32/elf32_loader.h>

namespace pvz2native {

/* Instructions the port rewrites in the loaded image before any guest code runs.
 *
 * This is a last resort: the engine asks Android whether the device is online
 * (AndroidHttpProxy.GetNetworkStatus / sub_247084) and uses the SAME answer for
 * unrelated decisions:
 *
 *   - "should I wait for downloads?" -- the loading screen. Answer "online" and
 *     the boot never finishes (resumable / PvZ2Web / snapshot waits).
 *   - "may a purchase start at all?" -- the purchase broker; offline ->
 *     "Service Unavailable".
 *   - "may LoD / CDN schedule apply?" -- Level of the Day; offline at apply
 *     time skips map events even when lod_config was served from the OBB.
 *
 * One JNI answer cannot satisfy all three. Rewriting the specific BL call sites
 * (purchase broker + LoD/CDN apply) forces those paths "online" while boot still
 * hears "offline" via GetNetworkStatus.
 *
 * Separately on 4.5.2, LoD activation itself is broken even when online: the
 * path after the LevelOfTheDay toggle BL's three consecutive BX LR stubs
 * (sub_40F440) instead of the real apply (sub_3487AC). That call site is
 * rewritten to the real apply, and offline property-gate BNEs are NOP'd.
 *
 * Each patched site is verified to be the instruction we expect before anything
 * is written, and the addresses are per-version and live in game/symbols.cpp
 * like every other .so address.
 */

/* Applies the patches for the detected version. Call once, after
 * game_symbols_detect and before the first guest instruction executes (dynarmic
 * caches translated blocks, so a later write would be ignored by any code
 * already run). Silent when the version maps no patches or when the emulated
 * store is off; loud about anything it declines to patch. */
void game_apply_patches(pvz2_elf_image_t *img);

/* SVC probes around LoD apply (sub_3487AC), schedule match (sub_40F44C), and
 * the Active-UID writer (sub_69F8DC). Call once after
 * runtime::set_session_image / build_import_handler_cache and before any guest
 * code runs -- it needs make_guest_callback(). Logs r0-r3, caller LR, host time,
 * and return so we can see where the apply chain dies before UID write. */
void game_install_lod_apply_probe(pvz2_elf_image_t *img);

}  // namespace pvz2native

#endif
