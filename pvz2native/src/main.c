#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include <glad/gl.h>
#include <pvz2native/config.h>
#ifdef _WIN32
#include <windows.h>
#include <SDL_syswm.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <unistd.h>
#endif
#include <pvz2native/gfx/frame_limiter.h>
#include <pvz2native/gfx/gl_requirements.h>
#include <pvz2native/gfx/video_mode.h>
#include <pvz2native/input/input_queue.h>
#include <pvz2native/pvz2_session.h>
#include <pvz2native/surface.h>
#include <pvz2native/config_screen.h>
#include <pvz2native/achievements.h>
#include <pvz2native/host_assets.h>
#include <pvz2native/host_paths.h>
#include <pvz2native/input/gamepad.h>
#include <pvz2native/log.h>
#include <pvz2native/save_dates.h>

/* Set once the window exists, so input mapping can use logical window coords. */
static SDL_Window *g_window = NULL;

/* Cached window/drawable metrics for hot input paths (mouse events). */
static int g_cached_ww = 0, g_cached_wh = 0;
static int g_cached_dw = 0, g_cached_dh = 0;

/* Resize throttling: coalesce bursts of window-size events into fewer guest
 * resize calls, while still keeping input mapping up to date every event. */
static int g_resize_pending = 0;
static Uint32 g_last_resize_apply_ms = 0;
static const Uint32 kResizeApplyMinMs = 16;
static const int kWheelScrollTouchRepeats = 6;
static const int kWheelScrollTouchBorderPad = 10;

typedef struct wheel_scroll_state {
    int active;
    int step;
    int center_x;
    int start_y;
    int end_y;
} wheel_scroll_state_t;

static wheel_scroll_state_t g_wheel_scroll = {0};

static void refresh_window_metrics(void) {
    if (!g_window) return;
    SDL_GetWindowSize(g_window, &g_cached_ww, &g_cached_wh);
    SDL_GL_GetDrawableSize(g_window, &g_cached_dw, &g_cached_dh);
}

/* Mouse events should be interpreted in the same coordinate space SDL reports
 * for that moment. In fullscreen/DPI scenarios F11 can switch between logical
 * window units and pixel units; detect that per-event and map into frozen touch
 * space with aspect-fit conversion. */
static int map_window_to_touch(int wx, int wy, int *tx, int *ty) {
    int ww = g_cached_ww;
    int wh = g_cached_wh;
    int dw = g_cached_dw;
    int dh = g_cached_dh;

    if ((ww <= 0 || wh <= 0 || dw <= 0 || dh <= 0) && g_window) {
        refresh_window_metrics();
        ww = g_cached_ww;
        wh = g_cached_wh;
        dw = g_cached_dw;
        dh = g_cached_dh;
    }

    if (ww <= 0 || wh <= 0) {
        ww = (int)pvz2_surface_width();
        wh = (int)pvz2_surface_height();
    }
    if (dw <= 0 || dh <= 0) {
        dw = (int)pvz2_surface_width();
        dh = (int)pvz2_surface_height();
    }

    int low_w = ww;
    int low_h = wh;
    int high_w = dw;
    int high_h = dh;
    if ((long long)dw * (long long)dh < (long long)ww * (long long)wh) {
        low_w = dw;
        low_h = dh;
        high_w = ww;
        high_h = wh;
    }

    /* Por defecto usamos el espacio más pequeño (normalmente coordenadas
     * lógicas del evento). Si el evento cae fuera, cambiamos al mayor. */
    int src_w = low_w;
    int src_h = low_h;
    if ((wx >= src_w || wy >= src_h) && high_w > 0 && high_h > 0) {
        src_w = high_w;
        src_h = high_h;
    }

    const float tw = (float)pvz2_touch_space_width();
    const float th = (float)pvz2_touch_space_height();
    if (src_w <= 0 || src_h <= 0 || tw <= 0.0f || th <= 0.0f) return 0;

    if (wx < 0 || wy < 0 || wx >= src_w || wy >= src_h) return 0;

    /* La imagen del juego se estira para ocupar todo el framebuffer actual.
     * Por eso el mapeo correcto es por ejes independientes (sin letterbox). */
    const float gx = ((float)wx / (float)(src_w - 1)) * (tw - 1.0f);
    const float gy = ((float)wy / (float)(src_h - 1)) * (th - 1.0f);

    *tx = (int)(gx + 0.5f);
    *ty = (int)(gy + 0.5f);
    return 1;
}

typedef struct runtime_shortcut {
    SDL_Scancode scancode;
    float x;
    float y;
    char name[64];
} runtime_shortcut_t;

static runtime_shortcut_t g_shortcuts[PVZ2_MAX_SHORTCUTS];
static int g_shortcut_count = 0;
static int g_text_logs_enabled = 1;

/* Aspect bucket for binds is latched once from the launch window size.
 * The game's HUD layout is chosen at process start and does not follow later
 * maximize/resize, so we must not re-pick 16:9 vs 4:3 after that. */
static int g_bind_aspect_latched = 0;
static int g_bind_use_43 = 0; /* 1 = prefer 4:3 coords when a bind has them */

/* Midpoint between 4:3 (~1.333) and 16:9 (~1.778). */
static const float kBindAspect43Max = 1.5f;

static void latch_bind_aspect_from_size(int w, int h) {
    if (w <= 0 || h <= 0) return;
    g_bind_use_43 = ((float)w / (float)h) < kBindAspect43Max ? 1 : 0;
    g_bind_aspect_latched = 1;
}

/* Keep host-side touch state so we can always emit a matching UP if focus is
 * lost (e.g. click on another monitor while holding/dragging). */
static int g_mouse_touch_active = 0;
static int g_mouse_touch_x = 0;
static int g_mouse_touch_y = 0;
static Uint32 g_last_touch_input_ms = 0;
static Uint32 g_last_forced_touch_reset_ms = 0;
static const Uint32 kTouchResetIdleMs = 10000;
static int g_pending_keyboard_hide = 0;
static uint32_t g_pending_keyboard_hide_generation = 0;
static int g_booted;

static void release_active_mouse_touch(void) {
    if (!g_mouse_touch_active) return;
    pvz2_input_push_touch(PVZ2_TOUCH_UP, g_mouse_touch_x, g_mouse_touch_y);
    g_last_touch_input_ms = SDL_GetTicks();
    g_mouse_touch_active = 0;
}

static void hard_reset_touch_state(void) {
    pvz2_input_force_touch_reset();
    g_mouse_touch_active = 0;
}

static void mark_touch_activity(void) { g_last_touch_input_ms = SDL_GetTicks(); }

static void arm_pending_keyboard_hide(void) {
    if (!pvz2_input_keyboard_wanted()) return;
    g_pending_keyboard_hide = 1;
    g_pending_keyboard_hide_generation = pvz2_input_keyboard_wanted_generation();
}

static void maybe_finish_pending_keyboard_hide(void) {
    if (!g_pending_keyboard_hide) return;
    g_pending_keyboard_hide = 0;

    if (pvz2_input_keyboard_wanted_generation() != g_pending_keyboard_hide_generation) return;
    if (!pvz2_input_keyboard_wanted()) return;

    pvz2_input_set_keyboard_wanted(0);
    pvz2_log("[input] host dismissed keyboard after click outside text mode");
}

static void push_mouse_wheel_scroll(int wx, int wy, int wheel_y) {
    if (wheel_y == 0) return;
    if (g_mouse_touch_active) return;

    if (g_wheel_scroll.active) {
        return;
    }

    int tx = 0;
    int ignored_ty = 0;
    if (!map_window_to_touch(wx, wy, &tx, &ignored_ty)) return;

    const int touch_h = (int)pvz2_touch_space_height();
    const int touch_w = (int)pvz2_touch_space_width();
    if (touch_h <= 1 || touch_w <= 1) return;

    int min_x = kWheelScrollTouchBorderPad;
    int max_x = touch_w - 1 - kWheelScrollTouchBorderPad;
    int min_y = kWheelScrollTouchBorderPad;
    int max_y = touch_h - 1 - kWheelScrollTouchBorderPad;

    if (min_x > max_x) {
        min_x = 0;
        max_x = touch_w - 1;
    }

    if (min_y > max_y) {
        min_y = 0;
        max_y = touch_h - 1;
    }

    int center_x = tx;
    if (center_x < min_x) center_x = min_x;
    if (center_x > max_x) center_x = max_x;

    // Posiciones del gesto expresadas como porcentaje de la altura.
    int scroll_down_start_y = (touch_h * 50) / 100;
    int scroll_up_start_y = (touch_h * 50) / 100;
    int scroll_top_end_y = (touch_h * 20) / 100;
    int scroll_bottom_end_y = (touch_h * 80) / 100;

    int start_y = wheel_y > 0 ? scroll_down_start_y : scroll_up_start_y;
    int end_y = wheel_y > 0 ? scroll_bottom_end_y : scroll_top_end_y;

    if (start_y < min_y) start_y = min_y;
    if (start_y > max_y) start_y = max_y;
    if (end_y < min_y) end_y = min_y;
    if (end_y > max_y) end_y = max_y;

    if (start_y == end_y) return;

    g_wheel_scroll.active = 1;
    g_wheel_scroll.step = 0;
    g_wheel_scroll.center_x = center_x;
    g_wheel_scroll.start_y = start_y;
    g_wheel_scroll.end_y = end_y;
}

static void process_mouse_wheel_scroll(void) {
    if (!g_wheel_scroll.active) return;
    if (g_mouse_touch_active) {
        g_wheel_scroll.active = 0;
        g_wheel_scroll.step = 0;
        return;
    }

    if (g_wheel_scroll.step == 0) {
        pvz2_input_push_touch(PVZ2_TOUCH_DOWN, g_wheel_scroll.center_x, g_wheel_scroll.start_y);

        g_mouse_touch_x = g_wheel_scroll.center_x;
        g_mouse_touch_y = g_wheel_scroll.start_y;
    } else if (g_wheel_scroll.step <= kWheelScrollTouchRepeats) {
        int current_y =
            g_wheel_scroll.start_y +
            ((g_wheel_scroll.end_y - g_wheel_scroll.start_y) * g_wheel_scroll.step) /
                kWheelScrollTouchRepeats;

        pvz2_input_push_touch(PVZ2_TOUCH_MOVE, g_wheel_scroll.center_x, current_y);

        g_mouse_touch_x = g_wheel_scroll.center_x;
        g_mouse_touch_y = current_y;
    } else {
        pvz2_input_push_touch(PVZ2_TOUCH_UP, g_wheel_scroll.center_x, g_wheel_scroll.end_y);

        g_mouse_touch_x = g_wheel_scroll.center_x;
        g_mouse_touch_y = g_wheel_scroll.end_y;

        g_wheel_scroll.active = 0;
        g_wheel_scroll.step = 0;

        mark_touch_activity();
        return;
    }

    mark_touch_activity();
    g_wheel_scroll.step++;
}

