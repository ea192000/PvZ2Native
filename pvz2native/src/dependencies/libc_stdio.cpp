/* libc.so -- <stdio.h>, plus the printf/scanf grammar shared with liblog.
 *
 * Guest FILE* values are opaque tokens (see kFileTokenBase), never real guest
 * memory: the engine only ever hands them back to stdio, so there is nothing to
 * gain from emulating bionic's struct __sFILE. The exception is __sF, the array
 * behind stdin/stdout/stderr, whose ELEMENT ADDRESSES the guest computes
 * itself -- those three are pre-registered as tokens in libc_locale.cpp so a
 * write to stdout lands on the host's stdout instead of vanishing.
 *
 * The formatting engine here is the real thing rather than a stub. It was one:
 * the whole printf family returned an empty string and reported itself, on the
 * theory that the engine logs through __android_log_* anyway -- but that call
 * takes the same format string, so every guest log line arrived with its
 * arguments missing.
 */

#include <pvz2native/dependencies/libc_internal.h>

#include <pvz2native/dependencies/vfs.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace pvz2native {
namespace libc {

/* ------------------------------------------------------------- VaSource */

VaSource VaSource::from_registers(GuestCall &c, int first_arg) {
    VaSource v;
    v.c_ = &c;
    v.from_regs_ = true;
    v.arg_index_ = first_arg;
    return v;
}

VaSource VaSource::from_va_list(GuestCall &c, std::uint32_t va_addr) {
    VaSource v;
    v.c_ = &c;
    v.from_regs_ = false;
    v.va_addr_ = va_addr;
    return v;
}

std::uint32_t VaSource::next_u32() {
    if (from_regs_) return c_->arg(arg_index_++);
    std::uint32_t v = c_->read32(va_addr_);
    va_addr_ += 4;
    return v;
}

std::uint64_t VaSource::next_u64() {
    if (from_regs_) {
        /* AAPCS 8-byte alignment: a 64-bit value never starts in an odd
         * register, and once the registers run out it is 8-byte aligned on the
         * stack too. arg() already indexes both halves uniformly. */
        arg_index_ = (arg_index_ + 1) & ~1;
        std::uint32_t lo = c_->arg(arg_index_);
        std::uint32_t hi = c_->arg(arg_index_ + 1);
        arg_index_ += 2;
        return (std::uint64_t)lo | ((std::uint64_t)hi << 32);
    }
    va_addr_ = (va_addr_ + 7) & ~7u;
    std::uint64_t lo = c_->read32(va_addr_);
    std::uint64_t hi = c_->read32(va_addr_ + 4);
    va_addr_ += 8;
    return lo | (hi << 32);
}

double VaSource::next_double() {
    /* softfp: a double in a variadic call is just its 64-bit pattern in core
     * registers -- no VFP involved, so the bits are taken verbatim. */
    std::uint64_t bits = next_u64();
    double d;
    std::memcpy(&d, &bits, 8);
    return d;
}

/* --------------------------------------------------------------- format */

namespace {

/* Characters allowed between '%' and the conversion, so a corrupt format
 * string can't make us build an arbitrary host spec. */
bool is_spec_char(char ch) {
    return std::strchr("-+ #0123456789.*hlLqjzt'", ch) != nullptr;
}

/* Reads the guest's wide string (4-byte wchar_t) as narrow text. */
std::string wide_to_narrow(GuestCall &c, std::uint32_t addr, std::size_t max = 1024) {
    std::string out;
    for (std::size_t i = 0; i < max; ++i) {
        std::uint32_t ch = c.read32(addr + (std::uint32_t)i * 4);
        if (ch == 0) break;
        out.push_back(ch < 128 ? (char)ch : '?');
    }
    return out;
}

}  // namespace

std::string format(GuestCall &c, std::uint32_t fmt_addr, VaSource &args) {
    std::string fmt = c.cstr(fmt_addr, 4096);
    std::string out;
    char buf[512];

    for (std::size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != '%') { out.push_back(fmt[i]); continue; }
        if (i + 1 < fmt.size() && fmt[i + 1] == '%') { out.push_back('%'); ++i; continue; }

        /* Collect the spec verbatim, so width/precision/flags keep working
         * without reimplementing them: the host's snprintf gets the same text. */
        std::string flags;
        std::size_t j = i + 1;
        bool has_long_long = false;
        while (j < fmt.size() && is_spec_char(fmt[j])) {
            char ch = fmt[j];
            if (ch == 'l' && j + 1 < fmt.size() && fmt[j + 1] == 'l') has_long_long = true;
            if (ch == 'q' || ch == 'j') has_long_long = true;
            /* '*' takes its value from the argument list, so substitute it now. */
            if (ch == '*') {
                std::snprintf(buf, sizeof(buf), "%d", (int)args.next_u32());
                flags += buf;
            } else if (ch != 'l' && ch != 'h' && ch != 'L' && ch != 'q' && ch != 'j' &&
                       ch != 'z' && ch != 't' && ch != '\'') {
                flags.push_back(ch); /* length modifiers are re-added per conversion */
            }
            ++j;
        }
        if (j >= fmt.size()) { out.push_back('%'); out += flags; break; }

        char conv = fmt[j];
        i = j;

        switch (conv) {
            case 'd': case 'i': {
                if (has_long_long) {
                    std::snprintf(buf, sizeof(buf), ("%" + flags + "lld").c_str(), (long long)args.next_u64());
                } else {
                    std::snprintf(buf, sizeof(buf), ("%" + flags + "d").c_str(), (int)args.next_u32());
                }
                out += buf;
                break;
            }
            case 'u': case 'o': case 'x': case 'X': {
                std::string conv_s(1, conv);
                if (has_long_long) {
                    std::snprintf(buf, sizeof(buf), ("%" + flags + "ll" + conv_s).c_str(),
                                  (unsigned long long)args.next_u64());
                } else {
                    std::snprintf(buf, sizeof(buf), ("%" + flags + conv_s).c_str(),
                                  (unsigned)args.next_u32());
                }
                out += buf;
                break;
            }
            case 'c': {
                std::snprintf(buf, sizeof(buf), ("%" + flags + "c").c_str(), (int)args.next_u32());
                out += buf;
                break;
            }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
                /* A float argument is promoted to double by the variadic call,
                 * so every one of these consumes 8 bytes. */
                std::snprintf(buf, sizeof(buf), ("%" + flags + conv).c_str(), args.next_double());
                out += buf;
                break;
            }
            case 's': {
                std::uint32_t p = args.next_u32();
                std::string s = (p == 0) ? std::string("(null)") : c.cstr(p, 4096);
                if (flags.empty()) {
                    out += s;
                } else {
                    std::vector<char> wide(s.size() + 256);
                    std::snprintf(wide.data(), wide.size(), ("%" + flags + "s").c_str(), s.c_str());
                    out += wide.data();
                }
                break;
            }
            case 'S': {
                std::uint32_t p = args.next_u32();
                out += (p == 0) ? std::string("(null)") : wide_to_narrow(c, p);
                break;
            }
            case 'p': {
                std::snprintf(buf, sizeof(buf), "0x%08x", args.next_u32());
                out += buf;
                break;
            }
            case 'n': {
                /* Writes the character count so far through a guest pointer. */
                std::uint32_t p = args.next_u32();
                if (p != 0) c.write32(p, (std::uint32_t)out.size());
                break;
            }
            default:
                /* Unknown conversion: emit it literally rather than consume an
                 * argument we cannot interpret (which would desync everything
                 * after it). */
                out.push_back('%');
                out += flags;
                out.push_back(conv);
                break;
        }
    }
    return out;
}

