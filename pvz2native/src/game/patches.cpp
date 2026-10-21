/* Guest-code patches -- see include/pvz2native/game/patches.h for why. */

#include <pvz2native/game/patches.h>

#include <pvz2native/config.h>
#include <pvz2native/dependencies/dependency.h>
#include <pvz2native/dex/dex.h>
#include <pvz2native/game/symbols.h>
#include <pvz2native/diagnostics/verbose.h>
#include <pvz2native/log.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <cstring>
#include <string>
#include <vector>

namespace pvz2native {
namespace {

/* ARM (never Thumb here -- the engine's ARM32 code in both mapped builds is
 * A32): cond 1011 imm24 is BL. Masking the condition off means a conditional
 * BL still matches, and B (1010) still does not. */
constexpr std::uint32_t kOpcodeMask = 0x0F000000u;
constexpr std::uint32_t kOpcodeBl   = 0x0B000000u;

/* MOV R0, #1 -- the value the patched-out call used to return for "online". */
constexpr std::uint32_t kMovR0One = 0xE3A00001u;

/* ARM NOP (MOV R0, R0) -- used to neutralize early-exit BNEs on LoD apply. */
constexpr std::uint32_t kNopMovR0R0 = 0xE1A00000u;

/* The two receipt instructions, before and after. MOV R1,#1 -> #0 turns "POST
 * this receipt and wait" into "finish it now"; MOV R0,#0 -> #1 turns that
 * branch's verdict from "not validated" into "validated". */
constexpr std::uint32_t kMovR1One  = 0xE3A01001u;
constexpr std::uint32_t kMovR1Zero = 0xE3A01000u;
constexpr std::uint32_t kMovR0Zero = 0xE3A00000u;

bool in_image(const pvz2_elf_image_t *img, std::uint32_t offset) {
    const std::uint64_t addr = (std::uint64_t)img->so_base + offset;
    return offset != 0 && addr + 4 <= img->mem_size;
}

std::uint32_t read_word(const pvz2_elf_image_t *img, std::uint32_t offset) {
    std::uint32_t w = 0;
    std::memcpy(&w, &img->mem[img->so_base + offset], 4);
    return w;
}

void write_word(pvz2_elf_image_t *img, std::uint32_t offset, std::uint32_t word) {
    std::memcpy(&img->mem[img->so_base + offset], &word, 4);
}

/* Rewrites one `BL <online?>` into `MOV R0, #1`, i.e. makes that one call site
 * read "connected" without changing what the callee returns to anyone else.
 * Refuses -- loudly -- if the instruction is not the BL the table promised,
 * because writing over something else would corrupt the engine silently. */
bool force_online_call(pvz2_elf_image_t *img, std::uint32_t offset, const char *kind) {
    if (!in_image(img, offset)) {
        pvz2_log("[patch] %s gate 0x%x is outside the image -- skipped", kind, offset);
        return false;
    }
    const std::uint32_t word = read_word(img, offset);
    if ((word & kOpcodeMask) != kOpcodeBl) {
        pvz2_log("[patch] %s gate 0x%x is not a BL (found 0x%08x) -- skipped; the "
                 "address in src/game/symbols.cpp is wrong for this build",
                 kind, offset, word);
        return false;
    }
    write_word(img, offset, kMovR0One);
    return true;
}

/* Same, but the call site is made to read "offline" (MOV R0, #0). */
bool force_offline_call(pvz2_elf_image_t *img, std::uint32_t offset, const char *kind) {
    if (!in_image(img, offset)) {
        pvz2_log("[patch] %s gate 0x%x is outside the image -- skipped", kind, offset);
        return false;
    }
    const std::uint32_t word = read_word(img, offset);
    if ((word & kOpcodeMask) != kOpcodeBl) {
        pvz2_log("[patch] %s gate 0x%x is not a BL (found 0x%08x) -- skipped; the "
                 "address in src/game/symbols.cpp is wrong for this build",
                 kind, offset, word);
        return false;
    }
    write_word(img, offset, kMovR0Zero);
    return true;
}

unsigned patch_gate_list(pvz2_elf_image_t *img, const std::uint32_t *gates, std::size_t count,
                         const char *kind, bool online = true) {
    unsigned patched = 0;
    for (std::size_t i = 0; i < count && gates[i] != 0; ++i) {
        if (online ? force_online_call(img, gates[i], kind) : force_offline_call(img, gates[i], kind))
            ++patched;
    }
    return patched;
}

/* Replaces one instruction, but only if it is exactly the one the table
 * promised -- a mismatch means the address belongs to another build and writing
 * over it would corrupt the engine silently, so it is reported and skipped. */
bool rewrite_word(pvz2_elf_image_t *img, std::uint32_t offset, std::uint32_t expect,
                  std::uint32_t replace, const char *what) {
    if (offset == 0) return false;
    if (!in_image(img, offset)) {
        pvz2_log("[patch] %s (0x%x) is outside the image -- skipped", what, offset);
        return false;
    }
    const std::uint32_t word = read_word(img, offset);
    if (word != expect) {
        pvz2_log("[patch] %s (0x%x) is 0x%08x, expected 0x%08x -- skipped; the address in "
                 "src/game/symbols.cpp is wrong for this build",
                 what, offset, word, expect);
        return false;
    }
    write_word(img, offset, replace);
    return true;
}

/* ARM BL encoding: PC-relative, PC = from + 8. */
std::uint32_t encode_bl(std::uint32_t from, std::uint32_t to) {
    const std::int32_t delta = (std::int32_t)(to - (from + 8));
    return 0xEB000000u | (((std::uint32_t)delta >> 2) & 0x00FFFFFFu);
}

/* 4.5.2: wire the LoD manager into the real apply (sub_3487AC) instead of the
 * BX LR stubs at sub_40F440, and NOP the two property-gate BNEs that otherwise
 * exit before schedule matching on this offline port. The map-tick BLEQ is left
 * alone -- forcing it unconditional invoked apply with r0=predicate (0/1). */
bool force_lod_apply(pvz2_elf_image_t *img) {
    bool any = false;
    /* BNE that skips the apply BL when the LevelOfTheDay toggle probe != 1. */
    if (rewrite_word(img, sym().patch.lod_apply_stub_skip_bne, 0x1A000001u, kNopMovR0R0,
                     "lod: stub skip BNE->NOP"))
        any = true;
    const std::uint32_t stub = sym().patch.lod_apply_stub_call;
    if (stub != 0) {
        /* Expected: BL sub_40F440. Replacement: BL real apply. */
        const std::uint32_t apply = sym().patch.lod_apply_fn;
        const std::uint32_t expect = encode_bl(stub, 0x40f440);
        const std::uint32_t replace = encode_bl(stub, apply != 0 ? apply : 0x3487ac);
        if (rewrite_word(img, stub, expect, replace, "lod: stub->apply"))
            any = true;
    }
    /* BNE after CMP R0,#1 (ready gate) -- expected imm24 encodes +0x21. */
    if (rewrite_word(img, sym().patch.lod_apply_ready_bne, 0x1A000021u, kNopMovR0R0,
                     "lod: ready BNE->NOP"))
        any = true;
    /* BNE after second property gate inside sub_40F44C. */
    if (rewrite_word(img, sym().patch.lod_apply_match_bne, 0x1A000060u, kNopMovR0R0,
                     "lod: match BNE->NOP"))
        any = true;
    /* BNE when profile +0x178 < +0x184 -- sticky Active LoD after a write that
     * refreshed alod/end but not the "last scheduled" field. */
    if (pvz2_config()->lod_legacy_hacks &&
        rewrite_word(img, sym().patch.lod_match_progress_bne, 0x1A000035u, kNopMovR0R0,
                     "lod: progress BNE->NOP"))
        any = true;
    /* BLS skipping UID writer when schedule_uid <= +0x178. */
    if (pvz2_config()->lod_legacy_hacks &&
        rewrite_word(img, sym().patch.lod_match_uid_bls, 0x9A000008u, kNopMovR0R0,
                     "lod: uid-order BLS->NOP"))
        any = true;
    /* BEQ into the float/skewed SexyApp clock -- that path was returning
     * ~2031 timestamps, so schedule match picked birthday UID 2980 and the
     * map banner showed ~40000h. Fall through to time() instead. */
    if (rewrite_word(img, sym().patch.lod_time_float_beq, 0x0A000004u, kNopMovR0R0,
                     "lod: time float BEQ->NOP"))
        any = true;
    return any;
}

/* Makes a completed payment finish locally AND finish as validated. Both halves
 * are required: the first alone leaves the engine at its "Unable to contact
 * store" dialog, because the branch it selects is the one that records a receipt
 * as unvalidated. */
bool accept_receipt_locally(pvz2_elf_image_t *img) {
    const bool finish_now = rewrite_word(img, sym().patch.purchase_local_receipt, kMovR1One,
                                         kMovR1Zero, "receipt: finish without the server");
    const bool verdict = rewrite_word(img, sym().patch.purchase_receipt_verdict, kMovR0Zero,
                                      kMovR0One, "receipt: verdict");
    if (finish_now == verdict) return finish_now;

    /* Exactly one landed. Say so plainly rather than leaving a half-patched
     * flow to be diagnosed from the game's behaviour. */
    pvz2_log("[patch] only half of the receipt patch applied -- purchases will %s",
             finish_now ? "report \"Unable to contact store\""
                        : "hang waiting for server validation");
    return false;
}

}  // namespace

void game_apply_patches(pvz2_elf_image_t *img) {
    if (img == nullptr) return;

    /* LoD/CDN sites are independent of the emulated store: they separate "apply
     * schedule while online" from GetNetworkStatus during boot (wifi-global
     * hangs the loading screen on resumable/PvZ2Web waits). */
    {
        const std::uint32_t *gates = sym().patch.lod_cdn_online_gate;
        const std::size_t count = sizeof(sym().patch.lod_cdn_online_gate) / sizeof(gates[0]);
        const unsigned n = patch_gate_list(img, gates, count, "lod/cdn");
        if (n != 0) {
            pvz2_log("[patch] lod/cdn: %u connectivity check(s) forced to \"online\" "
                     "so LoD can apply while boot stays offline",
                     n);
        }
    }

    /* LoD schedule commit (alod / wm) -- independent of connectivity patches. */
    if (force_lod_apply(img)) {
        pvz2_log("[patch] lod apply: stub->real apply, ready/match/time gates NOP'd so schedule can "
                 "commit Active LoD (tick BLEQ left alone)%s",
                 pvz2_config()->lod_legacy_hacks
                     ? "; legacy: progress + uid-order gates NOP'd, completed-field reset on"
                     : "; the engine's own completed-level rules are kept");
    }

    /* With the emulated store off the engine is meant to find no store at all,
     * and "no connection" is a perfectly good reason for it to say so. The
     * broker's connectivity checks read GetNetworkStatus, which is promoted to
     * wifi after boot (so LoD/CDN work); left alone they would let a purchase
     * start, RequestPayment is ignored and the dialog would sit on "Buying..."
     * forever. Force them offline instead. */
    if (pvz2_config()->emulate_iap == 0) {
        const std::uint32_t *gates = sym().patch.purchase_online_gate;
        const std::size_t count = sizeof(sym().patch.purchase_online_gate) / sizeof(gates[0]);
        const unsigned n = patch_gate_list(img, gates, count, "purchase", false);
        if (n != 0) {
            pvz2_log("[patch] purchase broker: %u connectivity check(s) forced to \"offline\" "
                     "(emulate_iap=0) so Buy reports \"no connection\" instead of hanging",
                     n);
        }
        return;
    }

    {
        const std::uint32_t *gates = sym().patch.purchase_online_gate;
        const std::size_t count = sizeof(sym().patch.purchase_online_gate) / sizeof(gates[0]);
        const unsigned patched = patch_gate_list(img, gates, count, "purchase");
        unsigned listed = 0;
        for (std::size_t i = 0; i < count && gates[i] != 0; ++i) ++listed;
        if (patched != 0) {
            pvz2_log("[patch] purchase broker: %u connectivity check(s) forced to \"online\" "
                     "so the emulated store is reachable offline",
                     patched);
        }
        if (patched < listed) {
            pvz2_log("[patch] %u purchase gate(s) left alone -- buying will report "
                     "\"Service Unavailable\"",
                     listed - patched);
        }
    }

    if (accept_receipt_locally(img)) {
        pvz2_log("[patch] purchase receipts are accepted locally, exactly as a validation "
                 "server answering \"passed\"");
    }
}

namespace {

/* Runtime probes for sub_3487AC / sub_40F44C. Entry is overwritten with SVC
 * (same index as a make_guest_callback stub). The handler emulates the original
 * PUSH but parks an exit trampoline as the saved LR so the matching POP reports
 * the return value without a nested call_guest (which would lose stack args). */
constexpr std::uint32_t kLodApplyPush = 0xE92D4070u; /* PUSH {r4,r5,r6,lr} */
constexpr std::uint32_t kLodMatchPush = 0xE92D4BF0u; /* PUSH {r4-r9,r11,lr} */
constexpr std::uint32_t kLodWriterPush = 0xE92D4010u; /* PUSH {r4,lr} */

/* Profile/LoD object fields touched by sub_69F8DC / getters next to it. */
constexpr std::uint32_t kLodOffPrev = 0x178;
constexpr std::uint32_t kLodOffAlod = 0x184;
constexpr std::uint32_t kLodOffWmish = 0x190;

struct LodProbeFrame {
    std::uint32_t lr = 0;
    std::uint32_t r0 = 0, r1 = 0, r2 = 0, r3 = 0;
    std::uint32_t seq = 0;
    std::uint32_t pre_178 = 0, pre_184 = 0, pre_190 = 0;
    bool obj_ok = false;
    std::chrono::steady_clock::time_point t0{};
};

thread_local std::vector<LodProbeFrame> g_lod_apply_frames;
thread_local std::vector<LodProbeFrame> g_lod_match_frames;
std::uint32_t g_lod_apply_exit_stub = 0;
std::uint32_t g_lod_match_exit_stub = 0;
std::atomic<std::uint32_t> g_lod_apply_seq{0};
std::atomic<std::uint32_t> g_lod_match_seq{0};

bool lod_obj_readable(GuestCall &c, std::uint32_t obj) {
    return obj >= 0x01000000u && c.in_bounds(obj, 0x200);
}

bool lod_interesting_str(const std::string &s) {
    if (s == "none" || s == "default" || s == "Default") return true;
    if (s.size() >= 5 && s.compare(0, 4, "lod_") == 0) return true;
    return false;
}

/* Dump alod-ish dwords + any nearby cstring/"none"/"lod_*" pointers or SSO. */
void dump_lod_object_state(GuestCall &c, std::uint32_t obj, const char *tag, std::uint32_t pre_178,
                           std::uint32_t pre_184, std::uint32_t pre_190, bool have_pre) {
    if (!lod_obj_readable(c, obj)) {
        c.log("[lod-apply] %s obj=0x%08x not a dumpable pointer", tag, obj);
        return;
    }
    const std::uint32_t v178 = c.read32(obj + kLodOffPrev);
    const std::uint32_t v184 = c.read32(obj + kLodOffAlod);
    const std::uint32_t v190 = c.read32(obj + kLodOffWmish);
    if (have_pre) {
        c.log("[lod-apply] %s obj=0x%08x +0x178=%u->%u +0x184(alod?)=%u->%u +0x190=%u->%u", tag,
              obj, pre_178, v178, pre_184, v184, pre_190, v190);
    } else {
        c.log("[lod-apply] %s obj=0x%08x +0x178=%u +0x184(alod?)=%u +0x190=%u", tag, obj, v178,
              v184, v190);
    }

    unsigned hits = 0;
    for (std::uint32_t off = 0; off < 0x200 && hits < 12; off += 4) {
        /* Inline short string (SSO / embedded). */
        if (c.in_bounds(obj + off, 8)) {
            const std::string inline_s = c.cstr(obj + off, 16);
            if (lod_interesting_str(inline_s)) {
                c.log("[lod-apply] %s obj+0x%x inline \"%s\"", tag, off, inline_s.c_str());
                ++hits;
                continue;
            }
        }
        const std::uint32_t p = c.read32(obj + off);
        if (!c.in_bounds(p, 8)) continue;
        const std::string s = c.cstr(p, 32);
        if (!lod_interesting_str(s)) continue;
        c.log("[lod-apply] %s obj+0x%x -> \"%s\" (ptr=0x%08x)", tag, off, s.c_str(), p);
        ++hits;
    }
}

void lod_apply_exit(GuestCall &c) {
    if (g_lod_apply_frames.empty()) {
        c.log("[lod-apply] EXIT without matching ENTER (r0=0x%08x lr=0x%08x) -- probe stack "
              "corrupt?",
              c.arg(0), c.lr());
        return;
    }
    const LodProbeFrame fr = g_lod_apply_frames.back();
    g_lod_apply_frames.pop_back();
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                             fr.t0)
                        .count();
    const std::time_t wall = (std::time_t)(std::time(nullptr) + pvz2_time_offset_sec());
    c.log("[lod-apply] EXIT  #%u r0=0x%08x (%.3f ms) wall=%lld caller_lr=0x%08x "
          "enter_args=(0x%08x,0x%08x,0x%08x,0x%08x)",
          fr.seq, c.arg(0), ms, (long long)wall, fr.lr, fr.r0, fr.r1, fr.r2, fr.r3);
    if (fr.obj_ok) {
        dump_lod_object_state(c, fr.r0, "post", fr.pre_178, fr.pre_184, fr.pre_190, true);
    } else if (lod_obj_readable(c, fr.r0)) {
        dump_lod_object_state(c, fr.r0, "post", 0, 0, 0, false);
    }
    c.regs[14] = fr.lr;
}

void lod_apply_enter(GuestCall &c) {
    LodProbeFrame fr;
    fr.lr = c.lr();
    fr.r0 = c.arg(0);
    fr.r1 = c.arg(1);
    fr.r2 = c.arg(2);
    fr.r3 = c.arg(3);
    fr.seq = g_lod_apply_seq.fetch_add(1) + 1;
    fr.t0 = std::chrono::steady_clock::now();
    fr.obj_ok = lod_obj_readable(c, fr.r0);
    if (fr.obj_ok) {
        fr.pre_178 = c.read32(fr.r0 + kLodOffPrev);
        fr.pre_184 = c.read32(fr.r0 + kLodOffAlod);
        fr.pre_190 = c.read32(fr.r0 + kLodOffWmish);
    }
    g_lod_apply_frames.push_back(fr);

    const std::time_t wall = (std::time_t)(std::time(nullptr) + pvz2_time_offset_sec());
    c.log("[lod-apply] ENTER #%u r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x lr=0x%08x "
          "sp=0x%08x wall=%lld",
          fr.seq, fr.r0, fr.r1, fr.r2, fr.r3, fr.lr, c.sp(), (long long)wall);
    if (fr.obj_ok) {
        dump_lod_object_state(c, fr.r0, "pre", fr.pre_178, fr.pre_184, fr.pre_190, false);
    }

    /* Emulate PUSH {r4,r5,r6,lr} with exit stub as the saved return address. */
    const std::uint32_t sp = c.sp() - 16;
    c.write32(sp + 0, c.regs[4]);
    c.write32(sp + 4, c.regs[5]);
    c.write32(sp + 8, c.regs[6]);
    c.write32(sp + 12, g_lod_apply_exit_stub);
    c.regs[13] = sp;
    /* Resume at the instruction after the SVC we planted (original SUB sp,#8). */
    c.no_return();
}

/* Profile object is r1 (sb) in sub_40F44C -- r0 is the LoD props/manager. */
constexpr std::uint32_t kLodMaxSaneEndSkewSec = 2u * 86400u; /* LoD days are 1-long */

void lod_match_exit(GuestCall &c) {
    if (g_lod_match_frames.empty()) {
        c.log("[lod-match] EXIT without matching ENTER (r0=0x%08x lr=0x%08x) -- probe stack "
              "corrupt?",
              c.arg(0), c.lr());
        return;
    }
    const LodProbeFrame fr = g_lod_match_frames.back();
    g_lod_match_frames.pop_back();
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                             fr.t0)
                        .count();
    const std::time_t wall = (std::time_t)(std::time(nullptr) + pvz2_time_offset_sec());

