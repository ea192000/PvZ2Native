/* See include/pvz2native/host_files.h. */

#include <pvz2native/host_files.h>

#include <filesystem>
#include <fstream>

namespace pvz2native {
namespace host_files {

bool readable(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    return (bool)f;
}

std::vector<std::uint8_t> read(const std::string &path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const auto n = f.tellg();
    if (n <= 0) return {};
    std::vector<std::uint8_t> out((std::size_t)n);
    f.seekg(0);
    f.read(reinterpret_cast<char *>(out.data()), n);
    if (!f) return {};
    return out;
}

namespace {

bool write_stream(const std::string &path, const std::vector<std::uint8_t> &body) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    if (!body.empty()) f.write(reinterpret_cast<const char *>(body.data()), (std::streamsize)body.size());
    return (bool)f;
}

}  // namespace

bool write(const std::string &path, const std::vector<std::uint8_t> &body) {
    std::error_code ec;
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    return write_stream(path, body);
}

bool write_atomic(const std::string &path, const std::vector<std::uint8_t> &body) {
    const std::string tmp = path + ".tmp_port";
    std::error_code ec;
    if (!write_stream(tmp, body)) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

}  // namespace host_files
}  // namespace pvz2native
