/* SDL audio sink for the emulated OpenSL buffer-queue player -- see
 * include/pvz2native/audio/audio_output.h for the threading contract.
 *
 * Deliberately dumb: a FIFO of host-owned PCM blocks, an SDL callback that
 * drains it, and a counter of finished blocks for the main thread to pick up.
 * All the OpenSL semantics live in opensles.cpp; this file knows only about
 * bytes and the device.
 */

#include <pvz2native/audio/audio_output.h>
#include <pvz2native/log.h>

#include <SDL.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

namespace pvz2native {
namespace audio {
namespace {

std::mutex g_lock;                        /* guards everything below */
SDL_AudioDeviceID g_device = 0;
std::deque<std::vector<std::uint8_t>> g_queue;
std::size_t g_head_offset = 0;            /* bytes of g_queue.front() already played */
std::uint32_t g_completed = 0;            /* finished buffers not yet reported to the guest */
bool g_playing = false;

/* Wakes the guest thread parked in wait_for_completion(). Signalled from
 * whichever thread produced the completion -- SDL's audio thread when a buffer
 * finished playing, or the caller of clear()/shutdown(). */
std::condition_variable g_completion_cv;
bool g_shutdown = false;

/* Current format, so configure() can skip a reopen when nothing changed. */
int g_channels = 0, g_rate = 0, g_bits = 0;

/* Health counters -- see audio::diag(). Atomic so the callback thread and the
 * main-thread reader need no shared lock. */
std::atomic<std::uint64_t> g_underruns{0};
std::atomic<std::uint64_t> g_enqueue_fails{0};

/* A runaway producer must not grow this without bound. The game keeps only a
 * handful of buffers in flight; far more than that means something is wrong. */
constexpr std::size_t kMaxQueued = 64;

/* Pulls `len` bytes out of the FIFO. Runs on SDL's audio thread and touches
 * host memory only. Silence-fills whatever it cannot satisfy, which is what an
 * underrun should sound like rather than repeating the last block. */
void SDLCALL audio_callback(void * /*user*/, Uint8 *stream, int len) {
    std::unique_lock<std::mutex> lk(g_lock);
    const std::uint32_t completed_before = g_completed;
    int written = 0;
    while (written < len && g_playing && !g_queue.empty()) {
        std::vector<std::uint8_t> &front = g_queue.front();
        const std::size_t avail = front.size() - g_head_offset;
        const std::size_t want = (std::size_t)(len - written);
        const std::size_t n = avail < want ? avail : want;
        std::memcpy(stream + written, front.data() + g_head_offset, n);
        written += (int)n;
        g_head_offset += n;
        if (g_head_offset >= front.size()) { /* this buffer has fully played */
            g_queue.pop_front();
            g_head_offset = 0;
            ++g_completed;
        }
    }
    if (written < len) {
        std::memset(stream + written, 0, (std::size_t)(len - written));
        /* Only a real starvation counts: silence while stopped/paused is expected
         * and not a fault. */
        if (g_playing) {
            g_underruns.fetch_add(1, std::memory_order_relaxed);
            /* Keep the completion loop alive across the underrun. bq_pump_thread
             * runs ONLY when a completion is produced, and it is the sole place
             * Wwise re-submits audio (CAkSinkOpenSL enqueues from inside that
             * callback). If a starved callback produced no completion, the pump
             * would never wake again: no buffer plays, so nothing completes, so
             * nothing refills -- a permanent stall. Wwise then backs its command
             * queue up until a caller blocks on it ("audio command queue is
             * full", the hang heard as the last DMA buffer looping). Synthesising
             * one completion per starved callback keeps Wwise prompted at the
             * buffer rate -- exactly what a real OpenSL callback does as each
             * queue slot frees -- so it re-primes and recovers instead of dying.
             * Only when this callback produced no real completion, so a busy
             * stream is never double-counted. */
            if (g_completed == completed_before) ++g_completed;
        }
    }
    /* Notified with the lock dropped: the waiter is a guest thread that will
     * immediately want g_lock back, and this is SDL's audio thread, which must
     * return in time for the next block. */
    const bool produced = g_completed != completed_before;
    lk.unlock();
    if (produced) g_completion_cv.notify_one();
}

/* Closes the device with the lock NOT held -- SDL_CloseAudioDevice waits for
 * the callback to finish, and the callback takes g_lock. */
void close_device_locked(SDL_AudioDeviceID dev) {
    if (dev != 0) SDL_CloseAudioDevice(dev);
}

}  // namespace

bool configure(int channels, int sample_rate_hz, int bits_per_sample) {
    SDL_AudioDeviceID old = 0;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        if (g_device != 0 && channels == g_channels && sample_rate_hz == g_rate &&
            bits_per_sample == g_bits) {
            return true; /* already open in exactly this format */
        }
        old = g_device;
        g_device = 0;
        g_queue.clear();
        g_head_offset = 0;
        g_completed = 0;
    }
    close_device_locked(old);

