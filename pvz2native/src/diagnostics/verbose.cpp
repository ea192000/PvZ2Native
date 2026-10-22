/* [log] verbose=1 timeline -- see diagnostics/verbose.h. */

#include <pvz2native/diagnostics/verbose.h>

#include <pvz2native/config.h>
#include <pvz2native/dex/dex.h>
#include <pvz2native/diagnostics/diagnostics.h>
#include <pvz2native/diagnostics/quest_trace.h>
#include <pvz2native/log.h>
#include <pvz2native/runtime/dynarmic_config.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

namespace pvz2native {
namespace diagnostics {
namespace {

namespace rt_ = pvz2native::runtime;
using clock_t_ = std::chrono::steady_clock;

std::atomic<int> g_frame{0};
std::atomic<std::uint64_t> g_gl_draws{0};

/* Input bookkeeping. Written from the frame thread only (ui_process_events and
 * after_frame both run inside the frame call), so plain fields are enough. */
struct InputState {
    std::uint64_t events = 0;
    std::uint64_t downs = 0;
    std::uint64_t ups = 0;
    clock_t_::time_point last_delivery{};
    bool any_delivery = false;
} g_in;

/* Per-window rates for the [state] line. */
struct Window {
    clock_t_::time_point start = clock_t_::now();
    int frame0 = 0;
    std::uint64_t draws0 = 0;
    std::uint64_t imports = 0, jni = 0;
} g_win;

constexpr double kStateEverySec = 5.0;

double secs_since(clock_t_::time_point t) {
    return std::chrono::duration<double>(clock_t_::now() - t).count();
}

std::mutex g_census_lock;
std::string g_census_why;
clock_t_::time_point g_census_last = clock_t_::now();
constexpr double kCensusEverySec = 20.0;

/* Counts the live heap blocks whose first word points into libPVZ2's writable
 * segment -- i.e. C++ objects with a vtable -- grouped by that vtable. The engine has
 * no RTTI, so a class has no name here, but its vtable address is stable between
 * runs: lines are "vtable offset in the .so : live objects". Comparing two censuses
 * (a healthy map vs. the dead one) shows which classes are alive only in the bad state;
 * the offset can then be looked up in the disassembly (the constructor stores it). */
void census(pvz2_elf_image_t *img, GuestRuntime *rt, int frame, const std::string &why) {
    const std::uint32_t lo = img->so_base + img->text_vaddr + img->text_size;
    const std::uint32_t hi = img->so_base + img->so_span;
    std::map<std::uint32_t, std::uint32_t> counts;
    std::uint64_t objects = 0;
    rt->heap.for_each_live([&](std::uint32_t addr, std::uint32_t size) {
        if (size < 8 || addr + 4 > img->mem_size) return;
        std::uint32_t v;
        std::memcpy(&v, &img->mem[addr], 4);
        if (v < lo || v >= hi || (v & 3)) return;
        ++counts[v - img->so_base];
        ++objects;
    });
    pvz2_log("[census f=%d] %s: %llu objects with a vtable, %zu classes", frame, why.c_str(),
             (unsigned long long)objects, counts.size());
    std::string line;
    for (const auto &kv : counts) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), " %x:%u", kv.first, kv.second);
        line += buf;
        if (line.size() > 1100) {
            pvz2_log("[census f=%d]%s", frame, line.c_str());
            line.clear();
        }
    }
    if (!line.empty()) pvz2_log("[census f=%d]%s", frame, line.c_str());
}

}  // namespace

void request_census(const char *why) {
    if (!verbose()) return;
    std::lock_guard<std::mutex> lk(g_census_lock);
    g_census_why = why;
}

bool verbose() { return pvz2_config()->verbose != 0; }

bool input_trace() { return pvz2_config()->input != 0 || pvz2_config()->verbose != 0; }

int current_frame() { return g_frame.load(std::memory_order_relaxed); }

void vlog(const char *tag, const char *fmt, ...) {
    if (!verbose()) return;
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    pvz2_log("[%s f=%d] %s", tag, current_frame(), msg);
}

void note_gl_draw() {
    if (verbose()) g_gl_draws.fetch_add(1, std::memory_order_relaxed);
}

void note_input_delivered(unsigned events, bool had_down, bool had_up) {
    if (!verbose()) return;
    g_in.events += events;
    if (had_down) ++g_in.downs;
    if (had_up) ++g_in.ups;
    g_in.last_delivery = clock_t_::now();
    g_in.any_delivery = true;
}

void after_frame(pvz2_elf_image_t *img, GuestRuntime *rt, int frame) {
    g_frame.store(frame, std::memory_order_relaxed);
    if (!verbose()) return;

    quest_trace_after_frame(frame);

    {
        std::string why;
        {
            std::lock_guard<std::mutex> lk(g_census_lock);
            why.swap(g_census_why);
        }
        if (why.empty() && secs_since(g_census_last) >= kCensusEverySec) why = "periodic";
        if (!why.empty()) {
            g_census_last = clock_t_::now();
            census(img, rt, frame, why);
        }
    }

    const rt_::CallStats &f = rt_::last_call_stats();
    const std::uint64_t draws_total = g_gl_draws.load(std::memory_order_relaxed);
    g_win.imports += f.import_calls;
    g_win.jni += f.jni_calls;

    const double win_secs = secs_since(g_win.start);
    if (win_secs < kStateEverySec) return;

    const int frames = frame - g_win.frame0;
    std::uint64_t heap_in_use = 0, heap_peak = 0, heap_total = 0;
    rt->heap.usage(heap_in_use, heap_peak, heap_total);
    char input_age[48] = "never";
    if (g_in.any_delivery) {
        std::snprintf(input_age, sizeof(input_age), "%.1fs ago", secs_since(g_in.last_delivery));
    }
    vlog("state",
         "%.1f fps, %.0f draws/s, %.0f imports/frame, %.0f jni/frame | input: %llu events "
         "(%llu down, %llu up), last %s | heap %llu/%llu MB",
         frames / win_secs, (double)(draws_total - g_win.draws0) / win_secs,
         frames > 0 ? (double)g_win.imports / frames : 0.0, frames > 0 ? (double)g_win.jni / frames : 0.0,
         (unsigned long long)g_in.events, (unsigned long long)g_in.downs,
         (unsigned long long)g_in.ups, input_age, (unsigned long long)(heap_in_use >> 20),
         (unsigned long long)(heap_total >> 20));

    /* Which Java methods were asked for since the last report; throttles itself. */
    dex::report_call_census();

    g_win.start = clock_t_::now();
    g_win.frame0 = frame;
    g_win.draws0 = draws_total;
    g_win.imports = 0;
    g_win.jni = 0;
}

}  // namespace diagnostics
}  // namespace pvz2native
