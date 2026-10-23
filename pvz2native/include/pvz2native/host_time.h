#ifndef PVZ2NATIVE_HOST_TIME_H
#define PVZ2NATIVE_HOST_TIME_H

#include <cstdint>
#include <ctime>

/* Calendar conversions on the host, for the guest's libc emulation (dependencies/libc_time.cpp) and for the
 * engine-clock hook (game/patches.cpp). One implementation, so the two agree on what "local time" means. */
namespace pvz2native {
namespace host_time {

/* The calendar fields of `t` in the host's time zone / in UTC. */
std::tm local_tm(std::time_t t);
std::tm utc_tm(std::time_t t);

/* The UTC time a set of calendar fields denotes (the inverse of utc_tm), by pure arithmetic: it depends on
 * no time-zone state, unlike mktime, and exists on every host, unlike timegm. */
std::int64_t timegm(const std::tm &fields);

/* Seconds the host's local time is ahead of UTC at `t` (negative west of Greenwich, DST included). */
std::int64_t utc_offset(std::time_t t);

}  // namespace host_time
}  // namespace pvz2native

#endif
