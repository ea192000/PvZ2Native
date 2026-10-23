/* [log] verbose=1 quest trace, host side -- see diagnostics/quest_trace.h. */

#include <pvz2native/diagnostics/quest_trace.h>

#include <pvz2native/diagnostics/diagnostics.h>
#include <pvz2native/diagnostics/verbose.h>
#include <pvz2native/hash.h>
#include <pvz2native/host_files.h>
#include <pvz2native/rton_tree.h>
#include <pvz2native/log.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace pvz2native {
namespace diagnostics {
namespace {

namespace fs = std::filesystem;

constexpr const char *kPpPath = "userdata/pp.dat";
constexpr const char *kActiveDir = "userdata/activequests";
constexpr int kPollEveryFrames = 10;

using Node = RtonNode;

/* ------------------------------------------------------------------ names */

/* hash -> quest/group id, learned from every id the saves show (cqi only stores the hash). */
std::map<std::uint32_t, std::string> g_names;

void learn(const std::string &id) {
    if (!id.empty()) g_names.emplace(fnv1(id), id);
}

std::string name_of(std::uint32_t h) {
    auto it = g_names.find(h);
    if (it != g_names.end()) return it->second;
    char b[24];
    std::snprintf(b, sizeof(b), "#%u", h);
    return b;
}

std::string when(std::int64_t epoch) {
    const std::time_t t = (std::time_t)epoch;
    std::tm tmv{};
#if defined(_WIN32)
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    char b[40];
    std::strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &tmv);
    const long long age = (long long)std::time(nullptr) - (long long)epoch;
    char out[96];
    std::snprintf(out, sizeof(out), "%s UTC (%s%lldh%02lldm)", b, age < 0 ? "in " : "", std::llabs(age) / 3600,
                  (std::llabs(age) % 3600) / 60);
    return out;
}

/* --------------------------------------------------------------- snapshots */

struct Pp {
    bool ok = false;
    std::int64_t tqx = -1, qr = -1;
    std::map<std::uint32_t, int> cqi;                 /* hash -> s */
    std::vector<std::pair<std::uint32_t, std::int64_t>> qlgi; /* group hash -> l */
};

struct ActiveQuest {
    std::string cls;
    std::int64_t status = -1;
    std::string issued;
    std::vector<std::int64_t> left;
};

struct Active {
    bool ok = false;
    std::vector<std::pair<std::string, ActiveQuest>> q; /* in file order */
};

Pp parse_pp(const fs::path &p) {
    Pp r;
    const std::vector<std::uint8_t> d = host_files::read(p.string());
    if (d.empty()) return r;
    Node root;
    if (!rton_parse(d, root)) return r;
    r.ok = true;
    if (const Node *n = root.find("tqx")) r.tqx = n->i;
    if (const Node *n = root.find("qr")) r.qr = n->i;
    if (const Node *c = root.find("cqi")) {
        for (const Node &e : c->a) {
            const Node *i = e.get("i"), *s = e.get("s");
            if (i) r.cqi[(std::uint32_t)i->i] = s ? (int)s->i : -1;
        }
    }
    if (const Node *c = root.find("qlgi")) {
        for (const Node &e : c->a) {
            const Node *i = e.get("i"), *l = e.get("l");
            if (i && l) r.qlgi.emplace_back((std::uint32_t)i->i, l->i);
        }
    }
    return r;
}

Active parse_active(const fs::path &p) {
    Active r;
    const std::vector<std::uint8_t> d = host_files::read(p.string());
    if (d.empty()) return r;
    Node root;
    if (!rton_parse(d, root)) return r;
    r.ok = true;
    const Node *objs = root.get("objects");
    if (!objs) return r;
    for (const Node &o : objs->a) {
        const Node *cls = o.get("objclass"), *data = o.get("objdata");
        if (!data) continue;
        const Node *id = data->get("QuestID");
        if (!id) continue;
        ActiveQuest q;
        if (cls) q.cls = cls->s;
        if (const Node *s = data->get("Status")) q.status = s->i;
        if (const Node *s = data->get("QuestIssueDate")) q.issued = s->s;
        if (const Node *k = data->get("m_killCountRemaining"))
            for (const Node &e : k->a)
                if (const Node *z = e.get("ZombieCount")) q.left.push_back(z->i);
        learn(id->s);
        r.q.emplace_back(id->s, std::move(q));
    }
    return r;
}

std::string left_str(const std::vector<std::int64_t> &v) {
    std::string s;
    for (std::int64_t x : v) {
        if (!s.empty()) s += ",";
        s += std::to_string((long long)x);
    }
    return s.empty() ? "-" : s;
}

/* ------------------------------------------------------------------ watcher */

struct FileWatch {
    fs::path path;
    std::int64_t mtime = -1;
    std::int64_t size = -1;
    std::int64_t pending_mtime = -1;
    std::int64_t pending_size = -1;
    bool seen = false;
};

FileWatch g_pp, g_act;
Pp g_prev_pp;
Active g_prev_active;
bool g_have_pp = false, g_have_act = false;
int g_last_poll = -1000;
bool g_names_seeded = false;

void seed_names() {
    if (g_names_seeded) return;
    g_names_seeded = true;
    for (const char *n : {"GenericPickingGroup", "PremiumPickingGroup", "ElderPickingGroup", "EpicPickingGroup"})
        learn(n);
}

/* True once the file's (mtime,size) have been the same on two polls in a row and differ from
 * the last parsed version: the engine writes through a temp file and renames, but a plain
 * rewrite in place must not be parsed half-written. */
bool settled_change(FileWatch &w) {
    std::error_code ec;
    if (!fs::is_regular_file(w.path, ec)) return false;
    const std::int64_t sz = (std::int64_t)fs::file_size(w.path, ec);
    const auto ft = fs::last_write_time(w.path, ec);
    const std::int64_t mt = (std::int64_t)ft.time_since_epoch().count();
    if (ec) return false;
    if (mt == w.mtime && sz == w.size) return false;
    if (mt != w.pending_mtime || sz != w.pending_size) {
        w.pending_mtime = mt;
        w.pending_size = sz;
        return false;
    }
    w.mtime = mt;
    w.size = sz;
    return true;
}

void log_pp(const Pp &now, const Pp *prev) {
    if (!prev) {
        vlog("quest-save", "pp.dat at startup: tqx=%lld qr=%lld cqi=%zu entries", (long long)now.tqx,
             (long long)now.qr, now.cqi.size());
        for (const auto &kv : now.qlgi)
            vlog("quest-save", "pp.dat qlgi  %-22s last pick %s", name_of(kv.first).c_str(), when(kv.second).c_str());
        return;
    }
    std::string d;
    if (now.tqx != prev->tqx) d += " tqx " + std::to_string((long long)prev->tqx) + "->" + std::to_string((long long)now.tqx);
    if (now.qr != prev->qr) d += " qr " + std::to_string((long long)prev->qr) + "->" + std::to_string((long long)now.qr);
    for (const auto &kv : now.cqi)
        if (!prev->cqi.count(kv.first))
            d += " CQI+" + name_of(kv.first) + "(s=" + std::to_string(kv.second) + ")";
    for (const auto &kv : prev->cqi)
        if (!now.cqi.count(kv.first)) d += " CQI-" + name_of(kv.first);
    for (const auto &kv : now.cqi) {
        auto it = prev->cqi.find(kv.first);
        if (it != prev->cqi.end() && it->second != kv.second)
            d += " CQI~" + name_of(kv.first) + " s " + std::to_string(it->second) + "->" + std::to_string(kv.second);
    }
    for (const auto &kv : now.qlgi) {
        std::int64_t before = -1;
        for (const auto &pk : prev->qlgi)
            if (pk.first == kv.first) before = pk.second;
        if (before != kv.second)
            d += " qlgi[" + name_of(kv.first) + "] l=" + when(kv.second);
    }
    vlog("quest-save", "pp.dat written (cqi=%zu):%s", now.cqi.size(), d.empty() ? " no tracked change" : d.c_str());
}

void log_active(const Active &now, const Active *prev) {
    if (!prev) {
        vlog("quest-save", "ActiveQuests at startup: %zu quest(s)", now.q.size());
        for (const auto &kv : now.q)
            vlog("quest-save", "  %-42s %-28s status=%lld issued=%s left=%s", kv.first.c_str(), kv.second.cls.c_str(),
                 (long long)kv.second.status, kv.second.issued.c_str(), left_str(kv.second.left).c_str());
        return;
    }
    std::string d;
    std::map<std::string, const ActiveQuest *> before;
    for (const auto &kv : prev->q) before[kv.first] = &kv.second;
    std::set<std::string> kept;
    for (const auto &kv : now.q) {
        kept.insert(kv.first);
        auto it = before.find(kv.first);
        if (it == before.end()) {
            d += " ACTIVE+" + kv.first + "(status=" + std::to_string((long long)kv.second.status) +
                 ",left=" + left_str(kv.second.left) + ",issued=" + kv.second.issued + ")";
            continue;
        }
        if (it->second->status != kv.second.status)
            d += " STATUS " + kv.first + " " + std::to_string((long long)it->second->status) + "->" +
                 std::to_string((long long)kv.second.status);
        if (it->second->left != kv.second.left)
            d += " LEFT " + kv.first + " " + left_str(it->second->left) + "->" + left_str(kv.second.left);
    }
    for (const auto &kv : prev->q)
        if (!kept.count(kv.first)) d += " ACTIVE-" + kv.first + "(was status=" + std::to_string((long long)kv.second.status) +
                                       ",left=" + left_str(kv.second.left) + ")";
    vlog("quest-save", "ActiveQuests written (%zu):%s", now.q.size(), d.empty() ? " no tracked change" : d.c_str());
}

/* Each profile keeps its own active quests in activequests/<profile>_ActiveQuests. The engine rewrites the file
 * of the profile in play, so the one touched last is the one to follow; switching profiles resets what the
 * trace remembers, so its diffs never compare two profiles. */
void find_active_path() {
    std::error_code ec;
    if (!fs::is_directory(kActiveDir, ec)) return;
    fs::path newest;
    fs::file_time_type newest_time{};
    for (const auto &e : fs::directory_iterator(kActiveDir, ec)) {
        const fs::path p = e.path() / "save_QuestsActive.rton";
        const auto t = fs::last_write_time(p, ec);
        if (ec) {
            ec.clear();
            continue;
        }
        if (newest.empty() || t > newest_time) {
            newest = p;
            newest_time = t;
        }
    }
    if (newest.empty() || newest == g_act.path) return;
    if (!g_act.path.empty())
        vlog("quest-save", "active quests now follow %s", newest.parent_path().filename().string().c_str());
    g_act = FileWatch{};
    g_act.path = newest;
    g_prev_active = Active{};
    g_have_act = false;
}

}  // namespace

