/* Repairs to the player's save made before the guest loads it -- see include/pvz2native/save_dates.h.
 *
 * Both repairs are the same operation on the same files: parse the RTON, plan a few byte-range edits, apply
 * them, prove the result still parses and has nothing left to fix, and only then replace the file. That is
 * edit_save_file(); each repair is only the plan. */

#include <pvz2native/save_dates.h>

#include <pvz2native/config.h>
#include <pvz2native/host_files.h>
#include <pvz2native/log.h>
#include <pvz2native/rton_tree.h>

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace {

using pvz2native::RtonEdit;
using pvz2native::RtonNode;

constexpr long long kFutureLimitSeconds = 30LL * 86400; /* anything further ahead than this is bogus */
constexpr long long kResetBackSeconds = 86400;          /* reset to "one day ago", past the 12 h pick wait */
constexpr std::int64_t kQuestClaimed = 3;               /* `s` of a cqi entry: 2 = completed, 3 = claimed */

/* The engine's own folder name for save data, as vfs.cpp maps it, relative to the working dir. */
const char *const kSaveFiles[] = {"userdata/pp.dat", "userdata/snapshot1.dat", "userdata/snapshot2.dat"};

/* A plan reads the parsed file, appends the edits it wants and returns how many changes they make. A file
 * is rewritten only when it is non-zero, and only if `verify` accepts the re-parsed result. */
using Plan = std::function<int(const RtonNode &root, std::vector<RtonEdit> &edits)>;
using Verify = std::function<bool(const RtonNode &root)>;

/* Returns the number of changes made to one file; 0 when it is missing, needs nothing or was left alone. */
int edit_save_file(const std::string &path, const Plan &plan, const Verify &verify) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return 0;
    const std::vector<std::uint8_t> data = pvz2native::host_files::read(path);

    RtonNode root;
    if (!pvz2native::rton_parse(data, root)) {
        pvz2_log("[save] %s: not understood as RTON -- left untouched", path.c_str());
        return 0;
    }
    std::vector<RtonEdit> edits;
    const int changes = plan(root, edits);
    if (changes == 0) return 0;

    const std::vector<std::uint8_t> out = pvz2native::rton_apply_edits(data, std::move(edits));
    RtonNode check;
    if (!pvz2native::rton_parse(out, check) || !verify(check)) {
        pvz2_log("[save] %s: rewrite did not verify -- left untouched", path.c_str());
        return 0;
    }
    if (!pvz2native::host_files::write_atomic(path, out)) {
        pvz2_log("[save] %s: could not replace the file", path.c_str());
        return 0;
    }
    return changes;
}

/* qlgi: the last pick time `l` of each quest group. */
int plan_future_dates(const RtonNode &root, long long now, std::vector<RtonEdit> &edits) {
    const RtonNode *groups = root.find("qlgi");
    if (groups == nullptr) return 0;
    const std::uint64_t limit = (std::uint64_t)(now + kFutureLimitSeconds);
    const std::uint64_t replacement = (std::uint64_t)(now - kResetBackSeconds);
    int changes = 0;
    for (const RtonNode &group : groups->a) {
        const RtonNode *l = group.get("l");
        if (l == nullptr || l->kind != RtonNode::Int || (std::uint64_t)l->i <= limit) continue;
        edits.push_back({l->begin, l->end, pvz2native::rton_encode_int(l->tag, replacement)});
        ++changes;
    }
    return changes;
}

/* cqi: the quests the player has completed, one {i: id hash, s: state} per quest. A repeatable that was
 * claimed goes; the element count is rewritten to match. */
int plan_claimed_repeatables(const RtonNode &root, const std::vector<std::uint32_t> &repeatable_sorted,
                             std::vector<RtonEdit> &edits, std::size_t &kept) {
    const RtonNode *cqi = root.find("cqi");
    if (cqi == nullptr || cqi->kind != RtonNode::Arr) return 0;
    int removed = 0;
    kept = 0;
    for (const RtonNode &entry : cqi->a) {
        const RtonNode *id = entry.get("i");
        const RtonNode *state = entry.get("s");
        const bool claimed_repeatable =
            id != nullptr && state != nullptr && state->i == kQuestClaimed &&
            std::binary_search(repeatable_sorted.begin(), repeatable_sorted.end(), (std::uint32_t)id->i);
        if (claimed_repeatable) {
            edits.push_back({entry.begin, entry.end, {}});
            ++removed;
        } else {
            ++kept;
        }
    }
    if (removed > 0) edits.push_back({cqi->count_begin, cqi->count_end, pvz2native::rton_encode_varint(kept)});
    return removed;
}

}  // namespace

extern "C" int pvz2_fix_future_save_dates(void) {
    const pvz2_config_t *cfg = pvz2_config();
    if (cfg->no_fix_save_dates) return 0;
    if (cfg->force_date[0] != '\0') return 0;

    const long long now = (long long)std::time(nullptr);
    const Plan plan = [now](const RtonNode &root, std::vector<RtonEdit> &edits) {
        return plan_future_dates(root, now, edits);
    };
    const Verify verify = [now](const RtonNode &root) {
        std::vector<RtonEdit> none;
        return plan_future_dates(root, now, none) == 0;
    };
    int total = 0;
    for (const char *name : kSaveFiles) {
        const int n = edit_save_file(name, plan, verify);
        if (n > 0) {
            pvz2_log("[save] %s: %d quest-pick time(s) were more than 30 days in the future -- reset to yesterday",
                     name, n);
            total += n;
        }
    }
    return total;
}

int pvz2_fix_repeat_quests(const std::vector<std::uint32_t> &repeatable_sorted) {
    if (repeatable_sorted.empty()) return 0;
    int total = 0;
    for (const char *name : kSaveFiles) {
        std::size_t kept = 0;
        const Plan plan = [&](const RtonNode &root, std::vector<RtonEdit> &edits) {
            return plan_claimed_repeatables(root, repeatable_sorted, edits, kept);
        };
        const Verify verify = [&](const RtonNode &root) {
            std::vector<RtonEdit> none;
            std::size_t left = 0;
            const RtonNode *cqi = root.find("cqi");
            return plan_claimed_repeatables(root, repeatable_sorted, none, left) == 0 && cqi != nullptr &&
                   cqi->a.size() == kept;
        };
        const int n = edit_save_file(name, plan, verify);
        if (n > 0) {
            pvz2_log("[save] %s: %d claimed repeatable quest(s) taken off the completed list (the engine would "
                     "complete them on its own, without kills, whenever they are picked again)",
                     name, n);
            total += n;
        }
    }
    return total;
}
