#ifndef PVZ2NATIVE_RTON_TREE_H
#define PVZ2NATIVE_RTON_TREE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

/* RTON files as a tree, for the places that need to look inside one (the quest definitions, the player's
 * save) and to change a few values in place (the save repairs in save_dates.cpp).
 *
 * Every node keeps the byte range it occupies in the file, so a value can be replaced, or an array element
 * cut out, by splicing bytes -- RTON has no length prefixes, only an element count on arrays, so nothing
 * else in the file moves or needs fixing. A file the parser does not follow completely fails as a whole
 * instead of yielding a partial tree. Integers of every width land in `i`; floats, RTIDs and binary blobs
 * are skipped (kind Nil). */
namespace pvz2native {

struct RtonNode {
    enum Kind { Nil, Int, Str, Obj, Arr } kind = Nil;
    std::uint8_t tag = 0;            /* the value's type byte; an Int is re-encoded with it */
    std::size_t begin = 0, end = 0;  /* the whole value, type byte included */
    std::size_t count_begin = 0, count_end = 0; /* Arr: the element-count varint */
    std::int64_t i = 0;
    std::string s;
    std::vector<std::pair<std::string, RtonNode>> o;
    std::vector<RtonNode> a;

    /* The value of key `k` of this object, or nullptr. */
    const RtonNode *get(const std::string &k) const;
    /* First node called `k` anywhere below this one, or nullptr. */
    const RtonNode *find(const std::string &k) const;
};

/* True when `data` is an RTON v1 file this parser follows completely; `root` is then its top object. */
bool rton_parse(const std::vector<std::uint8_t> &data, RtonNode &root);

/* --- changing a file --- */

/* Replace bytes [begin, end) with `bytes` (end == begin inserts; empty `bytes` cuts). */
struct RtonEdit {
    std::size_t begin = 0, end = 0;
    std::vector<std::uint8_t> bytes;
};

/* `data` with the edits applied. They may come in any order but must not overlap. */
std::vector<std::uint8_t> rton_apply_edits(const std::vector<std::uint8_t> &data, std::vector<RtonEdit> edits);

/* The bytes of an integer value of type `tag` (a fixed-width or varint type, as in RtonNode::tag). */
std::vector<std::uint8_t> rton_encode_int(std::uint8_t tag, std::uint64_t v);

/* The bytes of a bare varint, as used for array element counts. */
std::vector<std::uint8_t> rton_encode_varint(std::uint64_t v);

}  // namespace pvz2native

#endif
