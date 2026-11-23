// BitaxeZero - per-device poller and online-only farm aggregation.
//
// One worker thread per device loops http_get -> parse_miner_info and, under
// a per-device mutex, updates that device's MinerState; the LVGL UI reads
// state() and the farm_* aggregates from its own thread at any time. The
// parser (src/bitaxe.hpp) has already applied the temperature plausibility
// filter (-1 = unknown for chip/VR temps), and the poller passes everything
// through untouched - no reading is rewritten here, no color is decided here.
//
// Contract notes:
//   - Cadence: interval_ms between polls of an online device; a failed poll
//     doubles the wait each miss, capped at 30 s, so all-offline farms stop
//     hammering dead addresses; a focused device (set_focus) polls every 2 s.
//     set_focus() truncates an in-flight wait on the spot: the newly (or no
//     longer) focused device wakes and re-enters its loop immediately.
//   - set_devices() restarts the worker set: old threads are stopped and
//     joined, the MinerState of devices that stay in the list is carried
//     over (no offline flicker between calls), and so is their fresher
//     in-memory last_seen (the caller's list is the stale file view; order
//     and hostname stay the caller's). New workers start immediately. Safe
//     to call repeatedly, including with an unchanged list. Duplicate
//     addresses collapse to one worker (the first entry wins); every list
//     entry still reaches the persistence callback. During the reap→spawn
//     swap a concurrent reader can observe a brief all-offline worker window
//     (state()/farm_*/network_ok() read an empty set) — safe, never a
//     dangling reference; the state and last_seen carry-over exists to
//     minimize real-world flicker.
//   - Aggregation sums online devices only, with the display field
//     (display_hashrate: hash_rate_10m when reported, else hash_rate_gh), so
//     the hero total is the exact sum of the cards' values. A device
//     contributes its display value when >= 0, else it counts as offline for
//     the hash-rate total (still online for online_count()).
//     Power values that are unknown (-1) are skipped. farm_efficiency() is
//     farm W per farm TH/s, -1 when the farm hash rate is 0 (renders "—").
//   - network_ok(): true unless the injected net_check says the default
//     interface is down AND no device probe contradicts it. A probe counts
//     in the device's favour when it returned ok or protocol_error (some
//     HTTP server answered); dns_failed/refused/timeout count against, and
//     so does a device that has not been probed yet. With zero devices,
//     network_ok() therefore mirrors net_check().
//   - persist_cb, when installed (do it before set_devices()/first use),
//     receives a consistent copy of the poller's device list whenever a
//     last_seen changed - at most one callback per batch of changes, and
//     a final flush on destruction. Exceptions from the callback are
//     swallowed; the poller must not die with the worker threads. A flush
//     may lag one set_devices() swap (its pending flag was set before the
//     swap, its list snapshot races the swap install): the snapshot is
//     always a whole list, never half-updated, but the caller that removed
//     or added a device must persist the new list once more after
//     set_devices() returns so that stale write is superseded.
//   - Injections must not throw. The poller tolerates a throwing http_fn by
//     counting that poll as failed (offline; a failed probe for network_ok());
//     a throwing net_check or persist_cb propagates to the calling thread.
//   - The plan caps the farm at 16 devices: the poller keeps the full list
//     (persistence sees all of it) but spawns workers for at most the first
//     16 unique addresses, so extra entries simply never poll (they stay
//     offline/default).

#ifndef BX_POLLER_HPP
#define BX_POLLER_HPP

#include "bitaxe.hpp"
#include "config.hpp"
#include "http.hpp"

