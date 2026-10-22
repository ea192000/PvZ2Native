#ifndef PVZ2NATIVE_HASH_H
#define PVZ2NATIVE_HASH_H

#include <cstdint>
#include <string_view>

namespace pvz2native {

/* 32-bit FNV-1 (multiply, then xor), the hash the engine keys quest ids, event names and similar by. */
inline std::uint32_t fnv1(std::string_view s) {
    std::uint32_t h = 2166136261u;
    for (unsigned char c : s) h = (h * 16777619u) ^ c;
    return h;
}

}  // namespace pvz2native

#endif