static void maybe_force_touch_recovery(void) {
    if (!g_booted) return;
    const Uint32 now = SDL_GetTicks();
    if (g_last_touch_input_ms == 0) {
        g_last_touch_input_ms = now;
        return;
    }
    if (!g_mouse_touch_active) return;
    if ((now - g_last_touch_input_ms) < kTouchResetIdleMs) return;
    if ((now - g_last_forced_touch_reset_ms) < kTouchResetIdleMs) return;

    hard_reset_touch_state();
    g_last_forced_touch_reset_ms = now;
    g_last_touch_input_ms = now;
}

static SDL_Scancode shortcut_scancode_from_name(const char *key) {
    if (!key || key[0] == '\0') return SDL_SCANCODE_UNKNOWN;
    if (SDL_strcasecmp(key, "ENTER") == 0) return SDL_SCANCODE_RETURN;
    if (SDL_strcasecmp(key, "RETURN") == 0) return SDL_SCANCODE_RETURN;
    if (SDL_strcasecmp(key, "KP_ENTER") == 0) return SDL_SCANCODE_KP_ENTER;
    if (SDL_strcasecmp(key, "SPACE") == 0) return SDL_SCANCODE_SPACE;
    return SDL_GetScancodeFromName(key);
}

static void load_runtime_shortcuts(const pvz2_config_t *cfg) {
    g_shortcut_count = 0;
    if (!cfg) return;

    /* If we have not latched yet (early boot), prefer configured video size,
     * then fall back to 16:9. A later call with the real launch size re-latches
     * before gameplay. */
    if (!g_bind_aspect_latched) {
        if (cfg->video_width > 0 && cfg->video_height > 0)
            latch_bind_aspect_from_size(cfg->video_width, cfg->video_height);
        else
            g_bind_use_43 = 0;
    }

    for (unsigned i = 0; i < cfg->shortcut_count && g_shortcut_count < PVZ2_MAX_SHORTCUTS; ++i) {
        const pvz2_shortcut_binding_t *in = &cfg->shortcuts[i];
        SDL_Scancode sc = shortcut_scancode_from_name(in->key);
        if (sc == SDL_SCANCODE_UNKNOWN) {
            pvz2_log("[shortcut] ignored: bind_%s (unknown key '%s')", in->name, in->key);
            continue;
        }
        runtime_shortcut_t *out = &g_shortcuts[g_shortcut_count++];
        out->scancode = sc;
        if (g_bind_use_43 && in->has_43) {
            out->x = in->x43;
            out->y = in->y43;
        } else {
            out->x = in->x;
            out->y = in->y;
        }
        SDL_snprintf(out->name, sizeof(out->name), "%s", in->name);
        pvz2_log("[shortcut] bind_%s: key=%s (scancode=%d) -> (%.2f,%.2f)%s",
                 out->name, in->key, (int)sc, out->x, out->y,
                 (g_bind_use_43 && in->has_43) ? " [4:3]" : " [16:9]");
    }
}

/* Reload the runtime shortcut table from the current live config.
 * Called after pvz2_config_update_bind_key so changes take effect immediately. */
void pvz2_reload_shortcuts(void) {
    load_runtime_shortcuts(pvz2_config());
}

/* Turns one SDL event into a guest input event.
 *
 * The engine has exactly one input channel -- UI_ProcessEvents -- so
 * everything funnels through the queue and is serialised there once per frame.
 *
 * Only ENTER and BACKSPACE are sent as key events, because the engine's
 * CharToKeyCode maps every other keycode to 0 and drops it; printable
 * characters have to arrive as SDL_TEXTINPUT text instead. ESCAPE is
 * deliberately NOT mapped to Android's BACK key: it already means "quit" here,
 * and having one key do both would make the window impossible to close once
 * the game starts consuming input. */
static void feed_input_event(const SDL_Event *e) {
    switch (e->type) {
        case SDL_MOUSEBUTTONDOWN:
            if (e->button.button == SDL_BUTTON_LEFT) {
                arm_pending_keyboard_hide();
                int tx = 0, ty = 0;
                if (map_window_to_touch(e->button.x, e->button.y, &tx, &ty)) {
                    pvz2_input_push_touch(PVZ2_TOUCH_DOWN, tx, ty);
                    g_mouse_touch_active = 1;
                    g_mouse_touch_x = tx;
                    g_mouse_touch_y = ty;
                    mark_touch_activity();
                }
            } else if (e->button.button == SDL_BUTTON_RIGHT) {
                hard_reset_touch_state();
                pvz2_input_push_key(PVZ2_KEY_BACK, 1);
                mark_touch_activity();
            } else if (e->button.button == SDL_BUTTON_MIDDLE) {
                hard_reset_touch_state();
                pvz2_input_push_key(PVZ2_KEY_MENU, 1);
                mark_touch_activity();
            }
            break;
        case SDL_MOUSEBUTTONUP:
            if (e->button.button == SDL_BUTTON_LEFT) {
                int tx = 0, ty = 0;
                if (map_window_to_touch(e->button.x, e->button.y, &tx, &ty)) {
                    pvz2_input_push_touch(PVZ2_TOUCH_UP, tx, ty);
                    g_mouse_touch_x = tx;
                    g_mouse_touch_y = ty;
                    mark_touch_activity();
                } else if (g_mouse_touch_active) {
                    pvz2_input_push_touch(PVZ2_TOUCH_UP, g_mouse_touch_x, g_mouse_touch_y);
                    mark_touch_activity();
                } else {
                    hard_reset_touch_state();
                    mark_touch_activity();
                }
                g_mouse_touch_active = 0;
            } else if (e->button.button == SDL_BUTTON_RIGHT) {
                pvz2_input_push_key(PVZ2_KEY_BACK, 0);
                mark_touch_activity();
            } else if (e->button.button == SDL_BUTTON_MIDDLE) {
                pvz2_input_push_key(PVZ2_KEY_MENU, 0);
                mark_touch_activity();
            }
            break;
        case SDL_MOUSEMOTION:
            /* A mouse with no button held has no touch equivalent; sending it
             * would look to the engine like a finger dragging across the
             * screen at all times. */
            if ((e->motion.state & SDL_BUTTON_LMASK) && g_mouse_touch_active) {
                int tx = 0, ty = 0;
                if (map_window_to_touch(e->motion.x, e->motion.y, &tx, &ty)) {
                    pvz2_input_push_touch(PVZ2_TOUCH_MOVE, tx, ty);
                    g_mouse_touch_x = tx;
                    g_mouse_touch_y = ty;
                    mark_touch_activity();
                }
            }
            break;
        case SDL_MOUSEWHEEL: {
            int mx = 0, my = 0;
            SDL_GetMouseState(&mx, &my);
            push_mouse_wheel_scroll(mx, my, e->wheel.y);
            break;
        }
        case SDL_TEXTINPUT:
            pvz2_input_push_text(e->text.text);
            break;
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            int code = 0;
            if (e->key.keysym.sym == SDLK_RETURN || e->key.keysym.sym == SDLK_KP_ENTER) {
                code = PVZ2_KEY_ENTER;
            } else if (e->key.keysym.sym == SDLK_BACKSPACE) {
                code = PVZ2_KEY_DEL;
            }
            if (code != 0) pvz2_input_push_key(code, e->type == SDL_KEYDOWN ? 1 : 0);
            break;
        }
        default:
            break;
    }
}

/* SDL only delivers SDL_TEXTINPUT while text input is active, so this is what
 * makes typing reach the game -- driven by the engine's own
 * Device_ShowKeyboard/Device_HideKeyboard calls. */
static void sync_text_input(void) {
    int wanted = pvz2_input_keyboard_wanted();
    SDL_bool active = SDL_IsTextInputActive();
    if (wanted && !active) {
        SDL_StartTextInput();
    } else if (!wanted && active) {
        SDL_StopTextInput();
    }
}

static int g_quit_requested = 0;

#ifdef _WIN32
static HICON g_app_icon = NULL;
static HCURSOR g_app_cursor = NULL;
static int g_app_cursor_w = 0;
static int g_app_cursor_h = 0;
static int g_app_cursor_base_w = 0;
static int g_app_cursor_base_h = 0;
static WNDPROC g_prev_wndproc = NULL;

static void open_debug_console(void) {
    if (GetConsoleWindow() == NULL && AllocConsole()) {
        FILE *stream = NULL;
        freopen_s(&stream, "CONOUT$", "w", stdout);
        freopen_s(&stream, "CONOUT$", "w", stderr);
        freopen_s(&stream, "CONIN$", "r", stdin);
    }
    HWND cw = GetConsoleWindow();
    if (cw) ShowWindow(cw, SW_SHOW);
}

static LRESULT CALLBACK app_wndproc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param) {
    if (msg == WM_SETCURSOR && g_app_cursor && LOWORD(l_param) == HTCLIENT) {
        SetCursor(g_app_cursor);
        return TRUE;
    }
    if (g_prev_wndproc) return CallWindowProc(g_prev_wndproc, hwnd, msg, w_param, l_param);
    return DefWindowProc(hwnd, msg, w_param, l_param);
}

static int cursor_size_from_handle(HCURSOR cursor, int *w, int *h) {
    if (!cursor || !w || !h) return 0;

    ICONINFO ii;
    if (!GetIconInfo((HICON)cursor, &ii)) return 0;

    BITMAP bm = {0};
    int ok = 0;
    if (ii.hbmColor && GetObject(ii.hbmColor, sizeof(bm), &bm) == sizeof(bm)) {
        *w = bm.bmWidth;
        *h = bm.bmHeight;
        ok = 1;
    } else if (ii.hbmMask && GetObject(ii.hbmMask, sizeof(bm), &bm) == sizeof(bm)) {
        *w = bm.bmWidth;
        *h = bm.bmHeight / 2;
        ok = 1;
    }

    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return ok;
}

static void apply_console_visibility(int show_console) {
    if (show_console) {
        open_debug_console();
    }
}

static int prepare_log_console(void) {
    if (GetConsoleWindow() != NULL || !AllocConsole()) return 0;

    FILE *stream = NULL;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
    freopen_s(&stream, "CONIN$", "r", stdin);
    return 1;
}

