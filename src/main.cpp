// BitaxeZero entry point: LVGL init, the Cardputer Zero evdev keyboard (or
// SDL2 in the simulator), the MinerPoller wiring and the --smoke-test
// harness. The init/input/event-loop recipe is ported from CWTrainerZero's
// main.cpp (audio dropped - the Bitaxe UI has no sound path).

#include "lvgl.h"

#include "bitaxe.hpp"
#include "config.hpp"
#include "poller.hpp"
#include "screens.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#if BX_CARDPUTER_ZERO
#include "src/drivers/display/fb/lv_linux_fbdev.h"
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>
#else
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <SDL.h>

#include "stb_image_write.h"

// POSIX loopback plumbing for the smoke harness's canned HTTP server (the
// add-by-IP and poller steps run against it, loopback only - no live device,
// no LAN dependency).
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

constexpr int kWidth = 320, kHeight = 170;

// LVGL's tick source: milliseconds since this process started.
std::uint32_t ticks() {
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    return static_cast<std::uint32_t>(
        duration_cast<milliseconds>(steady_clock::now() - start).count());
}

using bx::MinerPoller;
using bx::SavedDevice;

enum Key {
    key_none, key_quit, key_devices, key_enter, key_rescan,
    key_add, key_remove, key_up, key_down,
};

// The app shell routes every key (see App::key); main.cpp only translates
// input codes into Key values, plus the typed characters App::type() feeds
// into the add-by-IP textarea. No handler here touches a blocking call: the
// rescan runs on its own background thread, the add probe is the app shell's
// submit step.

void refresh_from_poller(MinerPoller& poller,
                         const std::vector<SavedDevice>& devices,
                         bx::ui::MainScreen& screen, int highlight) {
    std::vector<bx::ui::DeviceRow> rows;
    rows.reserve(devices.size());
    for (const auto& device : devices)
        rows.push_back({device, poller.state(device.address)});
    bx::ui::FarmView farm;
    farm.hash_rate_gh = poller.farm_hashrate();
    farm.power = poller.farm_power();
    farm.efficiency = poller.farm_efficiency();
    farm.rig_count = static_cast<int>(devices.size());
    farm.online_count = poller.online_count();
    farm.network_ok = poller.network_ok();
    screen.update(rows, farm, std::time(nullptr), highlight);
}

// ---- the saved-device store: add / remove / persistence, one shared code
// path ----
//
// The devices screen (A: add by IP, X: remove) and the smoke harness drive
// the same DeviceStore: the address helpers below are its primitives only.

// The add-by-IP probe budget and the rescan's listen window (the rescan runs
// on its own background thread; the probe is the app shell's Enter submit).
constexpr int kAddProbeTimeoutMs = 2000;
constexpr int kRescanListenMs = 4000;

// The add-by-IP probe: GET /api/system/info on the esp-miner HTTP port.
// A blocking call, but only from the app shell's Enter handlers, worst case
// kAddProbeTimeoutMs, and the rescan path never comes through here.
using HttpFn = std::function<bx::HttpResult(const std::string&,
                                            const std::string&, int, int,
                                            std::string&)>;

struct AddProbe {
    bool ok = false;
    bx::MinerInfo info;
    std::string error;  // one line for the devices screen's inline error
};

AddProbe probe_add_target(const std::string& address, const HttpFn& http_fn) {
    std::string body;
    bx::MinerInfo info;
    const bx::HttpResult result = http_fn(address, "/api/system/info",
                                          bx::kMinerHttpPort,
                                          kAddProbeTimeoutMs, body);
    if (result == bx::HttpResult::ok &&
        bx::parse_miner_info(body, info)) {
        return {true, info, {}};
    }
    const char* reason =
        result == bx::HttpResult::timeout ? "timed out"
        : result == bx::HttpResult::refused ? "connection refused"
        : result == bx::HttpResult::dns_failed ? "address not found"
        : result == bx::HttpResult::ok ? "the API response made no sense"
        : "not a Bitaxe";
    return {false, {}, "no Bitaxe at " + address + " (" + reason + ")"};
}

// True when the address is already in the saved set.
bool address_saved(const std::vector<SavedDevice>& devices,
                   const std::string& address) {
    for (const auto& device : devices)
        if (device.address == address) return true;
    return false;
}

// Appends the probed device, or fills in the saved hostname of an existing
// still-unnamed entry. Returns true when the list changed.
bool insert_saved(std::vector<SavedDevice>& devices,
                  const std::string& address, const std::string& hostname,
                  std::int64_t last_seen) {
    for (auto& device : devices) {
        if (device.address != address) continue;
        if (device.hostname.empty() && !hostname.empty()) {
            device.hostname = hostname;
            return true;
        }
        return false;
    }
    devices.push_back({address, hostname, last_seen});
    return true;
}

// Removes one address; true when it was there to remove.
bool remove_saved(std::vector<SavedDevice>& devices,
                  const std::string& address) {
    for (std::size_t i = 0; i < devices.size(); ++i) {
        if (devices[i].address == address) {
            devices.erase(devices.begin() + static_cast<long>(i));
            return true;
        }
    }
    return false;
}

