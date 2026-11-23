#include "config.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h> // ::open with O_DIRECTORY
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h> // ::write, ::fsync, ::close, ::mkstemp

namespace bx {
namespace {

std::runtime_error config_error(const std::filesystem::path& file, const std::string& what) {
    return std::runtime_error("bitaxezero: device config '" + file.string() + "': " + what);
}

// Best-effort fsync of the directory holding `dir`/`file`, so a rename is
// durable too. Errors are deliberately ignored: losing only the directory
// entry across a host crash recovers to the previous config.
void sync_parent_dir(const std::filesystem::path& dir) {
    const auto fd = ::open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
}

SavedDevice device_from_json(const nlohmann::json& entry, const std::filesystem::path& file) {
    if (!entry.is_object() || entry.size() != 3 || !entry.contains("address") || !entry.contains("hostname") ||
        !entry.contains("last_seen") || !entry["address"].is_string() || !entry["hostname"].is_string() ||
        !entry["last_seen"].is_number_integer())
        throw config_error(file, "expected {\"address\", \"hostname\", \"last_seen\"} strings/integer");
    return SavedDevice{
        entry["address"].get<std::string>(),
        entry["hostname"].get<std::string>(),
        entry["last_seen"].get<std::int64_t>(),
    };
}

} // namespace

std::filesystem::path config_dir() {
    if (const auto* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && std::filesystem::path(xdg).is_absolute())
        return std::filesystem::path(xdg) / "bitaxezero";
    if (const auto* home = std::getenv("HOME"); home && std::filesystem::path(home).is_absolute())
        return std::filesystem::path(home) / ".config" / "bitaxezero";
    return {};
}

std::filesystem::path devices_file() {
    const auto dir = config_dir();
    if (dir.empty()) return dir;
    return dir / devices_file_name;
}

std::vector<SavedDevice> load_devices(const std::filesystem::path& file) {
    std::error_code ec;
    const bool present = std::filesystem::exists(file, ec);
    if (ec || !present) return {};

    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    nlohmann::json doc;
    try {
        in >> doc;
    } catch (const nlohmann::json::exception& e) {
        throw config_error(file, "malformed JSON (" + std::string(e.what()) + ")");
    }
    if (!doc.is_array()) throw config_error(file, "not a JSON array");

    std::vector<SavedDevice> devices;
    devices.reserve(doc.size());
    for (const auto& entry : doc) devices.push_back(device_from_json(entry, file));
    return devices;
}

void save_devices(const std::filesystem::path& file, const std::vector<SavedDevice>& devices) {
    nlohmann::json doc = nlohmann::json::array();
    for (const auto& device : devices)
        doc.push_back(nlohmann::json{
            {"address", device.address},
            {"hostname", device.hostname},
            {"last_seen", device.last_seen},
        });
    const auto payload = doc.dump(2) + "\n";

    std::error_code ec;
    if (!file.parent_path().empty()) std::filesystem::create_directories(file.parent_path(), ec);
    if (ec) throw config_error(file, "cannot create directory " + file.parent_path().string());

    // Write to a unique temporary file, fsync it, then rename so a crash can
    // never leave a half-written config behind. The old config is only
    // replaced once the new data is fully on disk; on any earlier failure the
    // temporary file is removed and the previous config stays intact.
    auto temporary = file.string() + ".tmp.XXXXXX";
    const auto fd = ::mkstemp(temporary.data());
    if (fd < 0)
        throw config_error(file, std::string("mkstemp failed: ") + std::strerror(errno));

    std::string failure; // first failure's reason; rename happens only when empty
    std::size_t written = 0;
    while (written < payload.size()) {
        errno = 0;
        const auto count = ::write(fd, payload.data() + written, payload.size() - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            failure = std::string("write: ") + std::strerror(errno);
            break;
        }
        written += static_cast<std::size_t>(count);
    }
    if (failure.empty() && ::fsync(fd) != 0) failure = std::string("fsync: ") + std::strerror(errno);
    if (::close(fd) != 0 && failure.empty()) failure = std::string("close: ") + std::strerror(errno);

    if (failure.empty()) {
        std::filesystem::rename(temporary, file, ec);
        if (ec) failure = "rename: " + ec.message();
        else sync_parent_dir(file.parent_path()); // best effort; failure to fsync the directory is not fatal
    }

    if (!failure.empty()) {
        std::error_code remove_ec;
        std::filesystem::remove(temporary, remove_ec);
        throw config_error(file, "cannot write config (" + failure + ")");
    }
}

void update_last_seen(std::vector<SavedDevice>& devices, const std::string& address, std::int64_t now) {
    for (auto& device : devices)
        if (device.address == address) device.last_seen = now;
}

} // namespace bx