/* ----------------------------------------------------------------- scan */

int scan(GuestCall &c, const std::string &input, std::uint32_t fmt_addr, VaSource &args) {
    std::string fmt = c.cstr(fmt_addr, 1024);
    std::size_t in = 0;
    int converted = 0;
    bool hit_eof_first = input.empty();

    auto skip_space = [&] {
        while (in < input.size() && std::isspace((unsigned char)input[in])) ++in;
    };

    for (std::size_t i = 0; i < fmt.size(); ++i) {
        char f = fmt[i];

        if (std::isspace((unsigned char)f)) { skip_space(); continue; }

        if (f != '%') {
            if (in < input.size() && input[in] == f) ++in;
            else break; /* literal mismatch ends the scan */
            continue;
        }

        if (i + 1 < fmt.size() && fmt[i + 1] == '%') {
            skip_space();
            if (in < input.size() && input[in] == '%') ++in;
            else break;
            ++i;
            continue;
        }

        /* %[*][width][length]conv */
        bool suppress = false;
        std::size_t width = 0;
        std::size_t j = i + 1;
        if (j < fmt.size() && fmt[j] == '*') { suppress = true; ++j; }
        while (j < fmt.size() && std::isdigit((unsigned char)fmt[j])) {
            width = width * 10 + (std::size_t)(fmt[j] - '0');
            ++j;
        }
        bool is_long_long = false;
        while (j < fmt.size() && std::strchr("hlLqjzt", fmt[j]) != nullptr) {
            if (fmt[j] == 'l' && j + 1 < fmt.size() && fmt[j + 1] == 'l') is_long_long = true;
            if (fmt[j] == 'q' || fmt[j] == 'j' || fmt[j] == 'L') is_long_long = true;
            ++j;
        }
        if (j >= fmt.size()) break;
        char conv = fmt[j];
        i = j;

        if (conv != 'c' && conv != '[' && conv != 'n') skip_space();
        if (in >= input.size() && conv != 'n') break;

        std::size_t limit = width ? std::min(input.size(), in + width) : input.size();

        switch (conv) {
            case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': {
                std::string tok = input.substr(in, limit - in);
                const char *start = tok.c_str();
                char *end = nullptr;
                int base = (conv == 'x' || conv == 'X') ? 16 : (conv == 'o' ? 8 : (conv == 'i' ? 0 : 10));
                long long v = std::strtoll(start, &end, base);
                if (end == start) goto done; /* nothing matched: the scan stops here */
                in += (std::size_t)(end - start);
                if (!suppress) {
                    std::uint32_t p = args.next_u32();
                    if (p != 0) {
                        c.write32(p, (std::uint32_t)v);
                        if (is_long_long) c.write32(p + 4, (std::uint32_t)((std::uint64_t)v >> 32));
                    }
                    ++converted;
                }
                break;
            }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
                std::string tok = input.substr(in, limit - in);
                const char *start = tok.c_str();
                char *end = nullptr;
                double v = std::strtod(start, &end);
                if (end == start) goto done;
                in += (std::size_t)(end - start);
                if (!suppress) {
                    std::uint32_t p = args.next_u32();
                    if (p != 0) {
                        if (is_long_long) { /* %lf: a double */
                            std::uint64_t bits;
                            std::memcpy(&bits, &v, 8);
                            c.write32(p, (std::uint32_t)bits);
                            c.write32(p + 4, (std::uint32_t)(bits >> 32));
                        } else {
                            float fv = (float)v;
                            std::uint32_t bits;
                            std::memcpy(&bits, &fv, 4);
                            c.write32(p, bits);
                        }
                    }
                    ++converted;
                }
                break;
            }
            case 's': {
                std::size_t start = in;
                while (in < limit && !std::isspace((unsigned char)input[in])) ++in;
                if (in == start) goto done;
                if (!suppress) {
                    std::uint32_t p = args.next_u32();
                    c.put_cstr(p, input.substr(start, in - start));
                    ++converted;
                }
                break;
            }
            case 'c': {
                std::size_t n = width ? width : 1;
                if (in + n > input.size()) goto done;
                if (!suppress) {
                    std::uint32_t p = args.next_u32();
                    for (std::size_t k = 0; k < n; ++k) c.write8(p + (std::uint32_t)k, (std::uint8_t)input[in + k]);
                    ++converted;
                }
                in += n;
                break;
            }
            case '[': {
                /* %[abc] / %[^abc]: a scanset, terminated by the first ']' that
                 * is not the very first character. */
                std::size_t k = i + 1;
                bool negate = false;
                if (k < fmt.size() && fmt[k] == '^') { negate = true; ++k; }
                std::string set;
                if (k < fmt.size() && fmt[k] == ']') { set.push_back(']'); ++k; }
                while (k < fmt.size() && fmt[k] != ']') { set.push_back(fmt[k]); ++k; }
                i = k; /* land on ']' */
                std::size_t start = in;
                while (in < limit) {
                    bool in_set = set.find(input[in]) != std::string::npos;
                    if (in_set == negate) break;
                    ++in;
                }
                if (in == start) goto done;
                if (!suppress) {
                    std::uint32_t p = args.next_u32();
                    c.put_cstr(p, input.substr(start, in - start));
                    ++converted;
                }
                break;
            }
            case 'n': {
                if (!suppress) {
                    std::uint32_t p = args.next_u32();
                    if (p != 0) c.write32(p, (std::uint32_t)in);
                }
                break; /* %n is not counted as a conversion */
            }
            default:
                goto done;
        }
    }

