#include "bitaxe.hpp"
#include "config.hpp"
#include "discover.hpp"
#include "http.hpp"
#include "poller.hpp"

#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <netdb.h> // ::getaddrinfo (http tests: name/literal resolution)
#include <netinet/in.h> // ::sockaddr_in{,6}
#include <poll.h> // ::poll (accept guard)
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/socket.h> // ::socket, ::bind, ::listen, ::accept
#include <sys/types.h> // ssize_t
#include <system_error>
#include <thread>
#include <unistd.h> // ::getpid, ::setenv, ::unsetenv, ::close, ::recv, ::send
#include <vector>

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error(#condition); } while (false)

namespace {

// Restores two environment variables after overriding them, so the XDG
// branch-coverage checks cannot leak state into the rest of main().
class EnvScope {
public:
    EnvScope(const char* name1, const char* name2)
        : name1_(name1), name2_(name2), value1_(::getenv(name1)), value2_(::getenv(name2)) {}
    ~EnvScope() {
        restore(name1_, value1_);
        restore(name2_, value2_);
    }
    EnvScope(const EnvScope&) = delete;
    EnvScope& operator=(const EnvScope&) = delete;

private:
    static void restore(const char* name, const char* value) {
        if (value) ::setenv(name, value, 1);
        else ::unsetenv(name);
    }
    const char* name1_;
    const char* name2_;
    const char* value1_;
    const char* value2_;
};

std::filesystem::path temp_dir(const char* tag) {
    std::ostringstream name;
    name << "bx-config-test-" << tag << '-' << ::getpid();
    const auto dir = std::filesystem::temp_directory_path() / name.str();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

std::string file_bytes(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

bx::SavedDevice device(const char* address, const char* hostname, std::int64_t last_seen) {
    return {address, hostname, last_seen};
}

} // namespace

// ---- http: helpers for the canned one-shot HTTP servers below ----

namespace http_test {

// Reads until the end of the request head (or EOF); a request is small enough
// to arrive whole, so no partial-line buffering is needed beyond this loop.
std::string read_request(int fd) {
    std::string data;
    char buf[1024];
    while (data.find("\r\n\r\n") == std::string::npos) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        data.append(buf, static_cast<std::size_t>(n));
    }
    return data;
}

bool write_all(int fd, const std::string& data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t n = ::send(fd, data.data() + offset, data.size() - offset, 0);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            return false;
        }
        offset += static_cast<std::size_t>(n);
    }
    return true;
}

std::string make_response(int code, const std::string& body) {
    std::ostringstream head;
    head << "HTTP/1.0 " << code << " Status\r\nContent-Type: application/json\r\n"
         << "Content-Length: " << body.size() << "\r\n\r\n";
    return head.str() + body;
}

// A one-connection canned server on an ephemeral loopback port: it binds port
// 0 and reports the OS-chosen port, then serves exactly one accept in a
// background std::thread. finish() (also run by the destructor) closes the
// listener and joins, so main() can never outlive the handler. The 5 s accept
// guard guarantees the join cannot hang when a test never connects.
class TestServer {
public:
    using Handler = std::function<void(int fd, std::string& request_seen)>;

    TestServer(int family, Handler handler) : handler_(std::move(handler)) {
        listen_fd_ = ::socket(family, SOCK_STREAM, 0);
        CHECK(listen_fd_ >= 0);
        const int reuse = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_storage addr {};
        socklen_t addrlen = 0;
        if (family == AF_INET6) {
            auto* in6 = reinterpret_cast<sockaddr_in6*>(&addr);
            in6->sin6_family = AF_INET6;
            in6->sin6_addr = in6addr_loopback; // ::1
            in6->sin6_port = 0;
            addrlen = sizeof(*in6);
        } else {
            auto* in4 = reinterpret_cast<sockaddr_in*>(&addr);
            in4->sin_family = AF_INET;
            in4->sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1
            in4->sin_port = 0;
            addrlen = sizeof(*in4);
        }
        CHECK(::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), addrlen) == 0);
        CHECK(::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &addrlen) == 0);
        port_ = family == AF_INET6
                    ? ntohs(reinterpret_cast<sockaddr_in6*>(&addr)->sin6_port)
                    : ntohs(reinterpret_cast<sockaddr_in*>(&addr)->sin_port);
        CHECK(::listen(listen_fd_, 1) == 0);

        worker_ = std::thread([this] {
            pollfd poll_set {};
            poll_set.fd = listen_fd_;
            poll_set.events = POLLIN;
            if (::poll(&poll_set, 1, 5000) <= 0) return; // nothing ever connected
            const int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) return;
            handler_(fd, request_seen_);
            ::close(fd);
        });
    }

    TestServer(const TestServer&) = delete;
    TestServer& operator=(const TestServer&) = delete;

    ~TestServer() { finish(); }

    int port() const { return port_; }
    // Only safe after finish(): the worker wrote request_seen_.
    const std::string& request() const { return request_seen_; }

    void finish() {
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        if (worker_.joinable()) worker_.join();
    }

private:
    Handler handler_;
    std::thread worker_;
    int listen_fd_ = -1;
    int port_ = 0;
    std::string request_seen_;
};

long long elapsed_ms(std::chrono::steady_clock::time_point started) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - started)
        .count();
}

} // namespace http_test

// ---- poller: helpers for the MinerPoller scenarios below ----

namespace poller_test {

// Polls until done() holds or the dealine passes; the last attempt always
// counts. Guard interval 10 ms keeps timing-sensitive tests honest.
template <typename Done>
bool wait_until(Done done, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return done();
}

// A port bound and immediately released: connecting to it yields ECONNREFUSED
// fast, which stands in for a powered-off Bitaxe on the LAN.
int closed_loopback_port() {
    const int probe = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(probe >= 0);
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    CHECK(::bind(probe, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t addrlen = sizeof(addr);
    CHECK(::getsockname(probe, reinterpret_cast<sockaddr*>(&addr), &addrlen) == 0);
    const int port = ntohs(addr.sin_port);
    ::close(probe);
    return port;
}

// The Task-4 canned-server pattern, restarted for accept after accept: the
// poller hits the same device several times per second, so the server loops
// until finish() instead of serving one connection. The 100 ms poll interval
// bounds the join in finish() even when nothing ever connects.
class MultiServer {
public:
    using Handler = std::function<void(int fd, const std::string& request)>;

    MultiServer(int family, Handler handler) : handler_(std::move(handler)) {
        listen_fd_ = ::socket(family, SOCK_STREAM, 0);
        CHECK(listen_fd_ >= 0);
        const int reuse = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_storage addr {};
        socklen_t addrlen = 0;
        CHECK(family == AF_INET); // the poller scenarios only need IPv4
        auto* in4 = reinterpret_cast<sockaddr_in*>(&addr);
        in4->sin_family = AF_INET;
        in4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        in4->sin_port = 0;
        addrlen = sizeof(*in4);
        CHECK(::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), addrlen) == 0);
        CHECK(::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr),
                            &addrlen) == 0);
        port_ = ntohs(in4->sin_port);
        CHECK(::listen(listen_fd_, 8) == 0);

        worker_ = std::thread([this] {
            while (running_.load()) {
                pollfd poll_set {};
                poll_set.fd = listen_fd_;
                poll_set.events = POLLIN;
                if (::poll(&poll_set, 1, 100) <= 0) continue;
                const int fd = ::accept(listen_fd_, nullptr, nullptr);
                if (fd < 0) continue;
                const std::string request = http_test::read_request(fd);
                handler_(fd, request);
                ::close(fd);
                // request_seen_ is read only after finish(), per the contract
                // below; the worker thread is its sole writer until then.
                request_seen_ = request;
            }
        });
    }

    MultiServer(const MultiServer&) = delete;
    MultiServer& operator=(const MultiServer&) = delete;

    ~MultiServer() { finish(); }

    int port() const { return port_; }
    // Only safe after finish(): the worker wrote request_seen_.
    const std::string& request() const { return request_seen_; }

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
    Handler handler_;
    std::thread worker_;
    std::atomic<bool> running_{true};
    int listen_fd_ = -1;
    int port_ = 0;
    std::string request_seen_;
};