    /* sub_40F44C keeps the out-byte in R6 (= original R2). The path that
     * actually runs the UID writer (BL 69F8DC) then falls into the epilogue
     * with the writer's R0 (=0) and never executes the success STRB R0,[R6]
     * at 0x40F564. Apply reads that stack byte to arm the map LoD strip --
     * so a fresh commit looks "applied" in the profile but the banner stays
     * dark. Force the out-flag + return when Active LoD landed. */
    if (fr.obj_ok && lod_obj_readable(c, fr.r1)) {
        const std::uint32_t alod = c.read32(fr.r1 + kLodOffAlod);
        const std::uint32_t end = c.read32(fr.r1 + kLodOffWmish);
        const std::uint32_t now = (std::uint32_t)(std::time(nullptr) + pvz2_time_offset_sec());
        const bool active = alod != 0 && end > now && (end - now) <= kLodMaxSaneEndSkewSec;
        if (active) {
            if (c.in_bounds(fr.r2, 1)) {
                const std::uint8_t prev = c.read8(fr.r2);
                if (prev != 1) {
                    c.write8(fr.r2, 1);
                    c.log("[lod-match] force out-flag @0x%08x %u -> 1 (alod=%u end=%u)", fr.r2,
                          prev, alod, end);
                }
            }
            if (c.arg(0) == 0) {
                c.regs[0] = 1;
                c.log("[lod-match] force return 1 (writer left r0=0 after alod=%u)", alod);
            }
        }
    }

