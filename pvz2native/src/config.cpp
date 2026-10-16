/* config.ini reader and self-seeder -- see include/pvz2native/config.h.
 *
 * A tiny, dependency-free INI parser: `[section]` headers, `key = value` lines,
 * `;`/`#` comments. Unknown sections and keys are ignored on purpose so a config
 * written for a newer build does not break an older one. Booleans accept 1/0,
 * true/false, yes/no, on/off. The whole file is optional; with no file every
 * switch takes the default recorded in kSettings below.
 *
 * After load, heal_config_ini() migrates an older/incomplete file in place:
 * missing required sections/keys are appended with defaults, and values that
 * fail the type/format check are rewritten to the live default. Custom extra
 * keys (unknown bind_*, extra board cells, etc.) are left alone.
 *
 * kSettings is the single source of truth. The parser, the built-in defaults and
 * the config.ini written on first run are all generated from that one table,
 * which is the point: they used to be three hand-maintained lists and all three
 * had drifted apart --
 *
 *   - `save_dir` was documented in config.h as a [paths] key and had a
 *     resolve-relative branch in load(), but no parser entry at all, so it was
 *     unsettable and that branch was dead code;
 *   - the seeded file advertised `main.7.com.ea.game.pvz2_na.obb` while the code
 *     defaulted to `main.147.com.ea.game.pvz2_row.obb`, so a player who
 *     uncommented the documented line broke their own boot;
 *   - config.h promised fps_limit defaulted to 120 while it was actually 60;
 *   - `[log] input` was parsed but appeared in no generated file, so the switch
 *     existed and nobody could find it.
 *
 * Adding a switch is now one row.
 */

#include <pvz2native/config.h>
#include <pvz2native/host_paths.h>
#include <pvz2native/log.h>

#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <random>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/utsname.h>
#endif

#include <SDL.h>

namespace {

/* The single instance. Zero-initialised => every flag off and both paths empty,
 * which is the correct "no config loaded yet" state for anything that reads
 * pvz2_config() before pvz2_config_load() runs. */
pvz2_config_t g_config{};

/* Default resource file names, resolved under <exe_dir>. kSettings quotes
 * these in the seeded file's documentation, so the two cannot disagree. */
constexpr char kDefaultSoName[] = "libPVZ2.so";
constexpr char kDefaultObbName[] = "data.obb";
constexpr char kDefaultLogFileName[] = "pvz2.log";

enum class Kind {
    kBool,    /* 1/0, true/false, yes/no, on/off; anything else reads as off */
    kUInt,    /* strtoul, base 0 */
    kInt,     /* strtol, base 0 */
    kFloat,   /* finite float */
    kString,  /* copied verbatim, trimmed */
    kLower,   /* copied lowercased, trimmed */
    kPath,    /* copied verbatim; resolved against the exe folder after parsing */
    kNetwork, /* none/mobile/wifi -> 0/1/2 */
    kFps,     /* like kInt, but a value with no digits keeps the default */
};

struct Setting {
    const char *section;
    const char *key;
    const char *alias; /* accepted spelling that is not advertised, or nullptr */
    Kind kind;
    std::size_t offset;   /* into pvz2_config_t */
    std::size_t capacity; /* char-array fields only */

    /* Comment block written above this key in a seeded config.ini, one line per
     * '\n'. Written with a leading "; ". */
    const char *comment;

    /* The value written into a seeded config.ini AND applied as the built-in
     * default before the file is parsed -- so those two can never disagree.
     *
     * nullptr means the key is written commented out and is NOT given a default
     * here: either the zero-initialised value is already correct (video width and
     * height, where 0 means "derive from mode"), or the default depends on the
     * executable's folder and finish_defaults() computes it (the three paths, and
     * user_locale). */
    const char *seed;