    if (SDL_WasInit(SDL_INIT_AUDIO) == 0 && SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        pvz2_log("[audio] SDL_InitSubSystem(AUDIO) failed: %s", SDL_GetError());
        return false;
    }

    SDL_AudioSpec want{};
    want.freq = sample_rate_hz;
    want.format = (bits_per_sample == 8) ? AUDIO_U8 : AUDIO_S16SYS;
    want.channels = (Uint8)channels;
    /* Small enough to keep latency low, large enough that the per-callback cost
     * stays negligible at the rates the game uses. */
    want.samples = 1024;
    want.callback = audio_callback;

    SDL_AudioSpec have{};
    /* No format changes allowed: the game hands us PCM in exactly one layout,
     * and letting SDL silently convert channels or width would misinterpret it. */
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (dev == 0) {
        pvz2_log("[audio] SDL_OpenAudioDevice(%d Hz, %d ch, %d bit) failed: %s",
                 sample_rate_hz, channels, bits_per_sample, SDL_GetError());
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(g_lock);
        g_device = dev;
        g_channels = channels;
        g_rate = sample_rate_hz;
        g_bits = bits_per_sample;
    }
    SDL_PauseAudioDevice(dev, g_playing ? 0 : 1);
    pvz2_log("[audio] device open -- %d Hz, %d ch, %d bit", sample_rate_hz, channels,
             bits_per_sample);
    return true;
}

void set_playing(bool playing) {
    SDL_AudioDeviceID dev;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        g_playing = playing;
        dev = g_device;
    }
    if (dev != 0) SDL_PauseAudioDevice(dev, playing ? 0 : 1);
}

bool enqueue(const void *pcm, std::size_t bytes) {
    if (pcm == nullptr || bytes == 0) return false;
    std::lock_guard<std::mutex> lk(g_lock);
    if (g_device == 0 || g_queue.size() >= kMaxQueued) {
        g_enqueue_fails.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const std::uint8_t *p = static_cast<const std::uint8_t *>(pcm);
    g_queue.emplace_back(p, p + bytes);
    return true;
}

void diag(std::uint64_t &underruns, std::uint64_t &enqueue_fails) {
    underruns = g_underruns.load(std::memory_order_relaxed);
    enqueue_fails = g_enqueue_fails.load(std::memory_order_relaxed);
}

void clear() {
    {
        std::lock_guard<std::mutex> lk(g_lock);
        /* Everything dropped still owes the guest a completion, or it will wait
         * forever for buffers it will never hear about again. */
        g_completed += (std::uint32_t)g_queue.size();
        g_queue.clear();
        g_head_offset = 0;
    }
    g_completion_cv.notify_all();
}

std::uint32_t queued_count() {
    std::lock_guard<std::mutex> lk(g_lock);
    return (std::uint32_t)g_queue.size();
}

bool take_completion() {
    std::lock_guard<std::mutex> lk(g_lock);
    if (g_completed == 0) return false;
    --g_completed;
    return true;
}

bool wait_for_completion() {
    std::unique_lock<std::mutex> lk(g_lock);
    /* Wait for a completion, but only up to a short timeout -- NOT forever.
     *
     * On a real completion, consume it and return true (the caller refills the
     * queue). On TIMEOUT with no completion, still return true, so the caller
     * ticks Wwise anyway. This is the fix for the intermittent "tid=7 stuck in
     * sem_wait" hang: Wwise's audio thread is woken only from inside its
     * buffer-queue callback, which the pump runs once per completion -- so ANY
     * gap in the SDL callback delivering completions (the device paused,
     * reconfigured, or momentarily CPU-starved) leaves that thread asleep, its
     * command queue backing up until it fills and the game hangs. A periodic
     * tick guarantees the callback runs regularly regardless. Wwise's own
     * "is data needed" check makes a tick a no-op when the buffer queue is
     * already full, so ticking on a timeout cannot overfill the queue. */
    g_completion_cv.wait_for(lk, std::chrono::milliseconds(15),
                             [] { return g_completed != 0 || g_shutdown; });
    if (g_shutdown) return false;
    if (g_completed != 0) --g_completed;
    return true; /* completion consumed, or timed out -- either way, tick Wwise */
}

void shutdown() {
    SDL_AudioDeviceID dev;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        dev = g_device;
        g_device = 0;
        g_queue.clear();
        g_head_offset = 0;
        g_completed = 0;
        g_playing = false;
        /* Released before the device closes: the callback thread is a guest
         * thread and must be allowed to unwind out of the JIT. */
        g_shutdown = true;
    }
    g_completion_cv.notify_all();
    close_device_locked(dev);
}

}  // namespace audio
}  // namespace pvz2native