static void apply_windows_assets(SDL_Window *window) {
    if (!window) return;

    SDL_SysWMinfo wm;
    SDL_VERSION(&wm.version);
    if (SDL_GetWindowWMInfo(window, &wm) != SDL_TRUE || wm.subsystem != SDL_SYSWM_WINDOWS) return;

    HWND hwnd = wm.info.win.window;
    HMODULE self = GetModuleHandle(NULL);

    if (!g_app_icon) {
        g_app_icon = (HICON)LoadImageA(self, MAKEINTRESOURCEA(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    }
    if (g_app_icon) {
        SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)g_app_icon);
        SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)g_app_icon);
        SetClassLongPtr(hwnd, GCLP_HICON, (LONG_PTR)g_app_icon);
        SetClassLongPtr(hwnd, GCLP_HICONSM, (LONG_PTR)g_app_icon);
    }

    if (g_app_cursor_base_w <= 0 || g_app_cursor_base_h <= 0) {
        HCURSOR base = (HCURSOR)LoadImageA(self, MAKEINTRESOURCEA(100), IMAGE_CURSOR, 0, 0,
                                           LR_DEFAULTSIZE);
        if (base) {
            int bw = 0, bh = 0;
            if (cursor_size_from_handle(base, &bw, &bh) && bw > 0 && bh > 0) {
                g_app_cursor_base_w = bw;
                g_app_cursor_base_h = bh;
            }
            DestroyCursor(base);
        }
        if (g_app_cursor_base_w <= 0) g_app_cursor_base_w = 32;
        if (g_app_cursor_base_h <= 0) g_app_cursor_base_h = 32;
    }

    int dw = 0, dh = 0;
    SDL_GL_GetDrawableSize(window, &dw, &dh);
    if (dw <= 0 || dh <= 0) SDL_GetWindowSize(window, &dw, &dh);

    float scale = 1.0f;
    if (dh > 720) scale = (float)dh / 720.0f;

    int target_w = (int)(g_app_cursor_base_w * scale + 0.5f);
    int target_h = (int)(g_app_cursor_base_h * scale + 0.5f);
    if (target_w < g_app_cursor_base_w) target_w = g_app_cursor_base_w;
    if (target_h < g_app_cursor_base_h) target_h = g_app_cursor_base_h;

    if (!g_app_cursor || target_w != g_app_cursor_w || target_h != g_app_cursor_h) {
        HCURSOR next = (HCURSOR)LoadImageA(self, MAKEINTRESOURCEA(100), IMAGE_CURSOR, target_w,
                                           target_h, 0);
        if (!next) {
            next = (HCURSOR)LoadImageA(self, MAKEINTRESOURCEA(100), IMAGE_CURSOR, 0, 0,
                                       LR_DEFAULTSIZE);
            target_w = g_app_cursor_base_w;
            target_h = g_app_cursor_base_h;
        }
        if (next) {
            if (g_app_cursor) DestroyCursor(g_app_cursor);
            g_app_cursor = next;
            g_app_cursor_w = target_w;
            g_app_cursor_h = target_h;
        }
    }

    if (g_app_cursor) {
        SetClassLongPtr(hwnd, GCLP_HCURSOR, (LONG_PTR)g_app_cursor);
        SetCursor(g_app_cursor);
        SDL_ShowCursor(SDL_ENABLE);
    }

    if (!g_prev_wndproc) {
        g_prev_wndproc = (WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)app_wndproc);
    }
}

static void release_host_assets(void) {
    /* Win32 cursor/icon are process-owned; nothing SDL-specific to free here. */
}
#else
/* Linux / non-Windows: custom cursor + window icon from assets/ (same files
 * Windows embeds as resources). */

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_THREAD_LOCALS
#include "stb_image.h"

static SDL_Cursor *g_sdl_cursor = NULL;
static SDL_Surface *g_cursor_base = NULL; /* RGBA, native CUR size */
static int g_cursor_hot_x = 0;
static int g_cursor_hot_y = 0;
static int g_sdl_cursor_w = 0;
static int g_sdl_cursor_h = 0;
static int g_window_icon_set = 0;

static void apply_console_visibility(int show_console) { (void)show_console; }

/* Decode one 32-bpp XOR+AND DIB image from a .cur/.ico blob into an RGBA surface.
 * Classic CUR stores height*2 in the BITMAPINFOHEADER (XOR + AND mask). */
static SDL_Surface *surface_from_cur_dib(const Uint8 *img, Uint32 img_bytes, int entry_w,
                                         int entry_h) {
    if (!img || img_bytes < 40 || entry_w <= 0 || entry_h <= 0) return NULL;

    const Uint32 biSize = (Uint32)img[0] | ((Uint32)img[1] << 8) | ((Uint32)img[2] << 16) |
                          ((Uint32)img[3] << 24);
    if (biSize < 40) return NULL;

    const Sint32 biWidth = (Sint32)((Uint32)img[4] | ((Uint32)img[5] << 8) | ((Uint32)img[6] << 16) |
                                    ((Uint32)img[7] << 24));
    const Sint32 biHeightRaw =
        (Sint32)((Uint32)img[8] | ((Uint32)img[9] << 8) | ((Uint32)img[10] << 16) |
                 ((Uint32)img[11] << 24));
    const Uint16 biBitCount = (Uint16)(img[14] | (img[15] << 8));
    const Uint32 biCompression =
        (Uint32)img[16] | ((Uint32)img[17] << 8) | ((Uint32)img[18] << 16) | ((Uint32)img[19] << 24);

    if (biBitCount != 32 || biCompression != 0) return NULL;

    const int w = biWidth > 0 ? biWidth : entry_w;
    int h = biHeightRaw;
    if (h < 0) h = -h;
    /* CUR/ICO DIBs store XOR+AND stacked; visible height is half. */
    if (h == entry_h * 2) h = entry_h;
    if (w <= 0 || h <= 0) return NULL;

    const size_t xor_bytes = (size_t)w * (size_t)h * 4u;
    if (biSize + xor_bytes > img_bytes) return NULL;

    const Uint8 *xor_px = img + biSize;
    /* AND mask follows XOR; row is DWORD-aligned 1 bpp. */
    const size_t and_row = ((size_t)w + 31u) / 32u * 4u;
    const Uint8 *and_px = (biSize + xor_bytes + and_row * (size_t)h <= img_bytes)
                              ? (xor_px + xor_bytes)
                              : NULL;

    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surf) return NULL;

    Uint8 *dst = (Uint8 *)surf->pixels;
    for (int y = 0; y < h; ++y) {
        /* DIB rows are bottom-up. */
        const Uint8 *src = xor_px + (size_t)(h - 1 - y) * (size_t)w * 4u;
        Uint8 *row = dst + (size_t)y * (size_t)surf->pitch;
        for (int x = 0; x < w; ++x) {
            const Uint8 b = src[x * 4 + 0];
            const Uint8 g = src[x * 4 + 1];
            const Uint8 r = src[x * 4 + 2];
            Uint8 a = src[x * 4 + 3];
            if (and_px) {
                const Uint8 *and_row_p = and_px + (size_t)(h - 1 - y) * and_row;
                const int transparent = (and_row_p[x / 8] >> (7 - (x % 8))) & 1;
                if (transparent) a = 0;
                else if (a == 0) a = 255;
            } else if (a == 0 && (r | g | b)) {
                a = 255;
            }
            row[x * 4 + 0] = r;
            row[x * 4 + 1] = g;
            row[x * 4 + 2] = b;
            row[x * 4 + 3] = a;
        }
    }
    return surf;
}

static int is_png_blob(const Uint8 *img, Uint32 nbytes) {
    static const Uint8 kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (!img || nbytes < 8) return 0;
    for (int i = 0; i < 8; ++i)
        if (img[i] != kSig[i]) return 0;
    return 1;
}

static SDL_Surface *surface_from_png_blob(const Uint8 *img, Uint32 nbytes) {
    int w = 0, h = 0, comp = 0;
    unsigned char *px = stbi_load_from_memory(img, (int)nbytes, &w, &h, &comp, 4);
    if (!px || w <= 0 || h <= 0) {
        if (px) stbi_image_free(px);
        return NULL;
    }
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surf) {
        stbi_image_free(px);
        return NULL;
    }
    for (int y = 0; y < h; ++y) {
        SDL_memcpy((Uint8 *)surf->pixels + (size_t)y * (size_t)surf->pitch, px + (size_t)y * (size_t)w * 4u,
                   (size_t)w * 4u);
    }
    stbi_image_free(px);
    return surf;
}

/* Load a .ico (type 1) or .cur (type 2). For icons, prefer the largest PNG then
 * the largest 32bpp DIB. For cursors, prefer largest 32bpp DIB (hotspot out). */
static SDL_Surface *load_win_image_from_assets(const char *relpath, Uint16 expect_type, int *hot_x,
                                               int *hot_y) {
    if (hot_x) *hot_x = 0;
    if (hot_y) *hot_y = 0;

    char *base = pvz2_host_install_path();
    if (!base) return NULL;
    char path[1024];
    SDL_snprintf(path, sizeof(path), "%s%s", base, relpath);
    SDL_free(base);

    SDL_RWops *rw = SDL_RWFromFile(path, "rb");
    if (!rw) return NULL;
    const Sint64 sz64 = SDL_RWsize(rw);
    if (sz64 < 6 || sz64 > 16 * 1024 * 1024) {
        SDL_RWclose(rw);
        return NULL;
    }
    const size_t sz = (size_t)sz64;
    Uint8 *buf = (Uint8 *)SDL_malloc(sz);
    if (!buf) {
        SDL_RWclose(rw);
        return NULL;
    }
    if (SDL_RWread(rw, buf, 1, sz) != sz) {
        SDL_free(buf);
        SDL_RWclose(rw);
        return NULL;
    }
    SDL_RWclose(rw);

    const Uint16 type = (Uint16)(buf[2] | (buf[3] << 8));
    const Uint16 count = (Uint16)(buf[4] | (buf[5] << 8));
    if (type != expect_type || count == 0 || 6 + (size_t)count * 16 > sz) {
        SDL_free(buf);
        return NULL;
    }

    int best_png = -1, best_png_area = -1;
    int best_dib = -1, best_dib_area = -1;
    for (Uint16 i = 0; i < count; ++i) {
        const Uint8 *e = buf + 6 + (size_t)i * 16;
        int ew = e[0] ? e[0] : 256;
        int eh = e[1] ? e[1] : 256;
        const Uint32 nbytes =
            (Uint32)e[8] | ((Uint32)e[9] << 8) | ((Uint32)e[10] << 16) | ((Uint32)e[11] << 24);
        const Uint32 off =
            (Uint32)e[12] | ((Uint32)e[13] << 8) | ((Uint32)e[14] << 16) | ((Uint32)e[15] << 24);
        if (off >= sz || nbytes < 8 || off + nbytes > sz) continue;
        const int area = ew * eh;
        if (is_png_blob(buf + off, nbytes)) {
            if (area > best_png_area) {
                best_png_area = area;
                best_png = (int)i;
            }
        } else if (nbytes >= 40) {
            const Uint16 biBit = (Uint16)(buf[off + 14] | (buf[off + 15] << 8));
            if (biBit == 32 && area > best_dib_area) {
                best_dib_area = area;
                best_dib = (int)i;
            }
        }
    }

    const int pick = best_png >= 0 ? best_png : best_dib;
    if (pick < 0) {
        SDL_free(buf);
        return NULL;
    }

    const Uint8 *e = buf + 6 + (size_t)pick * 16;
    const int ew = e[0] ? e[0] : 256;
    const int eh = e[1] ? e[1] : 256;
    if (hot_x) *hot_x = (int)(e[4] | (e[5] << 8));
    if (hot_y) *hot_y = (int)(e[6] | (e[7] << 8));
    const Uint32 nbytes =
        (Uint32)e[8] | ((Uint32)e[9] << 8) | ((Uint32)e[10] << 16) | ((Uint32)e[11] << 24);
    const Uint32 off =
        (Uint32)e[12] | ((Uint32)e[13] << 8) | ((Uint32)e[14] << 16) | ((Uint32)e[15] << 24);

    SDL_Surface *surf = NULL;
    if (is_png_blob(buf + off, nbytes))
        surf = surface_from_png_blob(buf + off, nbytes);
    else
        surf = surface_from_cur_dib(buf + off, nbytes, ew, eh);
    SDL_free(buf);
    return surf;
}

