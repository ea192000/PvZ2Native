#ifndef PVZ2NATIVE_LOG_H
#define PVZ2NATIVE_LOG_H

#include <stdarg.h>

/* Host-side log line used by GuestCall::log and by code that has no DexCall.
 *
 * Format:  pvz2: {YYYY-MM-DD HH:MM:SS} <message>\n
 *
 * Gated by pvz2_log_set_enabled (main applies [log] console / logfile). When
 * disabled, nothing is written -- same outcome as freopen(/dev/null) on the
 * console, without leaving ad-hoc printf that ignore the switch.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* 1 = print, 0 = silent. Starts at 1; main flips it after reading config.ini. */
void pvz2_log_set_enabled(int enabled);
int pvz2_log_enabled(void);

void pvz2_log(const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

void pvz2_logv(const char *fmt, va_list ap);

#ifdef __cplusplus
}
#endif

#endif