#include <arpa/inet.h>  // ::htons, ::htonl (default_network_probe)
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h> // ::socket, ::connect, ::close

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace bx {

// The esp-miner HTTP API's port: discovery reports it, but SavedDevice does
// not carry it, so the poller fixes it here.
inline constexpr int kMinerHttpPort = 80;

// One Bitaxe as the poller last saw it: the parsed MinerInfo (unknown fields
// stay -1, including clamped temperatures), whether the last poll got a 200
// with an API document, and the Unix epoch seconds of the last successful
// poll (0 = never answered within this worker set).
struct MinerState {
    MinerInfo info;
    bool online = false;
    std::int64_t last_seen_epoch = 0;
};

// True when the OS has a usable default route: a UDP socket "connect" against
// a public resolver address succeeds without any packet being sent and fails
// (ENETUNREACH) when no default interface exists. Injected tests stub this.
inline bool default_network_probe() {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    sockaddr_in target {};
    target.sin_family = AF_INET;
    target.sin_port = ::htons(53);
    target.sin_addr.s_addr = ::htonl(0x08080808u); // 8.8.8.8, never contacted
    const bool up = ::connect(fd, reinterpret_cast<const sockaddr*>(&target),
                              sizeof(target)) == 0;
    ::close(fd);
    return up;
}

class MinerPoller {
public:
    // http_fn has exactly http_get's signature; net_check answers "OS default
    // interface exists and is up". Tests stub both.
    explicit MinerPoller(
        int interval_ms = 5000, int timeout_ms = 2000,
        std::function<HttpResult(const std::string&, const std::string&, int, int,
                                 std::string&)>
            http_fn = &http_get,
        std::function<bool()> net_check = default_network_probe);

    // Stops and joins every worker. Safe while workers are mid-request.
    ~MinerPoller();

    MinerPoller(const MinerPoller&) = delete;
    MinerPoller& operator=(const MinerPoller&) = delete;

    // Replaces the polled device set, reusing the state of devices that stay
    // in the list. Workers persist last_seen updates through the callback
    // below with the list installed here.
    void set_devices(const std::vector<SavedDevice>& devices);

    // Consistent copy of one device's state; default (offline, unknown info)
    // for an address that is not in the current set.
    MinerState state(const std::string& address) const;

    // Farm totals: sums over online devices only, 0.0 when nothing is online.
    double farm_hashrate() const;
    double farm_power() const;

    // Farm W per farm TH/s; -1 (renders "—") when the farm hash rate is 0,
    // or when no online device reports a known power (never a fake 0 W/TH).
    double farm_efficiency() const;

    int online_count() const;

    // See the contract notes above; net_check() is consulted live per call.
    bool network_ok() const;

    // The focused device polls every 2 s; empty string clears the focus.
    void set_focus(const std::string& address);

    // Install before set_devices()/first use. Fire-and-forget from worker
    // threads; the callee must not call back into this poller.
    std::function<void(const std::vector<SavedDevice>&)> persist_cb;

private:
    struct Worker;

    // Stops and joins the current workers, returning them so set_devices can
    // carry over their states before they are destroyed.
    std::vector<std::unique_ptr<Worker>> reap_workers();

    // Worker-side hook: records one successful poll's epoch and fires the
    // persistence callback for the changed list.
    void note_last_seen(const std::string& address, std::int64_t epoch);

    struct Totals {
        double hash_rate_gh = 0.0;
        double power = 0.0;
        int online = 0;
        bool power_known = false; // at least one online rig reported watts
    };
    Totals totals() const;

    const std::function<HttpResult(const std::string&, const std::string&, int, int,
                                   std::string&)>
        http_fn_;
    const std::function<bool()> net_check_;
    const int interval_ms_;
    const int timeout_ms_;

    // The worker set. Guarded by workers_mutex_, which is also the reader
    // handoff for state()/farm_*/network_ok()/set_focus(); workers only take
    // their own Worker::mutex (consistent order: workers_mutex_ first).
    mutable std::mutex workers_mutex_;
    std::vector<std::unique_ptr<Worker>> workers_;
    std::string focus_;

    // The device list a persistence callback sees, and its bookkeeping.
    // Consistent snapshots under devices_mutex_; persist_mutex_ only
    // serializes "at most one pending callback".
    mutable std::mutex devices_mutex_;
    std::vector<SavedDevice> devices_;
    std::mutex persist_mutex_;
    bool pending_persist_ = false;
};

} // namespace bx

#endif // BX_POLLER_HPP
