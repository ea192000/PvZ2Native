#ifndef PVZ2NATIVE_RUNTIME_GUEST_HEAP_H
#define PVZ2NATIVE_RUNTIME_GUEST_HEAP_H

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace pvz2native {

/* General-purpose malloc/free heap for guest code (and our own JNI array
 * allocations), backed by a carved-out region of the emulated address
 * space. Bookkeeping (free list / allocated sizes) lives on the host side;
 * guest code only ever sees the payload pointers it asked for.
 *
 * The free list is indexed twice -- by address, to coalesce neighbours, and
 * by size, to find a block -- so both malloc and free are O(log m) in the
 * number of free blocks. It used to be one vector that alloc scanned
 * linearly and free re-sorted in full, which is O(m log m) per free: fine
 * for the first thousand allocations and ruinous by the millionth. Loading
 * the resource groups fragments the heap into thousands of holes, and
 * onSurfaceCreated's ~6.9M allocations were costing ~16us each -- 113
 * seconds in one call, degrading as it went, which is what made the boot
 * look like it had hung rather than slowed. */
class GuestHeap {
public:
    /* Forensic record kept per alloc/free so the RtNameTable use-after-free
     * (doc 9.23) can be root-caused: when a comparison later reads a "key"
     * whose bytes are heap pointers, query_addr() reports whether that
     * address is currently live and, if freed, the guest LR that freed it and
     * the guest LR that most recently re-allocated it. */
    struct OpRec { uint32_t addr, size, lr; uint64_t seq; };

    void init(uint32_t base, uint32_t size);

    uint32_t alloc(uint32_t n, uint32_t lr = 0);

    /* memalign/posix_memalign: returns a block whose address is a multiple of
     * `align` (rounded up to a power of two). Plain alloc() only promises 8,
     * and the engine does ask for more -- 128-byte alignment for its vertex and
     * texture staging buffers. Handing those a merely-8-aligned block is the
     * kind of thing that works until it doesn't. */
    uint32_t alloc_aligned(uint32_t n, uint32_t align, uint32_t lr = 0);

    void free_ptr(uint32_t addr, uint32_t lr = 0);

    uint32_t size_of(uint32_t addr);

    /* Current and peak bytes handed out, and the heap's total span. Lets a
     * periodic diagnostic show whether usage is climbing toward exhaustion --
     * the failure mode behind a game that runs fine for minutes and then hangs,
     * because operator new returning 0 aborts the guest (see report_exhausted). */
    void usage(uint64_t &in_use, uint64_t &peak, uint64_t &total);

    /* Calls fn(addr, size) for every live block, with the heap lock held (fn must not
     * allocate or free guest memory). For the object census in diagnostics/verbose. */
    void for_each_live(const std::function<void(uint32_t, uint32_t)> &fn);

    /* Forensics for a suspected use-after-free address. Fills out_alloc/out_free
     * with the most-recent alloc/free record whose block covered `x` (seq==0
     * meaning "no record"), and reports whether `x` is currently live. */
    void query_addr(uint32_t x, bool &currently_live, uint32_t &live_base,
                    OpRec &out_alloc, OpRec &out_free);

    /* Full chronological birth/death timeline of every block that ever
     * covered `x`, so a use-after-free can be traced back to the premature
     * free (not just the last one). */
    std::string history(uint32_t x);

private:
    struct Block { uint32_t addr, size; };

    /* size -> addr, so a request finds the smallest block that fits with one
     * lower_bound. Multi- because equally-sized holes are the common case. */
    using FreeBySize = std::multimap<uint32_t, uint32_t>;

    /* Each address-ordered entry carries its own position in the size index,
     * so removing a block costs a pointer chase instead of a search through
     * every hole that happens to share its size. Declared after FreeBySize so
     * free_by_addr_ (and the iterators it holds) is destroyed first. */
    struct FreeNode {
        uint32_t size;
        FreeBySize::iterator size_it;
    };

    /* Adds a block to both indices, merging it with the neighbouring free
     * blocks on either side if they are adjacent. */
    void insert_free(Block blk);

    void log_op(std::deque<OpRec> &log, uint32_t addr, uint32_t size, uint32_t lr);
    static OpRec find_covering(const std::deque<OpRec> &log, uint32_t x);

    /* Says out loud that an allocation could not be served. Called with lock_
     * held, from both alloc paths.
     *
     * Exhaustion used to be a bare `return 0`, and that silence cost a long
     * hunt once already: `operator new` returning 0 makes the guest throw
     * std::bad_alloc, our call frames carry no handler (LR is the halt
     * sentinel), so the unwinder walks to the bottom of the stack and calls
     * abort() -- and the visible symptom is not "out of memory" but an engine
     * that runs frames forever without ever loading a resource group or drawing
     * anything. Capped, because a game that has run out will ask again. */
    void report_exhausted(uint32_t want, uint32_t align);

    static constexpr size_t kOpLogMax = 16384;
    FreeBySize free_by_size_;
    std::map<uint32_t, FreeNode> free_by_addr_;
    std::unordered_map<uint32_t, uint32_t> allocated_;
    std::deque<OpRec> alloc_log_, free_log_;
    std::deque<Block> quarantine_;
    uint32_t quarantine_depth_ = 0;
    uint64_t op_seq_ = 0;

    /* Enough to say how full the heap was when a request failed, which is the
     * difference between "genuinely too small" and "fragmented". */
    uint32_t base_ = 0, total_ = 0;
    uint64_t in_use_ = 0, peak_in_use_ = 0;
    int exhausted_reports_ = 0;

    std::mutex lock_;
};

}  // namespace pvz2native

#endif
