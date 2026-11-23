#include "http.hpp"

#include <cctype> // std::toupper, std::tolower
#include <cerrno>
#include <chrono>
#include <cstdlib> // ::atoi, ::strtoul
#include <fcntl.h>
#include <limits>
#include <netdb.h> // ::getaddrinfo, ::freeaddrinfo
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h> // ::close, ::recv, ::send

namespace bx {

namespace {

using Clock = std::chrono::steady_clock;

// Milliseconds until the overall deadline, 0 once it has been spent. All of
// resolve, connect, send and receive share the one deadline: http_get is
// bounded by timeout_ms as a whole, not per phase.
int remaining_ms(Clock::time_point deadline) {
    const auto now = Clock::now();
    if (now >= deadline) return 0;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    if (ms > std::numeric_limits<int>::max()) return std::numeric_limits<int>::max();
    return static_cast<int>(ms);
}

bool send_all(int fd, const std::string& data, Clock::time_point deadline) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        pollfd poll_set {};
        poll_set.fd = fd;
        poll_set.events = POLLOUT;
        const int ready = ::poll(&poll_set, 1, remaining_ms(deadline));
        if (ready < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (ready == 0) return false; // deadline spent
        const ssize_t n = ::send(fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return false;
        }
        offset += static_cast<std::size_t>(n);
    }
    return true;
}

// The head runs from the start to the first "\r\n\r\n" (bare "\n\n" accepted
// for hand-rolled servers); body_start is the first body byte, npos while the
// head is still incomplete.
void locate_head(const std::string& raw, std::size_t& body_start) {
    const std::size_t crlf = raw.find("\r\n\r\n");
    if (crlf != std::string::npos) {
        body_start = crlf + 4;
        return;
    }
    const std::size_t lf = raw.find("\n\n");
    body_start = lf != std::string::npos ? lf + 2 : std::string::npos;
}

// Content-Length from a completed head, or npos when absent/not yet known.
std::size_t content_length(const std::string& raw) {
    std::size_t body_start = std::string::npos;
    locate_head(raw, body_start);
    if (body_start == std::string::npos) return std::string::npos;

    std::size_t line_end = std::string::npos;
    for (std::size_t pos = 0; (line_end = raw.find('\n', pos)) < body_start; pos = line_end + 1) {
        std::string line = raw.substr(pos, line_end - pos);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = line.substr(0, colon);
        for (char& c : name) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        if (name != "CONTENT-LENGTH") continue;
        std::size_t value = colon + 1;
        while (value < line.size() && (line[value] == ' ' || line[value] == '\t')) ++value;
        if (value >= line.size() || line.find_first_not_of("0123456789", value) != std::string::npos) {
            return std::string::npos;
        }
        return static_cast<std::size_t>(::strtoul(line.c_str() + value, nullptr, 10));
    }
    return std::string::npos;
}

// Whether the body already announced by a completed head is fully in hand.
bool body_complete(const std::string& raw) {
    std::size_t body_start = std::string::npos;
    locate_head(raw, body_start);
    if (body_start == std::string::npos) return false; // head still incomplete
    const std::size_t length = content_length(raw);
    if (length == std::string::npos) return false; // read to EOF instead
    return raw.size() >= body_start + length;
}

} // namespace

