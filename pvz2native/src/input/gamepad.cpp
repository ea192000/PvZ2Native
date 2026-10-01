#include "pvz2native/input/gamepad.h"

#include "pvz2native/config.h"
#include "pvz2native/config_screen.h"
#include "pvz2native/achievements.h"
#include "pvz2native/input/input_queue.h"
#include "pvz2native/surface.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

enum class MenuId : int {
    Top = 0,
    Left = 1,
    Bottom = 2,
    Board = 3,
    Count = 4,
};

enum class DpadDir : int { None = 0, Up, Down, Left, Right };

struct FocusPoint {
    std::string name;
    float nx = 0.0f;
    float ny = 0.0f;
    MenuId menu = MenuId::Top;
    int row = 0;
    int col = 0;
};

struct DragGestureState {
    bool active = false;
    int step = 0;
    int start_x = 0;
    int start_y = 0;
    int end_x = 0;
    int end_y = 0;
    bool horizontal = false;
};

static const int kDragRepeats = 6;
static const int kDragBorderPad = 10;
static const float kDragCooldownSec = 0.10f;
static const float kDpadRepeatInitialSec = 0.18f;
static const float kDpadRepeatMinSec = 0.14f;
static const float kDpadRepeatAccelSec = 0.010f;

class GamepadModule {
public:
    void init(SDL_Window *window, int use_43);
    void shutdown();
    void reload();
    int handle_event(const SDL_Event *e);
    void tick(float dt_sec);

private:
    void try_open_controller();
    void rebuild_focus_points();
    void rebuild_bind_map();
    void refresh_config();

    MenuId classify_shortcut_menu(const char *name) const;
    const FocusPoint *find_board_cell(int row, int col) const;
    void get_default_menu_pos(MenuId menu, int *wx, int *wy) const;
    void goto_menu(MenuId target);
    void dpad_jump(DpadDir dir);
    void set_focus_index(int idx);
    int find_nearest_focus(float nx, float ny, MenuId only_menu, bool filter_menu) const;
    int find_focus_by_name(const char *name) const;
    void on_button_a_press();
    void on_button_a_release();
    void tap_bind(const char *bind_name);
    void inject_touch_down_at_cursor();
    void inject_touch_up_at_cursor();
    void inject_ui_mouse_down_at_cursor();
    void inject_ui_mouse_up_at_cursor();
    void warp_cursor(int wx, int wy);
    void norm_to_window(float nx, float ny, int *wx, int *wy) const;
    void cursor_to_norm(float *nx, float *ny) const;
    int map_window_to_touch(int wx, int wy, int *tx, int *ty) const;
    int map_norm_to_touch(float nx, float ny, int *tx, int *ty) const;
    void clamp_to_window(int *wx, int *wy) const;

    int handle_controller_button(const SDL_Event *e);
    DpadDir dpad_from_button(Uint8 button) const;
    void on_dpad_press(DpadDir dir);
    void on_dpad_release(DpadDir dir);
    void tick_left_stick(float dt_sec);
    void tick_right_stick(float dt_sec);
    void tick_right_stick_ui(float dt_sec);
    void tick_triggers();
    void tick_dpad_repeat(float dt_sec);
    void start_drag_gesture(int dir_x, int dir_y);
    void process_drag_gesture();
    void apply_border_pad(int touch_w, int touch_h, int *x, int *y) const;

    bool is_our_controller_instance(SDL_JoystickID instance_id) const;
    bool controller_ready() const;
    bool overlay_open() const;

    SDL_Window *window_ = nullptr;
    SDL_GameController *controller_ = nullptr;
    int use_43_ = 0;
    int enabled_ = 0;
    int port_ = 0;
    float stick_speed_ = 1.6f;
    float stick_deadzone_ = 0.25f;
    float trigger_threshold_ = 0.50f;

    int cursor_x_ = 0;
    int cursor_y_ = 0;
    bool a_held_ = false;
    bool a_held_was_snap_ = false;
    MenuId active_menu_ = MenuId::Board;
    int current_focus_ = -1; /* index into focus_points_; source of truth for D-pad */
    bool snap_nav_ = false;  /* true after D-pad / shoulder menu jump; cleared by left stick */
    int menu_last_focus_[static_cast<int>(MenuId::Count)];
    int menu_last_x_[static_cast<int>(MenuId::Count)] = {};
    int menu_last_y_[static_cast<int>(MenuId::Count)] = {};
    bool menu_visited_[static_cast<int>(MenuId::Count)] = {};

    std::vector<FocusPoint> focus_points_;
    std::unordered_map<SDL_GameControllerButton, std::string> bind_by_button_;

    DpadDir dpad_held_ = DpadDir::None;
    float dpad_repeat_timer_ = 0.0f;
    float dpad_repeat_interval_ = kDpadRepeatInitialSec;

    bool l2_prev_ = false;
    bool r2_prev_ = false;
    float drag_cooldown_ = 0.0f;
    DragGestureState drag_;
};

GamepadModule g_gamepad;

static float axis_to_float(Sint16 v) {
    const float denom = 32767.0f;
    return std::max(-1.0f, std::min(1.0f, static_cast<float>(v) / denom));
}

static float trigger_to_float(Sint16 v) {
    return std::max(0.0f, std::min(1.0f, static_cast<float>(v) / 32767.0f));
}

static float clamp01(float v) {
    return std::max(0.0f, std::min(1.0f, v));
}