static int load_cursor_base_from_assets(void) {
    if (g_cursor_base) return 1;
    g_cursor_base = load_win_image_from_assets("assets/cursor.cur", 2, &g_cursor_hot_x, &g_cursor_hot_y);
    if (!g_cursor_base) return 0;
    if (g_cursor_hot_x < 0) g_cursor_hot_x = 0;
    if (g_cursor_hot_y < 0) g_cursor_hot_y = 0;
    if (g_cursor_hot_x >= g_cursor_base->w) g_cursor_hot_x = g_cursor_base->w / 8;
    if (g_cursor_hot_y >= g_cursor_base->h) g_cursor_hot_y = g_cursor_base->h / 8;
    return 1;
}

static void apply_window_icon_from_assets(SDL_Window *window) {
    if (!window || g_window_icon_set) return;
    SDL_Surface *icon = load_win_image_from_assets("assets/icon.ico", 1, NULL, NULL);
    if (!icon) return;
    SDL_SetWindowIcon(window, icon);
    SDL_FreeSurface(icon);
    g_window_icon_set = 1;
}

static void apply_windows_assets(SDL_Window *window) {
    if (!window) return;

    apply_window_icon_from_assets(window);

    if (!load_cursor_base_from_assets()) return;

    int dw = 0, dh = 0;
    SDL_GL_GetDrawableSize(window, &dw, &dh);
    if (dw <= 0 || dh <= 0) SDL_GetWindowSize(window, &dw, &dh);

    /* Match Windows visual size: ~32px at 720p, scales up with window height.
     * Source stays at the CUR's highest frame; we only downscale for display. */
    float scale = 1.0f;
    if (dh > 720) scale = (float)dh / 720.0f;

    const int ref = 32; /* same ballpark as Win32 LR_DEFAULTSIZE */
    int target_w = (int)(ref * scale + 0.5f);
    int target_h = (int)(ref * scale * (float)g_cursor_base->h / (float)g_cursor_base->w + 0.5f);
    if (target_w < 16) target_w = 16;
    if (target_h < 16) target_h = 16;
    if (target_w > g_cursor_base->w) target_w = g_cursor_base->w;
    if (target_h > g_cursor_base->h) target_h = g_cursor_base->h;

    if (g_sdl_cursor && target_w == g_sdl_cursor_w && target_h == g_sdl_cursor_h) {
        SDL_SetCursor(g_sdl_cursor);
        SDL_ShowCursor(SDL_ENABLE);
        return;
    }

    SDL_Surface *scaled = g_cursor_base;
    SDL_Surface *owned = NULL;
    if (target_w != g_cursor_base->w || target_h != g_cursor_base->h) {
        owned = SDL_CreateRGBSurfaceWithFormat(0, target_w, target_h, 32, SDL_PIXELFORMAT_RGBA32);
        if (!owned) return;
#if SDL_VERSION_ATLEAST(2, 0, 16)
        if (SDL_SoftStretchLinear(g_cursor_base, NULL, owned, NULL) != 0)
            SDL_BlitScaled(g_cursor_base, NULL, owned, NULL);
#else
        SDL_BlitScaled(g_cursor_base, NULL, owned, NULL);
#endif
        scaled = owned;
    }

    const int hot_x = g_cursor_hot_x * target_w / g_cursor_base->w;
    const int hot_y = g_cursor_hot_y * target_h / g_cursor_base->h;
    SDL_Cursor *next = SDL_CreateColorCursor(scaled, hot_x, hot_y);
    if (owned) SDL_FreeSurface(owned);
    if (!next) return;

    if (g_sdl_cursor) SDL_FreeCursor(g_sdl_cursor);
    g_sdl_cursor = next;
    g_sdl_cursor_w = target_w;
    g_sdl_cursor_h = target_h;
    SDL_SetCursor(g_sdl_cursor);
    SDL_ShowCursor(SDL_ENABLE);
}

static void release_host_assets(void) {
    if (g_sdl_cursor) {
        SDL_Cursor *def = SDL_GetDefaultCursor();
        if (def) SDL_SetCursor(def);
        SDL_FreeCursor(g_sdl_cursor);
        g_sdl_cursor = NULL;
    }
    g_sdl_cursor_w = 0;
    g_sdl_cursor_h = 0;
    if (g_cursor_base) {
        SDL_FreeSurface(g_cursor_base);
        g_cursor_base = NULL;
    }
}
#endif

void pvz2_host_pre_sdl_quit(void) {
    config_screen_shutdown();
    release_host_assets();
}

void pvz2_request_app_quit(void) {
    g_quit_requested = 1;
}

/* Current borderless-fullscreen state; F11 toggles it. */
static int g_fullscreen = 0;

/* Flipped once pvz2_session_start returns. While 0 the window title shows the
 * boot progress (the boot is minutes of dark window and this is the only sign
 * it is alive); once 1 the title shows the live FPS instead. */
static int g_booted = 0;

/* When the console is hidden ([log] console=0), keep a stable game title
 * instead of churny boot/FPS status updates. */
static int g_live_window_title = 1;
static const char *kGameTitle = "Plants vs. Zombies 2: It's About Time";

static GLuint g_splash_tex = 0;
static int g_splash_w = 0;
static int g_splash_h = 0;
static GLuint g_splash_bar_fill_tex = 0;
static GLuint g_splash_bar_frame_tex = 0;
static int g_boot_overlay_active = 0;
static int g_splash_fade_active = 0;
static Uint32 g_splash_fade_started = 0;
static const Uint32 kSplashFadeMs = 400;
/* The bar moves in steps: each boot phase raises g_boot_progress by the same kBootBarStep and the drawn value
 * g_boot_shown jumps to it when it is behind. In between, the bar creeps on its own at a constant
 * kBootBarCreepPxPerSec (the engine reports nothing inside its longest phase, onSurfaceCreated) up to
 * kBootBarCeiling; the final step, once the first frame is done, takes it to 100 %. */
static float g_boot_progress = 0.02f;
static float g_boot_shown = 0.02f;
static Uint32 g_boot_bar_tick = 0;
static const float kBootBarCreepPxPerSec = 20.0f;
static const float kBootBarStep = 0.01f;
static const float kBootBarCeiling = 0.99f;
static char g_last_boot_status[160] = {0};

static void destroy_boot_splash(void);

static void update_boot_progress_from_status(const char *status) {
    if (!status || status[0] == '\0') return;

    /* Every constructor is its own status line; they all count as one phase. */
    const char *kCtors = "booting: static constructors";
    const int is_ctor = SDL_strncmp(status, kCtors, SDL_strlen(kCtors)) == 0;
    const int was_ctor = SDL_strncmp(g_last_boot_status, kCtors, SDL_strlen(kCtors)) == 0;
    if (is_ctor ? !was_ctor : SDL_strcmp(status, g_last_boot_status) != 0) {
        g_boot_progress += kBootBarStep;
        if (g_boot_progress > kBootBarCeiling) g_boot_progress = kBootBarCeiling;
    }

    SDL_snprintf(g_last_boot_status, sizeof(g_last_boot_status), "%s", status);
}

/* Fraction of the bar to draw; fill_w_px is the bar's width in pixels (the creep speed is in pixels). */
static float advance_boot_bar(int fill_w_px) {
    const Uint32 now = SDL_GetTicks();
    float dt = g_boot_bar_tick ? (float)(now - g_boot_bar_tick) / 1000.0f : 0.0f;
    g_boot_bar_tick = now;
    if (dt > 0.25f) dt = 0.25f; /* a stall in the host must not turn into a leap */

    if (g_boot_shown < g_boot_progress) g_boot_shown = g_boot_progress;
    const float px = fill_w_px > 0 ? 1.0f / (float)fill_w_px : 0.0f;
    if (g_boot_shown < kBootBarCeiling && g_boot_progress < 1.0f) {
        g_boot_shown += kBootBarCreepPxPerSec * px * dt;
        if (g_boot_shown > kBootBarCeiling) g_boot_shown = kBootBarCeiling;
    }
    if (g_boot_shown > 1.0f) g_boot_shown = 1.0f;
    return g_boot_shown;
}

static int load_bmp_texture_from_memory(const void *ptr, int bytes, Uint32 pixel_format,
                                        GLenum gl_format, int magenta_key_to_alpha, GLuint *out_tex,
                                        int *out_w, int *out_h) {
    if (!out_tex || !out_w || !out_h || !ptr || bytes <= 0) return 0;

    SDL_RWops *rw = SDL_RWFromConstMem(ptr, bytes);
    if (!rw) return 0;

    SDL_Surface *bmp = SDL_LoadBMP_RW(rw, 1);
    if (!bmp) return 0;

    SDL_Surface *surface = SDL_ConvertSurfaceFormat(bmp, pixel_format, 0);
    SDL_FreeSurface(bmp);
    if (!surface) return 0;

    if (magenta_key_to_alpha) apply_magenta_colorkey(surface);

    glGenTextures(1, out_tex);
    glBindTexture(GL_TEXTURE_2D, *out_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    /* Set UNPACK_ROW_LENGTH to handle SDL surfaces whose pitch may be padded
     * (pitch != width * bytes_per_pixel). Without this, each row would be read
     * from the wrong offset causing a shear/deformation artifact. */
    int bpp = surface->format->BytesPerPixel;
    if (bpp > 0) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, surface->pitch / bpp);
    }
    glTexImage2D(GL_TEXTURE_2D, 0, gl_format, surface->w, surface->h, 0, gl_format, GL_UNSIGNED_BYTE,
                 surface->pixels);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    *out_w = surface->w;
    *out_h = surface->h;
    SDL_FreeSurface(surface);
    return *out_tex != 0;
}

