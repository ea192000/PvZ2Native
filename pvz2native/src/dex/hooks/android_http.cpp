/* com.popcap.SexyAppFramework.AndroidHttpTransaction
 *
 * The engine's HTTP client, and the reason 4.5.2 never finishes loading.
 *
 * On a device this class is asynchronous: the engine builds a transaction with a
 * pointer to its C++ side (the `nativeTransaction` long), calls Start(), and a
 * Java thread connects, reads the response, and fires one of the engine's native
 * callbacks -- HttpReceivedResponse / HttpReceivedData / HttpTransactionComplete,
 * or, on any IOException, HttpTransactionError. THAT callback is how the engine
 * learns the request is over. With no network the connect() throws after the
 * 30-second timeout and the error path runs, so the engine proceeds offline.
 *
 * This port has no network and its Start() did nothing, so the transaction
 * neither completed nor errored -- it just hung. The [jni] census caught the
 * consequence: 10 transactions started, then the loading screen polling
 * Config_ConfigKeyExists and GetNetworkStatus every frame forever, waiting on a
 * download that can never finish.
 *
 * Default behaviour still fails every transaction immediately via
 * HttpTransactionError (device post-timeout). For CDN hot-package GETs,
 * Start() replays Response -> Data -> Complete from userdata/CDN.<maj.min>/
 * (version from [game] product_version_string). If that folder is missing it
 * is seeded once from the OBB Packages subgroup, then always read from disk
 * so later launches skip the heavy OBB inflate. [runtime] use_cdn=0 turns it off.
 * Set [runtime] rewrite_cdn=1 to force a one-shot overwrite from the OBB each launch. Ads / draper /
 * anything without a match still fail.
 */

#include <pvz2native/dex/dex.h>
#include <pvz2native/config.h>
#include <pvz2native/dependencies/vfs.h>
#include <pvz2native/game/symbols.h>
#include <pvz2native/log.h>
#include <pvz2native/runtime/guest_runtime.h>
#include <pvz2native/runtime/rsb_index.h>

