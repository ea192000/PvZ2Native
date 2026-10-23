/* See include/pvz2native/host_time.h. */

#include <pvz2native/host_time.h>

namespace pvz2native {
namespace host_time {

std::tm local_tm(std::time_t t) {
    std::tm out{};
#if defined(_WIN32)
    localtime_s(&out, &t);
#else
    localtime_r(&t, &out);
#endif
    return out;
}

std::tm utc_tm(std::time_t t) {
    std::tm out{};
#if defined(_WIN32)
    gmtime_s(&out, &t);
#else
    gmtime_r(&t, &out);
#endif
    return out;
}

std::int64_t timegm(const std::tm &fields) {
    /* Days from 1970-01-01 to the given y/m/d, by Howard Hinnant's days_from_civil. Valid for the whole
     * range of a 32-bit time_t and free of leap-year edge cases, which hand-rolled loops routinely get
     * wrong around 1900/2000. */
    int y = fields.tm_year + 1900;
    const unsigned m = (unsigned)fields.tm_mon + 1;
    const unsigned d = (unsigned)fields.tm_mday;
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    const std::int64_t days = (std::int64_t)era * 146097 + (std::int64_t)doe - 719468;
    return days * 86400 + fields.tm_hour * 3600 + fields.tm_min * 60 + fields.tm_sec;
}

std::int64_t utc_offset(std::time_t t) { return timegm(local_tm(t)) - (std::int64_t)t; }

}  // namespace host_time
}  // namespace pvz2native