static int load_bmp_texture_from_resource(int resource_id, Uint32 pixel_format, GLenum gl_format,
                                          int magenta_key_to_alpha, GLuint *out_tex, int *out_w,
                                          int *out_h) {
#ifdef _WIN32
    if (!out_tex || !out_w || !out_h) return 0;

    HMODULE self = GetModuleHandle(NULL);
    HRSRC res = FindResourceA(self, MAKEINTRESOURCEA(resource_id), RT_RCDATA);
    if (!res) return 0;

    HGLOBAL h = LoadResource(self, res);
    if (!h) return 0;

    DWORD bytes = SizeofResource(self, res);
    const void *ptr = LockResource(h);
    if (!ptr || bytes == 0) return 0;

    return load_bmp_texture_from_memory(ptr, (int)bytes, pixel_format, gl_format,
                                        magenta_key_to_alpha, out_tex, out_w, out_h);
#else
    int bytes = 0;
    void *buf = pvz2_load_asset_bytes(resource_id, &bytes);
    if (!buf) return 0;
    const int ok = load_bmp_texture_from_memory(buf, bytes, pixel_format, gl_format,
                                                magenta_key_to_alpha, out_tex, out_w, out_h);
    SDL_free(buf);
    return ok;
#endif
}

static int load_embedded_splash_texture(void) {
    if (!load_bmp_texture_from_resource(101, SDL_PIXELFORMAT_RGBA32, GL_RGBA, 0, &g_splash_tex,
                                        &g_splash_w, &g_splash_h)) {
        return 0;
    }

    int ignored_w = 0, ignored_h = 0;
    if (!load_bmp_texture_from_resource(102, SDL_PIXELFORMAT_RGBA32, GL_RGBA, 0,
                                        &g_splash_bar_fill_tex, &ignored_w, &ignored_h)) {
        if (g_splash_tex != 0) {
            glDeleteTextures(1, &g_splash_tex);
            g_splash_tex = 0;
        }
        g_splash_w = 0;
        g_splash_h = 0;
        return 0;
    }

    if (!load_bmp_texture_from_resource(103, SDL_PIXELFORMAT_RGBA32, GL_RGBA, 1,
                                        &g_splash_bar_frame_tex, &ignored_w, &ignored_h)) {
        if (g_splash_bar_fill_tex != 0) {
            glDeleteTextures(1, &g_splash_bar_fill_tex);
            g_splash_bar_fill_tex = 0;
        }
        if (g_splash_tex != 0) {
            glDeleteTextures(1, &g_splash_tex);
            g_splash_tex = 0;
        }
        g_splash_w = 0;
        g_splash_h = 0;
        return 0;
    }

    return g_splash_tex != 0 && g_splash_bar_fill_tex != 0 && g_splash_bar_frame_tex != 0;
}

