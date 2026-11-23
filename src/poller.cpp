// BitaxeZero - per-device poller and online-only farm aggregation: the
// worker-thread machinery declared in poller.hpp.

#include "poller.hpp"

#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace bx {
namespace {

// Cadence constants from the spec: the focused device polls every 2 s and a
// device that stays offline backs off to at most 30 s.
inline constexpr int kFocusIntervalMs = 2000;
inline constexpr int kMaxBackoffMs = 30000;
inline constexpr std::size_t kMaxWorkers = 16; // plan cap for the farm

// Clamp a caller-supplied base interval to something schedulable.
inline int positive_or(int value, int fallback) { return value > 0 ? value : fallback; }

std::int64_t now_epoch_s() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

// One device's loop. Lifetime: owned by MinerPoller; the thread runs only
// until stop is set, and reap_workers() joins before any Worker is destroyed.
struct MinerPoller::Worker {
    // The base interval and poll timeout live on the parent; a Worker only
    // needs its identity and mutable loop state.
    explicit Worker(std::string addr) : address(std::move(addr)) {}

    const std::string address;

    mutable std::mutex mutex;      // guards the fields below
    std::condition_variable wake;  // signaled with stop/focus changes
    std::atomic<bool> stop{false};
    std::atomic<bool> focused{false};

    MinerState state;                                   // guarded by mutex
    std::optional<HttpResult> last_result;              // nullopt = not probed yet
    int failures = 0;                                   // consecutive failed polls
    long focus_generation = 0;                          // bumped by set_focus
    std::chrono::steady_clock::time_point wake_deadline{}; // current sleep end

    std::thread thread;

