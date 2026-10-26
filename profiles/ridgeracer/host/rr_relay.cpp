#include "rr_relay.hpp"

#include <cstdlib>
#include <atomic>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#else
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <random>
#endif

namespace rr::net {
namespace {

using Id = std::array<std::uint8_t, 6>;

struct UrlParts { bool tls = false; std::string host; int port = 0; std::string path; bool ok = false; };
UrlParts split_url(const std::string &url) {
    UrlParts u;
    std::string rest;
    if (url.rfind("wss://", 0) == 0) { u.tls = true; rest = url.substr(6); }
    else if (url.rfind("ws://", 0) == 0) { rest = url.substr(5); }
    else return u;
    const auto slash = rest.find('/');
    std::string hostport = rest.substr(0, slash);
    u.path = slash == std::string::npos ? "/" : rest.substr(slash);
    const auto colon = hostport.rfind(':');
    if (colon != std::string::npos) { u.host = hostport.substr(0, colon); u.port = std::atoi(hostport.c_str() + colon + 1); }
    else { u.host = hostport; u.port = u.tls ? 443 : 80; }
    u.ok = !u.host.empty() && u.port > 0;
    return u;
}

#if defined(__EMSCRIPTEN__)

// The page's WebSocket lives on the engine's own thread; its messages are queued by the browser and picked up when the
// engine passes through the event loop (once a frame, see rr_js_yield in rr_present_web.cpp).
EM_JS(int, rr_ws_open, (const char *url), {
    try {
        var R = Module.rrRelay = Module.rrRelay || {};
        if (R.ws) { try { R.ws.close(); } catch (e) {} }
        R.queue = []; R.state = 1; R.error = '';
        var ws = new WebSocket(UTF8ToString(url));
        ws.binaryType = 'arraybuffer';
        ws.onopen = function() { if (R.ws === ws) R.state = 2; };
        ws.onmessage = function(e) { if (R.ws === ws && typeof e.data !== 'string' && R.queue.length < 4096) R.queue.push(new Uint8Array(e.data)); };
        ws.onerror = function() { if (R.ws === ws) R.error = 'connection error'; };
        ws.onclose = function(e) { if (R.ws === ws) { R.state = 3; if (!R.error) R.error = 'closed (' + e.code + ')'; } };
        R.ws = ws;
        return 1;
    } catch (e) {
        return 0;
    }
});
EM_JS(int, rr_ws_state, (), { var R = Module.rrRelay; return R ? R.state : 0; });   // 1 connecting, 2 open, 3 closed
EM_JS(void, rr_ws_send, (const void *data, int len), {
    var R = Module.rrRelay;
    if (R && R.ws && R.state === 2) { try { R.ws.send(HEAPU8.slice(data, data + len)); } catch (e) {} }
});
EM_JS(int, rr_ws_recv, (void *buf, int cap), {
    var R = Module.rrRelay;
    if (!R || !R.queue || R.queue.length === 0) return -1;
    var m = R.queue.shift();
    var n = Math.min(m.length, cap);
    HEAPU8.set(m.subarray(0, n), buf);
    return n;
});

class WebRelay final : public Relay {
public:
    bool open(const std::string &url) override { return rr_ws_open(url.c_str()) != 0; }
    bool ready() const override { return rr_ws_state() == 2; }
    bool failed() const override { return rr_ws_state() == 3; }
    void send(const Id &dst, const std::uint8_t *data, std::size_t len) override {
        if (len > 1500) return;
        std::uint8_t b[1506];
        std::memcpy(b, dst.data(), 6);
        std::memcpy(b + 6, data, len);
        rr_ws_send(b, static_cast<int>(len + 6));
    }
    bool recv(Id &src, std::vector<std::uint8_t> &data) override {
        std::uint8_t b[1600];
        for (;;) {
            const int n = rr_ws_recv(b, static_cast<int>(sizeof b));
            if (n < 0) return false;
            if (n < 6) continue;
            std::memcpy(src.data(), b, 6);
            data.assign(b + 6, b + n);
            return true;
        }
    }
    std::string describe() const override {
        const int s = rr_ws_state();
        return s == 2 ? "connected" : s == 1 ? "connecting" : s == 3 ? "closed" : "not started";
    }
};

#else

// Shared part of the thread-based clients: a reader thread fills `in_`, sends happen on the caller's thread.
class ThreadRelay : public Relay {
public:
    // (each final class stops the thread in its own destructor, while close_transport() still exists)
    bool ready() const override { return state_.load() == 2; }
    bool failed() const override { return state_.load() == 3; }
    bool recv(Id &src, std::vector<std::uint8_t> &data) override {
        std::lock_guard<std::mutex> lock(m_);
        if (in_.empty()) return false;
        std::vector<std::uint8_t> &f = in_.front();
        std::memcpy(src.data(), f.data(), 6);
        data.assign(f.begin() + 6, f.end());
        in_.pop_front();
        return true;
    }
    std::string describe() const override {
        const int s = state_.load();
        std::lock_guard<std::mutex> lock(m_);
        return s == 2 ? "connected" : s == 1 ? "connecting" : s == 3 ? "closed: " + error_ : "not started";
    }

protected:
    void arrived(const std::uint8_t *p, std::size_t n) {
        if (n < 6) return;
        std::lock_guard<std::mutex> lock(m_);
        if (in_.size() < 4096) in_.emplace_back(p, p + n);
    }
    void fail(const std::string &why) {
        { std::lock_guard<std::mutex> lock(m_); if (error_.empty()) error_ = why; }
        state_ = 3;
    }
    void start_thread(std::function<void()> fn) {
        stop_thread();
        { std::lock_guard<std::mutex> lock(m_); error_.clear(); in_.clear(); }
        state_ = 1;
        thread_ = std::thread(std::move(fn));
    }
    void stop_thread() {
        quit_ = true;
        close_transport();
        if (thread_.joinable()) thread_.join();
        quit_ = false;
    }
    virtual void close_transport() = 0;

