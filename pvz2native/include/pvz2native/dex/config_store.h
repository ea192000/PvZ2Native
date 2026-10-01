#ifndef PVZ2NATIVE_DEX_CONFIG_STORE_H
#define PVZ2NATIVE_DEX_CONFIG_STORE_H

#include <string>
#include <cstdint>

namespace pvz2native {
namespace dex {

/* Read/write one string key in the same on-disk store the engine's
 * SharedPreferences emulation uses (<save_dir>/prefs.dat, or next to the .exe
 * when [paths] save is unset) -- for host-side state that should live next to
 * the game's own saved data instead of a new file. Gated by [game]
 * persist_saves, same as every other key already in that store. */
void config_store_set_string(const std::string &key, const std::string &value);
bool config_store_get_string(const std::string &key, std::string &out);

/* Read a numeric prefs key (MusicVolume / SfxVolume / Muted, etc.). */
bool config_store_get_int64(const std::string &key, std::int64_t &out);

/* Re-read prefs.dat from disk into the in-memory store. Use before sampling
 * volume/mute for host UI sounds so we pick up in-game changes. */
void config_store_reload(void);

}  // namespace dex
}  // namespace pvz2native

#endif