    /* Shown after "; <key> = " when seed is nullptr: an example, not a default. */
    const char *example;
};

/* Sections are emitted into a seeded file in the order they first appear here. */
const Setting kSettings[] = {
    /* The `so` and `obb` examples are generated from kDefaultSoName/kDefaultObbName
     * -- see example_for() -- so the documented name is always the real one. */
    {"paths", "so", nullptr, Kind::kPath, offsetof(pvz2_config_t, so_path), sizeof(g_config.so_path),
     "Where to load the game data from. A relative path is resolved against the\n"
     "folder holding the .exe.",
     nullptr, nullptr},
    {"paths", "obb", nullptr, Kind::kPath, offsetof(pvz2_config_t, obb_path),
     sizeof(g_config.obb_path), nullptr, nullptr, nullptr},
    {"paths", "save", nullptr, Kind::kPath, offsetof(pvz2_config_t, save_dir),
     sizeof(g_config.save_dir),
     "Where persisted settings go (see persist_saves). Leave unset to store\n"
     "prefs.dat directly next to the .exe; set a relative path (for example\n"
     "save) to use a subfolder instead.", nullptr,
     "save"},

    {"log", "verbose", nullptr, Kind::kBool, offsetof(pvz2_config_t, verbose), 0,
     "Blow-by-blow boot log: one banner per .init_array entry and lifecycle call.", nullptr, "0"},
    {"log", "trace", nullptr, Kind::kBool, offsetof(pvz2_config_t, trace), 0,
     "Per-import-call trace. Extremely loud (~57 MB of stdout per run).", nullptr, "0"},
    {"log", "pc_sample", nullptr, Kind::kBool, offsetof(pvz2_config_t, pc_sample), 0,
     "[pc-sample]/[HOT] instruction sampling and hot-frame slicing. This is the\n"
     "source of the \"pc=0x.... lr=0x....\" spam.",
     nullptr, "0"},
    {"log", "input", nullptr, Kind::kBool, offsetof(pvz2_config_t, input), 0,
     "Per-event touch/key logging, plus a frame heartbeat with heap and audio\n"
     "health. This is the switch to turn on for a freeze or a dead click: if the\n"
     "heartbeat stops, the engine hung inside onDrawFrame; if it keeps ticking,\n"
     "the event arrived and the engine chose to ignore it.",
     nullptr, "0"},
    {"log", "console", nullptr, Kind::kBool, offsetof(pvz2_config_t, console), 0,
     "Show console/log output (1) or keep it quiet (0). On Windows this also\n"
     "shows or hides the console window.",
     nullptr, "0"},
    {"log", "logfile", nullptr, Kind::kPath, offsetof(pvz2_config_t, log_file),
     sizeof(g_config.log_file),
     "Write the same text the console would print into this file. A relative\n"
     "path is resolved against the folder holding the .exe. Leave commented to\n"
     "skip the file entirely; uncomment to create/overwrite it on each launch.",
     nullptr, nullptr},

    {"runtime", "no_page_table", nullptr, Kind::kBool, offsetof(pvz2_config_t, no_page_table), 0,
     "Route guest memory through the slow callback path (needed by watchpoints).", nullptr, "0"},
    {"runtime", "heap_quarantine", nullptr, Kind::kUInt, offsetof(pvz2_config_t, heap_quarantine), 0,
     "Hold the N most-recently-freed heap blocks out of the reuse pool (0 = off).", nullptr, "0"},
    {"runtime", "force_date", nullptr, Kind::kString, offsetof(pvz2_config_t, force_date),
     sizeof(g_config.force_date),
     "Date and time the game believes it is, as \"YYYY-MM-DD HH:MM:SS\" (local time; time and\n"
     "seconds optional). The clock starts there and then runs normally. Does not touch the system\n"
     "clock. Leave unset for the real clock. Back up userdata/ first: dates are saved into the\n"
     "profile.",
     nullptr, "2026-10-17 10:00:00"},
    {"runtime", "lod_legacy_hacks", nullptr, Kind::kBool, offsetof(pvz2_config_t, lod_legacy_hacks), 0,
     "Daily pinata party. 0 (default) keeps the engine's own rules: once you win, the event leaves\n"
     "the map until the next one starts. 1 re-enables older workarounds that keep a REPLAY banner\n"
     "on the map after winning (they reset the \"completed level\" field and skip two engine\n"
     "checks). Takes effect at the next launch.", nullptr, "0"},
    {"runtime", "rewrite_cdn", nullptr, Kind::kBool, offsetof(pvz2_config_t, rewrite_cdn), 0,
     "Re-copy OBB Packages into userdata/CDN.<tag> on every launch (overwrite).\n"
     "0 = seed only when CDN.<tag> is missing / not yet .seeded (default).\n"
     "1 = always refresh CDN files from the current OBB once per launch.",
     nullptr, "0"},

    {"gl", "debug_clear", nullptr, Kind::kBool, offsetof(pvz2_config_t, gl_debug_clear), 0,
     "Force a loud clear colour instead of the engine's opaque black.", nullptr, "0"},
    {"gl", "no_viewport_fix", nullptr, Kind::kBool, offsetof(pvz2_config_t, gl_no_viewport_fix), 0,
     "Disable the zero-size viewport substitution.", nullptr, "0"},
    {"gl", "flat_fragment", nullptr, Kind::kBool, offsetof(pvz2_config_t, gl_flat_fragment), 0,
     "Replace every fragment shader body with a constant colour (bisection tool).", nullptr, "0"},
    {"gl", "strict", nullptr, Kind::kBool, offsetof(pvz2_config_t, gl_strict), 0,
     "Query GL state on every suspicious call and report mismatches.", nullptr, "0"},
    {"gl", "no_etc2", nullptr, Kind::kBool, offsetof(pvz2_config_t, gl_no_etc2), 0,
     "Always decode ETC1 textures on the CPU. By default they go to the GPU as ETC2 (a\n"
     "superset) when the driver supports it, which is faster to load and uses less memory.",
     nullptr, "0"},

    {"video", "mode", nullptr, Kind::kLower, offsetof(pvz2_config_t, video_mode),
     sizeof(g_config.video_mode),
     "Launch resolution. The window is always resizable and the engine re-fits\n"
     "on resize, so this is only the STARTING size.\n"
     "  mode = auto   -> match the display's aspect at a moderate base height\n"
     "                   (light on the GPU; 4:3/16:10/16:9 each get right shape)\n"
     "  mode = native -> match the display's full resolution (sharpest, heaviest)\n"
     "An explicit width AND height (both > 0) override mode and are used as-is.",
     nullptr, "auto"},
    {"video", "width", nullptr, Kind::kInt, offsetof(pvz2_config_t, video_width), 0, nullptr,
     nullptr, "0"},
    {"video", "height", nullptr, Kind::kInt, offsetof(pvz2_config_t, video_height), 0, nullptr,
     nullptr, "0"},
    {"video", "fullscreen", nullptr, Kind::kBool, offsetof(pvz2_config_t, video_fullscreen), 0,
     "Start borderless-fullscreen (F11 toggles it at runtime either way).", "1", nullptr},
    {"video", "display", nullptr, Kind::kInt, offsetof(pvz2_config_t, video_display), 0,
     "SDL display index to open the window on and to measure [video] mode\n"
     "auto/native against (0 = primary). Optional: an index that does not exist\n"
     "at launch (monitor unplugged, etc.) falls back to 0 automatically.",
     nullptr, "0"},
    {"video", "fps_limit", "fpslimit", Kind::kFps, offsetof(pvz2_config_t, fps_limit), 0,
     "Frame-rate cap, in FPS. The simulation runs off the wall clock, so a lower\n"
     "cap does not slow the game down -- it only stops the loop from redrawing as\n"
     "fast as the CPU allows and pinning a core. Vsync stays on, so what reaches\n"
     "the screen is whichever is lower, this or the monitor's refresh rate.\n"
     "  0 = no limit at all (vsync off too: tearing, one core saturated).",
     "60", nullptr},

    {"game", "user_locale", nullptr, Kind::kString, offsetof(pvz2_config_t, user_locale),
     sizeof(g_config.user_locale),
     "Locale the engine is told the device uses, as <lang>_<REGION>. Steers\n"
     "currency, store region and which localized text loads; an unavailable\n"
     "language falls back to English. The country code is taken from the part\n"
     "after '_'.",
     "en_US", nullptr},
    {"game", "emulate_iap", nullptr, Kind::kBool, offsetof(pvz2_config_t, emulate_iap), 0,
     "Emulated in-app purchases: on = the store is available and every purchase\n"
     "the game requests completes for free; off = store reports not supported.\n"
     "Default off: gem-priced store items do not need Play Billing.",
     nullptr, "0"},
    {"game", "achievement_notifications", nullptr, Kind::kBool,
     offsetof(pvz2_config_t, achievement_notifications), 0,
     "Show the achievement unlock popup and sound. Achievements are still saved\n"
     "when this is off; only the toast is suppressed. Default on.",
     nullptr, "1"},

    {"game", "package_name", nullptr, Kind::kString, offsetof(pvz2_config_t, package_name),
     sizeof(g_config.package_name),
     "The Android package name the engine is told it runs as. Must match the\n"
     "package baked into [paths] obb's filename (main.<versionCode>.<package>.obb)\n"
     "or the RSB load fails and boot never leaves the loading screen.",
     nullptr, "com.ea.game.pvz2_na"},
    {"game", "activity_name", nullptr, Kind::kString, offsetof(pvz2_config_t, activity_name),
     sizeof(g_config.activity_name), "The Activity class name reported back to the engine.",
     nullptr, "com.popcap.PvZ2.PvZ2GameActivity"},
    {"game", "device_name", nullptr, Kind::kString, offsetof(pvz2_config_t, device_name),
     sizeof(g_config.device_name), "Device_GetDeviceName's answer.", "", nullptr},
    {"game", "hardware_model", nullptr, Kind::kString, offsetof(pvz2_config_t, hardware_model),
     sizeof(g_config.hardware_model), "Diag_GetHardwareModel's answer.", "", nullptr},
    {"game", "os_version", nullptr, Kind::kString, offsetof(pvz2_config_t, os_version),
     sizeof(g_config.os_version), "Diag_GetOSVersion's answer (an Android version string).", nullptr,
     "14"},
    {"game", "currency_symbol", nullptr, Kind::kString, offsetof(pvz2_config_t, currency_symbol),
     sizeof(g_config.currency_symbol), "Info_SysGetUserCurrencySymbol's answer.", "$", nullptr},
    {"game", "currency_code", nullptr, Kind::kString, offsetof(pvz2_config_t, currency_code),
     sizeof(g_config.currency_code),
     "Info_SysGetUserCurrencyCode's answer, an ISO 4217 code. A null currency\n"
     "reads to the in-app store as \"not configured\" (Service Unavailable), so\n"
     "this must stay non-empty even if you change it.",
     "USD", nullptr},
    {"game", "product_version_string", nullptr, Kind::kString,
     offsetof(pvz2_config_t, product_version_string), sizeof(g_config.product_version_string),
     "Info_SysGetProductVersionString's answer.", "4.5.2", nullptr},
    {"game", "player_uuid", nullptr, Kind::kString, offsetof(pvz2_config_t, player_uuid),
     sizeof(g_config.player_uuid),
     "Util_GetUUIDString's answer. Generated once when config.ini is first created.", "", nullptr},
    {"game", "persist_saves", nullptr, Kind::kBool, offsetof(pvz2_config_t, persist_saves), 0,
     "Persist the game's saved settings (age gate, options) to the save folder\n"
     "so they survive a restart. Off makes every launch a clean first run.",
     nullptr, "1"},
    {"game", "network", nullptr, Kind::kNetwork, offsetof(pvz2_config_t, network_status), 0,
     "Connectivity reported to the engine: none, mobile or wifi.\n"
     "Leave at none: boot stays offline (avoids loading-screen waits), then the\n"
     "hook promotes to wifi after DidBecomeActive so LoD/map online gates open\n"
     "against Packages served from the OBB. Explicit mobile/wifi forces that\n"
     "answer for the whole session. The store does not need this.",
     nullptr, "none"},

    {"gamepad", "enabled", nullptr, Kind::kBool, offsetof(pvz2_config_t, gamepad_enabled), 0,
     "Enable SDL gamepad support (Xbox 360 and other mapped controllers).", "1", nullptr},
    {"gamepad", "port", nullptr, Kind::kInt, offsetof(pvz2_config_t, gamepad_port), 0,
     "SDL joystick/controller index to open (0 = first / \"port 0\").", "0", nullptr},
    {"gamepad", "stick_speed", nullptr, Kind::kFloat, offsetof(pvz2_config_t, gamepad_stick_speed), 0,
     "Left-stick cursor speed in screen-fractions per second (tune in playtest).", "1.6", nullptr},
    {"gamepad", "stick_deadzone", nullptr, Kind::kFloat,
     offsetof(pvz2_config_t, gamepad_stick_deadzone), 0,
     "Analog stick deadzone (0..1). Default ~25%.", "0.25", nullptr},
    {"gamepad", "trigger_threshold", nullptr, Kind::kFloat,
     offsetof(pvz2_config_t, gamepad_trigger_threshold), 0,
     "L2/R2 press threshold when the pad reports triggers as axes (0..1).\n"
     "Optional: triggers are treated as digital buttons internally; leave this\n"
     "commented unless you need to tune the axis-to-digital cutover.",
     nullptr, "0.50"},
};

/* --- small helpers --------------------------------------------------------- */

std::string trim(const std::string &s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (char &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

/* Strict bool: only recognised tokens are accepted. Garbage keeps the default. */
bool parse_bool_token(const std::string &v, int *out) {
    const std::string t = lower(trim(v));
    if (t == "1" || t == "true" || t == "yes" || t == "on") {
        *out = 1;
        return true;
    }
    if (t == "0" || t == "false" || t == "no" || t == "off") {
        *out = 0;
        return true;
    }
    return false;
}

/* Strict integer: must start with a digit (or sign/0x) and consume the whole
 * trimmed string. "abc", "", "12x" are rejected so the pre-applied default stays. */
bool parse_int_token(const std::string &raw, long *out, int base = 0) {
    const std::string t = trim(raw);
    if (t.empty()) return false;
    char *end = nullptr;
    const long v = std::strtol(t.c_str(), &end, base);
    if (end == t.c_str()) return false;
    while (*end != '\0') {
        if (!std::isspace((unsigned char)*end)) return false;
        ++end;
    }
    *out = v;
    return true;
}

bool parse_uint_token(const std::string &raw, unsigned long *out) {
    const std::string t = trim(raw);
    if (t.empty()) return false;
    char *end = nullptr;
    const unsigned long v = std::strtoul(t.c_str(), &end, 0);
    if (end == t.c_str()) return false;
    while (*end != '\0') {
        if (!std::isspace((unsigned char)*end)) return false;
        ++end;
    }
    *out = v;
    return true;
}

void *field(const Setting &s) { return (char *)&g_config + s.offset; }

bool parse_finite_number(const std::string &text, double *out) {
    const std::string t = trim(text);
    if (t.empty()) return false;
    char *end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    if (end == t.c_str()) return false;
    while (*end != '\0') {
        if (!std::isspace((unsigned char)*end)) return false;
        ++end;
    }
    *out = v;
    return true;
}

/* Clamp a normalized coordinate into [0,1]. */
static double clamp01d(double v) {
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

/* Parse "x,y" (spaces around the comma are fine). */
static bool parse_xy_pair(const std::string &text, double *x, double *y) {
    const std::string t = trim(text);
    const std::size_t c = t.find(',');
    if (c == std::string::npos) return false;
    if (!parse_finite_number(t.substr(0, c), x)) return false;
    if (!parse_finite_number(t.substr(c + 1), y)) return false;
    *x = clamp01d(*x);
    *y = clamp01d(*y);
    return true;
}

/* Format a bind value for config.ini.
 * Preferred:  KEY / x16,y16 / x43,y43   (spaces optional on read)
 * Without 4:3: KEY / x16,y16 */
static std::string format_bind_value(const char *key_name,
                                     float x, float y,
                                     int has_43, float x43, float y43) {
    char buf[192];
    if (has_43) {
        std::snprintf(buf, sizeof(buf), "%s / %.2f,%.2f / %.2f,%.2f",
                      key_name, (double)x, (double)y, (double)x43, (double)y43);
    } else {
        std::snprintf(buf, sizeof(buf), "%s / %.2f,%.2f",
                      key_name, (double)x, (double)y);
    }
    return std::string(buf);
}

/* Decode a shortcut bind value. Returns false if the format is unusable. */
bool decode_shortcut_value(const std::string &val, std::string *key_name, double *x, double *y,
                           double *x43, double *y43, int *has_43) {
    const std::string raw = trim(val);
    if (raw.empty() || !key_name || !x || !y || !x43 || !y43 || !has_43) return false;

    *has_43 = 0;
    *x43 = 0.0;
    *y43 = 0.0;

    /* New format: KEY / x16,y16 [/ x43,y43]  — spaces around '/' are optional. */
    if (raw.find('/') != std::string::npos) {
        std::vector<std::string> parts;
        std::size_t start = 0;
        while (start <= raw.size()) {
            std::size_t slash = raw.find('/', start);
            if (slash == std::string::npos) {
                parts.push_back(trim(raw.substr(start)));
                break;
            }
            parts.push_back(trim(raw.substr(start, slash - start)));
            start = slash + 1;
        }
        if (parts.size() < 2 || parts[0].empty()) return false;
        *key_name = parts[0];
        if (!parse_xy_pair(parts[1], x, y)) return false;
        if (parts.size() >= 3 && !parts[2].empty()) {
            if (!parse_xy_pair(parts[2], x43, y43)) return false;
            *has_43 = 1;
        }
        return true;
    }

    /* Legacy: KEY,x,y */
    const std::size_t c1 = raw.find(',');
    if (c1 == std::string::npos) return false;
    const std::size_t c2 = raw.find(',', c1 + 1);
    if (c2 == std::string::npos) return false;
    *key_name = trim(raw.substr(0, c1));
    if (key_name->empty()) return false;
    if (!parse_xy_pair(raw.substr(c1 + 1), x, y)) return false;
    return true;
}

bool parse_shortcut_bind(const std::string &key, const std::string &val) {
    if (key.rfind("bind_", 0) != 0) return false;
    if (g_config.shortcut_count >= PVZ2_MAX_SHORTCUTS) return true;

    const std::string name = trim(key.substr(5));
    if (name.empty()) return true;

    std::string key_name;
    double x = 0.0, y = 0.0, x43 = 0.0, y43 = 0.0;
    int has_43 = 0;
    if (!decode_shortcut_value(val, &key_name, &x, &y, &x43, &y43, &has_43))
        return true; /* consume the key; heal will restore a default if known */

    pvz2_shortcut_binding_t &b = g_config.shortcuts[g_config.shortcut_count++];
    std::snprintf(b.name, sizeof(b.name), "%s", name.c_str());
    std::snprintf(b.key, sizeof(b.key), "%s", key_name.c_str());
    b.x = (float)x;
    b.y = (float)y;
    b.x43 = (float)x43;
    b.y43 = (float)y43;
    b.has_43 = has_43;
    return true;
}

/* Decode a board cell value. Returns false if the format is unusable. */
bool decode_board_value(const std::string &val, double *x, double *y, double *x43, double *y43,
                        int *has_43) {
    const std::string raw = trim(val);
    if (raw.empty() || !x || !y || !x43 || !y43 || !has_43) return false;
    *has_43 = 0;
    *x43 = 0.0;
    *y43 = 0.0;

    if (raw.find('/') != std::string::npos) {
        std::vector<std::string> parts;
        std::size_t start = 0;
        while (start <= raw.size()) {
            std::size_t slash = raw.find('/', start);
            if (slash == std::string::npos) {
                parts.push_back(trim(raw.substr(start)));
                break;
            }
            parts.push_back(trim(raw.substr(start, slash - start)));
            start = slash + 1;
        }
        if (parts.empty() || !parse_xy_pair(parts[0], x, y)) return false;
        if (parts.size() >= 2 && !parts[1].empty()) {
            if (!parse_xy_pair(parts[1], x43, y43)) return false;
            *has_43 = 1;
        }
        return true;
    }
    return parse_xy_pair(raw, x, y);
}

bool parse_board_cell(const std::string &key, const std::string &val) {
    /* position_ROW_COL = x16,y16 [/ x43,y43] */
    if (key.rfind("position_", 0) != 0) return false;
    if (g_config.board_count >= PVZ2_MAX_BOARD_CELLS) return true;

    int row = 0, col = 0;
    if (std::sscanf(key.c_str(), "position_%d_%d", &row, &col) != 2) return true;
    if (row < 1 || col < 1) return true;

    double x = 0, y = 0, x43 = 0, y43 = 0;
    int has_43 = 0;
    if (!decode_board_value(val, &x, &y, &x43, &y43, &has_43))
        return true; /* consume; heal restores known defaults */

    pvz2_board_cell_t &c = g_config.board[g_config.board_count++];
    c.row = row;
    c.col = col;
    c.x = (float)x;
    c.y = (float)y;
    c.x43 = (float)x43;
    c.y43 = (float)y43;
    c.has_43 = has_43;
    return true;
}

bool parse_gamepad_bind_entry(const std::string &key, const std::string &val) {
    if (g_config.gamepad_bind_count >= PVZ2_MAX_GAMEPAD_BINDS) return true;
    const std::string btn = trim(key);
    if (btn.empty()) return true;

    std::string bind = trim(val);
    if (lower(bind) == "null" || bind.empty())
        bind.clear();

    pvz2_gamepad_bind_t &b = g_config.gamepad_binds[g_config.gamepad_bind_count++];
    std::snprintf(b.button, sizeof(b.button), "%s", btn.c_str());
    std::snprintf(b.bind_name, sizeof(b.bind_name), "%s", bind.c_str());
    return true;
}

/* Built-in defaults for [shortcuts] / [board] / [gamepad_binds]. Shared by the
 * first-run seeder, in-memory fill, and heal_config_ini(). */
struct DefaultShortcut {
    const char *name;  /* without the bind_ prefix */
    const char *value; /* KEY / x16,y16 / x43,y43 */
};

const DefaultShortcut kDefaultShortcuts[] = {
    {"plant_1", "1 / 0.05,0.16 / 0.06,0.16"},
    {"plant_2", "2 / 0.05,0.26 / 0.06,0.26"},
    {"plant_3", "3 / 0.05,0.36 / 0.06,0.36"},
    {"plant_4", "4 / 0.05,0.46 / 0.06,0.46"},
    {"plant_5", "5 / 0.05,0.56 / 0.06,0.55"},
    {"plant_6", "6 / 0.05,0.66 / 0.06,0.65"},
    {"plant_7", "7 / 0.05,0.76 / 0.06,0.75"},
    {"plant_8", "8 / 0.05,0.86 / 0.06,0.85"},
    {"replay", "R / 0.05,0.96 / 0.06,0.94"},
    {"speed", "SPACE / 0.91,0.07 / 0.88,0.06"},
    {"pause", "ENTER / 0.97,0.07 / 0.96,0.06"},
    {"plant_food", "F / 0.37,0.94 / 0.22,0.94"},
    {"power_1", "Q / 0.70,0.94 / 0.59,0.94"},
    {"power_2", "W / 0.77,0.94 / 0.69,0.94"},
    {"power_3", "E / 0.84,0.94 / 0.79,0.94"},
    {"shovel", "S / 0.95,0.94 / 0.93,0.94"},
    {"back", "BACKSPACE / 0.04,0.06 / 0.05,0.05"},
    {"almanaq", "A / 0.11,0.06 / 0.14,0.05"},
    {"connect", "C / 0.17,0.06 / 0.23,0.05"},
    {"vasebreaker", "V / 0.24,0.06 / 0.31,0.05"},
    {"zen_garden", "Z / 0.30,0.06 / 0.40,0.05"},
};

struct DefaultBoardCell {
    const char *row_col; /* "1_1" */
    const char *value;   /* x16,y16 / x43,y43 */
};

const DefaultBoardCell kDefaultBoard[] = {
    {"1_1", "0.36,0.21 / 0.21,0.25"}, {"1_2", "0.43,0.21 / 0.30,0.25"},
    {"1_3", "0.51,0.21 / 0.39,0.25"}, {"1_4", "0.58,0.21 / 0.48,0.25"},
    {"1_5", "0.65,0.21 / 0.57,0.25"}, {"1_6", "0.72,0.21 / 0.66,0.25"},
    {"1_7", "0.79,0.21 / 0.75,0.25"}, {"1_8", "0.87,0.21 / 0.84,0.25"},
    {"1_9", "0.94,0.21 / 0.92,0.25"}, {"2_1", "0.36,0.36 / 0.21,0.39"},
    {"2_2", "0.43,0.36 / 0.30,0.39"}, {"2_3", "0.51,0.36 / 0.39,0.39"},
    {"2_4", "0.58,0.36 / 0.48,0.39"}, {"2_5", "0.65,0.36 / 0.57,0.39"},
    {"2_6", "0.72,0.36 / 0.66,0.39"}, {"2_7", "0.79,0.36 / 0.75,0.39"},
    {"2_8", "0.87,0.36 / 0.84,0.39"}, {"2_9", "0.94,0.36 / 0.92,0.39"},
    {"3_1", "0.36,0.51 / 0.21,0.54"}, {"3_2", "0.43,0.51 / 0.30,0.54"},
    {"3_3", "0.51,0.51 / 0.39,0.54"}, {"3_4", "0.58,0.51 / 0.48,0.54"},
    {"3_5", "0.65,0.51 / 0.57,0.54"}, {"3_6", "0.72,0.51 / 0.66,0.54"},
    {"3_7", "0.79,0.51 / 0.75,0.54"}, {"3_8", "0.87,0.51 / 0.84,0.54"},
    {"3_9", "0.94,0.51 / 0.92,0.54"}, {"4_1", "0.36,0.66 / 0.21,0.68"},
    {"4_2", "0.43,0.66 / 0.30,0.68"}, {"4_3", "0.51,0.66 / 0.39,0.68"},
    {"4_4", "0.58,0.66 / 0.48,0.68"}, {"4_5", "0.65,0.66 / 0.57,0.68"},
    {"4_6", "0.72,0.66 / 0.66,0.68"}, {"4_7", "0.79,0.66 / 0.75,0.68"},
    {"4_8", "0.87,0.66 / 0.84,0.68"}, {"4_9", "0.94,0.66 / 0.92,0.68"},
    {"5_1", "0.36,0.81 / 0.21,0.82"}, {"5_2", "0.43,0.81 / 0.30,0.82"},
    {"5_3", "0.51,0.81 / 0.39,0.82"}, {"5_4", "0.58,0.81 / 0.48,0.82"},
    {"5_5", "0.65,0.81 / 0.57,0.82"}, {"5_6", "0.72,0.81 / 0.66,0.82"},
    {"5_7", "0.79,0.81 / 0.75,0.82"}, {"5_8", "0.87,0.81 / 0.84,0.82"},
    {"5_9", "0.94,0.81 / 0.92,0.82"},
};

struct DefaultGamepadBind {
    const char *button;
    const char *bind;
};

const DefaultGamepadBind kDefaultGamepadBinds[] = {
    {"start", "bind_pause"}, {"x", "bind_power_1"},     {"y", "bind_power_2"},
    {"b", "bind_power_3"},   {"l3", "bind_plant_food"}, {"r3", "bind_shovel"},
};

void seed_default_board_and_gamepad_binds(FILE *f) {
    fputs("\n[board]\n"
          "; Lawn cell centers for gamepad D-pad / R1. No key — coordinates only.\n"
          "; Format: position_ROW_COL = x16,y16 / x43,y43\n",
          f);
    for (const DefaultBoardCell &c : kDefaultBoard)
        std::fprintf(f, "position_%s = %s\n", c.row_col, c.value);

    fputs("\n[gamepad_binds]\n"
          "; Map a gamepad button to an existing [shortcuts] bind_* (coords reused).\n"
          "; NULL or empty = unmapped. Reserved in-game: A, L1/R1/L2/R2, D-pad, sticks, Back.\n",
          f);
    for (const DefaultGamepadBind &b : kDefaultGamepadBinds)
        std::fprintf(f, "%s = %s\n", b.button, b.bind);
}

bool board_has_cell(int row, int col) {
    for (unsigned i = 0; i < g_config.board_count; ++i)
        if (g_config.board[i].row == row && g_config.board[i].col == col) return true;
    return false;
}

bool shortcut_has_name(const char *name) {
    for (unsigned i = 0; i < g_config.shortcut_count; ++i)
        if (SDL_strcasecmp(g_config.shortcuts[i].name, name) == 0) return true;
    return false;
}

bool gamepad_has_button(const char *button) {
    for (unsigned i = 0; i < g_config.gamepad_bind_count; ++i)
        if (SDL_strcasecmp(g_config.gamepad_binds[i].button, button) == 0) return true;
    return false;
}

void apply_builtin_shortcut_defaults(void) {
    for (const DefaultShortcut &d : kDefaultShortcuts) {
        if (shortcut_has_name(d.name)) continue;
        char key[64];
        std::snprintf(key, sizeof(key), "bind_%s", d.name);
        parse_shortcut_bind(key, d.value);
    }
}

void apply_builtin_board_defaults(void) {
    for (const DefaultBoardCell &c : kDefaultBoard) {
        int row = 0, col = 0;
        if (std::sscanf(c.row_col, "%d_%d", &row, &col) != 2) continue;
        if (board_has_cell(row, col)) continue;
        char key[32];
        std::snprintf(key, sizeof(key), "position_%s", c.row_col);
        parse_board_cell(key, c.value);
    }
}

void apply_builtin_gamepad_bind_defaults(void) {
    for (const DefaultGamepadBind &d : kDefaultGamepadBinds) {
        if (gamepad_has_button(d.button)) continue;
        parse_gamepad_bind_entry(d.button, d.bind);
    }
}

/* Writes one already-trimmed value into its config field, per the row's kind.
 * Invalid tokens leave the pre-applied default untouched. */
void store(const Setting &s, const std::string &raw) {
    switch (s.kind) {
        case Kind::kBool: {
            int v = 0;
            if (parse_bool_token(raw, &v)) *(int *)field(s) = v;
            break;
        }
        case Kind::kUInt: {
            unsigned long v = 0;
            if (parse_uint_token(raw, &v)) *(unsigned *)field(s) = (unsigned)v;
            break;
        }
        case Kind::kInt: {
            long v = 0;
            if (parse_int_token(raw, &v)) *(int *)field(s) = (int)v;
            break;
        }
        case Kind::kFloat: {
            double v = 0.0;
            if (parse_finite_number(raw, &v)) *(float *)field(s) = (float)v;
            break;
        }
        case Kind::kString:
        case Kind::kPath: {
            const std::string v = trim(raw);
            if (v.empty()) return; /* an empty value is "unset", not "clear it" */
            std::snprintf((char *)field(s), s.capacity, "%s", v.c_str());
            break;
        }
        case Kind::kLower: {
            const std::string v = lower(trim(raw));
            if (v.empty()) return;
            std::snprintf((char *)field(s), s.capacity, "%s", v.c_str());
            break;
        }
        case Kind::kNetwork: {
            /* Named rather than numeric because the numbers are Android's
             * (AndroidHttpProxy.GetNetworkStatus), not something a player should
             * have to know. An unrecognised word keeps the default. */
            const std::string t = lower(trim(raw));
            if (t == "wifi" || t == "on" || t == "1" || t == "true" || t == "yes")
                *(int *)field(s) = 2;
            else if (t == "mobile" || t == "cell")
                *(int *)field(s) = 1;
            else if (t == "none" || t == "off" || t == "0" || t == "false" || t == "no")
                *(int *)field(s) = 0;
            break;
        }
        case Kind::kFps: {
            const std::string t = trim(raw);
            long v = 0;
            /* A value with no digits at all (typo, empty) keeps the default rather
             * than reading as 0 and silently uncapping the loop; a real number
             * <= 0 is the documented "no limit". */
            if (parse_int_token(t, &v, 10)) *(int *)field(s) = v > 0 ? (int)v : 0;
            break;
        }
    }
}

bool value_ok_for_setting(const Setting &s, const std::string &raw) {
    switch (s.kind) {
        case Kind::kBool: {
            int v = 0;
            return parse_bool_token(raw, &v);
        }
        case Kind::kUInt: {
            unsigned long v = 0;
            return parse_uint_token(raw, &v);
        }
        case Kind::kInt: {
            long v = 0;
            return parse_int_token(raw, &v);
        }
        case Kind::kFloat: {
            double v = 0.0;
            return parse_finite_number(raw, &v);
        }
        case Kind::kString:
        case Kind::kPath:
        case Kind::kLower:
            /* Empty means "unset" and is allowed on disk for optional paths;
             * required keys with empty seed still count as present. */
            return true;
        case Kind::kNetwork: {
            const std::string t = lower(trim(raw));
            return t == "wifi" || t == "on" || t == "1" || t == "true" || t == "yes" ||
                   t == "mobile" || t == "cell" || t == "none" || t == "off" || t == "0" ||
                   t == "false" || t == "no";
        }
        case Kind::kFps: {
            long v = 0;
            return parse_int_token(raw, &v, 10);
        }
    }
    return false;
}

void apply(const std::string &section, const std::string &key, const std::string &val) {
    if (section == "shortcuts" && parse_shortcut_bind(key, val)) return;
    if ((section == "board" || section == "tablero") && parse_board_cell(key, val)) return;
    if (section == "gamepad_binds" && parse_gamepad_bind_entry(key, val)) return;

    for (const Setting &s : kSettings) {
        if (section != s.section) continue;
        if (key != s.key && (s.alias == nullptr || key != s.alias)) continue;
        store(s, val);
        return;
    }
    /* Unknown (section, key) pairs are silently ignored -- see the header. */
}

bool apply_override_name(const std::string &raw_name, const std::string &raw_value) {
    const std::string name = lower(trim(raw_name));
    if (name.empty()) return false;

    const std::size_t dot = name.find('.');
    if (dot != std::string::npos) {
        const std::string section = trim(name.substr(0, dot));
        const std::string key = trim(name.substr(dot + 1));
        if (section.empty() || key.empty() || section == "shortcuts") return false;

        for (const Setting &s : kSettings) {
            if (section != s.section) continue;
            if (key != s.key && (s.alias == nullptr || key != s.alias)) continue;
            store(s, raw_value);
            return true;
        }
        return false;
    }

    if (name.rfind("bind_", 0) == 0) return false;

    const Setting *found = nullptr;
    for (const Setting &s : kSettings) {
        if (name != s.key && (s.alias == nullptr || name != s.alias)) continue;
        if (found != nullptr) return false; /* ambiguo sin sección */
        found = &s;
    }
    if (!found) return false;
    store(*found, raw_value);
    return true;
}

/* Parses an already-open INI stream into g_config. */
void parse(FILE *f) {
    std::string section;
    char line[1024];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        std::string s = trim(line);
        if (s.empty() || s[0] == ';' || s[0] == '#') continue;
        if (s.front() == '[' && s.back() == ']') {
            section = lower(trim(s.substr(1, s.size() - 2)));
            continue;
        }
        const std::size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        apply(section, lower(trim(s.substr(0, eq))), s.substr(eq + 1));
    }
}

/* --- the seeded file ------------------------------------------------------- */

void write_comment(FILE *f, const char *text) {
    fputs("; ", f);
    for (const char *p = text; *p != '\0'; ++p) {
        std::fputc(*p, f);
        if (*p == '\n') fputs("; ", f);
    }
    std::fputc('\n', f);
}

/* The commented-out example value for a row that has no literal default.
 *
 * The two resource paths derive theirs from the same constants the loader falls
 * back to, keyed on the field rather than the key name. They used to be separate
 * string literals in the seeded text, and the advertised .obb name did not match
 * the one the code actually looked for. */
std::string example_for(const Setting &s) {
    if (s.offset == offsetof(pvz2_config_t, so_path)) return std::string(kDefaultSoName);
    if (s.offset == offsetof(pvz2_config_t, obb_path)) return std::string(kDefaultObbName);
    if (s.offset == offsetof(pvz2_config_t, log_file)) return std::string(kDefaultLogFileName);
    return s.example != nullptr ? s.example : "";
}

int detected_display_refresh_hz(void);
int detected_default_fps_limit(void);
std::string detected_host_device_name(void);
std::string detected_host_hardware_model(void);
std::string generated_player_uuid(void);

const char *seed_value_for(const Setting &s, char *tmp, std::size_t tmp_cap) {
    if (s.offset == offsetof(pvz2_config_t, user_locale) && g_config.user_locale[0] != '\0')
        return g_config.user_locale;
    if (s.offset == offsetof(pvz2_config_t, currency_symbol) && g_config.currency_symbol[0] != '\0')
        return g_config.currency_symbol;
    if (s.offset == offsetof(pvz2_config_t, currency_code) && g_config.currency_code[0] != '\0')
        return g_config.currency_code;
    if (s.offset == offsetof(pvz2_config_t, device_name)) {
        const std::string value = g_config.device_name[0] != '\0' ? g_config.device_name
                                                                   : detected_host_device_name();
        std::snprintf(tmp, tmp_cap, "%s", value.c_str());
        return tmp;
    }
    if (s.offset == offsetof(pvz2_config_t, hardware_model)) {
        const std::string value = g_config.hardware_model[0] != '\0' ? g_config.hardware_model
                                                                      : detected_host_hardware_model();
        std::snprintf(tmp, tmp_cap, "%s", value.c_str());
        return tmp;
    }
    if (s.offset == offsetof(pvz2_config_t, player_uuid)) {
        const std::string value = g_config.player_uuid[0] != '\0' ? g_config.player_uuid
                                                                   : generated_player_uuid();
        std::snprintf(tmp, tmp_cap, "%s", value.c_str());
        return tmp;
    }
    if (s.offset == offsetof(pvz2_config_t, fps_limit)) {
        std::snprintf(tmp, tmp_cap, "%d",
                      g_config.fps_limit > 0 ? g_config.fps_limit : detected_default_fps_limit());
        return tmp;
    }
    return s.seed;
}

/* Generated from kSettings, so it documents exactly what the parser accepts and
 * exactly the values it defaults to. Best-effort: failure (read-only folder) is
 * fine, since the in-memory defaults are the same ones this file would state. */
void seed_default(const char *ini_path) {
    FILE *f = std::fopen(ini_path, "wb");
    if (f == nullptr) return;

    fputs("; PvZ2 configuration.\n"
          ";\n"
          "; Written automatically on first run, with every switch at its default --\n"
          "; deleting the file changes nothing about how the game behaves. Set a value\n"
          "; to change it; booleans accept 1/0, true/false, yes/no, on/off. This is the\n"
          "; single source of truth -- the old PVZ2_* environment variables are gone.\n",
          f);

    const char *current = nullptr;
    char dynamic_seed[64];
    for (const Setting &s : kSettings) {
        if (current == nullptr || std::strcmp(current, s.section) != 0) {
            current = s.section;
            std::fprintf(f, "\n[%s]\n", s.section);
        }
        if (s.comment != nullptr) write_comment(f, s.comment);
        if (s.seed != nullptr) {
            std::fprintf(f, "%s = %s\n", s.key, seed_value_for(s, dynamic_seed, sizeof(dynamic_seed)));
        } else {
            std::fprintf(f, "; %s = %s\n", s.key, example_for(s).c_str());
        }
    }

    fputs("\n[shortcuts]\n"
          "; Dynamic keyboard-to-tap binds. Any key that starts with bind_ is parsed.\n"
          "; Format: bind_name = KEY / x16,y16 [/ x43,y43]\n"
          ";   KEY     = SDL key name (examples: 1, Q, SPACE, ENTER)\n"
          ";   x16,y16 = normalized tap on 16:9 layouts (always used by the settings preview)\n"
          ";   x43,y43 = optional normalized tap on 4:3 layouts; if omitted, 16:9 is used\n"
          "; Spaces around '/' are optional. Legacy KEY,x,y is still accepted.\n",
          f);
    for (const DefaultShortcut &d : kDefaultShortcuts)
        std::fprintf(f, "bind_%s = %s\n", d.name, d.value);

    seed_default_board_and_gamepad_binds(f);

    std::fclose(f);
}

/* --- path resolution ------------------------------------------------------- */

void set_path(char *dst, std::size_t cap, const std::string &v) {
    std::snprintf(dst, cap, "%s", v.c_str());
}

/* base_dir with a guaranteed trailing separator (pvz2_host_data_path /
 * SDL_GetBasePath supply one, but callers may pass a hand-built path). */
std::string base_with_sep(const char *base_dir) {
    std::string base = (base_dir != nullptr) ? base_dir : "";
    if (!base.empty() && base.back() != '/' && base.back() != '\\') base.push_back('/');
    return base;
}

/* A drive-qualified ("C:/..."), UNC, or root-relative path is used verbatim;
 * anything else is treated as relative to the executable's folder, so a
 * config.ini can say `so = libPVZ2.so` and still work from any CWD. */
bool is_absolute(const char *p) {
    if (p == nullptr || p[0] == '\0') return false;
    if (p[0] == '/' || p[0] == '\\') return true;
    return std::isalpha((unsigned char)p[0]) && p[1] == ':';
}

/* Gives a path field its final, concrete value: the caller's `fallback` when the
 * file set nothing, otherwise the file's value resolved against the exe folder. */
void finish_path(char *dst, std::size_t cap, const char *base_dir, const std::string &fallback) {
    if (dst[0] == '\0') {
        set_path(dst, cap, fallback);
    } else if (!is_absolute(dst)) {
        set_path(dst, cap, base_with_sep(base_dir) + dst);
    }
}

int detected_default_fps_limit(void) {
    return detected_display_refresh_hz() >= 60 ? 60 : 30;
}

std::string detected_host_device_name(void) {
    const char *platform = SDL_GetPlatform();
    return (platform != nullptr && platform[0] != '\0') ? std::string(platform) : std::string("Windows");
}

/* Reads PRETTY_NAME="..." out of /etc/os-release (or /usr/lib/os-release as a
 * fallback), the standard cross-distro way to get a human string like
 * "Ubuntu 22.04.3 LTS". Empty return means "not found / not parseable". */
#ifndef _WIN32
std::string linux_pretty_name(void) {
    const char *candidates[] = {"/etc/os-release", "/usr/lib/os-release"};
    for (const char *path : candidates) {
        FILE *f = std::fopen(path, "r");
        if (f == nullptr) continue;
        char line[512];
        std::string pretty;
        while (std::fgets(line, sizeof(line), f) != nullptr) {
            if (std::strncmp(line, "PRETTY_NAME=", 12) != 0) continue;
            pretty = trim(line + 12);
            if (!pretty.empty() && pretty.front() == '"') {
                const std::size_t close = pretty.find('"', 1);
                pretty = pretty.substr(1, close == std::string::npos ? std::string::npos
                                                                      : close - 1);
            }
            break;
        }
        std::fclose(f);
        if (!pretty.empty()) return pretty;
    }
    return std::string();
}
#endif

std::string detected_host_hardware_model(void) {
#ifdef _WIN32
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll != nullptr) {
        using RtlGetVersionFn = LONG(WINAPI *)(OSVERSIONINFOW *);
        RtlGetVersionFn rtl_get_version =
            reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
        if (rtl_get_version != nullptr && rtl_get_version(&info) == 0) {
            if (info.dwMajorVersion == 10) {
                return info.dwBuildNumber >= 22000 ? "Microsoft Windows 11" : "Microsoft Windows 10";
            }
            if (info.dwMajorVersion == 6 && info.dwMinorVersion == 3) return "Microsoft Windows 8.1";
            if (info.dwMajorVersion == 6 && info.dwMinorVersion == 2) return "Microsoft Windows 8";
            if (info.dwMajorVersion == 6 && info.dwMinorVersion == 1) return "Microsoft Windows 7";
            char version[64];
            std::snprintf(version, sizeof(version), "Microsoft Windows %lu.%lu",
                          (unsigned long)info.dwMajorVersion, (unsigned long)info.dwMinorVersion);
            return version;
        }
    }
    /* RtlGetVersion unavailable (should not happen on a real Windows box) --
     * still Windows, so the "Microsoft" branding is correct even here. */
    return "Microsoft Windows";
#elif defined(__APPLE__)
    /* Not "Microsoft" anything -- this is Apple's OS. uname's release is the
     * Darwin kernel version (e.g. "23.1.0"), which is the closest portable
     * identifier without pulling in Cocoa/Foundation for a version string. */
    struct utsname uts{};
    if (uname(&uts) == 0 && uts.release[0] != '\0') return std::string("macOS (Darwin ") + uts.release + ")";
    return "macOS";
#elif defined(__linux__)
    /* Prefer the distro's own PRETTY_NAME (e.g. "Ubuntu 22.04.3 LTS"); fall back
     * to "Linux <kernel release>" if os-release is missing (minimal/embedded
     * systems). Never "Microsoft" -- that was a copy-paste of the Windows
     * branding onto every other platform. */
    const std::string pretty = linux_pretty_name();
    if (!pretty.empty()) return pretty;
    struct utsname uts{};
    if (uname(&uts) == 0 && uts.release[0] != '\0') return std::string("Linux ") + uts.release;
    return "Linux";
#else
    /* Any other SDL-supported platform: use SDL's own name verbatim rather than
     * inventing one, and definitely not "Microsoft " + it. */
    return detected_host_device_name();
#endif
}

std::string generated_player_uuid(void) {
    std::random_device rd;
    std::uniform_int_distribution<unsigned int> dist(0, 255);
    unsigned char bytes[16];
    for (unsigned char &b : bytes) b = (unsigned char)dist(rd);

    bytes[6] = (unsigned char)((bytes[6] & 0x0F) | 0x40); /* version 4 */
    bytes[8] = (unsigned char)((bytes[8] & 0x3F) | 0x80); /* variant 1 */

    char out[64];
    std::snprintf(out, sizeof(out),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
                  bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14],
                  bytes[15]);
    return out;
}

std::string detected_locale_tag(void) {
    SDL_Locale *locs = SDL_GetPreferredLocales();
    if (!locs) return "";

    std::string out;
    if (locs[0].language && locs[0].language[0] != '\0') {
        out = locs[0].language;
        if (locs[0].country && locs[0].country[0] != '\0') {
            out.push_back('_');
            out += locs[0].country;
        }
    }
    SDL_free(locs);
    return out;
}

void detect_currency_from_locale(const std::string &locale, const char **code, const char **symbol) {
    *code = "USD";
    *symbol = "$";
    if (locale.size() < 2) return;

    const std::string t = lower(locale);
    if (t.rfind("es_ar", 0) == 0 || t.rfind("es_cl", 0) == 0 || t.rfind("es_co", 0) == 0 ||
        t.rfind("es_mx", 0) == 0 || t.rfind("es_uy", 0) == 0) {
        *code = "ARS";
        *symbol = "$";
        if (t.rfind("es_cl", 0) == 0) *code = "CLP";
        if (t.rfind("es_co", 0) == 0) *code = "COP";
        if (t.rfind("es_mx", 0) == 0) *code = "MXN";
        if (t.rfind("es_uy", 0) == 0) *code = "UYU";
        return;
    }
    if (t.rfind("es_es", 0) == 0 || t.rfind("fr_", 0) == 0 || t.rfind("de_", 0) == 0 ||
        t.rfind("it_", 0) == 0 || t.rfind("pt_pt", 0) == 0 || t.rfind("nl_", 0) == 0) {
        *code = "EUR";
        *symbol = "€";
        return;
    }
    if (t.rfind("pt_br", 0) == 0) {
        *code = "BRL";
        *symbol = "R$";
        return;
    }
    if (t.rfind("en_gb", 0) == 0) {
        *code = "GBP";
        *symbol = "£";
        return;
    }
    if (t.rfind("ja_", 0) == 0) {
        *code = "JPY";
        *symbol = "¥";
        return;
    }
    if (t.rfind("ko_", 0) == 0) {
        *code = "KRW";
        *symbol = "₩";
        return;
    }
    if (t.rfind("zh_cn", 0) == 0 || t.rfind("zh_sg", 0) == 0) {
        *code = "CNY";
        *symbol = "¥";
        return;
    }
    if (t.rfind("zh_tw", 0) == 0 || t.rfind("zh_hk", 0) == 0) {
        *code = "TWD";
        *symbol = "NT$";
        return;
    }
    if (t.rfind("en_ca", 0) == 0 || t.rfind("fr_ca", 0) == 0) {
        *code = "CAD";
        *symbol = "$";
        return;
    }
    if (t.rfind("en_au", 0) == 0) {
        *code = "AUD";
        *symbol = "$";
        return;
    }
    if (t.rfind("en_nz", 0) == 0) {
        *code = "NZD";
        *symbol = "$";
        return;
    }
    if (t.rfind("en_in", 0) == 0 || t.rfind("hi_", 0) == 0) {
        *code = "INR";
        *symbol = "₹";
        return;
    }
    if (t.rfind("ru_", 0) == 0) {
        *code = "RUB";
        *symbol = "₽";
        return;
    }
    if (t.rfind("tr_", 0) == 0) {
        *code = "TRY";
        *symbol = "₺";
        return;
    }
}

int detected_display_refresh_hz(void) {
    SDL_DisplayMode dm;
    if (SDL_GetCurrentDisplayMode(0, &dm) != 0) return 60;
    const int hz = dm.refresh_rate;
    return hz > 0 ? hz : 60;
}

void apply_host_autodetect_defaults(void) {
    const std::string loc = detected_locale_tag();
    if (!loc.empty()) {
        std::snprintf(g_config.user_locale, sizeof(g_config.user_locale), "%s", loc.c_str());
    } else {
        std::snprintf(g_config.user_locale, sizeof(g_config.user_locale), "en_US");
    }

    const std::string device_name = detected_host_device_name();
    const std::string hardware_model = detected_host_hardware_model();
    const std::string player_uuid = generated_player_uuid();
    std::snprintf(g_config.device_name, sizeof(g_config.device_name), "%s", device_name.c_str());
    std::snprintf(g_config.hardware_model, sizeof(g_config.hardware_model), "%s",
                  hardware_model.c_str());
    std::snprintf(g_config.player_uuid, sizeof(g_config.player_uuid), "%s", player_uuid.c_str());

    g_config.fps_limit = detected_default_fps_limit();

    const char *code = "USD";
    const char *symbol = "$";
    detect_currency_from_locale(g_config.user_locale, &code, &symbol);
    std::snprintf(g_config.currency_code, sizeof(g_config.currency_code), "%s", code);
    std::snprintf(g_config.currency_symbol, sizeof(g_config.currency_symbol), "%s", symbol);
}

/* The defaults that cannot be literals in kSettings because they depend on where
 * the executable lives. Runs after parsing, so a value from the file wins. */
void finish_defaults(const char *base_dir) {
    const std::string base = base_with_sep(base_dir);
    finish_path(g_config.so_path, sizeof(g_config.so_path), base_dir, base + kDefaultSoName);
    finish_path(g_config.obb_path, sizeof(g_config.obb_path), base_dir, base + kDefaultObbName);
    finish_path(g_config.save_dir, sizeof(g_config.save_dir), base_dir, base);
    /* Optional path: an unset log_file stays empty rather than inventing a
     * default (that would create a log file the player never asked for). */
    if (g_config.log_file[0] != '\0' && !is_absolute(g_config.log_file))
        set_path(g_config.log_file, sizeof(g_config.log_file), base + g_config.log_file);

    if (g_config.user_locale[0] == '\0') {
        const std::string loc = detected_locale_tag();
        if (!loc.empty()) {
            std::snprintf(g_config.user_locale, sizeof(g_config.user_locale), "%s", loc.c_str());
        } else {
            std::snprintf(g_config.user_locale, sizeof(g_config.user_locale), "en_US");
        }
    }

    if (g_config.device_name[0] == '\0') {
        const std::string device_name = detected_host_device_name();
        std::snprintf(g_config.device_name, sizeof(g_config.device_name), "%s", device_name.c_str());
    }

    if (g_config.hardware_model[0] == '\0') {
        const std::string hardware_model = detected_host_hardware_model();
        std::snprintf(g_config.hardware_model, sizeof(g_config.hardware_model), "%s",
                      hardware_model.c_str());
    }

    if (g_config.player_uuid[0] == '\0') {
        const std::string player_uuid = generated_player_uuid();
        std::snprintf(g_config.player_uuid, sizeof(g_config.player_uuid), "%s", player_uuid.c_str());
    }

    if (g_config.fps_limit <= 0) {
        g_config.fps_limit = detected_default_fps_limit();
    }

    if (g_config.currency_code[0] == '\0' || g_config.currency_symbol[0] == '\0') {
        const char *code = "USD";
        const char *symbol = "$";
        detect_currency_from_locale(g_config.user_locale, &code, &symbol);
        if (g_config.currency_code[0] == '\0')
            std::snprintf(g_config.currency_code, sizeof(g_config.currency_code), "%s", code);
        if (g_config.currency_symbol[0] == '\0')
            std::snprintf(g_config.currency_symbol, sizeof(g_config.currency_symbol), "%s", symbol);
    }

    apply_builtin_shortcut_defaults();
    apply_builtin_board_defaults();
    apply_builtin_gamepad_bind_defaults();
}

/* --- config.ini migration / heal ------------------------------------------ */

std::string trim_line_ending(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

std::string format_live_setting(const Setting &s) {
    char buf[256];
    switch (s.kind) {
        case Kind::kBool:
            return *(int *)field(s) ? "1" : "0";
        case Kind::kUInt:
            std::snprintf(buf, sizeof(buf), "%u", *(unsigned *)field(s));
            return buf;
        case Kind::kInt:
        case Kind::kFps:
            std::snprintf(buf, sizeof(buf), "%d", *(int *)field(s));
            return buf;
        case Kind::kFloat:
            std::snprintf(buf, sizeof(buf), "%.2f", (double)*(float *)field(s));
            return buf;
        case Kind::kString:
        case Kind::kPath:
        case Kind::kLower:
            return (char *)field(s);
        case Kind::kNetwork: {
            const int n = *(int *)field(s);
            if (n == 2) return "wifi";
            if (n == 1) return "mobile";
            return "none";
        }
    }
    return "";
}

/* Replace an existing key or append it at the end of [section]. Creates the
 * section at EOF when missing. Returns true if lines were modified. */
bool upsert_ini_key(std::vector<std::string> &lines, const char *section, const char *key,
                    const char *value) {
    const std::string sect = std::string("[") + section + "]";
    const std::string new_line = std::string(key) + " = " + value + "\n";
    bool in_section = false;
    std::size_t insert_at = std::string::npos;

    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string trimmed = trim_line_ending(lines[i]);
        if (trimmed == sect) {
            in_section = true;
            insert_at = i + 1;
            continue;
        }
        if (!trimmed.empty() && trimmed[0] == '[') {
            if (in_section) break;
            in_section = false;
            continue;
        }
        if (!in_section) continue;

        insert_at = i + 1;
        std::string stripped = trim(trimmed);
        if (stripped.empty() || stripped[0] == ';' || stripped[0] == '#') continue;
        const std::size_t eq = stripped.find('=');
        if (eq == std::string::npos) continue;
        std::string k = lower(trim(stripped.substr(0, eq)));
        if (k != lower(key)) continue;
        if (trim_line_ending(lines[i]) == trim_line_ending(new_line)) return false;
        lines[i] = new_line;
        return true;
    }

    if (insert_at != std::string::npos) {
        lines.insert(lines.begin() + (std::ptrdiff_t)insert_at, new_line);
        return true;
    }
    if (!lines.empty() && trim_line_ending(lines.back()).size() > 0)
        lines.push_back("\n");
    lines.push_back(sect + "\n");
    lines.push_back(new_line);
    return true;
}

/* Look up key in section; returns true if an active (non-comment) line exists. */
bool find_ini_key(const std::vector<std::string> &lines, const char *section, const char *key,
                  std::string *value_out) {
    const std::string sect = std::string("[") + section + "]";
    bool in_section = false;
    for (const auto &l : lines) {
        const std::string trimmed = trim_line_ending(l);
        if (trimmed == sect) {
            in_section = true;
            continue;
        }
        if (!trimmed.empty() && trimmed[0] == '[') {
            in_section = false;
            continue;
        }
        if (!in_section) continue;
        std::string stripped = trim(trimmed);
        if (stripped.empty() || stripped[0] == ';' || stripped[0] == '#') continue;
        const std::size_t eq = stripped.find('=');
        if (eq == std::string::npos) continue;
        if (lower(trim(stripped.substr(0, eq))) != lower(key)) continue;
        if (value_out) *value_out = stripped.substr(eq + 1);
        return true;
    }
    return false;
}

/* True if [section] has an active OR commented `key = ...` line. Used so heal
 * can add a newly documented optional key without treating a player's commented
 * copy as missing (which would duplicate it). */
bool ini_mentions_key(const std::vector<std::string> &lines, const char *section, const char *key) {
    const std::string sect = std::string("[") + section + "]";
    const std::string want = lower(key);
    bool in_section = false;
    for (const auto &l : lines) {
        const std::string trimmed = trim_line_ending(l);
        if (trimmed == sect) {
            in_section = true;
            continue;
        }
        if (!trimmed.empty() && trimmed[0] == '[') {
            in_section = false;
            continue;
        }
        if (!in_section) continue;
        std::string stripped = trim(trimmed);
        if (stripped.empty()) continue;
        if (stripped[0] == ';' || stripped[0] == '#') stripped = trim(stripped.substr(1));
        const std::size_t eq = stripped.find('=');
        if (eq == std::string::npos) continue;
        if (lower(trim(stripped.substr(0, eq))) == want) return true;
    }
    return false;
}

/* Inserts `; key = example` at the end of [section] when the key is absent even
 * as a comment. Does not uncomment or invent an active default. */
bool insert_commented_ini_key(std::vector<std::string> &lines, const char *section, const char *key,
                              const std::string &example, const char *comment = nullptr) {
    if (ini_mentions_key(lines, section, key)) return false;
    const std::string sect = std::string("[") + section + "]";
    std::string new_line;
    if (comment != nullptr) {
        std::string c = comment;
        std::size_t p = 0;
        while (p <= c.size()) {
            std::size_t e = c.find('\n', p);
            if (e == std::string::npos) e = c.size();
            new_line += "; " + c.substr(p, e - p) + "\n";
            p = e + 1;
        }
    }
    new_line += std::string("; ") + key + " = " + example + "\n";
    bool in_section = false;
    std::size_t insert_at = std::string::npos;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string trimmed = trim_line_ending(lines[i]);
        if (trimmed == sect) {
            in_section = true;
            insert_at = i + 1;
            continue;
        }
        if (!trimmed.empty() && trimmed[0] == '[') {
            if (in_section) break;
            in_section = false;
            continue;
        }
        if (in_section) insert_at = i + 1;
    }
    if (insert_at != std::string::npos) {
        lines.insert(lines.begin() + (std::ptrdiff_t)insert_at, new_line);
        return true;
    }
    if (!lines.empty() && trim_line_ending(lines.back()).size() > 0) lines.push_back("\n");
    lines.push_back(sect + "\n");
    lines.push_back(new_line);
    return true;
}

