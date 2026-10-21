#include <pvz2native/runtime/guest_heap.h>
#include <pvz2native/log.h>

#include <pvz2native/config.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pvz2native {

void GuestHeap::report_exhausted(uint32_t want, uint32_t align) {
    constexpr int kMaxReports = 8;
    if (exhausted_reports_ >= kMaxReports) return;
    ++exhausted_reports_;

    /* The largest hole matters as much as the total: a heap with 100MB free in
     * thousands of small pieces cannot serve one 4MB texture, and the fix for
     * that is not a bigger heap. */
    uint32_t largest = free_by_size_.empty() ? 0 : free_by_size_.rbegin()->first;
    pvz2_log("[heap] EXHAUSTED -- %u bytes%s refused. in use %llu / %u MB "
                "(peak %llu MB), %zu free holes, largest %u KB",
                want, align > 8 ? " (aligned)" : "", (unsigned long long)(in_use_ >> 20), total_ >> 20,
                (unsigned long long)(peak_in_use_ >> 20), free_by_size_.size(), largest >> 10);
    if (exhausted_reports_ == kMaxReports) {
        pvz2_log("[heap] further exhaustion reports suppressed");
    }
    std::fflush(stdout);
}

void GuestHeap::for_each_live(const std::function<void(uint32_t, uint32_t)> &fn) {
    std::lock_guard<std::mutex> lk(lock_);
    for (const auto &kv : allocated_) fn(kv.first, kv.second);
}

void GuestHeap::usage(uint64_t &in_use, uint64_t &peak, uint64_t &total) {
    std::lock_guard<std::mutex> lk(lock_);
    in_use = in_use_;
    peak = peak_in_use_;
    total = total_;
}

void GuestHeap::init(uint32_t base, uint32_t size) {
    base_ = base;
    total_ = size;
    in_use_ = peak_in_use_ = 0;
    exhausted_reports_ = 0;
    insert_free({base, size});
    /* [runtime] heap_quarantine=N: hold the N most-recently-freed blocks out of
     * the reuse pool. If the wcscmp loop vanishes with a large N, the
     * corruption is definitively reuse of a prematurely-freed key buffer
     * (the freed bytes stay intact until reuse, so the map key keeps
     * reading as the correct string). Off (0) by default. */
    quarantine_depth_ = pvz2_config()->heap_quarantine;
}

uint32_t GuestHeap::alloc(uint32_t n, uint32_t lr) {
    std::lock_guard<std::mutex> lock(lock_);
    if (n == 0) n = 1;
    n = (n + 7u) & ~7u;

    /* Smallest block that fits, rather than the first one that does: the
     * size index makes best-fit the cheap option, and it leaves the large
     * holes intact for the large requests instead of shaving every one of
     * them down. */
    auto sit = free_by_size_.lower_bound(n);
    if (sit == free_by_size_.end()) {
        report_exhausted(n, 8);
        return 0;
    }

    const uint32_t addr = sit->second;
    const uint32_t size = sit->first;
    free_by_size_.erase(sit);
    free_by_addr_.erase(addr);

    if (size > n) {
        /* The remainder was interior to the block just taken, so it cannot be
         * adjacent to any other free block -- no coalescing pass needed. */
        auto rem_it = free_by_size_.emplace(size - n, addr + n);
        free_by_addr_.emplace(addr + n, FreeNode{size - n, rem_it});
    }

    allocated_[addr] = n;
    in_use_ += n;
    if (in_use_ > peak_in_use_) peak_in_use_ = in_use_;
    log_op(alloc_log_, addr, n, lr);
    return addr;
}

uint32_t GuestHeap::alloc_aligned(uint32_t n, uint32_t align, uint32_t lr) {
    /* Round the request up to a power of two, as the C standard requires of the
     * caller anyway, so the mask below is valid for anything we are handed. */
    uint32_t pow2 = 8;
    while (pow2 < align && pow2 < 0x10000000u) pow2 <<= 1;
    if (pow2 <= 8) return alloc(n, lr);

    std::lock_guard<std::mutex> lock(lock_);
    if (n == 0) n = 1;
    n = (n + 7u) & ~7u;

    /* memalign is rare -- the engine uses it for vertex and texture staging
     * buffers -- so a scan is affordable here in a way it is not in alloc().
     * Starting at lower_bound(n) skips every hole too small to be a
     * candidate whatever its alignment. */
    for (auto sit = free_by_size_.lower_bound(n); sit != free_by_size_.end(); ++sit) {
        const uint32_t base = sit->second;
        const uint32_t end = base + sit->first;
        const uint32_t addr = (base + pow2 - 1) & ~(pow2 - 1);
        if (addr < base || addr + n > end) continue; /* alignment padding doesn't fit */

        /* Carve the aligned span out and give the head/tail remainders back,
         * so alignment padding is reusable instead of leaked. */
        free_by_size_.erase(sit); /* invalidates sit -- must not loop again */
        free_by_addr_.erase(base);
        if (addr > base) insert_free({base, addr - base});
        if (addr + n < end) insert_free({addr + n, end - (addr + n)});
        allocated_[addr] = n;
        in_use_ += n;
        if (in_use_ > peak_in_use_) peak_in_use_ = in_use_;
        log_op(alloc_log_, addr, n, lr);
        return addr;
    }
    report_exhausted(n, align);
    return 0;
}

