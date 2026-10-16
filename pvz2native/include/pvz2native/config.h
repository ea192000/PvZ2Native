#ifndef PVZ2NATIVE_CONFIG_H
#define PVZ2NATIVE_CONFIG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PVZ2_MAX_SHORTCUTS 64
#define PVZ2_MAX_BOARD_CELLS 64
#define PVZ2_MAX_GAMEPAD_BINDS 32

typedef struct pvz2_shortcut_binding {
    char name[64];
    char key[32];
    /* Canonical 16:9 tap position (also used by the settings bind preview). */
    float x;
    float y;
    /* Optional 4:3 tap position from the INI. Unused when has_43 == 0. */
    float x43;
    float y43;
    int has_43; /* 1 if a 4:3 pair was present in config.ini */
} pvz2_shortcut_binding_t;

/* Lawn cell center for gamepad focus ([board] position_ROW_COL). */
typedef struct pvz2_board_cell {
    int row; /* 1-based */
    int col; /* 1-based */
    float x;
    float y;
    float x43;
    float y43;
    int has_43;
} pvz2_board_cell_t;

/* Maps a gamepad button name ("start","b",...) to a shortcut id ("bind_pause").
 * bind_name empty means NULL / unmapped. */
typedef struct pvz2_gamepad_bind {
    char button[16];
    char bind_name[64];
} pvz2_gamepad_bind_t;

/* Runtime configuration, read once from config.ini in the portable data folder
 * (next to the .exe, or next to the .AppImage — see pvz2_host_data_path()).
 *
 * Every debug switch defaults OFF, so a plain build (no config.ini at all) is
 * quiet and fast. The .ini is the ONLY source of truth -- the old PVZ2_*
 * environment variables are no longer consulted. See config.cpp for the parser
 * and pvz2_config_load() for how defaults and path resolution are filled in.
 *
 * C-callable on purpose: main.c (the SDL host loop) is C, and the dependency /
 * runtime shims that read these flags are C++. Both include this one header. */
