/* See include/pvz2native/quest_fix.h. */

#include <pvz2native/quest_fix.h>

#include <pvz2native/config.h>
#include <pvz2native/dex/dex.h>
#include <pvz2native/hash.h>
#include <pvz2native/log.h>
#include <pvz2native/rton_tree.h>
#include <pvz2native/save_dates.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

namespace pvz2native {
namespace {

/* Hashes (ascending) of every quest with `Repeatable` true in QuestMgr.Quests. */
std::vector<std::uint32_t> repeatable_quest_hashes(const std::vector<std::uint8_t> &quests_rton) {
    std::vector<std::uint32_t> out;
    RtonNode root;
    if (!rton_parse(quests_rton, root)) return out;
    const RtonNode *objects = root.get("objects");
    if (objects == nullptr) return out;
    for (const RtonNode &obj : objects->a) {
        const RtonNode *cls = obj.get("objclass");
        const RtonNode *data = obj.get("objdata");
        if (cls == nullptr || data == nullptr || cls->s != "QuestMgr") continue;
        const RtonNode *quests = data->get("Quests");
        if (quests == nullptr) continue;
        for (const RtonNode &entry : quests->a) {
            /* Each element is {<QuestDataClass>: {UniqueID, Repeatable, ...}}. */
            for (const auto &kv : entry.o) {
                const RtonNode *id = kv.second.get("UniqueID");
                const RtonNode *rep = kv.second.get("Repeatable");
                if (id != nullptr && rep != nullptr && rep->kind == RtonNode::Str && rep->s == "true")
                    out.push_back(fnv1(id->s)); /* cqi stores the hash of the quest id */
            }
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

}  // namespace

int fix_completed_repeatables(GuestRuntime *rt) {
    if (!pvz2_config()->allow_repeat_quests) return 0;
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<std::uint8_t> body = dex::read_cdn_cached_package(rt, "PACKAGES\\QUESTS.RTON", "quests.json");
    if (body.empty()) {
        pvz2_log("[save] quest fix skipped: QUESTS.RTON could not be read from the OBB");
        return 0;
    }
    const std::vector<std::uint32_t> repeatable = repeatable_quest_hashes(body);
    if (repeatable.empty()) {
        pvz2_log("[save] quest fix skipped: no repeatable quest found in QUESTS.RTON");
        return 0;
    }
    const int removed = pvz2_fix_repeat_quests(repeatable);
    pvz2_log("[save] quest fix: %zu repeatable quest(s) known from the OBB, %d entr%s removed (%.0f ms)",
             repeatable.size(), removed, removed == 1 ? "y" : "ies",
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    return removed;
}

}  // namespace pvz2native
