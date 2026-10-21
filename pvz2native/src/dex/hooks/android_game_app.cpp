/* com.popcap.SexyAppFramework.AndroidGameApp
 *
 * The engine's main bridge into Android: device info, config storage, input
 * events, and -- most importantly -- asset access. Behaviour here was
 * ground-truthed against the decompiled classes.dex rather than guessed, and
 * each hook notes what the real Java does.
 */

#include <pvz2native/config.h>
#include <pvz2native/dependencies/vfs.h>
#include <pvz2native/dex/config_store.h>
#include <pvz2native/dex/dex.h>
#include <pvz2native/diagnostics/verbose.h>
#include <pvz2native/input/input_queue.h>

#include <SDL.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

namespace pvz2native {
namespace dex {
namespace {

constexpr const char *kClass = "com/popcap/SexyAppFramework/AndroidGameApp";

/* sub_9F0774 wraps 1024 bytes of its own stack as the direct ByteBuffer, and
 * starts reading records 16 bytes in (the count sits at +0). */
constexpr std::uint32_t kEventBufferSize = 1024;
constexpr std::uint32_t kEventRecordsOffset = 16;

/* Loads the .obb index on first use -- parsing 588 RSG headers costs a few ms
 * and nothing before the RSB load needs it. */
const RsbIndex::Entry *find_asset(DexCall &d, const std::string &name) {
    std::lock_guard<std::mutex> lk(d.c.rt->rsb_index_lock);
    if (!d.c.rt->rsb_index_tried) {
        d.c.rt->rsb_index_tried = true;
        d.c.rt->rsb_index.load(vfs::obb_host_path());
    }
    return d.c.rt->rsb_index.find(name);
}

/* --- [log] input diagnostics --------------------------------------------- */

std::uint32_t rd32(const std::vector<std::uint8_t> &v, std::size_t off) {
    if (off + 4 > v.size()) return 0;
    return (std::uint32_t)v[off] | ((std::uint32_t)v[off + 1] << 8) |
           ((std::uint32_t)v[off + 2] << 16) | ((std::uint32_t)v[off + 3] << 24);
}

/* Phase names as the ENGINE reads them (sub_9F0298's switch), not as the
 * numbering suggests: 3 is the end of a touch, 1 is a move. */
const char *touch_phase_name(std::uint32_t p) {
    switch (p) {
        case 0: return "DOWN";
        case 1: return "MOVE";
        case 3: return "UP";
        case 4: return "CANCEL";
        default: return "??";
    }
}

/* Walks the exact bytes just handed to the engine, stepping each record by the
 * size the engine will step it -- so a desync shows up here as an "unknown
 * record type" rather than as silently misread input. */
void log_records(DexCall &d, const std::vector<std::uint8_t> &r) {
    std::size_t off = 0;
    while (off + 4 <= r.size()) {
        const std::uint32_t type = rd32(r, off);
        if (type == 0) {
            const std::uint32_t phase = rd32(r, off + 36);
            d.c.log("[input]   touch %-6s (phase=%u) id=%u x=%d y=%d", touch_phase_name(phase),
                    phase, rd32(r, off + 4), (int)rd32(r, off + 8), (int)rd32(r, off + 12));
            off += 48;
        } else if (type == 1) {
            d.c.log("[input]   key code=%u down=%u", rd32(r, off + 4), rd32(r, off + 8));
            off += 32;
        } else if (type == 6) {
            const std::uint32_t n = rd32(r, off + 8);
            d.c.log("[input]   text len=%u", n);
            off += 12 + n + ((4 - n % 4) % 4);
        } else {
            //d.c.log("[input]   UNKNOWN record type %u at +%u -- encoder/engine disagree",
                    //type, (unsigned)off);
            break;
        }
    }
}

/* Same walk as log_records, but only to learn whether a touch started or ended
 * in this batch -- that feeds the [state] input counters in diagnostics/verbose. */
void scan_touches(const std::vector<std::uint8_t> &r, bool &had_down, bool &had_up) {
    std::size_t off = 0;
    while (off + 4 <= r.size()) {
        const std::uint32_t type = rd32(r, off);
        if (type == 0) {
            const std::uint32_t phase = rd32(r, off + 36);
            if (phase == 0) had_down = true;
            if (phase == 3) had_up = true;
            off += 48;
        } else if (type == 1) {
            off += 32;
        } else if (type == 6) {
            const std::uint32_t n = rd32(r, off + 8);
            off += 12 + n + ((4 - n % 4) % 4);
        } else {
            break;
        }
    }
}

/* boolean UI_ProcessEvents(ByteBuffer OutData)
 *
 * The real mUIEventManager.ProcessEvents writes the number of pending input
 * events into the front of the direct ByteBuffer, then returns whether any were
 * handled.
 *
 * That buffer is a slice of the ENGINE'S OWN STACK -- sub_9F0774's local event
 * scratch, wrapped by NewDirectByteBuffer. Leaving it untouched meant the engine
 * read a stack-garbage event count, and whenever that was >= 1 it dispatched
 * off_CCFA10[garbage index] with garbage payloads, ran off into the input
 * handlers (sub_9F0298 -> sub_9E8528) and returned straight to the halt
 * sentinel -- so onDrawFrame never got back to sub_9E8AE4 to run the update
 * loop, and LawnApp::Update never executed a single frame.
 *
 * This is also the ONLY way input gets in: there are no Native_onKey* entry
 * points, and the keyboard JNI surface (Device_ShowKeyboard and friends) never
 * returns text. So every touch, key and typed character the host collects is
 * serialised here -- see input_queue.h for the record layouts, which were read
 * out of the engine's own dispatch handlers.
 *
 * Layout: the count goes at +0, and the records start at +16. The engine wraps
 * 1024 bytes, so 1008 remain for records; input_queue::drain leaves anything
 * that does not fit queued for the next frame. */
void ui_process_events(DexCall &d) {
    std::uint32_t buf = d.arg(0);
    if (buf == 0 || !d.c.in_bounds(buf, kEventBufferSize)) {
        if (buf != 0 && d.c.in_bounds(buf, 4)) d.c.write32(buf, 0);
        d.ret_bool(false);
        return;
    }

    static std::vector<std::uint8_t> records;
    const std::size_t count = input::drain(records, kEventBufferSize - kEventRecordsOffset);

    d.c.write32(buf, (std::uint32_t)count);
    for (std::size_t i = 0; i < records.size(); ++i) {
        d.c.write8(buf + kEventRecordsOffset + (std::uint32_t)i, records[i]);
    }
    /* All of this is [log] input=1 (or verbose=1) only. It used to print the "delivered" line
     * unconditionally, which spammed a normal (input=0) run with a line per
     * frame that had any event. The per-record breakdown was already gated;
     * now the summary line is too. */
    if (count != 0 && diagnostics::input_trace()) {
        d.c.log("[input] f=%d delivered %u event(s), %u bytes", diagnostics::current_frame() + 1,
                (unsigned)count, (unsigned)records.size());
        /* Spell out every record. Needed whenever input "does nothing" -- it
         * separates "the host never sent it" from "the engine was handed it and
         * ignored it", which no amount of staring at the engine's handlers can
         * distinguish. */
        log_records(d, records);
    }
    if (count != 0 && diagnostics::verbose()) {
        bool had_down = false, had_up = false;
        scan_touches(records, had_down, had_up);
        diagnostics::note_input_delivered((unsigned)count, had_down, had_up);
    }
    d.ret_bool(count != 0);
}

/* void Device_ShowKeyboard(...) / void Device_ShowNumericKeyboard()
 * void Device_HideKeyboard() / boolean Device_IsKeyboardShowing()
 *
 * On a device these drive the soft keyboard through InputMethodManager. Here
 * they drive SDL's text input: main.c polls pvz2_input_keyboard_wanted() and
 * calls SDL_StartTextInput/SDL_StopTextInput accordingly, which is what makes
 * SDL_TEXTINPUT events start arriving.
 *
 * ShowNumericKeyboard is new in 4.5.2 and was the whole of why the age-gate
 * screen did nothing: the "AGE" field asks for it on tap, it was unhooked, so
 * SDL text input never turned on and no typing reached the box. On a device the
 * only difference from the plain keyboard is setUseNumericKeyboard(true), i.e.
 * which soft IME appears; on PC the digits come from the same physical keyboard
 * either way and the engine's numeric field filters non-digits itself, so it
 * maps to the identical "text input on".
 *
 * IsKeyboardShowing used to be left unhooked because "neither is true here" --
 * correct while there was no keyboard at all, wrong now: the engine gates the
 * name-entry flow on it, so it has to reflect the real state. */
void show_keyboard(DexCall &d) {
    pvz2_input_set_keyboard_wanted(1);
    d.c.log("[input] engine asked for the keyboard -- SDL text input on");
    d.ret(0);
}

void show_numeric_keyboard(DexCall &d) {
    pvz2_input_set_keyboard_wanted(1);
    d.c.log("[input] engine asked for the NUMERIC keyboard -- SDL text input on");
    d.ret(0);
}

void hide_keyboard(DexCall &d) {
    pvz2_input_set_keyboard_wanted(0);
    d.c.log("[input] engine dismissed the keyboard");
    d.ret(0);
}

void is_keyboard_showing(DexCall &d) { d.ret_bool(pvz2_input_keyboard_wanted() != 0); }

/* String Resources_GetAssetFileInfo(String InFilename, long[] outFileInfo)
 *
 * On a device this opens the name as an APK asset and answers with
 * (getPackageResourcePath(), afd.getStartOffset(), afd.getLength()) so native
 * code can read the asset straight out of the APK -- assets are stored
 * uncompressed there, so an offset+length into the zip is enough.
 *
 * This APK ships no assets at all, but the .obb does contain the files, stored
 * raw for everything except textures. So the identical contract is answered
 * pointing at the .obb instead: same shape, same read path on the guest side,
 * no extraction. Returns null -- exactly what Java returns for a missing asset
 * -- when the name is not in the RSB, or is a compressed texture that cannot be
 * served as a flat range. */
void get_asset_file_info(DexCall &d) {
    std::string name = d.string_arg(0);
    std::uint32_t out_array = d.arg(1);

    const RsbIndex::Entry *found = find_asset(d, name);
    if (found == nullptr || found->compressed || out_array == 0) {
        d.c.log("[asset] \"%s\" -> %s", name.c_str(),
                found == nullptr      ? "NOT IN RSB"
                : found->compressed   ? "compressed texture, cannot serve raw"
                                      : "no out array");
        d.ret(0); /* null */
        return;
    }

    /* long[] is a plain 8-bytes-per-element guest buffer (see NewLongArray),
     * little-endian like the rest of the guest. */
    d.c.write32(out_array + 0, (std::uint32_t)(found->offset & 0xFFFFFFFFu));
    d.c.write32(out_array + 4, (std::uint32_t)(found->offset >> 32));
    d.c.write32(out_array + 8, found->size);
    d.c.write32(out_array + 12, 0);
    d.c.log("[asset] \"%s\" -> %s @0x%llx size=0x%x", name.c_str(), vfs::obb_host_path(),
            (unsigned long long)found->offset, found->size);
    d.ret_string(vfs::obb_host_path());
}

/* long Resources_GetAssetFileSize(String InFilename)
 *
 * Java returns the asset's length, or -1 when absent. The engine sizes its read
 * buffer from THIS call, not from the long[] that GetAssetFileInfo fills --
 * returning 0 made it allocate nothing and read 0 bytes, which surfaced as
 * "Invalid document object in resource file" once the info call started
 * succeeding. */
void get_asset_file_size(DexCall &d) {
    std::string name = d.string_arg(0);
    const RsbIndex::Entry *found = find_asset(d, name);
    std::uint64_t size = (found != nullptr && !found->compressed) ? found->size : (std::uint64_t)-1;
    d.c.log("[asset] size(\"%s\") -> %lld", name.c_str(), (long long)(std::int64_t)size);
    d.ret64(size);
}

/* long Resources_GetFileSystemBlockCount(String path)
 * long Resources_GetFileSystemBlocksFree(String path)
 * long Resources_GetFileSystemBlockSize(String path)
 *
 * StatFs on a device, and the engine's only way to ask "is there room?".
 *
 * Unhooked these answered 0, which does not read as "unknown" -- it reads as a
 * full disk with zero-byte blocks, a state no real device is ever in. The same
 * reasoning as the statfs() shim in libc_unistd.cpp, which this deliberately
 * agrees with down to the block size: report the truth about the host disk, so
 * a space check neither blocks on a false negative nor greenlights a write that
 * then fails halfway. Java answers -1 when StatFs throws, so that is what an
 * unreadable path gets here too. */
constexpr std::uint64_t kBlockSize = 4096;

std::uint64_t disk_space(DexCall &d, int which) {
    const std::string guest_path = d.string_arg(0);
    std::error_code ec;
    const std::filesystem::space_info s =
        std::filesystem::space(vfs::translate(d.c.rt, guest_path), ec);
    if (ec) return (std::uint64_t)-1;
    switch (which) {
        case 0: return (std::uint64_t)s.capacity / kBlockSize;
        case 1: return (std::uint64_t)s.available / kBlockSize;
        default: return kBlockSize;
    }
}

void fs_block_count(DexCall &d) { d.ret64(disk_space(d, 0)); }
void fs_blocks_free(DexCall &d) { d.ret64(disk_space(d, 1)); }
void fs_block_size(DexCall &d) { d.ret64(disk_space(d, 2)); }

/* --- config storage ---
 *
 * SharedPreferences on a device: a real, persistent, typed key/value store the
 * engine both writes AND reads back. The previous port accepted every write and
 * discarded it, and answered every read and every ConfigKeyExists with "not
 * present". That is not "a valid first-launch state" -- it is an amnesiac store,
 * and it violates the one invariant the engine relies on: what you write, you
 * can read.
 *
 * The [jni] census caught the consequence: at the end of the loading screen the
 * engine polls Config_ConfigKeyExists once every frame, forever, and our store
 * says false every time. The boot writes a key and then waits for it to exist;
 * with a store that forgets, that wait never ends. So the fix is to be a real
 * store -- honour writes, answer reads and existence from what was written.
 *
 * It also survives across launches, and now so does this store: it is loaded
 * once from <save_dir>/prefs.dat (or directly next to the .exe when save_dir is
 * left at its default root path) and rewritten on every change, gated by
 * [game] persist_saves (on by default). That is what fixes the age gate --
 * without it the engine wrote the entered age, could read it back within the
 * session, but found the store empty on the next launch and asked again every
 * time. persist_saves=0 restores the old "every run is a clean first launch"
 * behaviour, which is still what you want while bringing a version up.
 *
 * Booleans and integers share one numeric map because a key is only ever read
 * back as the type it was written; strings are separate. ConfigKeyExists checks
 * both. */
struct ConfigStore {
    std::mutex lock;
    std::map<std::string, std::string> strings;
    std::map<std::string, std::int64_t> numbers;
};

std::string prefs_path() {
    return std::string(pvz2_config()->save_dir) + "/prefs.dat";
}

/* A tiny length-prefixed binary format, so a key or a string value may contain
 * any byte (spaces, '=', newlines) without an escaping scheme:
 *   'S' u32 keylen key u32 vallen val   -- a string entry
 *   'N' u32 keylen key i64 val          -- a numeric entry
 * A short read stops parsing and keeps whatever was read, so a truncated file
 * degrades to a partial store rather than a crash. */
bool read_u32(std::FILE *f, std::uint32_t &v) {
    return std::fread(&v, sizeof(v), 1, f) == 1;
}
bool read_key(std::FILE *f, std::string &key) {
    std::uint32_t n = 0;
    if (!read_u32(f, n) || n > (1u << 20)) return false;
    key.resize(n);
    return n == 0 || std::fread(&key[0], 1, n, f) == n;
}

/* Both callers hold s.lock. */
void store_load_locked(ConfigStore &s) {
    if (!pvz2_config()->persist_saves) return;
    std::FILE *f = std::fopen(prefs_path().c_str(), "rb");
    if (f == nullptr) return;
    int type;
    while ((type = std::fgetc(f)) != EOF) {
        std::string key;
        if (!read_key(f, key)) break;
        if (type == 'S') {
            std::uint32_t vlen = 0;
            if (!read_u32(f, vlen) || vlen > (1u << 24)) break;
            std::string val(vlen, '\0');
            if (vlen != 0 && std::fread(&val[0], 1, vlen, f) != vlen) break;
            s.strings[key] = std::move(val);
        } else if (type == 'N') {
            std::int64_t val = 0;
            if (std::fread(&val, sizeof(val), 1, f) != 1) break;
            s.numbers[key] = val;
        } else {
            break; /* unknown record -- stop rather than resync blindly */
        }
    }
    std::fclose(f);
}

void store_save_locked(ConfigStore &s) {
    if (!pvz2_config()->persist_saves) return;
    std::error_code ec;
    std::filesystem::create_directories(pvz2_config()->save_dir, ec);
    /* Write to a temp file and rename over the target, so an interrupted write
     * cannot leave a half-written prefs file that fails to load next launch. */
    const std::string path = prefs_path();
    const std::string tmp = path + ".tmp";
    std::FILE *f = std::fopen(tmp.c_str(), "wb");
    if (f == nullptr) return;
    for (const auto &kv : s.strings) {
        const std::uint32_t klen = (std::uint32_t)kv.first.size();
        const std::uint32_t vlen = (std::uint32_t)kv.second.size();
        std::fputc('S', f);
        std::fwrite(&klen, sizeof(klen), 1, f);
        std::fwrite(kv.first.data(), 1, klen, f);
        std::fwrite(&vlen, sizeof(vlen), 1, f);
        std::fwrite(kv.second.data(), 1, vlen, f);
    }
    for (const auto &kv : s.numbers) {
        const std::uint32_t klen = (std::uint32_t)kv.first.size();
        const std::int64_t val = kv.second;
        std::fputc('N', f);
        std::fwrite(&klen, sizeof(klen), 1, f);
        std::fwrite(kv.first.data(), 1, klen, f);
        std::fwrite(&val, sizeof(val), 1, f);
    }
    std::fclose(f);
    std::filesystem::rename(tmp, path, ec);
    if (ec) std::filesystem::remove(tmp, ec); /* rename failed; don't leave the temp behind */
}

ConfigStore &config_store() {
    static ConfigStore store;
    static std::once_flag once;
    /* Loaded exactly once, on first use -- by which point pvz2_config_load has
     * long run (it happens in main before the session starts). */
    std::call_once(once, [] {
        std::lock_guard<std::mutex> lk(store.lock);
        store_load_locked(store);
    });
    return store;
}

/* Names each key the engine touches ONCE, so the log shows the working set
 * without a per-frame poll flooding it. This is what turns "polling some key
 * every frame" into a named key. */
void note_key(DexCall &d, const char *op, const std::string &key) {
    static std::mutex seen_lock;
    static std::map<std::string, bool> seen;
    std::lock_guard<std::mutex> lk(seen_lock);
    if (seen.emplace(std::string(op) + "\0" + key, true).second) {
        d.c.log("[config] %s(\"%s\")", op, key.c_str());
    }
}

void config_key_exists(DexCall &d) {
    const std::string key = d.string_arg(0);
    note_key(d, "exists", key);
    ConfigStore &s = config_store();
    std::lock_guard<std::mutex> lk(s.lock);
    d.ret_bool(s.strings.count(key) != 0 || s.numbers.count(key) != 0);
}

void config_erase_key(DexCall &d) {
    const std::string key = d.string_arg(0);
    note_key(d, "erase", key);
    ConfigStore &s = config_store();
    std::lock_guard<std::mutex> lk(s.lock);
    s.strings.erase(key);
    s.numbers.erase(key);
    store_save_locked(s);
    d.ret(0);
}

void config_read_string(DexCall &d) {
    const std::string key = d.string_arg(0);
    note_key(d, "read.str", key);
    ConfigStore &s = config_store();
    std::lock_guard<std::mutex> lk(s.lock);
    auto it = s.strings.find(key);
    /* Absent stays null -- the Java getString(key, null) default. A key present
     * but empty returns the empty string, which is a different, real value. */
    if (it == s.strings.end()) d.ret(0);
    else d.ret_string(it->second);
}

void config_read_integer(DexCall &d) {
    const std::string key = d.string_arg(0);
    note_key(d, "read.int", key);
    ConfigStore &s = config_store();
    std::lock_guard<std::mutex> lk(s.lock);
    auto it = s.numbers.find(key);
    d.ret(it == s.numbers.end() ? 0u : (std::uint32_t)it->second);
}

void config_read_boolean(DexCall &d) {
    const std::string key = d.string_arg(0);
    note_key(d, "read.bool", key);
    ConfigStore &s = config_store();
    std::lock_guard<std::mutex> lk(s.lock);
    auto it = s.numbers.find(key);
    d.ret_bool(it != s.numbers.end() && it->second != 0);
}

void config_write_string(DexCall &d) {
    const std::string key = d.string_arg(0);
    note_key(d, "write.str", key);
    ConfigStore &s = config_store();
    std::lock_guard<std::mutex> lk(s.lock);
    s.strings[key] = d.string_arg(1);
    s.numbers.erase(key); /* a key is one type at a time, like SharedPreferences */
    store_save_locked(s);
    d.ret_bool(true);
}

void config_write_integer(DexCall &d) {
    const std::string key = d.string_arg(0);
    note_key(d, "write.int", key);
    ConfigStore &s = config_store();
    std::lock_guard<std::mutex> lk(s.lock);
    s.numbers[key] = (std::int32_t)d.arg(1);
    s.strings.erase(key);
    store_save_locked(s);
    d.ret_bool(true);
}

void config_write_boolean(DexCall &d) {
    const std::string key = d.string_arg(0);
    note_key(d, "write.bool", key);
    ConfigStore &s = config_store();
    std::lock_guard<std::mutex> lk(s.lock);
    s.numbers[key] = d.arg(1) != 0 ? 1 : 0;
    s.strings.erase(key);
    store_save_locked(s);
    d.ret_bool(true);
}

/* --- device / orientation --- */

/* int Device_GetCurrentUIOrientation() -- 4 is landscape-right, which matches
 * the host window's aspect. */
void get_ui_orientation(DexCall &d) { d.ret(4); }
void is_supported_orientation(DexCall &d) { d.ret_bool(true); }

/* boolean Web_SysOpenURL(String) -- on Android this launches an external
 * VIEW intent; here we open the host's default browser. */
void open_url(DexCall &d) {
    const std::string url = d.string_arg(0);
    if (url.empty()) {
        d.c.log("[dex] Web_SysOpenURL: empty URL");
        d.ret_bool(false);
        return;
    }

    if (SDL_OpenURL(url.c_str()) == 0) {
        d.c.log("[dex] Web_SysOpenURL opened: %s", url.c_str());
        d.ret_bool(true);
    } else {
        d.c.log("[dex] Web_SysOpenURL failed (%s): %s", SDL_GetError(), url.c_str());
        d.ret_bool(false);
    }
}

/* --- identity strings ---
 *
 * These end up in save files, analytics payloads and the version display. */
void product_version(DexCall &d) { d.ret_string(pvz2_config()->product_version_string); }

/* Everything below is a [game] key in config.ini rather than a literal here --
 * see config.h's package_name/activity_name/device_name/hardware_model/
 * os_version/currency_symbol/currency_code/product_version_string. package_name
 * in particular is read by dex/hooks/purchase_driver.cpp too, so the two can
 * never disagree the way this hook and that file's own copy used to. */
void product_version_string(DexCall &d) { d.ret_string(pvz2_config()->product_version_string); }
void currency_symbol(DexCall &d) { d.ret_string(pvz2_config()->currency_symbol); }
/* Info_SysGetUserCurrencyCode -- the ISO 4217 code (e.g. "USD"). Unhooked it
 * returned null, and the in-app store treats a null currency as "store not
 * configured", which surfaces as the "Service Unavailable" purchase dialog. */
void currency_code(DexCall &d) { d.ret_string(pvz2_config()->currency_code); }
void package_name(DexCall &d) { d.ret_string(pvz2_config()->package_name); } /* matches the real .obb name */
void activity_name(DexCall &d) { d.ret_string(pvz2_config()->activity_name); }
/* Both come from [game] user_locale in config.ini (default "en_US"). The country
 * is the part after '_', upper-cased, so the two can never disagree -- a locale
 * of es_AR yields country AR without a second setting to keep in sync. */
void user_locale(DexCall &d) { d.ret_string(pvz2_config()->user_locale); }
void country_code(DexCall &d) {
    const std::string locale = pvz2_config()->user_locale;
    const std::size_t us = locale.find('_');
    std::string country = (us == std::string::npos) ? "US" : locale.substr(us + 1);
    for (char &c : country) c = (char)std::toupper((unsigned char)c);
    d.ret_string(country.empty() ? "US" : country);
}
void device_name(DexCall &d) { d.ret_string(pvz2_config()->device_name); }
void device_id(DexCall &d) { d.ret_string("UNKNOWN/EMULATOR"); } /* AndroidGameApp.java's own fallback */

/* String Util_GetUUIDString()
 *
 * Java answers with UUID.randomUUID().toString(). Unhooked it returned null,
 * and the engine builds std::strings out of these without checking. We persist
 * one generated value in config.ini so it stays stable for this installation
 * but differs across fresh configs / machines. */
void uuid_string(DexCall &d) { d.ret_string(pvz2_config()->player_uuid); }
void os_version(DexCall &d) { d.ret_string(pvz2_config()->os_version); }
void hardware_model(DexCall &d) { d.ret_string(pvz2_config()->hardware_model); }

/* --- storage locations ---
 *
 * Relative-path semantics matter more than the exact string: the engine
 * concatenates these with resource names and hands the result to fopen, which
 * vfs::translate() then resolves. */
void data_folder(DexCall &d) { d.ret_string("."); }

}  // namespace

void config_store_set_string(const std::string &key, const std::string &value) {
    ConfigStore &s = config_store();
    std::lock_guard<std::mutex> lk(s.lock);
    s.strings[key] = value;
    s.numbers.erase(key);
    store_save_locked(s);
}

bool config_store_get_string(const std::string &key, std::string &out) {
    ConfigStore &s = config_store();
    std::lock_guard<std::mutex> lk(s.lock);
    auto it = s.strings.find(key);
    if (it == s.strings.end()) return false;
    out = it->second;
    return true;
}

bool config_store_get_int64(const std::string &key, std::int64_t &out) {
    ConfigStore &s = config_store();
    std::lock_guard<std::mutex> lk(s.lock);
    auto it = s.numbers.find(key);
    if (it == s.numbers.end()) return false;
    out = it->second;
    return true;
}

void config_store_reload(void) {
    ConfigStore &s = config_store();
    std::lock_guard<std::mutex> lk(s.lock);
    s.strings.clear();
    s.numbers.clear();
    store_load_locked(s);
}

void register_android_game_app(HookTable &t) {
    t.add(kClass, "UI_ProcessEvents", ui_process_events);

    t.add(kClass, "Device_ShowKeyboard", show_keyboard);
    t.add(kClass, "Device_ShowNumericKeyboard", show_numeric_keyboard);
    t.add(kClass, "Device_HideKeyboard", hide_keyboard);
    t.add(kClass, "Device_IsKeyboardShowing", is_keyboard_showing);

    t.add(kClass, "Resources_GetAssetFileInfo", get_asset_file_info);
    t.add(kClass, "Resources_GetAssetFileSize", get_asset_file_size);

    t.add(kClass, "Resources_GetFileSystemBlockCount", fs_block_count);
    t.add(kClass, "Resources_GetFileSystemBlocksFree", fs_blocks_free);
    t.add(kClass, "Resources_GetFileSystemBlockSize", fs_block_size);

    /* A real, session-persistent key/value store -- see the ConfigStore note.
     * These have to agree with each other (a write must be visible to the next
     * read and to KeyExists), which is the whole point and why they moved from
     * an accept-and-forget stub to actual storage. */
    t.add(kClass, "Config_ConfigKeyExists", config_key_exists);
    t.add(kClass, "Config_ConfigEraseKey", config_erase_key);
    t.add(kClass, "Config_ConfigReadString", config_read_string);
    t.add(kClass, "Config_ConfigReadInteger", config_read_integer);
    t.add(kClass, "Config_ConfigReadBoolean", config_read_boolean);
    t.add(kClass, "Config_ConfigWriteString", config_write_string);
    t.add(kClass, "Config_ConfigWriteInteger", config_write_integer);
    t.add(kClass, "Config_ConfigWriteBoolean", config_write_boolean);

    t.add(kClass, "Web_SysOpenURL", open_url);
    t.add(kClass, "Device_IsSupportedUIOrientation", is_supported_orientation);
    t.add(kClass, "Device_GetCurrentUIOrientation", get_ui_orientation);

    t.add(kClass, "Info_SysGetProductVersion", product_version);
    t.add(kClass, "Info_SysGetProductVersionString", product_version_string);
    t.add(kClass, "Info_SysGetUserCurrencySymbol", currency_symbol);
    t.add(kClass, "Info_SysGetUserCurrencyCode", currency_code);
    t.add(kClass, "Info_SysGetPackageName", package_name);
    t.add(kClass, "Info_SysGetActivityName", activity_name);
    t.add(kClass, "Info_SysGetUserLocale", user_locale);
    t.add(kClass, "Info_SysGetCountryCodeString", country_code);

    t.add(kClass, "Device_GetDeviceName", device_name);
    t.add(kClass, "Diag_GetDeviceID", device_id);
    t.add(kClass, "Util_GetUUIDString", uuid_string);
    t.add(kClass, "Diag_GetOSVersion", os_version);
    t.add(kClass, "Diag_GetHardwareModel", hardware_model);

    t.add(kClass, "Device_GetCachesDir", data_folder);
    t.add(kClass, "Resources_GetResourceFolder", data_folder);
    t.add(kClass, "Resources_GetUserDataFolder", data_folder);
    t.add(kClass, "Resources_GetCacheDataFolder", data_folder);
    t.add(kClass, "Resources_GetAppSupportDataFolder", data_folder);
    t.add(kClass, "Resources_GetExternalStorageDirectory", data_folder);

    /* Deliberately NOT hooked, because the unhooked default (0/false/null) is
     * already the correct answer for this port:
     *   Device_IsTablet -- not true here.
     *   Info_SysGetTimeBombDate -- this build has no expiry.
     *   Info_SysGetIntentExtraDataString -- the app was not launched from an
     *     intent with extras. */
}

}  // namespace dex
}  // namespace pvz2native
