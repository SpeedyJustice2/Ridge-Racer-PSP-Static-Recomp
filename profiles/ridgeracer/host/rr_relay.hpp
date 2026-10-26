// Relay link for online play: a WebSocket to the room service (online/worker, "/relay"), which forwards the game's
// datagrams between players by their 6-byte id.  Used for every player in the browser build (browsers cannot send
// UDP), and by the PC build for browser players and for PC players whose routers block direct connections.
//
// Framing (binary WebSocket messages): to the service [6-byte destination id][datagram], from the service
// [6-byte sender id][datagram].  The service fills in the sender id itself, so it cannot be forged.
//
// Implementations: the browser's WebSocket (rr_relay.cpp, __EMSCRIPTEN__), WinHTTP on Windows, and a plain ws://
// client on Linux/macOS (no TLS: enough for local tests; wss:// is reported as unsupported there).
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rr::net {

class Relay {
public:
    virtual ~Relay() = default;
    // Starts connecting (returns at once).  `url` is ws:// or wss://, ending in "/relay?mac=<our id>".
    virtual bool open(const std::string &url) = 0;
    virtual bool ready() const = 0;    // connected
    virtual bool failed() const = 0;   // gave up / closed (open() again to retry)
    virtual void send(const std::array<std::uint8_t, 6> &dst, const std::uint8_t *data, std::size_t len) = 0;
    // Next datagram that arrived, with its sender's id; false when none is waiting.
    virtual bool recv(std::array<std::uint8_t, 6> &src, std::vector<std::uint8_t> &data) = 0;
    virtual std::string describe() const = 0;   // for the log ("connected", "connecting", an error)
};

// nullptr when this build has no WebSocket client for that kind of URL.
std::unique_ptr<Relay> make_relay(const std::string &url);

} // namespace rr::net