bool section_exists(const std::vector<std::string> &lines, const char *section) {
    const std::string sect = std::string("[") + section + "]";
    for (const auto &l : lines)
        if (trim_line_ending(l) == sect) return true;
    return false;
}

const char *board_section_name(const std::vector<std::string> &lines) {
    if (section_exists(lines, "board")) return "board";
    if (section_exists(lines, "tablero")) return "tablero";
    return "board";
}

/* Migrate an older/incomplete config.ini in place: add missing required keys
 * and rewrite values that fail the type/format check. Best-effort. */
void heal_config_ini(const char *ini_path) {
    if (ini_path == nullptr) return;

    std::vector<std::string> lines;
    {
        FILE *f = std::fopen(ini_path, "rb");
        if (f == nullptr) return;
        char buf[1024];
        while (std::fgets(buf, sizeof(buf), f) != nullptr) lines.push_back(buf);
        std::fclose(f);
    }

    bool dirty = false;

    /* Required kSettings rows: those written active (seed != nullptr) on first run. */
    for (const Setting &s : kSettings) {
        if (s.seed == nullptr) continue;
        std::string raw;
        const char *write_key = s.key;
        bool present = find_ini_key(lines, s.section, s.key, &raw);
        if (!present && s.alias != nullptr && find_ini_key(lines, s.section, s.alias, &raw)) {
            present = true;
            write_key = s.alias; /* keep the player's spelling when fixing */
        }
        if (present && value_ok_for_setting(s, raw)) continue;
        if (upsert_ini_key(lines, s.section, write_key, format_live_setting(s).c_str())) dirty = true;
    }

    /* Optional [log] logfile stays commented unless the player uncomments it.
     * Seed it into older files so the switch is visible without creating a log. */
    if (insert_commented_ini_key(lines, "log", "logfile", kDefaultLogFileName)) dirty = true;

    /* Optional [video] display stays commented (0 = primary) unless the player
     * uncomments it, same as logfile above. */
    if (insert_commented_ini_key(lines, "video", "display", "0")) dirty = true;

    /* Optional [runtime] rewrite_cdn stays commented (0) unless the player
     * uncomments it -- older installs never had the key. */
    if (insert_commented_ini_key(lines, "runtime", "rewrite_cdn", "0")) dirty = true;

    /* Optional switches added later: shown commented, with their comment, in files made before them. */
    for (const Setting &s : kSettings) {
        const bool added_later = (std::strcmp(s.section, "runtime") == 0 &&
                                  (std::strcmp(s.key, "lod_legacy_hacks") == 0 || std::strcmp(s.key, "force_date") == 0)) ||
                                 (std::strcmp(s.section, "gl") == 0 && std::strcmp(s.key, "no_etc2") == 0);
        if (!added_later || s.seed != nullptr) continue;
        if (insert_commented_ini_key(lines, s.section, s.key, example_for(s), s.comment)) dirty = true;
    }

    for (const DefaultShortcut &d : kDefaultShortcuts) {
        char key[64];
        std::snprintf(key, sizeof(key), "bind_%s", d.name);
        std::string raw;
        const bool present = find_ini_key(lines, "shortcuts", key, &raw);
        std::string kn;
        double x = 0, y = 0, x43 = 0, y43 = 0;
        int has_43 = 0;
        if (present && decode_shortcut_value(raw, &kn, &x, &y, &x43, &y43, &has_43)) continue;
        if (upsert_ini_key(lines, "shortcuts", key, d.value)) dirty = true;
    }

    const char *board_sec = board_section_name(lines);
    for (const DefaultBoardCell &c : kDefaultBoard) {
        char key[32];
        std::snprintf(key, sizeof(key), "position_%s", c.row_col);
        std::string raw;
        const bool present = find_ini_key(lines, board_sec, key, &raw);
        double x = 0, y = 0, x43 = 0, y43 = 0;
        int has_43 = 0;
        if (present && decode_board_value(raw, &x, &y, &x43, &y43, &has_43)) continue;
        if (upsert_ini_key(lines, board_sec, key, c.value)) dirty = true;
    }

    for (const DefaultGamepadBind &b : kDefaultGamepadBinds) {
        std::string raw;
        const bool present = find_ini_key(lines, "gamepad_binds", b.button, &raw);
        /* Any present value (including NULL/empty) is accepted for binds. */
        if (present) continue;
        if (upsert_ini_key(lines, "gamepad_binds", b.button, b.bind)) dirty = true;
    }

    if (!dirty) return;

    FILE *f = std::fopen(ini_path, "wb");
    if (f == nullptr) return;
    for (const auto &l : lines) std::fputs(l.c_str(), f);
    std::fclose(f);
}

}  // namespace

