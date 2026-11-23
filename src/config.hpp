#ifndef BX_CONFIG_HPP
#define BX_CONFIG_HPP

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace bx {

// One miner remembered on the devices screen. `address` identifies the device
// (unique, used by update_last_seen); `last_seen` is the Unix epoch seconds of
// the last successful poll, persisted so "offline · <duration>" survives a
// restart.
struct SavedDevice {
    std::string address;
    std::string hostname;
    std::int64_t last_seen;
};

// Name of the versioned device-config file within config_dir().
inline constexpr char devices_file_name[] = "devices-v1";

// $XDG_CONFIG_HOME/bitaxezero when XDG_CONFIG_HOME is set and absolute, else
// $HOME/.config/bitaxezero. Empty when neither is set (the caller can then
// treat save/load paths as unsaved).
std::filesystem::path config_dir();

// config_dir() / "devices-v1"; empty when config_dir() is empty.
std::filesystem::path devices_file();

// Reads the device config, a JSON array of
// {"address": str, "hostname": str, "last_seen": int}. A missing or unreadable
// file yields an empty vector; anything else that is not an array of such
// objects throws std::runtime_error.
std::vector<SavedDevice> load_devices(const std::filesystem::path& file);

// Writes the device config (create_directories on the parent, write to a
// unique temporary file, fsync, rename over the target). Throws
// std::runtime_error on failure.
void save_devices(const std::filesystem::path& file, const std::vector<SavedDevice>& devices);

// In-memory only: sets last_seen = now on every entry whose address matches.
// Nothing touches storage; callers persist by calling save_devices.
void update_last_seen(std::vector<SavedDevice>& devices, const std::string& address, std::int64_t now);

} // namespace bx

#endif /* BX_CONFIG_HPP */
