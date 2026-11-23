// Minimal, timeout-bounded HTTP/1.0 GET for esp-miner: POSIX sockets only,
// plain http (esp-miner serves no TLS), IPv4 and IPv6, no chunked encoding.
#pragma once

#include <string>

namespace bx {

// ok only on HTTP 200; timeout when resolve+connect+receive together exceed
// timeout_ms; refused when the server actively rejects the connection;
// dns_failed when the host cannot be resolved; protocol_error for a malformed
// response or a non-200 status.
enum class HttpResult {
    ok,
    timeout,
    refused,
    dns_failed,
    protocol_error,
};

// Fills body with the raw response body (headers stripped). On any non-ok
// result, body is left empty. Hard ceiling 2000 ms is passed by callers; this
// honors whatever timeout_ms it receives.
HttpResult http_get(const std::string& host, const std::string& path, int port,
                    int timeout_ms, std::string& body);

} // namespace bx
