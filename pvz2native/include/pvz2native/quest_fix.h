#ifndef PVZ2NATIVE_QUEST_FIX_H
#define PVZ2NATIVE_QUEST_FIX_H

#include <pvz2native/runtime/guest_runtime.h>

namespace pvz2native {

/* Keeps already-claimed repeatable quests from completing by themselves (see pvz2_fix_repeat_quests in
 * save_dates.h for the why). The set of repeatable quests is read from QUESTS.RTON inside the OBB -- the file
 * the engine loads -- so nothing about the quests is hardcoded and other game versions work as long as their
 * quests carry the same `Repeatable` flag. If the file cannot be read or lists no repeatable quest, the save is
 * left alone and a [save] line says why.
 *
 * Does nothing with [runtime] allow_repeat_quests=0. Call once, after the OBB path is known and before the
 * guest loads its save (pp.dat is read late in the boot). Returns the number of entries removed. */
int fix_completed_repeatables(GuestRuntime *rt);

}  // namespace pvz2native

#endif