static SDL_GameControllerButton button_name_to_sdl(const char *name) {
    if (!name || name[0] == '\0') return static_cast<SDL_GameControllerButton>(-1);
    if (SDL_strcasecmp(name, "start") == 0) return SDL_CONTROLLER_BUTTON_START;
    if (SDL_strcasecmp(name, "back") == 0) return SDL_CONTROLLER_BUTTON_BACK;
    if (SDL_strcasecmp(name, "b") == 0) return SDL_CONTROLLER_BUTTON_B;
    if (SDL_strcasecmp(name, "x") == 0) return SDL_CONTROLLER_BUTTON_X;
    if (SDL_strcasecmp(name, "y") == 0) return SDL_CONTROLLER_BUTTON_Y;
    if (SDL_strcasecmp(name, "l3") == 0) return SDL_CONTROLLER_BUTTON_LEFTSTICK;
    if (SDL_strcasecmp(name, "r3") == 0) return SDL_CONTROLLER_BUTTON_RIGHTSTICK;
    if (SDL_strcasecmp(name, "a") == 0) return SDL_CONTROLLER_BUTTON_A;
    if (SDL_strcasecmp(name, "guide") == 0) return SDL_CONTROLLER_BUTTON_GUIDE;
    return static_cast<SDL_GameControllerButton>(-1);
}

bool GamepadModule::controller_ready() const {
    return enabled_ != 0 && controller_ != nullptr;
}

bool GamepadModule::overlay_open() const {
    /* Settings and quit-confirm both report open via this flag. */
    return config_screen_is_open() != 0;
}

bool GamepadModule::is_our_controller_instance(SDL_JoystickID instance_id) const {
    if (!controller_) return false;
    SDL_Joystick *joy = SDL_GameControllerGetJoystick(controller_);
    if (!joy) return false;
    return SDL_JoystickInstanceID(joy) == instance_id;
}

void GamepadModule::refresh_config() {
    const pvz2_config_t *cfg = pvz2_config();
    enabled_ = cfg->gamepad_enabled;
    port_ = cfg->gamepad_port;
    stick_speed_ = cfg->gamepad_stick_speed;
    if (stick_speed_ <= 0.0f) stick_speed_ = 1.6f;
    stick_deadzone_ = cfg->gamepad_stick_deadzone;
    if (stick_deadzone_ < 0.0f || stick_deadzone_ > 0.9f) stick_deadzone_ = 0.25f;
    trigger_threshold_ = cfg->gamepad_trigger_threshold;
    if (trigger_threshold_ <= 0.0f) trigger_threshold_ = 0.50f;
}

void GamepadModule::init(SDL_Window *window, int use_43) {
    window_ = window;
    use_43_ = use_43 ? 1 : 0;
    refresh_config();
    rebuild_focus_points();
    rebuild_bind_map();

    std::printf("[gamepad] init enabled=%d port=%d stick_speed=%.2f deadzone=%.2f "
                "board_cells=%u binds=%u focus=%zu\n",
                enabled_, port_, stick_speed_, stick_deadzone_,
                pvz2_config()->board_count, pvz2_config()->gamepad_bind_count,
                focus_points_.size());

    if (window_) {
        SDL_GetMouseState(&cursor_x_, &cursor_y_);
    }

    active_menu_ = MenuId::Board;
    current_focus_ = -1;
    snap_nav_ = false;
    for (int i = 0; i < static_cast<int>(MenuId::Count); ++i) menu_last_focus_[i] = -1;
    std::memset(menu_last_x_, 0, sizeof(menu_last_x_));
    std::memset(menu_last_y_, 0, sizeof(menu_last_y_));
    std::memset(menu_visited_, 0, sizeof(menu_visited_));

    dpad_held_ = DpadDir::None;
    dpad_repeat_timer_ = 0.0f;
    dpad_repeat_interval_ = kDpadRepeatInitialSec;
    l2_prev_ = false;
    r2_prev_ = false;
    drag_cooldown_ = 0.0f;
    drag_ = {};
    a_held_ = false;
    a_held_was_snap_ = false;

    if (!enabled_) {
        std::printf("[gamepad] disabled in config ([gamepad] enabled = 0)\n");
        return;
    }

    try_open_controller();
}

void GamepadModule::shutdown() {
    if (a_held_) on_button_a_release();
    if (controller_) {
        SDL_GameControllerClose(controller_);
        controller_ = nullptr;
    }
    window_ = nullptr;
}

void GamepadModule::reload() {
    const int prev_enabled = enabled_;
    const int prev_port = port_;
    refresh_config();
    rebuild_focus_points();
    rebuild_bind_map();

    /* Only reopen when the selected device / enabled flag changes. Stick
     * speed & deadzone tweaks just need refresh_config above. */
    if (enabled_ == prev_enabled && port_ == prev_port &&
        (controller_ != nullptr || !enabled_)) {
        return;
    }

    if (a_held_) on_button_a_release();
    if (controller_) {
        SDL_GameControllerClose(controller_);
        controller_ = nullptr;
    }
    if (enabled_) try_open_controller();
}

