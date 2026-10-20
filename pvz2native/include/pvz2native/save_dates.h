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
 * The files are edited in place through a small RTON walker; a file that is not fully understood
 * is left untouched, and the new bytes are verified to parse before the file is replaced.
 * Does nothing with [runtime] no_fix_save_dates=1, or while [runtime] force_date is set (the
 * clock is then intentionally not the real one).
 *
 * Returns the number of timestamps changed. Call after the config is loaded, before the session. */
int pvz2_fix_future_save_dates(void);

#ifdef __cplusplus
}
#endif

#endif
