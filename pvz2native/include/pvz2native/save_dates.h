#ifndef PVZ2NATIVE_SAVE_DATES_H
#define PVZ2NATIVE_SAVE_DATES_H

#ifdef __cplusplus
extern "C" {
#endif

/* Repairs timestamps in the player's save that lie in the future.
 *
 * Older builds ran the engine with a skewed clock (it reported ~2030), so quest-pick times were
 * saved as 2030-10-10. The engine then refuses to pick new daily/epic quests until that date,
 * and players cannot fix it by hand. Before the guest loads anything, every `l` of the `qlgi`
 * list (last pick per quest group) in userdata/pp.dat and userdata/snapshot*.dat that is more
 * than 30 days ahead of now is reset to one day ago (past the engine's own 12 h pick wait).
 *
 * The files are edited in place through the RTON tree (rton_tree.h); a file that is not fully understood
 * is left untouched, and the new bytes are verified to parse before the file is replaced.
 * Does nothing with [runtime] no_fix_save_dates=1, or while [runtime] force_date is set (the
 * clock is then intentionally not the real one).
 *
 * Returns the number of timestamps changed. Call after the config is loaded, before the session. */
int pvz2_fix_future_save_dates(void);

#ifdef __cplusplus
}

#include <cstdint>
#include <vector>

/* Takes the claimed repeatable quests off the player's list of completed quests (`cqi` in userdata/pp.dat and
 * snapshot*.dat).
 *
 * The engine picks a slot's quests among ALL its repeatable ones, completed or not, and a quest whose id
 * is on that list completes by itself on its first progress event, with the kill counter untouched and
 * the reward paid. A repeatable that was ever completed therefore "completes" every time it is picked
 * again. Without its entry it behaves like a fresh quest. Completing one adds the entry back, so this
 * runs on every launch. Only entries with s = 3 (claimed) go: one with s = 2 is a quest completed but still
 * waiting for its reward, and stays. Quests in progress and non-repeatable quests are never touched.
 *
 * Which quests are repeatable is read from the OBB's own quest definitions (see quest_fix.h), so it follows
 * the data the game runs with. `repeatable_sorted` holds the FNV-1 hashes of their ids, ascending; with none
 * the save is left alone. Same safeguards as above: a missing file is skipped, one that is not fully
 * understood is left alone, and the result is verified before it replaces the original.
 *
 * Returns the number of entries removed. Call before the guest loads its save. */
int pvz2_fix_repeat_quests(const std::vector<std::uint32_t> &repeatable_sorted);
#endif

#endif