// A handler that answers every connection with one 200 carrying `body`.
MultiServer::Handler serve_body(const std::string& body) {
    return [body](int fd, const std::string&) {
        http_test::write_all(fd, http_test::make_response(200, body));
    };
}

} // namespace poller_test

// ---- discover: helpers to synthesize mDNS datagrams for the parser tests ----

namespace mdns_test {

using Bytes = std::vector<std::uint8_t>;

void append_u16(Bytes& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void append_u32(Bytes& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
    }
}

// Uncompressed DNS name encoding from plain labels.
Bytes name_bytes(const std::vector<std::string>& labels) {
    Bytes out;
    for (const auto& label : labels) {
        CHECK(label.size() <= 63);
        out.push_back(static_cast<std::uint8_t>(label.size()));
        for (char c : label) out.push_back(static_cast<std::uint8_t>(c));
    }
    out.push_back(0);
    return out;
}

// One resource record: owner name, type, class, ttl, rdata.
Bytes record(const std::vector<std::string>& owner, std::uint16_t type,
             std::uint16_t dns_class, std::uint32_t ttl, const Bytes& rdata) {
    Bytes out = name_bytes(owner);
    append_u16(out, type);
    append_u16(out, dns_class);
    append_u32(out, ttl);
    append_u16(out, static_cast<std::uint16_t>(rdata.size()));
    out.insert(out.end(), rdata.begin(), rdata.end());
    return out;
}

Bytes header(std::uint16_t answers, std::uint16_t additional) {
    Bytes out(12, 0); // id 0, flags: QR + AA, one-shot response
    out[2] = 0x84;
    out[6] = static_cast<std::uint8_t>(answers >> 8);
    out[7] = static_cast<std::uint8_t>(answers & 0xFF);
    out[10] = static_cast<std::uint8_t>(additional >> 8);
    out[11] = static_cast<std::uint8_t>(additional & 0xFF);
    return out;
}

// The printer entry: a non-Bitaxe _http._tcp service on 192.0.2.23 —
// neither an "asic" TXT key nor an instance starting with "Bitaxe".
Bytes printer_ptr() {
    return record({"_http", "_tcp", "local"}, 12, 1, 4500,
                  name_bytes({"Office Printer", "_http", "_tcp", "local"}));
}

Bytes printer_srv_txt_a() {
    Bytes srv = {0, 0, 0, 0, 0x23, 0x8C}; // prio 0, weight 0, port 9100
    const Bytes target = name_bytes({"printer", "local"});
    srv.insert(srv.end(), target.begin(), target.end());
    Bytes txt;
    for (const auto& entry : std::vector<std::string>{"path=/webadmin",
                                                      "pdl=postscript"}) {
        txt.push_back(static_cast<std::uint8_t>(entry.size()));
        for (char c : entry) txt.push_back(static_cast<std::uint8_t>(c));
    }
    Bytes a = {192, 0, 2, 23};
    Bytes out = record({"Office Printer", "_http", "_tcp", "local"}, 33,
                       0x8001, 120, srv);
    const Bytes txt_rr =
        record({"Office Printer", "_http", "_tcp", "local"}, 16, 0x8001, 120, txt);
    out.insert(out.end(), txt_rr.begin(), txt_rr.end());
    const Bytes a_rr = record({"printer", "local"}, 1, 1, 120, a);
    out.insert(out.end(), a_rr.begin(), a_rr.end());
    return out;
}

} // namespace mdns_test