extern "C" void pvz2_config_load(const char *ini_path, const char *base_dir) {
    g_config = pvz2_config_t{}; /* reset to all-zero before (re)reading */
    g_config.emulate_iap = 0;
    g_config.achievement_notifications = 1;
    g_config.persist_saves = 1;
    g_config.gamepad_enabled = 1;
    g_config.gamepad_stick_speed = 1.6f;
    g_config.gamepad_stick_deadzone = 0.25f;
    g_config.gamepad_trigger_threshold = 0.50f;
    std::snprintf(g_config.os_version, sizeof(g_config.os_version), "%s", "14");
    std::snprintf(g_config.video_mode, sizeof(g_config.video_mode), "%s", "auto");
    std::snprintf(g_config.package_name, sizeof(g_config.package_name), "%s", "com.ea.game.pvz2_na");
    std::snprintf(g_config.activity_name, sizeof(g_config.activity_name), "%s",
                  "com.popcap.PvZ2.PvZ2GameActivity");

    /* Every non-zero default, taken from the same strings the seeded file shows.
     * This must precede parsing so an explicit entry in the file can still
     * override it, and so a config.ini written by an OLDER build -- which has no
     * line for a newer key at all -- gets the intended default rather than a zero
     * that means something else entirely (fps_limit 0 = uncapped, network 0 =
     * offline, which used to disable the store). */
    for (const Setting &s : kSettings) {
        if (s.seed != nullptr) store(s, s.seed);
    }

    if (ini_path != nullptr) {
        FILE *f = std::fopen(ini_path, "rb");
        if (f == nullptr) { /* first run: detect host-based defaults, then seed */
            apply_host_autodetect_defaults();
            finish_defaults(base_dir);
            seed_default(ini_path);
            f = std::fopen(ini_path, "rb");
        }
        if (f != nullptr) {
            parse(f);
            std::fclose(f);
        }
    }

    finish_defaults(base_dir);

    /* Fill gaps / fix garbage in an older config.ini so the player never has to
     * delete the file by hand after an upgrade. */
    if (ini_path != nullptr) heal_config_ini(ini_path);
}

