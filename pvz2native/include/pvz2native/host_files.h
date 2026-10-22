#ifndef PVZ2NATIVE_HOST_FILES_H
#define PVZ2NATIVE_HOST_FILES_H

#include <cstdint>
#include <string>
#include <vector>

/* Whole-file reads and writes on the host, for the port's own files (saves, CDN copies, prefs). No
 * function throws; a failure is reported through the return value. */
namespace pvz2native {
namespace host_files {

/* True when the file exists and can be opened for reading. */
bool readable(const std::string &path);

/* The whole file, or empty when it is missing, empty or unreadable. */
std::vector<std::uint8_t> read(const std::string &path);

/* Writes `body` to `path`, creating the parent folders and replacing an existing file. */
bool write(const std::string &path, const std::vector<std::uint8_t> &body);

/* Writes to a temporary file next to `path` and renames it over the target, so an interrupted write cannot
 * leave a half-written file behind. The target's folder must exist. */
bool write_atomic(const std::string &path, const std::vector<std::uint8_t> &body);

}  // namespace host_files
}  // namespace pvz2native

#endif
