#include "discover.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <limits>
#include <map>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace bx {

namespace {

using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;

// Record types this parser cares about.
constexpr std::uint16_t kTypeA = 1;
constexpr std::uint16_t kTypePtr = 12;
constexpr std::uint16_t kTypeTxt = 16;
constexpr std::uint16_t kTypeSrv = 33;

// Milliseconds until the deadline, 0 once it has been spent.
int remaining_ms(Clock::time_point deadline) {
    const auto now = Clock::now();
    if (now >= deadline) return 0;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        deadline - now)
                        .count();
    if (ms > std::numeric_limits<int>::max()) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(ms);
}

char char_lower(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

// DNS names compare case-insensitively (RFC 1035 2.3.3), labels preserved
// byte for byte otherwise.
bool name_ci_equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (char_lower(a[i]) != char_lower(b[i])) return false;
    }
    return true;
}

// Ordering for the device map: whole instance names, case-insensitively.
struct NameLess {
    bool operator()(const std::string& a, const std::string& b) const {
        for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
            const unsigned char left = static_cast<unsigned char>(char_lower(a[i]));
            const unsigned char right = static_cast<unsigned char>(char_lower(b[i]));
            if (left != right) return left < right;
        }
        return a.size() < b.size();
    }
};

// Whether name ends with suffix on a label boundary, case-insensitively.
bool name_suffix_matches(const std::string& name, const std::string& suffix) {
    if (name.size() < suffix.size() + 1) return false; // needs a label before it
    const std::size_t at = name.size() - suffix.size();
    if (name[at - 1] != '.') return false;
    return name_ci_equal(name.substr(at), suffix);
}

// The instance label is the first label of a service instance name.
std::string first_label(const std::string& name) {
    const std::size_t dot = name.find('.');
    if (dot == std::string::npos) return name;
    return name.substr(0, dot);
}

// Decodes the name encoded at packet[offset] into name (labels joined with
// dots, label bytes verbatim — control characters included, since an mDNS
// instance label is only escaped for display elsewhere). Advances offset past
// the encoding, following compression pointers but at most 8 of them and at
// most 63 labels in 255 bytes total: a looping, cycling or out-of-range
// pointer chain fails the parse instead of hanging it.
bool parse_name(const Bytes& packet, std::size_t& offset, std::string& name) {
    constexpr std::size_t kMaxLabels = 63;
    constexpr std::size_t kMaxJumps = 8;
    constexpr std::size_t kMaxNameBytes = 255;
    std::size_t scan = offset; // read position in the name encoding
    std::size_t end = offset;  // first position past the name encoding
    bool jumped = false;
    std::size_t jumps = 0;
    std::size_t labels = 0;
    std::size_t name_bytes = 0;
    for (;;) {
        if (scan >= packet.size()) return false;
        const std::uint8_t first = packet[scan];
        if (first == 0) { // end of the name
            if (!jumped) end = scan + 1;
            break;
        }
        switch (first & 0xC0) {
            case 0x00: { // ordinary label, 1..63 bytes
                const std::size_t length = first;
                if (packet.size() - scan < length + 1) return false;
                if (labels == kMaxLabels) return false;
                ++labels;
                if (!name.empty()) {
                    name.push_back('.');
                    if (++name_bytes > kMaxNameBytes) return false;
                }
                for (std::size_t i = 0; i < length; ++i) {
                    name.push_back(static_cast<char>(packet[scan + 1 + i]));
                }
                name_bytes += length;
                if (name_bytes > kMaxNameBytes) return false;
                scan += 1 + length;
                break;
            }
            case 0xC0: { // compression pointer, 14-bit offset from the start
                if (jumps == kMaxJumps) return false;
                if (packet.size() - scan < 2) return false;
                const std::size_t target =
                    (static_cast<std::size_t>(first & 0x3F) << 8) | packet[scan + 1];
                if (!jumped) end = scan + 2;
                scan = target;
                jumped = true;
                ++jumps;
                break;
            }
            default:
                // 0x40/0x80 label forms (EDNS0 extended labels, binary
                // labels) do not occur in mDNS traffic this parser reads.
                return false;
        }
    }
    offset = end;
    return true;
}

// One resource record of a parsed message: owner name, type, and where its
// rdata lives in the packet.
struct Record {
    std::string name;
    std::uint16_t type = 0;
    std::size_t rdata = 0;
    std::size_t rdata_size = 0;
};

