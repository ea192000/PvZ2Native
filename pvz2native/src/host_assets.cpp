/* Filesystem fallback for assets that Windows embeds via windows_resources.rc.
 * On Linux (and any non-RC build) the same numeric ids resolve to files under
 * <executable_dir>/assets/, matching the paths in the .rc (without the "../"). */

#include <pvz2native/host_assets.h>
#include <pvz2native/host_paths.h>

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
    {237, "config/achievement_popup.bmp"},
    {238, "config/achievement_menu.bmp"},
    {239, "config/achievement_item.bmp"},
    {240, "config/achievement_medal.bmp"},
    {241, "achievements/achievement_locked.bmp"},
    {242, "achievements/achievement_iceberg.bmp"},
    {243, "achievements/achievement_dog_whisperer.bmp"},
    {244, "achievements/achievement_broadside.bmp"},
    {245, "achievements/achievement_dragon_age.bmp"},
    {246, "achievements/achievement_fried_chicken.bmp"},
    {247, "achievements/achievement_giddyup.bmp"},
    {248, "achievements/achievement_high_five.bmp"},
    {249, "achievements/achievement_king_nut.bmp"},
    {250, "achievements/achievement_magical_fruit.bmp"},
    {251, "achievements/achievement_mutiny.bmp"},
    {252, "achievements/achievement_no_smoking.bmp"},
    {253, "achievements/achievement_egypt_stars.bmp"},
    {254, "achievements/achievement_off_the_green.bmp"},
    {255, "achievements/achievement_pat_the_birdy.bmp"},
    {256, "achievements/achievement_pirate_stars.bmp"},
    {257, "achievements/achievement_pod_squad.bmp"},
    {258, "achievements/achievement_sasquash.bmp"},
    {259, "achievements/achievement_shell_on_wheels.bmp"},
    {260, "achievements/achievement_spudow.bmp"},
    {261, "achievements/achievement_sun_catcher.bmp"},
    {262, "achievements/achievement_yarr_matey.bmp"},
    {263, "achievements/achievement_cowboy_stars.bmp"},
    {264, "achievements/achievement_prime_mover.bmp"},
    {265, "achievements/achievement_drop_the_boss.bmp"},
    {266, "achievements/achievement_the_jettisons.bmp"},
    {267, "achievements/achievement_re_re_spawn.bmp"},
    {268, "achievements/achievement_sun_damaged.bmp"},
    {269, "achievements/achievement_grass_effect.bmp"},
    {270, "achievements/achievement_tile_denial.bmp"},
    {271, "achievements/achievement_hammer_time.bmp"},
    {272, "achievements/achievement_fungus_humongous.bmp"},
    {273, "achievements/achievement_all_in_jest.bmp"},
    {274, "achievements/achievement_out_of_darkness.bmp"},
    {275, "achievements/achievement_not_baaad.bmp"},
    {276, "achievements/achievement_your_hat_sir.bmp"},
    {277, "achievements/achievement_egypt_vasebreaker_pack.bmp"},
    {278, "achievements/achievement_egypt_vasebreaker_endless_30.bmp"},
    {279, "achievements/achievement_cowboy_vasebreaker_pack.bmp"},
    {280, "achievements/achievement_pirate_vasebreaker_pack.bmp"},
    {281, "achievements/achievement_undertow.bmp"},
    {282, "achievements/achievement_ten_pin_kapow.bmp"},
    {283, "achievements/achievement_big_kahunka.bmp"},
    {284, "achievements/achievement_fly_fishing_fail.bmp"},
    {285, "achievements/achievement_banana_storm.bmp"},
    {286, "achievements/achievement_wave_goodbye.bmp"},
    {287, "achievements/achievement_knock_em_dead.bmp"},
    {288, "achievements/achievement_so_thawful.bmp"},
    {289, "achievements/achievement_barrage_a_trois.bmp"},
    {290, "achievements/achievement_ice_blockade.bmp"},
    {291, "achievements/achievement_run_weasley.bmp"},
    {292, "achievements/achievement_meltdown.bmp"},
    {293, "achievements/achievement_unwelcome_mat.bmp"},
    {294, "achievements/achievement_shovel_off.bmp"},
    {295, "achievements/achievement_porter_thwarter.bmp"},
    {296, "achievements/achievement_spawn_camping.bmp"},
    {297, "achievements/achievement_ragweeds_to_riches.bmp"},
    {298, "achievements/achievement_lost_and_found.bmp"},
    {299, "achievements/achievement_volume_goes_to_sixteen.bmp"},
    {300, "achievements/achievement_beet_it.bmp"},
    {301, "achievements/achievement_head_banger.bmp"},
    {302, "achievements/achievement_breaking_it_off.bmp"},
    {303, "achievements/achievement_back_from_the_future_3.bmp"},
    {304, "achievements/achievement_drop_the_mic.bmp"},
    {305, "achievements/achievement_dinos_sore.bmp"},
    {306, "achievements/achievement_terror_redactyled.bmp"},
    {307, "achievements/achievement_martial_marsh_masher.bmp"},
    {308, "achievements/achievement_hungry_hungry_dinos.bmp"},
    {309, "achievements/achievement_bash_potato.bmp"},
    {310, "achievements/achievement_meteor_right.bmp"},
    {311, "achievements/achievement_hot_off_the_presses.bmp"},
    {312, "achievements/achievement_time_traveler.bmp"},
    {313, "achievements/achievement_great_gargling_gargantuars.bmp"},
    {314, "achievements/achievement_youre_an_all_star.bmp"},
    {315, "achievements/achievement_the_shadow_family.bmp"},
    {316, "achievements/achievement_present_perfect.bmp"},
    {317, "config/achievement_unlock.wav"},
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

    char *base = pvz2_host_install_path();
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
