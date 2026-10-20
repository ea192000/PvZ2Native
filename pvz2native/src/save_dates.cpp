/* Future-dated timestamps in the save -- see include/pvz2native/save_dates.h.
 *
 * RTON has no length prefixes (objects end with 0xFF, arrays carry an element count), so a value
 * can be replaced by bytes of any size without touching anything else in the file. Strings are
 * cached by index, so the walker has to follow every string definition to resolve the key names
 * of later references -- nothing more of the format is interpreted. */

#include <pvz2native/save_dates.h>

#include <pvz2native/config.h>
#include <pvz2native/log.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

constexpr long long kFutureLimitSeconds = 30LL * 86400; /* anything further ahead than this is bogus */
constexpr long long kResetBackSeconds = 86400;          /* reset to "one day ago", past the 12 h pick wait */

struct IntPatch {
    std::size_t begin;
    std::size_t end;
    std::uint8_t tag;
};

class RtonWalker {
public:
    explicit RtonWalker(const std::vector<std::uint8_t> &d) : d_(d) {}

    /* False when the bytes are not an RTON v1 file this walker fully follows. */
    bool run(std::uint64_t limit, std::vector<IntPatch> &patches) {
        limit_ = limit;
        patches_ = &patches;
        if (d_.size() < 9 || std::memcmp(d_.data(), "RTON", 4) != 0) return false;
        if (d_[4] != 1 || d_[5] != 0 || d_[6] != 0 || d_[7] != 0) return false;
        pos_ = 8;
        if (!object_body("", 0)) return false;
        if (pos_ == d_.size()) return true;
        return d_.size() - pos_ == 4 && std::memcmp(d_.data() + pos_, "DONE", 4) == 0;
    }

private:
    const std::vector<std::uint8_t> &d_;
    std::size_t pos_ = 0;
    std::uint64_t limit_ = 0;
    std::vector<IntPatch> *patches_ = nullptr;
    std::vector<std::string> latin_, utf8_;

    bool need(std::size_t n) const { return pos_ + n <= d_.size(); }

    bool varint(std::uint64_t &v) {
        v = 0;
        for (int shift = 0; shift < 70; shift += 7) {
            if (!need(1)) return false;
            const std::uint8_t b = d_[pos_++];
            if (shift < 64) v |= (std::uint64_t)(b & 0x7F) << shift;
            if (!(b & 0x80)) return true;
        }
        return false;
    }

    bool skip(std::uint64_t n) {
        if (n > d_.size() || !need((std::size_t)n)) return false;
        pos_ += (std::size_t)n;
        return true;
    }

    /* A string value of any kind; `out` gets its bytes. */
    bool string_value(std::uint8_t tag, std::string &out) {
        std::uint64_t n = 0, bytes = 0, idx = 0;
        switch (tag) {
            case 0x81: case 0x90:
                if (!varint(n) || !need((std::size_t)n)) return false;
                out.assign((const char *)d_.data() + pos_, (std::size_t)n);
                pos_ += (std::size_t)n;
                if (tag == 0x90) latin_.push_back(out);
                return true;
            case 0x82: case 0x92:
                if (!varint(n) || !varint(bytes) || !need((std::size_t)bytes)) return false;
                out.assign((const char *)d_.data() + pos_, (std::size_t)bytes);
                pos_ += (std::size_t)bytes;
                if (tag == 0x92) utf8_.push_back(out);
                return true;
            case 0x91:
                if (!varint(idx) || idx >= latin_.size()) return false;
                out = latin_[(std::size_t)idx];
                return true;
            case 0x93:
                if (!varint(idx) || idx >= utf8_.size()) return false;
                out = utf8_[(std::size_t)idx];
                return true;
        }
        return false;
    }

    bool utf8_payload() {
        std::uint64_t chars = 0, bytes = 0;
        return varint(chars) && varint(bytes) && skip(bytes);
    }

    bool rtid() {
        if (!need(1)) return false;
        const std::uint8_t sub = d_[pos_++];
        std::uint64_t v = 0;
        switch (sub) {
            case 0x00: return true;
            case 0x01: return varint(v) && varint(v) && skip(4);
            case 0x02: return utf8_payload() && varint(v) && varint(v) && skip(4);
            case 0x03: return utf8_payload() && utf8_payload();
        }
        return false;
    }

    bool object_body(const std::string &own_key, int depth) {
        if (depth > 64) return false;
        while (need(1)) {
            if (d_[pos_] == 0xFF) {
                ++pos_;
                return true;
            }
            const std::uint8_t ktag = d_[pos_++];
            std::string key;
            if (!string_value(ktag, key)) return false;
            if (!value(key, own_key, depth)) return false;
        }
        return depth == 0; /* the root may end with the file */
    }