#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace pvz2native {
namespace dex {

/* Outside the anon namespace so net_mark_boot_finished can flip it. */
std::atomic<bool> g_boot_finished{false};

namespace {

constexpr const char *kClass = "com/popcap/SexyAppFramework/AndroidHttpTransaction";
constexpr const char *kProxyClass = "com/popcap/SexyAppFramework/AndroidHttpProxy";
constexpr std::size_t kDataChunk = 16384;

/* Per-transaction info captured at <init>: nativeTransaction for the callbacks,
 * method/url for logging + local CDN lookup, and the status/length the engine
 * reads back via GetStatusCode / GetResponseLength while handling Response. */
struct TxInfo {
    std::uint32_t native = 0;
    std::string method;
    std::string url;
    int status_code = 0;
    int response_length = 0;
};

std::mutex g_lock;
std::map<std::uint32_t, TxInfo> g_tx;

/* Absolute path of `userdata/...` from the process cwd (the build dir). */
std::string userdata_path(const std::string &rel) {
    return std::string("userdata/") + rel;
}

/* Path after the host, no leading slash, query stripped.
 * "https://host/android/liveconfig.json.4.5?x=1" -> "android/liveconfig.json.4.5" */
std::string url_rel_path(const std::string &url) {
    std::string u = url;
    const auto q = u.find('?');
    if (q != std::string::npos) u.resize(q);
    const auto scheme = u.find("://");
    std::size_t path_start = 0;
    if (scheme != std::string::npos) {
        const auto slash = u.find('/', scheme + 3);
        if (slash == std::string::npos) return {};
        path_start = slash + 1;
    } else if (!u.empty() && u[0] == '/') {
        path_start = 1;
    }
    return u.substr(path_start);
}

std::string basename_of(const std::string &rel) {
    const auto slash = rel.find_last_of('/');
    return slash == std::string::npos ? rel : rel.substr(slash + 1);
}

std::string ascii_lower_copy(std::string s) {
    for (char &ch : s) ch = (char)std::tolower((unsigned char)ch);
    return s;
}

/* Strip trailing CDN version segments: "liveconfig.json.4.5" -> "liveconfig.json".
 * Each dotted piece that is all digits is removed (so ".4" then ".5", or a
 * single ".45"). One pass used to leave "...json.4", which never matched the
 * OBB package map and made every CDN GET fall through to HttpTransactionError. */
std::string strip_cdn_version_suffix(std::string name) {
    for (;;) {
        const auto last_dot = name.find_last_of('.');
        if (last_dot == std::string::npos || last_dot + 1 >= name.size()) break;
        const std::string seg = name.substr(last_dot + 1);
        if (seg.empty() ||
            !std::all_of(seg.begin(), seg.end(),
                         [](unsigned char c) { return std::isdigit(c) != 0; })) {
            break;
        }
        /* Need a prior dot so we never strip the real extension ("file.4"). */
        if (name.find_last_of('.', last_dot - 1) == std::string::npos) break;
        name.resize(last_dot);
    }
    return name;
}

std::string rton_sibling(const std::string &name) {
    /* liveconfig.json / liveconfig.json.4.5 -> liveconfig.rton */
    std::string base = strip_cdn_version_suffix(name);
    const auto dot = base.find_last_of('.');
    if (dot != std::string::npos) base.resize(dot);
    return base + ".rton";
}

bool file_readable(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    return (bool)f;
}

std::vector<std::uint8_t> read_file_bytes(const std::string &path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const auto n = f.tellg();
    if (n <= 0) return {};
    std::vector<std::uint8_t> out((std::size_t)n);
    f.seekg(0);
    f.read(reinterpret_cast<char *>(out.data()), n);
    if (!f) return {};
    return out;
}

bool write_file_bytes(const std::string &path, const std::vector<std::uint8_t> &body) {
    std::error_code ec;
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    if (!body.empty()) f.write(reinterpret_cast<const char *>(body.data()), (std::streamsize)body.size());
    return (bool)f;
}

/* "4.5.2" / "9.6.1" -> "4.5" / "9.6" for userdata/CDN.<tag> and URL suffixes. */
std::string cdn_version_tag() {
    const char *raw = pvz2_config()->product_version_string;
    if (raw == nullptr || raw[0] == '\0') return "0.0";
    std::string v = raw;
    const auto first = v.find('.');
    if (first == std::string::npos) return v;
    const auto second = v.find('.', first + 1);
    if (second != std::string::npos) v.resize(second);
    return v;
}

std::string cdn_root_dir() {
    return userdata_path(std::string("CDN.") + cdn_version_tag());
}

/* Map a CDN URL basename (after stripping .4.5) to PACKAGES\<NAME>.RTON. */
struct CdnPackageMap {
    const char *cdn; /* lower-case basename without version suffix */
    const char *pkg;
};

constexpr CdnPackageMap kCdnPackageMap[] = {
    {"lod_config.json", "PACKAGES\\LOD_CONFIG.RTON"},
    {"liveconfig.json", "PACKAGES\\LIVECONFIG.RTON"},
    {"magento.json", "PACKAGES\\MAGENTO.RTON"},
    {"magento_filter.json", "PACKAGES\\MAGENTO_FILTER.RTON"},
    {"magento_schedule.json", "PACKAGES\\MAGENTO_SCHEDULE.RTON"},
    {"award_config.json", "PACKAGES\\AWARD_CONFIG.RTON"},
    {"arcade_config.json", "PACKAGES\\ARCADE_CONFIG.RTON"},
    {"easquared_config.json", "PACKAGES\\EASQUARED_CONFIG.RTON"},
    {"lootmodifierschedule.json", "PACKAGES\\LOOTMODIFIERSCHEDULE.RTON"},
};

std::string packages_path_for_cdn(const std::string &url) {
    std::string bare = strip_cdn_version_suffix(basename_of(url_rel_path(url)));
    for (char &c : bare) c = (char)std::tolower((unsigned char)c);
    for (const auto &m : kCdnPackageMap) {
        if (bare == m.cdn) return m.pkg;
    }
    return {};
}

/* Cached zlib-decompressed Packages subgroup + the absolute OBB base used by
 * RsbIndex entries (rsg_off + data_off). Packages stores its files inside one
 * zlib stream; the index offsets are relative to the decompressed payload. */
struct PackagesCache {
    std::string obb_path;
    std::uint64_t payload_base = 0; /* rsg_off + data_off */
    std::vector<std::uint8_t> data;
    bool tried = false;
    bool ok = false;
};

PackagesCache &packages_cache() {
    static PackagesCache c;
    return c;
}

std::uint32_t rd32_bytes(const std::uint8_t *p) {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

bool ensure_packages_decompressed(GuestRuntime *rt) {
    PackagesCache &c = packages_cache();
    const char *obb = vfs::obb_host_path();
    if (!obb || !obb[0]) return false;
    if (c.tried && c.obb_path == obb) return c.ok;
    c = PackagesCache{};
    c.tried = true;
    c.obb_path = obb;

    if (!rt) return false;
    {
        std::lock_guard<std::mutex> lk(rt->rsb_index_lock);
        if (!rt->rsb_index_tried) {
            rt->rsb_index_tried = true;
            rt->rsb_index.load(obb);
        }
        if (!rt->rsb_index.loaded()) return false;
    }

    FILE *f = std::fopen(obb, "rb");
    if (!f) return false;
    std::uint8_t head[0x100];
    if (std::fread(head, 1, sizeof(head), f) != sizeof(head) ||
        std::memcmp(head, "1bsr", 4) != 0) {
        std::fclose(f);
        return false;
    }
    const std::uint32_t rsg_number = rd32_bytes(head + 0x28);
    const std::uint32_t rsg_info_begin = rd32_bytes(head + 0x2C);
    const std::uint32_t rsg_info_each = rd32_bytes(head + 0x30);
    std::vector<std::uint8_t> info((std::size_t)rsg_number * rsg_info_each);
    if (std::fseek(f, (long)rsg_info_begin, SEEK_SET) != 0 ||
        std::fread(info.data(), 1, info.size(), f) != info.size()) {
        std::fclose(f);
        return false;
    }

    std::uint32_t rsg_off = 0, rsg_size = 0, data_off = 0;
    bool found = false;
    for (std::uint32_t i = 0; i < rsg_number; ++i) {
        const std::size_t rec = (std::size_t)i * rsg_info_each;
        char name[0x80];
        std::memcpy(name, info.data() + rec, 0x80);
        name[0x7f] = 0;
        if (std::strcmp(name, "Packages") != 0) continue;
        rsg_off = rd32_bytes(info.data() + rec + 0x80);
        rsg_size = rd32_bytes(info.data() + rec + 0x84);
        data_off = rd32_bytes(info.data() + rec + 0x90);
        found = true;
        break;
    }
    if (!found || rsg_size <= data_off) {
        std::fclose(f);
        return false;
    }

    const std::uint32_t zlen = rsg_size - data_off;
    std::vector<std::uint8_t> zbuf(zlen);
    if (std::fseek(f, (long)(rsg_off + data_off), SEEK_SET) != 0 ||
        std::fread(zbuf.data(), 1, zbuf.size(), f) != zbuf.size()) {
        std::fclose(f);
        return false;
    }
    std::fclose(f);

    z_stream strm{};
    if (inflateInit(&strm) != Z_OK) return false;
    c.data.resize(5 * 1024 * 1024); /* Packages expands to ~4.6 MB on 4.5.2 */
    strm.next_in = zbuf.data();
    strm.avail_in = (uInt)zbuf.size();
    strm.next_out = c.data.data();
    strm.avail_out = (uInt)c.data.size();
    int rc;
    for (;;) {
        rc = inflate(&strm, Z_NO_FLUSH);
        if (rc == Z_STREAM_END) break;
        if (rc != Z_OK) {
            inflateEnd(&strm);
            c.data.clear();
            return false;
        }
        if (strm.avail_out == 0) {
            const std::size_t produced = c.data.size();
            c.data.resize(produced + 2 * 1024 * 1024);
            strm.next_out = c.data.data() + produced;
            strm.avail_out = (uInt)(c.data.size() - produced);
        }
    }
    c.data.resize(strm.total_out);
    inflateEnd(&strm);
    c.payload_base = (std::uint64_t)rsg_off + data_off;
    c.ok = !c.data.empty();
    if (c.ok) {
        pvz2_log("[http] OBB Packages decompressed (%zu bytes) for CDN seed", c.data.size());
    } else {
        pvz2_log("[http] OBB Packages decompress failed -- CDN seed will fail");
    }
    return c.ok;
}

/* Read one PACKAGES\*.RTON out of the decompressed subgroup via RsbIndex. */
std::vector<std::uint8_t> read_obb_packages_file(GuestRuntime *rt, const std::string &pkg_path) {
    if (!ensure_packages_decompressed(rt)) return {};
    const RsbIndex::Entry *ent = nullptr;
    {
        std::lock_guard<std::mutex> lk(rt->rsb_index_lock);
        ent = rt->rsb_index.find(pkg_path);
    }
    if (!ent || ent->compressed || ent->size == 0) return {};
    PackagesCache &c = packages_cache();
    if (ent->offset < c.payload_base) return {};
    const std::uint64_t rel = ent->offset - c.payload_base;
    if (rel + ent->size > c.data.size()) return {};
    return std::vector<std::uint8_t>(c.data.begin() + (std::ptrdiff_t)rel,
                                     c.data.begin() + (std::ptrdiff_t)rel + (std::ptrdiff_t)ent->size);
}

void drop_packages_cache() {
    packages_cache() = PackagesCache{};
}

bool cdn_dir_ready(const std::string &root) {
    return std::filesystem::is_directory(root) && file_readable(root + "/.seeded");
}

/* Copy every known CDN package out of the OBB into userdata/CDN.<maj.min>/,
 * then drop the inflate cache so later reads hit the small on-disk files.
 * overwrite=true replaces existing files (rewrite_cdn); false is first seed. */
bool seed_cdn_dir_from_obb(GuestRuntime *rt, const std::string &root, bool overwrite) {
    const std::string tag = cdn_version_tag();
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec) {
        pvz2_log("[http] cannot create %s: %s", root.c_str(), ec.message().c_str());
        return false;
    }

    unsigned wrote = 0;
    for (const auto &m : kCdnPackageMap) {
        const auto body = read_obb_packages_file(rt, m.pkg);
        if (body.empty()) {
            pvz2_log("[http] CDN seed missing %s in OBB -- skipping", m.pkg);
            continue;
        }
        const std::string out = root + "/" + m.cdn + "." + tag;
        if (!write_file_bytes(out, body)) {
            pvz2_log("[http] CDN seed failed writing %s", out.c_str());
            return false;
        }
        ++wrote;
    }
    if (wrote == 0) {
        pvz2_log("[http] CDN seed wrote 0 files from OBB into %s", root.c_str());
        return false;
    }
    if (!write_file_bytes(root + "/.seeded", {})) {
        pvz2_log("[http] CDN seed could not write %s/.seeded", root.c_str());
        return false;
    }
    drop_packages_cache();
    if (overwrite) {
        pvz2_log("[http] re-seeded %s from OBB Packages (%u file(s), tag=%s, rewrite_cdn=1)",
                 root.c_str(), wrote, tag.c_str());
    } else {
        pvz2_log("[http] seeded %s from OBB Packages (%u file(s), tag=%s)", root.c_str(), wrote,
                 tag.c_str());
    }
    return true;
}

bool ensure_cdn_dir(GuestRuntime *rt, std::string &root_out) {
    root_out = cdn_root_dir();
    static bool s_rewrote_this_launch = false;
    const bool force = pvz2_config()->rewrite_cdn != 0;
    if (cdn_dir_ready(root_out) && !(force && !s_rewrote_this_launch)) return true;
    const bool ok = seed_cdn_dir_from_obb(rt, root_out, force);
    if (ok && force) s_rewrote_this_launch = true;
    return ok;
}

/* Resolve a CDN GET from userdata/CDN.<maj.min>/ (seeded from OBB once). */
bool resolve_cdn_body(GuestRuntime *rt, const std::string &url, std::string &src_label,
                      std::vector<std::uint8_t> &body) {
    body.clear();
    src_label.clear();
    if (packages_path_for_cdn(url).empty()) return false;

    std::string root;
    if (!ensure_cdn_dir(rt, root)) return false;

    const std::string rel = url_rel_path(url);
    if (rel.empty()) return false;
    const std::string base = basename_of(rel);
    const std::string bare = strip_cdn_version_suffix(base);
    const std::string rton = rton_sibling(base);
    const std::string tag = cdn_version_tag();
    /* OBB seed writes lowercase basenames; URLs often keep CamelCase
     * (LootModifierSchedule.json.4.5). Try both. */
    const std::string base_l = ascii_lower_copy(base);
    const std::string bare_l = ascii_lower_copy(bare);
    const std::string rton_l = ascii_lower_copy(rton);
    const std::string candidates[] = {
        root + "/" + rel,
        root + "/" + base,
        root + "/" + base_l,
        root + "/" + bare + "." + tag,
        root + "/" + bare_l + "." + tag,
        root + "/" + bare,
        root + "/" + bare_l,
        root + "/" + rton,
        root + "/" + rton_l,
        root + "/android/" + base,
        root + "/android/" + base_l,
        root + "/android/" + bare + "." + tag,
        root + "/android/" + bare_l + "." + tag,
        root + "/android/" + rton,
        root + "/android/" + rton_l,
    };
    for (const std::string &c : candidates) {
        if (!file_readable(c)) continue;
        body = read_file_bytes(c);
        if (!body.empty()) {
            src_label = c;
            return true;
        }
    }
    return false;
}

TxInfo tx_copy(std::uint32_t thiz) {
    std::lock_guard<std::mutex> lk(g_lock);
    auto it = g_tx.find(thiz);
    return it == g_tx.end() ? TxInfo{} : it->second;
}

void set_tx_status(std::uint32_t thiz, int status, int length) {
    std::lock_guard<std::mutex> lk(g_lock);
    auto it = g_tx.find(thiz);
    if (it == g_tx.end()) return;
    it->second.status_code = status;
    it->second.response_length = length;
}

std::uint32_t http_cb(std::uint32_t mapped, const char *name) {
    if (mapped != 0) return mapped;
    return find_native_method(kClass, name);
}

void fire_jlong(DexCall &d, std::uint32_t fn_off, std::uint32_t native) {
    const std::uint32_t fn = d.c.img->so_base + fn_off;
    const std::uint32_t args[4] = {kJniEnvPtrAddr, d.thiz, native, 0};
    d.c.call_guest(fn, args, 4);
}

void fire_data(DexCall &d, std::uint32_t fn_off, std::uint32_t native, std::uint32_t arr,
               std::uint32_t len) {
    /* void HttpReceivedData(JNIEnv*, jobject, jlong, jbyteArray, jint)
     * AAPCS: env/thiz in r0/r1, jlong in r2:r3, array+len on the stack. */
    const std::uint32_t fn = d.c.img->so_base + fn_off;
    const std::uint32_t args[6] = {kJniEnvPtrAddr, d.thiz, native, 0, arr, len};
    d.c.call_guest(fn, args, 6);
}

void fail_tx(DexCall &d, const TxInfo &tx) {
    const std::uint32_t err = http_cb(sym().jni_native.http_transaction_error, "HttpTransactionError");
    if (tx.native == 0 || err == 0) {
        d.c.log("[http] Start() on transaction 0x%08x with no error path (native=0x%08x, cb=0x%08x)"
                " method=%s url=%s -- request will hang",
                d.thiz, tx.native, err, tx.method.c_str(), tx.url.c_str());
        d.ret(0);
        return;
    }
    d.c.log("[http] failing %s %s (txn=0x%08x native=0x%08x) via HttpTransactionError",
            tx.method.empty() ? "?" : tx.method.c_str(),
            tx.url.empty() ? "?" : tx.url.c_str(), d.thiz, tx.native);
    fire_jlong(d, err, tx.native);
    d.ret(0);
}

bool serve_local(DexCall &d, const TxInfo &tx, const std::string &path,
                 const std::vector<std::uint8_t> &body) {
    const std::uint32_t resp =
        http_cb(sym().jni_native.http_received_response, "HttpReceivedResponse");
    const std::uint32_t data =
        http_cb(sym().jni_native.http_received_data, "HttpReceivedData");
    const std::uint32_t done =
        http_cb(sym().jni_native.http_transaction_complete, "HttpTransactionComplete");
    if (tx.native == 0 || resp == 0 || data == 0 || done == 0) {
        d.c.log("[http] cannot serve %s -- missing success natives "
                "(resp=0x%08x data=0x%08x complete=0x%08x native=0x%08x)",
                path.c_str(), resp, data, done, tx.native);
        return false;
    }

    set_tx_status(d.thiz, 200, (int)body.size());
    d.c.log("[http] serving %s %s -> %s (%zu bytes) via Response/Data/Complete",
            tx.method.empty() ? "?" : tx.method.c_str(),
            tx.url.empty() ? "?" : tx.url.c_str(), path.c_str(), body.size());

    fire_jlong(d, resp, tx.native);

    for (std::size_t off = 0; off < body.size(); off += kDataChunk) {
        const std::size_t n = std::min(kDataChunk, body.size() - off);
        const std::uint32_t arr = d.c.rt->heap.alloc((std::uint32_t)n);
        if (arr == 0) {
            d.c.log("[http] guest heap exhausted mid-serve of %s at +%zu -- falling back to error",
                    path.c_str(), off);
            return false;
        }
        std::memcpy(&d.c.img->mem[arr], body.data() + off, n);
        fire_data(d, data, tx.native, arr, (std::uint32_t)n);
    }
    /* Empty body: still fire Complete after Response, matching a 200 with no data. */
    fire_jlong(d, done, tx.native);
    return true;
}

/* AndroidHttpTransaction(long nativeTransaction, String method, String url)
 *
 * Built with NewObjectV, so the arguments arrive through the va_list. The jlong
 * occupies two words (low, then high=0 on ARM32); method/url follow at 2/3 --
 * same layout as GooglePlayPurchaseDriver(long, String). */
void ctor(DexCall &d) {
    TxInfo info;
    info.native = d.arg(0);
    info.method = d.string_arg(2);
    info.url = d.string_arg(3);
    std::lock_guard<std::mutex> lk(g_lock);
    g_tx[d.thiz] = std::move(info);
}

/* void Start() -- serve CDN bodies from the OBB (or userdata fallback), else
 * fail the way a device does after its connect timeout. */
void start(DexCall &d) {
    const TxInfo tx = tx_copy(d.thiz);

    const bool is_get =
        tx.method.empty() || tx.method == "GET" || tx.method == "get";
    if (pvz2_config()->use_cdn == 0) {
        static bool announced = false;
        if (!announced) {
            announced = true;
            d.c.log("[http] use_cdn=0 -- CDN requests are not served; the game uses the OBB data only");
        }
    } else if (is_get && !tx.url.empty()) {
        std::string src;
        std::vector<std::uint8_t> body;
        if (resolve_cdn_body(d.c.rt, tx.url, src, body)) {
            if (serve_local(d, tx, src, body)) {
                d.ret(0);
                return;
            }
            d.c.log("[http] CDN body unusable for %s (src=%s) -- failing",
                    tx.url.c_str(), src.c_str());
        }
    }

    fail_tx(d, tx);
}

/* void Release() -- the engine is done with the transaction; forget its mapping
 * so a long session cannot accumulate dead entries. */
void release(DexCall &d) {
    std::lock_guard<std::mutex> lk(g_lock);
    g_tx.erase(d.thiz);
    d.ret(0);
}

void get_status_code(DexCall &d) {
    const TxInfo tx = tx_copy(d.thiz);
    d.ret((std::uint32_t)tx.status_code);
}

void get_response_length(DexCall &d) {
    const TxInfo tx = tx_copy(d.thiz);
    d.ret((std::uint32_t)tx.response_length);
}

void get_status_line(DexCall &d) {
    const TxInfo tx = tx_copy(d.thiz);
    d.ret_string(tx.status_code == 200 ? "OK" : "");
}

void get_human_readable_url(DexCall &d) {
    const TxInfo tx = tx_copy(d.thiz);
    d.ret_string(tx.url);
}

/* empty-string GetResponseHeader -- headers are optional for these RTON bodies. */

/* --- AndroidHttpProxy ------------------------------------------------------
 *
 * int GetNetworkStatus() -- Android's connectivity, as the engine sees it:
 * 0 = none, 1 = mobile/WiMax, 2 = Wi-Fi, 3 = disconnected.
 *
 * Default config is none through onSurfaceCreated -- wifi that early hangs the
 * loading screen on content waits (PvZ2Web / resumable downloads) this port
 * cannot finish. After DidBecomeActive, net_mark_boot_finished promotes none
 * -> wifi so LoD/map online gates open; CDN bodies still come from OBB Packages.
 *
 * An explicit config of mobile/wifi is always returned as-is (including during
 * boot -- use with care). Purchases do not depend on this answer; the broker's
 * own checks are rewritten in game/patches.h.
 *
 * Startup() is deliberately NOT hooked. */
void get_network_status(DexCall &d) {
    int status = pvz2_config()->network_status;
    if (status == 0 && g_boot_finished.load(std::memory_order_relaxed)) status = 2;
    /* Log on first call and whenever the effective answer changes (boot -> wifi).
     * While still offline (boot), also log the guest LR so missing LoD/CDN gate
     * sites show up as "BL back to 0x...." without flooding post-boot wifi polls. */
    static int announced = -1;
    if (announced != status) {
        announced = status;
        d.c.log("[net] GetNetworkStatus -> %d (%s)%s; CDN GETs may still be served "
                "from userdata when a local file matches the URL",
                status, status == 2 ? "wifi" : status == 1 ? "mobile" : "no network",
                (status == 2 && pvz2_config()->network_status == 0)
                    ? " [promoted after boot]"
                    : "");
    }
    if (status == 0) {
        static unsigned boot_offline_logs = 0;
        if (boot_offline_logs < 32) {
            ++boot_offline_logs;
            d.c.log("[net] GetNetworkStatus offline probe lr=0x%08x (LoD/CDN gates "
                    "should be patched; boot/snapshot sites are left alone)",
                    d.c.lr());
        }
    }
    d.ret((std::uint32_t)status);
}

}  // namespace

void net_mark_boot_finished() {
    const bool was = g_boot_finished.exchange(true);
    if (!was) {
        pvz2_log("[net] boot finished -- GetNetworkStatus will report wifi "
                 "when config network=none (LoD / map online gates)");
    }
}

void register_android_http(HookTable &t) {
    t.add(kProxyClass, "GetNetworkStatus", get_network_status);

    t.add(kClass, "<init>", ctor);
    t.add(kClass, "Start", start);
    t.add(kClass, "Release", release);
    t.add(kClass, "GetStatusCode", get_status_code);
    t.add(kClass, "GetResponseLength", get_response_length);
    t.add(kClass, "GetStatusLine", get_status_line);
    t.add(kClass, "GetResponseHeader", hook_empty_string);
    t.add(kClass, "GetHumanReadableUrl", get_human_readable_url);
}

}  // namespace dex
}  // namespace pvz2native