    void run(MinerPoller& parent);
};

void MinerPoller::Worker::run(MinerPoller& parent) {
    while (!stop.load()) {
        bool succeeded = false;
        int wait_ms = 0;
        long focus_generation = 0; // snapshot for the race-free install below
        {
            std::string body;
            HttpResult result;
            try {
                result =
                    parent.http_fn_(address, "/api/system/info", kMinerHttpPort,
                                    parent.timeout_ms_, body);
            } catch (...) {
                // Injected http_fn must not throw; if it does anyway, the
                // poll survives as failed (offline; a failed probe for
                // network_ok()) and the worker loops on.
                result = HttpResult::timeout;
            }
            MinerInfo parsed;
            succeeded =
                result == HttpResult::ok && bx::parse_miner_info(body, parsed);
            const std::int64_t seen = succeeded ? now_epoch_s() : 0;
            {
                std::lock_guard<std::mutex> lock(mutex);
                last_result = result;
                state.online = succeeded;
                if (succeeded) {
                    state.info = parsed;
                    state.last_seen_epoch = seen;
                    failures = 0;
                } else {
                    ++failures; // previous known state stays for the UI
                }
                if (focused.load()) {
                    wait_ms = kFocusIntervalMs;
                } else if (succeeded) {
                    wait_ms = parent.interval_ms_;
                } else {
                    wait_ms = parent.interval_ms_;
                    for (int f = 0; f < failures && wait_ms < kMaxBackoffMs; ++f) {
                        wait_ms *= 2;
                    }
                    if (wait_ms > kMaxBackoffMs) wait_ms = kMaxBackoffMs;
                }
                focus_generation = this->focus_generation;
            }
            if (succeeded) parent.note_last_seen(address, seen);
        }
        {
            std::unique_lock<std::mutex> lock(mutex);
            // Install the computed cadence only when no set_focus() raced in
            // since the state section: a generation mismatch means set_focus
            // already bumped wake_deadline, and overwriting it here would
            // make the device honor its new cadence one full stale interval
            // later (up to a 30 s back-off) instead of on the spot.
            if (this->focus_generation == focus_generation) {
                wake_deadline = std::chrono::steady_clock::now() +
                                std::chrono::milliseconds(wait_ms);
            }
            // Sleep to the wake deadline. A set_focus() cadence change
            // replaces wake_deadline (under this mutex, before notifying),
            // and the predicate plus the fresh re-read below turn that into
            // a truncated wait instead of a stale up-to-30 s back-off.
            while (!stop.load()) {
                const auto deadline = wake_deadline;
                if (wake.wait_until(lock, deadline, [&] {
                        return stop.load() || wake_deadline != deadline;
                    })) {
                    continue; // stop or the deadline moved: re-read it
                }
                break; // timed out and the deadline still stands: poll again
            }
        }
    }
}

MinerPoller::MinerPoller(int interval_ms, int timeout_ms,
                         std::function<HttpResult(const std::string&,
                                                  const std::string&, int, int,
                                                  std::string&)>
                             http_fn,
                         std::function<bool()> net_check)
    : http_fn_(std::move(http_fn)),
      net_check_(std::move(net_check)),
      interval_ms_(positive_or(interval_ms, 5000)),
      timeout_ms_(positive_or(timeout_ms, 2000)) {}

std::vector<std::unique_ptr<MinerPoller::Worker>> MinerPoller::reap_workers() {
    std::vector<std::unique_ptr<Worker>> doomed;
    {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        doomed = std::move(workers_);
    }
    for (auto& worker : doomed) {
        worker->stop.store(true);
        worker->wake.notify_all();
    }
    // Join before any destructor: workers may be mid-request, and a still
    // joinable std::thread killed at scope exit would terminate the process.
    for (auto& worker : doomed) {
        if (worker->thread.joinable()) worker->thread.join();
    }
    return doomed;
}

MinerPoller::~MinerPoller() {
    reap_workers();

    // Last-seen bookkeeping may still have a batch queued: flush it so a
    // clean shutdown keeps the file as fresh as the last successful poll.
    bool flush = false;
    {
        std::lock_guard<std::mutex> lock(persist_mutex_);
        flush = pending_persist_;
        pending_persist_ = false;
    }
    if (!flush) return;
    std::vector<SavedDevice> snapshot;
    {
        std::lock_guard<std::mutex> lock(devices_mutex_);
        snapshot = devices_;
    }
    try {
        if (persist_cb) persist_cb(snapshot);
    } catch (...) {
        // The UI owns the callback; worker-thread teardown must not throw.
    }
}

void MinerPoller::set_devices(const std::vector<SavedDevice>& devices) {
    auto old = reap_workers();

    // Carry over what the surviving workers saw so a set_devices() call does
    // not flicker the whole farm to offline.
    std::unordered_map<std::string, MinerState> carried_state;
    std::unordered_map<std::string, std::optional<HttpResult>> carried_probe;
    for (auto& worker : old) {
        std::lock_guard<std::mutex> lock(worker->mutex);
        carried_state.emplace(worker->address, worker->state);
        carried_probe.emplace(worker->address, worker->last_result);
    }
    old.clear();

    // Install the new list before starting workers: their first successful
    // poll may already record a last_seen against it. The caller's list is
    // the stale (file) view, so surviving addresses keep the fresher
    // in-memory last_seen their workers recorded; order/hostname stay the
    // caller's.
    {
        std::unordered_map<std::string, std::int64_t> old_last_seen;
        std::lock_guard<std::mutex> lock(devices_mutex_);
        for (const auto& device : devices_) {
            old_last_seen.emplace(device.address, device.last_seen);
        }
        devices_.clear();
        devices_.reserve(devices.size());
        for (const auto& entry : devices) {
            devices_.push_back(entry);
            if (auto it = old_last_seen.find(entry.address);
                it != old_last_seen.end()) {
                devices_.back().last_seen = it->second;
            }
        }
    }

    const std::string focus_now = [this] {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        return focus_;
    }();

    std::lock_guard<std::mutex> lock(workers_mutex_);
    std::size_t spawned = 0;
    std::unordered_set<std::string> seen_addresses; // dedupe: first entry wins
    for (const auto& entry : devices) {
        if (spawned >= kMaxWorkers) break; // plan cap: at most 16 farms threads
        if (!seen_addresses.insert(entry.address).second) continue;
        auto worker = std::make_unique<Worker>(entry.address);
        if (auto it = carried_state.find(entry.address); it != carried_state.end()) {
            std::lock_guard<std::mutex> own(worker->mutex);
            worker->state = it->second;
            worker->last_result = carried_probe[entry.address];
            worker->failures = 0;
        }
        worker->focused.store(entry.address == focus_now);
        Worker* raw = worker.get();
        worker->thread = std::thread([this, raw] { raw->run(*this); });
        workers_.push_back(std::move(worker));
        ++spawned;
    }
}

MinerState MinerPoller::state(const std::string& address) const {
    std::lock_guard<std::mutex> lock(workers_mutex_);
    for (const auto& worker : workers_) {
        if (worker->address != address) continue;
        std::lock_guard<std::mutex> own(worker->mutex);
        return worker->state; // copy: readers never see a torn MinerState
    }
    return MinerState{};
}

MinerPoller::Totals MinerPoller::totals() const {
    Totals out;
    std::lock_guard<std::mutex> lock(workers_mutex_);
    for (const auto& worker : workers_) {
        std::lock_guard<std::mutex> own(worker->mutex);
        if (!worker->state.online) continue;
        ++out.online;
        // The display rate: the 10-minute rolling average when the device
        // reports one, else the instantaneous field, else the device hashes
        // nothing worth summing (still counts as online). The hero total
        // stays the exact sum of the cards' display values.
        const double gh = worker->state.info.display_hashrate();
        if (gh >= 0) out.hash_rate_gh += gh;
        if (worker->state.info.power >= 0) {
            out.power += worker->state.info.power;
            out.power_known = true;
        }
    }
    return out;
}

double MinerPoller::farm_hashrate() const { return totals().hash_rate_gh; }

double MinerPoller::farm_power() const { return totals().power; }

double MinerPoller::farm_efficiency() const {
    const Totals all = totals();
    if (all.hash_rate_gh <= 0) return -1;
    if (!all.power_known) return -1; // nothing reporting watts: "—", not 0
    return all.power / (all.hash_rate_gh / 1000.0); // W per TH/s
}

int MinerPoller::online_count() const { return totals().online; }

bool MinerPoller::network_ok() const {
    // The interface probe alone wins: devices cannot outvote a real route.
    if (net_check_()) return true;
    // Every device probe failed (or has not happened yet) AND the interface
    // is down: the "no network" state. One device an HTTP server answered
    // (ok or protocol_error) outvotes; dns_failed/refused/timeout count
    // against it.
    std::lock_guard<std::mutex> lock(workers_mutex_);
    for (const auto& worker : workers_) {
        std::lock_guard<std::mutex> own(worker->mutex);
        if (worker->last_result == HttpResult::ok ||
            worker->last_result == HttpResult::protocol_error) {
            return true;
        }
    }
    return false;
}

void MinerPoller::set_focus(const std::string& address) {
    // Bump the wake deadline of every device whose focus status changed to
    // right now, so the new (or restored) cadence takes effect immediately
    // instead of after the in-flight back-off/sleep. The fresh wait loop in
    // Worker::run() then re-enters with the earlier deadline.
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(workers_mutex_);
    focus_ = address;
    for (auto& worker : workers_) {
        const bool newly_focused = worker->address == address;
        const bool was_focused =
            worker->focused.exchange(newly_focused);   // reads the old value
        if (was_focused != newly_focused) {
            std::lock_guard<std::mutex> own(worker->mutex);
            ++worker->focus_generation; // invalidates the worker's pending install
            worker->wake_deadline = now;
        }
        worker->wake.notify_all();
    }
}

void MinerPoller::note_last_seen(const std::string& address, std::int64_t epoch) {
    {
        std::lock_guard<std::mutex> lock(devices_mutex_);
        bool changed = false;
        for (auto& device : devices_) {
            if (device.address == address && device.last_seen != epoch) {
                device.last_seen = epoch;
                changed = true;
            }
        }
        if (!changed) return;
        std::lock_guard<std::mutex> pending(persist_mutex_);
        pending_persist_ = true;
    }
    // One callback per batch: the first thread to claim the pending flag
    // fires it, later successes during the flush collapse into the next one.
    bool fire = false;
    {
        std::lock_guard<std::mutex> pending(persist_mutex_);
        fire = pending_persist_;
        pending_persist_ = false;
    }
    if (!fire) return;
    std::vector<SavedDevice> snapshot;
    {
        std::lock_guard<std::mutex> lock(devices_mutex_);
        snapshot = devices_; // whole-list copy: never a half-updated callback
    }
    try {
        if (persist_cb) persist_cb(snapshot);
    } catch (...) {
        // The UI owns the callback; worker threads must not die with it.
    }
}

} // namespace bx
