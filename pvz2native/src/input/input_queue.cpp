/* The host-to-guest input queue -- see include/pvz2native/input/input_queue.h.
 *
 * Record layouts were read out of the engine's own dispatch handlers
 * (off_CCFA10, seven entries). Each handler advances the cursor by its own
 * record size, so the sizes below are not a convention we chose -- they are
 * what the engine will step over, and getting one wrong desynchronises every
 * later record in the same frame.
 */

#include <pvz2native/input/input_queue.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>

namespace {

/* Milliseconds since the epoch, on the SAME clock the engine sees through
 * gettimeofday (libc_time.cpp's now_ns uses system_clock too). It matters that
 * the two agree: the engine timestamps nothing itself -- it stores the value we
 * put in the touch record and compares it against its own gettimeofday-derived
 * clock, so a different epoch would make every gesture look impossibly old. */
double now_ms() {
    return (double)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

struct Event {
    enum class Kind { Touch, Key, Text } kind;
    int a = 0, b = 0, c = 0;    /* touch: phase,x,y  |  key: keycode,down */
    std::uint32_t id = 0;       /* touch id; see g_next_touch_id */
    double time_ms = 0.0;       /* when it happened, not when it was drained */
    std::string text;
};

/* Touch ids must NEVER be zero.
 *
 * The engine's gesture recognizer keeps the id of the touch it captured in a
 * slot it leaves ZEROED when it has captured nothing, and its touch-up handler
 * (sub_1C9FEC) decides whether the gesture is its own with a bare
 *     if (incoming_id == captured_id) { ...; return 1; }
 * A hardcoded id of 0 therefore matched "nothing captured" on every single
 * release, so the recognizer swallowed EVERY touch-up and returned "handled" --
 * the real UI path (sub_9C7428 -> sub_9DAB60) never ran. That one function is
 * both what delivers MouseUp to the captured widget and what clears the hover
 * (it ends in sub_9D9F80(screen, -1, -1)), which is exactly why a clicked
 * button stayed lit forever and never fired.
 *
 * A monotonic counter per touch sequence also matches what the field really is:
 * a unique touch handle, not Android's pointer index (which does start at 0). */
std::atomic<std::uint32_t> g_next_touch_id{1};
std::atomic<std::uint32_t> g_active_touch_id{1};

std::uint32_t touch_id_for(int phase) {
    if (phase != PVZ2_TOUCH_DOWN) return g_active_touch_id.load(std::memory_order_relaxed);
    std::uint32_t id = g_next_touch_id.fetch_add(1, std::memory_order_relaxed);
    if (id == 0) id = g_next_touch_id.fetch_add(1, std::memory_order_relaxed); /* skip on wrap */
    g_active_touch_id.store(id, std::memory_order_relaxed);
    return id;
}

std::mutex g_lock;
std::deque<Event> g_queue;
std::atomic<int> g_keyboard_wanted{0};
std::atomic<std::uint32_t> g_keyboard_wanted_generation{1};

/* A runaway producer must not grow this without bound; input older than this
 * is worthless anyway. */
constexpr std::size_t kMaxQueued = 256;

void push(Event ev) {
    ev.time_ms = now_ms(); /* stamped on arrival, not on drain */
    std::lock_guard<std::mutex> lk(g_lock);
    if (g_queue.size() >= kMaxQueued) g_queue.pop_front();
    g_queue.push_back(std::move(ev));
}

void put32(std::vector<std::uint8_t> &out, std::uint32_t v) {
    out.push_back((std::uint8_t)(v & 0xFF));
    out.push_back((std::uint8_t)((v >> 8) & 0xFF));
    out.push_back((std::uint8_t)((v >> 16) & 0xFF));
    out.push_back((std::uint8_t)((v >> 24) & 0xFF));
}

/* Size the engine will advance the cursor by for this event. */
std::size_t record_size(const Event &ev) {
    switch (ev.kind) {
        case Event::Kind::Touch: return 48;
        case Event::Kind::Key: return 32;
        case Event::Kind::Text: {
            const std::size_t n = ev.text.size();
            return 12 + n + ((4 - n % 4) % 4); /* padded so the next record stays aligned */
        }
    }
    return 0;
}

void encode(std::vector<std::uint8_t> &out, const Event &ev) {
    const std::size_t start = out.size();
    switch (ev.kind) {
        case Event::Kind::Touch:
            /* sub_9F0298: reads +4, +8, +12, +16, +20, +24, a double at +28 and
             * the phase at +36, then steps 48. The two coordinate pairs are
             * both fed through the screen-to-app rescale, so they carry the
             * same position; the engine keeps one as the transformed point and
             * one as the raw one. */
            put32(out, 0);           /* +0  type */
            put32(out, ev.id);       /* +4  touch id -- never 0, see g_next_touch_id */
            put32(out, (std::uint32_t)ev.b); /* +8  x */
            put32(out, (std::uint32_t)ev.c); /* +12 y */
            put32(out, (std::uint32_t)ev.b); /* +16 x again */
            put32(out, (std::uint32_t)ev.c); /* +20 y again */
            put32(out, 0);           /* +24 */
            /* +28 is a DOUBLE (sub_9F0298 reads it as one), in milliseconds on
             * the engine's own gettimeofday clock. It used to be hardcoded to
             * zero, which makes every gesture look as if it began at the epoch
             * -- anything deriving a duration or a velocity from it gets an
             * absurd answer. */
            {
                std::uint64_t bits;
                std::memcpy(&bits, &ev.time_ms, 8);
                put32(out, (std::uint32_t)(bits & 0xFFFFFFFFu)); /* +28 low  */
                put32(out, (std::uint32_t)(bits >> 32));         /* +32 high */
            }
            put32(out, (std::uint32_t)ev.a); /* +36 phase */
            put32(out, 0);           /* +40 */
            put32(out, 0);           /* +44 */
            break;

        case Event::Kind::Key:
            /* sub_9F0374: keycode at +4, down/up at +8, and +12 must be ZERO --
             * a non-zero value there makes the handler return without
             * dispatching the key at all. Steps 32. */
            put32(out, 1);                       /* +0  type */
            put32(out, (std::uint32_t)ev.a);     /* +4  Android keycode */
            put32(out, (std::uint32_t)ev.b);     /* +8  down/up */
            put32(out, 0);                       /* +12 must stay 0 */
            put32(out, 0);
            put32(out, 0);
            put32(out, 0);
            put32(out, 0);
            break;

        case Event::Kind::Text: {
            /* sub_9F0688 -> sub_9EBD40: id at +4, byte length at +8, then the
             * raw bytes, and the cursor advances by 12 + len rounded up to 4.
             * The string is built byte by byte, so it is NOT NUL-terminated
             * and the length is authoritative. */
            const std::size_t n = ev.text.size();
            put32(out, 6);                     /* +0  type */
            put32(out, 0);                     /* +4  id */
            put32(out, (std::uint32_t)n);      /* +8  length */
            out.insert(out.end(), ev.text.begin(), ev.text.end());
            for (std::size_t pad = (4 - n % 4) % 4; pad != 0; --pad) out.push_back(0);
            break;
        }
    }
    /* If this ever trips, the engine and this encoder disagree about how far to
     * step, and every following record in the frame is misread. */
    (void)start;
}

}  // namespace

extern "C" {

void pvz2_input_push_touch(int phase, int x, int y) {
    Event ev;
    ev.kind = Event::Kind::Touch;
    ev.a = phase;
    ev.b = x;
    ev.c = y;
    /* Allocated here, not at drain time, so a DOWN/MOVE/UP sequence keeps one
     * id even when the frames that deliver them are far apart. */
    ev.id = touch_id_for(phase);
    push(std::move(ev));
}

void pvz2_input_push_key(int android_keycode, int down) {
    Event ev;
    ev.kind = Event::Kind::Key;
    ev.a = android_keycode;
    ev.b = down;
    push(std::move(ev));
}

void pvz2_input_push_text(const char *utf8) {
    if (utf8 == nullptr || *utf8 == '\0') return;
    Event ev;
    ev.kind = Event::Kind::Text;
    ev.text = utf8;
    push(std::move(ev));
}

void pvz2_input_force_touch_reset(void) {
    const std::uint32_t active_id = g_active_touch_id.load(std::memory_order_relaxed);

    /* Clear any guest-side captured pointer using the SAME id that was active. */
    Event up;
    up.kind = Event::Kind::Touch;
    up.a = PVZ2_TOUCH_UP;
    up.b = 0;
    up.c = 0;
    up.id = active_id;
    push(std::move(up));

    Event cancel;
    cancel.kind = Event::Kind::Touch;
    cancel.a = PVZ2_TOUCH_CANCEL;
    cancel.b = 0;
    cancel.c = 0;
    cancel.id = active_id;
    push(std::move(cancel));

    /* Ensure following DOWN uses a fresh id not equal to any potentially
     * captured stale id in the guest. */
    std::uint32_t id = g_next_touch_id.fetch_add(1, std::memory_order_relaxed);
    if (id == 0) id = g_next_touch_id.fetch_add(1, std::memory_order_relaxed);
    g_active_touch_id.store(id, std::memory_order_relaxed);
}

int pvz2_input_keyboard_wanted(void) { return g_keyboard_wanted.load(std::memory_order_relaxed); }

void pvz2_input_set_keyboard_wanted(int wanted) {
    g_keyboard_wanted.store(wanted, std::memory_order_relaxed);
    std::uint32_t next = g_keyboard_wanted_generation.fetch_add(1, std::memory_order_relaxed) + 1;
    if (next == 0) {
        g_keyboard_wanted_generation.store(1, std::memory_order_relaxed);
    }
}

std::uint32_t pvz2_input_keyboard_wanted_generation(void) {
    return g_keyboard_wanted_generation.load(std::memory_order_relaxed);
}

}  // extern "C"

namespace pvz2native {
namespace input {

std::size_t drain(std::vector<std::uint8_t> &out, std::size_t capacity) {
    out.clear();
    std::size_t count = 0;
    std::lock_guard<std::mutex> lk(g_lock);
    while (!g_queue.empty()) {
        const Event &ev = g_queue.front();
        const std::size_t need = record_size(ev);
        if (need == 0) { g_queue.pop_front(); continue; }
        if (out.size() + need > capacity) break; /* leave it queued for next frame */
        encode(out, ev);
        g_queue.pop_front();
        ++count;
    }
    return count;
}

}  // namespace input
}  // namespace pvz2native
