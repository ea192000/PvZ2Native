#ifndef PVZ2NATIVE_INPUT_INPUT_QUEUE_H
#define PVZ2NATIVE_INPUT_INPUT_QUEUE_H

#include <stdint.h>

/* Host input -> guest, through the one channel the engine actually has.
 *
 * PvZ2 takes ALL input (touch, keys, text) through a single Java call:
 * AndroidGameApp.UI_ProcessEvents(ByteBuffer). There are no Native_onKey*
 * entry points -- the binary even carries "Unimplemented Android
 * implementation of IAppDriver::KeyDown called - this is an error" -- and the
 * keyboard JNI surface is only ShowKeyboard/HideKeyboard/IsKeyboardShowing,
 * none of which returns text.
 *
 * So the host side queues events here and the UI_ProcessEvents hook serialises
 * them into the engine's buffer once per frame. Going through the JNI method
 * rather than patching the native input path is deliberate: hooks are keyed by
 * (class, method) name, which is stable across game versions, whereas .so
 * offsets are not.
 *
 * The pushers are C-callable because main.c's SDL loop is C. */

#ifdef __cplusplus
extern "C" {
#endif

/* Phases, matching the sub-type sub_9F0298 switches on at record +36.
 *
 * UP is 3 and MOVE is 1 -- NOT the other way round, however much the numbering
 * suggests it. Read out of the handlers themselves:
 *   0 -> sub_9E8208  driver vtable +432  appends the touch, sets app[221]=1
 *   1 -> sub_9E8378  driver vtable +440  overwrites the entry IN PLACE  => MOVE
 *   3 -> sub_9E8528  driver vtable +436  clears app[221] AND erases the
 *                                        entry from the touch vector => UP/END
 *   4 -> sub_9E8828  driver vtable +444  clears the whole vector      => CANCEL
 * The vtable order (down/up/move/cancel) is the giveaway; the dispatch-table
 * index order simply does not match it.
 *
 * Getting these two backwards is what made a click leave its button stuck in
 * the pressed state forever: the engine never saw a TouchUp, so the button
 * never fired, app[221] stayed 1 (dropping every later DOWN under single
 * touch), and the never-erased touches grew the vector without bound. */
#define PVZ2_TOUCH_DOWN 0
#define PVZ2_TOUCH_MOVE 1
#define PVZ2_TOUCH_UP 3
#define PVZ2_TOUCH_CANCEL 4

/* Android keycodes. These are the only ones the engine's CharToKeyCode maps to
 * something non-zero (66->13, 67->8, 82->18, 4->241); every other keycode
 * becomes 0 and is discarded, which is why printable characters have to arrive
 * as text events instead of key events. */
#define PVZ2_KEY_BACK 4
#define PVZ2_KEY_ENTER 66
#define PVZ2_KEY_DEL 67
#define PVZ2_KEY_MENU 82

/* Pointer position in window pixels; the engine rescales to app space itself
 * (sub_9D9390, integer arithmetic -- these really are pixels, not floats). */
void pvz2_input_push_touch(int phase, int x, int y);

void pvz2_input_push_key(int android_keycode, int down);

/* UTF-8, as SDL_TEXTINPUT delivers it. */
void pvz2_input_push_text(const char *utf8);

/* Host-side safety reset for stuck touch sequences: emits a synthetic
 * TOUCH_CANCEL and rotates the active touch id so future events cannot match a
 * stale in-guest capture. */
void pvz2_input_force_touch_reset(void);

/* Non-zero while the engine has asked for the soft keyboard, i.e. between
 * Device_ShowKeyboard and Device_HideKeyboard. main.c uses it to drive
 * SDL_StartTextInput/SDL_StopTextInput. */
int pvz2_input_keyboard_wanted(void);

/* Set by the Device_*Keyboard hooks in src/dex/hooks/android_game_app.cpp. */
void pvz2_input_set_keyboard_wanted(int wanted);

/* Incremented every time the keyboard-visible state is explicitly refreshed by
 * the guest, even if the value stays the same (for example clicking from one
 * text field into another). */
uint32_t pvz2_input_keyboard_wanted_generation(void);

#ifdef __cplusplus
}  /* extern "C" */

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pvz2native {
namespace input {

/* Serialises queued events into `out` as the exact bytes the engine expects to
 * find in its ByteBuffer, and returns how many events were written.
 *
 * Stops before exceeding `capacity`; anything that does not fit stays queued
 * for the next frame rather than being dropped, so a burst of typing cannot
 * lose characters. The caller copies `out` into guest memory -- this module
 * deliberately knows nothing about the guest address space. */
std::size_t drain(std::vector<std::uint8_t> &out, std::size_t capacity);

}  // namespace input
}  // namespace pvz2native
#endif

#endif