static void draw_boot_splash_layer(SDL_Window *window, float alpha, int clear_background,
                                   int present_now) {
    if (!window || g_splash_tex == 0) return;

    int dw = 0, dh = 0;
    SDL_GL_GetDrawableSize(window, &dw, &dh);
    if (dw <= 0 || dh <= 0) return;

    /* Cover/crop: scale the image so it fills the entire screen while preserving
     * aspect ratio. The image is centered on X and anchored to the top (Y=0).
     * Any overflow is cropped via UV clipping. */

    GLint prev_program = 0, prev_fbo = 0, prev_viewport[4] = {0, 0, 0, 0}, prev_matrix_mode = GL_MODELVIEW;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    glGetIntegerv(GL_MATRIX_MODE, &prev_matrix_mode);

    glPushAttrib(GL_ALL_ATTRIB_BITS);

    /* Draw on the default framebuffer using a predictable fixed-function state,
     * then restore everything so guest rendering state does not get corrupted. */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(0);
    glViewport(0, 0, dw, dh);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    if (clear_background) {
        glDisable(GL_BLEND);
        glClearColor(0.06f, 0.07f, 0.10f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glMatrixMode(GL_TEXTURE);
    glPushMatrix();
    glLoadIdentity();

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_splash_tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glColor4f(1.0f, 1.0f, 1.0f, alpha);

    /* Compute UV range for cover/crop:
     * - scale = max(dw/img_w, dh/img_h)  -> fills the screen on both axes
     * - UV X: center-crop (equal margin on left and right)
     * - UV Y: top anchored at 0, bottom may exceed 1 (cropped by GL_CLAMP_TO_EDGE) */
    float u0 = 0.0f, u1 = 1.0f, v0 = 0.0f, v1 = 1.0f;
    if (g_splash_w > 0 && g_splash_h > 0) {
        float scale = (float)dw / (float)g_splash_w;
        if ((float)dh / (float)g_splash_h > scale)
            scale = (float)dh / (float)g_splash_h;
        float rendered_w = (float)g_splash_w * scale;
        float rendered_h = (float)g_splash_h * scale;
        float ucrop = ((rendered_w - (float)dw) * 0.5f) / rendered_w;
        u0 = ucrop;
        u1 = 1.0f - ucrop;
        v0 = 0.0f;
        v1 = (float)dh / rendered_h;
    }

    glBegin(GL_QUADS);
    glTexCoord2f(u0, v1); glVertex2f(-1.0f, -1.0f);
    glTexCoord2f(u1, v1); glVertex2f( 1.0f, -1.0f);
    glTexCoord2f(u1, v0); glVertex2f( 1.0f,  1.0f);
    glTexCoord2f(u0, v0); glVertex2f(-1.0f,  1.0f);
    glEnd();

    glDisable(GL_TEXTURE_2D);

    const float scale_x = (float)dw / 1920.0f;
    const float scale_y = (float)dh / 1080.0f;

    const int frame_w_px = (int)(484.0f * scale_x + 0.5f);
    const int frame_h_px = (int)(61.0f * scale_y + 0.5f);
    int frame_x_px = (dw - frame_w_px) / 2;
    const int frame_center_from_bottom_px = (int)(92.0f * scale_y + 0.5f);
    const int frame_center_y_px = frame_center_from_bottom_px;
    int frame_y_px = frame_center_y_px - frame_h_px / 2;

    if (frame_x_px < 0) frame_x_px = 0;
    if (frame_y_px < 0) frame_y_px = 0;
    if (frame_x_px + frame_w_px > dw) frame_x_px = dw - frame_w_px;
    if (frame_y_px + frame_h_px > dh) frame_y_px = dh - frame_h_px;

    const int fill_inset_left_px = (int)(10.0f * scale_x + 0.5f);
    const int fill_inset_right_px = (int)(14.0f * scale_x + 0.5f);
    const int fill_inset_top_px = (int)(8.0f * scale_y + 0.5f);
    const int fill_inset_bottom_px = (int)(10.0f * scale_y + 0.5f);

    int fill_x_px = frame_x_px + fill_inset_left_px;
    int fill_y_px = frame_y_px + fill_inset_top_px - 1;
    int fill_w_px = frame_w_px - fill_inset_left_px - fill_inset_right_px;
    int fill_h_px = frame_h_px - fill_inset_top_px - fill_inset_bottom_px + 2;

    if (fill_w_px < 1) fill_w_px = 1;
    if (fill_h_px < 1) fill_h_px = 1;
    if (fill_x_px < frame_x_px) fill_x_px = frame_x_px;
    if (fill_y_px < frame_y_px) fill_y_px = frame_y_px;
    if (fill_x_px + fill_w_px > frame_x_px + frame_w_px) {
        fill_w_px = (frame_x_px + frame_w_px) - fill_x_px;
    }
    if (fill_y_px + fill_h_px > frame_y_px + frame_h_px) {
        fill_h_px = (frame_y_px + frame_h_px) - fill_y_px;
    }
    if (fill_w_px < 1) fill_w_px = 1;
    if (fill_h_px < 1) fill_h_px = 1;

    const float progress = advance_boot_bar(fill_w_px);
    const int fill_cur_w_px = (int)((float)fill_w_px * progress + 0.5f);

    const float frame_x0 = -1.0f + (2.0f * (float)frame_x_px) / (float)dw;
    const float frame_y0 = -1.0f + (2.0f * (float)frame_y_px) / (float)dh;
    const float frame_x1 = -1.0f + (2.0f * (float)(frame_x_px + frame_w_px)) / (float)dw;
    const float frame_y1 = -1.0f + (2.0f * (float)(frame_y_px + frame_h_px)) / (float)dh;

    const float fill_x0 = -1.0f + (2.0f * (float)fill_x_px) / (float)dw;
    const float fill_y0 = -1.0f + (2.0f * (float)fill_y_px) / (float)dh;
    const float fill_x1 = -1.0f + (2.0f * (float)(fill_x_px + fill_cur_w_px)) / (float)dw;
    const float fill_y1 = -1.0f + (2.0f * (float)(fill_y_px + fill_h_px)) / (float)dh;

    glEnable(GL_TEXTURE_2D);
    glColor4f(1.0f, 1.0f, 1.0f, alpha);

    if (g_splash_bar_fill_tex != 0 && fill_cur_w_px > 0) {
        glBindTexture(GL_TEXTURE_2D, g_splash_bar_fill_tex);
        glBegin(GL_QUADS);
        glTexCoord2f(0.0f, 1.0f); glVertex2f(fill_x0, fill_y0);
        glTexCoord2f(1.0f, 1.0f); glVertex2f(fill_x1, fill_y0);
        glTexCoord2f(1.0f, 0.0f); glVertex2f(fill_x1, fill_y1);
        glTexCoord2f(0.0f, 0.0f); glVertex2f(fill_x0, fill_y1);
        glEnd();
    }

    if (g_splash_bar_frame_tex != 0) {
        glBindTexture(GL_TEXTURE_2D, g_splash_bar_frame_tex);
        glBegin(GL_QUADS);
        glTexCoord2f(0.0f, 1.0f); glVertex2f(frame_x0, frame_y0);
        glTexCoord2f(1.0f, 1.0f); glVertex2f(frame_x1, frame_y0);
        glTexCoord2f(1.0f, 0.0f); glVertex2f(frame_x1, frame_y1);
        glTexCoord2f(0.0f, 0.0f); glVertex2f(frame_x0, frame_y1);
        glEnd();
    }

    glDisable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);

    glMatrixMode(GL_TEXTURE);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();

    glPopAttrib();
    glUseProgram(prev_program);
    glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    glMatrixMode(prev_matrix_mode);

    if (present_now) {
        SDL_GL_SwapWindow(window);
        SDL_PumpEvents();
    }
}

static void begin_boot_splash_fade(void) {
    if (g_splash_tex == 0) {
        g_boot_overlay_active = 0;
        return;
    }
    g_boot_overlay_active = 1;
    g_splash_fade_active = 1;
    g_splash_fade_started = 0;
}

static void draw_boot_splash_fade(SDL_Window *window) {
    if (!window || !g_boot_overlay_active || g_splash_tex == 0 || !g_splash_fade_active) return;

    if (g_splash_fade_started == 0) {
        g_splash_fade_started = SDL_GetTicks();
        g_boot_progress = 1.0f; /* the first frame is done: the final step */
        draw_boot_splash_layer(window, 1.0f, 0, 0);
        return;
    }

    const Uint32 elapsed = SDL_GetTicks() - g_splash_fade_started;
    if (elapsed >= kSplashFadeMs) {
        destroy_boot_splash();
        return;
    }

    const float alpha = 1.0f - (float)elapsed / (float)kSplashFadeMs;
    draw_boot_splash_layer(window, alpha, 0, 0);
}

/* One defined frame while the boot has not started: the loading screen when its textures loaded, a plain
 * colour otherwise. Safe to call repeatedly. */
static void paint_startup_frame(SDL_Window *window, int have_splash) {
    if (have_splash) {
        g_boot_overlay_active = 1;
        draw_boot_splash_layer(window, 1.0f, 1, 1);
    } else {
        g_boot_overlay_active = 0;
        glClearColor(0.06f, 0.07f, 0.10f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        SDL_GL_SwapWindow(window);
        SDL_PumpEvents();
    }
}

static void destroy_boot_splash(void) {
    if (g_splash_tex != 0) {
        pvz2_frame_limit_vsync_resume();
        glDeleteTextures(1, &g_splash_tex);
        g_splash_tex = 0;
    }
    if (g_splash_bar_fill_tex != 0) {
        glDeleteTextures(1, &g_splash_bar_fill_tex);
        g_splash_bar_fill_tex = 0;
    }
    if (g_splash_bar_frame_tex != 0) {
        glDeleteTextures(1, &g_splash_bar_frame_tex);
        g_splash_bar_frame_tex = 0;
    }
    g_splash_w = 0;
    g_splash_h = 0;
    g_boot_overlay_active = 0;
    g_splash_fade_active = 0;
    g_splash_fade_started = 0;
    g_boot_progress = 0.02f;
    g_boot_shown = 0.02f;
    g_boot_bar_tick = 0;
    g_last_boot_status[0] = '\0';
}

/* Re-reads the window's drawable size and re-renders the game at it. Publishing
 * the size is immediate, so THIS frame already fills the window and the touch
 * mapping is right straight away; only re-running onSurfaceChanged is deferred to
 * the frame thread, because it is a guest call.
 *
 * The touch space is deliberately NOT moved -- pvz2_surface_set freezes it at the
 * launch size on its first call, which is the whole reason that rule lives in the
 * surface module instead of here. */
static void update_window_size(void) {
    if (!g_window) return;
    refresh_window_metrics();
    int dw = g_cached_dw;
    int dh = g_cached_dh;
    if (dw > 0 && dh > 0) {
        pvz2_surface_set((uint32_t)dw, (uint32_t)dh);
        pvz2_session_request_resize();
    }
    apply_windows_assets(g_window);
}

/* Same surface sync as update_window_size, but without guest resize callback.
 * Used before session start so the FIRST pvz2_surface_set (touch freeze point)
 * matches the real startup drawable size, including fullscreen/DPI changes. */
static void sync_initial_surface_size(void) {
    if (!g_window) return;
    refresh_window_metrics();
    int dw = g_cached_dw;
    int dh = g_cached_dh;
    if (dw <= 0 || dh <= 0) {
        dw = g_cached_ww;
        dh = g_cached_wh;
    }
    if (dw > 0 && dh > 0) pvz2_surface_set((uint32_t)dw, (uint32_t)dh);
}

static void request_resize_sync(void) { g_resize_pending = 1; }

static void apply_pending_resize_sync(int force_now) {
    if (!g_resize_pending) return;
    const Uint32 now = SDL_GetTicks();
    if (!force_now && (now - g_last_resize_apply_ms) < kResizeApplyMinMs) return;
    update_window_size();
    g_last_resize_apply_ms = now;
    g_resize_pending = 0;

    if (!g_booted && g_boot_overlay_active && g_window && g_splash_tex != 0) {
        draw_boot_splash_layer(g_window, 1.0f, 1, 1);
    }
}

/* Closing the window has to take effect immediately. The boot runs for many
 * seconds inside a single guest call that cannot be unwound, so setting a flag
 * would leave the window on screen -- ignoring the close -- until the boot
 * finishes. Exiting straight from the event handler is the only way to make the
 * X button respond during startup; the OS reclaims everything.
 * Escape is handled separately (quit-confirm dialog); only SDL_QUIT (title-bar
 * X / Alt+F4) comes through here. */
/* SDL_QUIT (title-bar X / Alt+F4). After boot, break the main loop so
 * session_end can join guest threads; during boot the host is stuck inside a
 * long guest call, so exit immediately. */
static void handle_quit(void) {
    if (g_booted) {
        g_quit_requested = 1;
        return;
    }
    fflush(stdout);
    pvz2_host_pre_sdl_quit();
    exit(0);
}

/* F11 toggles borderless fullscreen. FULLSCREEN_DESKTOP (not real fullscreen)
 * keeps the desktop resolution and just borderlessly fills the screen, which
 * avoids a mode switch and plays well with the compositor scaling. The size
 * change it causes arrives as SDL_WINDOWEVENT_SIZE_CHANGED, so update_window_size
 * re-fits the engine the same way an ordinary resize does -- nothing extra here. */
static void toggle_fullscreen(void) {
    if (!g_window) return;
    g_fullscreen = !g_fullscreen;
    SDL_SetWindowFullscreen(g_window, g_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    request_resize_sync();
    apply_pending_resize_sync(1);
}

/* Settings overlay: apply resolution / fullscreen without relaunching. Keeps
 * g_fullscreen in sync with F11 so a later toggle does not invert the wrong way. */
void pvz2_host_apply_video_settings(int fullscreen, int width, int height) {
    if (!g_window) return;
    g_fullscreen = fullscreen ? 1 : 0;
    if (g_fullscreen) {
        SDL_SetWindowFullscreen(g_window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    } else {
        SDL_SetWindowFullscreen(g_window, 0);
        if (width > 0 && height > 0)
            SDL_SetWindowSize(g_window, width, height);
        {
            const int d = pvz2_effective_display();
            SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED_DISPLAY(d),
                                   SDL_WINDOWPOS_CENTERED_DISPLAY(d));
        }
    }
    request_resize_sync();
    apply_pending_resize_sync(1);
}

/* Settings overlay: move the window to another display without a restart.
 * A fullscreen window has to drop out of fullscreen to actually change which
 * monitor it is on -- SDL_SetWindowPosition alone does not migrate a
 * FULLSCREEN_DESKTOP window -- so this briefly windows it, repositions, then
 * re-applies fullscreen if it was on. */
void pvz2_host_apply_display_settings(int display_index) {
    if (!g_window) return;
    const int was_fullscreen = g_fullscreen;
    if (was_fullscreen) SDL_SetWindowFullscreen(g_window, 0);
    SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED_DISPLAY(display_index),
                          SDL_WINDOWPOS_CENTERED_DISPLAY(display_index));
    if (was_fullscreen) SDL_SetWindowFullscreen(g_window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    request_resize_sync();
    apply_pending_resize_sync(1);
}

static float clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

static int trigger_shortcut_for_scancode(SDL_Scancode scancode) {
    if (scancode == SDL_SCANCODE_UNKNOWN) return 0;

    for (int i = 0; i < g_shortcut_count; ++i) {
        const runtime_shortcut_t *s = &g_shortcuts[i];
        if (s->scancode != scancode) continue;

        const float tw = (float)pvz2_touch_space_width();
        const float th = (float)pvz2_touch_space_height();
        if (tw <= 0.0f || th <= 0.0f) return 0;

        const float nx = clamp01(s->x);
        const float ny = clamp01(s->y);
        const int tx = (int)(nx * (tw - 1.0f) + 0.5f);
        const int ty = (int)(ny * (th - 1.0f) + 0.5f);

        pvz2_input_push_touch(PVZ2_TOUCH_DOWN, tx, ty);
        pvz2_input_push_touch(PVZ2_TOUCH_UP, tx, ty);
        mark_touch_activity();
        return 1;
    }
    return 0;
}

/* The single place an SDL event turns into an action. Both drain sites -- the
 * boot-time host_pump and the steady-state loop -- route through here so quit,
 * F11 and resize behave identically in both, and adding a shortcut is a one-line
 * change in one function rather than two that have to be kept in sync. */
static void handle_sdl_event(const SDL_Event *e) {
    if (e->type == SDL_QUIT) {
        handle_quit();
    } else if (e->type == SDL_KEYDOWN && e->key.keysym.sym == SDLK_F11 && e->key.repeat == 0) {
        toggle_fullscreen();
    } else if (e->type == SDL_WINDOWEVENT) {
        if (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
            e->window.event == SDL_WINDOWEVENT_RESIZED) {
            refresh_window_metrics();
            request_resize_sync();
            apply_pending_resize_sync(0);
        } else if (e->window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
                   e->window.event == SDL_WINDOWEVENT_LEAVE ||
                   e->window.event == SDL_WINDOWEVENT_HIDDEN ||
                   e->window.event == SDL_WINDOWEVENT_MINIMIZED) {
            release_active_mouse_touch();
            hard_reset_touch_state();
            g_last_touch_input_ms = SDL_GetTicks();
        }
    }

    if (e->type == SDL_KEYDOWN && e->key.repeat == 0) {
        const int typing = SDL_IsTextInputActive() || pvz2_input_keyboard_wanted();
        if (!typing && trigger_shortcut_for_scancode(e->key.keysym.scancode)) return;
    }

    if (pvz2_gamepad_handle_event(e)) return;

    /* Also fed to the input queue: F11/ESC map to no guest key, so this is safe,
     * and anything typed or clicked during the boot still reaches the engine. */
    feed_input_event(e);
}

/* Once-per-half-second FPS in the window title, from real presented frames.
 * This replaced the old per-frame "Native_onDrawFrame[N]" status text, which
 * churned a mutex and a SetWindowTitle every frame. Measured off the wall clock,
 * so it is honest about dropped/duplicated frames. */
static void update_fps_title(void) {
    if (!g_live_window_title) return;
    static Uint32 window_start = 0;
    static int frames = 0;
    static int started = 0;
    Uint32 now = SDL_GetTicks();
    if (!started) { window_start = now; started = 1; }
    ++frames;
    Uint32 elapsed = now - window_start;
    if (elapsed >= 500) {
        double fps = (double)frames * 1000.0 / (double)elapsed;
        char title[64];
        SDL_snprintf(title, sizeof(title), "PvZ2 - %.0f FPS", fps);
        if (g_window) SDL_SetWindowTitle(g_window, title);
        frames = 0;
        window_start = now;
    }
}

/* Called from inside long guest calls (see pvz2_session_set_host_pump). The
 * boot sequence runs for minutes in one call, so without draining the message
 * queue the OS marks the window "Not responding" for all of startup. Rate
 * limited because it is invoked on every JIT slice boundary, which during
 * instruction-level tracing is every few dozen ticks. */
static void host_pump(void) {
    static Uint32 last_pump = 0;
    Uint32 now = SDL_GetTicks();
    if (now - last_pump < 100) return;
    last_pump = now;

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        /* Title-bar X / Alt+F4 always exits immediately, even mid-boot or
         * while the quit-confirm dialog is up. Escape opens settings, then the quit dialog. */
        if (event.type == SDL_QUIT) {
            handle_quit();
            continue;
        }
        if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE &&
            event.key.repeat == 0) {
            config_screen_request_escape();
            continue;
        }
        /* The boot runs for seconds inside a single guest call, so events
         * (including an F11) have to be acted on here too, not just in the
         * steady-state loop -- hence the shared handler.
         * Quit-confirm must also be drawable/interactive mid-boot. */
        if (event.type == SDL_WINDOWEVENT)
            handle_sdl_event(&event);
        /* Stick/A/Back must work on quit-confirm even mid-boot. */
        if (pvz2_gamepad_handle_event(&event))
            continue;
        if (achievements_screen_is_open()) {
            achievements_screen_handle_event(&event);
        } else if (config_screen_is_open()) {
            config_screen_handle_event(&event);
        } else if (event.type != SDL_WINDOWEVENT) {
            handle_sdl_event(&event);
        }
    }
    /* Stick/D-pad timing must only tick once per frame. After boot the
     * steady-state loop owns pvz2_gamepad_tick; doing it here too made the
     * left stick and D-pad feel ~2x as fast. Boot still needs a tick so
     * quit-confirm mid-load can be aimed with the stick. */
    if (!g_booted) {
        static Uint32 s_last_boot_gp_ms = 0;
        const Uint32 gp_now = SDL_GetTicks();
        float gp_dt = 0.1f;
        if (s_last_boot_gp_ms != 0) {
            gp_dt = (float)(gp_now - s_last_boot_gp_ms) / 1000.0f;
            if (gp_dt < 0.0f) gp_dt = 0.0f;
            if (gp_dt > 0.15f) gp_dt = 0.15f;
        }
        s_last_boot_gp_ms = gp_now;
        pvz2_gamepad_tick(gp_dt);
    }
    apply_pending_resize_sync(0);
    maybe_force_touch_recovery();
    sync_text_input();

    /* The boot takes minutes behind a dark window; without this there is no way
     * to tell a working startup from a hang. Only while booting: once the game
     * is running the steady-state loop owns the title and shows the FPS instead
     * (host_pump still runs between JIT slices inside a frame, so this guard is
     * what stops it from fighting update_fps_title over the title). */
    if (g_window && !g_booted) {
        char status[160];
        pvz2_session_status(status, sizeof(status));
        update_boot_progress_from_status(status);

        if (g_live_window_title) {
            char title[224];
            SDL_snprintf(title, sizeof(title), "PvZ2 - %s", status);
            SDL_SetWindowTitle(g_window, title);
        }

        if (g_boot_overlay_active && g_splash_tex != 0) {
            /* If quit-confirm is up, draw splash without presenting so the
             * dialog can composite on top before a single SwapWindow. */
            const int present_now = config_screen_is_open() ? 0 : 1;
            draw_boot_splash_layer(g_window, 1.0f, 1, present_now);
        }
        /* Quit-confirm can appear over the splash while still booting. */
        if (config_screen_is_open()) {
            config_screen_draw(g_window);
            SDL_GL_SwapWindow(g_window);
        }
    } else if (g_window && g_splash_fade_active && g_splash_fade_started == 0 && g_splash_tex != 0) {
        /* The first frame after the boot takes seconds; until it is done and the fade starts, keep the loading
         * screen (and its bar) alive instead of leaving the window frozen. */
        draw_boot_splash_layer(g_window, 1.0f, 1, 1);
    }
}

/* Config lives in config.ini next to the executable, and CLI can override any
 * static key at runtime via --key=value or --section.key=value. This keeps
 * config.ini as the baseline while allowing one-off launch overrides without
 * editing the file. Dynamic [shortcuts] bind_* entries stay config-only. */
static int logs_fully_disabled(const pvz2_config_t *cfg) {
    return cfg && cfg->console == 0 && cfg->verbose == 0 && cfg->trace == 0 &&
           cfg->pc_sample == 0 && cfg->input == 0;
}

static int path_is_absolute(const char *p) {
    if (!p || !p[0]) return 0;
    if (p[0] == '/' || p[0] == '\\') return 1;
    return ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':';
}

static void write_sdl_log_to_stderr(void *userdata, int category,
                                    SDL_LogPriority priority, const char *message) {
    static const char *const priority_names[] = {
        "", "VERBOSE", "DEBUG", "INFO", "WARN", "ERROR", "CRITICAL"
    };
    (void)userdata;
    (void)category;
    if (!message || priority < SDL_LOG_PRIORITY_VERBOSE ||
        priority >= SDL_NUM_LOG_PRIORITIES) return;
    fprintf(stderr, "%s: %s\n", priority_names[priority], message);
}

/* [log] logfile: create/overwrite the file at launch only when the key is set.
 * stdout/stderr then go there (same text the console would print). Relative
 * names are resolved against the executable folder, matching [paths] so/obb. */
static void apply_logfile_redirect(const pvz2_config_t *cfg) {
    if (!cfg || cfg->log_file[0] == '\0') return;

    char resolved[1024];
    const char *path = cfg->log_file;
    if (!path_is_absolute(path)) {
        char *base = pvz2_host_data_path();
        SDL_snprintf(resolved, sizeof(resolved), "%s%s", base ? base : "", path);
        if (base) SDL_free(base);
        path = resolved;
    }

#ifdef _WIN32
    /* Keep the CRT FILE objects alive. Reopening stdout/stderr with
     * freopen_s is unsafe for a Windows-subsystem process that started
     * without console handles and can crash later when the CRT flushes them. */
    const int log_fd = _open(path, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY,
                             _S_IREAD | _S_IWRITE);
    if (log_fd == -1) return;

    const int stdout_fd = _fileno(stdout);
    const int stderr_fd = _fileno(stderr);
    if (stdout_fd == -1 || stderr_fd == -1 ||
        _dup2(log_fd, stdout_fd) != 0 || _dup2(log_fd, stderr_fd) != 0) {
        _close(log_fd);
        return;
    }
    _close(log_fd);
#else
    if (!freopen(path, "w", stdout)) return;
    if (dup2(fileno(stdout), STDERR_FILENO) < 0)
        (void)freopen(path, "a", stderr);
#endif
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    SDL_LogSetOutputFunction(write_sdl_log_to_stderr, NULL);
    g_text_logs_enabled = 1;
    pvz2_log_set_enabled(1);
}

static void apply_text_log_silence_if_needed(const pvz2_config_t *cfg) {
    if (cfg && cfg->log_file[0] != '\0') return;
    if (!logs_fully_disabled(cfg)) return;
    g_text_logs_enabled = 0;
    pvz2_log_set_enabled(0);
#ifdef _WIN32
    FILE *stream = NULL;
    freopen_s(&stream, "NUL", "w", stdout);
    freopen_s(&stream, "NUL", "w", stderr);
#else
    freopen("/dev/null", "w", stdout);
    freopen("/dev/null", "w", stderr);
#endif
}

static void print_usage(const char *argv0) {
    printf("%s\n", argv0);
    printf("\n");
    printf("Config file: config.ini next to the executable / .AppImage (written on first run).\n");
    printf("CLI overrides: pass --key=value or --section.key=value to override\n");
    printf("any static config key for this run only.\n");
    printf("Shortcuts (bind_*) are dynamic and intentionally excluded from CLI.\n");
    printf("\n");
    printf("Examples:\n");
    printf("  --fullscreen=1\n");
    printf("  --video.mode=native\n");
    printf("  --fps_limit=120\n");
    printf("  --paths.obb=data.obb\n");
}

/* Returns 1 when `path` can be opened and is not empty. SDL_RWFromFile takes UTF-8 paths
 * on every platform, unlike a bare fopen on Windows. */
static int required_file_ok(const char *path) {
    if (!path || !path[0]) return 0;
    SDL_RWops *f = SDL_RWFromFile(path, "rb");
    if (!f) return 0;
    Sint64 size = SDL_RWsize(f);
    SDL_RWclose(f);
    return size != 0;
}

/* Checks libPVZ2.so and the .obb right after the config is loaded. Without them the boot
 * would otherwise sit forever on a black window. Logs the problem, tells the player and
 * returns 0 so main can exit. */
static int check_required_files(const pvz2_config_t *cfg) {
    char msg[2200];
    msg[0] = '\0';
    int missing = 0;
    if (!required_file_ok(cfg->so_path)) {
        pvz2_log("[boot] ERROR: libPVZ2.so not found or empty: %s", cfg->so_path);
        SDL_snprintf(msg + strlen(msg), sizeof(msg) - strlen(msg), "Missing game library (.so):\n%s\n\n", cfg->so_path);
        missing = 1;
    }
    if (!required_file_ok(cfg->obb_path)) {
        pvz2_log("[boot] ERROR: data .obb not found or empty: %s", cfg->obb_path);
        SDL_snprintf(msg + strlen(msg), sizeof(msg) - strlen(msg), "Missing game data (.obb):\n%s\n\n", cfg->obb_path);
        missing = 1;
    }
    if (missing) {
        SDL_snprintf(msg + strlen(msg), sizeof(msg) - strlen(msg),
                     "Put the files at that location or change [paths] in config.ini.");
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "PvZ2 - Missing files", msg, NULL);
    }
    return !missing;
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) < 0) {
        pvz2_log("[sdl] SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    SDL_JoystickEventState(SDL_ENABLE);
    SDL_GameControllerEventState(SDL_ENABLE);

    /* Read config.ini from the portable data folder and resolve so/obb/save
     * relative to it. Loose binary / Windows: same as the exe folder. AppImage:
     * dirname($APPIMAGE) so data.obb + libPVZ2.so sit next to the .AppImage
     * while UI assets stay inside the mount (see pvz2_host_install_path). */
    char *base_path = pvz2_host_data_path();
    char ini_path[1024];
    SDL_snprintf(ini_path, sizeof(ini_path), "%sconfig.ini", base_path ? base_path : "");
    pvz2_config_load(ini_path, base_path ? base_path : "");

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (!arg || strncmp(arg, "--", 2) != 0) continue;
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) continue;

        const char *eq = strchr(arg + 2, '=');
        if (!eq || eq == arg + 2) {
            pvz2_log("[config] override ignored: %s (expected --key=value)", arg);
            continue;
        }

        char name[128];
        size_t name_len = (size_t)(eq - (arg + 2));
        if (name_len >= sizeof(name)) name_len = sizeof(name) - 1;
        memcpy(name, arg + 2, name_len);
        name[name_len] = '\0';

        const char *value = eq + 1;
        if (!pvz2_config_override(name, value)) {
            pvz2_log("[config] override ignored: %s (unknown/ambiguous or unsupported key)", arg);
        }
    }

    if (base_path) SDL_free(base_path);
    const pvz2_config_t *cfg = pvz2_config();
    const int show_console = cfg->console != 0 && cfg->log_file[0] == '\0';
    int hide_log_console = 0;