void GamepadModule::try_open_controller() {
    if (!enabled_ || controller_) return;

    SDL_JoystickEventState(SDL_ENABLE);
    SDL_GameControllerEventState(SDL_ENABLE);

    const int n = SDL_NumJoysticks();
    std::printf("[gamepad] devices=%d preferred_port=%d\n", n, port_);
    for (int i = 0; i < n; ++i) {
        const char *name = SDL_JoystickNameForIndex(i);
        const int is_gc = SDL_IsGameController(i) ? 1 : 0;
        std::printf("[gamepad]   [%d] \"%s\"%s\n", i, name ? name : "?",
                    is_gc ? " [GameController]" : " [joystick only]");
    }

    auto try_index = [&](int index) -> bool {
        if (index < 0 || index >= n) return false;
        if (!SDL_IsGameController(index)) return false;
        SDL_GameController *c = SDL_GameControllerOpen(index);
        if (!c) return false;
        if (!SDL_GameControllerGetAttached(c)) {
            SDL_GameControllerClose(c);
            return false;
        }
        controller_ = c;
        const char *name = SDL_GameControllerName(c);
        std::printf("[gamepad] using \"%s\" (device index %d)\n", name ? name : "?", index);
        return true;
    };

    /* 1) Exact device index from config (Xbox wireless receivers often need 1..3). */
    if (try_index(port_)) return;

    /* 2) Treat port as Nth GameController among mapped pads (0 = first GC). */
    int gc_ordinal = 0;
    for (int i = 0; i < n; ++i) {
        if (!SDL_IsGameController(i)) continue;
        if (gc_ordinal == port_ && try_index(i)) return;
        ++gc_ordinal;
    }

    /* 3) First usable GameController on any index. */
    for (int i = 0; i < n; ++i) {
        if (try_index(i)) return;
    }

    std::printf("[gamepad] no usable GameController found "
                "(wireless 360: try port=1..3, or check xpad/xboxdrv)\n");
}

MenuId GamepadModule::classify_shortcut_menu(const char *name) const {
    if (!name) return MenuId::Top;
    if (std::strncmp(name, "plant_", 6) == 0 || std::strcmp(name, "replay") == 0) return MenuId::Left;
    if (std::strcmp(name, "back") == 0 || std::strcmp(name, "almanaq") == 0 ||
        std::strcmp(name, "connect") == 0 || std::strcmp(name, "vasebreaker") == 0 ||
        std::strcmp(name, "zen_garden") == 0 || std::strcmp(name, "speed") == 0 ||
        std::strcmp(name, "pause") == 0) {
        return MenuId::Top;
    }
    if (std::strcmp(name, "plant_food") == 0 || std::strncmp(name, "power_", 6) == 0 ||
        std::strcmp(name, "shovel") == 0) {
        return MenuId::Bottom;
    }
    return MenuId::Top;
}

void GamepadModule::rebuild_focus_points() {
    focus_points_.clear();
    const pvz2_config_t *cfg = pvz2_config();

    for (unsigned i = 0; i < cfg->shortcut_count; ++i) {
        const pvz2_shortcut_binding_t *in = &cfg->shortcuts[i];
        FocusPoint fp;
        fp.name = in->name;
        if (use_43_ && in->has_43) {
            fp.nx = in->x43;
            fp.ny = in->y43;
        } else {
            fp.nx = in->x;
            fp.ny = in->y;
        }
        fp.menu = classify_shortcut_menu(in->name);
        focus_points_.push_back(fp);
    }

    for (unsigned i = 0; i < cfg->board_count; ++i) {
        const pvz2_board_cell_t *cell = &cfg->board[i];
        FocusPoint fp;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "position_%d_%d", cell->row, cell->col);
        fp.name = buf;
        if (use_43_ && cell->has_43) {
            fp.nx = cell->x43;
            fp.ny = cell->y43;
        } else {
            fp.nx = cell->x;
            fp.ny = cell->y;
        }
        fp.menu = MenuId::Board;
        fp.row = cell->row;
        fp.col = cell->col;
        focus_points_.push_back(fp);
    }

    current_focus_ = -1;
    snap_nav_ = false;
    for (int i = 0; i < static_cast<int>(MenuId::Count); ++i) menu_last_focus_[i] = -1;
}

void GamepadModule::rebuild_bind_map() {
    bind_by_button_.clear();
    const pvz2_config_t *cfg = pvz2_config();

    for (unsigned i = 0; i < cfg->gamepad_bind_count; ++i) {
        const pvz2_gamepad_bind_t *entry = &cfg->gamepad_binds[i];
        if (!entry->bind_name[0]) continue;
        if (SDL_strcasecmp(entry->bind_name, "NULL") == 0) continue;

        const SDL_GameControllerButton btn = button_name_to_sdl(entry->button);
        if (btn == static_cast<SDL_GameControllerButton>(-1)) continue;
        /* Reserved controls stay in code paths, not remappable here. */
        if (btn == SDL_CONTROLLER_BUTTON_A || btn == SDL_CONTROLLER_BUTTON_BACK ||
            btn == SDL_CONTROLLER_BUTTON_LEFTSHOULDER ||
            btn == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER ||
            btn == SDL_CONTROLLER_BUTTON_DPAD_UP || btn == SDL_CONTROLLER_BUTTON_DPAD_DOWN ||
            btn == SDL_CONTROLLER_BUTTON_DPAD_LEFT || btn == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
            continue;
        }

        bind_by_button_[btn] = entry->bind_name;
    }
}

const FocusPoint *GamepadModule::find_board_cell(int row, int col) const {
    for (const FocusPoint &fp : focus_points_) {
        if (fp.menu == MenuId::Board && fp.row == row && fp.col == col) return &fp;
    }
    return nullptr;
}

void GamepadModule::norm_to_window(float nx, float ny, int *wx, int *wy) const {
    int ww = 0;
    int wh = 0;
    if (window_) SDL_GetWindowSize(window_, &ww, &wh);
    if (ww <= 0) ww = static_cast<int>(pvz2_surface_width());
    if (wh <= 0) wh = static_cast<int>(pvz2_surface_height());

    const float denom_w = (ww > 1) ? static_cast<float>(ww - 1) : 1.0f;
    const float denom_h = (wh > 1) ? static_cast<float>(wh - 1) : 1.0f;
    *wx = static_cast<int>(clamp01(nx) * denom_w + 0.5f);
    *wy = static_cast<int>(clamp01(ny) * denom_h + 0.5f);
}