    c.log("[lod-match] EXIT  #%u r0=0x%08x (%.3f ms) wall=%lld caller_lr=0x%08x "
          "enter_args=(0x%08x,0x%08x,0x%08x,0x%08x)",
          fr.seq, c.arg(0), ms, (long long)wall, fr.lr, fr.r0, fr.r1, fr.r2, fr.r3);
    if (fr.obj_ok) {
        dump_lod_object_state(c, fr.r1, "profile-post", fr.pre_178, fr.pre_184, fr.pre_190, true);
    } else if (lod_obj_readable(c, fr.r1)) {
        dump_lod_object_state(c, fr.r1, "profile-post", 0, 0, 0, false);
    }
    c.regs[14] = fr.lr;
}

void lod_match_enter(GuestCall &c) {
    LodProbeFrame fr;
    fr.lr = c.lr();
    fr.r0 = c.arg(0);
    fr.r1 = c.arg(1);
    fr.r2 = c.arg(2);
    fr.r3 = c.arg(3);
    fr.seq = g_lod_match_seq.fetch_add(1) + 1;
    fr.t0 = std::chrono::steady_clock::now();
    /* Active-UID fields live on the profile (r1), not the props object (r0). */
    fr.obj_ok = lod_obj_readable(c, fr.r1);
    if (fr.obj_ok) {
        fr.pre_178 = c.read32(fr.r1 + kLodOffPrev);
        fr.pre_184 = c.read32(fr.r1 + kLodOffAlod);
        fr.pre_190 = c.read32(fr.r1 + kLodOffWmish);

        const std::uint32_t now = (std::uint32_t)(std::time(nullptr) + pvz2_time_offset_sec());
        /* Far-future alodet (e.g. 2031 birthday) sticks the map banner at
         * ~40000h and blocks re-match. Clear so today's schedule can commit. */
        if (fr.pre_190 > now && fr.pre_190 - now > kLodMaxSaneEndSkewSec) {
            c.log("[lod-match] clearing stale Active LoD alod=%u end=%u (now=%u, +%.1f days) "
                  "on profile 0x%08x",
                  fr.pre_184, fr.pre_190, now, (fr.pre_190 - now) / 86400.0, fr.r1);
            c.write32(fr.r1 + kLodOffPrev, 0);
            c.write32(fr.r1 + kLodOffAlod, 0);
            c.write32(fr.r1 + kLodOffWmish, 0);
            fr.pre_178 = fr.pre_184 = fr.pre_190 = 0;
        } else if (pvz2_config()->lod_legacy_hacks && fr.pre_184 != 0 && fr.pre_178 == fr.pre_184 && fr.pre_190 > now &&
                   (fr.pre_190 - now) <= kLodMaxSaneEndSkewSec) {
            /* Prior probe sync set +0x178==alod; that makes apply's R4!=0 and
             * clobbers the map out-flag. Real writer leaves +0x178 behind. */
            c.log("[lod-match] un-sync +0x178 %u -> 0 (was == alod; map out-flag needs "
                  "+0x178 < +0x184) on 0x%08x",
                  fr.pre_178, fr.r1);
            c.write32(fr.r1 + kLodOffPrev, 0);
            fr.pre_178 = 0;
        }
        /* Do NOT raise +0x178 to match alod — see above. */
    }
    g_lod_match_frames.push_back(fr);

    const std::time_t wall = (std::time_t)(std::time(nullptr) + pvz2_time_offset_sec());
    c.log("[lod-match] ENTER #%u r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x lr=0x%08x "
          "sp=0x%08x wall=%lld",
          fr.seq, fr.r0, fr.r1, fr.r2, fr.r3, fr.lr, c.sp(), (long long)wall);
    if (fr.obj_ok) {
        dump_lod_object_state(c, fr.r1, "profile-pre", fr.pre_178, fr.pre_184, fr.pre_190, false);
    }

    /* Emulate PUSH {r4-r9,r11,lr} (8 words) with exit stub as saved LR. */
    const std::uint32_t sp = c.sp() - 32;
    c.write32(sp + 0, c.regs[4]);
    c.write32(sp + 4, c.regs[5]);
    c.write32(sp + 8, c.regs[6]);
    c.write32(sp + 12, c.regs[7]);
    c.write32(sp + 16, c.regs[8]);
    c.write32(sp + 20, c.regs[9]);
    c.write32(sp + 24, c.regs[11]);
    c.write32(sp + 28, g_lod_match_exit_stub);
    c.regs[13] = sp;
    /* Resume at SUB sp,#0x10 after the SVC. */
    c.no_return();
}