HttpResult http_get(const std::string& host, const std::string& path, int port,
                    int timeout_ms, std::string& body) {
    body.clear();
    if (timeout_ms <= 0) return HttpResult::timeout;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);

    // esp-miner devices report both an ipv4 and an ipv6 address, so resolve
    // without pinning a family; no TLS anywhere, plain SOCK_STREAM http only.
    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* list = nullptr;
    const std::string service = std::to_string(port);
    // Resolution runs on the caller's budget: on a slow resolver the whole
    // call still stays inside timeout_ms.
    if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &list) != 0 || list == nullptr) {
        return HttpResult::dns_failed; // EAI_NONAME, EAI_NODATA, EAI_AGAIN, ...
    }

    // Connect to the first address that answers, all tries inside the deadline.
    int fd = -1;
    bool timed_out = false;
    for (const addrinfo* ai = list; ai != nullptr && fd < 0; ai = ai->ai_next) {
        const int candidate = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (candidate < 0) continue;
        // Non-blocking, so the poll below can carry the connect deadline.
        const int flags = ::fcntl(candidate, F_GETFL, 0);
        if (flags >= 0) (void)::fcntl(candidate, F_SETFL, flags | O_NONBLOCK);

        if (::connect(candidate, ai->ai_addr, ai->ai_addrlen) == 0) {
            fd = candidate;
            break;
        }
        if (errno != EINPROGRESS) {
            ::close(candidate);
            continue;
        }
        pollfd poll_set {};
        poll_set.fd = candidate;
        poll_set.events = POLLOUT;
        const int ready = ::poll(&poll_set, 1, remaining_ms(deadline));
        int connect_error = 0;
        socklen_t error_len = sizeof(connect_error);
        (void)::getsockopt(candidate, SOL_SOCKET, SO_ERROR, &connect_error, &error_len);
        if (ready == 1 && connect_error == 0) {
            fd = candidate;
            break;
        }
        if (ready == 0) timed_out = true; // the deadline is spent; try nothing further
        ::close(candidate);
        if (timed_out) break;
    }
    ::freeaddrinfo(list);
    if (fd < 0) {
        // One shared budget: a spent deadline is a timeout no matter which
        // address was being tried. Every other connect failure — refused
        // today, and an unreachable or filtered host too, which has no
        // closer bucket here — reads as refused.
        return timed_out ? HttpResult::timeout : HttpResult::refused;
    }

    const std::string request = "GET " + path + " HTTP/1.0\r\n"
                                "Host: " + host + ":" + service + "\r\n"
                                "User-Agent: bitaxezero\r\n"
                                "\r\n";
    if (! send_all(fd, request, deadline)) {
        const int spent = remaining_ms(deadline);
        ::close(fd);
        return spent == 0 ? HttpResult::timeout : HttpResult::refused;
    }

    // Read until the announced body is complete or the peer closes; HTTP/1.0
    // semantics, so no chunked encoding is handled (esp-miner never sends it).
    std::string raw;
    bool read_timed_out = false;
    bool protocol_error = false;
    while (! body_complete(raw)) {
        pollfd poll_set {};
        poll_set.fd = fd;
        poll_set.events = POLLIN;
        const int ready = ::poll(&poll_set, 1, remaining_ms(deadline));
        if (ready == 0) {
            read_timed_out = true;
            break;
        }
        if (ready < 0) {
            if (errno == EINTR) continue;
            protocol_error = true;
            break;
        }
        char chunk[4096];
        const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            protocol_error = true;
            break;
        }
        if (n == 0) break; // peer closed after the tail of the body
        raw.append(chunk, static_cast<std::size_t>(n));
    }
    ::close(fd);

    if (read_timed_out) return HttpResult::timeout;
    if (protocol_error) return HttpResult::protocol_error;

    std::size_t body_start = std::string::npos;
    locate_head(raw, body_start);
    if (body_start == std::string::npos) return HttpResult::protocol_error;

    const std::string status_line = raw.substr(0, raw.find('\n'));
    if (status_line.rfind("HTTP/1.", 0) != 0) return HttpResult::protocol_error;
    // Status code sits between the protocol and the reason: "HTTP/1.0 200 OK".
    const std::string code_text =
        status_line.size() >= 12 ? status_line.substr(9, 3) : std::string();
    if (code_text.size() != 3 ||
        code_text.find_first_not_of("0123456789") != std::string::npos) {
        return HttpResult::protocol_error;
    }
    if (::atoi(code_text.c_str()) != 200) return HttpResult::protocol_error;

    // Only status 200 is ok; chunked would have been silently misread above,
    // so refuse it rather than return a mangled body.
    std::size_t line_end = std::string::npos;
    for (std::size_t pos = 0; (line_end = raw.find('\n', pos)) < body_start; pos = line_end + 1) {
        std::string line = raw.substr(pos, line_end - pos);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        for (char& c : line) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (line.rfind("transfer-encoding:", 0) == 0) return HttpResult::protocol_error;
    }

    body = raw.substr(body_start);
    const std::size_t length = content_length(raw);
    if (length != std::string::npos && body.size() > length) {
        body.resize(length); // tolerate trailers or a pipelined next response
    }
    return HttpResult::ok;
}

} // namespace bx