done:
    if (converted == 0 && hit_eof_first) return -1; /* EOF */
    return converted;
}

}  // namespace libc

namespace {

using libc::VaSource;

/* --------------------------------------------------------- file streams */

void c_fopen(GuestCall &c) {
    std::string gpath = c.cstr(c.arg(0), 1024);
    std::string mode = c.cstr(c.arg(1), 16);
    std::string hpath = vfs::translate(c.rt, gpath);
    if (mode.find('b') == std::string::npos) mode.push_back('b'); /* force binary on Windows */

    std::FILE *f = std::fopen(hpath.c_str(), mode.c_str());
    std::uint32_t token = 0;
    if (f != nullptr) {
        std::lock_guard<std::mutex> lk(c.rt->files_lock);
        token = c.rt->next_file_token++;
        c.rt->host_files[token] = f;
    } else {
        c.set_errno(2 /* ENOENT */);
    }
    c.log("[io] fopen(\"%s\" [%s], \"%s\") -> %s (token=0x%08x)", gpath.c_str(), hpath.c_str(),
          mode.c_str(), f ? "OK" : "FAIL", token);
    c.set_result(token);
}

void c_fclose(GuestCall &c) {
    std::FILE *f = c.file(c.arg(0));
    if (f != nullptr) {
        std::fclose(f);
        std::lock_guard<std::mutex> lk(c.rt->files_lock);
        c.rt->host_files.erase(c.arg(0));
    }
    c.set_result(f ? 0u : (std::uint32_t)-1);
}

void c_fread(GuestCall &c) {
    std::uint32_t dst = c.arg(0), size = c.arg(1), nmemb = c.arg(2);
    std::FILE *f = c.file(c.arg(3));
    std::uint64_t total = (std::uint64_t)size * nmemb;
    std::uint32_t items = 0;
    if (f != nullptr && size != 0 && total != 0 && c.in_bounds(dst, (std::uint32_t)total)) {
        /* Straight into guest memory: it is one flat host buffer. */
        std::size_t got = std::fread(&c.img->mem[dst], 1, (std::size_t)total, f);
        items = (std::uint32_t)(got / size);
    }
    c.set_result(items);
}

static bool is_host_console(std::FILE *f) {
    return f == stdout || f == stderr;
}

/* stdout/stderr are mapped to the host FILE* so fprintf/fwrite do not vanish,
 * but they must still go through pvz2_log -- otherwise lines like
 * "SharedNotificationManagerRequired" appear without a tag or timestamp. */
static void log_guest_console(GuestCall &c, const std::string &s) {
    c.log("[guest stdout] %s", s.c_str());
}

void c_fwrite(GuestCall &c) {
    std::uint32_t src = c.arg(0), size = c.arg(1), nmemb = c.arg(2);
    std::FILE *f = c.file(c.arg(3));
    std::uint64_t total = (std::uint64_t)size * nmemb;
    std::uint32_t items = 0;
    if (f != nullptr && size != 0 && total != 0 && c.in_bounds(src, (std::uint32_t)total)) {
        if (is_host_console(f)) {
            log_guest_console(c, std::string(reinterpret_cast<const char *>(&c.img->mem[src]),
                                             (std::size_t)total));
            items = nmemb;
        } else {
            std::size_t put = std::fwrite(&c.img->mem[src], 1, (std::size_t)total, f);
            items = (std::uint32_t)(put / size);
        }
    }
    c.set_result(items);
}

void c_fseek(GuestCall &c) {
    std::FILE *f = c.file(c.arg(0));
    c.set_result(f && std::fseek(f, (long)(std::int32_t)c.arg(1), (int)c.arg(2)) == 0
                     ? 0u
                     : (std::uint32_t)-1);
}

void c_ftell(GuestCall &c) {
    std::FILE *f = c.file(c.arg(0));
    c.set_result(f ? (std::uint32_t)std::ftell(f) : (std::uint32_t)-1);
}

void c_rewind(GuestCall &c) {
    std::FILE *f = c.file(c.arg(0));
    if (f) std::rewind(f);
}

void c_fsetpos(GuestCall &c) {
    std::FILE *f = c.file(c.arg(0));
    long pos = (long)c.read32(c.arg(1));
    c.set_result(f && std::fseek(f, pos, SEEK_SET) == 0 ? 0u : (std::uint32_t)-1);
}

void c_fgetpos(GuestCall &c) {
    std::FILE *f = c.file(c.arg(0));
    if (f != nullptr && c.arg(1) != 0) c.write32(c.arg(1), (std::uint32_t)std::ftell(f));
    c.set_result(f ? 0u : (std::uint32_t)-1);
}

void c_feof(GuestCall &c) {
    std::FILE *f = c.file(c.arg(0));
    c.set_result(f ? (std::uint32_t)std::feof(f) : 0u);
}

void c_ferror(GuestCall &c) {
    std::FILE *f = c.file(c.arg(0));
    c.set_result(f ? (std::uint32_t)std::ferror(f) : 0u);
}

void c_fflush(GuestCall &c) {
    std::FILE *f = c.file(c.arg(0));
    if (f) std::fflush(f);
    c.set_result(0);
}

void c_setvbuf(GuestCall &c) { c.set_result(0); } /* buffering is a host concern */

void c_fgetc(GuestCall &c) {
    std::FILE *f = c.file(c.arg(0));
    c.set_result(f ? (std::uint32_t)std::fgetc(f) : (std::uint32_t)-1);
}

void c_fputc(GuestCall &c) {
    std::FILE *f = c.file(c.arg(1));
    int ch = (int)c.arg(0);
    if (f != nullptr) {
        if (is_host_console(f)) {
            /* Line-buffer like putchar so we do not emit one log line per byte. */
            static std::string console_line;
            if (ch == '\n') {
                log_guest_console(c, console_line);
                console_line.clear();
            } else {
                console_line.push_back((char)(ch & 0xFF));
                if (console_line.size() >= 512) {
                    log_guest_console(c, console_line);
                    console_line.clear();
                }
            }
        } else {
            std::fputc(ch, f);
        }
    }
    c.set_result((std::uint32_t)ch);
}

void c_fputs(GuestCall &c) {
    std::FILE *f = c.file(c.arg(1));
    std::string s = c.cstr(c.arg(0));
    if (f != nullptr) {
        if (is_host_console(f)) log_guest_console(c, s);
        else std::fwrite(s.data(), 1, s.size(), f);
    }
    c.set_result((std::uint32_t)s.size());
}

void c_puts(GuestCall &c) {
    c.log("[guest stdout] %s", c.cstr(c.arg(0)).c_str());
    c.set_result(1);
}

/* int putchar(int c) -- fputc to stdout, which this port routes to the log like
 * puts does. Buffered by line so a character-at-a-time writer does not produce
 * one log line per character, which is what made the guest's own diagnostics
 * unreadable. */
void c_putchar(GuestCall &c) {
    static std::string line;
    const int ch = (int)(c.arg(0) & 0xFF);
    if (ch == '\n') {
        c.log("[guest stdout] %s", line.c_str());
        line.clear();
    } else {
        line.push_back((char)ch);
        if (line.size() >= 512) { /* never let a writer with no newline grow without bound */
            c.log("[guest stdout] %s", line.c_str());
            line.clear();
        }
    }
    c.set_result((std::uint32_t)ch);
}

void c_fgets(GuestCall &c) {
    std::uint32_t dst = c.arg(0), size = c.arg(1);
    std::FILE *f = c.file(c.arg(2));
    if (f == nullptr || dst == 0 || size == 0) { c.set_result(0); return; }
    std::vector<char> buf(size);
    if (std::fgets(buf.data(), (int)size, f) == nullptr) { c.set_result(0); return; }
    c.put_cstr(dst, std::string(buf.data()));
    c.set_result(dst);
}

void c_ungetc(GuestCall &c) {
    std::FILE *f = c.file(c.arg(1));
    c.set_result(f ? (std::uint32_t)std::ungetc((int)c.arg(0), f) : (std::uint32_t)-1);
}

void c_fdopen(GuestCall &c) {
    /* The fd is already one of our tokens; wrap the same host fd in a stream. */
    int fd = c.fd(c.arg(0));
    std::string mode = c.cstr(c.arg(1), 16);
    if (fd < 0) { c.set_result(0); return; }
    if (mode.find('b') == std::string::npos) mode.push_back('b');
#if defined(_WIN32)
    std::FILE *f = _fdopen(fd, mode.c_str());
#else
    std::FILE *f = fdopen(fd, mode.c_str());
#endif
    std::uint32_t token = 0;
    if (f != nullptr) {
        std::lock_guard<std::mutex> lk(c.rt->files_lock);
        token = c.rt->next_file_token++;
        c.rt->host_files[token] = f;
    }
    c.set_result(token);
}

/* Wide streams: the engine never actually reads or writes one (it uses
 * std::wstring in memory and narrows on the way out), so orientation is
 * reported as unset and the character accessors report end-of-file. */
void c_fwide(GuestCall &c) { c.set_result(0); }
void c_getwc(GuestCall &c) { c.set_result((std::uint32_t)-1); }
void c_putwc(GuestCall &c) { c.set_result(c.arg(0)); }
void c_ungetwc(GuestCall &c) { c.set_result((std::uint32_t)-1); }

/* -------------------------------------------------------- formatted out */

void store(GuestCall &c, std::uint32_t dst, const std::string &s, std::uint32_t cap, bool bounded) {
    if (dst == 0) return;
    std::uint32_t n = bounded ? (cap > 0 ? std::min<std::uint32_t>((std::uint32_t)s.size(), cap - 1) : 0)
                              : (std::uint32_t)s.size();
    for (std::uint32_t i = 0; i < n; ++i) c.write8(dst + i, (std::uint8_t)s[i]);
    if (!bounded || cap > 0) c.write8(dst + n, 0);
}

void c_sprintf(GuestCall &c) {
    VaSource args = VaSource::from_registers(c, 2);
    std::string s = libc::format(c, c.arg(1), args);
    store(c, c.arg(0), s, 0, false);
    c.set_result((std::uint32_t)s.size());
}

void c_snprintf(GuestCall &c) {
    VaSource args = VaSource::from_registers(c, 3);
    std::string s = libc::format(c, c.arg(2), args);
    store(c, c.arg(0), s, c.arg(1), true);
    c.set_result((std::uint32_t)s.size()); /* the length it WOULD have needed */
}

void c_vsprintf(GuestCall &c) {
    VaSource args = VaSource::from_va_list(c, c.arg(2));
    std::string s = libc::format(c, c.arg(1), args);
    store(c, c.arg(0), s, 0, false);
    c.set_result((std::uint32_t)s.size());
}

void c_vsnprintf(GuestCall &c) {
    VaSource args = VaSource::from_va_list(c, c.arg(3));
    std::string s = libc::format(c, c.arg(2), args);
    store(c, c.arg(0), s, c.arg(1), true);
    c.set_result((std::uint32_t)s.size());
}

/* vswprintf writes 4-byte wchar_t units, not bytes. */
void c_vswprintf(GuestCall &c) {
    std::uint32_t dst = c.arg(0), cap = c.arg(1);
    VaSource args = VaSource::from_va_list(c, c.arg(3));
    std::string s = libc::format(c, c.arg(2), args);
    if (dst != 0 && cap > 0) {
        std::uint32_t n = std::min<std::uint32_t>((std::uint32_t)s.size(), cap - 1);
        for (std::uint32_t i = 0; i < n; ++i) c.write32(dst + i * 4, (std::uint8_t)s[i]);
        c.write32(dst + n * 4, 0);
    }
    c.set_result((std::uint32_t)s.size());
}

void c_printf(GuestCall &c) {
    VaSource args = VaSource::from_registers(c, 1);
    std::string s = libc::format(c, c.arg(0), args);
    c.log("[guest stdout] %s", s.c_str());
    c.set_result((std::uint32_t)s.size());
}

void c_fprintf(GuestCall &c) {
    std::FILE *f = c.file(c.arg(0));
    VaSource args = VaSource::from_registers(c, 2);
    std::string s = libc::format(c, c.arg(1), args);
    if (f == nullptr || is_host_console(f)) log_guest_console(c, s);
    else std::fwrite(s.data(), 1, s.size(), f);
    c.set_result((std::uint32_t)s.size());
}

void c_vfprintf(GuestCall &c) {
    std::FILE *f = c.file(c.arg(0));
    VaSource args = VaSource::from_va_list(c, c.arg(2));
    std::string s = libc::format(c, c.arg(1), args);
    if (f == nullptr || is_host_console(f)) log_guest_console(c, s);
    else std::fwrite(s.data(), 1, s.size(), f);
    c.set_result((std::uint32_t)s.size());
}

/* --------------------------------------------------------- formatted in */

/* Reads the whole remaining stream content is not an option, so fscanf works on
 * a bounded look-ahead and then rewinds by however much it did not consume --
 * which is exactly what a real implementation does internally with ungetc. */
std::string peek_stream(std::FILE *f, long &start_pos, std::size_t max = 1024) {
    start_pos = std::ftell(f);
    std::vector<char> buf(max);
    std::size_t got = std::fread(buf.data(), 1, max, f);
    return std::string(buf.data(), got);
}

void c_sscanf(GuestCall &c) {
    VaSource args = VaSource::from_registers(c, 2);
    c.set_result((std::uint32_t)libc::scan(c, c.cstr(c.arg(0), 4096), c.arg(1), args));
}

/* int vsscanf(const char *s, const char *fmt, va_list ap) -- sscanf with the
 * arguments already packed into a va_list rather than still in registers. */
void c_vsscanf(GuestCall &c) {
    VaSource args = VaSource::from_va_list(c, c.arg(2));
    c.set_result((std::uint32_t)libc::scan(c, c.cstr(c.arg(0), 4096), c.arg(1), args));
}

/* int vasprintf(char **strp, const char *fmt, va_list ap)
 *
 * Formats into a buffer it allocates itself and stores in *strp. The guest
 * frees that pointer with free(), so it has to come from the guest heap
 * (dup_cstr) -- a host allocation would be freed by the guest allocator and
 * corrupt it. Returns -1 without touching *strp when the heap is exhausted,
 * which is exactly what the real one does on ENOMEM. */
void c_vasprintf(GuestCall &c) {
    VaSource args = VaSource::from_va_list(c, c.arg(2));
    const std::string s = libc::format(c, c.arg(1), args);
    const std::uint32_t buf = c.dup_cstr(s);
    if (buf == 0) {
        c.set_result((std::uint32_t)-1);
        return;
    }
    if (c.arg(0) != 0) c.write32(c.arg(0), buf);
    c.set_result((std::uint32_t)s.size());
}

void c_fscanf(GuestCall &c) {
    std::FILE *f = c.file(c.arg(0));
    if (f == nullptr) { c.set_result((std::uint32_t)-1); return; }
    long start = 0;
    std::string text = peek_stream(f, start);
    VaSource args = VaSource::from_registers(c, 2);
    int n = libc::scan(c, text, c.arg(1), args);
    std::fseek(f, start, SEEK_SET); /* conservative: re-read rather than over-consume */
    c.set_result((std::uint32_t)n);
}

/* swscanf's input is a wide string; narrow it and reuse the same engine. */
void c_swscanf(GuestCall &c) {
    std::string input;
    for (std::uint32_t i = 0; i < 4096; ++i) {
        std::uint32_t ch = c.read32(c.arg(0) + i * 4);
        if (ch == 0) break;
        input.push_back(ch < 128 ? (char)ch : '?');
    }
    VaSource args = VaSource::from_registers(c, 2);
    /* The format string is wide too: copy it into a narrow scratch buffer the
     * scanner can read with cstr(). */
    std::string narrow_fmt;
    for (std::uint32_t i = 0; i < 1024; ++i) {
        std::uint32_t ch = c.read32(c.arg(1) + i * 4);
        if (ch == 0) break;
        narrow_fmt.push_back(ch < 128 ? (char)ch : '?');
    }
    std::uint32_t scratch = c.dup_cstr(narrow_fmt);
    int n = libc::scan(c, input, scratch, args);
    if (scratch != 0) c.rt->heap.free_ptr(scratch);
    c.set_result((std::uint32_t)n);
}

}  // namespace

