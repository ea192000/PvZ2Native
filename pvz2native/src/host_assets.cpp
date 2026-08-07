/* Filesystem fallback for assets that Windows embeds via windows_resources.rc.
 * On Linux (and any non-RC build) the same numeric ids resolve to files under
 * <executable_dir>/assets/, matching the paths in the .rc (without the "../"). */

#include <pvz2native/host_assets.h>

#include <cstdio>
#include <cstring>

#include <SDL.h>

namespace {

struct RcAsset {
    int id;
    const char *relpath; /* under assets/ */
};

/* Keep in sync with windows_resources.rc. */
const RcAsset kRcAssets[] = {
    {101, "splash.bmp"},
    {102, "splash_bar_fill.bmp"},
    {103, "splash_bar_frame.bmp"},
    {200, "config/panel_bg.bmp"},
    {201, "config/list_bg.bmp"},
    {202, "config/list_item.bmp"},
    {203, "config/btn_close_up.bmp"},
    {204, "config/btn_close_down.bmp"},
    {205, "config/btn_back_up.bmp"},
    {206, "config/btn_back_down.bmp"},
    {207, "config/btn_arrow_up.bmp"},
    {208, "config/btn_arrow_down.bmp"},
    {209, "config/chk_checked.bmp"},
    {210, "config/chk_unchecked.bmp"},
    {211, "config/scroll_track.bmp"},
    {212, "config/scroll_bar.bmp"},
    {213, "config/font.otf"},
    {214, "config/check_click.wav"},
    {215, "config/next_click.wav"},
    {216, "config/close_click.wav"},
    {217, "config/back_click.wav"},
    {218, "config/btn_edit_up.bmp"},
    {219, "config/btn_edit_down.bmp"},
    {220, "config/btn_save_up.bmp"},
    {221, "config/btn_save_down.bmp"},
    {222, "config/txt_bind_input.bmp"},
    {223, "config/img_bind_container.bmp"},
    {224, "config/img_bind_preview.bmp"},
    {225, "config/point_green.bmp"},
    {226, "config/confirm_click.wav"},
    {227, "config/btn_exit_up.bmp"},
    {228, "config/btn_exit_down.bmp"},
    {229, "config/btn_cancel_up.bmp"},
    {230, "config/btn_cancel_down.bmp"},
    {231, "config/panel_exit_bg.bmp"},
    {232, "config/exit_click.wav"},
    {233, "config/slider_thumb.bmp"},
    {234, "config/slider_track_empty.bmp"},
    {235, "config/slider_track_full.bmp"},
    {236, "config/slider_click.wav"},
};

}  // namespace

extern "C" const char *pvz2_rc_asset_relpath(int rc_id) {
    for (const RcAsset &a : kRcAssets) {
        if (a.id == rc_id) return a.relpath;
    }
    return nullptr;
}

extern "C" void *pvz2_load_asset_bytes(int rc_id, int *out_size) {
    if (out_size) *out_size = 0;
    const char *rel = pvz2_rc_asset_relpath(rc_id);
    if (!rel) return nullptr;

    char *base = SDL_GetBasePath();
    if (!base) return nullptr;

    char path[1024];
    SDL_snprintf(path, sizeof(path), "%sassets/%s", base, rel);
    SDL_free(base);

    FILE *f = std::fopen(path, "rb");
    if (!f) return nullptr;
    if (std::fseek(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        return nullptr;
    }
    const long sz = std::ftell(f);
    if (sz <= 0) {
        std::fclose(f);
        return nullptr;
    }
    if (std::fseek(f, 0, SEEK_SET) != 0) {
        std::fclose(f);
        return nullptr;
    }

    void *buf = SDL_malloc((size_t)sz);
    if (!buf) {
        std::fclose(f);
        return nullptr;
    }
    const size_t n = std::fread(buf, 1, (size_t)sz, f);
    std::fclose(f);
    if (n != (size_t)sz) {
        SDL_free(buf);
        return nullptr;
    }
    if (out_size) *out_size = (int)sz;
    return buf;
}