/* sub_69F8DC -- Active UID / state writer (STR to +0x184 / +0x190). Enter-only. */
void lod_uid_writer_enter(GuestCall &c) {
    const std::uint32_t obj = c.arg(0);
    const std::uint32_t a = c.arg(1);
    const std::uint32_t b = c.arg(2);
    const std::uint32_t prev_alod =
        lod_obj_readable(c, obj) ? c.read32(obj + kLodOffAlod) : 0xffffffffu;
    if (a != prev_alod) {
        c.log("[lod-apply] UID-WRITER obj=0x%08x alod %u -> %u end=%u lr=0x%08x", obj, prev_alod, a,
              b, c.lr());
    }
    /* Leave +0x178 alone — see lod_match_enter. Syncing it to alod made
     * apply's R4!=0 path clobber the world-map LoD out-flag. */
    /* Emulate PUSH {r4,lr} with the real return address. */
    const std::uint32_t sp = c.sp() - 8;
    c.write32(sp + 0, c.regs[4]);
    c.write32(sp + 4, c.lr());
    c.regs[13] = sp;
    c.no_return();
}

bool install_svc_at(pvz2_elf_image_t *img, std::uint32_t offset, std::uint32_t expect_word,
                    const char *enter_name, ImportHandler enter_fn, const char *what) {
    if (offset == 0) return false;
    if (!in_image(img, offset)) {
        pvz2_log("[lod-apply] %s skipped -- 0x%x outside image", what, offset);
        return false;
    }
    const std::uint32_t word = read_word(img, offset);
    if (word != expect_word) {
        pvz2_log("[lod-apply] %s skipped -- entry 0x%x is 0x%08x, expected 0x%08x", what, offset,
                 word, expect_word);
        return false;
    }
    const std::uint32_t enter_stub = make_guest_callback(enter_name, enter_fn);
    if (enter_stub == 0) {
        pvz2_log("[lod-apply] %s skipped -- trampoline table full", what);
        return false;
    }
    std::uint32_t svc = 0;
    std::memcpy(&svc, &img->mem[enter_stub], 4);
    write_word(img, offset, svc);
    pvz2_log("[lod-apply] %s armed at 0x%x (svc via 0x%08x)", what, offset, enter_stub);
    return true;
}

}  // namespace

