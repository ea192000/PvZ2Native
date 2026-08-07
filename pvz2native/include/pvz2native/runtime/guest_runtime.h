#ifndef PVZ2NATIVE_RUNTIME_GUEST_RUNTIME_H
#define PVZ2NATIVE_RUNTIME_GUEST_RUNTIME_H

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#include <dynarmic/interface/exclusive_monitor.h>

#include <pvz2native/elf32/elf32_loader.h>
#include <pvz2native/runtime/guest_heap.h>
#include <pvz2native/runtime/guest_memmap.h>
#include <pvz2native/runtime/guest_sync.h>
#include <pvz2native/runtime/guest_zlib.h>
#include <pvz2native/runtime/rsb_index.h>

namespace pvz2native {

/* processor_id 0 is used by every synchronous run_export()/run_at_offset()
 * call (they never run concurrently with each other); spawned guest threads
 * get ids 2..kThreadStackMax+1 (see GuestRuntime::next_thread_id), so the
 * monitor needs a slot for each of those too. */
constexpr uint32_t kThreadStackMax = 8;
constexpr size_t kMonitorProcessorCount = kThreadStackMax + 2;

/* Bump allocator for opaque JNI "handles" (jclass/jmethodID/jfieldID/...):
 * the guest never dereferences these as real memory, only passes them back
 * into other JNI calls, so a unique non-null address is all that's needed.
 * Carved out of the gap between the trampoline table and the JNI vtable --
 * runtime/guest_memmap.h owns that layout and checks the fit. */
using runtime::memmap::kFakeHandleBase;
using runtime::memmap::kFakeHandleEnd;

/* Guest FILE* tokens: opaque cookies the guest only ever hands back to
 * stdio calls (never dereferences), kept well outside the guest address
 * space. Guest fd tokens: small + positive so guest `fd < 0` failure
 * checks keep working. Both index host tables in GuestRuntime. */
constexpr uint32_t kFileTokenBase = 0xF1000000;
constexpr uint32_t kFdTokenBase = 0x00004000;

/* State shared by every guest "thread" (each is its own host std::thread
 * driving its own dynarmic Jit instance over the same emulated address
 * space -- see spawn_guest_thread below). Access to any of these tables
 * must go through their respective lock; the emulated memory buffer itself
 * is not otherwise synchronized between guest threads (fine for this
 * bring-up stage -- real guest memory ordering isn't something we need to
 * model yet). */
struct GuestRuntime {
    pvz2_elf_image_t *img = nullptr;
    GuestHeap heap;

    /* Real ARM code -- not just our own SVC-trampoline imports -- can
     * execute LDREX/STREX directly (e.g. libstdc++ atomics/refcounting on
     * ARMv7). Without a configured monitor, dynarmic hits an internal
     * `assert(conf.global_monitor != nullptr)` and calls std::terminate()
     * the moment such an instruction runs -- discovered by hitting exactly
     * that crash once real engine code (not just infra JNI stubs) started
     * executing. One monitor is shared by every Jit instance touching this
     * image, each with its own processor_id (see kMonitorProcessorCount). */
    Dynarmic::ExclusiveMonitor monitor{kMonitorProcessorCount};

    std::mutex handles_lock;
    uint32_t next_fake_handle = kFakeHandleBase;

    std::mutex mutexes_lock;
    std::unordered_map<uint32_t, std::unique_ptr<GuestMutex>> guest_mutexes;

    std::mutex conds_lock;
    std::unordered_map<uint32_t, std::unique_ptr<GuestCond>> guest_conds;

    std::mutex sems_lock;
    std::unordered_map<uint32_t, std::unique_ptr<GuestSem>> guest_sems;

    std::mutex rwlocks_lock;
    std::unordered_map<uint32_t, std::unique_ptr<GuestRwLock>> guest_rwlocks;

    std::mutex once_lock;
    std::unordered_map<uint32_t, bool> once_done;