void GamepadModule::cursor_to_norm(float *nx, float *ny) const {
    int ww = 0;
    int wh = 0;
    if (window_) SDL_GetWindowSize(window_, &ww, &wh);
    if (ww <= 0) ww = static_cast<int>(pvz2_surface_width());
    if (wh <= 0) wh = static_cast<int>(pvz2_surface_height());

    const float denom_w = (ww > 1) ? static_cast<float>(ww - 1) : 1.0f;
    const float denom_h = (wh > 1) ? static_cast<float>(wh - 1) : 1.0f;
    *nx = static_cast<float>(cursor_x_) / denom_w;
    *ny = static_cast<float>(cursor_y_) / denom_h;
}

void GamepadModule::clamp_to_window(int *wx, int *wy) const {
    int ww = 0;
    int wh = 0;
    if (window_) SDL_GetWindowSize(window_, &ww, &wh);
    if (ww <= 0) ww = static_cast<int>(pvz2_surface_width());
    if (wh <= 0) wh = static_cast<int>(pvz2_surface_height());

    if (ww > 0) *wx = std::max(0, std::min(*wx, ww - 1));
    if (wh > 0) *wy = std::max(0, std::min(*wy, wh - 1));
}

void GamepadModule::warp_cursor(int wx, int wy) {
    clamp_to_window(&wx, &wy);
    cursor_x_ = wx;
    cursor_y_ = wy;
    if (window_) SDL_WarpMouseInWindow(window_, wx, wy);

    /* Keep the guest touch in sync while A is held — D-pad snaps and stick
     * moves both go through here, so drag/hold gestures track the cursor. */
    if (a_held_ && !overlay_open()) {
        int tx = 0;
        int ty = 0;
        if (map_window_to_touch(cursor_x_, cursor_y_, &tx, &ty))
            pvz2_input_push_touch(PVZ2_TOUCH_MOVE, tx, ty);
    }
}

int GamepadModule::map_window_to_touch(int wx, int wy, int *tx, int *ty) const {
    int ww = 0;
    int wh = 0;
    if (window_) SDL_GetWindowSize(window_, &ww, &wh);
    if (ww <= 0) ww = static_cast<int>(pvz2_surface_width());
    if (wh <= 0) wh = static_cast<int>(pvz2_surface_height());

    const float tw = static_cast<float>(pvz2_touch_space_width());
    const float th = static_cast<float>(pvz2_touch_space_height());
    if (ww <= 0 || wh <= 0 || tw <= 0.0f || th <= 0.0f) return 0;
    if (wx < 0 || wy < 0 || wx >= ww || wy >= wh) return 0;

    const float denom_w = (ww > 1) ? static_cast<float>(ww - 1) : 1.0f;
    const float denom_h = (wh > 1) ? static_cast<float>(wh - 1) : 1.0f;
    *tx = static_cast<int>((static_cast<float>(wx) / denom_w) * (tw - 1.0f) + 0.5f);
    *ty = static_cast<int>((static_cast<float>(wy) / denom_h) * (th - 1.0f) + 0.5f);
    return 1;
}

int GamepadModule::map_norm_to_touch(float nx, float ny, int *tx, int *ty) const {
    const float tw = static_cast<float>(pvz2_touch_space_width());
    const float th = static_cast<float>(pvz2_touch_space_height());
    if (tw <= 0.0f || th <= 0.0f) return 0;

    nx = clamp01(nx);
    ny = clamp01(ny);
    *tx = static_cast<int>(nx * (tw - 1.0f) + 0.5f);
    *ty = static_cast<int>(ny * (th - 1.0f) + 0.5f);
    return 1;
}

void GamepadModule::get_default_menu_pos(MenuId menu, int *wx, int *wy) const {
    if (menu == MenuId::Board) {
        if (const FocusPoint *fp = find_board_cell(1, 1)) {
            norm_to_window(fp->nx, fp->ny, wx, wy);
            return;
        }
        for (const FocusPoint &fp : focus_points_) {
            if (fp.menu == MenuId::Board) {
                norm_to_window(fp.nx, fp.ny, wx, wy);
                return;
            }
        }
    } else if (menu == MenuId::Left) {
        for (const FocusPoint &fp : focus_points_) {
            if (fp.menu == MenuId::Left && fp.name == "plant_1") {
                norm_to_window(fp.nx, fp.ny, wx, wy);
                return;
            }
        }
        for (const FocusPoint &fp : focus_points_) {
            if (fp.menu == MenuId::Left) {
                norm_to_window(fp.nx, fp.ny, wx, wy);
                return;
            }
        }
    } else {
        for (const FocusPoint &fp : focus_points_) {
            if (fp.menu == menu) {
                norm_to_window(fp.nx, fp.ny, wx, wy);
                return;
            }
        }
    }

    norm_to_window(0.5f, 0.5f, wx, wy);
}

void GamepadModule::set_focus_index(int idx) {
    if (idx < 0 || idx >= static_cast<int>(focus_points_.size())) return;

    const int from = static_cast<int>(active_menu_);
    menu_last_x_[from] = cursor_x_;
    menu_last_y_[from] = cursor_y_;
    menu_last_focus_[from] = current_focus_;
    menu_visited_[from] = true;

    current_focus_ = idx;
    snap_nav_ = true;
    const FocusPoint &fp = focus_points_[static_cast<size_t>(idx)];
    active_menu_ = fp.menu;

    int wx = 0;
    int wy = 0;
    norm_to_window(fp.nx, fp.ny, &wx, &wy);
    warp_cursor(wx, wy);

    const int to = static_cast<int>(active_menu_);
    menu_visited_[to] = true;
    menu_last_x_[to] = wx;
    menu_last_y_[to] = wy;
    menu_last_focus_[to] = idx;
}

