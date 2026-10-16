/* libc.so -- <unistd.h>, <fcntl.h>, <sys/stat.h>, <dirent.h>: real host-backed
 * file I/O (doc 9.22).
 *
 * The game's own native RSB/RTON loader opens "main.rsb" and every other
 * resource through these entry points and decodes the "1bsr"/"pgsr" container
 * formats itself, so none of that is reimplemented -- it just needs honest file
 * I/O over the real .obb, which vfs::translate() supplies.
 *
 * bionic's struct layouts are NOT the host's and must be written field by field
 * at the offsets below (confirmed against bionic's sys/stat.h __STAT64_BODY and
 * dirent.h -- doc 9.21).
 */

#include <pvz2native/dependencies/dependency.h>
#include <pvz2native/dependencies/vfs.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace pvz2native {
namespace {

/* ------------------------------------------------ bionic struct offsets */

/* struct stat is 104 bytes on armeabi-v7a. */
constexpr std::uint32_t kStatSize = 104;
constexpr std::uint32_t kStatMode = 0x10;
constexpr std::uint32_t kStatSizeLo = 0x30;
constexpr std::uint32_t kStatMtime = 0x50;

/* struct dirent: d_ino(8) d_off(8) d_reclen(2) d_type(1) d_name[256]. */
constexpr std::uint32_t kDirentReclen = 16;
constexpr std::uint32_t kDirentType = 18;
constexpr std::uint32_t kDirentName = 19;
constexpr std::uint32_t kDirentSize = 19 + 256;
constexpr std::uint8_t kDtDir = 4;
constexpr std::uint8_t kDtReg = 8;

void write_stat(GuestCall &c, std::uint32_t buf, std::uint32_t mode, std::uint64_t size,
                std::uint32_t mtime) {
    for (std::uint32_t i = 0; i < kStatSize; i += 4) c.write32(buf + i, 0);
    c.write32(buf + kStatMode, mode);
    c.write32(buf + kStatSizeLo, (std::uint32_t)(size & 0xFFFFFFFFu));
    c.write32(buf + kStatSizeLo + 4, (std::uint32_t)(size >> 32));
    c.write32(buf + kStatMtime, mtime);
}

/* ----------------------------------------------------- file descriptors */

/* /dev/urandom, which Android has and a Windows filesystem does not.
 *
 * Not a nicety: libc++'s std::random_device opens it in a CONSTRUCTOR, and when
 * the open failed it threw std::system_error out of a .init_array entry. Our
 * frames carry no handler (LR is the halt sentinel), so the unwinder ran off
 * the bottom of the stack into abort() -- taking the rest of that translation
 * unit's global constructors with it and leaving whatever they build
 * half-initialised, with nothing downstream saying why.
 *
 * A fixed token just below kFdTokenBase: positive, so the guest's `fd < 0`
 * checks still work, and outside the range the allocator ever hands out, so it
 * cannot collide with a real file.
 *
 * Note this is a TOKEN, not a guest address, even though it looks like one. It
 * sits numerically inside the trampoline range in runtime/guest_memmap.h and that
 * is harmless: fd tokens are never dereferenced, they only index a host table.
 * Do not "fix" the apparent overlap by moving it. */
constexpr std::uint32_t kRandomFdToken = kFdTokenBase - 1;

bool is_random_device(const std::string &path) {
    return path == "/dev/urandom" || path == "/dev/random";
}

void fill_random(GuestCall &c, std::uint32_t dst, std::uint32_t count) {
    /* Seeded once from the host's own entropy; the guest wants unpredictable
     * bytes, not reproducible ones. */
    static std::mutex lock;
    static std::mt19937 rng{std::random_device{}()};
    std::lock_guard<std::mutex> lk(lock);
    for (std::uint32_t i = 0; i < count; ++i) {
        c.write8(dst + i, (std::uint8_t)(rng() & 0xFFu));
    }
}

void c_open(GuestCall &c) {
    std::string gpath = c.cstr(c.arg(0), 1024);
    if (is_random_device(gpath)) {
        c.log("[io] open(\"%s\") -> OK (synthetic random device, token=0x%08x)", gpath.c_str(),
              kRandomFdToken);
        c.set_result(kRandomFdToken);
        return;
    }
    std::string hpath = vfs::translate(c.rt, gpath);
#if defined(_WIN32)
    int fd = _open(hpath.c_str(), vfs::translate_open_flags(c.arg(1)), 0666);
#else
    int fd = open(hpath.c_str(), vfs::translate_open_flags(c.arg(1)), 0666);
#endif
    std::uint32_t token = (std::uint32_t)-1;
    if (fd >= 0) {
        std::lock_guard<std::mutex> lk(c.rt->files_lock);
        token = c.rt->next_fd_token++;
        c.rt->host_fds[token] = fd;
    } else {
        c.set_errno(2 /* ENOENT */);
    }
    c.log("[io] open(\"%s\" [%s], 0x%x) -> %s (token=0x%08x)", gpath.c_str(), hpath.c_str(), c.arg(1),
          fd >= 0 ? "OK" : "FAIL", token);
    c.set_result(token);
}

void c_close(GuestCall &c) {
    if (c.arg(0) == kRandomFdToken) { c.set_result(0); return; }
    int fd = c.fd(c.arg(0));
    if (fd < 0) { c.set_result((std::uint32_t)-1); return; }
#if defined(_WIN32)
    _close(fd);
#else
    close(fd);
#endif
    std::lock_guard<std::mutex> lk(c.rt->files_lock);
    c.rt->host_fds.erase(c.arg(0));
    c.set_result(0);
}

void c_read(GuestCall &c) {
    std::uint32_t dst = c.arg(1), count = c.arg(2);
    if (c.arg(0) == kRandomFdToken) {
        if (!c.in_bounds(dst, count)) {
            c.set_errno(14 /* EFAULT */);
            c.set_result((std::uint32_t)-1);
            return;
        }
        /* /dev/urandom never short-reads and never blocks. */
        fill_random(c, dst, count);
        c.set_result(count);
        return;
    }
    int fd = c.fd(c.arg(0));
    int got = -1;
    if (fd >= 0 && c.in_bounds(dst, count)) {
#if defined(_WIN32)
        got = _read(fd, &c.img->mem[dst], count);
#else
        got = (int)read(fd, &c.img->mem[dst], count);
#endif
    }
    c.set_result((std::uint32_t)got);
}

void c_write(GuestCall &c) {
    int fd = c.fd(c.arg(0));
    std::uint32_t src = c.arg(1), count = c.arg(2);
    int put = -1;
    if (fd >= 0 && c.in_bounds(src, count)) {
#if defined(_WIN32)
        put = _write(fd, &c.img->mem[src], count);
#else
        put = (int)write(fd, &c.img->mem[src], count);
#endif
    }
    c.set_result((std::uint32_t)put);
}

void c_lseek(GuestCall &c) {
    int fd = c.fd(c.arg(0));
    if (fd < 0) { c.set_result((std::uint32_t)-1); return; }
#if defined(_WIN32)
    c.set_result((std::uint32_t)_lseek(fd, (long)(std::int32_t)c.arg(1), (int)c.arg(2)));
#else
    c.set_result((std::uint32_t)lseek(fd, (off_t)(std::int32_t)c.arg(1), (int)c.arg(2)));
#endif
}

/* writev(fd, iov, iovcnt): the iovec array lives in guest memory, two words per
 * entry. Writing each slice in turn is equivalent to the atomic vector write
 * for a regular file, which is all this is ever used on. */
void c_writev(GuestCall &c) {
    int fd = c.fd(c.arg(0));
    std::uint32_t iov = c.arg(1), cnt = c.arg(2);
    std::uint32_t total = 0;
    for (std::uint32_t i = 0; i < cnt; ++i) {
        std::uint32_t base = c.read32(iov + i * 8);
        std::uint32_t len = c.read32(iov + i * 8 + 4);
        if (len == 0 || !c.in_bounds(base, len)) continue;
        if (fd >= 0) {
#if defined(_WIN32)
            int put = _write(fd, &c.img->mem[base], len);
#else
            int put = (int)write(fd, &c.img->mem[base], len);
#endif
            if (put > 0) total += (std::uint32_t)put;
            if (put != (int)len) break;
        } else {
            /* Not one of our fds: bionic's stderr path. Report rather than
             * silently swallow -- this is how the guest's own crash handler
             * talks. */
            c.log("[guest fd %u] %.*s", c.arg(0), (int)len, (const char *)&c.img->mem[base]);
            total += len;
        }
    }
    c.set_result(total);
}

void c_ftruncate(GuestCall &c) {
    int fd = c.fd(c.arg(0));
#if defined(_WIN32)
    c.set_result(fd >= 0 && _chsize(fd, (long)c.arg(1)) == 0 ? 0u : (std::uint32_t)-1);
#else
    c.set_result(fd >= 0 && ftruncate(fd, (off_t)c.arg(1)) == 0 ? 0u : (std::uint32_t)-1);
#endif
}

void c_fsync(GuestCall &c) {
    int fd = c.fd(c.arg(0));
#if defined(_WIN32)
    c.set_result(fd >= 0 && _commit(fd) == 0 ? 0u : (std::uint32_t)-1);
#else
    c.set_result(fd >= 0 && fsync(fd) == 0 ? 0u : (std::uint32_t)-1);
#endif
}

/* ------------------------------------------------------------ metadata */

void stat_path(GuestCall &c) {
    std::string hpath = vfs::translate(c.rt, c.cstr(c.arg(0), 1024));
    std::error_code ec;
    auto status = std::filesystem::status(hpath, ec);
    if (ec) {
        for (std::uint32_t i = 0; i < kStatSize; i += 4) c.write32(c.arg(1) + i, 0);
        c.set_errno(2 /* ENOENT */);
        c.set_result((std::uint32_t)-1);
        return;
    }
    bool is_dir = std::filesystem::is_directory(status);
    std::uintmax_t size = is_dir ? 0 : std::filesystem::file_size(hpath, ec);
    if (ec) size = 0;
    /* S_IFDIR 0040000 / S_IFREG 0100000, plus rwxr-xr-x. */
    write_stat(c, c.arg(1), (is_dir ? 0040000u : 0100000u) | 0755u, size, 0);
    c.set_result(0);
}

void c_fstat(GuestCall &c) {
    int fd = c.fd(c.arg(0));
    std::uint32_t buf = c.arg(1);
#if defined(_WIN32)
    struct _stati64 st;
    if (fd >= 0 && _fstati64(fd, &st) == 0) {
#else
    struct stat st;
    if (fd >= 0 && fstat(fd, &st) == 0) {
#endif
        write_stat(c, buf, (std::uint32_t)st.st_mode, (std::uint64_t)st.st_size,
                   (std::uint32_t)st.st_mtime);
        c.set_result(0);
        return;
    }
    for (std::uint32_t i = 0; i < kStatSize; i += 4) c.write32(buf + i, 0);
    c.set_errno(2 /* ENOENT */);
    c.set_result((std::uint32_t)-1);
}

void c_access(GuestCall &c) {
    std::string hpath = vfs::translate(c.rt, c.cstr(c.arg(0), 1024));
    std::error_code ec;
    c.set_result(std::filesystem::exists(hpath, ec) ? 0u : (std::uint32_t)-1);
}

void c_mkdir(GuestCall &c) {
    std::string hpath = vfs::translate(c.rt, c.cstr(c.arg(0), 1024));
    std::error_code ec;
    std::filesystem::create_directories(hpath, ec);
    /* "already exists" is not an error for any caller here. */
    c.set_result(0);
}

void c_unlink(GuestCall &c) {
    std::string hpath = vfs::translate(c.rt, c.cstr(c.arg(0), 1024));
    std::error_code ec;
    c.set_result(std::filesystem::remove(hpath, ec) ? 0u : (std::uint32_t)-1);
}

/* rmdir removes an EMPTY directory only, so remove() (not remove_all) is the
 * right call: it fails on a non-empty one exactly as rmdir must. */
void c_rmdir(GuestCall &c) {
    std::string hpath = vfs::translate(c.rt, c.cstr(c.arg(0), 1024));
    std::error_code ec;
    c.set_result(std::filesystem::remove(hpath, ec) ? 0u : (std::uint32_t)-1);
}

/* Accepted and ignored. Every guest path is rewritten by vfs::translate()
 * anyway, and the engine treats its resource root as "." -- so honouring a
 * chdir would move the host process out from under paths the VFS has already
 * resolved, for no gain. Reporting success is what the guest expects; the
 * alternative, failing, is what makes an installer-style path give up. */
void c_chdir(GuestCall &c) { c.set_result(0); }

/* There are no meaningful POSIX permissions to set on the host side here, and
 * the game only ever calls this to mark its own save files writable, which they
 * already are. */
void c_chmod(GuestCall &c) { c.set_result(0); }

/* Nothing in the guest tree is a symlink, so this is not "unimplemented" -- it
 * is the correct answer, and EINVAL is precisely "not a symbolic link". */
void c_readlink(GuestCall &c) {
    c.set_errno(22 /* EINVAL */);
    c.set_result((std::uint32_t)-1);
}

/* utime(path, times): the engine only ever uses it to touch a save file it has
 * just written, and the host has already given that file a current timestamp. */
void c_utime(GuestCall &c) { c.set_result(0); }

/* int getentropy(void *buffer, size_t length) -- 0 on success, -1/EIO on
 * failure, and length > 256 is EIO by specification. Backed by the same host
 * entropy as /dev/urandom above, because the caller (libc++'s random_device,
 * and any hashing seed) genuinely wants unpredictable bytes. */
void c_getentropy(GuestCall &c) {
    const std::uint32_t buf = c.arg(0), len = c.arg(1);
    if (len > 256 || buf == 0 || !c.in_bounds(buf, len)) {
        c.set_errno(5 /* EIO */);
        c.set_result((std::uint32_t)-1);
        return;
    }
    fill_random(c, buf, len);
    c.set_result(0);
}

/* --- what 9.6.1 adds --------------------------------------------------------
 *
 * Permission and ownership bits have no meaning here: the guest tree is a
 * directory on the host owned by whoever ran the emulator, and there is exactly
 * one guest user (see getuid in libc_misc.cpp). Reporting success is not a
 * shortcut -- it is what these calls do on a filesystem without POSIX
 * ownership, which is where an Android app's external storage usually lives. */
void c_fchmod(GuestCall &c) { c.set_result(0); }
void c_fchmodat(GuestCall &c) { c.set_result(0); }
void c_lchown(GuestCall &c) { c.set_result(0); }
void c_utimensat(GuestCall &c) { c.set_result(0); }

/* Hard links, symlinks and device nodes. EPERM is "the filesystem does not
 * support this", which is true of the host paths this VFS maps onto and is a
 * state callers already handle -- it is what they get on FAT/exFAT storage.
 * Reporting success and creating nothing would leave the caller believing a
 * path exists that does not. */
void c_link(GuestCall &c) {
    c.set_errno(1 /* EPERM */);
    c.set_result((std::uint32_t)-1);
}
void c_symlink(GuestCall &c) {
    c.set_errno(1 /* EPERM */);
    c.set_result((std::uint32_t)-1);
}
void c_mknod(GuestCall &c) {
    c.set_errno(1 /* EPERM */);
    c.set_result((std::uint32_t)-1);
}

/* int truncate(const char *path, off_t length) -- ftruncate by path. */
void c_truncate(GuestCall &c) {
    const std::string hpath = vfs::translate(c.rt, c.cstr(c.arg(0), 1024));
#if defined(_WIN32)
    int fd = _open(hpath.c_str(), _O_RDWR | _O_BINARY);
    if (fd < 0) {
        c.set_errno(2 /* ENOENT */);
        c.set_result((std::uint32_t)-1);
        return;
    }
    int rc = _chsize(fd, (long)c.arg(1));
    _close(fd);
#else
    int rc = truncate(hpath.c_str(), (off_t)c.arg(1));
#endif
    if (rc != 0) c.set_errno(2 /* ENOENT */);
    c.set_result(rc == 0 ? 0u : (std::uint32_t)-1);
}

/* char *realpath(const char *path, char *resolved)
 *
 * The guest tree is virtual, so "canonical" means lexically canonical: there
 * are no symlinks to follow (see c_readlink) and translating to a HOST path
 * would hand the guest an absolute Windows path it would then fail to reopen.
 * Collapses "." and ".." and duplicate separators, and keeps the answer in the
 * guest's own namespace. */
void c_realpath(GuestCall &c) {
    const std::uint32_t out = c.arg(1);
    const std::string in = c.cstr(c.arg(0), 1024);
    if (in.empty()) {
        c.set_errno(2 /* ENOENT */);
        c.set_result(0);
        return;
    }

    std::vector<std::string> parts;
    const bool absolute = in[0] == '/';
    std::size_t i = 0;
    while (i < in.size()) {
        std::size_t j = in.find('/', i);
        if (j == std::string::npos) j = in.size();
        const std::string seg = in.substr(i, j - i);
        if (seg == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!seg.empty() && seg != ".") {
            parts.push_back(seg);
        }
        i = j + 1;
    }
    std::string canonical = absolute ? "/" : "";
    for (std::size_t k = 0; k < parts.size(); ++k) {
        if (k) canonical += '/';
        canonical += parts[k];
    }
    if (canonical.empty()) canonical = ".";

    /* A NULL `resolved` means "allocate it for me", and the caller frees it --
     * so it must come from the same heap free() uses. */
    std::uint32_t dst = out;
    if (dst == 0) {
        dst = c.dup_cstr(canonical);
        c.set_result(dst);
        return;
    }
    c.put_cstr(dst, canonical);
    c.set_result(dst);
}

/* long pathconf(const char *path, int name) / fpathconf(int fd, int name).
 * Only the two limits anyone actually queries are answered; -1 without setting
 * errno is the specified way to say "no limit / not applicable". */
void c_pathconf(GuestCall &c) {
    switch (c.arg(1)) {
        case 3:  c.set_result(255); return;  /* _PC_NAME_MAX */
        case 4:  c.set_result(4096); return; /* _PC_PATH_MAX */
        case 5:  c.set_result(4096); return; /* _PC_PIPE_BUF */
        default: c.set_result((std::uint32_t)-1); return;
    }
}

/* int pipe(int fds[2]) -- a REAL host pipe.
 *
 * Implemented rather than refused because a pipe is the classic self-pipe
 * wakeup, and a thread that cannot create one may spin or block forever instead
 * of taking an error path. Nothing about it needs a network or a second
 * process, so there is no reason to refuse. */
void c_pipe(GuestCall &c) {
    const std::uint32_t out = c.arg(0);
    int fds[2] = {-1, -1};
#if defined(_WIN32)
    int rc = _pipe(fds, 4096, _O_BINARY);
#else
    int rc = pipe(fds);
#endif
    if (rc != 0 || out == 0) {
        c.set_errno(24 /* EMFILE */);
        c.set_result((std::uint32_t)-1);
        return;
    }
    std::lock_guard<std::mutex> lk(c.rt->files_lock);
    for (int i = 0; i < 2; ++i) {
        const std::uint32_t token = c.rt->next_fd_token++;
        c.rt->host_fds[token] = fds[i];
        c.write32(out + (std::uint32_t)i * 4, token);
    }
    c.set_result(0);
}

/* int dup2(int oldfd, int newfd)
 *
 * Guest descriptors are tokens into host_fds, not real numbers, so this cannot
 * defer to the host's dup2 -- it duplicates the underlying host descriptor and
 * rebinds the NEW TOKEN to it, which is the behaviour the guest can observe. */
void c_dup2(GuestCall &c) {
    const std::uint32_t oldtok = c.arg(0), newtok = c.arg(1);
    if (oldtok == newtok) {
        c.set_result(newtok);
        return;
    }
    int oldfd = c.fd(oldtok);
    if (oldfd < 0) {
        c.set_errno(9 /* EBADF */);
        c.set_result((std::uint32_t)-1);
        return;
    }
#if defined(_WIN32)
    int copy = _dup(oldfd);
#else
    int copy = dup(oldfd);
#endif
    if (copy < 0) {
        c.set_errno(24 /* EMFILE */);
        c.set_result((std::uint32_t)-1);
        return;
    }
    std::lock_guard<std::mutex> lk(c.rt->files_lock);
    auto existing = c.rt->host_fds.find(newtok);
    if (existing != c.rt->host_fds.end()) {
#if defined(_WIN32)
        _close(existing->second);
#else
        close(existing->second);
#endif
    }
    c.rt->host_fds[newtok] = copy;
    c.set_result(newtok);
}

/* int fcntl(int fd, int cmd, ...)
 *
 * Only the flag queries the guest actually makes. F_GETFL must report a real
 * access mode -- O_RDWR -- because a caller that reads 0 concludes the
 * descriptor is read-only and may refuse to write to it. */
void c_fcntl(GuestCall &c) {
    const int fd = c.fd(c.arg(0));
    if (fd < 0) {
        c.set_errno(9 /* EBADF */);
        c.set_result((std::uint32_t)-1);
        return;
    }
    switch (c.arg(1)) {
        case 1: c.set_result(0); return;  /* F_GETFD -> no FD_CLOEXEC       */
        case 2: c.set_result(0); return;  /* F_SETFD -> accepted, ignored   */
        case 3: c.set_result(2); return;  /* F_GETFL -> O_RDWR              */
        case 4: c.set_result(0); return;  /* F_SETFL -> accepted, ignored   */
        default:
            c.set_errno(22 /* EINVAL */);
            c.set_result((std::uint32_t)-1);
            return;
    }
}

/* ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count)
 *
 * A real copy through a host buffer. Both descriptors are ours, so this is
 * ordinary file I/O -- the name is misleading, no socket is involved. Honours
 * the *offset in/out contract: when non-NULL, reading starts there and the file
 * position of in_fd is left alone. */
void c_sendfile(GuestCall &c) {
    const int out_fd = c.fd(c.arg(0));
    const int in_fd = c.fd(c.arg(1));
    const std::uint32_t offptr = c.arg(2);
    std::uint32_t count = c.arg(3);
    if (out_fd < 0 || in_fd < 0) {
        c.set_errno(9 /* EBADF */);
        c.set_result((std::uint32_t)-1);
        return;
    }

    long start = -1;
    if (offptr != 0) {
        start = (long)c.read32(offptr);
#if defined(_WIN32)
        long saved = _lseek(in_fd, 0, SEEK_CUR);
        _lseek(in_fd, start, SEEK_SET);
#else
        long saved = lseek(in_fd, 0, SEEK_CUR);
        lseek(in_fd, start, SEEK_SET);
#endif
        (void)saved;
    }

    std::vector<char> buf(64 * 1024);
    std::uint32_t moved = 0;
    while (count > 0) {
        const std::uint32_t want = count < buf.size() ? count : (std::uint32_t)buf.size();
#if defined(_WIN32)
        int got = _read(in_fd, buf.data(), want);
#else
        int got = (int)read(in_fd, buf.data(), want);
#endif
        if (got <= 0) break;
#if defined(_WIN32)
        int put = _write(out_fd, buf.data(), got);
#else
        int put = (int)write(out_fd, buf.data(), got);
#endif
        if (put <= 0) break;
        moved += (std::uint32_t)put;
        count -= (std::uint32_t)put;
        if (put < got) break; /* short write: stop, as sendfile does */
    }
    if (offptr != 0) c.write32(offptr, (std::uint32_t)(start + (long)moved));
    c.set_result(moved);
}

/* int statfs(const char *path, struct statfs *buf)
 *
 * Reports the real free space, because the one caller that matters is a
 * "do I have room to download/unpack this?" check -- answering with a made-up
 * number could either block the game on a false negative or let it start a
 * write that fails halfway. struct statfs on armeabi-v7a is 64 bytes with
 * 32-bit fields; only f_bsize/f_blocks/f_bfree/f_bavail are ever read. */
void c_statfs(GuestCall &c) {
    const std::uint32_t buf = c.arg(1);
    if (buf == 0) {
        c.set_errno(14 /* EFAULT */);
        c.set_result((std::uint32_t)-1);
        return;
    }
    const std::string hpath = vfs::translate(c.rt, c.cstr(c.arg(0), 1024));
    std::error_code ec;
    std::filesystem::space_info space = std::filesystem::space(hpath, ec);
    if (ec) {
        c.set_errno(2 /* ENOENT */);
        c.set_result((std::uint32_t)-1);
        return;
    }

    constexpr std::uint64_t kBlockSize = 4096;
    auto blocks = [&](std::uintmax_t bytes) -> std::uint32_t {
        const std::uint64_t n = (std::uint64_t)bytes / kBlockSize;
        /* The fields are 32-bit, so a large drive genuinely does not fit; cap
         * rather than wrap, which would report a nearly-full disk. */
        return (std::uint32_t)std::min<std::uint64_t>(n, 0xFFFFFFFFull);
    };

    for (std::uint32_t i = 0; i < 64; i += 4) c.write32(buf + i, 0);
    c.write32(buf + 0, 0);                        /* f_type    */
    c.write32(buf + 4, (std::uint32_t)kBlockSize); /* f_bsize   */
    c.write32(buf + 8, blocks(space.capacity));    /* f_blocks  */
    c.write32(buf + 12, blocks(space.free));       /* f_bfree   */
    c.write32(buf + 16, blocks(space.available));  /* f_bavail  */
}

void c_rename(GuestCall &c) {
    std::string from = vfs::translate(c.rt, c.cstr(c.arg(0), 1024));
    std::string to = vfs::translate(c.rt, c.cstr(c.arg(1), 1024));
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    c.set_result(ec ? (std::uint32_t)-1 : 0u);
}

void c_getcwd(GuestCall &c) {
    std::uint32_t buf = c.arg(0), size = c.arg(1);
    /* The engine treats its resource root as ".", and every path it builds is
     * relative to it, so reporting the real host cwd would be wrong. */
    const std::string cwd = ".";
    if (buf != 0 && size > cwd.size()) {
        c.put_cstr(buf, cwd);
        c.set_result(buf);
        return;
    }
    c.set_result(0);
}

/* --------------------------------------------------------- directories */

struct DirHandle {
    std::filesystem::directory_iterator it;
    std::uint32_t dirent_block = 0; /* guest-visible struct dirent returned by readdir */
};

std::mutex g_dirs_lock;
std::map<std::uint32_t, DirHandle> g_dirs;

void c_opendir(GuestCall &c) {
    std::string hpath = vfs::translate(c.rt, c.cstr(c.arg(0), 1024));
    std::error_code ec;
    std::filesystem::directory_iterator it(hpath, ec);
    if (ec) {
        c.set_errno(2 /* ENOENT */);
        c.set_result(0);
        return;
    }
    /* The DIR* is a guest allocation so it is a unique, non-null token the
     * guest can compare against NULL; its contents are ours. */
    std::uint32_t handle = c.rt->heap.alloc(8);
    if (handle == 0) { c.set_result(0); return; }
    std::uint32_t block = c.rt->heap.alloc(kDirentSize);

    std::lock_guard<std::mutex> lk(g_dirs_lock);
    g_dirs[handle] = DirHandle{it, block};
    c.set_result(handle);
}

/* Fills the dirent block from the iterator, or returns 0 at end of stream. */
std::uint32_t next_entry(GuestCall &c, std::uint32_t handle) {
    std::lock_guard<std::mutex> lk(g_dirs_lock);
    auto found = g_dirs.find(handle);
    if (found == g_dirs.end()) return 0;
    DirHandle &d = found->second;
    if (d.it == std::filesystem::directory_iterator{}) return 0;

    std::string name = d.it->path().filename().string();
    bool is_dir = d.it->is_directory();
    std::error_code ec;
    d.it.increment(ec);

    std::uint32_t block = d.dirent_block;
    if (block == 0) return 0;
    for (std::uint32_t i = 0; i < kDirentSize; i += 4) c.write32(block + i, 0);
    c.write16(block + kDirentReclen, (std::uint16_t)kDirentSize);
    c.write8(block + kDirentType, is_dir ? kDtDir : kDtReg);
    if (name.size() > 255) name.resize(255);
    c.put_cstr(block + kDirentName, name);
    return block;
}

void c_readdir(GuestCall &c) {
    c.set_result(next_entry(c, c.arg(0)));
}

/* readdir_r(dirp, entry, result): fills the CALLER's buffer and points *result
 * at it, or NULLs *result at end of stream. */
void c_readdir_r(GuestCall &c) {
    std::uint32_t entry = c.arg(1), result = c.arg(2);
    std::uint32_t block = next_entry(c, c.arg(0));
    if (block == 0) {
        if (result != 0) c.write32(result, 0);
        c.set_result(0);
        return;
    }
    if (entry != 0) {
        for (std::uint32_t i = 0; i < kDirentSize; ++i) c.write8(entry + i, c.read8(block + i));
    }
    if (result != 0) c.write32(result, entry);
    c.set_result(0);
}

void c_closedir(GuestCall &c) {
    std::uint32_t handle = c.arg(0);
    std::uint32_t block = 0;
    {
        std::lock_guard<std::mutex> lk(g_dirs_lock);
        auto found = g_dirs.find(handle);
        if (found != g_dirs.end()) {
            block = found->second.dirent_block;
            g_dirs.erase(found);
        }
    }
    if (block != 0) c.rt->heap.free_ptr(block);
    if (handle != 0) c.rt->heap.free_ptr(handle);
    c.set_result(0);
}

/* fnmatch over the subset the engine uses for resource globs: '*', '?' and
 * literal text. Character classes are not used by any caller. */
bool glob_match(const char *pat, const char *str) {
    if (*pat == '\0') return *str == '\0';
    if (*pat == '*') {
        for (const char *s = str;; ++s) {
            if (glob_match(pat + 1, s)) return true;
            if (*s == '\0') return false;
        }
    }
    if (*str == '\0') return false;
    if (*pat == '?' || *pat == *str) return glob_match(pat + 1, str + 1);
    return false;
}

void c_fnmatch(GuestCall &c) {
    std::string pattern = c.cstr(c.arg(0), 256);
    std::string name = c.cstr(c.arg(1), 1024);
    c.set_result(glob_match(pattern.c_str(), name.c_str()) ? 0u : 1u /* FNM_NOMATCH */);
}

/* ------------------------------------------------------------ terminals */

void c_ioctl(GuestCall &c) { c.set_result((std::uint32_t)-1); } /* no device nodes exist here */
void c_poll(GuestCall &c) { c.set_result(0); }                  /* nothing is ever ready */

}  // namespace

void register_libc_unistd(ImportTable &t) {
    t.add("open", c_open);
    t.add("close", c_close);
    t.add("read", c_read);
    t.add("write", c_write);
    t.add("writev", c_writev);
    t.add("lseek", c_lseek);
    t.add("ftruncate", c_ftruncate);
    t.add("fsync", c_fsync);

    t.add("stat", stat_path);
    t.add("lstat", stat_path);
    t.add("fstat", c_fstat);
    t.add("access", c_access);
    t.add("mkdir", c_mkdir);
    t.add("rmdir", c_rmdir);
    t.add("unlink", c_unlink);
    t.add("remove", c_unlink);
    t.add("rename", c_rename);
    t.add("getcwd", c_getcwd);
    t.add("chdir", c_chdir);
    t.add("chmod", c_chmod);
    t.add("readlink", c_readlink);
    t.add("utime", c_utime);
    t.add("statfs", c_statfs);

    t.add("opendir", c_opendir);
    t.add("readdir", c_readdir);
    t.add("readdir_r", c_readdir_r);
    t.add("closedir", c_closedir);
    t.add("fnmatch", c_fnmatch);

    t.add("ioctl", c_ioctl);
    t.add("poll", c_poll);

    /* --- added for 9.6.1 --- */
    t.add("getentropy", c_getentropy);
    t.add("fchmod", c_fchmod);
    t.add("fchmodat", c_fchmodat);
    t.add("lchown", c_lchown);
    t.add("utimensat", c_utimensat);
    t.add("link", c_link);
    t.add("symlink", c_symlink);
    t.add("mknod", c_mknod);
    t.add("truncate", c_truncate);
    t.add("realpath", c_realpath);
    t.add("pathconf", c_pathconf);
    t.add("fpathconf", c_pathconf);
    t.add("pipe", c_pipe);
    t.add("dup2", c_dup2);
    t.add("fcntl", c_fcntl);
    t.add("sendfile", c_sendfile);
}

}  // namespace pvz2native