// The mDNS instance label is usually "Bitaxe Alpha 601 (ABCD)"; the display
// name is what is inside it. The literal label is the fallback.
std::string instance_name(const std::string& instance) {
    std::string out = instance;
    const std::string prefix = "Bitaxe ";
    if (out.rfind(prefix, 0) == 0) out.erase(0, prefix.size());
    if (out.size() >= 2 && out.back() == ')') {
        const auto open = out.find('(');
        if (open != std::string::npos) out.resize(open);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (out.empty()) return instance;
    return out;
}

// The saved-device store: the live configured list, the devices-v1 file it
// is saved through, and the exact paths the devices screen drives. The smoke
// harness instantiates it against a temporary file to pin the on-disk
// behaviour ("remove leaves one row fewer", "add lands the probed address").
//
// The write path is the same one poller persist callbacks run through
// (write_file), so a read-only config dir degrades to a stderr note instead
// of killing the app, and every mutation's write serializes against the
// callback writes on one mutex.
struct DeviceStore {
    explicit DeviceStore(std::filesystem::path file) : file_(std::move(file)) {}

    std::vector<SavedDevice> devices;  // the configured device list

    void write_file(const std::vector<SavedDevice>& list) {
        std::lock_guard<std::mutex> lock(persist_mutex_);
        try {
            bx::save_devices(file_, list);
        } catch (const std::exception& err) {
            std::cerr << "bitaxezero: cannot save " << file_.string() << ": "
                      << err.what() << '\n';
        } catch (...) {}
    }
    void persist() { write_file(devices); }

    // The add-by-IP path: dedupe check, probe, insert, persist. `http_fn`
    // lets the smoke harness point the probe at a canned loopback server;
    // the app shell passes &bx::http_get. `fallback_name` is the hostname
    // used when the device did not report one (the app shell passes the
    // address for A, the mDNS instance for a found row). With
    // `save_on_probe_failure` the device is saved anyway when the probe
    // flakes — for a FOUND row an mDNS announcement is evidence enough to
    // save with the instance name; the A path stays strictly probe-gated
    // because there the user supplied the address with nothing behind it.
    AddProbe add_by_ip(const std::string& address, const HttpFn& http_fn,
                       const std::string& fallback_name = "",
                       bool save_on_probe_failure = false) {
        if (address_saved(devices, address))
            return {false, {}, address + " is already saved"};
        AddProbe probe = probe_add_target(address, http_fn);
        bool named = probe.ok && !probe.info.hostname.empty();
        if (!probe.ok && !save_on_probe_failure) {
            // One inline error line; nothing was saved.
            return probe;
        }
        insert_saved(devices, address,
                     named ? probe.info.hostname
                           : (fallback_name.empty() ? address : fallback_name),
                     std::time(nullptr));
        persist();
        return probe;
    }

    // The remove path: erase one address and persist.
    bool remove(const std::string& address) {
        if (!remove_saved(devices, address)) return false;
        persist();
        return true;
    }

    std::filesystem::path file_path() const { return file_; }

  private:
    std::filesystem::path file_;   // the devices-v1 file persist() writes
    std::mutex persist_mutex_;     // App writes vs poller callback writes
};

// The devices-screen app shell: owns the poller, the three screens, the
// background rescan and the add-by-IP mode. Keys arrive as Key values; the
// typed keys (add mode) arrive through type().
class App {
public:
    explicit App(std::filesystem::path devices_file)
        : store_(std::move(devices_file)) {
        // Install before set_devices()/first use (see poller.hpp). The
        // callback fires from poller worker threads; it only writes the
        // file, never touches App state.
        poller_.persist_cb = [this](const std::vector<SavedDevice>& devices) {
            store_.write_file(devices);
        };
        store_.devices = bx::load_devices(store_.file_path());
        poller_.set_devices(store_.devices);
        screen_.create(lv_screen_active());
        detail_.create(lv_screen_active());
        devices_screen_.create(lv_screen_active());
        refresh_();
    }

    ~App() {
        // The rescan writes only found_ under its mutex; join so the app
        // can never outlive the thread it spawned.
        if (rescan_thread_.joinable()) rescan_thread_.join();
        // Final device-list write while every member still lives: the
        // poller's destructor flush (see poller.hpp) then finds the file
        // already current and runs against live store members.
        store_.persist();
    }

    void key(Key key_value) {
        if (add_mode_) {
            // In add mode the keyboard is a keyboard: the letters the key
            // table claims (S/A/X, F1) go to the textarea like everything
            // else, Enter submits, Esc backs out to the devices screen.
            switch (key_value) {
                case key_enter: submit_add_(); break;
                case key_quit: cancel_add_(); break;
                case key_rescan: devices_screen_.add_char('s'); break;
                case key_add: devices_screen_.add_char('a'); break;
                case key_remove: devices_screen_.add_char('x'); break;
                case key_devices: devices_screen_.add_char('1'); break;
                default: break;  // Up/Down stay navigation keys, not text
            }
            return;
        }
        if (detail_open_) {
            // Read-only while a detail is up: Esc returns to the devices
            // screen (if that is what opened it) or the farm, and clears
            // the focus.
            if (key_value == key_quit) close_detail_();
            return;
        }
        if (devices_open_) {
            switch (key_value) {
                case key_quit: close_devices_(); break;
                case key_up: move_highlight_(-1); break;
                case key_down: move_highlight_(1); break;
                case key_enter: open_highlighted_(); break;
                case key_add: enter_add_mode_(); break;
                case key_remove: remove_highlighted_(); break;
                case key_rescan: start_rescan_(); break;
                default: break;  // F1 on the devices screen: no-op
            }
            return;
        }
        // Main screen.
        switch (key_value) {
            case key_quit: running = false; break;
            case key_devices: open_devices_(); break;
            case key_rescan: open_devices_(); start_rescan_(); break;
            case key_enter:
                if (main_highlight_ >= 0 &&
                    main_highlight_ < static_cast<int>(store_.devices.size()))
                    open_detail_(store_.devices[static_cast<std::size_t>(
                                     main_highlight_)].address);
                break;
            case key_up: move_main_highlight_(-1); break;
            case key_down: move_main_highlight_(1); break;
            default: break;
        }
    }

    // One typed character ('\\b' = backspace). Only the add-by-IP mode
    // consumes it; everywhere else it is a no-op.
    void type(char ch) {
        if (!add_mode_ || ch == 0) return;
        if (ch == '\b') devices_screen_.add_backspace();
        else devices_screen_.add_char(ch);
    }

    void tick() {
        const std::uint32_t now = ticks();
        if (now - last_refresh_ms_ >= kRefreshMs) {
            last_refresh_ms_ = now;
            refresh_();
            // The detail screen repaints from the same state; with focus set
            // the poller delivers that device's freshest numbers on its own
            // 2 s cadence.
            if (detail_open_) refresh_detail_();
            if (devices_open_) refresh_devices_();
        }
    }

    bool running = true;

private:
    static constexpr std::uint32_t kRefreshMs = 500;  // UI repaint cadence

    void refresh_() {
        refresh_from_poller(poller_, store_.devices, screen_, main_highlight_);
    }

    void refresh_detail_() {
        for (const auto& device : store_.devices) {
            if (device.address == detail_address_) {
                detail_.update({device, poller_.state(detail_address_)});
                return;
            }
        }
        // The focused device vanished from the set (it can only be X-removed,
        // and remove_highlighted_ closes the detail first): nothing to
        // repaint, the closed focus state stays.
    }

    void refresh_devices_() {
        std::vector<bx::ui::DeviceRow> rows;
        rows.reserve(store_.devices.size());
        for (const auto& device : store_.devices)
            rows.push_back({device, poller_.state(device.address)});
        std::vector<bx::DiscoveredDevice> found;
        {
            std::lock_guard<std::mutex> lock(found_mutex_);
            found.reserve(found_.size());
            for (const auto& device : found_)
                if (!address_saved(store_.devices, device.address))
                    found.push_back(device);
        }
        const int total = static_cast<int>(rows.size() + found.size());
        if (highlight_ >= total) highlight_ = total - 1;
        // The lower clamp too: open_devices_ clears the highlight and this
        // refresh must end with the first available row highlighted (or -1
        // only when nothing can be shown) - Enter and X act on the row the
        // user sees.
        if (highlight_ < 0) highlight_ = total > 0 ? 0 : -1;
        devices_screen_.update(rows, found, std::time(nullptr),
                               rescanning_.load(), add_mode_, add_error_,
                               highlight_);
    }

    void open_detail_(const std::string& address) {
        if (address.empty()) return;
        detail_address_ = address;
        poller_.set_focus(address);  // 2 s poll cadence while the screen shows
        detail_.update({saved_for_(address), poller_.state(address)});
        detail_.show();
        detail_open_ = true;
    }

    void close_detail_() {
        poller_.set_focus("");  // back to the farm cadence
        detail_address_.clear();
        detail_.hide();
        detail_open_ = false;
    }

    SavedDevice saved_for_(const std::string& address) const {
        for (const auto& device : store_.devices)
            if (device.address == address) return device;
        return {};
    }

    // --- devices screen ---

    void open_devices_() {
        devices_open_ = true;
        highlight_ = -1;  // refresh_devices_ clamps to the first available
        devices_screen_.show();
        refresh_devices_();
    }

    void close_devices_() {
        if (add_mode_) cancel_add_();
        devices_screen_.hide();
        devices_open_ = false;
    }

    void move_highlight_(int delta) {
        // Combined index: SAVED rows first, then FOUND rows.
        int found_count = 0;
        {
            std::lock_guard<std::mutex> lock(found_mutex_);
            for (const auto& device : found_)
                if (!address_saved(store_.devices, device.address)) ++found_count;
        }
        const int total = static_cast<int>(store_.devices.size()) + found_count;
        if (total == 0) {
            highlight_ = -1;
            return;
        }
        highlight_ = highlight_ < 0 ? (delta > 0 ? 0 : total - 1)
                                    : highlight_ + delta;
        if (highlight_ < 0) highlight_ = 0;
        if (highlight_ >= total) highlight_ = total - 1;
    }

    void move_main_highlight_(int delta) {
        if (store_.devices.empty()) return;
        main_highlight_ = main_highlight_ + delta;
        if (main_highlight_ < 0) main_highlight_ = 0;
        if (main_highlight_ >= static_cast<int>(store_.devices.size()))
            main_highlight_ = static_cast<int>(store_.devices.size()) - 1;
        refresh_();
    }

    // Enter: the highlighted SAVED row opens its detail; the highlighted
    // FOUND row is added (probed, saved, pushed to the poller). The found
    // index walks the same filtered list the screen displays: entries whose
    // address is already saved never count as its rows, so a highlighted
    // found row is always the displayed unsaved device.
    void open_highlighted_() {
        const int saved_count = static_cast<int>(store_.devices.size());
        if (highlight_ >= 0 && highlight_ < saved_count) {
            open_detail_(store_.devices[static_cast<std::size_t>(highlight_)]
                             .address);
            return;
        }
        if (highlight_ < 0) return;
        bx::DiscoveredDevice device;
        {
            std::lock_guard<std::mutex> lock(found_mutex_);
            std::vector<bx::DiscoveredDevice> displayed;
            for (const auto& entry : found_)
                if (!address_saved(store_.devices, entry.address))
                    displayed.push_back(entry);
            const int index = highlight_ - saved_count;
            if (index >= 0 && index < static_cast<int>(displayed.size()))
                device = displayed[static_cast<std::size_t>(index)];
        }
        if (!device.address.empty()) add_discovered_(device);
    }

    void add_discovered_(const bx::DiscoveredDevice& device) {
        // The device announced itself over mDNS: the probe only upgrades the
        // saved name to the API hostname, and a flaked probe still saves
        // with the instance label (announce = evidence; the A path is the
        // strictly probe-gated one). Found rows are re-filtered away on the
        // next refresh.
        const std::string address = device.address;
        const bool already = address_saved(store_.devices, address);
        store_.add_by_ip(address, &bx::http_get,
                         instance_name(device.instance),
                         /*save_on_probe_failure=*/true);
        if (!already) {
            poller_.set_devices(store_.devices);
            // The set_devices swap may leave a persist callback pending from
            // the pre-swap device list; re-write the configured list now so
            // that stale flush (fired after the swap) cannot resurrect the
            // removed / drop the added device if the app dies in between.
            store_.persist();
        }
        // probe.error is deliberately not surfaced on this path: the row is
        // saved either way, its name possibly only announcing itself.
    }

    void remove_highlighted_() {
        // X acts only on SAVED rows.
        if (highlight_ < 0 || highlight_ >= static_cast<int>(store_.devices.size()))
            return;
        const std::string address =
            store_.devices[static_cast<std::size_t>(highlight_)].address;
        // Guard: a removed device must never leave behind a detail screen
        // focused on it.
        if (detail_open_ && detail_address_ == address) close_detail_();
        if (store_.remove(address)) {
            poller_.set_devices(store_.devices);
            // Same hardened swap as add_discovered_: the store's persist
            // wrote the shrunk list, the poller is pointed at it, and this
            // re-write supersedes any stale pre-swap flush.
            store_.persist();
        }
        // Up/Down state for the main screen: with one device gone the old
        // index may point past the end; clamp so returning to the farm
        // always has a highlighted card while one remains (and not below 0).
        if (main_highlight_ >= static_cast<int>(store_.devices.size()))
            main_highlight_ =
                static_cast<int>(store_.devices.size()) - 1;
    }

    // --- add by IP ---

    void enter_add_mode_() {
        add_mode_ = true;
        add_error_.clear();
        devices_screen_.add_clear();
        refresh_devices_();
    }

    void cancel_add_() {
        add_mode_ = false;
        add_error_.clear();
        refresh_devices_();
    }

    void submit_add_() {
        // Trim one-line whitespace so a stray space cannot produce an
        // "address not found" for a valid IP.
        const std::string address = trim(devices_screen_.add_text());
        if (address.empty()) {
            add_error_ = "type an IP address first - enter saves, esc cancels";
            refresh_devices_();
            return;
        }
        // The store's add path: dedupe check ("already saved"), then the
        // probe + insert + persist. The probe blocks this handler for at
        // most the 2 s timeout; that is the documented add-by-IP behaviour
        // (only the rescan is async).
        const AddProbe probe = store_.add_by_ip(address, &bx::http_get);
        if (!probe.ok) {
            // One inline line ("... is already saved" / the probe reason);
            // nothing was saved, the screen stays in add mode.
            add_error_ = probe.error;
            refresh_devices_();
            return;
        }
        poller_.set_devices(store_.devices);
        // Same hardened swap as add_discovered_: supersede a flushed
        // pre-swap stale persist with a final write of the new list.
        store_.persist();
        // Success leaves add mode, and the SAVED section shows the fetched
        // hostname immediately (this refresh, not the next tick).
        add_mode_ = false;
        add_error_.clear();
        refresh_devices_();
    }

    // The rescan worker: one at a time (the busy flag gates it), and the
    // flags unwind cleanly if thread construction throws (out of memory,
    // thread cap) so a later S can still start one.
    void start_rescan_() {
        bool idle = false;
        if (rescanning_.compare_exchange_strong(idle, true)) {
            if (rescan_thread_.joinable()) rescan_thread_.join();
            try {
                rescan_thread_ = std::thread([this] {
                    auto devices = bx::discover_baxes(kRescanListenMs);
                    {
                        std::lock_guard<std::mutex> lock(found_mutex_);
                        found_ = std::move(devices);
                    }
                    rescanning_.store(false);
                });
            } catch (const std::system_error& err) {
                rescanning_.store(false);  // one line, back to idle
                std::cerr << "bitaxezero: rescan unavailable: " << err.what()
                          << '\n';
            }
        }
    }

    static std::string trim(const std::string& text) {
        const auto first = text.find_first_not_of(" \t");
        if (first == std::string::npos) return {};
        const auto last = text.find_last_not_of(" \t");
        return text.substr(first, last - first + 1);
    }

    // Member order is the teardown contract: store_ is declared before
    // poller_ and therefore destroyed after it, so the poller's destructor
    // flush (its final persist callback) runs while the store — and its
    // mutex — still exist. The ~App body's final persist already made that
    // a no-op on the file, but the ordering keeps the callback path safe
    // even if a worker records a last_seen in the window between.
    DeviceStore store_;
    MinerPoller poller_;
    bx::ui::MainScreen screen_;
    bx::ui::DetailScreen detail_;
    bx::ui::DevicesScreen devices_screen_;
    std::string detail_address_;
    bool detail_open_ = false;
    bool devices_open_ = false;
    bool add_mode_ = false;
    std::string add_error_;
    int main_highlight_ = 0;
    int highlight_ = -1;

    // The rescan: one worker at a time (busy flag), results under the mutex
    // that the devices refresh reads.
    std::thread rescan_thread_;
    std::atomic<bool> rescanning_{false};
    std::mutex found_mutex_;
    std::vector<bx::DiscoveredDevice> found_;

    std::uint32_t last_refresh_ms_ = 0;
};

void idle() { std::this_thread::sleep_for(std::chrono::milliseconds(4)); }

#if BX_CARDPUTER_ZERO

// Cardputer Zero key mapping (the internal TCA8418 matrix reports standard
// KEY_* codes; the launcher's Fn layer produces KEY_F1). Unclaimed keys can
// still mean text: keycode_char() feeds the add-by-IP textarea below.
Key map_key(unsigned code) {
    switch (code) {
        case KEY_ESC: return key_quit;
        case KEY_F1: return key_devices;
        case KEY_ENTER: return key_enter;
        case KEY_S: return key_rescan;
        case KEY_A: return key_add;
        case KEY_X: return key_remove;
        case KEY_UP: return key_up;
        case KEY_DOWN: return key_down;
        default: return key_none;
    }
}

// The TCA8418 matrix controller emits its own raw event codes for the
// symbol keys, and those collide with standard KEY_* numbers - the physical
// dot key arrives as code 91 (standard KEY_DOT is 52, which the controller
// uses for '*'). Ported verbatim from M5Stack's FactoryTest
// src/platform/linux_input.cpp K_TCA8418_SYMBOL_KEYS, and checked
// FIRST in keycode_char() below, exactly as that translator's
// printable_evdev_key() does (TCA table, then standard symbols, then
// alpha). Verified against the on-device FactoryTest translation.
struct TcaSymbolKey {
    unsigned code;
    char ch;
};
constexpr std::array<TcaSymbolKey, 32> kTca8418SymbolKeys{{
    {26, '!'}, {27, '@'}, {39, '#'}, {40, '$'}, {41, '%'}, {43, '^'},
    {51, '&'}, {52, '*'}, {53, '('}, {94, ')'}, {55, '~'}, {69, '`'},
    {70, '_'}, {71, '-'}, {72, '+'}, {73, '='}, {74, '['}, {75, ']'},
    {76, '{'}, {77, '}'}, {79, ';'}, {80, ':'}, {81, '\''}, {82, '"'},
    {83, '<'}, {85, '>'}, {86, '\\'}, {89, '|'}, {90, ','}, {91, '.'},
    {92, '/'}, {93, '?'},
}};

// The character a plain KEY_* press types (no shift layer): letters, digits,
// the punctuation an IP address may need. 0 for keys that type nothing.
char keycode_char(unsigned code) {
    // TCA8418 first: these codes shadow standard KEY_* numbers (52 = '*',
    // 91 = '.', 90 = ',', 92 = '/'), so the collision must win.
    for (const TcaSymbolKey& entry : kTca8418SymbolKeys) {
        if (entry.code == code) return entry.ch;
    }
    switch (code) {
        case KEY_SPACE: return ' ';
        case KEY_MINUS: return '-';
        case KEY_DOT: return '.';
        case KEY_COMMA: return ',';
        case KEY_SLASH: return '/';
        case KEY_BACKSPACE: return '\b';
        default: break;
    }
    if (code >= KEY_A && code <= KEY_Z) return static_cast<char>('a' + (code - KEY_A));
    if (code == KEY_0) return '0';
    if (code >= KEY_1 && code <= KEY_9)
        return static_cast<char>('1' + (code - KEY_1));
    return 0;
}

// Cardputer Zero keyboard: pick the one evdev device that can type (the
// heuristic filters out the touch panel and friends), remembering whether it
// is the internal TCA8418 matrix - ported unchanged from CWTrainerZero.
class DeviceKeyboard {
public:
    explicit DeviceKeyboard(const std::string& named) {
        if (!named.empty()) { open_candidate(named.c_str()); return; }
        const auto* dir_path = "/dev/input";
        auto* dir = opendir(dir_path);
        if (!dir) return;
        while (auto* entry = readdir(dir)) {
            if (std::strncmp(entry->d_name, "event", 5) != 0) continue;
            if (open_candidate((std::string(dir_path) + "/" + entry->d_name).c_str()))
                break;
        }
        closedir(dir);
    }
    ~DeviceKeyboard() { if (fd_ >= 0) close(fd_); }
    bool valid() const { return fd_ >= 0; }
    void dispatch(App& app) {
        input_event event{};
        while (::read(fd_, &event, sizeof(event)) == sizeof(event)) {
            if (event.type != EV_KEY || event.value != 1) continue;
            const Key key = map_key(event.code);
            if (key != key_none) {
                app.key(key);
            } else if (const char ch = keycode_char(event.code)) {
                app.type(ch);
            }
        }
    }
private:
    int fd_ = -1;
    bool open_candidate(const char* path) {
        const auto fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) return false;
        constexpr auto bits = sizeof(unsigned long) * 8;
        std::array<unsigned long, KEY_MAX / bits + 1> keys{};
        const auto has = [&](unsigned n){ return keys[n / bits] & (1UL << (n % bits)); };
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys.data()) < 0 ||
            !has(KEY_K) || !has(KEY_M) || !has(KEY_ENTER) || !has(KEY_SPACE)) {
            close(fd); return false;
        }
        fd_ = fd;
        return true;
    }
};