// Reads one DNS message body: questions (and their entries) are skipped,
// A/PTR/TXT/SRV answers collected in packet order. Malformed — short
// header, truncated name or record, rdata wider than the packet — fails.
bool parse_records(const Bytes& packet, std::vector<Record>& out) {
    if (packet.size() < 12) return false;
    const std::uint16_t flags =
        static_cast<std::uint16_t>((packet[2] << 8) | packet[3]);
    if ((flags & 0x8000) == 0) return true; // a query, nothing to report

    std::size_t counts[4];
    for (std::size_t section = 0; section < 4; ++section) {
        const std::size_t at = 4 + 2 * section;
        counts[section] = (packet[at] << 8) | packet[at + 1];
    }

    std::size_t offset = 12;
    for (std::size_t section = 0; section < 4; ++section) { // qd, an, ns, ar
        const bool is_question = section == 0;
        for (std::size_t entry = 0; entry < counts[section]; ++entry) {
            Record record;
            if (!parse_name(packet, offset, record.name)) return false;
            if (packet.size() - offset < 4) return false; // type + class
            record.type = static_cast<std::uint16_t>((packet[offset] << 8) |
                                                     packet[offset + 1]);
            offset += 4;
            if (is_question) continue;
            if (packet.size() - offset < 6) return false; // ttl + rdlength
            record.rdata_size =
                static_cast<std::size_t>((packet[offset + 4] << 8) |
                                         packet[offset + 5]);
            offset += 6;
            if (record.rdata_size > packet.size() - offset) return false;
            record.rdata = offset;
            offset += record.rdata_size;
            if (record.type == kTypeA || record.type == kTypePtr ||
                record.type == kTypeTxt || record.type == kTypeSrv) {
                out.push_back(std::move(record));
            }
        }
    }
    return true;
}

constexpr const char* kServiceSuffix = "_http._tcp.local";

// Whether the name is a full service instance name of the type we browse
// for: <instance>._http._tcp.local (case-insensitive on the suffix).
bool instance_name_matches(const std::string& name) {
    return name_suffix_matches(name, kServiceSuffix);
}

// The rdata bytes [record.rdata, record.rdata + record.rdata_size) hold a
// (possibly compressed) name; decodes it and checks that the part stored in
// the rdata stays inside the rdata.
bool parse_rdata_name(const Bytes& packet, const Record& record,
                      std::size_t at, std::string& name) {
    if (!parse_name(packet, at, name)) return false;
    return at <= record.rdata + record.rdata_size;
}

void merge_device(DiscoveredDevice& into, const DiscoveredDevice& from) {
    if (!from.instance.empty()) into.instance = from.instance;
    if (!from.host.empty()) into.host = from.host;
    if (!from.address.empty()) into.address = from.address;
    if (from.port != 0) into.port = from.port;
    for (const auto& entry : from.txt) into.txt[entry.first] = entry.second;
}

std::string dotted_address(const Bytes& packet, std::size_t at) {
    std::string address = std::to_string(packet[at]);
    for (std::size_t i = 1; i < 4; ++i) {
        address.push_back('.');
        address += std::to_string(packet[at + i]);
    }
    return address;
}

} // namespace

std::vector<DiscoveredDevice> parse_mdns_response(
    const std::vector<std::uint8_t>& packet) {
    std::vector<Record> records;
    if (!parse_records(packet, records)) return {};

    // Devices keyed by their whole instance name (case-insensitively, so a
    // different-case spelling in a later record still reaches the same
    // device; the decoded labels keep their case).
    std::map<std::string, DiscoveredDevice, NameLess> found;

    // Pass 1: the answers that name an instance — PTR targets, SRV and TXT
    // owners — create devices; SRV and TXT fill them.
    for (const Record& record : records) {
        switch (record.type) {
            case kTypePtr: {
                std::string target;
                if (!parse_rdata_name(packet, record, record.rdata, target)) {
                    return {};
                }
                if (instance_name_matches(target)) {
                    DiscoveredDevice& device = found[target];
                    if (device.instance.empty()) {
                        device.instance = first_label(target);
                    }
                }
                break;
            }
            case kTypeSrv: {
                // priority (2) + weight (2) + port (2) + at least one name byte
                if (record.rdata_size < 7) return {};
                if (packet.size() - (record.rdata + 6) < 1) return {};
                if (!instance_name_matches(record.name)) continue;
                DiscoveredDevice& device = found[record.name];
                if (device.instance.empty()) {
                    device.instance = first_label(record.name);
                }
                device.port = (packet[record.rdata + 4] << 8) | packet[record.rdata + 5];
                std::string host;
                if (!parse_rdata_name(packet, record, record.rdata + 6, host)) {
                    return {};
                }
                device.host = host;
                break;
            }
            case kTypeTxt: {
                if (!instance_name_matches(record.name)) continue;
                DiscoveredDevice& device = found[record.name];
                if (device.instance.empty()) {
                    device.instance = first_label(record.name);
                }
                // RFC 1035 4.1.4/6.1: character-strings, each with a one-byte
                // length prefix; a zero-length string is padding.
                const std::size_t end = record.rdata + record.rdata_size;
                std::size_t at = record.rdata;
                while (at < end) {
                    const std::size_t length = packet[at];
                    if (length > end - (at + 1)) return {};
                    ++at;
                    if (length == 0) continue;
                    const std::string entry(packet.begin() + static_cast<long>(at),
                                            packet.begin() + static_cast<long>(at + length));
                    at += length;
                    const std::size_t equals = entry.find('=');
                    if (equals == 0 || equals == std::string::npos) continue;
                    device.txt[entry.substr(0, equals)] = entry.substr(equals + 1);
                }
                break;
            }
            default:
                break; // A records second pass, AAAA and the rest ignored
        }
    }

    // Pass 2: A records fill addresses, matched case-insensitively against
    // the hosts the SRV targets announced.
    for (const Record& record : records) {
        if (record.type != kTypeA) continue;
        if (record.rdata_size != 4) return {};
        for (auto& entry : found) {
            DiscoveredDevice& device = entry.second;
            if (device.address.empty() && !device.host.empty() &&
                name_ci_equal(device.host, record.name)) {
                device.address = dotted_address(packet, record.rdata);
            }
        }
    }

    std::vector<DiscoveredDevice> devices;
    devices.reserve(found.size());
    for (auto& entry : found) devices.push_back(std::move(entry.second));
    return devices;
}

