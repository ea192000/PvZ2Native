#include <pvz2native/log.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <mutex>

namespace {

std::mutex g_lock;
int g_enabled = 1;

void format_timestamp(char *buf, std::size_t n) {
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const std::time_t t = clock::to_time_t(now);
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    std::snprintf(buf, n, "%04d-%02d-%02d %02d:%02d:%02d", tm_buf.tm_year + 1900,
                  tm_buf.tm_mon + 1, tm_buf.tm_mday, tm_buf.tm_hour, tm_buf.tm_min,
                  tm_buf.tm_sec);
}

/* Collapse CR/LF so a message that already ends with \\n does not produce a
 * blank line after our own trailing newline. Internal newlines become " | ". */
void sanitize_message(char *msg) {
    if (msg == nullptr) return;
    /* Trim trailing whitespace / newlines. */
    std::size_t n = std::strlen(msg);
    while (n > 0 && (msg[n - 1] == '\n' || msg[n - 1] == '\r' || msg[n - 1] == ' ' ||
                     msg[n - 1] == '\t')) {
        msg[--n] = '\0';
    }
    /* Flatten embedded newlines into spaces (one physical console line). */
    for (std::size_t i = 0; msg[i] != '\0'; ++i) {
        if (msg[i] == '\r' || msg[i] == '\n') msg[i] = ' ';
    }
}

}  // namespace

extern "C" void pvz2_log_set_enabled(int enabled) { g_enabled = enabled ? 1 : 0; }

extern "C" int pvz2_log_enabled(void) { return g_enabled; }

extern "C" void pvz2_logv(const char *fmt, va_list ap) {
    if (!g_enabled || fmt == nullptr) return;

    char msg[1536];
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    sanitize_message(msg);
    if (msg[0] == '\0') return;

    char ts[40];
    format_timestamp(ts, sizeof(ts));

    std::lock_guard<std::mutex> lg(g_lock);
    std::printf("pvz2: {%s} %s\n", ts, msg);
    std::fflush(stdout);
}

extern "C" void pvz2_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    pvz2_logv(fmt, ap);
    va_end(ap);
}