int GamepadModule::find_focus_by_name(const char *name) const {
    if (!name) return -1;
    for (int i = 0; i < static_cast<int>(focus_points_.size()); ++i) {
        if (focus_points_[static_cast<size_t>(i)].name == name) return i;
    }
    return -1;
}

int GamepadModule::find_nearest_focus(float nx, float ny, MenuId only_menu, bool filter_menu) const {
    int best = -1;
    float best_d = 1.0e9f;
    for (int i = 0; i < static_cast<int>(focus_points_.size()); ++i) {
        const FocusPoint &fp = focus_points_[static_cast<size_t>(i)];
        if (filter_menu && fp.menu != only_menu) continue;
        const float dx = fp.nx - nx;
        const float dy = fp.ny - ny;
        const float d = dx * dx + dy * dy;
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

void GamepadModule::goto_menu(MenuId target) {
    const int from = static_cast<int>(active_menu_);
    menu_last_x_[from] = cursor_x_;
    menu_last_y_[from] = cursor_y_;
    menu_last_focus_[from] = current_focus_;
    menu_visited_[from] = true;

    int idx = -1;
    const int to = static_cast<int>(target);
    if (menu_visited_[to] && menu_last_focus_[to] >= 0 &&
        menu_last_focus_[to] < static_cast<int>(focus_points_.size()) &&
        focus_points_[static_cast<size_t>(menu_last_focus_[to])].menu == target) {
        idx = menu_last_focus_[to];
    } else if (target == MenuId::Board) {
        idx = find_focus_by_name("position_1_1");
        if (idx < 0) idx = find_nearest_focus(0.5f, 0.5f, MenuId::Board, true);
    } else if (target == MenuId::Left) {
        idx = find_focus_by_name("plant_1");
        if (idx < 0) idx = find_nearest_focus(0.05f, 0.16f, MenuId::Left, true);
    } else {
        idx = find_nearest_focus(0.5f, 0.5f, target, true);
    }

    if (idx < 0) {
        /* Fallback: any point tagged with that menu. */
        for (int i = 0; i < static_cast<int>(focus_points_.size()); ++i) {
            if (focus_points_[static_cast<size_t>(i)].menu == target) {
                idx = i;
                break;
            }
        }
    }

    if (idx >= 0) {
        set_focus_index(idx);
    } else {
        active_menu_ = target;
        int wx = 0, wy = 0;
        get_default_menu_pos(target, &wx, &wy);
        warp_cursor(wx, wy);
        menu_visited_[to] = true;
        menu_last_x_[to] = wx;
        menu_last_y_[to] = wy;
    }
}

void GamepadModule::dpad_jump(DpadDir dir) {
    if (dir == DpadDir::None || focus_points_.empty()) return;

    /* Navigate from the focused node (exact coords), not the pixel cursor —
     * warping rounds to integers and euclidean-from-cursor caused stuck loops
     * and jumps into the board from the top bar. */
    float cnx = 0.0f;
    float cny = 0.0f;
    int from_idx = current_focus_;
    if (from_idx < 0 || from_idx >= static_cast<int>(focus_points_.size())) {
        cursor_to_norm(&cnx, &cny);
        from_idx = find_nearest_focus(cnx, cny, MenuId::Top, false);
        if (from_idx >= 0) {
            cnx = focus_points_[static_cast<size_t>(from_idx)].nx;
            cny = focus_points_[static_cast<size_t>(from_idx)].ny;
            current_focus_ = from_idx;
            active_menu_ = focus_points_[static_cast<size_t>(from_idx)].menu;
        } else {
            return;
        }
    } else {
        cnx = focus_points_[static_cast<size_t>(from_idx)].nx;
        cny = focus_points_[static_cast<size_t>(from_idx)].ny;
    }

    /* Must move clearly along the pressed axis; heavy penalty on the cross axis
     * so a vertical plant column / horizontal top row wins over diagonal board hits. */
    static const float kEps = 0.015f;
    static const float kCrossWeight = 5.0f;

    int best = -1;
    float best_score = 1.0e9f;

    for (int i = 0; i < static_cast<int>(focus_points_.size()); ++i) {
        if (i == from_idx) continue;
        const FocusPoint &fp = focus_points_[static_cast<size_t>(i)];
        const float dx = fp.nx - cnx;
        const float dy = fp.ny - cny;

        float primary = 0.0f;
        float cross = 0.0f;
        switch (dir) {
            case DpadDir::Left:
                if (dx >= -kEps) continue;
                primary = -dx;
                cross = std::fabs(dy);
                break;
            case DpadDir::Right:
                if (dx <= kEps) continue;
                primary = dx;
                cross = std::fabs(dy);
                break;
            case DpadDir::Up:
                if (dy >= -kEps) continue;
                primary = -dy;
                cross = std::fabs(dx);
                break;
            case DpadDir::Down:
                if (dy <= kEps) continue;
                primary = dy;
                cross = std::fabs(dx);
                break;
            default:
                continue;
        }

        const float score = primary + kCrossWeight * cross;
        if (score < best_score) {
            best_score = score;
            best = i;
        }
    }

    if (best >= 0) set_focus_index(best);
}

void GamepadModule::inject_touch_down_at_cursor() {
    if (window_) SDL_GetMouseState(&cursor_x_, &cursor_y_);
    int tx = 0;
    int ty = 0;
    if (!map_window_to_touch(cursor_x_, cursor_y_, &tx, &ty)) return;
    pvz2_input_push_touch(PVZ2_TOUCH_DOWN, tx, ty);
}

void GamepadModule::inject_touch_up_at_cursor() {
    if (window_) SDL_GetMouseState(&cursor_x_, &cursor_y_);
    int tx = 0;
    int ty = 0;
    if (!map_window_to_touch(cursor_x_, cursor_y_, &tx, &ty)) return;
    pvz2_input_push_touch(PVZ2_TOUCH_UP, tx, ty);
}

void GamepadModule::inject_ui_mouse_down_at_cursor() {
    if (window_) SDL_GetMouseState(&cursor_x_, &cursor_y_);

    SDL_Event down;
    std::memset(&down, 0, sizeof(down));
    down.type = SDL_MOUSEBUTTONDOWN;
    down.button.timestamp = SDL_GetTicks();
    down.button.windowID = window_ ? SDL_GetWindowID(window_) : 0;
    down.button.which = 0;
    down.button.button = SDL_BUTTON_LEFT;
    down.button.state = SDL_PRESSED;
    down.button.clicks = 1;
    down.button.x = cursor_x_;
    down.button.y = cursor_y_;
    if (achievements_screen_is_open())
        achievements_screen_handle_event(&down);
    else
        config_screen_handle_event(&down);
}

void GamepadModule::inject_ui_mouse_up_at_cursor() {
    if (window_) SDL_GetMouseState(&cursor_x_, &cursor_y_);

    SDL_Event up;
    std::memset(&up, 0, sizeof(up));
    up.type = SDL_MOUSEBUTTONUP;
    up.button.timestamp = SDL_GetTicks();
    up.button.windowID = window_ ? SDL_GetWindowID(window_) : 0;
    up.button.which = 0;
    up.button.button = SDL_BUTTON_LEFT;
    up.button.state = SDL_RELEASED;
    up.button.clicks = 1;
    up.button.x = cursor_x_;
    up.button.y = cursor_y_;
    if (achievements_screen_is_open())
        achievements_screen_handle_event(&up);
    else
        config_screen_handle_event(&up);
}

void GamepadModule::on_button_a_press() {
    if (a_held_) return;
    a_held_ = true;
    a_held_was_snap_ = snap_nav_ && active_menu_ != MenuId::Board;

    if (overlay_open()) {
        inject_ui_mouse_down_at_cursor();
        return;
    }
    inject_touch_down_at_cursor();
}

void GamepadModule::on_button_a_release() {
    if (!a_held_) return;
    a_held_ = false;

    if (overlay_open()) {
        inject_ui_mouse_up_at_cursor();
        a_held_was_snap_ = false;
        return;
    }

    inject_touch_up_at_cursor();

    /* Auto-return to the board only after releasing A, and only when the
     * press started from a fixed D-pad / L-R focus (not free analog aim). */
    if (a_held_was_snap_) {
        a_held_was_snap_ = false;
        const int from = static_cast<int>(active_menu_);
        menu_last_focus_[from] = current_focus_;
        menu_last_x_[from] = cursor_x_;
        menu_last_y_[from] = cursor_y_;
        menu_visited_[from] = true;
        goto_menu(MenuId::Board);
    }
}

void GamepadModule::tap_bind(const char *bind_name) {
    if (!bind_name || !bind_name[0]) return;
    if (SDL_strcasecmp(bind_name, "NULL") == 0) return;

    const char *lookup = bind_name;
    if (std::strncmp(bind_name, "bind_", 5) == 0) lookup = bind_name + 5;

    const pvz2_config_t *cfg = pvz2_config();
    const pvz2_shortcut_binding_t *shortcut = nullptr;
    for (unsigned i = 0; i < cfg->shortcut_count; ++i) {
        if (std::strcmp(cfg->shortcuts[i].name, lookup) == 0) {
            shortcut = &cfg->shortcuts[i];
            break;
        }
    }
    if (!shortcut) return;

    float nx = 0.0f;
    float ny = 0.0f;
    if (use_43_ && shortcut->has_43) {
        nx = shortcut->x43;
        ny = shortcut->y43;
    } else {
        nx = shortcut->x;
        ny = shortcut->y;
    }

    int tx = 0;
    int ty = 0;
    if (!map_norm_to_touch(nx, ny, &tx, &ty)) return;
    pvz2_input_push_touch(PVZ2_TOUCH_DOWN, tx, ty);
    pvz2_input_push_touch(PVZ2_TOUCH_UP, tx, ty);
}

DpadDir GamepadModule::dpad_from_button(Uint8 button) const {
    switch (static_cast<SDL_GameControllerButton>(button)) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            return DpadDir::Up;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            return DpadDir::Down;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            return DpadDir::Left;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            return DpadDir::Right;
        default:
            return DpadDir::None;
    }
}

void GamepadModule::on_dpad_press(DpadDir dir) {
    if (dir == DpadDir::None) return;
    dpad_held_ = dir;
    dpad_repeat_timer_ = 0.0f;
    dpad_repeat_interval_ = kDpadRepeatInitialSec;
    dpad_jump(dir);
}

void GamepadModule::on_dpad_release(DpadDir dir) {
    if (dpad_held_ == dir) dpad_held_ = DpadDir::None;
}

int GamepadModule::handle_controller_button(const SDL_Event *e) {
    if (!controller_ready()) return 0;
    if (!is_our_controller_instance(e->cbutton.which)) return 0;

    const bool down = e->type == SDL_CONTROLLERBUTTONDOWN;
    const SDL_GameControllerButton button = static_cast<SDL_GameControllerButton>(e->cbutton.button);

    /* Overlay UI: cursor + A hold-click + Back/Escape toggle only. */
    if (overlay_open()) {
        if (button == SDL_CONTROLLER_BUTTON_A) {
            if (down) on_button_a_press();
            else on_button_a_release();
            return 1;
        }
        if (!down) return 1;
        if (button == SDL_CONTROLLER_BUTTON_BACK) {
            config_screen_request_quit_confirm();
            return 1;
        }
        return 1;
    }

    const DpadDir dpad = dpad_from_button(e->cbutton.button);
    if (dpad != DpadDir::None) {
        if (down) {
            on_dpad_press(dpad);
        } else {
            on_dpad_release(dpad);
        }
        return 1;
    }

    if (button == SDL_CONTROLLER_BUTTON_A) {
        if (down) on_button_a_press();
        else on_button_a_release();
        return 1;
    }

    if (!down) return 0;

    switch (button) {
        case SDL_CONTROLLER_BUTTON_BACK:
            config_screen_request_quit_confirm();
            return 1;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            goto_menu(MenuId::Top);
            return 1;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            goto_menu(MenuId::Bottom);
            return 1;
        default:
            break;
    }

    const auto it = bind_by_button_.find(button);
    if (it != bind_by_button_.end()) {
        tap_bind(it->second.c_str());
        return 1;
    }

    return 0;
}

int GamepadModule::handle_event(const SDL_Event *e) {
    if (!enabled_) return 0;

    switch (e->type) {
        case SDL_CONTROLLERDEVICEADDED:
            /* Rescan: wireless receivers add/remove slots dynamically. */
            if (!controller_) try_open_controller();
            return 0;
        case SDL_CONTROLLERDEVICEREMOVED:
            if (controller_ && is_our_controller_instance(e->cdevice.which)) {
                if (a_held_) on_button_a_release();
                if (pvz2_config()->verbose) std::printf("[gamepad] disconnected (port %d)\n", port_);
                SDL_GameControllerClose(controller_);
                controller_ = nullptr;
            }
            return 0;
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP:
            return handle_controller_button(e);
        default:
            return 0;
    }
}

void GamepadModule::tick_left_stick(float dt_sec) {
    Sint16 ax = SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_LEFTX);
    Sint16 ay = SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_LEFTY);

    float sx = axis_to_float(ax);
    float sy = axis_to_float(ay);

    if (std::fabs(sx) < stick_deadzone_) sx = 0.0f;
    if (std::fabs(sy) < stick_deadzone_) sy = 0.0f;
    if (sx == 0.0f && sy == 0.0f) return;

    /* Free aim: leave snap/D-pad focus mode so A no longer teleports to board. */
    snap_nav_ = false;
    current_focus_ = -1;

    int ww = 0;
    int wh = 0;
    if (window_) SDL_GetWindowSize(window_, &ww, &wh);
    if (ww <= 0) ww = static_cast<int>(pvz2_surface_width());
    if (wh <= 0) wh = static_cast<int>(pvz2_surface_height());

    const float speed = stick_speed_ * dt_sec;
    cursor_x_ += static_cast<int>(sx * speed * static_cast<float>(ww));
    cursor_y_ += static_cast<int>(sy * speed * static_cast<float>(wh));
    warp_cursor(cursor_x_, cursor_y_);
}