#ifdef _WIN32
    if (cfg->log_file[0] != '\0') hide_log_console = prepare_log_console();
#endif
    apply_text_log_silence_if_needed(cfg);
    apply_console_visibility(show_console);
    apply_logfile_redirect(cfg);
#ifdef _WIN32
    if (hide_log_console) {
        HWND console_window = GetConsoleWindow();
        if (console_window) ShowWindow(console_window, SW_HIDE);
    }
#endif
    g_live_window_title = show_console;
    load_runtime_shortcuts(cfg);
    pvz2_log("[path] so=%s", cfg->so_path);
    pvz2_log("[path] obb=%s", cfg->obb_path);
    if (!check_required_files(cfg)) {
        pvz2_host_pre_sdl_quit();
        SDL_Quit();
        return 1;
    }
    pvz2_fix_future_save_dates();

    /* PvZ2's .so only imports gl* symbols, never egl* -- on real Android the
     * Java-side GLSurfaceView owns EGL context creation and the native side
     * just receives an already-current context in onSurfaceCreated. SDL's
     * GL context here plays that same role. GL2.0 compatibility profile
     * (matches the root CMakeLists' `glad_add_library` config) keeps both the
     * fixed-function pipeline (matrix stack, client-state arrays) GLES1.1
     * code needs and the shader entry points GLES2 code needs. */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    /* Launch size from [video] (auto-aspect by default) and display mode -- see
     * gfx/video_mode. The touch space freezes on the FIRST pvz2_surface_set, so
     * that set is intentionally delayed until after any startup fullscreen switch,
     * then synced from the real drawable size before pvz2_session_start. */
    int launch_w = 0, launch_h = 0, start_fullscreen = 0;
    pvz2_choose_window_size(&launch_w, &launch_h, &start_fullscreen);
    pvz2_log("[video] launching at %dx%d%s", launch_w, launch_h,
             start_fullscreen ? " (fullscreen)" : "");
    const char *initial_title = g_live_window_title ? "PvZ2" : kGameTitle;
    const int launch_display = pvz2_effective_display();
    SDL_Window *window = SDL_CreateWindow(initial_title,
                                           SDL_WINDOWPOS_CENTERED_DISPLAY(launch_display),
                                           SDL_WINDOWPOS_CENTERED_DISPLAY(launch_display),
                                           launch_w, launch_h,
                                           SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!window) {
        pvz2_log("[sdl] SDL_CreateWindow failed: %s", SDL_GetError());
        pvz2_host_pre_sdl_quit();
        SDL_Quit();
        return 1;
    }
    apply_windows_assets(window);
    g_window = window;
    refresh_window_metrics();
    /* Freeze bind aspect from the real launch size (HUD layout is boot-locked). */
    latch_bind_aspect_from_size(launch_w, launch_h);
    load_runtime_shortcuts(cfg);
    pvz2_log("[shortcut] aspect: %s (launch %dx%d)",
             g_bind_use_43 ? "4:3" : "16:9", launch_w, launch_h);
    pvz2_gamepad_init(window, g_bind_use_43);

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    if (!gl_context) {
        pvz2_log("[sdl] SDL_GL_CreateContext failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        pvz2_host_pre_sdl_quit();
        SDL_Quit();
        return 1;
    }
    SDL_GL_MakeCurrent(window, gl_context);

    /* Cap the frame rate ([video] fps_limit, vsync + pacing -- see gfx/frame_limiter).
     * The engine advances its simulation from the real wall clock (gettimeofday),
     * not from a frame count, so presenting fewer frames does NOT slow the game
     * down -- it only stops the loop from spinning onDrawFrame thousands of times
     * a second and pinning a core at 100%. That is the single biggest win for a
     * weak CPU like the N2805: it drops the per-second work by roughly the ratio
     * of that uncapped rate to the capped one. */
    pvz2_frame_limit_init();

    int glad_version = gladLoadGL((GLADloadfunc)SDL_GL_GetProcAddress);
    if (glad_version == 0) {
        pvz2_log("[gl] gladLoadGL failed to load any GL functions");
        SDL_GL_DeleteContext(gl_context);
        SDL_DestroyWindow(window);
        pvz2_host_pre_sdl_quit();
        SDL_Quit();
        return 1;
    }
    pvz2_log("[gl] context ready: %s (loaded via glad %d.%d)", (const char *)glGetString(GL_VERSION),
             GLAD_VERSION_MAJOR(glad_version), GLAD_VERSION_MINOR(glad_version));

    /* The loading screen goes up BEFORE anything else that takes time (the GPU capability check, the
     * fullscreen switch, the boot). The window was created hidden, so the player never sees it blank: it
     * appears with the first frame already painted. */
    const int have_splash = load_embedded_splash_texture();
    paint_startup_frame(window, have_splash);
    SDL_ShowWindow(window);
    paint_startup_frame(window, have_splash); /* some drivers only present after the window is shown */
    pvz2_log("[boot] loading screen on screen at %u ms", (unsigned)SDL_GetTicks());

    /* Verify the GPU can actually run the engine (OpenGL 2.0 + framebuffer
     * objects) BEFORE loading the .so. A GPU that falls short is shown a message
     * box naming it and the versions, then the app exits cleanly instead of
     * booting for seconds only to render a silent black window. Running this also
     * latches the GLSL dialect the shader shim will emit (120 vs 130). */
    if (!pvz2_gl_check_requirements(window)) {
        SDL_GL_DeleteContext(gl_context);
        SDL_DestroyWindow(window);
        pvz2_host_pre_sdl_quit();
        SDL_Quit();
        return 1;
    }

    /* Apply the configured fullscreen state now that the window and context
     * exist; the resulting size change is handled like any other resize. */
    if (start_fullscreen) {
        g_fullscreen = 1;
        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    }

    /* Freeze touch-space against the final startup drawable (windowed or
     * fullscreen), then let later resizes use update_window_size(). */
    sync_initial_surface_size();

    /* Resolved from config.ini above: <exe>/libPVZ2.so by default, or the
     * [paths] so override. The matching .obb is resolved the same way and read
     * by the VFS layer from the config. */
    const char *so_path = cfg->so_path;

    /* The fullscreen switch and the resize above leave the window blank for a moment: repaint the loading
     * screen (already loaded) before the (multi-minute) boot. */
    paint_startup_frame(window, have_splash);

    pvz2_session_set_host_pump(host_pump);
    /* The loading screen is repainted from inside the boot many times a second: without vsync those presents do not
     * wait for the display and the boot is not slowed down. Back on when the loading screen is gone. */
    if (have_splash) pvz2_frame_limit_vsync_suspend();
    pvz2_session_t *session = pvz2_session_start(so_path);
    if (session) begin_boot_splash_fade();
    else destroy_boot_splash();
    /* Nonzero exit when the session never started -- a missing .so, or a build
     * of the game whose addresses we do not have (see src/game/symbols.cpp). */
    int exit_code = session ? 0 : 1;

    /* Boot is done: from here the title shows FPS, not boot progress. */
    g_booted = 1;
    config_screen_notify_booted();

    if (session) {
        /* The window's real drawable size (HiDPI can differ from the requested
         * size), handed to the compositor before the first frame. */
        update_window_size();

        /* host_pump may already have seen a close during the boot. */
        int running = !g_quit_requested;
        while (running) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                /* Title-bar X always exits; Escape opens settings, then quit-confirm.
                 * Both must be handled even while the overlay is open. */
                if (event.type == SDL_QUIT) {
                    handle_quit();
                    continue;
                }
                if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE &&
                    event.key.repeat == 0) {
                    config_screen_request_escape();
                    continue;
                }
                /* Window events (resize, focus loss, etc.) must always be
                 * processed regardless of whether the config overlay is open,
                 * so the game doesn't get stuck with a stale viewport. */
                if (event.type == SDL_WINDOWEVENT)
                    handle_sdl_event(&event);
                /* Gamepad first: while overlays are open it only drives the
                 * OS cursor + A click + Back; in-game actions stay suppressed. */
                if (pvz2_gamepad_handle_event(&event))
                    continue;
                /* Achievements screen takes priority over config screen for input. */
                if (achievements_screen_is_open()) {
                    achievements_screen_handle_event(&event);
                } else if (config_screen_is_open()) {
                    config_screen_handle_event(&event);
                } else if (event.type != SDL_WINDOWEVENT) {
                    handle_sdl_event(&event);
                }
            }
            {
                static Uint32 s_last_gp_ms = 0;
                const Uint32 now = SDL_GetTicks();
                float dt = 1.0f / 60.0f;
                if (s_last_gp_ms != 0) {
                    dt = (float)(now - s_last_gp_ms) / 1000.0f;
                    if (dt < 0.0f) dt = 0.0f;
                    if (dt > 0.1f) dt = 0.1f;
                }
                s_last_gp_ms = now;
                pvz2_gamepad_tick(dt);
            }
            if (!config_screen_is_open() && !achievements_screen_is_open()) {
                process_mouse_wheel_scroll();
                maybe_force_touch_recovery();
                sync_text_input();
            }
            apply_pending_resize_sync(0);
            if (!running || g_quit_requested) break;

            /* The guest renders its scene into an offscreen FBO and composites
             * it to the default framebuffer with a degenerate (zero-width)
             * viewport, so it never actually writes the backbuffer. With
             * double buffering, swapping every frame then alternates between
             * two buffers holding different stale content, which reads on
             * screen as flicker. Clearing here gives a stable window until the
             * composite works; the guest binds its own FBO immediately after,
             * so this does not disturb its rendering. */
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glClearColor(0.06f, 0.07f, 0.10f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            /* The guest issues its GL calls into the context made current on
             * this thread, so the frame has to run here, between the pump and
             * the swap -- not on a worker. */
            if (!pvz2_session_frame(session)) break;
            maybe_finish_pending_keyboard_hide();
            if (g_boot_overlay_active) draw_boot_splash_fade(window);
            /* Draw config screen overlay on top of the game frame (before swap) */
            if (config_screen_is_open()) config_screen_draw(window);
            /* Achievements: popup draws always (if animating), screen on top of config */
            achievements_draw(window);
            SDL_GL_SwapWindow(window);
            update_fps_title();
            /* After the swap and the title, so the wait is the last thing in the
             * frame and the FPS shown is measured over the paced interval. */
            pvz2_frame_limit_wait();
        }
        pvz2_session_end(session);
    }

    pvz2_gamepad_shutdown();
    achievements_shutdown();
    /* Cursor / UI audio must go before DestroyWindow + SDL_Quit (X11/Pulse). */
    pvz2_host_pre_sdl_quit();
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return exit_code;
}