extern "C" int pvz2_config_override(const char *name, const char *value) {
    if (name == nullptr || value == nullptr) return 0;
    return apply_override_name(name, value) ? 1 : 0;
}

extern "C" const pvz2_config_t *pvz2_config(void) { return &g_config; }

extern "C" long long pvz2_time_offset_sec(void) {
    static const long long offset = [] {
        const char *s = g_config.force_date;
        if (s[0] == '\0') return 0LL;
        int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
        if (std::sscanf(s, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &sec) < 3 &&
            std::sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) < 3) {
            pvz2_log("[time] force_date '%s' ignored: expected YYYY-MM-DD HH:MM:SS", s);
            return 0LL;
        }
        std::tm tmv{};
        tmv.tm_year = y - 1900;
        tmv.tm_mon = mo - 1;
        tmv.tm_mday = d;
        tmv.tm_hour = h;
        tmv.tm_min = mi;
        tmv.tm_sec = sec;
        tmv.tm_isdst = -1;
        const std::time_t forced = std::mktime(&tmv);
        if (forced == (std::time_t)-1) {
            pvz2_log("[time] force_date '%s' ignored: not a valid date", s);
            return 0LL;
        }
        const long long off = (long long)forced - (long long)std::time(nullptr);
        pvz2_log("[time] force_date=%s -> the game's clock is %+lld s from the host clock", s, off);
        return off;
    }();
    return offset;
}