void GamepadModule::apply_border_pad(int touch_w, int touch_h, int *x, int *y) const {
    int min_x = kDragBorderPad;
    int max_x = touch_w - 1 - kDragBorderPad;
    int min_y = kDragBorderPad;
    int max_y = touch_h - 1 - kDragBorderPad;

    if (min_x > max_x) {
        min_x = 0;
        max_x = touch_w - 1;
    }
    if (min_y > max_y) {
        min_y = 0;
        max_y = touch_h - 1;
    }

    if (*x < min_x) *x = min_x;
    if (*x > max_x) *x = max_x;
    if (*y < min_y) *y = min_y;
    if (*y > max_y) *y = max_y;
}

void GamepadModule::start_drag_gesture(int dir_x, int dir_y) {
    if (drag_.active) return;

    const int touch_w = static_cast<int>(pvz2_touch_space_width());
    const int touch_h = static_cast<int>(pvz2_touch_space_height());
    if (touch_w <= 1 || touch_h <= 1) return;

    const int center_x = (touch_w * 50) / 100;
    const int center_y = (touch_h * 50) / 100;
    const int top_y = (touch_h * 20) / 100;
    const int bottom_y = (touch_h * 80) / 100;
    const int left_x = (touch_w * 20) / 100;
    const int right_x = (touch_w * 80) / 100;

    drag_.horizontal = std::abs(dir_x) > std::abs(dir_y);
    if (drag_.horizontal) {
        drag_.start_y = center_y;
        drag_.end_y = center_y;
        drag_.start_x = center_x;
        drag_.end_x = (dir_x > 0) ? left_x : right_x;
        apply_border_pad(touch_w, touch_h, &drag_.start_x, &drag_.start_y);
        apply_border_pad(touch_w, touch_h, &drag_.end_x, &drag_.end_y);
    } else {
        drag_.start_x = center_x;
        drag_.end_x = center_x;
        drag_.start_y = center_y;
        drag_.end_y = (dir_y < 0) ? bottom_y : top_y;
        apply_border_pad(touch_w, touch_h, &drag_.start_x, &drag_.start_y);
        apply_border_pad(touch_w, touch_h, &drag_.end_x, &drag_.end_y);
    }

    if (drag_.start_x == drag_.end_x && drag_.start_y == drag_.end_y) return;

    drag_.active = true;
    drag_.step = 0;
}

