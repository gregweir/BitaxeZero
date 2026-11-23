#!/usr/bin/env python3
# Captures one raw mDNS response packet carrying the Bitaxe _http._tcp records
# and writes it to tests/fixtures/mdns-response.bin. Standard-library only.
#
# It sends the standard PTR query for _http._tcp.local to the 224.0.0.251:5353
# multicast group, first trying to listen on 5353 itself like avahi does (with
# SO_REUSEADDR/SO_REUSEPORT so both can share the port, joining the group on
# the chosen interface), and falling back to a legacy-querier socket on an
# ephemeral port. The first received packet that mentions the Bitaxe instance
# is the fixture (a synthetic response of the same shape is committed: the
# wire instance label is "Bitaxe Alpha 601 (ABCD)"; avahi's
# display escaping renders its spaces and parentheses as \032, \040, \041).
#
# usage: python3 scripts/capture-mdns.py [IFADDR] [TARGET]
#   IFADDR  local address to send the query from (default: auto)
#   TARGET  responder address to query by unicast (default: none). Some
#           wireless mDNS clients answer direct unicast queries even when the
#           access point does not deliver group-addressed queries to them, so
#           the script first asks by multicast and then falls back to a
#           unicast query with the QU (unicast-response) bit to TARGET.

import socket
import sys
import time

MDNS_GROUP = "224.0.0.251"
MDNS_PORT = 5353
WAIT_SECONDS = 6


def build_query(unicast_response: bool = False) -> bytes:
    # DNS header: one question, no flags set (legacy unicast query).
    header = b"\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00"
    name = b"".join(
        len(part).to_bytes(1, "big") + part
        for part in (b"_http", b"_tcp", b"local")
    ) + b"\x00"
    ptr = b"\x00\x0c"  # QTYPE PTR
    qclass = b"\x80\x01" if unicast_response else b"\x00\x01"
    return header + name + ptr + qclass


def capture(packet_source, sender_filter, deadline) -> bytes:
    while time.time() < deadline:
        packet_source.settimeout(max(0.1, deadline - time.time()))
        try:
            packet, sender = packet_source.recvfrom(65535)
        except socket.timeout:
            break
        print("response from %s: %d bytes" % (sender[0], len(packet)))
        if sender_filter(sender) and b"Bitaxe" in packet:
            print("fixture packet found")
            print(hexdump(packet))
            return packet
    return b""


def main() -> int:
    ifaddr = sys.argv[1] if len(sys.argv) > 1 else ""
    target = sys.argv[2] if len(sys.argv) > 2 else ""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    except OSError:
        pass  # optional
    joined = False
    try:
        sock.bind(("", MDNS_PORT))
        # As a standard mDNS port, responders answer on the multicast group.
        membership = socket.inet_aton(MDNS_GROUP) + \
            socket.inet_aton(ifaddr or "0.0.0.0")
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, membership)
        joined = True
        print("listening on 5353, group joined")
    except OSError as e:
        # Legacy querier: respond to the ephemeral port instead.
        print("bind 5353 failed (%s), using an ephemeral port" % e)
        sock.bind(("", 0))
        joined = ifaddr != ""
        if joined:
            membership = socket.inet_aton(MDNS_GROUP) + socket.inet_aton(ifaddr)
            sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                            membership)
    address = ("", MDNS_PORT)
    if ifaddr:
        # Pin the query to the one LAN interface.
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF,
                        socket.inet_aton(ifaddr))
    elif joined and sock.getsockname()[0] != "0.0.0.0":
        address = sock.getsockname()
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
    deadline = time.time() + WAIT_SECONDS
    # A few repeats: one lost datagram should not cost the whole capture.
    for _ in range(3):
        sock.sendto(build_query(), address)
        time.sleep(0.3)
    print("query sent, listening for responses...")
    packet = capture(sock, lambda sender: True, deadline)

    if not packet and target and ifaddr:
        # Direct unicast query with the QU bit, bypassing group delivery.
        print("no multicast response, querying %s by unicast" % target)
        sock.sendto(build_query(unicast_response=True), (target, MDNS_PORT))
        packet = capture(sock, lambda sender: sender[0] == target,
                         time.time() + WAIT_SECONDS)

    sock.close()
    if not packet:
        print("no Bitaxe response captured")
        return 1
    with open("tests/fixtures/mdns-response.bin", "wb") as out:
        out.write(packet)
    print("fixture written: %d bytes" % len(packet))
    return 0


def hexdump(data: bytes) -> str:
    lines = []
    for offset in range(0, len(data), 16):
        chunk = data[offset:offset + 16]
        hexs = " ".join("%02x" % b for b in chunk)
        text = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append("%04x  %-47s  %s" % (offset, hexs, text))
    return "\n".join(lines)


if __name__ == "__main__":
    sys.exit(main())
