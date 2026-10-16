/* com.popcap.SexyAppFramework.GooglePlayPurchaseDriver -- emulated in-app store.
 *
 * On a device this class drives Google Play Billing (IabHelper): the engine
 * calls RequestPayment(sku), a purchase UI appears, and when it finishes Java
 * calls the engine's native FirePaymentComplete back with the receipt. There is
 * no Play Billing here, so this port SHORT-CIRCUITS the flow: with
 * [game] emulate_iap on, the store reports itself available and every purchase
 * the game asks for is granted for free by calling FirePaymentComplete
 * ourselves. Default is off (gem store does not need emulated IAP).
 *
 * Two things make this correct rather than a hack:
 *
 *  - The native callbacks are reached the real way. RegisterNatives recorded the
 *    engine's FirePaymentComplete / FireDidRefresh addresses (see
 *    jni_env.cpp / find_native_method); we invoke those exact functions with the
 *    driver's own native pointer, captured at construction, so the engine's
 *    purchase manager processes the result through its normal path.
 *
 *  - The completion is DEFERRED, not fired from inside RequestPayment. On a
 *    device it arrives asynchronously (runOnUiThread -> billing -> callback), and
 *    the engine's C++ expects that: calling FirePaymentComplete reentrantly while
 *    the engine is still inside its RequestPayment call could corrupt the state
 *    it sets up around that call. So a request is queued here and delivered by
 *    iap_deliver_pending() once per frame, between top-level guest calls.
 *
 * The receipt fields are fabricated to look like a real Google Play purchase
 * (productId = the sku, purchaseState = 0). If a future engine build verifies
 * them more strictly, this is the one place to adjust -- every value is logged on
 * delivery.
 */

#include <pvz2native/dex/dex.h>

#include <pvz2native/config.h>
#include <pvz2native/log.h>
#include <pvz2native/dependencies/dependency.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace pvz2native {
namespace dex {
namespace {

constexpr const char *kClass = "com/popcap/SexyAppFramework/GooglePlayPurchaseDriver";
constexpr const char *kSkuClass = "com/popcap/SexyAppFramework/purchase/SkuDetails";

/* Google Play's purchaseState for a completed purchase. */
constexpr int kPurchaseStatePurchased = 0;

/* One deferred action the engine is owed. */
struct Pending {
    enum Kind { Complete, Refresh } kind;
    std::string sku;                /* the product for a Complete */
    std::vector<std::string> skus;  /* the products queried for a Refresh */
};

/* Everything the emulated store remembers. Guarded because the frame thread
 * delivers (iap_deliver_pending) while the guest thread enqueues (the hooks). */
struct Store {
    std::mutex lock;
    std::uint32_t driver_thiz = 0; /* the GooglePlayPurchaseDriver jobject */
    std::uint32_t native_ptr = 0;  /* mNativeDriverPtr, low word (guest is 32-bit) */
    bool native_ptr_known = false;
    std::vector<Pending> pending;

    /* Every product the engine has ever asked about, in first-asked order. The
     * engine replaces its price table with each FireDidRefresh, so a refresh
     * must always report the whole catalogue or the products left out lose
     * their price (the shop falls back to a bare "Buy"). */
    std::vector<std::string> catalogue;
    std::set<std::string> catalogue_set;

    /* SkuDetails jobject -> its product id, so the getSku hook can answer for the
     * specific object the engine is asking about. Separate lock: it is read by
     * the SkuDetails getters (which run while FireDidRefresh executes) and would
     * otherwise contend with the delivery holding `lock`. */
    std::mutex sku_lock;
    std::map<std::uint32_t, std::string> sku_by_obj;
    std::map<std::string, std::uint32_t> obj_by_sku; /* reuse one object per product across refreshes */
};
Store &store() {
    static Store s;
    return s;
}

bool iap_on() { return pvz2_config()->emulate_iap != 0; }

/* --- fabricated receipt --------------------------------------------------- */

std::string json_escape(const std::string &s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

/* The purchase-data JSON Google Play would have signed. The engine's native side
 * parses productId and purchaseToken out of this. */
std::string purchase_json(const std::string &sku) {
    const std::string e = json_escape(sku);
    return std::string("{\"orderId\":\"PVZ2NATIVE.") + e + "\",\"packageName\":\"" +
           pvz2_config()->package_name + "\",\"productId\":\"" + e + "\",\"purchaseTime\":0,\"purchaseState\":" +
           std::to_string(kPurchaseStatePurchased) + ",\"purchaseToken\":\"pvz2native." + e +
           "\",\"developerPayload\":\"\"}";
}

/* URL-safe base64 without padding -- what GooglePlayPurchaseDriver builds via
 * Base64.encodeWebSafe(bytes, false) for the second callback argument. */
std::string base64url(const std::string &in) {
    static const char *tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    std::size_t i = 0;
    for (; i + 3 <= in.size(); i += 3) {
        const std::uint32_t n = (std::uint8_t)in[i] << 16 | (std::uint8_t)in[i + 1] << 8 |
                                (std::uint8_t)in[i + 2];
        out += tbl[(n >> 18) & 63];
        out += tbl[(n >> 12) & 63];
        out += tbl[(n >> 6) & 63];
        out += tbl[n & 63];
    }
    const std::size_t rem = in.size() - i;
    if (rem == 1) {
        const std::uint32_t n = (std::uint32_t)(std::uint8_t)in[i] << 16;
        out += tbl[(n >> 18) & 63];
        out += tbl[(n >> 12) & 63];
    } else if (rem == 2) {
        const std::uint32_t n = (std::uint8_t)in[i] << 16 | (std::uint8_t)in[i + 1] << 8;
        out += tbl[(n >> 18) & 63];
        out += tbl[(n >> 12) & 63];
        out += tbl[(n >> 6) & 63];
    }
    return out;
}

/* --- guest-memory helpers (delivery runs between frames, with img/rt) ------ */

/* Copies a host string into freshly allocated guest memory as a NUL-terminated
 * UTF-8 payload -- which is exactly what a jstring is in this port (its bytes ARE
 * its address). Returns 0 if the heap is exhausted. Never freed: the engine may
 * hold the pointer (NewGlobalRef is identity here), the same reason ret_string
 * leaks its buffers. */
std::uint32_t alloc_gstr(pvz2_elf_image_t *img, GuestRuntime *rt, const std::string &s) {
    const std::uint32_t addr = rt->heap.alloc((std::uint32_t)s.size() + 1);
    if (addr == 0 || (std::uint64_t)addr + s.size() + 1 > img->mem_size) return 0;
    std::memcpy(&img->mem[addr], s.data(), s.size());
    img->mem[addr + s.size()] = 0;
    return addr;
}

void deliver_complete(pvz2_elf_image_t *img, GuestRuntime *rt, std::uint32_t thiz,
                      std::uint32_t native_ptr, const std::string &sku) {
    const std::uint32_t fn = find_native_method(kClass, "FirePaymentComplete");
    if (fn == 0) {
        pvz2_log("[iap] cannot grant \"%s\": FirePaymentComplete native not registered",
                    sku.c_str());
        return;
    }

    const std::string original_json = purchase_json(sku);
    const std::string signature; /* Play would sign this; the engine's Java verifier is gone */
    const std::string wrapper =
        std::string("{\"signedData\":") + "\"" + json_escape(original_json) + "\",\"signature\":\"" +
        json_escape(signature) + "\"}";
    const std::string receipt_id = "pvz2native." + sku;
    const std::string order_id = "PVZ2NATIVE." + sku;
    const std::string base64 = base64url(wrapper);

    const std::uint32_t g_receipt = alloc_gstr(img, rt, receipt_id);
    const std::uint32_t g_base64 = alloc_gstr(img, rt, base64);
    const std::uint32_t g_sku = alloc_gstr(img, rt, sku);
    const std::uint32_t g_json = alloc_gstr(img, rt, original_json);
    const std::uint32_t g_sig = alloc_gstr(img, rt, signature);
    const std::uint32_t g_order = alloc_gstr(img, rt, order_id);
    if (g_receipt == 0 || g_base64 == 0 || g_sku == 0 || g_json == 0 || g_sig == 0 || g_order == 0) {
        pvz2_log("[iap] cannot grant \"%s\": guest heap exhausted building the receipt",
                    sku.c_str());
        return;
    }

    /* void FirePaymentComplete(JNIEnv*, jobject, jlong, jstring receiptId,
     *   jstring base64, jstring sku, jstring originalJson, jstring signature,
     *   jstring orderId, jboolean originalPurchase).
     * AAPCS: env=r0, thiz=r1, the jlong takes the r2:r3 pair (r2 is even), the
     * six jstrings and the jboolean spill onto the stack. */
    const std::uint32_t args[] = {
        kJniEnvPtrAddr, thiz, native_ptr, 0u, g_receipt, g_base64,
        g_sku,          g_json, g_sig,    g_order, 1u /* originalPurchase */,
    };
    pvz2_log("[iap] granting \"%s\" -> FirePaymentComplete(fn=0x%08x, ptr=0x%08x)",
                sku.c_str(), fn, native_ptr);
    call_guest_between_frames_n(fn, args, (int)(sizeof(args) / sizeof(args[0])));
}

void deliver_refresh(pvz2_elf_image_t *img, GuestRuntime *rt, std::uint32_t thiz,
                     std::uint32_t native_ptr, const std::vector<std::string> &skus) {
    const std::uint32_t fn = find_native_method(kClass, "FireDidRefresh");
    if (fn == 0) {
        pvz2_log("[iap] FireDidRefresh native not registered -- refresh dropped");
        return;
    }

    /* void FireDidRefresh(JNIEnv*, jobject, jlong, SkuDetails[]).
     *
     * A NON-null array is what tells the engine the store query SUCCEEDED; the
     * real driver only passes null when it FAILED, and that is exactly what made
     * every purchase report "Service Unavailable". So build a SkuDetails[] with
     * one tagged object per queried product. Each object's product id is
     * remembered for the getSku hook; the other fields are constants. */
    std::uint32_t n = (std::uint32_t)skus.size();
    const std::uint32_t bytes = n > 0 ? n * 4u : 4u; /* a 0-length array still needs a valid handle */
    const std::uint32_t arr = rt->heap.alloc(bytes);
    if (arr == 0 || (std::uint64_t)arr + bytes > img->mem_size) {
        pvz2_log("[iap] cannot build the SkuDetails array -- heap exhausted");
        return;
    }

    /* One object per product. They must NOT come from rt->new_object: its handle
     * pool (guest_memmap.h, 512 handles shared with every class/method id) is
     * already ~60% used at boot, and once it runs out every further handle
     * aliases the first one. With 400+ products the ones past the ~300th
     * (alphabetically: gems, sprouts, plants) all collapsed into one object, so
     * the engine never saw their price and the shop printed a bare "Buy". A
     * jobject here is just a distinct non-null word tagged with its class, so a
     * 4-byte heap block does the job without touching the pool. */
    {
        std::lock_guard<std::mutex> lk(store().sku_lock);
        for (std::uint32_t i = 0; i < n; ++i) {
            auto it = store().obj_by_sku.find(skus[i]);
            std::uint32_t obj;
            if (it != store().obj_by_sku.end()) {
                obj = it->second;
            } else {
                obj = rt->heap.alloc(4);
                if (obj == 0 || (std::uint64_t)obj + 4 > img->mem_size) {
                    pvz2_log("[iap] heap exhausted creating SkuDetails #%u -- the rest get no price", i);
                    n = i;
                    break;
                }
                rt->tag_object_class(obj, kSkuClass);
                store().sku_by_obj[obj] = skus[i];
                store().obj_by_sku[skus[i]] = obj;
            }
            std::memcpy(&img->mem[arr + i * 4], &obj, 4);
        }
    }
    set_array_length(arr, n); /* GetArrayLength must report n, not n*4 -- see jni_env */

    const std::uint32_t args[] = {kJniEnvPtrAddr, thiz, native_ptr, 0u, arr};
    pvz2_log("[iap] FireDidRefresh(fn=0x%08x, ptr=0x%08x, %u sku(s))", fn, native_ptr, n);
    call_guest_between_frames_n(fn, args, (int)(sizeof(args) / sizeof(args[0])));
}

/* --- SkuDetails ----------------------------------------------------------- */

/* Reads a Java String[] the engine handed us: each element is a jstring, which
 * in this port IS a pointer to its UTF-8 bytes. The array is one the engine
 * built via NewObjectArray, so its byte size is 4 per element. Capped so a
 * garbage handle cannot spin this into a huge loop; the cap must stay well above
 * the number of products in MAGENTO.RTON (850+ with the modded shop) -- products
 * past it get no price. */
std::vector<std::string> read_string_array(DexCall &d, std::uint32_t arr) {
    std::vector<std::string> out;
    if (arr == 0) return out;
    std::uint32_t n = d.c.rt->heap.size_of(arr) / 4u;
    if (n > 8192) {
        pvz2_log("[iap] Refresh array reports %u entries -- reading only the first 8192", n);
        n = 8192;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::uint32_t p = d.c.read32(arr + i * 4u);
        if (p != 0) out.push_back(d.c.cstr(p, 256));
    }
    return out;
}

std::string sku_of(std::uint32_t obj) {
    std::lock_guard<std::mutex> lk(store().sku_lock);
    auto it = store().sku_by_obj.find(obj);
    return it == store().sku_by_obj.end() ? std::string() : it->second;
}

/* The getters the engine calls on each SkuDetails while it reads the refresh
 * result. getSku is per-object; the rest are constants that are good enough for
 * an emulated free store. None may return null -- the engine feeds these into
 * std::string, which aborts on null. */
void sku_get_sku(DexCall &d) { d.ret_string(sku_of(d.thiz)); }
void sku_get_type(DexCall &d) { d.ret_string("inapp"); }
/* Free, but in the configured currency -- see [game] currency_symbol. */
void sku_get_price(DexCall &d) { d.ret_string(std::string(pvz2_config()->currency_symbol) + "0.00"); }
void sku_get_title(DexCall &d) { d.ret_string(sku_of(d.thiz)); }
void sku_get_description(DexCall &d) { d.ret_string(""); }

/* --- hooks ---------------------------------------------------------------- */

/* GooglePlayPurchaseDriver(long nativeDriverPtr, String publicKey). Captures the
 * object and its native pointer, which FirePaymentComplete needs as its first
 * argument. The jlong occupies two va_list words (low, then high); the guest is
 * 32-bit so the high word is 0 and the low word is the pointer. */
void ctor(DexCall &d) {
    Store &s = store();
    std::lock_guard<std::mutex> lk(s.lock);
    s.driver_thiz = d.thiz;
    if (d.va != 0) {
        s.native_ptr = d.arg(0);
        s.native_ptr_known = true;
        d.c.log("[iap] driver constructed: thiz=0x%08x nativePtr=0x%08x (hi=0x%08x)", d.thiz,
                d.arg(0), d.arg(1));
    } else {
        /* Plain NewObject gave no va_list, so the ctor arguments are unavailable
         * here -- delivery will refuse rather than call a native with a null
         * pointer. Loud because it disables the whole feature. */
        s.native_ptr_known = false;
        d.c.log("[iap] driver constructed WITHOUT a va_list: native pointer unknown, purchases "
                "cannot be granted (thiz=0x%08x)", d.thiz);
    }
    d.ret(0);
}

void can_make_payments(DexCall &d) {
    const bool ok = iap_on();
    d.c.log("[iap] CanMakePayments -> %s", ok ? "true" : "false");
    d.ret_bool(ok);
}

/* ProductTypeIsSupported(int prodType): 0=Consumable, 1=Restorable, 2=Subscription.
 * Everything but subscriptions is supported when the store is on. */
void product_type_is_supported(DexCall &d) {
    const bool ok = iap_on() && d.arg(0) != 2 /* Subscription */;
    d.c.log("[iap] ProductTypeIsSupported(%u) -> %s", d.arg(0), ok ? "true" : "false");
    d.ret_bool(ok);
}

void request_payment(DexCall &d) {
    const std::string sku = d.string_arg(0);
    if (!iap_on()) {
        d.c.log("[iap] RequestPayment(\"%s\") ignored -- emulate_iap is off", sku.c_str());
        d.ret(0);
        return;
    }
    Store &s = store();
    std::lock_guard<std::mutex> lk(s.lock);
    s.pending.push_back({Pending::Complete, sku});
    d.c.log("[iap] RequestPayment(\"%s\") queued for grant", sku.c_str());
    d.ret(0);
}

void refresh(DexCall &d) {
    if (!iap_on()) {
        d.ret(0);
        return;
    }
    /* Refresh(String[] skus) -- capture the products so the deferred
     * FireDidRefresh can report a real catalogue for exactly these. */
    const std::vector<std::string> asked = read_string_array(d, d.arg(0));
    Store &s = store();
    std::lock_guard<std::mutex> lk(s.lock);
    for (const std::string &k : asked) {
        if (s.catalogue_set.insert(k).second) s.catalogue.push_back(k);
    }
    s.pending.push_back({Pending::Refresh, std::string(), s.catalogue});
    d.c.log("[iap] Refresh(%zu sku(s)) queued; reporting the whole catalogue (%zu)", asked.size(),
            s.catalogue.size());
    d.ret(0);
}

/* Fire-and-forget acknowledgements; nothing to do without a real store --
 * hook_ignored is the shared stub from dex.h. */
void has_unconfirmed_payments(DexCall &d) { d.ret_bool(false); }
void on_activity_result(DexCall &d) { d.ret_bool(false); }

}  // namespace

void iap_deliver_pending(pvz2_elf_image_t *img, GuestRuntime *rt) {
    Store &s = store();

    std::vector<Pending> due;
    std::uint32_t thiz, native_ptr;
    bool ptr_known;
    {
        std::lock_guard<std::mutex> lk(s.lock);
        if (s.pending.empty()) return;
        due.swap(s.pending);
        thiz = s.driver_thiz;
        native_ptr = s.native_ptr;
        ptr_known = s.native_ptr_known;
    }

    if (!ptr_known || thiz == 0) {
        pvz2_log("[iap] %zu action(s) pending but the driver/native pointer is unknown -- "
                    "dropping (see the ctor log)", due.size());
        return;
    }

    for (const Pending &p : due) {
        if (p.kind == Pending::Complete) {
            deliver_complete(img, rt, thiz, native_ptr, p.sku);
        } else {
            deliver_refresh(img, rt, thiz, native_ptr, p.skus);
        }
    }
}

void register_purchase_driver(HookTable &t) {
    t.add(kClass, "<init>", ctor);
    t.add(kClass, "CanMakePayments", can_make_payments);
    t.add(kClass, "ProductTypeIsSupported", product_type_is_supported);
    t.add(kClass, "RequestPayment", request_payment);
    t.add(kClass, "Refresh", refresh);
    t.add(kClass, "ConfirmDelivery", hook_ignored);
    t.add(kClass, "HasUnconfirmedPayments", has_unconfirmed_payments);
    t.add(kClass, "Close", hook_ignored);
    t.add(kClass, "onActivityResult", on_activity_result);

    /* The SkuDetails the engine reads out of the FireDidRefresh catalogue. */
    t.add(kSkuClass, "getSku", sku_get_sku);
    t.add(kSkuClass, "getType", sku_get_type);
    t.add(kSkuClass, "getPrice", sku_get_price);
    t.add(kSkuClass, "getTitle", sku_get_title);
    t.add(kSkuClass, "getDescription", sku_get_description);
}

}  // namespace dex
}  // namespace pvz2native
