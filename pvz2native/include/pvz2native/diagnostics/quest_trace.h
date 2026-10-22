#ifndef PVZ2NATIVE_DIAGNOSTICS_QUEST_TRACE_H
#define PVZ2NATIVE_DIAGNOSTICS_QUEST_TRACE_H

#include <pvz2native/dependencies/dependency.h>

#include <string>

/* [log] verbose=1 -- the quest trace (host side).
 *
 * Quests are the engine's least observable system: what is picked, what completes and why
 * only shows up as a changed save file. This module makes those changes visible. Everything
 * is a no-op when verbose is 0.
 *
 * Line tags (the guest-side ones are in game/patches.cpp, game_install_quest_trace):
 *   [quest-save] a snapshot of userdata/pp.dat (qlgi/cqi/tqx/qr) and of the active quests
 *                (userdata/activequests/<profile>_ActiveQuests/save_QuestsActive.rton), logged
 *                once at startup and again, as a diff, whenever either file changes
 *   [quest-io]   the guest opens one of those files for writing, with the engine call chain
 *                that led there (.so offsets, same heuristic as [engine-log] callers)
 *
 * Reading the diff: `ACTIVE +ID` / `-ID` are quests entering/leaving the active list,
 * `STATUS ID a->b` is the engine's own state (1 = in progress, 2 = completed, not yet claimed),
 * `LEFT ID a->b` the kills still needed, `CQI +name` a quest entering the completed list (cqi),
 * and the `qlgi` line shows when each picking group last picked. */

namespace pvz2native {
namespace diagnostics {

/* Polls the two save files; call once per frame (it rate-limits itself). */
void quest_trace_after_frame(int frame);

/* The guest just opened `path` with `mode` (fopen): if it is a quest save opened for writing,
 * logs who asked. */
void quest_trace_note_open(GuestCall &c, const std::string &path, const std::string &mode);

}  // namespace diagnostics
}  // namespace pvz2native

#endif
