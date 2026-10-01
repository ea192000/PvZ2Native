#pragma once
/* Host path helpers: where the binary's install files live vs where portable
 * game data (obb/so/config/saves) live.
 *
 * Loose binary / Windows: both == SDL_GetBasePath() (folder of the .exe).
 * AppImage: install == mount (SDL_GetBasePath); data == dirname($APPIMAGE).
 *
 * Returned strings are owned like SDL_GetBasePath(): trailing separator, free
 * with SDL_free(). */

#ifdef __cplusplus
extern "C" {
#endif

/* config.ini, data.obb, libPVZ2.so, prefs.dat / save_dir defaults. */
char *pvz2_host_data_path(void);

/* UI assets under assets/ (Linux filesystem fallback for RC embeds). */
char *pvz2_host_install_path(void);

#ifdef __cplusplus
}
#endif