void quest_trace_after_frame(int frame) {
    if (!verbose()) return;
    if (frame - g_last_poll < kPollEveryFrames) return;
    g_last_poll = frame;
    seed_names();
    if (g_pp.path.empty()) g_pp.path = kPpPath;
    find_active_path();

    /* The active quests first: that is what teaches the hash -> name table cqi needs. */
    if (!g_act.path.empty() && settled_change(g_act)) {
        Active now = parse_active(g_act.path);
        if (!now.ok) {
            vlog("quest-save", "%s: not understood as RTON", g_act.path.string().c_str());
        } else {
            log_active(now, g_have_act ? &g_prev_active : nullptr);
            g_prev_active = std::move(now);
            g_have_act = true;
        }
    }
    if (settled_change(g_pp)) {
        Pp now = parse_pp(g_pp.path);
        if (!now.ok) {
            vlog("quest-save", "%s: not understood as RTON", g_pp.path.string().c_str());
        } else {
            log_pp(now, g_have_pp ? &g_prev_pp : nullptr);
            g_prev_pp = std::move(now);
            g_have_pp = true;
        }
    }
}

void quest_trace_note_open(GuestCall &c, const std::string &path, const std::string &mode) {
    if (!verbose()) return;
    if (mode.find('w') == std::string::npos && mode.find('a') == std::string::npos) return;
    if (path.find("ActiveQuests") == std::string::npos && path.find("pp.dat") == std::string::npos) return;
    vlog("quest-io", "guest opens %s for writing, lr=.so+0x%x, callers:%s", path.c_str(),
         c.img != nullptr ? c.lr() - c.img->so_base : c.lr(), guess_callers(c, 14).c_str());
}

}  // namespace diagnostics
}  // namespace pvz2native
