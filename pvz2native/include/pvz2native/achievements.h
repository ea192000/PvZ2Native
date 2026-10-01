#pragma once
/* Local achievements: catalog, unlock popup toast, and scrollable list overlay.
 *
 * Persistence of completed Google Play service IDs lives here (prefs.dat key
 * shared with the previous google_play.cpp store). The config screen opens the
 * list overlay; unlock hooks call achievements_notify_service_unlocked().
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <SDL.h>

/* Record a newly unlocked achievement by Google Play service id. Returns 1 if
 * this was the first time (persisted + popup queued), 0 if already known. */
int achievements_notify_service_unlocked(const char *service_id);

/* Show the unlock popup for a catalog id (e.g. "iceberg"). Safe from any thread
 * that can touch prefs; GL work happens on the next achievements_draw(). */
void achievements_show_popup(const char *catalog_id);

/* Open/close the achievements list overlay (drawn above the config screen). */
void achievements_screen_request_open(void);
void achievements_screen_request_close(void);
int achievements_screen_is_open(void);

/* Non-zero while the list overlay is open OR a popup animation is active. */
int achievements_ui_is_active(void);

/* Forward SDL events while the list is open. Popup ignores input. */
void achievements_screen_handle_event(const SDL_Event *e);

/* Draw popup (always, if animating) and list overlay (if open). Call after the
 * config screen draw, before SDL_GL_SwapWindow. */
void achievements_draw(SDL_Window *window);

/* Returns non-zero if the Google Play service id was already marked completed. */
int achievements_is_service_completed(const char *service_id);

/* Clear the entire completed set (and persist). Used by Play_ResetAchievements. */
void achievements_reset_completed(void);

/* Release GL textures / font. Idempotent. */
void achievements_shutdown(void);

#ifdef __cplusplus
}
#endif
