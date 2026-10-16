/* libc.so -- <locale.h>, collation, and the imported DATA symbols.
 *
 * Ten of libPVZ2.so's imports are STT_OBJECT: the guest reads THROUGH them
 * rather than calling them, so the ELF loader gives each a block of real guest
 * memory (see PVZ2_DATA_IMPORT_BASE) and this module lays out the contents.
 *
 * Getting the shape right matters more than the values. bionic inherits its
 * ctype implementation from OpenBSD, where the exported symbols are POINTERS to
 * tables rather than the tables themselves, and every table is indexed [c+1] so
 * that EOF (-1) is a legal subscript. Both facts are visible in the binary:
 *
 *     LDR  R2, [R7,R2]        ; R2 = &_ctype_       (the symbol's storage)
 *     LDR  LR, [R2]           ; LR = *_ctype_       (the table pointer)
 *     LDRB R4, [R3,R2]        ; ch
 *     ADD  R4, R4, LR
 *     LDRB R5, [R4,#1]        ; table[ch + 1]
 *
 *     LDR  R6, [R11]          ; R6 = *_tolower_tab_
 *     ADD  R2, R6, R2,LSL#1   ; 16-bit entries
 *     LDRH R8, [R2,#2]        ; table[c + 1]
 *
 * So each slot holds a pointer at +0 and the table it points to further in.
 * Writing the table AT the symbol -- which is what this did before the
 * disassembly was checked -- made the guest load the first four table bytes as
 * the table address, so every isalpha/isdigit/isspace/tolower in the engine
 * (RTON and XML parsing, path handling, string comparison) read from a garbage
 * address and answered nonsense, with nothing reported anywhere.
 */

#include <pvz2native/dependencies/dependency.h>
#include <pvz2native/log.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>

#include <pvz2native/dependencies/libc_internal.h>

namespace pvz2native {
namespace {

/* Bit values from bionic's <ctype.h>. */
constexpr unsigned char kCtlU = 0x01; /* _U upper */
constexpr unsigned char kCtlL = 0x02; /* _L lower */
constexpr unsigned char kCtlN = 0x04; /* _N digit */
constexpr unsigned char kCtlS = 0x08; /* _S space */
constexpr unsigned char kCtlP = 0x10; /* _P punct */
constexpr unsigned char kCtlC = 0x20; /* _C control */
constexpr unsigned char kCtlX = 0x40; /* _X hex digit */
constexpr unsigned char kCtlB = 0x80; /* _B blank */

unsigned char ctype_bits(int c) {
    unsigned char v = 0;
    if (c >= 'A' && c <= 'Z') v |= kCtlU;
    if (c >= 'a' && c <= 'z') v |= kCtlL;
    if (c >= '0' && c <= '9') v |= kCtlN;
    if (c == ' ' || (c >= '\t' && c <= '\r')) v |= kCtlS;
    if (c == ' ') v |= kCtlB;
    if ((c >= 0x21 && c <= 0x2F) || (c >= 0x3A && c <= 0x40) ||
        (c >= 0x5B && c <= 0x60) || (c >= 0x7B && c <= 0x7E)) v |= kCtlP;
    if (c < 0x20 || c == 0x7F) v |= kCtlC;
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) v |= kCtlX;
    return v;
}

/* Layout inside one data-import slot (PVZ2_DATA_IMPORT_SLOT is 1024 bytes):
 * the exported pointer at +0, the table it addresses at +16. */
constexpr std::uint32_t kTableOffset = 16;

/* sizeof(struct __sFILE) on armeabi-v7a bionic. Confirmed in the binary: a
 * write to stderr is `ADD R3, R1, #0xA8` off the base of __sF, i.e. &__sF[2]
 * with a 0x54 stride. */
constexpr std::uint32_t kSizeofFILE = 0x54;

void put8(pvz2_elf_image_t *img, std::uint32_t addr, unsigned char v) {
    if (addr < img->mem_size) img->mem[addr] = v;
}

void put16(pvz2_elf_image_t *img, std::uint32_t addr, std::uint16_t v) {
    if ((std::uint64_t)addr + 2 <= img->mem_size) std::memcpy(&img->mem[addr], &v, 2);
}

void put32(pvz2_elf_image_t *img, std::uint32_t addr, std::uint32_t v) {
    if ((std::uint64_t)addr + 4 <= img->mem_size) std::memcpy(&img->mem[addr], &v, 4);
}

void build_ctype_table(pvz2_elf_image_t *img, std::uint32_t slot) {
    std::uint32_t table = slot + kTableOffset;
    put32(img, slot, table);
    put8(img, table, 0); /* index 0 is the EOF entry; the guest indexes [c+1] */
    for (int c = 0; c < 256; ++c) put8(img, table + 1 + (std::uint32_t)c, ctype_bits(c));
}

void build_case_table(pvz2_elf_image_t *img, std::uint32_t slot, bool to_lower) {
    std::uint32_t table = slot + kTableOffset;
    put32(img, slot, table);
    put16(img, table, 0xFFFF); /* EOF maps to EOF */
    for (int c = 0; c < 256; ++c) {
        int r = c;
        if (to_lower && c >= 'A' && c <= 'Z') r = c + 32;
        if (!to_lower && c >= 'a' && c <= 'z') r = c - 32;
        put16(img, table + 2 + (std::uint32_t)c * 2, (std::uint16_t)r);
    }
}

/* --------------------------------------------------------------- locale */

void c_setlocale(GuestCall &c) {
    /* Only the "C" locale exists here, which is also what an Android app gets
     * unless it explicitly asks for another. Returning its name (rather than
     * NULL, which means "failed") keeps callers that check the result happy. */
    static std::uint32_t name = 0;
    if (name == 0) name = c.dup_cstr("C");
    c.set_result(name);
}

void c_localeconv(GuestCall &c) {
    /* struct lconv, C locale: "." for the decimal point and empty strings for
     * everything else. One persistent block, since callers keep the pointer. */
    static std::uint32_t block = 0;
    if (block == 0) {
        block = c.rt->heap.alloc(64);
        std::uint32_t dot = c.dup_cstr(".");
        std::uint32_t empty = c.dup_cstr("");
        for (std::uint32_t i = 0; i < 64; i += 4) c.write32(block + i, empty);
        c.write32(block + 0, dot); /* decimal_point is the first member */
    }
    c.set_result(block);
}

/* Collation in the C locale is plain byte (or wchar) order, so strcoll IS strcmp
 * and strxfrm is a copy. Sharing the loop rather than restating it means the
 * "is" in that sentence is enforced instead of merely intended. */
void c_strcoll(GuestCall &c) { libc::compare_bytes(c, libc::kUnbounded, true, false); }

void c_strxfrm(GuestCall &c) {
    std::uint32_t dst = c.arg(0), src = c.arg(1), n = c.arg(2), len = 0;
    while (c.read8(src + len) != 0) ++len;
    for (std::uint32_t i = 0; i < n && i <= len; ++i) c.write8(dst + i, c.read8(src + i));
    c.set_result(len);
}

void c_wcscoll(GuestCall &c) { libc::compare_wide(c, libc::kUnbounded, true); }

void c_wcsxfrm(GuestCall &c) {
    std::uint32_t dst = c.arg(0), src = c.arg(1), n = c.arg(2), i = 0;
    for (;; ++i) {
        std::uint32_t ch = c.read32(src + i * 4);
        if (i < n && dst != 0) c.write32(dst + i * 4, ch);
        if (ch == 0) break;
    }
    c.set_result(i);
}

}  // namespace