int main() try {
    // config_dir() follows the trainer's XDG handling: $XDG_CONFIG_HOME/bitaxezero
    // (absolute only), else $HOME/.config/bitaxezero.
    if (const char* xdg = ::getenv("XDG_CONFIG_HOME"); xdg && xdg[0] != '\0') {
        CHECK((std::filesystem::path(xdg) / "bitaxezero") == bx::config_dir());
    } else if (const char* home = ::getenv("HOME"); home && home[0] != '\0') {
        CHECK((std::filesystem::path(home) / ".config" / "bitaxezero") == bx::config_dir());
    }

    // XDG branch coverage: a non-absolute XDG_CONFIG_HOME is ignored and the
    // $HOME fallback used; with neither variable usable, config_dir() and
    // devices_file() are empty (not just a bare "devices-v1" name).
    {
        const EnvScope scope("XDG_CONFIG_HOME", "HOME");
        if (const char* home = ::getenv("HOME"); home && home[0] != '\0') {
            ::setenv("XDG_CONFIG_HOME", "relative/xdg", 1);
            CHECK(bx::config_dir() == std::filesystem::path(home) / ".config" / "bitaxezero");
        }
        ::unsetenv("XDG_CONFIG_HOME");
        ::unsetenv("HOME");
        CHECK(bx::config_dir().empty());
        CHECK(bx::devices_file().empty());
    }

    // Round-trip: save_devices then load_devices returns the three devices.
    {
        const auto dir = temp_dir("roundtrip");
        const auto file = dir / "devices-v1";
        const std::vector<bx::SavedDevice> saved = {
            device("192.0.2.101", "bitaxe-alpha", 1700000000),
            device("192.0.2.102", "bitaxe-beta", 1700000123),
            device("192.0.2.103", "bitaxe-gamma", 1700000456),
        };
        bx::save_devices(file, saved);
        const auto loaded = bx::load_devices(file);
        CHECK(loaded.size() == 3);
        for (std::size_t i = 0; i < saved.size(); ++i) {
            CHECK(loaded[i].address == saved[i].address);
            CHECK(loaded[i].hostname == saved[i].hostname);
            CHECK(loaded[i].last_seen == saved[i].last_seen);
        }
        // File format: a JSON array of objects with the three documented keys
        // (trailing newline allowed).
        const auto text = file_bytes(file);
        const auto trimmed = text.substr(0, text.find_last_not_of(" \n\r\t") + 1);
        CHECK(!trimmed.empty() && trimmed.front() == '[' && trimmed.back() == ']');
        CHECK(text.find("\"address\"") != std::string::npos);
        CHECK(text.find("\"hostname\"") != std::string::npos);
        CHECK(text.find("\"last_seen\"") != std::string::npos);
        std::filesystem::remove_all(dir);
    }

    // A missing file loads as empty; it is not created by a load.
    {
        const auto dir = temp_dir("missing");
        const auto loaded = bx::load_devices(dir / "does-not-exist" / "devices-v1");
        CHECK(loaded.empty());
        std::filesystem::remove_all(dir);
    }

    // Malformed JSON (and a non-array document) throws std::runtime_error.
    {
        const auto dir = temp_dir("malformed");
        const auto file = dir / "devices-v1";
        std::ofstream(file) << "[{\"address\": \"192.0.2.1\", \"hostname\";";
        bool threw = false;
        try {
            bx::load_devices(file);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
        std::ofstream(file) << "\"just a string\"";
        threw = false;
        try {
            bx::load_devices(file);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
        std::filesystem::remove_all(dir);
    }

    // update_last_seen only touches the matching address, in memory: the file
    // stays byte-identical until save_devices is called again.
    {
        const auto dir = temp_dir("lastseen");
        const auto file = dir / "devices-v1";
        const std::vector<bx::SavedDevice> saved = {
            device("10.0.0.1", "one", 100),
            device("10.0.0.2", "two", 200),
            device("10.0.0.3", "three", 300),
        };
        bx::save_devices(file, saved);
        const auto before = file_bytes(file);

        auto devices = bx::load_devices(file);
        bx::update_last_seen(devices, "10.0.0.2", 999);
        CHECK(file_bytes(file) == before);
        CHECK(devices[0].last_seen == 100);
        CHECK(devices[1].last_seen == 999);
        CHECK(devices[2].last_seen == 300);
        CHECK(devices[1].hostname == "two");

        // An unknown address changes nothing.
        bx::update_last_seen(devices, "10.0.0.99", 111);
        CHECK(devices[0].last_seen == 100);
        CHECK(devices[1].last_seen == 999);
        CHECK(devices[2].last_seen == 300);

        bx::save_devices(file, devices);
        const auto reloaded = bx::load_devices(file);
        CHECK(reloaded.size() == 3);
        CHECK(reloaded[0].last_seen == 100);
        CHECK(reloaded[1].last_seen == 999);
        CHECK(reloaded[2].last_seen == 300);
        std::filesystem::remove_all(dir);
    }

    // ---- bitaxe: esp-miner API mapping against the live-farm fixtures ----

    const auto close_to = [](double got, double want, double eps) {
        return std::fabs(got - want) <= eps;
    };

    // v2.15.3 (Alpha 601, single BM1370, pools[] array layout).
    {
        bx::MinerInfo m;
        CHECK(bx::parse_miner_info(file_bytes("tests/fixtures/device-v2.15.3.json"), m));
        CHECK(m.hostname == "Alpha 601");
        CHECK(m.asic == "BM1370");
        CHECK(m.board_version == "601");
        CHECK(m.fw_version == "v2.15.3");
        CHECK(close_to(m.hash_rate_gh, 1251.4011133, 0.001)); // hashRate
        CHECK(close_to(m.hash_rate_10m, 1247.6013184, 0.001));
        // Display preference: the 10-minute average, not the jittering
        // instantaneous hashRate.
        CHECK(close_to(m.display_hashrate(), 1247.6013184, 0.001));
        CHECK(close_to(m.expected_gh, 1280.4000488, 0.001));
        CHECK(close_to(m.chip_temp, 47.75, 0.001));
        CHECK(close_to(m.chip2_temp, -1.0, 0.0)); // no second chip
        CHECK(close_to(m.vr_temp, 55.0, 0.001));
        CHECK(close_to(m.power, 20.112548, 0.001));
        CHECK(close_to(m.voltage_mv, 5300.0, 0.001));
        CHECK(close_to(m.error_pct, 6.4106343, 0.001));
        CHECK(close_to(m.pool_latency_ms, 18.31, 0.001));
        CHECK(m.fan_pct == 75);
        CHECK(m.fan_rpm == 8210);
        CHECK(m.frequency_mhz == 621); // actualFrequency 621.4, rounded
        CHECK(m.shares_accepted == 1501);
        CHECK(m.shares_rejected == 0);
        CHECK(m.best_diff == 7830235648LL);
        CHECK(m.best_session_diff == 1712345678LL);
        CHECK(m.uptime_s == 2361600LL);
        // Pool mapping from pools[0]; the wallet id is sanitized in the fixture.
        CHECK(m.pool_url == "example.org");
        CHECK(m.pool_port == 3333);
        CHECK(m.pool_user.size() >= 11);
        CHECK(m.pool_user.substr(m.pool_user.size() - 11) == "WorkerAlpha");
        CHECK(m.using_fallback == false);
        CHECK(m.pool_user == "bc1qExample.WorkerAlpha");
    }

    // v2.12 (Turbo 800, dual BM1370, flat stratum layout, no pools[] array).
    {
        const auto text = file_bytes("tests/fixtures/device-v2.12.json");
        CHECK(text.find("\"pools\"") == std::string::npos);
        bx::MinerInfo m;
        CHECK(bx::parse_miner_info(text, m));
        CHECK(m.hostname == "Turbo 800");
        CHECK(m.asic == "BM1370");
        CHECK(m.board_version == "800");
        CHECK(m.fw_version == "v2.12.0");
        CHECK(close_to(m.hash_rate_gh, 1992.4051172, 0.001));
        CHECK(close_to(m.hash_rate_10m, 1983.3986816, 0.001));
        CHECK(close_to(m.display_hashrate(), 1983.3986816, 0.001));
        CHECK(close_to(m.expected_gh, 2064.0000977, 0.001));
        CHECK(close_to(m.chip_temp, 43.125, 0.001));
        CHECK(close_to(m.chip2_temp, 58.625, 0.001)); // dual chip
        CHECK(close_to(m.vr_temp, 54.0, 0.001));
        CHECK(close_to(m.power, 38.4023438, 0.001));
        CHECK(close_to(m.voltage_mv, 12048.125, 0.001));
        CHECK(close_to(m.error_pct, 2.8451723, 0.001));
        CHECK(close_to(m.pool_latency_ms, 22.62, 0.001));
        CHECK(m.fan_pct == 80);
        CHECK(m.fan_rpm == 5200);
        CHECK(m.frequency_mhz == 540); // flat "frequency", no actualFrequency
        CHECK(m.shares_accepted == 2211);
        CHECK(m.shares_rejected == 1);
        CHECK(m.best_diff == 61123445678LL); // exceeds 32 bits
        CHECK(m.best_session_diff == 61123445678LL);
        CHECK(m.uptime_s == 1911600LL);
        // Pool mapping from the flat stratum fields.
        CHECK(m.pool_url == "example.org");
        CHECK(m.pool_port == 3333);
        CHECK(m.pool_user == "bc1qExample.WorkerTurbo");
        CHECK(m.using_fallback == false);
    }

    // Display fallback: older firmware without a 10m average reports its
    // instantaneous hashRate; unknown fields keep the -1 dash.
    {
        bx::MinerInfo m;
        CHECK(bx::parse_miner_info(
            "{\"hostname\":\"old\",\"hashRate\":333.5,\"hashRate_10m\":-1}",
            m));
        CHECK(close_to(m.display_hashrate(), 333.5, 0.001));
        bx::MinerInfo none;
        CHECK(bx::parse_miner_info("{\"hostname\":\"n\"}", none));
        CHECK(close_to(none.display_hashrate(), -1.0, 0.0));
    }

    // parse_miner_info rejects unparseable/empty input and leaves out intact.
    {
        bx::MinerInfo out;
        out.hostname = "sentinel";
        CHECK(!bx::parse_miner_info("", out));
        CHECK(!bx::parse_miner_info("   ", out));
        CHECK(!bx::parse_miner_info("{\"temp\":", out));
        CHECK(!bx::parse_miner_info("not json at all", out));
        CHECK(out.hostname == "sentinel");
    }

    // Sparse documents keep the "unknown" defaults; absent and explicit -1
    // temps both mean unknown.
    {
        bx::MinerInfo m;
        CHECK(bx::parse_miner_info("{}", m));
        CHECK(m.hostname == "Bitaxe"); // degenerate fallback, no ipv4 either
        CHECK(m.pool_port == 0);
        CHECK(m.using_fallback == false);
        CHECK(close_to(m.chip_temp, -1.0, 0.0));
        CHECK(close_to(m.chip2_temp, -1.0, 0.0));
        CHECK(close_to(m.vr_temp, -1.0, 0.0));
        CHECK(close_to(m.power, -1.0, 0.0));
        CHECK(m.fan_rpm == -1);
        CHECK(m.fan_pct == -1);
        CHECK(m.frequency_mhz == -1);
        CHECK(m.shares_accepted == -1);
        CHECK(m.uptime_s == -1);

        CHECK(bx::parse_miner_info("{\"temp\": -1, \"temp2\": -1, "
                                        "\"frequency\": 620}", m));
        CHECK(close_to(m.chip_temp, -1.0, 0.0));
        CHECK(close_to(m.chip2_temp, -1.0, 0.0));
        CHECK(m.frequency_mhz == 620);
        // Hostname falls back to "Bitaxe <ip>" when the API omits it.
        CHECK(bx::parse_miner_info("{\"ipv4\": \"192.0.2.5\"}", m));
        CHECK(m.hostname == "Bitaxe 192.0.2.5");
    }

    // Fallback-pool flag: modern key, legacy key, and boolean form.
    {
        bx::MinerInfo m;
        CHECK(bx::parse_miner_info(
            "{\"isUsingFallbackStratum\": 1, \"pools\": [{\"stratumURL\": "
            "\"p.io\", \"stratumPort\": 3333, \"stratumUser\": \"u\"}]}", m));
        CHECK(m.using_fallback == true);
        CHECK(m.pool_url == "p.io" && m.pool_port == 3333 && m.pool_user == "u");
        CHECK(bx::parse_miner_info(
            "{\"useFallbackStratum\": 1, \"stratumURL\": \"f.io\", "
            "\"stratumPort\": 4444, \"stratumUser\": \"v\"}", m));
        CHECK(m.using_fallback == true);
        CHECK(m.pool_user == "v" && m.pool_url == "f.io" && m.pool_port == 4444);
        CHECK(bx::parse_miner_info(
            "{\"isUsingFallbackStratum\": false, \"stratumURL\": \"g.io\", "
            "\"stratumPort\": 5555, \"stratumUser\": \"w\"}", m));
        CHECK(m.using_fallback == false);
        CHECK(m.pool_user == "w" && m.pool_port == 5555);
    }

    // Implausible sensor readings count as unknown: chip/VR temps outside
    // (0, 120] C map to -1 (only temperatures are clamped this way), while
    // temp2 = -1 keeps meaning "no second chip".
    {
        bx::MinerInfo m;
        CHECK(bx::parse_miner_info(
            "{\"temp\": 0.0, \"temp2\": 55.125, \"vrTemp\": 55}", m));
        CHECK(close_to(m.chip_temp, -1.0, 0.0));
        CHECK(close_to(m.chip2_temp, 55.125, 0.0)); // valid dual-chip reading
        CHECK(close_to(m.vr_temp, 55.0, 0.0));
        CHECK(bx::parse_miner_info(
            "{\"temp\": 200.7, \"temp2\": 0.0, \"vrTemp\": 120.5}", m));
        CHECK(close_to(m.chip_temp, -1.0, 0.0));
        CHECK(close_to(m.chip2_temp, -1.0, 0.0)); // bogus temp2 -> unknown
        CHECK(close_to(m.vr_temp, -1.0, 0.0));
        CHECK(bx::parse_miner_info(
            "{\"temp\": 120.0, \"temp2\": -1, \"vrTemp\": 0.5, "
            "\"hashRate\": 3.5, \"power\": 5.0}", m));
        CHECK(close_to(m.chip_temp, 120.0, 0.0)); // boundary is plausible
        CHECK(close_to(m.chip2_temp, -1.0, 0.0));
        CHECK(close_to(m.vr_temp, 0.5, 0.0));
        // ...and nothing but temperatures is clamped.
        CHECK(close_to(m.hash_rate_gh, 3.5, 0.0));
        CHECK(close_to(m.power, 5.0, 0.0));
    }

    // Integer fields keep exact 64-bit precision above 2^53.
    {
        bx::MinerInfo m;
        CHECK(bx::parse_miner_info(
            "{\"bestDiff\": 9007199254740993, \"sharesAccepted\": 9007199254740993}",
            m));
        CHECK(m.best_diff == 9007199254740993LL); // 2^53 + 1, not rounded
        CHECK(m.shares_accepted == 9007199254740993LL);
    }

    // Pure formatting, locale-independent.
    CHECK(bx::format_hashrate(1400.4205322) == "1.40 TH/s");
    CHECK(bx::format_hashrate(2066.74) == "2.07 TH/s");
    CHECK(bx::format_hashrate(999.5) == "999.50 GH/s");
    CHECK(bx::format_hashrate(0.0) == "0.00 GH/s");
    CHECK(bx::format_hashrate(1000.0) == "1.00 TH/s");
    CHECK(bx::format_hashrate(999.995) == "1.00 TH/s"); // rounding crosses
    CHECK(bx::format_hashrate(-1.0) == "\xE2\x80\x94"); // em dash for unknown
    CHECK(bx::format_hashrate(-12.5) == "\xE2\x80\x94");

    CHECK(bx::format_diff(-1.0) == "\xE2\x80\x94");
    CHECK(bx::format_diff(2.08e8) == "208.00M");
    CHECK(bx::format_diff(975000.0) == "975.00K");
    CHECK(bx::format_diff(999995.0) == "1.00M"); // rounding crosses
    CHECK(bx::format_diff(9792755360.0) == "9.79G");
    CHECK(bx::format_diff(2080028779.0) == "2.08G");

    CHECK(bx::format_uptime(59) == "<1m");
    CHECK(bx::format_uptime(90) == "1m");
    CHECK(bx::format_uptime(4320) == "1h12m");
    CHECK(bx::format_uptime(7320) == "2h02m");
    CHECK(bx::format_uptime(273600) == "3d04h"); // 3 d + 4 h
    CHECK(bx::format_uptime(-1) == "\xE2\x80\x94");

    // Wire format, header stripping, and body: a fast 200 that carries the
    // v2.12 fixture JSON verbatim, over an IPv4 loopback ephemeral port.
    {
        const std::string expect = file_bytes("tests/fixtures/device-v2.12.json");
        http_test::TestServer server(AF_INET, [&expect](int fd, std::string& seen) {
            seen = http_test::read_request(fd);
            http_test::write_all(fd, http_test::make_response(200, expect));
        });
        std::string body;
        const auto started = std::chrono::steady_clock::now();
        const auto result = bx::http_get("127.0.0.1", "/api/system/info", server.port(), 2000, body);
        const auto took = http_test::elapsed_ms(started);
        CHECK(took < 2000);
        server.finish();
        CHECK(result == bx::HttpResult::ok);
        CHECK(body == expect);
        CHECK(server.request().rfind("GET /api/system/info HTTP/1.0", 0) == 0);
        CHECK(server.request().find("Host: 127.0.0.1") != std::string::npos);
        CHECK(server.request().find("\r\n\r\n") != std::string::npos);
    }

    // IPv6: the devices report both families, so a ::1 literal must work too.
    {
        http_test::TestServer server(AF_INET6, [](int fd, std::string& seen) {
            seen = http_test::read_request(fd);
            http_test::write_all(fd, http_test::make_response(200, "{\"ok\":true}"));
        });
        std::string body = "sentinel";
        const auto result = bx::http_get("::1", "/ok", server.port(), 1000, body);
        server.finish();
        CHECK(result == bx::HttpResult::ok);
        CHECK(body == "{\"ok\":true}");
        CHECK(server.request().rfind("GET /ok HTTP/1.0", 0) == 0);
    }

    // Timeout bound: the server announces Content-Length but withholds the
    // body for 2500 ms; with a 2000 ms deadline the client must give up on its
    // own, in <= 2100 ms and without the body it never received.
    {
        const std::string expect = file_bytes("tests/fixtures/device-v2.12.json");
        http_test::TestServer server(AF_INET, [&expect](int fd, std::string& seen) {
            seen = http_test::read_request(fd);
            // Headers only: the body is announced but withheld, arriving (if
            // at all) after the client's deadline.
            std::ostringstream head;
            head << "HTTP/1.0 200 Status\r\nContent-Type: application/json\r\n"
                 << "Content-Length: " << expect.size() << "\r\n\r\n";
            http_test::write_all(fd, head.str());
            std::this_thread::sleep_for(std::chrono::milliseconds(2500));
        });
        std::string body = "stale";
        const auto started = std::chrono::steady_clock::now();
        const auto result = bx::http_get("127.0.0.1", "/api/system/info", server.port(), 2000, body);
        const auto took = http_test::elapsed_ms(started);
        CHECK(result == bx::HttpResult::timeout);
        CHECK(body.empty());
        CHECK(took >= 1900);
        CHECK(took <= 2100);
    }

    // A smaller deadline is honored too (here 300 ms) even when the server
    // accepts and then never answers at all.
    {
        http_test::TestServer server(AF_INET, [](int fd, std::string& seen) {
            seen = http_test::read_request(fd);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        });
        std::string body;
        const auto started = std::chrono::steady_clock::now();
        const auto result = bx::http_get("127.0.0.1", "/stall", server.port(), 300, body);
        const auto took = http_test::elapsed_ms(started);
        CHECK(result == bx::HttpResult::timeout);
        CHECK(body.empty());
        CHECK(took >= 250);
        CHECK(took <= 1000);
    }

    // A listening-but-closed port gives ECONNREFUSED, not a timeout.
    {
        const int probe = ::socket(AF_INET, SOCK_STREAM, 0);
        CHECK(probe >= 0);
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        CHECK(::bind(probe, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        socklen_t addrlen = sizeof(addr);
        CHECK(::getsockname(probe, reinterpret_cast<sockaddr*>(&addr), &addrlen) == 0);
        const int closed_port = ntohs(addr.sin_port);
        ::close(probe);

        std::string body;
        const auto result = bx::http_get("127.0.0.1", "/api/system/info", closed_port, 1000, body);
        CHECK(result == bx::HttpResult::refused);
        CHECK(body.empty());
    }

    // Only 200 is ok: 404 (and any other status) is a protocol error.
    {
        http_test::TestServer server(AF_INET, [](int fd, std::string& seen) {
            seen = http_test::read_request(fd);
            http_test::write_all(fd, http_test::make_response(404, "{\"missing\":true}"));
        });
        std::string body = "stale";
        const auto result = bx::http_get("127.0.0.1", "/nope", server.port(), 1000, body);
        server.finish();
        CHECK(result == bx::HttpResult::protocol_error);
        CHECK(body.empty());
    }

    // A response with no HTTP head at all is a protocol error, not ok.
    {
        http_test::TestServer server(AF_INET, [](int fd, std::string& seen) {
            seen = http_test::read_request(fd);
            http_test::write_all(fd, "this is not a response\r\n\r\n");
        });
        std::string body;
        const auto result = bx::http_get("127.0.0.1", "/junk", server.port(), 1000, body);
        CHECK(result == bx::HttpResult::protocol_error);
        CHECK(body.empty());
    }

    // A name that cannot resolve fails fast as dns_failed (no network lookup:
    // spaces are invalid in a hostname, so the resolver rejects immediately).
    {
        std::string body;
        const auto result = bx::http_get("not a host", "/api/system/info", 80, 500, body);
        CHECK(result == bx::HttpResult::dns_failed);
        CHECK(body.empty());
    }

    // ---- poller: per-device threads, farm aggregation, persistence hook ----

    const std::int64_t poller_now = std::chrono::duration_cast<std::chrono::seconds>(
                                        std::chrono::system_clock::now().time_since_epoch())
                                        .count();

    // Step 1, scenario 1: three devices, two reachable (the committed v2.15.3
    // and v2.12 fixtures, served over real loopback HTTP; the poller's
    // addresses are mapped onto the canned servers' ephemeral ports), one
    // unreachable (a released port: refused). The persistence hook writes the
    // polled list through save_devices, so the file carries fresh last_seen
    // for the two reachable devices and the stale value for the dead one.
    {
        const std::string fixture15 = file_bytes("tests/fixtures/device-v2.15.3.json");
        const std::string fixture12 = file_bytes("tests/fixtures/device-v2.12.json");
        poller_test::MultiServer server_a(AF_INET, poller_test::serve_body(fixture15));
        poller_test::MultiServer server_b(AF_INET, poller_test::serve_body(fixture12));
        const int dead_port = poller_test::closed_loopback_port();

        const std::map<std::string, int> port_by_address = {
            {"192.0.2.101", server_a.port()},
            {"192.0.2.102", server_b.port()},
            {"192.0.2.103", dead_port},
        };

        const auto dir = temp_dir("poller-persist");
        const auto file = dir / "devices-v1";

        bx::MinerPoller farm(
            80, 1000,
            [&port_by_address](const std::string& host, const std::string& path, int port,
                               int timeout_ms, std::string& body) {
                const auto it = port_by_address.find(host);
                if (it == port_by_address.end()) {
                    return bx::http_get(host, path, port, timeout_ms, body);
                }
                return bx::http_get("127.0.0.1", path, it->second, timeout_ms, body);
            },
            [] { return true; });
        farm.persist_cb = [&file](const std::vector<bx::SavedDevice>& devices) {
            bx::save_devices(file, devices);
        };
        farm.set_devices({
            bx::SavedDevice{"192.0.2.101", "Alpha 601", 1000},
            bx::SavedDevice{"192.0.2.102", "Turbo 800", 2000},
            bx::SavedDevice{"192.0.2.103", "sleepy", 1700000456},
        });

        CHECK(poller_test::wait_until([&] { return farm.online_count() == 2; }, 5000));

        // Parsed fixture values reach the UI through state(), raw: the
        // poller adds nothing and rewrites nothing.
        const bx::MinerState a = farm.state("192.0.2.101");
        CHECK(a.online);
        CHECK(a.info.hostname == "Alpha 601");
        CHECK(a.info.fw_version == "v2.15.3");
        CHECK(close_to(a.info.chip_temp, 47.75, 0.001));
        CHECK(close_to(a.info.chip2_temp, -1.0, 0.0)); // single chip stays "no second chip"
        CHECK(a.last_seen_epoch >= poller_now);        // seen this run, not invented

        const bx::MinerState b = farm.state("192.0.2.102");
        CHECK(b.online);
        CHECK(b.info.hostname == "Turbo 800");
        CHECK(close_to(b.info.chip2_temp, 58.625, 0.001)); // dual chip survives
        CHECK(close_to(b.info.power, 38.4023438, 0.001));

        // Aggregation counts online devices only; the dead one drops out of
        // the sums and never pretends to have been seen.
        const bx::MinerState dead = farm.state("192.0.2.103");
        CHECK(!dead.online);
        CHECK(dead.last_seen_epoch == 0);

        // The display preference reaches the totals: the farm sums each
        // device's 10m average (not the jittering instantaneous hashRate),
        // so the hero equals the exact sum of the per-device cards.
        CHECK(close_to(a.info.display_hashrate(), 1247.6013184, 0.001));
        CHECK(close_to(b.info.display_hashrate(), 1983.3986816, 0.001));
        CHECK(close_to(farm.farm_hashrate(),
                       a.info.display_hashrate() + b.info.display_hashrate(),
                       0.01)); // hero == the exact sum of the cards
        CHECK(close_to(farm.farm_hashrate(), 1247.6013184 + 1983.3986816, 0.01));
        CHECK(close_to(farm.farm_power(), 20.112548 + 38.4023438, 0.01));
        CHECK(close_to(farm.farm_efficiency(),
                       (20.112548 + 38.4023438) /
                           ((1247.6013184 + 1983.3986816) / 1000.0),
                       0.05)); // farm W per farm TH/s
        CHECK(farm.network_ok()); // net_check true decides it outright

        // The hook persisted the poller's device list: fresh last_seen for
        // the two reachable devices, the stale value passed through for the
        // offline device.
        CHECK(poller_test::wait_until(
            [&] {
                const auto loaded = bx::load_devices(file);
                if (loaded.size() != 3) return false;
                if (loaded[0].address != "192.0.2.101" ||
                    loaded[2].address != "192.0.2.103") {
                    return false;
                }
                return loaded[0].last_seen >= poller_now &&
                       loaded[1].last_seen >= poller_now &&
                       loaded[2].last_seen == 1700000456;
            },
            5000));

        // The canned server saw esp-miner's system-info request.
        server_a.finish();
        CHECK(server_a.request().rfind("GET /api/system/info", 0) == 0);
        server_b.finish();
        CHECK(server_b.request().rfind("GET /api/system/info", 0) == 0);

        std::filesystem::remove_all(dir);
    }

    // Step 1, scenario 2: all three unreachable with the interface probe up —
    // LAN fine, rigs dead, so network_ok stays true and the farm renders its
    // nothing-online states (0.0 / 0.0 / -1). Turning the injected net_check
    // stub false (and every probe still failing) is what reads "no network".
    {
        const int dead_port = poller_test::closed_loopback_port();
        std::atomic<int> probes{0};
        std::atomic<bool> interface_up{true};
        bx::MinerPoller dead_farm(
            80, 500,
            [&](const std::string&, const std::string& path, int, int timeout_ms,
                std::string& body) {
                ++probes;
                return bx::http_get("127.0.0.1", path, dead_port, timeout_ms, body);
            },
            [&] { return interface_up.load(); });
        dead_farm.set_devices({
            bx::SavedDevice{"10.9.9.1", "one", 0},
            bx::SavedDevice{"10.9.9.2", "two", 0},
            bx::SavedDevice{"10.9.9.3", "three", 0},
        });
        CHECK(poller_test::wait_until([&] { return probes.load() >= 3; }, 5000));

        CHECK(dead_farm.online_count() == 0);
        CHECK(dead_farm.farm_hashrate() == 0.0);
        CHECK(dead_farm.farm_power() == 0.0);
        CHECK(dead_farm.farm_efficiency() == -1);
        CHECK(dead_farm.network_ok()); // probes failed, interface up: not network

        interface_up.store(false);
        CHECK(!dead_farm.network_ok()); // interface down AND every probe failed
    }

    // network_ok() probe semantics with stubs: protocol_error means some HTTP
    // server answered, so it counts in the device's favour on a failed
    // interface probe; with no devices probed at all, a failed interface
    // probe alone reads "no network".
    {
        std::atomic<int> answered_probes{0};
        bx::MinerPoller answered(
            80, 500,
            [&](const std::string&, const std::string&, int, int, std::string&) {
                ++answered_probes;
                return bx::HttpResult::protocol_error;
            },
            [] { return false; });
        answered.set_devices({bx::SavedDevice{"10.0.0.5", "five", 0}});
        CHECK(poller_test::wait_until([&] { return answered_probes.load() >= 1; }, 3000));
        CHECK(answered.network_ok());

        bx::MinerPoller empty(80, 500,
                              [](const std::string&, const std::string&, int, int,
                                 std::string&) { return bx::HttpResult::refused; },
                              [] { return false; });
        CHECK(!empty.network_ok());
    }

    // One successful probe (stubbed ok) outvotes a false net_check.
    {
        bx::MinerPoller one_ok(
            80, 500,
            [](const std::string& host, const std::string&, int, int,
               std::string& body) {
                if (host == "10.0.0.1") {
                    body = "{\"hostname\":\"one\"}";
                    return bx::HttpResult::ok;
                }
                return bx::HttpResult::refused;
            },
            [] { return false; });
        one_ok.set_devices({
            bx::SavedDevice{"10.0.0.1", "one", 0},
            bx::SavedDevice{"10.0.0.2", "two", 0},
        });
        CHECK(poller_test::wait_until(
            [&] { return one_ok.state("10.0.0.1").online; }, 3000));
        CHECK(one_ok.online_count() == 1);
        CHECK(one_ok.network_ok());
    }

    // Step 1, scenario 3: the poller passes sensor values through untouched —
    // parse_miner_info already did the plausibility filter (implausible temps
    // clamped to -1), and the poller must not rewrite unknowns (or anything
    // else) on the way out. The hot rig reads raw; bogus and absent second
    // chips both land as -1.
    {
        const auto serve_fixed = [](const std::string& body) {
            return [body](const std::string&, const std::string&, int, int,
                          std::string& out) {
                out = body;
                return bx::HttpResult::ok;
            };
        };
        const auto probe_json = [&serve_fixed](const std::string& body) {
            bx::MinerPoller p(60, 500, serve_fixed(body), [] { return true; });
            p.set_devices({bx::SavedDevice{"10.1.1.9", "probe", 0}});
            CHECK(poller_test::wait_until(
                [&] { return p.state("10.1.1.9").online; }, 3000));
            return p.state("10.1.1.9").info;
        };

        const bx::MinerInfo hot = probe_json(
            "{\"hostname\":\"Hot\",\"hashRate\":76.5,\"power\":9.5,"
            "\"temp\":76.0,\"temp2\":96.5,\"vrTemp\":80.0}");
        CHECK(close_to(hot.chip_temp, 76.0, 0.0));    // plausible heat: raw
        CHECK(close_to(hot.chip2_temp, 96.5, 0.0));
        CHECK(close_to(hot.vr_temp, 80.0, 0.0));

        const bx::MinerInfo unknowns = probe_json(
            "{\"hostname\":\"U\",\"temp\": -1, \"temp2\": 0}");
        CHECK(close_to(unknowns.chip_temp, -1.0, 0.0)); // stays unknown post-filter
        CHECK(close_to(unknowns.chip2_temp, -1.0, 0.0)); // clamped 0, not rewritten

        const bx::MinerInfo single = probe_json(
            "{\"hostname\":\"S\",\"temp\": 44.375, \"temp2\": -1}");
        CHECK(close_to(single.chip_temp, 44.375, 0.0));
        CHECK(close_to(single.chip2_temp, -1.0, 0.0)); // -1 = no second chip

        // Hashless device: online (it answered) but counted as offline for
        // the hash-rate aggregation; its known power still sums.
        bx::MinerPoller hashless(
            60, 500,
            serve_fixed("{\"hostname\":\"H\",\"hashRate\": -1, "
                        "\"hashRate_10m\": -1, \"power\": 10.0}"),
            [] { return true; });
        hashless.set_devices({bx::SavedDevice{"10.1.1.8", "hashless", 0}});
        CHECK(poller_test::wait_until(
            [&] { return hashless.state("10.1.1.8").online; }, 3000));
        CHECK(hashless.online_count() == 1); // answered, so "online" ...
        CHECK(hashless.farm_hashrate() == 0.0); // ... but hashes nothing
        CHECK(hashless.farm_power() == 10.0);
        CHECK(hashless.farm_efficiency() == -1); // renders "—" in the UI

        // Fallback branch: a rig reporting only hash_rate_10m still counts.
        // Nothing in the farm knows its watts, so efficiency must render
        // "—" (-1), never an invented 0 W/TH.
        bx::MinerPoller ten_min(
            60, 500,
            serve_fixed("{\"hostname\":\"T\",\"hashRate\": -1, "
                        "\"hashRate_10m\": 500.0}"),
            [] { return true; });
        ten_min.set_devices({bx::SavedDevice{"10.1.1.7", "ten-min", 0}});
        CHECK(poller_test::wait_until(
            [&] { return ten_min.state("10.1.1.7").online; }, 3000));
        CHECK(ten_min.online_count() == 1);
        CHECK(ten_min.farm_hashrate() == 500.0); // the 10m field is the only value
        CHECK(ten_min.farm_power() == 0.0);
        CHECK(ten_min.farm_efficiency() == -1); // no known power anywhere

        // A rig with both fields aggregates through the display preference:
        // the 10m average, never the jittering instantaneous total.
        bx::MinerPoller steady(
            60, 500,
            serve_fixed("{\"hostname\":\"S\",\"hashRate\": 1000.0, "
                        "\"hashRate_10m\": 300.0}"),
            [] { return true; });
        steady.set_devices({bx::SavedDevice{"10.1.1.5", "steady", 0}});
        CHECK(poller_test::wait_until(
            [&] { return steady.state("10.1.1.5").online; }, 3000));
        CHECK(steady.state("10.1.1.5").info.display_hashrate() == 300.0);
        CHECK(steady.farm_hashrate() == 300.0);

        // An instant-only rig (older firmware) still counts, through the
        // fallback, and pins the W-per-TH formula.
        bx::MinerPoller rated(
            60, 500,
            serve_fixed("{\"hostname\":\"R\",\"hashRate\": 500.0, "
                        "\"power\": 12.5}"),
            [] { return true; });
        rated.set_devices({bx::SavedDevice{"10.1.1.6", "rated", 0}});
        CHECK(poller_test::wait_until(
            [&] { return rated.state("10.1.1.6").online; }, 3000));
        CHECK(rated.farm_hashrate() == 500.0);
        CHECK(rated.farm_power() == 12.5);
        CHECK(rated.farm_efficiency() == 25.0); // 12.5 W / 0.5 TH/s
    }

    // set_devices restarts the worker set cleanly: same list, repeatedly,
    // while the previous workers are mid-request, and the set polls again
    // afterwards. Destructor joins mid-request workers in bounded time.
    {
        std::atomic<int> stalls{0};
        const auto stall_ok = [&stalls](const std::string&, const std::string&, int,
                                        int, std::string& body) {
            ++stalls;
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            body = "{\"hostname\":\"stall\"}";
            return bx::HttpResult::ok;
        };
        const std::vector<bx::SavedDevice> same = {
            bx::SavedDevice{"10.2.0.1", "one", 0},
            bx::SavedDevice{"10.2.0.2", "two", 0},
            bx::SavedDevice{"10.2.0.3", "three", 0},
        };
        bx::MinerPoller restarted(60, 2000, stall_ok, [] { return true; });
        restarted.set_devices(same);
        restarted.set_devices(same); // restart while the first set is mid-stall
        restarted.set_devices(same);
        int after_restart = stalls.load();
        CHECK(poller_test::wait_until(
            [&] { return stalls.load() >= after_restart + 3; }, 5000));
        CHECK(poller_test::wait_until(
            [&] {
                return restarted.state("10.2.0.1").online &&
                       restarted.state("10.2.0.2").online &&
                       restarted.state("10.2.0.3").online;
            },
            5000));

        const auto torn_at = std::chrono::steady_clock::now();
        {
            bx::MinerPoller mid(60, 2000, stall_ok, [] { return true; });
            mid.set_devices(same);
            CHECK(poller_test::wait_until([&] { return stalls.load() >= 1; }, 3000));
        } // destructor: workers are inside their 120 ms stub or sleep
        const auto torn_ms = http_test::elapsed_ms(torn_at);
        CHECK(torn_ms < 3000); // bounded by the stub, not the 2000 ms timeout
    }

    // Focus: the addressed device polls at the 2 s focus cadence - with a
    // 4 s base interval, a third call within the test window can only come
    // from focus - and set_focus() truncates the in-flight wait on the spot.
    // Clearing focus restores the base cadence. The 4 s base keeps every
    // assertion's timing margin comfortable (2 s cadence vs 4 s base).
    {
        std::mutex stamp_gate;
        std::vector<long long> a_ms; // steady-clock ms of each call to A
        std::vector<long long> b_ms; // ... and to B
        std::atomic<int> a_calls{0};
        std::atomic<int> other_calls{0};
        const auto stamp_ms = [] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        };
        bx::MinerPoller focused(
            4000, 500,
            [&](const std::string& host, const std::string&, int, int,
                std::string& body) {
                if (host == "10.3.0.1") {
                    std::lock_guard<std::mutex> gate(stamp_gate);
                    a_ms.push_back(stamp_ms());
                    ++a_calls;
                } else {
                    std::lock_guard<std::mutex> gate(stamp_gate);
                    b_ms.push_back(stamp_ms());
                    ++other_calls;
                }
                body = "{\"hostname\":\"x\"}";
                return bx::HttpResult::ok;
            },
            [] { return true; });
        focused.set_devices({
            bx::SavedDevice{"10.3.0.1", "a", 0},
            bx::SavedDevice{"10.3.0.2", "b", 0},
        });
        focused.set_focus("10.3.0.1");

        // Three calls to the focused device: the set_devices first poll, the
        // focus truncation, then the 2 s cadence. Unfocused, the third call
        // would only land at ~8 s, so this proves focus - and proves the
        // truncation, which makes the third call land at ~2 s not ~5 s.
        CHECK(poller_test::wait_until([&] { return a_calls.load() >= 3; }, 4400));
        // The latest gap is the focus cadence: neither the 4 s base (>= 3 s)
        // nor a runaway loop (< 1 s).
        {
            std::lock_guard<std::mutex> gate(stamp_gate);
            CHECK(a_ms.size() >= 3);
            CHECK(a_ms.back() - a_ms[a_ms.size() - 2] >= 1000);
            CHECK(a_ms.back() - a_ms[a_ms.size() - 2] <= 2800);
        }
        // B is not focused: at most its start poll plus the ~4 s one so far.
        CHECK(other_calls.load() >= 1 && other_calls.load() <= 2);

        // Clearing focus: A is woken up at once (truncation) and then keeps
        // the 4 s base cadence - no 2 s-rhythm call inside the window below.
        focused.set_focus("");
        CHECK(poller_test::wait_until([&] { return a_calls.load() >= 4; }, 1200));
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        CHECK(a_calls.load() == 4);  // restored: next poll only at ~4 s
        // B's base cadence is pinned by its own stamps, independent of when
        // this check runs (total elapsed is scheduling-order dependent): its
        // second call may already have landed or still be due, but once two
        // exist their gap is the 4 s base, never the 2 s focus rhythm.
        CHECK(poller_test::wait_until(
            [&] {
                std::lock_guard<std::mutex> gate(stamp_gate);
                return b_ms.size() >= 2;
            },
            5000));
        {
            std::lock_guard<std::mutex> gate(stamp_gate);
            CHECK(b_ms.back() - b_ms[b_ms.size() - 2] >= 3000);
        }
    }

    // Back-off: a device that stays offline waits (interval doubled on each
    // miss) instead of hammering the dead address at the base cadence. At the
    // 60 ms base with misses at ~0, 120, 360 ms, 700 ms must see at most four
    // probes; the next would only land at ~840 ms.
    {
        const int dead_port = poller_test::closed_loopback_port();
        std::atomic<int> probes{0};
        bx::MinerPoller backoff(
            60, 500,
            [&](const std::string&, const std::string& path, int, int timeout_ms,
                std::string& body) {
                ++probes;
                return bx::http_get("127.0.0.1", path, dead_port, timeout_ms, body);
            },
            [] { return true; });
        backoff.set_devices({bx::SavedDevice{"10.4.0.1", "dead", 0}});
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        CHECK(probes.load() <= 4);
        CHECK(probes.load() >= 2);
    }

    // An injected http_fn must not throw; if it does anyway, the worker
    // counts the poll as failed instead of std::terminating the process.
    {
        bx::MinerPoller thrower(
            60, 500,
            [](const std::string&, const std::string&, int, int,
               std::string&) -> bx::HttpResult {
                throw std::runtime_error("injected stub blew up");
            },
            [] { return false; });
        thrower.set_devices({bx::SavedDevice{"10.5.0.1", "boom", 0}});
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CHECK(!thrower.state("10.5.0.1").online);
        CHECK(!thrower.network_ok()); // a thrown poll counts as a failed probe
    }

    // Duplicate addresses collapse to one worker (first entry wins): a rig
    // listed twice stays one device for state/online_count and cannot be
    // polled twice at once.
    {
        const int dead_port = poller_test::closed_loopback_port();
        bx::MinerPoller dupes(
            60, 500,
            [&dead_port](const std::string& host, const std::string& path, int,
                         int timeout_ms, std::string& body) {
                if (host == "10.6.0.1") {
                    body = "{\"hostname\":\"twice\"}";
                    return bx::HttpResult::ok;
                }
                return bx::http_get("127.0.0.1", path, dead_port, timeout_ms,
                                    body);
            },
            [] { return true; });
        dupes.set_devices({
            bx::SavedDevice{"10.6.0.1", "twice", 0},
            bx::SavedDevice{"10.6.0.1", "twice", 0},
            bx::SavedDevice{"10.6.0.2", "gone", 0},
            bx::SavedDevice{"10.6.0.2", "gone", 0},
        });
        CHECK(poller_test::wait_until(
            [&] { return dupes.state("10.6.0.1").online; }, 3000));
        CHECK(dupes.online_count() == 1); // two entries, one worker
    }

    // set_devices carries the fresher in-memory last_seen for surviving
    // addresses: the caller's list is the stale file view, and the
    // persistence callback must not roll a live rig back to it.
    {
        std::mutex saves_gate;
        std::vector<bx::SavedDevice> last_saved;
        std::atomic<int> saves{0};
        bx::MinerPoller carried(
            60, 500,
            [](const std::string&, const std::string&, int, int,
               std::string& body) {
                body = "{\"hostname\":\"kept\"}";
                return bx::HttpResult::ok;
            },
            [] { return true; });
        carried.persist_cb = [&](const std::vector<bx::SavedDevice>& devices) {
            {
                std::lock_guard<std::mutex> gate(saves_gate);
                last_saved = devices;
            }
            ++saves;
        };
        carried.set_devices({
            bx::SavedDevice{"10.7.0.1", "kept", 111},
            bx::SavedDevice{"10.7.0.9", "gone", 222},
        });
        CHECK(poller_test::wait_until(
            [&] {
                return carried.state("10.7.0.1").online && saves.load() >= 1;
            },
            3000));
        const int saves_before = saves.load();
        carried.set_devices({
            bx::SavedDevice{"10.7.0.1", "kept", 7}, // caller's stale view
            bx::SavedDevice{"10.7.0.2", "new", 0},
        });
        // The flush after the swap must show A still at its fresh in-memory
        // last_seen (carried, not rolled back to the caller's 7), the new
        // device polled and stamped, and the caller's order/hostname kept.
        CHECK(poller_test::wait_until(
            [&] {
                std::lock_guard<std::mutex> gate(saves_gate);
                return saves.load() > saves_before && last_saved.size() == 2 &&
                       last_saved[0].address == "10.7.0.1" &&
                       last_saved[0].hostname == "kept" &&
                       last_saved[0].last_seen >= poller_now &&
                       last_saved[1].address == "10.7.0.2" &&
                       last_saved[1].last_seen >= poller_now;
            },
            3000));
    }

    // ---- discover: mDNS response parsing against the synthetic fixture ----

    // The fixture is a synthetic response datagram in the same shape the
    // exporter produced (see scripts/capture-mdns.py's record layout): PTR
    // answers for the instance (one of them the
    // _axeos._sub PTR), SRV to "Alpha601.local" on port 80, TXT records and
    // its A record, all joined through name compression pointers.
    {
        const std::string& text = file_bytes("tests/fixtures/mdns-response.bin");
        const mdns_test::Bytes packet(text.begin(), text.end());
        const auto devices = bx::parse_mdns_response(packet);
        CHECK(devices.size() == 1);
        const bx::DiscoveredDevice& bitaxe = devices[0];
        CHECK(bitaxe.instance.find("Bitaxe") == 0);
        CHECK(bitaxe.host == "Alpha601.local");
        CHECK(bitaxe.address == "192.0.2.12");
        CHECK(bitaxe.port == 80);
        CHECK(bitaxe.txt.size() == 5);
        CHECK(bitaxe.txt.at("asic") == "BM1370");
        CHECK(bitaxe.txt.at("board") == "601");
        CHECK(bitaxe.txt.at("family") == "Gamma");
        CHECK(bitaxe.txt.at("asic_count") == "1");
        CHECK(bitaxe.txt.at("fw_version") == "v2.15.3");
        // The filter keeps the Bitaxe, TXT "asic" or the instance prefix.
        CHECK(bx::filter_baxes(devices).size() == 1);
    }

    // The printer entry alone: parsed unfiltered (one plain device, its own
    // SRV/TXT/A records intact), then filtered out by the Bitaxe rule.
    {
        mdns_test::Bytes packet = mdns_test::header(1, 3);
        const mdns_test::Bytes parts = mdns_test::printer_ptr();
        packet.insert(packet.end(), parts.begin(), parts.end());
        const mdns_test::Bytes more = mdns_test::printer_srv_txt_a();
        packet.insert(packet.end(), more.begin(), more.end());
        const auto devices = bx::parse_mdns_response(packet);
        CHECK(devices.size() == 1);
        CHECK(devices[0].instance == "Office Printer");
        CHECK(devices[0].host == "printer.local");
        CHECK(devices[0].address == "192.0.2.23");
        CHECK(devices[0].port == 9100);
        CHECK(devices[0].txt.at("path") == "/webadmin");
        CHECK(devices[0].txt.at("pdl") == "postscript");
        CHECK(devices[0].txt.count("asic") == 0);
        CHECK(bx::filter_baxes(devices).empty());
    }

    // Spliced variant: the printer records appended to the live fixture.
    // Both entries parse, only the Bitaxe survives the filter, with its
    // fields unchanged by the company it keeps.
    {
        const std::string& text = file_bytes("tests/fixtures/mdns-response.bin");
        mdns_test::Bytes mixed(text.begin(), text.end());
        CHECK(mixed[7] == 2); // two answers: the instance PTR twice
        ++mixed[7];           // + the printer PTR answer
        CHECK(mixed[11] == 5); // SRV, TXT, A and two AAAA
        mixed[11] += 3;        // + the printer SRV, TXT and A
        const mdns_test::Bytes parts = mdns_test::printer_ptr();
        mixed.insert(mixed.end(), parts.begin(), parts.end());
        const mdns_test::Bytes more = mdns_test::printer_srv_txt_a();
        mixed.insert(mixed.end(), more.begin(), more.end());
        CHECK(mixed.size() == text.size() + parts.size() + more.size());
        const auto devices = bx::parse_mdns_response(mixed);
        CHECK(devices.size() == 2);
        const auto kept = bx::filter_baxes(devices);
        CHECK(kept.size() == 1);
        CHECK(kept[0].address == "192.0.2.12");
        CHECK(kept[0].port == 80);
        CHECK(kept[0].txt.at("asic") == "BM1370");
    }

    // Bytes that do not parse come back as an empty vector, never a hang and
    // never a throw: short header, cut mid-name and mid-record, a
    // self-pointing compression loop, a pair of pointers bouncing between
    // each other, a pointer into the void, and plain garbage.
    {
        const std::string& text = file_bytes("tests/fixtures/mdns-response.bin");
        const mdns_test::Bytes fixture(text.begin(), text.end());
        const auto tail = [&fixture](std::size_t length) {
            return mdns_test::Bytes(fixture.begin(),
                                    fixture.begin() + static_cast<long>(length));
        };
        mdns_test::Bytes self_loop = mdns_test::header(0, 1);
        const mdns_test::Bytes self_loop_tail = {
            0xC0, 0x0C, // owner name: a pointer pointing back at itself
            0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x78, 0x00, 0x04,
            192, 0, 2, 12};
        self_loop.insert(self_loop.end(), self_loop_tail.begin(),
                         self_loop_tail.end());
        mdns_test::Bytes bounce_loop = mdns_test::header(0, 1);
        const mdns_test::Bytes bounce_loop_tail = {0xC0, 0x0E, 0xC0, 0x0C};
        bounce_loop.insert(bounce_loop.end(), bounce_loop_tail.begin(),
                           bounce_loop_tail.end());
        mdns_test::Bytes void_pointer = mdns_test::header(0, 1);
        const mdns_test::Bytes void_pointer_tail = {0xC0, 0xFB};
        void_pointer.insert(void_pointer.end(), void_pointer_tail.begin(),
                            void_pointer_tail.end());
        const std::vector<mdns_test::Bytes> malformed = {
            {},
            tail(11),  // one byte short of a header
            tail(21),  // cut inside the first name
            tail(40),  // cut between ttl and rdlength
            tail(fixture.size() - 1), // the last rdata byte missing
            self_loop,
            bounce_loop,
            void_pointer,
            mdns_test::Bytes(text.begin(), text.begin() + 5), // too short to be a message
        };
        for (const auto& packet : malformed) {
            CHECK(bx::parse_mdns_response(packet).empty());
        }
        // A query is not a response: with QR set but questions only, nothing
        // to report — and still not an error.
        mdns_test::Bytes query(12, 0);
        query[2] = 0x80; // QR set
        query[5] = 1;    // one question
        const mdns_test::Bytes question = mdns_test::name_bytes(
            {"_http", "_tcp", "local"});
        query.insert(query.end(), question.begin(), question.end());
        mdns_test::append_u16(query, 12);
        mdns_test::append_u16(query, 1);
        CHECK(bx::parse_mdns_response(query).empty());
    }

    // discover_baxes(0) touches no network: empty result, no throw, no wait.
    CHECK(bx::discover_baxes(0).empty());
    CHECK(bx::discover_baxes(-1).empty());

    // A real listen window stays inside its budget and returns promptly
    // whether or not anything answers (content depends on the LAN).
    {
        const auto started = std::chrono::steady_clock::now();
        const auto found = bx::discover_baxes(250);
        const auto took = http_test::elapsed_ms(started);
        CHECK(took <= 1000);
        (void)found;
    }

    std::cout << "bx-tests OK\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "bx-tests FAILED: " << e.what() << '\n';
    return 1;
}