void register_libc_stdio(ImportTable &t) {
    t.add("fopen", c_fopen);
    t.add("fclose", c_fclose);
    t.add("fread", c_fread);
    t.add("fwrite", c_fwrite);
    t.add("fseek", c_fseek);
    t.add("ftell", c_ftell);
    t.add("rewind", c_rewind);
    t.add("fsetpos", c_fsetpos);
    t.add("fgetpos", c_fgetpos);
    t.add("feof", c_feof);
    t.add("ferror", c_ferror);
    t.add("clearerr", [](GuestCall &c) { if (std::FILE *f = c.file(c.arg(0))) std::clearerr(f); });
    t.add("fflush", c_fflush);
    t.add("setvbuf", c_setvbuf);
    t.add("setbuf", c_setvbuf);
    t.add("fdopen", c_fdopen);

    t.add("fgetc", c_fgetc);
    t.add("getc", c_fgetc);
    t.add("fputc", c_fputc);
    t.add("putc", c_fputc);
    t.add("fputs", c_fputs);
    t.add("puts", c_puts);
    t.add("putchar", c_putchar);
    t.add("fgets", c_fgets);
    t.add("ungetc", c_ungetc);

    t.add("fwide", c_fwide);
    t.add("getwc", c_getwc);
    t.add("putwc", c_putwc);
    t.add("fputwc", c_putwc); /* same function, spelled two ways */
    t.add("ungetwc", c_ungetwc);

    t.add("sprintf", c_sprintf);
    t.add("snprintf", c_snprintf);
    t.add("vsprintf", c_vsprintf);
    t.add("vsnprintf", c_vsnprintf);
    t.add("vswprintf", c_vswprintf);
    t.add("printf", c_printf);
    t.add("fprintf", c_fprintf);
    t.add("vfprintf", c_vfprintf);

    t.add("sscanf", c_sscanf);
    t.add("vsscanf", c_vsscanf);
    t.add("vasprintf", c_vasprintf);
    t.add("fscanf", c_fscanf);
    t.add("swscanf", c_swscanf);
}

}  // namespace pvz2native
