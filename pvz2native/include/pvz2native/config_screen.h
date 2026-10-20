#pragma once
/* Config screen -- in-game settings overlay + quit-confirmation dialog.
 *
 * Activated by the Google Play button (Play_Connect hook).
 * While open (settings or quit confirm), main.c must suppress all
 * touch/mouse events to the game.
 *
 * Lifecycle (called from main.c):
 *   config_screen_open()         -- open the settings screen (idempotent if already open)
 *   config_screen_request_open() -- deferred open (no window pointer)
 *   config_screen_request_quit_confirm() -- show quit dialog (ESC / Exit Program);
 *                                          if already showing, dismisses it
 *   config_screen_request_escape() -- Escape key / controller Back: nothing open -> settings (plays exit_click);
 *                                    settings open -> quit dialog; quit dialog open -> dismiss it;
 *                                    achievements -> close them
 *   config_screen_is_open()      -- non-zero while settings OR quit confirm is visible
 *   config_screen_handle_event() -- forward SDL events while open
 *   config_screen_draw()         -- render the overlay (call after game draw, before SDL_GL_SwapWindow)
 *
 * On settings close: if any setting changed, config.ini is rewritten. Most
 * options apply live (FPS, resolution, currency, IAP, binds). A process
 * relaunch only happens when the game locale, free purchases or an Events
 * switch changes, because the engine reads those at boot.
 *
 * Window close (SDL_QUIT / title-bar X) bypasses the quit dialog and exits
 * immediately from main.c.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <SDL.h>

/* Open the config screen for the given window. Safe to call when already open. */
void config_screen_open(SDL_Window *window);

/* Request the config screen to open on the next frame (no window pointer needed).
 * The window is taken from the draw call. Useful when called from contexts that
 * do not have access to the SDL_Window pointer (e.g. dex hooks). */
void config_screen_request_open(void);

/* Show the quit-confirmation panel. If it is already visible, dismiss it
 * (so a second Escape cancels). Safe to call with no window pointer; resources
 * load on the next draw. */
void config_screen_request_quit_confirm(void);

/* Escape key handling (see the lifecycle notes above). */
void config_screen_request_escape(void);

/* Tell the overlay that the game session has finished booting. Releases any
 * temporary UI audio device opened during the splash so the game's OpenSL→SDL
 * sink can claim the output cleanly. */
void config_screen_notify_booted(void);

/* Close UI audio (and related samples) before SDL_Quit. Idempotent. */
void config_screen_shutdown(void);

/* Returns non-zero while the settings screen OR the quit-confirm dialog is visible. */
int config_screen_is_open(void);

/* Forward an SDL_Event to the config screen. Call this instead of game input
 * processing while config_screen_is_open() is non-zero. */
void config_screen_handle_event(const SDL_Event *e);

/* Render the config screen overlay. Must be called each frame while open,
 * after the game has rendered its frame but before SDL_GL_SwapWindow. */
void config_screen_draw(SDL_Window *window);

/* Shared image utility: set R=255,G=0,B=255 pixels to fully transparent.
 * Surface must be in SDL_PIXELFORMAT_RGBA32. */
void apply_magenta_colorkey(SDL_Surface *surface);

/* Play the same SFX used when opening the quit-confirm dialog (exit_click).
 * Used by the achievements screen open so they share that cue for now. */
void config_screen_play_exit_sound(void);

/* Play achievement_unlock.wav (also refreshes SfxVolume/Muted from prefs). */
void config_screen_play_achievement_unlock_sound(void);

#ifdef __cplusplus
}
#endif