    /* `key` names this value; `enclosing` names the container it sits in. Array elements keep the
     * array's own key, so an object inside "qlgi" is itself called "qlgi". */
    bool value(const std::string &key, const std::string &enclosing, int depth) {
        if (!need(1) || depth > 64) return false;
        const std::size_t begin = pos_;
        const std::uint8_t tag = d_[pos_++];
        std::uint64_t n = 0;
        std::string s;
        switch (tag) {
            case 0x00: case 0x01: case 0x02:
            case 0x09: case 0x0b: case 0x11: case 0x13: case 0x21: case 0x27:
            case 0x41: case 0x47: case 0x23: case 0x43: case 0x84:
                return true;
            case 0x08: case 0x0a: return skip(1);
            case 0x10: case 0x12: return skip(2);
            case 0x22: return skip(4);
            case 0x42: return skip(8);
            case 0x20: case 0x26: case 0x40: case 0x46: {
                const std::size_t width = (tag == 0x20 || tag == 0x26) ? 4 : 8;
                if (!need(width)) return false;
                std::uint64_t v = 0;
                for (std::size_t i = 0; i < width; ++i) v |= (std::uint64_t)d_[pos_ + i] << (8 * i);
                pos_ += width;
                note_int(key, enclosing, begin, tag, v);
                return true;
            }
            case 0x24: case 0x28: case 0x44: case 0x48:
                if (!varint(n)) return false;
                note_int(key, enclosing, begin, tag, n);
                return true;
            case 0x25: case 0x45:
                return varint(n);
            case 0x81: case 0x82: case 0x90: case 0x91: case 0x92: case 0x93:
                return string_value(tag, s);
            case 0x83: return rtid();
            case 0x85: return object_body(key, depth + 1);
            case 0x86: {
                if (!need(1) || d_[pos_++] != 0xFD || !varint(n)) return false;
                for (std::uint64_t i = 0; i < n; ++i)
                    if (!value(key, enclosing, depth + 1)) return false;
                if (!need(1) || d_[pos_++] != 0xFE) return false;
                return true;
            }
            case 0x87: {
                if (!need(1)) return false;
                ++pos_; /* sub-tag */
                std::uint64_t len = 0;
                return varint(n) && skip(n) && varint(len) && skip(len);
            }
        }
        return false;
    }

    void note_int(const std::string &key, const std::string &enclosing, std::size_t begin,
                  std::uint8_t tag, std::uint64_t v) {
        if (key != "l" || enclosing != "qlgi" || v <= limit_) return;
        patches_->push_back({begin, pos_, tag});
    }
};

void encode_int(std::vector<std::uint8_t> &out, std::uint8_t tag, std::uint64_t v) {
    out.push_back(tag);
    if (tag == 0x20 || tag == 0x26 || tag == 0x40 || tag == 0x46) {
        const int width = (tag == 0x20 || tag == 0x26) ? 4 : 8;
        for (int i = 0; i < width; ++i) out.push_back((std::uint8_t)((v >> (8 * i)) & 0xFF));
        return;
    }
    do {
        std::uint8_t b = (std::uint8_t)(v & 0x7F);
        v >>= 7;
        if (v) b |= 0x80;
        out.push_back(b);
    } while (v);
}

/* Rewrites one file; returns how many timestamps it changed. */
int fix_file(const std::filesystem::path &path, long long now) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return 0;

    std::vector<std::uint8_t> data;
    {
        std::ifstream f(path, std::ios::binary);
        if (!f) return 0;
        data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }

    std::vector<IntPatch> patches;
    RtonWalker walker(data);
    if (!walker.run((std::uint64_t)(now + kFutureLimitSeconds), patches)) {
        pvz2_log("[save] %s: not understood as RTON -- left untouched", path.string().c_str());
        return 0;
    }
    if (patches.empty()) return 0;

    const std::uint64_t replacement = (std::uint64_t)(now - kResetBackSeconds);
    std::vector<std::uint8_t> out;
    out.reserve(data.size());
    std::size_t at = 0;
    for (const IntPatch &p : patches) {
        out.insert(out.end(), data.begin() + (std::ptrdiff_t)at, data.begin() + (std::ptrdiff_t)p.begin);
        encode_int(out, p.tag, replacement);
        at = p.end;
    }
    out.insert(out.end(), data.begin() + (std::ptrdiff_t)at, data.end());

    /* Prove the result still parses and has nothing left to fix before replacing anything. */
    {
        std::vector<IntPatch> again;
        RtonWalker check(out);
        if (!check.run((std::uint64_t)(now + kFutureLimitSeconds), again) || !again.empty()) {
            pvz2_log("[save] %s: rewrite did not verify -- left untouched", path.string().c_str());
            return 0;
        }
    }

    std::filesystem::path tmp = path;
    tmp += ".tmp_futuredate";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return 0;
        f.write((const char *)out.data(), (std::streamsize)out.size());
        if (!f) return 0;
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        pvz2_log("[save] %s: could not replace the file (%s)", path.string().c_str(), ec.message().c_str());
        return 0;
    }
    return (int)patches.size();
}

}  // namespace

extern "C" int pvz2_fix_future_save_dates(void) {
    const pvz2_config_t *cfg = pvz2_config();
    if (cfg->no_fix_save_dates) return 0;
    if (cfg->force_date[0] != '\0') return 0;

    const long long now = (long long)std::time(nullptr);
    int total = 0;
    /* The engine's own folder name for save data, as vfs.cpp maps it, relative to the working dir. */
    for (const char *name : {"userdata/pp.dat", "userdata/snapshot1.dat", "userdata/snapshot2.dat"}) {
        const int n = fix_file(name, now);
        if (n > 0) {
            pvz2_log("[save] %s: %d quest-pick time(s) were more than 30 days in the future -- reset to yesterday",
                     name, n);
            total += n;
        }
    }
    return total;
}