std::vector<DiscoveredDevice> filter_baxes(
    std::vector<DiscoveredDevice> devices) {
    devices.erase(
        std::remove_if(devices.begin(), devices.end(),
                       [](const DiscoveredDevice& device) {
                           const bool has_asic =
                               device.txt.find("asic") != device.txt.end();
                           const bool bitaxe_named =
                               device.instance.rfind("Bitaxe", 0) == 0;
                           return !has_asic && !bitaxe_named;
                       }),
        devices.end());
    return devices;
}

std::vector<DiscoveredDevice> discover_baxes(int listen_ms) {
    std::vector<DiscoveredDevice> devices;
    if (listen_ms <= 0) return devices;

    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return devices;

    // The standard mDNS port first, shared with any local resolver the way
    // avahi shares it; when the port is not ours, fall back to an ephemeral
    // port — a legacy querier — and ask responders to answer by unicast.
    int reuse = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif
    sockaddr_in local {};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(5353);
    bool unicast_response = false;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        local.sin_port = 0;
        if (::bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
            ::close(fd);
            return devices;
        }
        unicast_response = true;
    }

    // Join the group on the kernel-chosen interface; unicast replies need no
    // group membership, so a failed join is not fatal.
    ip_mreq group {};
    group.imr_multiaddr.s_addr = htonl(0xE00000FB); // 224.0.0.251
    group.imr_interface.s_addr = htonl(INADDR_ANY);
    (void)::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &group,
                       sizeof(group));

    // The standard one-shot browse query (RFC 6762/6763): PTR _http._tcp.local.
    Bytes query;
    query.resize(12, 0);
    query[2] = 0; // id 0, no flags: a plain question
    query[4] = 0; query[5] = 1; // one question
    const char kQuestion[] = "\x05_http\x04_tcp\x05local";
    query.insert(query.end(), kQuestion, kQuestion + sizeof(kQuestion) - 1);
    query.push_back(0);
    query.push_back(0); query.push_back(kTypePtr);
    query.push_back(0); query.push_back(1); // class IN, or IN with the QU bit
    if (unicast_response) query.back() |= 0x80;

    sockaddr_in dest {};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(5353);
    dest.sin_addr.s_addr = htonl(0xE00000FB);

    const auto deadline = Clock::now() + std::chrono::milliseconds(listen_ms);
    if (::sendto(fd, query.data(), query.size(), 0,
                 reinterpret_cast<const sockaddr*>(&dest), sizeof(dest)) < 0) {
        // The group query may still have left; the unicast announcements the
        // window hears are worth the wait just the same.
    }

    // All instances seen during the window: instance names are unique per
    // service type, so the map keeps one device per instance, merged from
    // every datagram that names it (latest non-empty value per field).
    std::map<std::string, DiscoveredDevice, NameLess> found;

    for (;;) {
        const int remaining = remaining_ms(deadline);
        if (remaining <= 0) break;
        pollfd poll_set {};
        poll_set.fd = fd;
        poll_set.events = POLLIN;
        const int ready = ::poll(&poll_set, 1, remaining);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) break; // the budget is spent
        Bytes datagram(9216);
        sockaddr_in from {};
        socklen_t from_length = sizeof(from);
        const ssize_t received =
            ::recvfrom(fd, datagram.data(), datagram.size(), 0,
                       reinterpret_cast<sockaddr*>(&from), &from_length);
        if (received <= 0) {
            if (received < 0 && errno == EINTR) continue;
            break;
        }
        const auto parsed = parse_mdns_response(
            Bytes(datagram.begin(), datagram.begin() + received));
        // An instance name is the identifier of one service instance, so the
        // map keeps one device per instance across the whole window.
        for (const DiscoveredDevice& device : parsed) {
            if (device.instance.empty()) continue;
            DiscoveredDevice& known = found[device.instance];
            merge_device(known, device);
        }
    }

    ::close(fd);
    devices.reserve(found.size());
    for (auto& entry : found) devices.push_back(std::move(entry.second));
    return filter_baxes(std::move(devices));
}

} // namespace bx