#else

// Desktop key mapping. Esc quits (or backs out of the detail / devices
// screens), Enter opens the detail (or saves in add mode), F1 opens the
// devices screen, S rescans, A/X drive the devices screen, Up/Down move the
// card / row highlight. Unclaimed printable keys reach App::type().
Key map_key(SDL_Keycode key) {
    switch (key) {
        case SDLK_ESCAPE: return key_quit;
        case SDLK_RETURN: case SDLK_KP_ENTER: return key_enter;
        case SDLK_F1: return key_devices;
        case SDLK_s: return key_rescan;  // SDLK keycodes are lowercase
        case SDLK_a: return key_add;
        case SDLK_x: return key_remove;
        case SDLK_UP: return key_up;
        case SDLK_DOWN: return key_down;
        default: return key_none;
    }
}

class Desktop {
public:
    explicit Desktop(bool hidden) {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) fail();
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
        window_ = SDL_CreateWindow("bitaxezero (320 x 170 simulator)",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            kWidth * 2, kHeight * 2, hidden ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN);
        if (!window_) fail();
        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED);
        if (!renderer_) renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
        if (!renderer_) fail();
        SDL_RenderSetLogicalSize(renderer_, kWidth, kHeight);
        texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGB565,
            SDL_TEXTUREACCESS_STREAMING, kWidth, kHeight);
        if (!texture_) fail();
        display_ = lv_display_create(kWidth, kHeight);
        if (!display_) throw std::runtime_error("LVGL display creation failed");
        lv_display_set_color_format(display_, LV_COLOR_FORMAT_RGB565);
        lv_display_set_user_data(display_, this);
        lv_display_set_buffers(display_, buffer_.data(), nullptr,
            sizeof(buffer_), LV_DISPLAY_RENDER_MODE_FULL);
        lv_display_set_flush_cb(display_,
            [](lv_display_t* display, const lv_area_t*, std::uint8_t* px){
                auto& ui = *static_cast<Desktop*>(lv_display_get_user_data(display));
                SDL_UpdateTexture(ui.texture_, nullptr, px, kWidth * 2);
                SDL_RenderClear(ui.renderer_);
                SDL_RenderCopy(ui.renderer_, ui.texture_, nullptr, nullptr);
                SDL_RenderPresent(ui.renderer_);
                lv_display_flush_ready(display);
            });
    }

    ~Desktop() {
        if (display_) lv_display_delete(display_);
        if (texture_) SDL_DestroyTexture(texture_);
        if (renderer_) SDL_DestroyRenderer(renderer_);
        if (window_) SDL_DestroyWindow(window_);
        SDL_Quit();
    }

    void dispatch(App& app) {
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) app.key(key_quit);
            if (event.type != SDL_KEYDOWN || event.key.repeat) continue;
            const SDL_Keycode sym = event.key.keysym.sym;
            const Key key = map_key(sym);
            if (key != key_none) {
                app.key(key);
            } else if (sym == SDLK_BACKSPACE) {
                app.type('\b');
            } else if (sym >= SDLK_SPACE && sym <= 0x7e) {
                // printable-sym keys: digits, '.', a-z, etc. feed the
                // add-by-IP textarea (no-op outside add mode).
                app.type(static_cast<char>(sym & 0xff));
            }
        }
    }

    // Renders a pending frame and writes it as PNG (320 x 170) through
    // stb_image_write - the smoke harness's artifact.
    void screenshot(const std::filesystem::path& path) {
        lv_refr_now(display_);
        std::vector<std::uint8_t> rgb(3 * kWidth * kHeight);
        for (std::size_t i = 0; i < buffer_.size(); ++i) {
            const auto pixel = buffer_[i];
            const std::uint8_t r5 = (pixel >> 11) & 0x1f;
            const std::uint8_t g6 = (pixel >> 5) & 0x3f;
            const std::uint8_t b5 = pixel & 0x1f;
            rgb[3 * i + 0] = (r5 << 3) | (r5 >> 2);
            rgb[3 * i + 1] = (g6 << 2) | (g6 >> 4);
            rgb[3 * i + 2] = (b5 << 3) | (b5 >> 2);
        }
        if (!stbi_write_png(path.c_str(), kWidth, kHeight, 3, rgb.data(),
                            3 * kWidth))
            throw std::runtime_error("cannot write " + path.string());
    }

