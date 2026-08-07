#ifndef PVZ2NATIVE_HOST_ASSETS_H
#define PVZ2NATIVE_HOST_ASSETS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Map a windows_resources.rc RCDATA id to a path under assets/ (no leading
 * "assets/"). Returns NULL if the id is unknown. */
const char *pvz2_rc_asset_relpath(int rc_id);

/* Load the file for rc_id from <exe_dir>/assets/<relpath>.
 * On success returns an SDL_malloc buffer and sets *out_size; caller must
 * SDL_free(). Returns NULL on failure. */
void *pvz2_load_asset_bytes(int rc_id, int *out_size);

/* Release host UI resources that must die before SDL_Quit (config audio,
 * Linux color cursor, etc.). Safe to call more than once. */
void pvz2_host_pre_sdl_quit(void);

/* Ask the main loop to leave cleanly (session_end → SDL teardown). Prefer this
 * over exit(0) once the game has booted, so guest JIT threads can be joined. */
void pvz2_request_app_quit(void);

#ifdef __cplusplus
}
#endif

#endif /* PVZ2NATIVE_HOST_ASSETS_H */