    std::mutex threads_lock;
    std::unordered_map<uint32_t, std::thread> threads;
    std::unordered_map<uint32_t, uint32_t> thread_retvals;
    uint32_t next_thread_id = 2; /* 1 is reserved for the initial/"main" thread */
    uint32_t next_stack_slot = 0;

    std::atomic<uint32_t> next_tls_key{1};
    std::mutex log_lock;

    /* Real class/method identity for the fake JNI object model (see
     * pvz2native/gles or doc section 9.16): ground-truthed against the
     * decompiled Java in classesdex rather than handing out a fresh opaque
     * handle per call. This lets FindClass/GetObjectClass return the SAME
     * jclass for the same class name every time, GetMethodID return the
     * SAME jmethodID for the same (class, method) every time, and lets
     * CallXxxMethod actually know which real method is being invoked (via
     * jni_dispatch_table below) instead of silently returning 0 for
     * everything the engine calls back into. */
    std::mutex jni_meta_lock;
    std::unordered_map<std::string, uint32_t> class_handle_by_name;
    std::unordered_map<uint32_t, std::string> class_name_by_handle;
    std::map<std::pair<uint32_t, std::string>, uint32_t> method_handle_by_key;
    std::unordered_map<uint32_t, std::pair<uint32_t, std::string>> method_key_by_handle;
    std::unordered_map<uint32_t, std::string> object_class_name;

    /* Host-backed file I/O for the real resource loader (doc 9.22). Shared
     * across guest threads, so guarded by files_lock. host_files maps a guest
     * FILE* token -> real host FILE*; host_fds maps a guest fd token -> real
     * host fd. */
    std::mutex files_lock;
    std::unordered_map<uint32_t, FILE *> host_files;
    std::unordered_map<uint32_t, int> host_fds;
    uint32_t next_file_token = kFileTokenBase;
    uint32_t next_fd_token = kFdTokenBase;

    /* Set during session teardown so guest threads blocked in sem_wait /
     * cond_wait can wake and unwind before their Jit is destroyed. */
    std::atomic<bool> shutting_down{false};

    /* Set the moment translate_path() redirects a guest "main.rsb" path to
     * the real .obb -- the observable that tells the frame loop the engine
     * reached its own RSB load path (doc 9.23). */
    std::atomic<bool> rsb_touched{false};

    /* Index of the .obb's contents, used to answer the engine's
     * Resources_GetAssetFileInfo JNI call with a real (path, offset, length)
     * triple. Built on first use -- parsing 588 RSG headers costs a few ms,
     * and nothing before the RSB load needs it. */
    std::mutex rsb_index_lock;
    RsbIndex rsb_index;
    bool rsb_index_tried = false;

    /* Host zlib behind the guest's inflate/deflate imports -- the engine
     * decompresses RSG payloads (and every .PTX texture) through them. */
    GuestZlib zlib;

    uint32_t alloc_fake_handle();

    uint32_t find_or_alloc_class(const std::string &name);
    uint32_t find_or_alloc_method(uint32_t class_handle, const std::string &name);
    void tag_object_class(uint32_t obj_addr, const std::string &class_name);
    std::string class_name_of_object(uint32_t obj_addr);
    std::string class_name_of_handle(uint32_t class_handle);

    /* JNI NewObject/AllocObject: a distinct non-null handle tagged with its
     * class. Returning 0 here (which is what the unimplemented default used to
     * do) hands the engine a null jobject it then stores and cannot call. */
    uint32_t new_object(uint32_t class_handle);

    /* Returns false if method_id isn't one of ours (e.g. exhausted the fake
     * handle range and aliased to kFakeHandleBase). */
    bool lookup_method(uint32_t method_id, std::string &out_class_name, std::string &out_method_name);

    GuestMutex *get_or_create_mutex(uint32_t addr);
    GuestCond *get_or_create_cond(uint32_t addr);
    GuestSem *get_or_create_sem(uint32_t addr);
    GuestRwLock *get_or_create_rwlock(uint32_t addr);
};

}  // namespace pvz2native

#endif