void GuestHeap::free_ptr(uint32_t addr, uint32_t lr) {
    if (addr == 0) return;
    std::lock_guard<std::mutex> lock(lock_);
    auto it = allocated_.find(addr);
    if (it == allocated_.end()) return; /* not ours, or double free: ignore rather than crash */
    uint32_t n = it->second;
    allocated_.erase(it);
    in_use_ -= (in_use_ >= n) ? n : in_use_;
    log_op(free_log_, addr, n, lr);

    if (quarantine_depth_ > 0) {
        quarantine_.push_back({addr, n});
        if (quarantine_.size() <= quarantine_depth_) return; /* not yet eligible for reuse */
        Block released = quarantine_.front();
        quarantine_.pop_front();
        insert_free(released);
        return;
    }
    insert_free({addr, n});
}

uint32_t GuestHeap::size_of(uint32_t addr) {
    std::lock_guard<std::mutex> lock(lock_);
    auto it = allocated_.find(addr);
    return it == allocated_.end() ? 0 : it->second;
}

void GuestHeap::query_addr(uint32_t x, bool &currently_live, uint32_t &live_base,
                OpRec &out_alloc, OpRec &out_free) {
    std::lock_guard<std::mutex> lock(lock_);
    currently_live = false;
    live_base = 0;
    for (const auto &kv : allocated_) {
        if (x >= kv.first && x < kv.first + kv.second) { currently_live = true; live_base = kv.first; break; }
    }
    out_alloc = find_covering(alloc_log_, x);
    out_free = find_covering(free_log_, x);
}

std::string GuestHeap::history(uint32_t x) {
    std::lock_guard<std::mutex> lock(lock_);
    struct Rec { char tag; OpRec r; };
    std::vector<Rec> recs;
    for (const auto &r : alloc_log_) if (x >= r.addr && x < r.addr + r.size) recs.push_back({'A', r});
    for (const auto &r : free_log_)  if (x >= r.addr && x < r.addr + r.size) recs.push_back({'F', r});
    std::sort(recs.begin(), recs.end(), [](const Rec &a, const Rec &b) { return a.r.seq < b.r.seq; });
    std::string s;
    char buf[160];
    for (const auto &rc : recs) {
        std::snprintf(buf, sizeof(buf), "    %c seq=%llu base=0x%08x sz=%u lr=0x%08x\n",
                      rc.tag, (unsigned long long)rc.r.seq, rc.r.addr, rc.r.size, rc.r.lr);
        s += buf;
    }
    return s;
}

void GuestHeap::insert_free(Block blk) {
    /* Only the two immediate neighbours can be adjacent to this block, and
     * the address index names both in one lookup -- so coalescing costs a
     * lookup rather than the full sort-and-rebuild this used to do.
     *
     * `next` is the first free block at or after blk.addr; blk itself is not
     * in the map yet, so everything before `next` sits below blk.addr and
     * std::prev(next) is therefore the block that could abut it from below.
     * That stays true after the successor is erased. */
    auto next = free_by_addr_.lower_bound(blk.addr);

    if (next != free_by_addr_.end() && blk.addr + blk.size == next->first) {
        blk.size += next->second.size;
        free_by_size_.erase(next->second.size_it);
        next = free_by_addr_.erase(next);
    }
    if (next != free_by_addr_.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second.size == blk.addr) {
            blk.addr = prev->first;
            blk.size += prev->second.size;
            free_by_size_.erase(prev->second.size_it);
            free_by_addr_.erase(prev);
        }
    }

    auto size_it = free_by_size_.emplace(blk.size, blk.addr);
    free_by_addr_.emplace(blk.addr, FreeNode{blk.size, size_it});
}

void GuestHeap::log_op(std::deque<OpRec> &log, uint32_t addr, uint32_t size, uint32_t lr) {
    log.push_back({addr, size, lr, ++op_seq_});
    if (log.size() > kOpLogMax) log.pop_front();
}

GuestHeap::OpRec GuestHeap::find_covering(const std::deque<OpRec> &log, uint32_t x) {
    for (auto it = log.rbegin(); it != log.rend(); ++it) {
        if (x >= it->addr && x < it->addr + it->size) return *it;
    }
    return {0, 0, 0, 0};
}

}  // namespace pvz2native