void GamepadModule::process_drag_gesture() {
    if (!drag_.active) return;

    if (drag_.step == 0) {
        pvz2_input_push_touch(PVZ2_TOUCH_DOWN, drag_.start_x, drag_.start_y);
    } else if (drag_.step <= kDragRepeats) {
        int cx = drag_.start_x;
        int cy = drag_.start_y;
        if (drag_.horizontal) {
            cx = drag_.start_x +
                 ((drag_.end_x - drag_.start_x) * drag_.step) / kDragRepeats;
        } else {
            cy = drag_.start_y +
                 ((drag_.end_y - drag_.start_y) * drag_.step) / kDragRepeats;
        }
        pvz2_input_push_touch(PVZ2_TOUCH_MOVE, cx, cy);
    } else {
        pvz2_input_push_touch(PVZ2_TOUCH_UP, drag_.end_x, drag_.end_y);
        drag_.active = false;
        drag_.step = 0;
        drag_cooldown_ = kDragCooldownSec;
        return;
    }

    drag_.step++;
}

void GamepadModule::tick_right_stick(float dt_sec) {
    if (drag_cooldown_ > 0.0f) drag_cooldown_ = std::max(0.0f, drag_cooldown_ - dt_sec);

    process_drag_gesture();
    if (drag_.active) return;

    Sint16 ax = SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_RIGHTX);
    Sint16 ay = SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_RIGHTY);
    float sx = axis_to_float(ax);
    float sy = axis_to_float(ay);

    if (std::fabs(sx) < stick_deadzone_ && std::fabs(sy) < stick_deadzone_) return;
    if (drag_cooldown_ > 0.0f) return;

    /* Hold to repeat: each finished gesture + cooldown starts another while
     * the stick stays deflected (no need to recenter). */
    int dir_x = 0;
    int dir_y = 0;
    if (std::fabs(sy) >= std::fabs(sx)) {
        dir_y = (sy < 0.0f) ? -1 : 1;
    } else {
        dir_x = (sx < 0.0f) ? -1 : 1;
    }

    start_drag_gesture(dir_x, dir_y);
}