extern "C" int pvz2_config_update_bind_key(unsigned idx, const char *new_key) {
    if (!new_key || idx >= g_config.shortcut_count) return 0;

    /* Patch in-memory binding */
    std::snprintf(g_config.shortcuts[idx].key, sizeof(g_config.shortcuts[idx].key),
                  "%s", new_key);

    /* Patch config.ini on disk */
    char *base = pvz2_host_data_path();
    if (!base) return 1; /* memory updated but file not written */

    char ini_path[1024];
    SDL_snprintf(ini_path, sizeof(ini_path), "%sconfig.ini", base);
    SDL_free(base);

    /* Read existing lines */
    std::vector<std::string> lines;
    {
        FILE *f = std::fopen(ini_path, "rb");
        if (f) {
            char buf[512];
            while (std::fgets(buf, sizeof(buf), f))
                lines.push_back(buf);
            std::fclose(f);
        }
    }

    /* Build the key name: "bind_<name>" */
    std::string bind_key = std::string("bind_") + g_config.shortcuts[idx].name;

    /* Preserve both aspect pairs when rewriting the key. */
    const pvz2_shortcut_binding_t &b = g_config.shortcuts[idx];
    std::string full_val = format_bind_value(new_key, b.x, b.y, b.has_43, b.x43, b.y43);

    /* Find and replace the line in [shortcuts] section */
    bool in_shortcuts = false;
    bool found = false;
    for (auto &l : lines) {
        std::string trimmed = l;
        while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r'))
            trimmed.pop_back();
        if (trimmed == "[shortcuts]") { in_shortcuts = true; continue; }
        if (!trimmed.empty() && trimmed[0] == '[') { in_shortcuts = false; continue; }
        if (in_shortcuts) {
            std::string stripped = trimmed;
            while (!stripped.empty() && (stripped[0] == ';' || stripped[0] == ' '))
                stripped = stripped.substr(1);
            if (stripped.find(bind_key) == 0) {
                std::size_t eq = stripped.find('=');
                if (eq != std::string::npos) {
                    l = bind_key + " = " + full_val + "\n";
                    found = true;
                    break;
                }
            }
        }
    }
    if (!found) {
        /* Insert under [shortcuts] or append section */
        bool inserted = false;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            std::string t = lines[i];
            while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
            if (t == "[shortcuts]") {
                lines.insert(lines.begin() + (int)i + 1,
                             bind_key + " = " + full_val + "\n");
                inserted = true;
                break;
            }
        }
        if (!inserted) {
            lines.push_back("\n[shortcuts]\n");
            lines.push_back(bind_key + " = " + full_val + "\n");
        }
    }

    FILE *f = std::fopen(ini_path, "wb");
    if (f) {
        for (const auto &l : lines) std::fputs(l.c_str(), f);
        std::fclose(f);
    }
    return 1;
}