    std::atomic<int> state_{0};
    std::atomic<bool> quit_{false};
    mutable std::mutex m_;
    std::deque<std::vector<std::uint8_t>> in_;
    std::string error_;
    std::thread thread_;
};

#if defined(_WIN32)

// WinHTTP's WebSocket client (Windows 8 and later): handles wss:// (TLS) and proxies like the room-list requests.
class WinRelay final : public ThreadRelay {
public:
    ~WinRelay() override { stop_thread(); }
    bool open(const std::string &url) override {
        const UrlParts u = split_url(url);
        if (!u.ok) return false;
        start_thread([this, u] { run(u); });
        return true;
    }
    void send(const Id &dst, const std::uint8_t *data, std::size_t len) override {
        if (!ready() || len > 1500) return;
        std::uint8_t b[1506];
        std::memcpy(b, dst.data(), 6);
        std::memcpy(b + 6, data, len);
        std::lock_guard<std::mutex> lock(send_m_);
        if (ws_ != nullptr) WinHttpWebSocketSend(ws_, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, b, static_cast<DWORD>(len + 6));
    }

private:
    static std::wstring widen(const std::string &s) { return std::wstring(s.begin(), s.end()); }
    void run(UrlParts u) {
        HINTERNET session = WinHttpOpen(L"RidgeRacerPC", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (session == nullptr) session = WinHttpOpen(L"RidgeRacerPC", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        HINTERNET conn = session ? WinHttpConnect(session, widen(u.host).c_str(), static_cast<INTERNET_PORT>(u.port), 0) : nullptr;
        HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", widen(u.path).c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, u.tls ? WINHTTP_FLAG_SECURE : 0) : nullptr;
        bool ok = req != nullptr && WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
                  WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr);
        DWORD status = 0, sz = sizeof status;
        if (ok) WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
        HINTERNET ws = (ok && status == 101) ? WinHttpWebSocketCompleteUpgrade(req, 0) : nullptr;
        if (req) WinHttpCloseHandle(req);
        if (ws == nullptr) {
            fail(ok ? "upgrade refused (HTTP " + std::to_string(status) + ")" : "could not connect (error " + std::to_string(GetLastError()) + ")");
            if (conn) WinHttpCloseHandle(conn);
            if (session) WinHttpCloseHandle(session);
            return;
        }
        { std::lock_guard<std::mutex> lock(send_m_); ws_ = ws; }
        state_ = 2;
        std::vector<std::uint8_t> msg;
        std::uint8_t buf[2048];
        while (!quit_) {
            DWORD got = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
            const DWORD r = WinHttpWebSocketReceive(ws, buf, sizeof buf, &got, &type);
            if (r != NO_ERROR) { fail("connection lost (error " + std::to_string(r) + ")"); break; }
            if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) { fail("closed by the service"); break; }
            if (type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) { msg.insert(msg.end(), buf, buf + got); continue; }
            msg.insert(msg.end(), buf, buf + got);
            if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) arrived(msg.data(), msg.size());
            msg.clear();
        }
        { std::lock_guard<std::mutex> lock(send_m_); ws_ = nullptr; }
        WinHttpWebSocketClose(ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        WinHttpCloseHandle(ws);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
    }
    void close_transport() override {
        std::lock_guard<std::mutex> lock(send_m_);
        if (ws_ != nullptr) WinHttpWebSocketShutdown(ws_, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);   // wakes the reader
    }
    std::mutex send_m_;
    HINTERNET ws_ = nullptr;
};

#else

// Plain ws:// client over a TCP socket (RFC 6455, client side only).  No TLS: used for local tests against
// `wrangler dev`; a wss:// service is reported as unsupported on Linux/macOS for now.
class PosixRelay final : public ThreadRelay {
public:
    ~PosixRelay() override { stop_thread(); }
    bool open(const std::string &url) override {
        const UrlParts u = split_url(url);
        if (!u.ok || u.tls) return false;
        start_thread([this, u] { run(u); });
        return true;
    }
    void send(const Id &dst, const std::uint8_t *data, std::size_t len) override {
        if (!ready() || len > 1500) return;
        std::uint8_t payload[1506];
        std::memcpy(payload, dst.data(), 6);
        std::memcpy(payload + 6, data, len);
        send_frame(0x2, payload, len + 6);
    }

private:
    void send_frame(int opcode, const std::uint8_t *p, std::size_t n) {
        std::uint8_t f[1600];
        if (n > 1500) return;
        std::size_t h = 0;
        f[h++] = static_cast<std::uint8_t>(0x80 | opcode);
        if (n < 126) f[h++] = static_cast<std::uint8_t>(0x80 | n);
        else { f[h++] = 0x80 | 126; f[h++] = static_cast<std::uint8_t>(n >> 8); f[h++] = static_cast<std::uint8_t>(n & 0xff); }
        std::uint8_t mask[4];
        { std::lock_guard<std::mutex> lock(send_m_); for (auto &b : mask) b = static_cast<std::uint8_t>(rng_()); }
        std::memcpy(f + h, mask, 4); h += 4;
        for (std::size_t i = 0; i < n; ++i) f[h + i] = p[i] ^ mask[i & 3];
        std::lock_guard<std::mutex> lock(send_m_);
        if (fd_ >= 0) ::send(fd_, f, h + n, MSG_NOSIGNAL);
    }
    bool read_exact(std::uint8_t *p, std::size_t n) {
        while (n > 0) {
            const auto r = ::recv(fd_, p, n, 0);
            if (r <= 0) return false;
            p += r; n -= static_cast<std::size_t>(r);
        }
        return true;
    }
    void run(UrlParts u) {
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(u.host.c_str(), std::to_string(u.port).c_str(), &hints, &res) != 0 || res == nullptr) { fail("could not resolve " + u.host); return; }
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        const bool conn = fd >= 0 && ::connect(fd, res->ai_addr, res->ai_addrlen) == 0;
        freeaddrinfo(res);
        if (!conn) { if (fd >= 0) ::close(fd); fail("could not connect"); return; }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        { std::lock_guard<std::mutex> lock(send_m_); fd_ = fd; }
        const std::string req = "GET " + u.path + " HTTP/1.1\r\nHost: " + u.host + ":" + std::to_string(u.port) +
                                "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
        ::send(fd, req.data(), req.size(), MSG_NOSIGNAL);
        std::string head;
        char c;
        while (head.size() < 8192 && head.find("\r\n\r\n") == std::string::npos) { if (::recv(fd, &c, 1, 0) != 1) break; head += c; }
        if (head.rfind("HTTP/1.1 101", 0) != 0) { fail("upgrade refused: " + head.substr(0, head.find('\r'))); close_fd(); return; }
        state_ = 2;
        std::vector<std::uint8_t> msg;
        while (!quit_) {
            std::uint8_t h2[2];
            if (!read_exact(h2, 2)) { fail("connection lost"); break; }
            const int opcode = h2[0] & 0x0f;
            const bool fin = (h2[0] & 0x80) != 0;
            std::uint64_t n = h2[1] & 0x7f;
            if (n == 126) { std::uint8_t e[2]; if (!read_exact(e, 2)) break; n = (e[0] << 8) | e[1]; }
            else if (n == 127) { std::uint8_t e[8]; if (!read_exact(e, 8)) break; n = 0; for (int i = 0; i < 8; ++i) n = (n << 8) | e[i]; }
            if (n > (1u << 20)) { fail("frame too big"); break; }
            std::vector<std::uint8_t> p(static_cast<std::size_t>(n));
            if (n != 0 && !read_exact(p.data(), p.size())) { fail("connection lost"); break; }
            if (opcode == 0x8) { fail("closed by the service"); break; }
            if (opcode == 0x9) { send_frame(0xA, p.data(), p.size()); continue; }
            if (opcode == 0xA) continue;
            msg.insert(msg.end(), p.begin(), p.end());
            if (fin) { if (opcode == 0x2 || opcode == 0x0) arrived(msg.data(), msg.size()); msg.clear(); }
        }
        close_fd();
    }
    void close_fd() {
        std::lock_guard<std::mutex> lock(send_m_);
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }
    void close_transport() override {
        std::lock_guard<std::mutex> lock(send_m_);
        if (fd_ >= 0) ::shutdown(fd_, SHUT_RDWR);   // wakes the reader
    }
    std::mutex send_m_;
    int fd_ = -1;
    std::mt19937 rng_{std::random_device{}()};
};

#endif
#endif

} // namespace

std::unique_ptr<Relay> make_relay(const std::string &url) {
    const UrlParts u = split_url(url);
    if (!u.ok) return nullptr;
#if defined(__EMSCRIPTEN__)
    return std::make_unique<WebRelay>();
#elif defined(_WIN32)
    return std::make_unique<WinRelay>();
#else
    if (u.tls) return nullptr;
    return std::make_unique<PosixRelay>();
#endif
}

} // namespace rr::net