typedef struct pvz2_config {
    /* [log] -- what the harness prints. */
    int verbose;        /* blow-by-blow boot log: per .init_array entry, per lifecycle call */
    int trace;          /* per-import-call trace (~57MB/run, very loud) */
    int pc_sample;      /* [pc-sample]/[HOT] instruction sampling + hot-frame slicing */
    int input;          /* per-event touch/key logging + a frame heartbeat */
    int console;        /* show console/log output (1) or stay quiet (0); on Windows also shows/hides the console window */
    /* Empty unless [log] logfile is uncommented. Relative names resolve against
     * the executable folder, same as [paths] so/obb. */
    char log_file[512];

    /* [runtime] -- how the emulator behaves. */
    int no_page_table;        /* route guest memory through the slow callback path */
    unsigned heap_quarantine; /* hold the N most-recently-freed heap blocks out of reuse */
    /* 1 = re-copy OBB Packages into userdata/CDN.<tag> every launch (overwrite).
     * 0 = seed only when the CDN folder is missing / not yet .seeded. */
    int rewrite_cdn;
    char force_date[32];    /* [runtime] force_date: "YYYY-MM-DD [HH:MM[:SS]]" local time the game sees; empty = real clock */
    int lod_legacy_hacks;   /* 1 = old daily-pinata workarounds (progress/uid-order patches + completed-field reset) */

    /* [gl] -- rendering diagnostics. */
    int gl_debug_clear;     /* force a loud clear colour instead of the engine's black */
    int gl_no_viewport_fix; /* disable the zero-size viewport substitution */
    int gl_flat_fragment;   /* replace every fragment shader body with a constant colour */
    int gl_strict;          /* query GL state on every suspicious call */
    int gl_no_etc2;         /* always decode ETC1 on the CPU, never hand it to the GPU as ETC2 */

    /* [game] -- what the engine is told about the "device".
     *
     * The Java locale, e.g. "en_US" or "es_AR". Info_SysGetUserLocale returns it
     * verbatim, and Info_SysGetCountryCodeString derives the country from the
     * part after '_'. It steers currency, store region and localized text; a
     * value the .obb has no translation for just falls back to English, so any
     * <lang>_<REGION> string is safe. Empty -> "en_US". */
    char user_locale[32];

    /* Emulated in-app purchases. When on, the store reports itself available and
     * every purchase the engine requests completes immediately (the item is
     * granted). When off, the store reports "not supported" and nothing is
     * purchasable. Defaults ON -- see pvz2_config_load. */
    int emulate_iap;

    /* Host-side achievement unlock toast + sound. Achievements are still
     * recorded when off; only the popup/SFX are suppressed. Default on. */
    int achievement_notifications;

    /* Identity strings the engine asks Java for -- end up in save files, the
     * fabricated purchase receipt and analytics payloads. package_name is the
     * one with a correctness requirement: ResourceManager builds the RSB path
     * the engine looks for from packageName + versionCode, so it must name
     * whatever .obb [paths] obb actually points at, or boot fails with "RSB
     * Initialization failed" (see the .obb-mismatch note in config.cpp).
     * dex/hooks/purchase_driver.cpp reads the same field for the receipt JSON's
     * packageName, so the two can never disagree again. */
    char package_name[64];
    char activity_name[96];
    char device_name[64];
    char hardware_model[64];
    char os_version[16];
    char currency_symbol[8];
    char currency_code[8];
    char product_version_string[32];
    char player_uuid[64];

    /* [video] -- how the window and the guest surface are sized.
     *
     * mode picks the launch resolution when width/height are left at 0:
     *   "auto"   -- match the primary display's ASPECT at a moderate base height
     *               (kAutoBaseHeight), so proportions are right without the GPU
     *               cost of a full-native surface. The default.
     *   "native" -- match the display's full resolution.
     * An explicit width AND height (both > 0) override mode and are used verbatim.
     * The window is always resizable and the engine re-fits on resize, so this is
     * only the STARTING size. fullscreen starts the window borderless-fullscreen
     * (F11 toggles it at runtime either way). */
    char video_mode[16];
    int video_width;
    int video_height;
    int video_fullscreen;

    /* SDL display index the window opens on / the "auto"/"native" modes measure
     * against (0 = primary, matching SDL's own numbering). Always re-checked
     * against SDL_GetNumVideoDisplays() before use -- an index that no longer
     * exists (a monitor unplugged since it was chosen) silently falls back to 0
     * rather than failing to create a window. See pvz2_effective_display(). */
    int video_display;

    /* Frame-rate cap, in presented frames per second.
     *
     * The engine advances its simulation from the wall clock (gettimeofday), not
     * from a frame count, so presenting fewer frames does NOT slow the game
     * down -- it only stops the loop from re-running onDrawFrame as fast as the
     * CPU allows and pinning a core at 100%.
     *
     *   > 0 -- pace the loop to at most this many frames per second. Vsync stays
     *          on, so the rate that actually reaches the screen is the LOWER of
     *          this and the display's refresh rate.
     *   0   -- no cap at all: vsync is switched off too and frames are presented
     *          as fast as the machine manages (tearing, one core saturated).
     * Anything negative reads as 0. Default 60 -- stated once, in kSettings in
     * config.cpp, which is also what the generated config.ini is written from. */
    int fps_limit;

    /* What AndroidHttpProxy.GetNetworkStatus() reports to the engine:
     *   0 -- no network        1 -- mobile        2 -- Wi-Fi
     *
     * Default 0 (none): boot must stay offline or the loading screen waits on
     * real-network downloads this port cannot fetch. After DidBecomeActive the
     * hook promotes none -> wifi after DidBecomeActive so LoD/map online gates
     * open against OBB Packages. Stay offline through surface or loading hangs.
     * Set mobile/wifi explicitly to force that
     * answer even during boot (can hang if CDN isn't fully seeded). Purchases
     * do not need this -- see game/patches.h. */
    int network_status;

    /* Persist the engine's key/value store (its SharedPreferences equivalent) to
     * save_dir, so settings that must survive a restart -- the age gate above
     * all -- actually do. Off makes every launch a clean first run. Default ON. */
    int persist_saves;

    /* [paths] -- keys `so`, `obb` and `save`. Always concrete after load: a value
     * left unset in the .ini is filled with <exe_dir>/<default filename>
     * (so/obb) or the executable's own folder (save_dir), and a relative one is
     * resolved against the executable's folder. */
    char so_path[512];
    char obb_path[512];
    char save_dir[512];

    /* [shortcuts] -- dynamic binds parsed from keys `bind_*` with format
     * `KEY / x16,y16 [/ x43,y43]`, where x/y are normalized (0..1). */
    unsigned shortcut_count;
    pvz2_shortcut_binding_t shortcuts[PVZ2_MAX_SHORTCUTS];

    /* [board] -- lawn cell centers for gamepad D-pad / R1 (no keys). */
    unsigned board_count;
    pvz2_board_cell_t board[PVZ2_MAX_BOARD_CELLS];

    /* [gamepad] -- host gamepad options. */
    int gamepad_enabled;
    int gamepad_port;
    float gamepad_stick_speed;
    float gamepad_stick_deadzone;
    float gamepad_trigger_threshold;

    /* [gamepad_binds] -- button -> bind_* name (empty bind_name = NULL). */
    unsigned gamepad_bind_count;
    pvz2_gamepad_bind_t gamepad_binds[PVZ2_MAX_GAMEPAD_BINDS];
} pvz2_config_t;