void register_libc_locale(ImportTable &t) {
    t.add("setlocale", c_setlocale);
    t.add("localeconv", c_localeconv);
    t.add("strcoll", c_strcoll);
    t.add("strxfrm", c_strxfrm);
    t.add("wcscoll", c_wcscoll);
    t.add("wcsxfrm", c_wcsxfrm);
}

void initialize_data_imports(pvz2_elf_image_t *img, GuestRuntime *rt) {
    for (std::uint32_t i = 0; i < img->data_import_count; ++i) {
        const char *name = img->data_import_names[i];
        std::uint32_t addr = img->data_import_addrs[i];

        if (std::strcmp(name, "_ctype_") == 0) {
            build_ctype_table(img, addr);
        } else if (std::strcmp(name, "_tolower_tab_") == 0) {
            build_case_table(img, addr, true);
        } else if (std::strcmp(name, "_toupper_tab_") == 0) {
            build_case_table(img, addr, false);
        } else if (std::strcmp(name, "__stack_chk_guard") == 0) {
            /* The value itself, not a pointer: the guest only ever compares it
             * against the copy it saved on function entry. */
            put32(img, addr, 0xDEADBEEF);
        } else if (std::strcmp(name, "__sF") == 0 && rt != nullptr) {
            /* FILE __sF[3]; stdin/stdout/stderr are &__sF[0..2]. The guest
             * computes those addresses itself and hands them to fwrite/fputs as
             * FILE*, so they have to be live tokens or every diagnostic the
             * engine prints to stderr disappears. */
            std::lock_guard<std::mutex> lk(rt->files_lock);
            rt->host_files[addr + 0 * kSizeofFILE] = stdin;
            rt->host_files[addr + 1 * kSizeofFILE] = stdout;
            rt->host_files[addr + 2 * kSizeofFILE] = stderr;
        } else if (std::strcmp(name, "timezone") == 0) {
            /* bionic's `long timezone`: seconds WEST of UTC. New in 9.6.1.
             *
             * Taken from the host rather than left at the zero the block starts
             * as, because this module's localtime() is the host's -- reporting
             * UTC here while localtime returns local time would make any caller
             * that reconstructs a UTC timestamp from the two disagree with
             * itself by the offset. */
            const std::time_t probe = 0;
            std::tm utc{};
            std::tm local{};
#if defined(_WIN32)
            gmtime_s(&utc, &probe);
            localtime_s(&local, &probe);
#else
            gmtime_r(&probe, &utc);
            localtime_r(&probe, &local);
#endif
            /* The probe is the epoch, so local time may land on the previous or
             * next day -- and across a month boundary tm_mday jumps by ~30, not
             * by 1. Normalise the difference to -1/0/+1 before scaling it. */
            int day_diff = local.tm_mday - utc.tm_mday;
            if (day_diff > 1) day_diff = -1;
            else if (day_diff < -1) day_diff = 1;
            const long east = day_diff * 86400 + (local.tm_hour - utc.tm_hour) * 3600 +
                              (local.tm_min - utc.tm_min) * 60;
            put32(img, addr, (std::uint32_t)(std::int32_t)(-east));
        } else {
            /* The five OpenSLES SL_IID_* ids need distinct, non-zero values or
             * GetInterface cannot tell them apart -- see libopensles.cpp. */
            initialize_sl_interface_id(img, name, addr);
        }

        pvz2_log("[data-import] %-34s -> 0x%08x", name, addr);
    }
}

}  // namespace pvz2native