static bool is_remappable_pad_button(const char *button) {
    return button && (SDL_strcasecmp(button, "b") == 0 || SDL_strcasecmp(button, "x") == 0 ||
                      SDL_strcasecmp(button, "y") == 0 || SDL_strcasecmp(button, "l3") == 0 ||
                      SDL_strcasecmp(button, "r3") == 0);
}

static void normalize_gamepad_bind_name(const char *in, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!in || !in[0] || SDL_strcasecmp(in, "NULL") == 0) return;
    while (*in == ' ' || *in == '\t') ++in;
    if (!in[0] || SDL_strcasecmp(in, "NULL") == 0) return;
    if (std::strncmp(in, "bind_", 5) == 0)
        std::snprintf(out, out_sz, "%s", in);
    else
        std::snprintf(out, out_sz, "bind_%s", in);
}

static void patch_gamepad_bind_ini_line(std::vector<std::string> &lines, const char *button,
                                        const char *bind_name_or_null) {
    const std::string key = button;
    const std::string val =
        (bind_name_or_null && bind_name_or_null[0]) ? bind_name_or_null : "NULL";
    const std::string new_line = key + " = " + val + "\n";

    bool in_section = false;
    bool found = false;
    for (auto &l : lines) {
        std::string trimmed = l;
        while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r'))
            trimmed.pop_back();
        if (trimmed == "[gamepad_binds]") {
            in_section = true;
            continue;
        }
        if (!trimmed.empty() && trimmed[0] == '[') {
            in_section = false;
            continue;
        }
        if (!in_section) continue;

        std::string stripped = trimmed;
        while (!stripped.empty() && (stripped[0] == ' ' || stripped[0] == '\t'))
            stripped.erase(stripped.begin());
        if (!stripped.empty() && stripped[0] == ';') continue;

        std::size_t eq = stripped.find('=');
        if (eq == std::string::npos) continue;
        std::string k = stripped.substr(0, eq);
        while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
        if (SDL_strcasecmp(k.c_str(), button) != 0) continue;
        l = new_line;
        found = true;
        break;
    }

    if (found) return;

    bool inserted = false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::string t = lines[i];
        while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
        if (t == "[gamepad_binds]") {
            lines.insert(lines.begin() + (int)i + 1, new_line);
            inserted = true;
            break;
        }
    }
    if (!inserted) {
        lines.push_back("\n[gamepad_binds]\n");
        lines.push_back(new_line);
    }
}