void GamepadModule::tick_right_stick_ui(float dt_sec) {
    /* Config list scroll: same effect as mouse wheel (one item per pulse). */
    if (drag_cooldown_ > 0.0f) drag_cooldown_ = std::max(0.0f, drag_cooldown_ - dt_sec);

    Sint16 ay = SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_RIGHTY);
    float sy = axis_to_float(ay);
    if (std::fabs(sy) < stick_deadzone_) return;
    if (drag_cooldown_ > 0.0f) return;

    SDL_Event wheel;
    std::memset(&wheel, 0, sizeof(wheel));
    wheel.type = SDL_MOUSEWHEEL;
    wheel.wheel.timestamp = SDL_GetTicks();
    wheel.wheel.windowID = window_ ? SDL_GetWindowID(window_) : 0;
    wheel.wheel.which = 0;
    /* Stick up (negative Y) → wheel +1 → list scrolls toward top (same as mouse). */
    wheel.wheel.y = (sy < 0.0f) ? 1 : -1;
    wheel.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    if (achievements_screen_is_open())
        achievements_screen_handle_event(&wheel);
    else
        config_screen_handle_event(&wheel);

    drag_cooldown_ = kDragCooldownSec;
}

void GamepadModule::tick_triggers() {
    const Sint16 l2 = SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    const Sint16 r2 = SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    const bool l2_down = trigger_to_float(l2) >= trigger_threshold_;
    const bool r2_down = trigger_to_float(r2) >= trigger_threshold_;

    if (l2_down && !l2_prev_) goto_menu(MenuId::Left);
    if (r2_down && !r2_prev_) goto_menu(MenuId::Board);

    l2_prev_ = l2_down;
    r2_prev_ = r2_down;
}

void GamepadModule::tick_dpad_repeat(float dt_sec) {
    if (dpad_held_ == DpadDir::None) return;

    dpad_repeat_timer_ += dt_sec;
    if (dpad_repeat_timer_ < dpad_repeat_interval_) return;

    dpad_repeat_timer_ -= dpad_repeat_interval_;
    dpad_jump(dpad_held_);
    dpad_repeat_interval_ = std::max(kDpadRepeatMinSec, dpad_repeat_interval_ - kDpadRepeatAccelSec);
}

void GamepadModule::tick(float dt_sec) {
    if (!controller_ready()) return;

    /* Keep logical cursor synced with the OS cursor (mouse or prior warp). */
    if (window_) SDL_GetMouseState(&cursor_x_, &cursor_y_);

    /* Overlays still need free cursor aim + right-stick list scroll. */
    if (overlay_open()) {
        tick_left_stick(dt_sec);
        tick_right_stick_ui(dt_sec);
        return;
    }

    tick_left_stick(dt_sec);
    tick_right_stick(dt_sec);
    tick_triggers();
    tick_dpad_repeat(dt_sec);
}

}  // namespace

extern "C" {

void pvz2_gamepad_init(SDL_Window *window, int use_43) { g_gamepad.init(window, use_43); }

void pvz2_gamepad_shutdown(void) { g_gamepad.shutdown(); }

void pvz2_gamepad_reload(void) { g_gamepad.reload(); }

int pvz2_gamepad_handle_event(const SDL_Event *e) { return g_gamepad.handle_event(e); }

void pvz2_gamepad_tick(float dt_sec) { g_gamepad.tick(dt_sec); }

}  // extern "C"