/* Base height the "auto" video mode scales the display aspect to. 540 keeps the
 * fragment load close to the original fixed 960x540 while giving 4:3/16:10/16:9
 * displays each their correct proportions. */
#define PVZ2_AUTO_BASE_HEIGHT 540

/* Loads config.ini from `ini_path` (may be NULL or missing -> all defaults).
 * Any path left unset in the file is resolved against `base_dir` (the
 * executable's directory, normally with a trailing separator) as
 * base_dir + default name. Older/incomplete files are migrated in place:
 * missing required sections/keys are added with defaults, and values that
 * fail the expected type/format are rewritten to the default. Call once,
 * early, before pvz2_session_start. Safe to call again to reload. */
void pvz2_config_load(const char *ini_path, const char *base_dir);

/* Applies a single runtime override (used for command-line options).
 *
 * Accepted names:
 *   - key only:       "fullscreen", "fps_limit", "so", ...
 *   - section + key:  "video.fullscreen", "paths.so", ...
 *
 * Returns 1 if the key exists and was applied, 0 otherwise. `bind_*` and the
 * [shortcuts] section are intentionally not supported here. */
int pvz2_config_override(const char *name, const char *value);

/* Seconds to ADD to the host clock to get the time the guest sees: 0 unless [runtime] force_date
 * is set. The forced date is where the clock starts; it then runs normally. Computed once, on the
 * first call. */
long long pvz2_time_offset_sec(void);

/* The loaded config. Never NULL: before pvz2_config_load runs it returns an
 * all-defaults (all-off, empty paths) instance, so shim code can read it
 * unconditionally without an init check. */
const pvz2_config_t *pvz2_config(void);

/* Updates the key string of shortcut entry at position `idx` (0-based) both
 * in the in-memory config and in config.ini on disk.  `idx` must be less than
 * pvz2_config()->shortcut_count.  Returns 1 on success, 0 on failure. */
int pvz2_config_update_bind_key(unsigned idx, const char *new_key);

/* Sets [gamepad_binds] mapping for `button` (e.g. "x","b","l3") to `bind_name`
 * ("bind_pause", "NULL"/empty to clear). Persists to config.ini. Clears the
 * same bind_name from other remappable buttons (b/x/y/l3/r3) for UI uniqueness.
 * Returns 1 on success. */
int pvz2_config_update_gamepad_bind(const char *button, const char *bind_name);

/* Sets [gamepad] enabled + port and persists to config.ini. Returns 1 on success. */
int pvz2_config_update_gamepad_device(int enabled, int port);

/* Sets [video] display and persists to config.ini. `index` is clamped to
 * [0, SDL_GetNumVideoDisplays()) by the caller's own check (see
 * pvz2_effective_display); an out-of-range value here is still stored as-is so
 * a monitor unplugged and reconnected later is remembered. Returns 1 on
 * success. */
int pvz2_config_update_display(int index);

/* The configured [video] display, re-validated against how many displays SDL
 * currently reports -- 0 if the stored index no longer exists. Every call site
 * that indexes SDL_GetCurrentDisplayMode/SDL_CreateWindow's display argument
 * must go through this rather than reading video_display directly. */
int pvz2_effective_display(void);

/* which: 0 = stick_speed, 1 = stick_deadzone.
 * persist!=0 writes [gamepad] to config.ini; 0 only updates live memory. */
int pvz2_config_update_gamepad_stick_param(int which, float value, int persist);

/* Reloads the runtime shortcut table from the current live config.
 * Must be called after pvz2_config_update_bind_key for changes to take effect
 * immediately without restarting. */
void pvz2_reload_shortcuts(void);

/* Writes the settings-overlay fields into the in-memory config. Does not touch
 * disk. Returns 1 if a restart is required (locale or emulate_iap changed),
 * 0 if everything else can be applied live. achievement_notifications is always
 * applied live (no relaunch). */
int pvz2_config_apply_overlay_settings(int fullscreen, int width, int height,
                                       int fps_limit, const char *locale,
                                       const char *currency_symbol,
                                       const char *currency_code,
                                       int emulate_iap,
                                       int achievement_notifications);

/* Applies fullscreen / window size on the live SDL window and notifies the
 * guest of the new drawable size. Defined in main.c. */
void pvz2_host_apply_video_settings(int fullscreen, int width, int height);

/* Moves the live window onto `display_index` (already validated by the caller,
 * normally via pvz2_effective_display()) without requiring a restart. Defined
 * in main.c. */
void pvz2_host_apply_display_settings(int display_index);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif
