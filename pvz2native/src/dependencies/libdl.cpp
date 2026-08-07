/* libdl.so -- the dynamic loader interface.
 *
 * libPVZ2.so lists libdl in DT_NEEDED (every Android .so does) but this build
 * imports nothing from it. The module exists anyway, and answers honestly, so
 * that a differently-built version of the game -- one that dlopen()s an
 * optional plugin, or resolves a symbol lazily -- gets a clear "not found"
 * instead of a silent zero from the fallthrough path.
 *
 * There is nothing to load: our address space contains exactly one shared
 * object, mapped by src/elf32/. Reporting failure is the truth, and dlerror()
 * says so in words.
 */

#include <pvz2native/dependencies/dependency.h>

#include <string>

namespace pvz2native {
namespace {

void dl_open(GuestCall &c) {
    std::string name = c.arg(0) == 0 ? std::string("(self)") : c.cstr(c.arg(0), 512);
    c.log("[libdl] dlopen(\"%s\") -- only libPVZ2.so is mapped, returning NULL", name.c_str());
    c.set_result(0);
}

void dl_sym(GuestCall &c) {
    /* Could be served from the image's own .dynsym, but a caller that reaches
     * here already holds a handle we never issued. */
    c.log("[libdl] dlsym(\"%s\") on an unknown handle", c.cstr(c.arg(1), 256).c_str());
    c.set_result(0);
}

void dl_close(GuestCall &c) { c.set_result(0); }

void dl_error(GuestCall &c) {
    static std::uint32_t slot = 0;
    if (slot == 0) slot = c.dup_cstr("dynamic loading is not available in PvZ2");
    c.set_result(slot);
}

void dl_addr(GuestCall &c) { c.set_result(0); } /* 0 = failure, per dladdr(3) */

}  // namespace

void register_libdl(ImportTable &t) {
    t.add("dlopen", dl_open);
    t.add("dlsym", dl_sym);
    t.add("dlclose", dl_close);
    t.add("dlerror", dl_error);
    t.add("dladdr", dl_addr);
}

}  // namespace pvz2native