void game_install_lod_apply_probe(pvz2_elf_image_t *img) {
    if (img == nullptr) return;

    g_lod_apply_exit_stub = make_guest_callback("lod.apply.exit", lod_apply_exit);
    if (g_lod_apply_exit_stub == 0) {
        pvz2_log("[lod-apply] probe skipped -- trampoline table full (exit stub)");
        return;
    }
    g_lod_match_exit_stub = make_guest_callback("lod.match.exit", lod_match_exit);
    if (g_lod_match_exit_stub == 0) {
        pvz2_log("[lod-match] probe skipped -- trampoline table full (exit stub)");
        return;
    }

    install_svc_at(img, sym().patch.lod_apply_fn, kLodApplyPush, "lod.apply.enter", lod_apply_enter,
                   "apply probe");
    install_svc_at(img, sym().patch.lod_match_fn, kLodMatchPush, "lod.match.enter", lod_match_enter,
                   "match probe");
    install_svc_at(img, sym().patch.lod_uid_writer_fn, kLodWriterPush, "lod.uid.writer",
                   lod_uid_writer_enter, "UID-writer probe");
}

/* --- reward-screen singleton fix ------------------------------------------ *
 *
 * Each handler runs in place of ONE instruction (the PUSH at a function entry), so it
 * has to do what that instruction did; install_svc_at checks it is that instruction. */