private:
    [[noreturn]] static void fail() { throw std::runtime_error(SDL_GetError()); }
    alignas(64) std::array<std::uint16_t, kWidth * kHeight> buffer_{};
    SDL_Window* window_{};
    SDL_Renderer* renderer_{};
    SDL_Texture* texture_{};
    lv_display_t* display_{};
};

void require(bool valid, std::string_view description) {
    if (!valid) throw std::runtime_error(std::string(description));
}

// ---- smoke: helpers shaped after tests/test_core.cpp's canned-server and
// wait_until helpers so the devices step's add / remove / poll assertions run
// against loopback HTTP only, with no live device and no LAN dependency ----

// Polls until done() holds or the deadline passes; the last attempt counts.
template <typename Done>
bool smoke_wait_until(Done done, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return done();
}

// A canned esp-miner HTTP server on an ephemeral loopback port (bound to 0,
// the OS picks the port). Serves the same fixture document to every accepted
// connection until finish() (also run by the destructor) closes the listener
// and joins, so the smoke run can never outlive the handler thread.
class CannedMinerServer {
public:
    explicit CannedMinerServer(const std::filesystem::path& fixture) {
        std::ifstream in(fixture);
        body_.assign((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
        require(!body_.empty(), "canned server fixture document: " +
                                    fixture.string());
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        require(listen_fd_ >= 0, "canned server socket");
        const int reuse = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse,
                     sizeof(reuse));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);  // 127.0.0.1
        addr.sin_port = 0;
        require(::bind(listen_fd_, reinterpret_cast<const sockaddr*>(&addr),
                       sizeof(addr)) == 0,
                "canned server bind");
        require(::listen(listen_fd_, 4) == 0, "canned server listen");
        socklen_t addrlen = sizeof(addr);
        require(::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr),
                              &addrlen) == 0,
                "canned server port");
        port_ = ::ntohs(addr.sin_port);
        worker_ = std::thread([this] {
            // One accept per connection until finish(); every connection the
            // smoke drives (the probe, then the poller's polls) gets the
            // same canned answer.
            while (running_.load()) {
                pollfd watching{};
                watching.fd = listen_fd_;
                watching.events = POLLIN;
                if (::poll(&watching, 1, 100) <= 0) continue;
                if (!running_.load()) break;
                const int fd = ::accept(listen_fd_, nullptr, nullptr);
                if (fd < 0) continue;
                serve_one_(fd);
                ::close(fd);
            }
        });
    }

    CannedMinerServer(const CannedMinerServer&) = delete;
    CannedMinerServer& operator=(const CannedMinerServer&) = delete;

    ~CannedMinerServer() { finish(); }

    int port() const { return port_; }

    void finish() {
        running_.store(false);
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        if (worker_.joinable()) worker_.join();
    }

