// Minimal blocking HTTP(S) client for the online room-list service (a few tiny text requests).  Run it off the game's
// main thread.  Windows uses WinHTTP, Linux/macOS use the system's curl, Android has no implementation yet (status 0).
#pragma once
#include <string>

namespace rr::net {

struct HttpResult {
    int status{0};         // 0 = could not connect / no client available
    std::string body;
};

HttpResult http_request(const std::string &method, const std::string &url, int timeout_ms);

} // namespace rr::net
