/* RTON tree reader and editing helpers -- see include/pvz2native/rton_tree.h. */

#include <pvz2native/rton_tree.h>

#include <algorithm>
#include <cstring>

namespace pvz2native {

const RtonNode *RtonNode::get(const std::string &k) const {
    for (const auto &kv : o)
        if (kv.first == k) return &kv.second;
    return nullptr;
}

const RtonNode *RtonNode::find(const std::string &k) const {
    if (const RtonNode *n = get(k)) return n;
    for (const auto &kv : o)
        if (const RtonNode *n = kv.second.find(k)) return n;
    for (const RtonNode &e : a)
        if (const RtonNode *n = e.find(k)) return n;
    return nullptr;
}

namespace {

constexpr int kMaxDepth = 64;

/* The grammar: objects end with 0xFF, arrays are 0xFD <count> <elements> 0xFE, strings are cached by index
 * (0x90/0x92 define one, 0x91/0x93 refer back), so every definition has to be followed to resolve the key
 * names of later references. */
class RtonReader {
public:
    explicit RtonReader(const std::vector<std::uint8_t> &d) : d_(d) {}

    bool run(RtonNode &root) {
        if (d_.size() < 9 || std::memcmp(d_.data(), "RTON", 4) != 0 || d_[4] != 1 || d_[5] != 0 || d_[6] != 0 ||
            d_[7] != 0)
            return false;
        pos_ = 8;
        root.kind = RtonNode::Obj;
        root.begin = pos_;
        if (!object_body(root, 0)) return false;
        root.end = pos_;
        return pos_ == d_.size() || (d_.size() - pos_ == 4 && std::memcmp(d_.data() + pos_, "DONE", 4) == 0);
    }

private:
    const std::vector<std::uint8_t> &d_;
    std::size_t pos_ = 0;
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

    std::uint64_t fixed(int width) {
        std::uint64_t v = 0;
        for (int k = 0; k < width; ++k) v |= (std::uint64_t)d_[pos_ + k] << (8 * k);
        pos_ += (std::size_t)width;
        return v;
    }

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

    bool object_body(RtonNode &into, int depth) {
        if (depth > kMaxDepth) return false;
        while (need(1)) {
            if (d_[pos_] == 0xFF) {
                ++pos_;
                return true;
            }
            const std::uint8_t ktag = d_[pos_++];
            std::string key;
            if (!string_value(ktag, key)) return false;
            RtonNode v;
            if (!value(v, depth)) return false;
            into.o.emplace_back(std::move(key), std::move(v));
        }
        return depth == 0; /* the root may end with the file */
    }

    bool integer(RtonNode &out, std::int64_t v) {
        out.kind = RtonNode::Int;
        out.i = v;
        return true;
    }

    bool value(RtonNode &out, int depth) {
        if (!need(1) || depth > kMaxDepth) return false;
        out.begin = pos_;
        out.tag = d_[pos_++];
        const bool ok = value_body(out, depth);
        out.end = pos_;
        return ok;
    }

    bool value_body(RtonNode &out, int depth) {
        std::uint64_t n = 0;
        switch (out.tag) {
            case 0x00: return integer(out, 0);
            case 0x01: return integer(out, 1);
            case 0x02: case 0x09: case 0x0b: case 0x11: case 0x13: case 0x21: case 0x27:
            case 0x41: case 0x47: case 0x23: case 0x43: case 0x84:
                return true;
            case 0x08: return need(1) && integer(out, (std::int8_t)fixed(1));
            case 0x0a: return need(1) && integer(out, (std::int64_t)fixed(1));
            case 0x10: return need(2) && integer(out, (std::int16_t)fixed(2));
            case 0x12: return need(2) && integer(out, (std::int64_t)fixed(2));
            case 0x22: return skip(4);
            case 0x42: return skip(8);
            case 0x20: return need(4) && integer(out, (std::int32_t)fixed(4));
            case 0x26: return need(4) && integer(out, (std::int64_t)fixed(4));
            case 0x40: case 0x46: return need(8) && integer(out, (std::int64_t)fixed(8));
            case 0x24: case 0x28: case 0x44: case 0x48:
                return varint(n) && integer(out, (std::int64_t)n);
            case 0x25: case 0x45:
                return varint(n) && integer(out, (std::int64_t)((n >> 1) ^ (~(n & 1) + 1)));
            case 0x81: case 0x82: case 0x90: case 0x91: case 0x92: case 0x93:
                out.kind = RtonNode::Str;
                return string_value(out.tag, out.s);
            case 0x83: return rtid();
            case 0x85:
                out.kind = RtonNode::Obj;
                return object_body(out, depth + 1);
            case 0x86: {
                if (!need(1) || d_[pos_++] != 0xFD) return false;
                out.kind = RtonNode::Arr;
                out.count_begin = pos_;
                if (!varint(n)) return false;
                out.count_end = pos_;
                for (std::uint64_t k = 0; k < n; ++k) {
                    RtonNode e;
                    if (!value(e, depth + 1)) return false;
                    out.a.push_back(std::move(e));
                }
                return need(1) && d_[pos_++] == 0xFE;
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
};

}  // namespace

bool rton_parse(const std::vector<std::uint8_t> &data, RtonNode &root) { return RtonReader(data).run(root); }

std::vector<std::uint8_t> rton_apply_edits(const std::vector<std::uint8_t> &data, std::vector<RtonEdit> edits) {
    std::sort(edits.begin(), edits.end(), [](const RtonEdit &a, const RtonEdit &b) { return a.begin < b.begin; });
    std::vector<std::uint8_t> out;
    out.reserve(data.size());
    std::size_t at = 0;
    for (const RtonEdit &e : edits) {
        out.insert(out.end(), data.begin() + (std::ptrdiff_t)at, data.begin() + (std::ptrdiff_t)e.begin);
        out.insert(out.end(), e.bytes.begin(), e.bytes.end());
        at = e.end;
    }
    out.insert(out.end(), data.begin() + (std::ptrdiff_t)at, data.end());
    return out;
}

std::vector<std::uint8_t> rton_encode_varint(std::uint64_t v) {
    std::vector<std::uint8_t> out;
    do {
        std::uint8_t b = (std::uint8_t)(v & 0x7F);
        v >>= 7;
        if (v) b |= 0x80;
        out.push_back(b);
    } while (v);
    return out;
}

std::vector<std::uint8_t> rton_encode_int(std::uint8_t tag, std::uint64_t v) {
    std::vector<std::uint8_t> out{tag};
    if (tag == 0x20 || tag == 0x26 || tag == 0x40 || tag == 0x46) {
        const int width = (tag == 0x20 || tag == 0x26) ? 4 : 8;
        for (int i = 0; i < width; ++i) out.push_back((std::uint8_t)((v >> (8 * i)) & 0xFF));
        return out;
    }
    const std::vector<std::uint8_t> varint = rton_encode_varint(v);
    out.insert(out.end(), varint.begin(), varint.end());
    return out;
}

}  // namespace pvz2native