private:
    // Read the request head (always ends at the blank line), answer with the
    // canned esp-miner document, 200 with Content-Length.
    void serve_one_(int fd) {
        std::string request;
        char buffer[512];
        while (request.find("\r\n\r\n") == std::string::npos) {
            const ssize_t got = ::recv(fd, buffer, sizeof(buffer), 0);
            if (got <= 0) break;
            request.append(buffer, static_cast<std::size_t>(got));
        }
        std::ostringstream head;
        head << "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n"
             << "Content-Length: " << body_.size() << "\r\n\r\n";
        const std::string out = head.str() + body_;
        std::size_t offset = 0;
        while (offset < out.size()) {
            const ssize_t sent = ::send(fd, out.data() + offset,
                                        out.size() - offset, 0);
            if (sent <= 0) break;
            offset += static_cast<std::size_t>(sent);
        }
    }

    std::string body_;
    std::thread worker_;
    std::atomic<bool> running_{true};
    int listen_fd_ = -1;
    int port_ = 0;
};

// --smoke-test <dir> <fixtures>: renders the main screen from a fixed
// synthetic farm state built from the committed esp-miner fixtures (two
// online devices, one that never answers) and saves smoke-main.png. No
// poller is started and nothing touches the network.
int smoke(const std::filesystem::path& directory,
          const std::filesystem::path& fixtures) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) throw std::runtime_error("cannot create " + directory.string() +
                                     ": " + ec.message());

    const auto load_fixture = [&](const char* name) {
        const auto path = fixtures / name;
        std::ifstream in(path);
        if (!in) throw std::runtime_error("missing fixture " + path.string());
        const std::string json((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        bx::MinerInfo info;
        if (!bx::parse_miner_info(json, info))
            throw std::runtime_error("fixture unparseable: " + path.string());
        return info;
    };
    const auto gamma601 = load_fixture("device-v2.15.3.json");
    const auto turbo = load_fixture("device-v2.12.json");

    // Pinned wall clock so the offline card's duration is deterministic:
    // one hour of darkness renders as "1h00m".
    constexpr std::int64_t offline_last_seen = 1700000456;
    constexpr std::int64_t now = offline_last_seen + 3600;

    std::vector<bx::ui::DeviceRow> rows;
    rows.push_back({{"192.0.2.12", gamma601.hostname, now},
                    {gamma601, true, now}});
    rows.push_back({{"192.0.2.50", turbo.hostname, now},
                    {turbo, true, now}});
    rows.push_back({{"192.0.2.51", "Delta 800", offline_last_seen},
                    {bx::MinerInfo{}, false, offline_last_seen}});

    bx::ui::FarmView farm;  // what the poller would report for these two:
    // the display field, the 10-minute averages (same preference the poller
    // totals and the cards render with, so the hero sums the cards exactly).
    farm.hash_rate_gh = gamma601.display_hashrate() + turbo.display_hashrate();
    farm.power = gamma601.power + turbo.power;
    farm.efficiency = farm.power / (farm.hash_rate_gh / 1000.0);
    farm.rig_count = 3;
    farm.online_count = 2;
    farm.network_ok = true;

    Desktop desktop(true);
    bx::ui::MainScreen screen;
    screen.create(lv_screen_active());
    screen.update(rows, farm, now);
    desktop.screenshot(directory / "smoke-main.png");

    const auto cards = screen.card_texts();
    require(cards.size() == 3, "three device cards");
    const auto hero = screen.hero_text();
    const auto expected_hero =
        bx::format_hashrate(gamma601.display_hashrate() +
                            turbo.display_hashrate());
    // Pinned: the committed fixtures' 10m averages sum to 3.23 TH/s.
    require(expected_hero == "3.23 TH/s", "fixture farm hero pinned");
    require(hero == expected_hero, "hero label carries the farm hash rate");
    for (const auto& card : cards) {
        require(card.find("NaN") == std::string::npos,
            "no card shows NaN: " + card);
        require(card.find("0.00 TH/s") == std::string::npos,
            "unknown fields never render as 0.00 TH/s: " + card);
        require(card.find("TH/s") != std::string::npos || card.find("offline") != std::string::npos,
            "online cards carry a hash rate: " + card);
    }
    require(cards[2].find("offline") != std::string::npos,
        "third card shows offline");
    require(cards[2].find("1h00m") != std::string::npos,
        "offline duration from the pinned last_seen gap");
    require(cards[0].find("NaN") == std::string::npos &&
        cards[0].find(bx::ui::display_text(
            bx::format_hashrate(gamma601.display_hashrate()))) !=
            std::string::npos,
        "first card carries the v2.15.3 10m average");

    // --- detail step (Task 8), same fixture state, no poller started ---
    bx::ui::DetailScreen detail_view;  // covers the screen once shown below
    detail_view.create(lv_screen_active());
    detail_view.update(rows[0]);
    detail_view.show();
    desktop.screenshot(directory / "smoke-detail.png");

    require(gamma601.shares_accepted == 1501,
        "fixture sharesAccepted the detail asserts rely on");
    const auto title = detail_view.title_text();
    const auto lines = detail_view.line_texts();
    require(title.find("Alpha 601") != std::string::npos,
        "detail title is the device name");
    require(lines.size() == 8,
        "hash rate + card line + six read-only section lines");
    require(lines[2].find("example.org") != std::string::npos &&
            lines[2].find("3333") != std::string::npos,
        "pool line carries the fixture stratum host and port: " + lines[2]);
    require(lines[4].find(std::to_string(gamma601.shares_accepted)) !=
            std::string::npos,
        "shares line carries the fixture accepted count: " + lines[4]);
    require(lines[1].find("48\xC2\xB0") != std::string::npos &&
            lines[1].find("8210") != std::string::npos,
        "card line pinned through the card_detail path: " + lines[1]);
    for (const auto& line : lines) {
        require(line.find("0.00") == std::string::npos,
            "detail never renders 0.00 (blank-not-zero): " + line);
        require(line.find("NaN") == std::string::npos,
            "detail never renders NaN: " + line);
        require(line.find("\xC2\xB0\xC2\xB0") == std::string::npos,
            "no doubled degree sign: " + line);
    }
    require(lines[5].find("1.71G") != std::string::npos,
        "best line carries the session difficulty: " + lines[5]);
    require(lines[6].find("27d08h") != std::string::npos,
        "latency/uptime line carries the fixture uptime: " + lines[6]);
    require(lines[4].find("6.4%") != std::string::npos,
        "shares line carries the fixture error percent: " + lines[4]);
    require(lines[7].find("v2.15.3") != std::string::npos &&
            lines[7].find("board 601") != std::string::npos &&
            lines[7].find("BM1370") != std::string::npos,
        "firmware line carries fixture version, board and ASIC: " + lines[7]);
    require(lines[3].find("WorkerAlpha") != std::string::npos,
        "worker line carries the fixture pool user: " + lines[3]);
    require(lines[6].find("18") != std::string::npos,
        "latency line carries the fixture responseTime ms: " + lines[6]);

    // --- devices step (Task 9), same fixture state, no poller / no network:
    // the FOUND section is injected as a deterministic DiscoveredDevice.
    bx::ui::DevicesScreen devices_view;
    devices_view.create(lv_screen_active());
    bx::DiscoveredDevice announced;
    announced.instance = "Bitaxe Alpha 601 (ABCD)";
    announced.host = "Alpha601.local";
    announced.address = "192.0.2.77";
    announced.port = 80;
    announced.txt = {{"asic", "BM1370"}, {"family", "Gamma"},
                     {"board", "601"}};
    const std::vector<bx::DiscoveredDevice> found = {announced};
    devices_view.show();
    devices_view.update(rows, found, now, /*scanning=*/false,
                        /*add_mode=*/false, "", /*highlight=*/0);
    desktop.screenshot(directory / "smoke-devices.png");

    const auto saved_rows = devices_view.saved_texts();
    const auto found_rows = devices_view.found_texts();
    const auto labels = devices_view.label_texts();
    require(saved_rows.size() == 3, "three saved rows on the devices screen");
    require(found_rows.size() == 1, "one found row on the devices screen");
    bool found_label = false;
    for (const auto& label : labels)
        if (label.find("FOUND ON NETWORK") != std::string::npos)
            found_label = true;
    require(found_label, "FOUND ON NETWORK section label present");
    bool remove_hint = false;
    for (const auto& row : saved_rows)
        if (row.find("X remove") != std::string::npos) remove_hint = true;
    require(remove_hint, "'X remove' hint on the highlighted saved row");
    require(saved_rows[0].find(bx::ui::display_text(
                bx::format_hashrate(gamma601.display_hashrate()))) !=
                std::string::npos,
        "saved row shows its live hash rate: " + saved_rows[0]);
    require(saved_rows[2].find("offline • 1h00m") != std::string::npos,
        "offline saved row shows the pinned duration: " + saved_rows[2]);
    require(found_rows[0].find("Bitaxe Alpha 601 (ABCD)") != std::string::npos &&
                found_rows[0].find("192.0.2.77") != std::string::npos,
        "found row carries the discovered instance and address: " +
            found_rows[0]);
    require(found_rows[0].find("BM1370 • Gamma") != std::string::npos,
        "found row carries the TXT ASIC/family summary: " + found_rows[0]);

    // --- NO NETWORK state (Task 10), the WiFi-off hero on the dashboard:
    // same saved rows, network_ok false, nothing touched the network.
    devices_view.hide();
    detail_view.hide();
    bx::ui::FarmView no_network = farm;
    no_network.network_ok = false;
    screen.update(rows, no_network, now);
    desktop.screenshot(directory / "smoke-network.png");
    require(screen.hero_text() == "NO NETWORK",
        "no-network hero replaces the farm hash rate");

    // --- the app's real store mutations, against a canned loopback server
    // (no live device, no LAN dependency): the X remove shrinks the
    // devices-v1 file on disk, the A add-by-IP drives the real probe /
    // insert / save_devices chain against the canned esp-miner API, and a
    // real MinerPoller pointed at the same canned API reports the just-added
    // device online within one interval. Same DeviceStore, same code path
    // the devices screen runs.
    std::error_code store_ec;
    const auto store_dir = directory / "device-store";
    std::filesystem::create_directories(store_dir, store_ec);
    require(!store_ec, "smoke device-store directory: " + store_ec.message());
    const CannedMinerServer canned(fixtures / "device-v2.15.3.json");

    DeviceStore store(store_dir / "devices-v1");
    for (const auto& row : rows) store.devices.push_back(row.saved);
    store.persist();
    require(bx::load_devices(store.file_path()).size() == 3,
        "the devices-v1 file starts with the three fixture rows");

    // The X path: one row fewer on disk, and the removed address is gone.
    require(store.remove(rows[0].saved.address), "remove of a saved address");
    {
        const auto after_remove = bx::load_devices(store.file_path());
        require(after_remove.size() == 2 &&
                    !address_saved(after_remove, rows[0].saved.address),
            "remove leaves one row fewer on the devices-v1 file");
    }

    // The A path: the store's real add, with the HTTP transport redirected
    // to the canned server (port 80 → the ephemeral loopback port). Probe,
    // insert, save_devices all run; only the socket destination is injected.
    HttpFn probe_fn = [&canned](const std::string& host, const std::string&,
                               int port, int timeout_ms, std::string& body) {
        (void)host;
        (void)port;
        return bx::http_get("127.0.0.1", "/api/system/info", canned.port(),
                            timeout_ms, body);
    };
    const AddProbe probe = store.add_by_ip("192.0.2.77", probe_fn);
    require(probe.ok, "add-by-IP probe against the canned API document");
    require(probe.info.hostname == "Alpha 601",
        "the probe fetched the fixture hostname");
    {
        const auto after_add = bx::load_devices(store.file_path());
        bool landed = false, named = false;
        for (const auto& device : after_add) {
            if (device.address == "192.0.2.77") {
                landed = true;
                named = device.hostname == "Alpha 601";
            }
        }
        require(after_add.size() == 3 && landed,
            "the devices-v1 file contains the added address");
        require(named, "the added row on disk carries the fetched hostname");
    }

    // The poller half of the mutation chain: set_devices with the post-add
    // list, then state() flips online with the parsed fixture values within
    // the worker's 200 ms interval - what the devices screen's SAVED rows
    // read at their 500 ms tick.
    {
        // The canned device's own display value (its 10m average): every
        // stored address is redirected to the same server, so the farm
        // total must equal the sum of one fixture display value per device.
        const double canned_display_gh = gamma601.display_hashrate();
        const auto close_to = [](double got, double want, double tol) {
            return got >= want - tol && got <= want + tol;
        };
        const double want_farm_gh =
            canned_display_gh * static_cast<double>(store.devices.size());
        MinerPoller poller(200, 1000, probe_fn, [] { return true; });
        poller.set_devices(store.devices);
        require(smoke_wait_until(
                    [&] {
                        const bx::MinerState state =
                            poller.state("192.0.2.77");
                        return state.online &&
                               state.info.display_hashrate() >= 0 &&
                               state.info.hostname == "Alpha 601";
                    },
                    3000),
            "MinerPoller reports the added device online within one "
            "interval");
        // The hero/card preference reaches the farm totals: the aggregate is
        // the 10m average per device - exactly the cards' display values,
        // not the instantaneous hashRate the fixture also carries.
        require(smoke_wait_until(
                    [&] {
                        return close_to(poller.farm_hashrate(),
                                        want_farm_gh, 0.01);
                    },
                    3000),
            "farm total is the sum of the 10m display values");
    }

    std::cout << "bx-smoke: hero '" << hero << "', online 2/3, offline card '"
              << cards[2] << "'\n";
    std::cout << "bx-smoke: detail '" << lines[0] << "' with pool line '"
              << lines[2] << "'\n";
    std::cout << "bx-smoke: devices found row '" << found_rows[0] << "', '"
              << saved_rows[0] << "'\n";
    std::cout << "bx-smoke: smoke-main.png + smoke-detail.png + "
                 "smoke-devices.png + smoke-network.png written; "
                 "smoke assertions passed.\n";
    return 0;
}

#endif

}  // namespace

