#ifndef PVZ2NATIVE_INPUT_GAMEPAD_H
#define PVZ2NATIVE_INPUT_GAMEPAD_H

#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Host gamepad support (SDL_GameController). All logic lives in gamepad.cpp;
 * main.c only calls these entry points.
 *
 * Safe with no controller: init/open failures are non-fatal.
 * Runs in parallel with mouse and keyboard. */

/* use_43: 1 if launch aspect latched as 4:3 (same latch as keyboard shortcuts). */
void pvz2_gamepad_init(SDL_Window *window, int use_43);
void pvz2_gamepad_shutdown(void);

/* Rebuild focus points / binds from the live config (e.g. after reload). */
void pvz2_gamepad_reload(void);

/* Returns 1 if the event was consumed by the gamepad module. */
int pvz2_gamepad_handle_event(const SDL_Event *e);

/* Per-frame: left stick, D-pad repeat, right-stick drag, trigger edges. */
void pvz2_gamepad_tick(float dt_sec);

#ifdef __cplusplus
}
#endif

#endif
