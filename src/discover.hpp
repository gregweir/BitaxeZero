// mDNS discovery of Bitaxe _http._tcp services: a pure DNS message parser
// (RFC 1035/6762, name compression, A/SRV/TXT/PTR records) and a POSIX
// multicast querier for the 224.0.0.251:5353 group. No avahi, no D-Bus.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bx {

// One service instance seen on the network. instance is the DNS-SD instance
// label verbatim (e.g. "Bitaxe Alpha 601 (ABCD)"), host the SRV target,
// address the dotted-quad A record, port the SRV port, txt the decoded
// TXT key=value strings.
struct DiscoveredDevice {
    std::string instance;
    std::string host;
    std::string address;
    int port = 0;
    std::map<std::string, std::string> txt;
};

// Parses one mDNS response datagram. Devices are keyed by instance; PTR
// answers, SRV, TXT and A records of one instance assemble into a single
// entry (any of which may be absent on its own). A datagram that does not
// parse — too short, truncated mid-record, looping or out-of-range
// compression pointers — yields an empty vector, never a throw. Non-mDNS
// entries (printers, NAS, ...) come back too; discover_baxes filters them.
std::vector<DiscoveredDevice> parse_mdns_response(
    const std::vector<std::uint8_t>& packet);

// Keeps only the entries that look like a Bitaxe: a TXT map carrying an
// "asic" key, or an instance label starting with "Bitaxe".
std::vector<DiscoveredDevice> filter_baxes(std::vector<DiscoveredDevice> devices);

// Sends the standard PTR query for _http._tcp.local to 224.0.0.251:5353 once
// and collects responses for listen_ms. When the shared mDNS port is taken,
// it binds an ephemeral port as a legacy querier and sets the QU bit so
// responders answer by unicast. Returns the Bitaxe entries only. Never
// throws: on any socket failure the result is simply empty, and a listen_ms
// <= 0 returns immediately without touching the network.
std::vector<DiscoveredDevice> discover_baxes(int listen_ms);

} // namespace bx