int main(int argc, char** argv) try {
    lv_init();
    lv_tick_set_cb(ticks);
#if BX_CARDPUTER_ZERO
    const std::string_view option = argc > 1 ? argv[1] : "";
    if (argc != 1 && !(argc == 3 && option == "--keyboard")) {
        std::cerr << "Usage: bitaxezero [--keyboard /dev/input/eventN]\n";
        return 2;
    }
    DeviceKeyboard keyboard(argc == 3 ? argv[2] : "");
    if (!keyboard.valid()) {
        std::cerr << "bitaxezero: no readable Cardputer keyboard with K, M, Space and "
                     "Enter. Check the launcher session.\n";
        return 1;
    }
    auto* display = lv_linux_fbdev_create();
    if (!display || lv_linux_fbdev_set_file(display, "/dev/fb0") != LV_RESULT_OK ||
        lv_display_get_horizontal_resolution(display) != kWidth ||
        lv_display_get_vertical_resolution(display) != kHeight) {
        std::cerr << "bitaxezero needs the 320 x 170 Cardputer Zero framebuffer.\n";
        return 1;
    }
    {
        App app(bx::devices_file());
        while (app.running) { keyboard.dispatch(app); app.tick(); lv_timer_handler(); idle(); }
    }
    lv_display_delete(display);
#else
    if (argc == 4 && std::string_view(argv[1]) == "--smoke-test")
        return smoke(argv[2], argv[3]);
    if (argc != 1) {
        std::cerr << "Usage: bitaxezero | bitaxezero --smoke-test <dir> <fixtures>\n";
        return 2;
    }
    {
        Desktop desktop(false);
        App app(bx::devices_file());
        while (app.running) { desktop.dispatch(app); app.tick(); lv_timer_handler(); idle(); }
    }
#endif
    lv_deinit();
    return 0;
} catch (const std::exception& err) {
    std::cerr << "bitaxezero: " << err.what() << '\n';
    return 1;
}