namespace {

constexpr std::uint32_t kScrCreatePush = 0xE92D48F0u;  /* PUSH {r4-r7,fp,lr} */
constexpr std::uint32_t kScrDestroyPush = 0xE92D4070u; /* PUSH {r4,r5,r6,lr} */

/* The reward-screen singleton (see GameSymbols::patch.scr_global).
 *
 * Its create function stores the new object in a global without
 * checking it, and its destroy function destroys whatever the global holds. The epic
 * finale opens two of these at once -- the journal's "continue / back to map" screen,
 * then the quest-award screen a second later -- so the first is orphaned: the award
 * screen's destroy clears the global, and when the journal screen later closes itself
 * its destroy finds 0 and does nothing. The screen stays registered as a widget and
 * swallows every click on the map afterwards.
 *
 * Nested screens are meant to behave like a stack, so that is what this makes them:
 * create remembers the instance it is about to overwrite, and a destroy that finds the
 * global empty takes the remembered one back out first. Plain, non-overlapping use
 * (the only case that worked before) never touches the stack. */
std::vector<std::uint32_t> g_scr_stack;

void scr_push_frame(GuestCall &c, int nregs, const int *regs) {
    const std::uint32_t sp = c.sp() - 4 * (nregs + 1);
    for (int i = 0; i < nregs; ++i) c.write32(sp + 4 * i, c.regs[regs[i]]);
    c.write32(sp + 4 * nregs, c.lr());
    c.regs[13] = sp;
}

void scr_create(GuestCall &c) {
    const std::uint32_t global = c.img->so_base + sym().patch.scr_global;
    const std::uint32_t live = c.read32(global);
    if (live != 0) {
        g_scr_stack.push_back(live);
        pvz2_log("[screen] a reward screen is created while 0x%08x is still alive (caller .so+0x%x, "
                 "board '%s'): keeping it so its own destroy still finds it (%zu nested)", live,
                 c.lr() - c.img->so_base, dex::current_board_level().c_str(), g_scr_stack.size());
    }
    pvz2native::diagnostics::vlog("screen", "CREATE  reward-screen singleton, caller .so+0x%x "
                                  "(board='%s', live=0x%08x)", c.lr() - c.img->so_base,
                                  dex::current_board_level().c_str(), live);
    static const int kRegs[] = {4, 5, 6, 7, 11};
    scr_push_frame(c, 5, kRegs);
    c.no_return();
}

void scr_destroy(GuestCall &c) {
    const std::uint32_t global = c.img->so_base + sym().patch.scr_global;
    std::uint32_t live = c.read32(global);
    if (live == 0) {
        /* Only take back a remembered screen that is still what we think it is: a live
         * heap block whose vptr is this class's. Anything else was freed or reused by the
         * engine in the meantime and must not be touched. */
        while (!g_scr_stack.empty()) {
            const std::uint32_t cand = g_scr_stack.back();
            g_scr_stack.pop_back();
            if (c.rt->heap.size_of(cand) != 0 && c.read32(cand) == c.img->so_base + sym().patch.scr_vtable) {
                live = cand;
                c.write32(global, live);
                pvz2_log("[screen] destroy found no screen (caller .so+0x%x): destroying the one "
                         "that was orphaned, 0x%08x (%zu nested left)", c.lr() - c.img->so_base,
                         live, g_scr_stack.size());
                break;
            }
            pvz2_log("[screen] dropped remembered screen 0x%08x: no longer a live screen object",
                     cand);
        }
    }
    pvz2native::diagnostics::vlog("screen", "DESTROY reward-screen singleton, caller .so+0x%x "
                                  "(live=0x%08x)", c.lr() - c.img->so_base, live);
    static const int kRegs[] = {4, 5, 6};
    scr_push_frame(c, 3, kRegs);
    c.no_return();
}

}  // namespace

void game_install_reward_screen_fix(pvz2_elf_image_t *img) {
    if (img == nullptr) return;
    const auto &p = sym().patch;
    install_svc_at(img, p.scr_create, kScrCreatePush, "scr.create", scr_create, "screen create");
    install_svc_at(img, p.scr_destroy, kScrDestroyPush, "scr.destroy", scr_destroy, "screen destroy");
}

}  // namespace pvz2native