extern "C" int pvz2_config_update_gamepad_bind(const char *button, const char *bind_name) {
    if (!is_remappable_pad_button(button)) return 0;

    char normalized[64];
    normalize_gamepad_bind_name(bind_name, normalized, sizeof(normalized));

    /* Uniqueness among remappable buttons: clear the same action elsewhere. */
    if (normalized[0]) {
        for (unsigned i = 0; i < g_config.gamepad_bind_count; ++i) {
            pvz2_gamepad_bind_t &e = g_config.gamepad_binds[i];
            if (!is_remappable_pad_button(e.button)) continue;
            if (SDL_strcasecmp(e.button, button) == 0) continue;
            if (SDL_strcasecmp(e.bind_name, normalized) != 0) continue;
            e.bind_name[0] = '\0';
        }
    }

    /* Upsert target button in memory. */
    int found_idx = -1;
    for (unsigned i = 0; i < g_config.gamepad_bind_count; ++i) {
        if (SDL_strcasecmp(g_config.gamepad_binds[i].button, button) == 0) {
            found_idx = (int)i;
            break;
        }
    }
    if (found_idx < 0) {
        if (g_config.gamepad_bind_count >= PVZ2_MAX_GAMEPAD_BINDS) return 0;
        found_idx = (int)g_config.gamepad_bind_count++;
        std::snprintf(g_config.gamepad_binds[found_idx].button,
                      sizeof(g_config.gamepad_binds[found_idx].button), "%s", button);
    }
    std::snprintf(g_config.gamepad_binds[found_idx].bind_name,
                  sizeof(g_config.gamepad_binds[found_idx].bind_name), "%s", normalized);

    char *base = pvz2_host_data_path();
    if (!base) return 1;

    char ini_path[1024];
    SDL_snprintf(ini_path, sizeof(ini_path), "%sconfig.ini", base);
    SDL_free(base);

    std::vector<std::string> lines;
    {
        FILE *f = std::fopen(ini_path, "rb");
        if (f) {
            char buf[512];
            while (std::fgets(buf, sizeof(buf), f)) lines.push_back(buf);
            std::fclose(f);
        }
    }

    /* Rewrite every remappable button we touched (target + cleared dupes). */
    static const char *kRemap[] = {"b", "x", "y", "l3", "r3"};
    for (const char *btn : kRemap) {
        const char *val = "";
        for (unsigned i = 0; i < g_config.gamepad_bind_count; ++i) {
            if (SDL_strcasecmp(g_config.gamepad_binds[i].button, btn) == 0) {
                val = g_config.gamepad_binds[i].bind_name;
                break;
            }
        }
        patch_gamepad_bind_ini_line(lines, btn, val);
    }

    FILE *f = std::fopen(ini_path, "wb");
    if (f) {
        for (const auto &l : lines) std::fputs(l.c_str(), f);
        std::fclose(f);
    }
    return 1;
}

static void patch_ini_section_key(std::vector<std::string> &lines, const char *section,
                                  const char *key, const char *value) {
    const std::string sect = std::string("[") + section + "]";
    const std::string new_line = std::string(key) + " = " + value + "\n";
    bool in_section = false;
    bool found = false;
    for (auto &l : lines) {
        std::string trimmed = l;
        while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r'))
            trimmed.pop_back();
        if (trimmed == sect) {
            in_section = true;
            continue;
        }
        if (!trimmed.empty() && trimmed[0] == '[') {
            in_section = false;
            continue;
        }
        if (!in_section) continue;

        std::string stripped = trimmed;
        while (!stripped.empty() && (stripped[0] == ' ' || stripped[0] == '\t'))
            stripped.erase(stripped.begin());
        if (!stripped.empty() && stripped[0] == ';') continue;

        std::size_t eq = stripped.find('=');
        if (eq == std::string::npos) continue;
        std::string k = stripped.substr(0, eq);
        while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
        if (SDL_strcasecmp(k.c_str(), key) != 0) continue;
        l = new_line;
        found = true;
        break;
    }
    if (found) return;

    bool inserted = false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::string t = lines[i];
        while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
        if (t == sect) {
            lines.insert(lines.begin() + (int)i + 1, new_line);
            inserted = true;
            break;
        }
    }
    if (!inserted) {
        lines.push_back("\n" + sect + "\n");
        lines.push_back(new_line);
    }
}

extern "C" int pvz2_effective_display(void) {
    const int n = SDL_GetNumVideoDisplays();
    if (n <= 0) return 0;
    if (g_config.video_display < 0 || g_config.video_display >= n) return 0;
    return g_config.video_display;
}

extern "C" int pvz2_config_update_display(int index) {
    g_config.video_display = index < 0 ? 0 : index;

    char *base = pvz2_host_data_path();
    if (!base) return 1;

    char ini_path[1024];
    SDL_snprintf(ini_path, sizeof(ini_path), "%sconfig.ini", base);
    SDL_free(base);

    std::vector<std::string> lines;
    {
        FILE *f = std::fopen(ini_path, "rb");
        if (f) {
            char buf[512];
            while (std::fgets(buf, sizeof(buf), f)) lines.push_back(buf);
            std::fclose(f);
        }
    }

    char idx_buf[16];
    SDL_snprintf(idx_buf, sizeof(idx_buf), "%d", g_config.video_display);
    patch_ini_section_key(lines, "video", "display", idx_buf);

    FILE *f = std::fopen(ini_path, "wb");
    if (f) {
        for (const auto &l : lines) std::fputs(l.c_str(), f);
        std::fclose(f);
    }
    return 1;
}

extern "C" int pvz2_config_update_gamepad_device(int enabled, int port) {
    g_config.gamepad_enabled = enabled ? 1 : 0;
    g_config.gamepad_port = port < 0 ? 0 : port;

    char *base = pvz2_host_data_path();
    if (!base) return 1;

    char ini_path[1024];
    SDL_snprintf(ini_path, sizeof(ini_path), "%sconfig.ini", base);
    SDL_free(base);

    std::vector<std::string> lines;
    {
        FILE *f = std::fopen(ini_path, "rb");
        if (f) {
            char buf[512];
            while (std::fgets(buf, sizeof(buf), f)) lines.push_back(buf);
            std::fclose(f);
        }
    }

    char port_buf[16];
    SDL_snprintf(port_buf, sizeof(port_buf), "%d", g_config.gamepad_port);
    patch_ini_section_key(lines, "gamepad", "enabled", g_config.gamepad_enabled ? "1" : "0");
    patch_ini_section_key(lines, "gamepad", "port", port_buf);

    FILE *f = std::fopen(ini_path, "wb");
    if (f) {
        for (const auto &l : lines) std::fputs(l.c_str(), f);
        std::fclose(f);
    }
    return 1;
}

extern "C" int pvz2_config_update_gamepad_stick_param(int which, float value, int persist) {
    if (which == 0) {
        if (value < 0.05f) value = 0.05f;
        if (value > 5.0f) value = 5.0f;
        g_config.gamepad_stick_speed = value;
    } else if (which == 1) {
        if (value < 0.0f) value = 0.0f;
        if (value > 0.9f) value = 0.9f;
        g_config.gamepad_stick_deadzone = value;
    } else {
        return 0;
    }

    if (!persist) return 1;

    char *base = pvz2_host_data_path();
    if (!base) return 1;

    char ini_path[1024];
    SDL_snprintf(ini_path, sizeof(ini_path), "%sconfig.ini", base);
    SDL_free(base);

    std::vector<std::string> lines;
    {
        FILE *f = std::fopen(ini_path, "rb");
        if (f) {
            char buf[512];
            while (std::fgets(buf, sizeof(buf), f)) lines.push_back(buf);
            std::fclose(f);
        }
    }

    char buf[32];
    if (which == 0) {
        SDL_snprintf(buf, sizeof(buf), "%.2f", g_config.gamepad_stick_speed);
        patch_ini_section_key(lines, "gamepad", "stick_speed", buf);
    } else {
        SDL_snprintf(buf, sizeof(buf), "%.2f", g_config.gamepad_stick_deadzone);
        patch_ini_section_key(lines, "gamepad", "stick_deadzone", buf);
    }

    FILE *f = std::fopen(ini_path, "wb");
    if (f) {
        for (const auto &l : lines) std::fputs(l.c_str(), f);
        std::fclose(f);
    }
    return 1;
}

extern "C" int pvz2_config_apply_overlay_settings(int fullscreen, int width, int height,
                                                  int fps_limit, const char *locale,
                                                  const char *currency_symbol,
                                                  const char *currency_code,
                                                  int emulate_iap,
                                                  int achievement_notifications) {
    const int locale_changed =
        (locale && locale[0] && std::strcmp(locale, g_config.user_locale) != 0) ? 1 : 0;
    const int iap_changed = ((emulate_iap ? 1 : 0) != g_config.emulate_iap) ? 1 : 0;

    g_config.video_fullscreen = fullscreen ? 1 : 0;
    g_config.video_width = width;
    g_config.video_height = height;
    g_config.fps_limit = fps_limit;
    g_config.emulate_iap = emulate_iap ? 1 : 0;
    g_config.achievement_notifications = achievement_notifications ? 1 : 0;

    if (locale && locale[0])
        std::snprintf(g_config.user_locale, sizeof(g_config.user_locale), "%s", locale);
    if (currency_symbol)
        std::snprintf(g_config.currency_symbol, sizeof(g_config.currency_symbol), "%s",
                      currency_symbol);
    if (currency_code)
        std::snprintf(g_config.currency_code, sizeof(g_config.currency_code), "%s",
                      currency_code);

    /* Locale: string tables load at boot. IAP: purchase driver / store path is
     * fixed for the session once the guest has asked. Both need a relaunch. */
    return (locale_changed || iap_changed) ? 1 : 0;
}
