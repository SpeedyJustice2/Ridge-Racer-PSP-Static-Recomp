#include "rr_net.hpp"
#include "rr_version.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <random>
#include <thread>

#include "rr_http.hpp"
#include "rr_online_config.hpp"
#include "rr_relay.hpp"

// The browser build has no UDP: it compiles the same code (Emscripten has the socket headers) but never opens a socket,
// and reaches every other player through the relay (RR_NET_WEB).
#if defined(__EMSCRIPTEN__)
#define RR_NET_SOCKETS 1
#define RR_NET_WEB 1
#elif defined(_WIN32)
#define RR_NET_SOCKETS 1
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#define RR_NET_SOCKETS 1
#endif
#ifndef RR_NET_WEB
#define RR_NET_WEB 0
#endif
#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace rr::net {
namespace {

constexpr int kFirstPort = 47100;
constexpr int kPortCount = 32;
constexpr std::size_t kHeader = 22;
constexpr std::size_t kMaxPayload = 1400;

enum PacketType : std::uint8_t {
    kBeacon = 1, kScanReq = 2, kPdp = 3, kHello = 4, kRequest = 5, kAccept = 6, kComplete = 7, kDeny = 8, kCancel = 9,
    kBye = 10, kPing = 11, kPunch = 12, kPong = 13, kPdpT = 14,   // kPdpT: a datagram with the sender's clock (jitter buffer)
    kMembers = 15,   // host -> joiners: the joiners in the order they joined (6 bytes each), like the PSP library's member list
    kFwd = 16        // a whole packet relayed by the host between two joiners (online: joiners only reach the host)
};

}   // namespace

static void nlog_hook(const std::string &s);
namespace { std::string mac_hex(const Mac &m); }

struct Node::Packet {
    std::uint8_t type{};
    unsigned port{};
    Mac src{}, dst{};
    std::vector<std::uint8_t> payload;
};

struct Node::Matching {
    int id{};
    int mode{};
    int max{};
    unsigned port{};
    std::uint32_t hello_ms{1000}, keepalive_ms{5000};
    bool started{false};
    std::vector<std::uint8_t> opt;
    std::vector<std::uint8_t> adv_opt;         // the last non-empty hello message: the room description shown in the server
                                               // browser, also while a race is on (the game empties `opt` then)
    std::map<Mac, std::uint64_t> members;      // established peers -> last time heard
    std::set<Mac> requesters;                  // asked to join us (host side)
    std::set<Mac> pending;                     // we asked to join them (client side)
    std::uint64_t last_hello{0}, last_ping{0}, last_rtt_log{0};
    std::map<Mac, std::uint32_t> rtt_ms;      // latest round trip per peer (online diagnostics)
    // Handshake resends (packets get lost on the internet): the request until the host answers, the accept until the
    // joiner confirms with "complete".  Receivers treat repeats as no-ops (or answer them again).
    std::map<Mac, std::vector<std::uint8_t>> request_opt;   // client: what we asked with (pending targets)
    std::map<Mac, std::vector<std::uint8_t>> accept_opt;    // host: accepted, not yet confirmed
    std::set<Mac> confirmed;                                // host: joiners whose "complete" arrived
    std::uint64_t last_resend{0};
    // More than two players: on the host the joiners in the order they were accepted (sent to every joiner as kMembers);
    // on a joiner that list as received from the host (it includes ourselves).
    std::vector<Mac> order;
    std::uint64_t last_members{0};
    bool members_dirty{false};
};

Node::Node() { t0_ = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
Node::~Node() { stop(); }

std::uint64_t Node::now_ms() const {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()) - t0_ + 1;
}

#if RR_NET_SOCKETS

bool Node::start(int port) {
    if (RR_NET_WEB) return false;   // (same-PC test mode needs UDP)
    if (running()) return true;
#ifdef _WIN32
    static bool wsa = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }();
    if (!wsa) return false;
#endif
    for (int i = 0; i < kPortCount; ++i) {
        const int try_port = port != 0 ? port : kFirstPort + i;
        const auto s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
        if (s == INVALID_SOCKET) return false;
#else
        if (s < 0) return false;
#endif
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<std::uint16_t>(try_port));
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == 0) {
#ifdef _WIN32
            u_long nb = 1;
            ioctlsocket(s, FIONBIO, &nb);
            // a datagram sent to a closed local port makes the next receive fail with a reset; ignore that
            DWORD bytes = 0; BOOL off = FALSE;
            WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12), &off, sizeof off, nullptr, 0, &bytes, nullptr, nullptr);
#else
            fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
            socket_ = static_cast<std::intptr_t>(s);
            port_ = try_port;
            mac_ = Mac{0x02, 0x52, 0x52, 0x00, static_cast<std::uint8_t>((try_port >> 8) & 0xff), static_cast<std::uint8_t>(try_port & 0xff)};
            sim_setup();
            return true;
        }
#ifdef _WIN32
        closesocket(s);
#else
        ::close(s);
#endif
        if (port != 0) return false;
    }
    return false;
}

void Node::stop() {
    if (socket_ == -1) return;
    online_.reset();   // (closes the relay link too)
    if (RR_NET_WEB) { socket_ = -1; return; }   // (no real socket in the browser)
#ifdef _WIN32
    closesocket(static_cast<SOCKET>(socket_));
#else
    ::close(static_cast<int>(socket_));
#endif
    socket_ = -1;
}

namespace {
// RR_NET_TRACE=1: one log line per packet sent / handled (not datagrams of the race itself), to follow a handshake.
bool net_trace() { static const bool on = std::getenv("RR_NET_TRACE") != nullptr; return on; }
const char *ptype_name(int t) {
    static const char *n[] = {"?", "beacon", "scanreq", "pdp", "hello", "request", "accept", "complete", "deny", "cancel", "bye", "ping", "punch", "pong", "pdpT", "members", "fwd"};
    return t >= 0 && t <= 16 ? n[t] : "?";
}
}
void Node::send_packet(int type, const Mac &dst, unsigned port, const void *payload, std::size_t len) {
    if (socket_ == -1) return;
    if (net_trace() && type != kPdp && type != kPdpT && type != kPing && type != kPong && type != kHello && type != kBeacon)
        nlog_hook(std::string("TRACE send ") + ptype_name(type) + " to " + mac_hex(dst) + " port " + std::to_string(port) + " len " + std::to_string(len));
    if (len > kMaxPayload) len = kMaxPayload;
    std::uint8_t buf[kHeader + kMaxPayload];
    std::memcpy(buf, "RRN1", 4);
    buf[4] = static_cast<std::uint8_t>(type);
    buf[5] = 0;
    buf[6] = static_cast<std::uint8_t>(port & 0xff);
    buf[7] = static_cast<std::uint8_t>((port >> 8) & 0xff);
    std::memcpy(buf + 8, mac_.data(), 6);
    std::memcpy(buf + 14, dst.data(), 6);
    buf[20] = static_cast<std::uint8_t>(len & 0xff);
    buf[21] = static_cast<std::uint8_t>((len >> 8) & 0xff);
    if (len != 0) std::memcpy(buf + kHeader, payload, len);
    if (online_) { online_send(buf, kHeader + len, dst); return; }
    auto send_to = [&](int udp_port) {
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(static_cast<std::uint16_t>(udp_port));
        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::sendto(static_cast<decltype(::socket(0, 0, 0))>(socket_), reinterpret_cast<const char *>(buf), static_cast<int>(kHeader + len), 0,
                 reinterpret_cast<sockaddr *>(&to), sizeof to);
    };
    const auto known = dst == kBroadcast ? peer_port_.end() : peer_port_.find(dst);
    if (known != peer_port_.end()) { send_to(known->second); return; }
    for (int i = 0; i < kPortCount; ++i)
        if (kFirstPort + i != port_) send_to(kFirstPort + i);
}

bool Node::parse_packet(const std::uint8_t *buf, std::size_t got, Packet &p, std::size_t &total) {
    if (got < kHeader || std::memcmp(buf, "RRN1", 4) != 0) return false;
    p.type = buf[4];
    p.port = static_cast<unsigned>(buf[6] | (buf[7] << 8));
    std::memcpy(p.src.data(), buf + 8, 6);
    std::memcpy(p.dst.data(), buf + 14, 6);
    const std::size_t len = static_cast<std::size_t>(buf[20] | (buf[21] << 8));
    if (kHeader + len > got) return false;
    p.payload.assign(buf + kHeader, buf + kHeader + len);
    total = kHeader + len;
    return true;
}

// ---- more than two players, online: joiners reach only the host, so the host passes on what they send each other ----
bool Node::host_member(const Mac &mac) const {
    for (const auto &m : matches_) if ((m->mode == kModeHost || m->mode == kModeP2P) && m->started && m->members.count(mac) != 0u) return true;
    return false;
}
const Mac *Node::joined_host() const {   // our host while we are a joiner in someone's room
    for (const auto &m : matches_) if (m->mode == kModeClient && m->started && !m->members.empty()) return &m->members.begin()->first;
    return nullptr;
}
// Host: send a joiner's packet on to the other joiners (all of them for a broadcast, else the one it is addressed to).
void Node::forward_between_joiners(const Mac &from, const Mac &dst, const std::uint8_t *raw, std::size_t len) {
    for (const auto &m : matches_) {
        if (!(m->mode == kModeHost || m->mode == kModeP2P) || !m->started || m->members.count(from) == 0u) continue;
        for (const auto &[peer, heard] : m->members) {
            (void)heard;
            if (peer == from || (dst != kBroadcast && peer != dst)) continue;
            send_packet(kFwd, peer, 0, raw, len);
        }
        return;
    }
}
// A packet relayed by the host (kFwd): on the host from a joiner (pass it on / take it), on a joiner from its host.
void Node::handle_forwarded(const Packet &outer) {
    Packet in;
    std::size_t total = 0;
    if (!parse_packet(outer.payload.data(), outer.payload.size(), in, total) || in.src == mac_) return;
    if (host_member(outer.src)) {
        if (in.src != outer.src) return;   // (a joiner can only send as itself)
        if (in.dst != mac_) forward_between_joiners(in.src, in.dst, outer.payload.data(), total);
        if (in.dst == mac_ || in.dst == kBroadcast) handle(in);
        return;
    }
    const Mac *h = joined_host();
    if (h == nullptr || *h != outer.src) return;   // only our own host relays to us
    if (in.dst == mac_ || in.dst == kBroadcast) handle(in);
}

void Node::receive(const std::uint8_t *buf, std::size_t got, const void *from_addr) {
    static const sockaddr_in kNoAddr{};
    const sockaddr_in &from = from_addr != nullptr ? *static_cast<const sockaddr_in *>(from_addr) : kNoAddr;
    if (online_ && relay_src_ == nullptr && got >= 20 && buf[4] == 0x21 && buf[5] == 0x12 && buf[6] == 0xA4 && buf[7] == 0x42) { online_stun(buf, got); return; }
    Packet p;
    std::size_t total = 0;
    if (!parse_packet(buf, got, p, total)) return;
    if (p.src == mac_) return;
    if (p.dst != mac_ && p.dst != kBroadcast) return;
    if (net_trace() && p.type != kPdp && p.type != kPdpT && p.type != kPing && p.type != kPong && p.type != kHello && p.type != kBeacon)
        nlog_hook(std::string("TRACE got ") + ptype_name(p.type) + " from " + mac_hex(p.src) + (relay_src_ != nullptr ? " (relay)" : "") + " port " + std::to_string(p.port));
    if (relay_src_ != nullptr) { if (p.src != *relay_src_ || !online_accept_relay(p.src)) return; }
    else if (online_) { if (!online_accept(p.src, &from)) return; }
    else peer_port_[p.src] = static_cast<int>(ntohs(from.sin_port));
    if (online_) {
        if (p.type == kFwd) { handle_forwarded(p); return; }
        // a joiner's broadcast game data also goes to the other joiners (they cannot reach each other directly)
        if ((p.type == kPdp || p.type == kPdpT) && p.dst == kBroadcast && host_member(p.src)) forward_between_joiners(p.src, kBroadcast, buf, total);
    }
    handle(p);
}

void Node::sim_setup() {
    auto num = [](const char *name, double def) { const char *e = std::getenv(name); return e != nullptr && e[0] != '\0' ? std::atof(e) : def; };
    sim_.delay_ms = std::max(0, static_cast<int>(num("RR_NET_SIM_DELAY", 0)));
    sim_.jitter_ms = std::max(0, static_cast<int>(num("RR_NET_SIM_JITTER", 0)));
    sim_.loss = std::clamp(num("RR_NET_SIM_LOSS", 0) / 100.0, 0.0, 1.0);
    sim_.dup = std::clamp(num("RR_NET_SIM_DUP", 0) / 100.0, 0.0, 1.0);
    if (const char *e = std::getenv("RR_NET_SIM_SPIKE"); e != nullptr && e[0] != '\0') {
        double every = 0, ms = 0, first = 0;
        if (std::sscanf(e, "%lf,%lf,%lf", &every, &ms, &first) >= 2 && every > 0 && ms > 0) {
            sim_.spike_every_ms = static_cast<int>(every * 1000.0); sim_.spike_ms = static_cast<int>(ms); sim_.spike_start_ms = static_cast<int>(std::max(0.0, first) * 1000.0);
        }
    }
    if (const char *e = std::getenv("RR_NET_SIM_SEED")) sim_.rng = static_cast<std::uint32_t>(std::strtoul(e, nullptr, 0)) | 1u;
    else sim_.rng ^= static_cast<std::uint32_t>(port_) * 2654435761u;
    sim_.on = sim_.delay_ms > 0 || sim_.jitter_ms > 0 || sim_.loss > 0 || sim_.dup > 0 || sim_.spike_ms > 0;
    if (sim_.on) {
        char b[220];
        std::snprintf(b, sizeof b, "network simulator ON (incoming packets): delay %d ms, jitter +-%d ms, loss %.1f%%, duplicates %.1f%%, spikes %d ms every %.1f s from %.0f s",
                      sim_.delay_ms, sim_.jitter_ms, sim_.loss * 100.0, sim_.dup * 100.0, sim_.spike_ms, sim_.spike_every_ms / 1000.0, sim_.spike_start_ms / 1000.0);
        nlog_hook(b);
    }
}

void Node::sim_arrived(const std::uint8_t *buf, std::size_t got, const void *from_addr) {
    auto rnd = [this] { sim_.rng ^= sim_.rng << 13; sim_.rng ^= sim_.rng >> 17; sim_.rng ^= sim_.rng << 5; return (sim_.rng & 0xffffffu) / 16777216.0; };
    const std::uint64_t now = now_ms();
    if (rnd() < sim_.loss) { ++sim_.dropped; return; }
    const int copies = rnd() < sim_.dup ? 2 : 1;
    if (copies == 2) ++sim_.duplicated;
    for (int c = 0; c < copies; ++c) {
        const double d = sim_.delay_ms + (sim_.jitter_ms > 0 ? (rnd() * 2.0 - 1.0) * sim_.jitter_ms : 0.0);
        std::uint64_t due = now + static_cast<std::uint64_t>(std::max(0.0, d)) + static_cast<std::uint64_t>(c);
        if (sim_.spike_every_ms > 0 && due >= static_cast<std::uint64_t>(sim_.spike_start_ms)) {   // the line stalls for spike_ms at the start of every period (from first_s on)
            const std::uint64_t pos = (due - static_cast<std::uint64_t>(sim_.spike_start_ms)) % static_cast<std::uint64_t>(sim_.spike_every_ms);
            if (pos < static_cast<std::uint64_t>(sim_.spike_ms)) { due += static_cast<std::uint64_t>(sim_.spike_ms) - pos; ++sim_.held_in_spike; }
        }
        due = std::max(due, sim_.last_due);   // jitter delays packets, but a real line keeps their order
        sim_.last_due = due;
        Held h{due, std::vector<std::uint8_t>(buf, buf + got), {}};
        std::memcpy(h.from.data(), from_addr, std::min(sizeof(sockaddr_in), h.from.size()));
        held_.push_back(std::move(h));
    }
}

void Node::sim_release(std::uint64_t now) {
    while (!held_.empty() && held_.front().due <= now) {
        Held h = std::move(held_.front());
        held_.pop_front();
        ++sim_.passed;
        sockaddr_in from{};
        std::memcpy(&from, h.from.data(), std::min(sizeof from, h.from.size()));
        receive(h.data.data(), h.data.size(), &from);
    }
    if (now - sim_.last_report >= 10000) {
        sim_.last_report = now;
        char b[200];
        std::snprintf(b, sizeof b, "network simulator: %llu delivered, %llu dropped, %llu duplicated, %llu held by spikes, %zu in flight",
                      static_cast<unsigned long long>(sim_.passed), static_cast<unsigned long long>(sim_.dropped), static_cast<unsigned long long>(sim_.duplicated),
                      static_cast<unsigned long long>(sim_.held_in_spike), held_.size());
        nlog_hook(b);
    }
}

void Node::poll() {
    if (socket_ == -1) return;
    for (int guard = 0; guard < 256 && !RR_NET_WEB; ++guard) {
        std::uint8_t buf[kHeader + kMaxPayload + 64];
        sockaddr_in from{};
#ifdef _WIN32
        int from_len = sizeof from;
#else
        socklen_t from_len = sizeof from;
#endif
        const auto got = ::recvfrom(static_cast<decltype(::socket(0, 0, 0))>(socket_), reinterpret_cast<char *>(buf), static_cast<int>(sizeof buf), 0,
                                    reinterpret_cast<sockaddr *>(&from), &from_len);
        if (got < 0) break;
        if (sim_.on) sim_arrived(buf, static_cast<std::size_t>(got), &from);
        else receive(buf, static_cast<std::size_t>(got), &from);
    }
    if (online_) online_relay_tick(now_ms());
    if (sim_.on) sim_release(now_ms());
    if (!pdp_.empty()) pdp_release(now_ms());
    const auto now = now_ms();
    if (online_) online_tick(now);
    if (connected_ && now - last_beacon_ >= 500) { last_beacon_ = now; beacon(); }
    if (scanning_ && now >= scan_until_) {
        scanning_ = false;
        scan_result_ = scan_found_;
        ctl_events_.push_back(kCtlScan);
    }
    for (auto &mp : matches_) {
        Matching &m = *mp;
        if (!m.started) continue;
        if ((m.mode == kModeHost || m.mode == kModeP2P) && now - m.last_hello >= m.hello_ms) {
            m.last_hello = now;
            send_packet(kHello, kBroadcast, m.port, m.opt.data(), m.opt.size());
        }
        if (m.mode == kModeHost || m.mode == kModeP2P) {   // the member list for joiners (more than two players)
            const auto before = m.order.size();
            m.order.erase(std::remove_if(m.order.begin(), m.order.end(), [&](const Mac &x) { return m.members.count(x) == 0u; }), m.order.end());
            for (const auto &[peer, heard] : m.members) { (void)heard; if (std::find(m.order.begin(), m.order.end(), peer) == m.order.end()) m.order.push_back(peer); }
            if (m.order.size() != before) m.members_dirty = true;
            if (!m.members.empty() && (m.members_dirty || now - m.last_members >= 1000)) {
                m.members_dirty = false;
                m.last_members = now;
                std::vector<std::uint8_t> list;
                for (const Mac &x : m.order) list.insert(list.end(), x.begin(), x.end());
                for (const auto &[peer, heard] : m.members) { (void)heard; send_packet(kMembers, peer, m.port, list.data(), list.size()); }
            }
        }
        if (now - m.last_resend >= 250 && (!m.request_opt.empty() || !m.accept_opt.empty())) {   // handshake resends
            m.last_resend = now;
            for (const auto &[peer, opt] : m.request_opt) if (m.pending.count(peer) != 0u) send_packet(kRequest, peer, m.port, opt.data(), opt.size());
            for (const auto &[peer, opt] : m.accept_opt) if (m.members.count(peer) != 0u) send_packet(kAccept, peer, m.port, opt.data(), opt.size());
        }
        if (now - m.last_ping >= 1000) {
            m.last_ping = now;
            for (const auto &[peer, heard] : m.members) {
                (void)heard;
                std::uint8_t ts[8];
                for (int i = 0; i < 8; ++i) ts[i] = static_cast<std::uint8_t>((now >> (8 * i)) & 0xff);
                send_packet(kPing, peer, m.port, ts, sizeof ts);
            }
        }
        if (online_ && now - m.last_rtt_log >= 5000 && !m.rtt_ms.empty()) {
            m.last_rtt_log = now;
            std::string line = "round trip to the other player(s):";
            for (const auto &[peer, ms] : m.rtt_ms) line += " " + std::to_string(ms) + " ms";
            nlog_hook(line);
        }
        for (auto it = m.members.begin(); it != m.members.end();) {
            if (now - it->second > m.keepalive_ms) {
                match_events_.push_back({m.id, kMatchTimeout, it->first, {}});
                m.accept_opt.erase(it->first); m.confirmed.erase(it->first);
                it = m.members.erase(it);
            } else ++it;
        }
    }
}

#else   // no sockets (the browser build): the node never starts

bool Node::start(int) { return false; }
void Node::stop() {}
void Node::send_packet(int, const Mac &, unsigned, const void *, std::size_t) {}
void Node::poll() {}

#endif

void Node::beacon() {
    std::uint8_t payload[12] = {};
    std::memcpy(payload, group_, 8);
    payload[8] = 11;
    send_packet(kBeacon, kBroadcast, 0, payload, sizeof payload);
}

void Node::handle(const Packet &p) {
    const auto now = now_ms();
    switch (p.type) {
    case kPunch:
        return;
    case kScanReq:
        if (connected_) beacon();
        return;
    case kBeacon: {
        if (p.payload.size() < 12) return;
        ScanEntry e;
        std::memcpy(e.name, p.payload.data(), 8);
        e.bssid = p.src;
        e.channel = p.payload[8];
        for (const auto &f : scan_found_) if (std::memcmp(f.name, e.name, 8) == 0) return;
        scan_found_.push_back(e);
        return;
    }
    case kPdp: {
        if (p.payload.size() < 2) return;
        const unsigned src_port = static_cast<unsigned>(p.payload[0] | (p.payload[1] << 8));
        for (auto &[id, pdp] : pdp_) {
            if (pdp.port != p.port) continue;
            if (pdp.queue.size() < 256) pdp.queue.push_back({{p.src, src_port}, std::vector<std::uint8_t>(p.payload.begin() + 2, p.payload.end())});
            break;
        }
        for (auto &mp : matches_) { auto it = mp->members.find(p.src); if (it != mp->members.end()) it->second = now; }
        return;
    }
    case kPdpT: {
        if (p.payload.size() < 6) return;
        std::uint32_t sent = 0;
        for (int i = 0; i < 4; ++i) sent |= static_cast<std::uint32_t>(p.payload[static_cast<std::size_t>(i)]) << (8 * i);
        const unsigned src_port = static_cast<unsigned>(p.payload[4] | (p.payload[5] << 8));
        for (auto &[id, pdp] : pdp_) {
            if (pdp.port != p.port) continue;
            if (pdp.held.size() < 256) {
                const std::uint64_t at = pdp_playout_time(p.src, sent, now);
                {   // age of this data when the game gets it: the wait above the fastest trip + half the round trip
                    Playout &pl = playout_[p.src];
                    double age = static_cast<double>(static_cast<std::int64_t>(at) - static_cast<std::int64_t>(sent) - pl.base);
                    age += one_way_ms(p.src);
                    age = std::clamp(age, 0.0, 1000.0);
                    pl.age_ema = pl.age_ema < 0.0 ? age : pl.age_ema + (age - pl.age_ema) * 0.05;
                    pl.age_at = now;
                }
                std::vector<std::uint8_t> data(p.payload.begin() + 6, p.payload.end());
                predict(data, p.src, sent, at);   // (as of when the game gets it)
                pdp.held.push_back({at, {{p.src, src_port}, std::move(data)}});
            }
            break;
        }
        pdp_release(now);
        for (auto &mp : matches_) { auto it = mp->members.find(p.src); if (it != mp->members.end()) it->second = now; }
        return;
    }
    default:
        break;
    }
    Matching *m = match_for_port(p.port);
    if (m != nullptr) handle_match(*m, p);
}

void Node::handle_match(Matching &m, const Packet &p) {
    const auto now = now_ms();
    if (auto it = m.members.find(p.src); it != m.members.end()) it->second = now;
    const bool hosts = m.mode == kModeHost || m.mode == kModeP2P;
    const bool joins = m.mode == kModeClient || m.mode == kModeP2P;
    switch (p.type) {
    case kHello:
        if (m.started && joins && m.members.find(p.src) == m.members.end()) match_events_.push_back({m.id, kMatchHello, p.src, p.payload});
        break;
    case kRequest:
        if (!m.started || !hosts) break;
        if (auto acc = m.accept_opt.find(p.src); acc != m.accept_opt.end()) {   // our accept got lost: send it again
            send_packet(kAccept, p.src, m.port, acc->second.data(), acc->second.size());
            break;
        }
        if (m.members.find(p.src) != m.members.end()) break;
        if (m.members.size() >= static_cast<std::size_t>(m.max)) { send_packet(kDeny, p.src, m.port, nullptr, 0); break; }
        if (m.requesters.insert(p.src).second) match_events_.push_back({m.id, kMatchRequest, p.src, p.payload});
        break;
    case kAccept:
        if (m.pending.erase(p.src) == 0) {   // a repeat (our "complete" got lost): confirm again
            if (m.members.find(p.src) != m.members.end()) send_packet(kComplete, p.src, m.port, nullptr, 0);
            break;
        }
        m.request_opt.erase(p.src);
        m.members[p.src] = now;
        match_events_.push_back({m.id, kMatchAccept, p.src, p.payload});
        send_packet(kComplete, p.src, m.port, nullptr, 0);
        match_events_.push_back({m.id, kMatchEstablished, p.src, {}});
        break;
    case kComplete:
        m.accept_opt.erase(p.src);
        if (m.members.find(p.src) != m.members.end() && m.confirmed.insert(p.src).second) match_events_.push_back({m.id, kMatchEstablished, p.src, {}});
        break;
    case kDeny:
        m.request_opt.erase(p.src);
        if (m.pending.erase(p.src) != 0) match_events_.push_back({m.id, kMatchDeny, p.src, p.payload});
        break;
    case kCancel:
        if (m.requesters.erase(p.src) != 0 || m.pending.erase(p.src) != 0) match_events_.push_back({m.id, kMatchCancel, p.src, p.payload});
        break;
    case kBye:
        m.accept_opt.erase(p.src); m.confirmed.erase(p.src);
        if (m.members.erase(p.src) != 0) match_events_.push_back({m.id, kMatchBye, p.src, {}});
        break;
    case kMembers:   // from our host: everyone who joined, in order (includes us)
        if (joins && m.members.count(p.src) != 0u) {
            m.order.clear();
            for (std::size_t i = 0; i + 6 <= p.payload.size() && m.order.size() < 16; i += 6) { Mac x{}; std::memcpy(x.data(), p.payload.data() + i, 6); m.order.push_back(x); }
        }
        break;
    case kPing:
        if (p.payload.size() >= 8) send_packet(kPong, p.src, m.port, p.payload.data(), 8);
        break;
    case kPong:
        if (p.payload.size() >= 8) {
            std::uint64_t then = 0;
            for (int i = 0; i < 8; ++i) then |= static_cast<std::uint64_t>(p.payload[static_cast<std::size_t>(i)]) << (8 * i);
            if (now >= then) m.rtt_ms[p.src] = static_cast<std::uint32_t>(now - then);
        }
        break;
    default:
        break;
    }
}

Node::Matching *Node::find_match(int id) {
    for (auto &m : matches_) if (m->id == id) return m.get();
    return nullptr;
}
const Node::Matching *Node::find_match(int id) const {
    for (const auto &m : matches_) if (m->id == id) return m.get();
    return nullptr;
}
Node::Matching *Node::match_for_port(unsigned port) {
    for (auto &m : matches_) if (m->port == port) return m.get();
    return nullptr;
}

// ---- control ----
void Node::ctl_scan() {
    scan_found_.clear();
    scanning_ = true;
    if (online_) { scan_until_ = now_ms() + 8000; online_scan(); return; }
    scan_until_ = now_ms() + 1000;
    send_packet(kScanReq, kBroadcast, 0, nullptr, 0);
}
std::vector<ScanEntry> Node::ctl_scan_results() const { return scan_result_; }
void Node::ctl_scan_hold() {
    scan_found_.clear();
    scanning_ = true;
    scan_until_ = ~0ull;   // (until ctl_scan_finish)
}
void Node::ctl_scan_finish(const std::vector<ScanEntry> &found) {
    scan_found_ = found;
    scanning_ = true;
    scan_until_ = 0;       // reported by the next poll()
}
void Node::ctl_connect(const char name[8]) {
    std::memcpy(group_, name, 8);
    connected_ = true;
    last_beacon_ = now_ms();
    beacon();
    ctl_events_.push_back(kCtlConnected);
}
void Node::ctl_disconnect() {
    connected_ = false;
    online_end();
    ctl_events_.push_back(kCtlDisconnected);
}
bool Node::next_ctl_event(int &event) {
    if (ctl_events_.empty()) return false;
    event = ctl_events_.front();
    ctl_events_.pop_front();
    return true;
}

// ---- datagrams ----
int Node::pdp_create(unsigned port) {
    const int id = next_pdp_++;
    pdp_[id].port = port;
    return id;
}
void Node::pdp_delete(int id) { pdp_.erase(id); }
namespace {
// RR_NET_PDPLOG=N (tests): the first N datagrams the game sends and reads, with the wall clock (ms) and the payload as
// floats, to see what the game sends and how old the data it uses is.
void pdp_log(const char *what, const Mac &peer, const std::uint8_t *data, std::size_t len, long long extra) {
    static long left = [] { const char *e = std::getenv("RR_NET_PDPLOG"); return e ? std::atol(e) : 0L; }();
    if (left <= 0) return;
    --left;
    const long long wall = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::string l = std::string("PDPLOG ") + what + " wall=" + std::to_string(wall) + " peer=" + std::to_string(peer[5]) + " len=" + std::to_string(len) + " x=" + std::to_string(extra) + " :";
    char b[24];
    for (std::size_t i = 0; i + 4 <= len && i < 160; i += 4) {
        std::uint32_t v; std::memcpy(&v, data + i, 4);
        float f; std::memcpy(&f, &v, 4);
        if (std::fabs(f) > 1e-3f && std::fabs(f) < 1e6f) std::snprintf(b, sizeof b, " %zu:%.3f", i, static_cast<double>(f));
        else std::snprintf(b, sizeof b, " %zu:#%08x", i, v);
        l += b;
    }
    nlog_hook(l);
}
}   // namespace

int Node::pdp_send(int id, const Mac &dst, unsigned dst_port, const void *data, std::size_t len) {
    const auto it = pdp_.find(id);
    if (it == pdp_.end()) return -1;
    std::vector<std::uint8_t> payload(6 + std::min(len, kMaxPayload - 6));
    const std::uint32_t t = static_cast<std::uint32_t>(now_ms());
    for (int i = 0; i < 4; ++i) payload[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((t >> (8 * i)) & 0xff);
    payload[4] = static_cast<std::uint8_t>(it->second.port & 0xff);
    payload[5] = static_cast<std::uint8_t>((it->second.port >> 8) & 0xff);
    if (len != 0) std::memcpy(payload.data() + 6, data, payload.size() - 6);
    send_packet(kPdpT, dst, dst_port, payload.data(), payload.size());
    pdp_log("send", dst, static_cast<const std::uint8_t *>(data), len, static_cast<long long>(t));
    return 0;
}
namespace {
std::int64_t get_bits(const std::vector<std::uint8_t> &b, int off, int w, bool sign) {
    std::uint64_t v = 0;
    for (int k = 0; k < w; ++k) { const int bit = off + k; if ((b[static_cast<std::size_t>(bit >> 3)] >> (bit & 7)) & 1u) v |= 1ull << k; }
    std::int64_t r = static_cast<std::int64_t>(v);
    if (sign && (v >> (w - 1)) & 1u) r -= static_cast<std::int64_t>(1ull << w);
    return r;
}
void put_bits(std::vector<std::uint8_t> &b, int off, int w, std::int64_t value) {
    const std::uint64_t v = static_cast<std::uint64_t>(value);
    for (int k = 0; k < w; ++k) {
        const int bit = off + k;
        auto &byte = b[static_cast<std::size_t>(bit >> 3)];
        if ((v >> k) & 1u) byte = static_cast<std::uint8_t>(byte | (1u << (bit & 7)));
        else byte = static_cast<std::uint8_t>(byte & ~(1u << (bit & 7)));
    }
}
}   // namespace

// The game's race packet (32 bytes, byte 0 = 5), decoded from the rig (positions in mm, the car's +0x16a0 / +0x16a8 in m):
// bits 64-84 x (signed, mm), 86-105 -z (signed, mm), 129-142 x velocity and 145-158 -z velocity (signed, 0.00748 m/s),
// 192-207 heading (unsigned, -2 pi / 65536).
void Node::predict(std::vector<std::uint8_t> &data, const Mac &src, std::uint32_t sent, std::uint64_t now) {
    static const bool on = [] { const char *e = std::getenv("RR_NET_PREDICT"); return e != nullptr && std::strcmp(e, "1") == 0; }();   // (off: the game rejects edited race packets, see netpred in rr_hle.cpp)
    static const double frames = [] { const char *e = std::getenv("RR_NET_PREDICT_FRAMES"); return e ? std::atof(e) : 8.0; }();
    if (!on || data.size() != 32u || data[0] != 5u) return;
    static const int test = [] { const char *e = std::getenv("RR_NET_PREDICT_TEST"); return e ? std::atoi(e) : 0; }();
    if (test == 1) return;                                                       // (rig: untouched)
    if (test == 2) { put_bits(data, 64, 21, get_bits(data, 64, 21, true) ^ 1); return; }   // (rig: 1 mm change only)
    if (test == 3) { data[31] ^= 1u; return; }                                  // (rig: last byte only)
    Playout &pl = playout_[src];
    const double yaw = static_cast<double>(get_bits(data, 192, 16, false));
    double yaw_rate = 0.0;   // units per ms
    if (pl.have_prev) {
        const double dt = static_cast<double>(static_cast<std::int32_t>(sent - pl.prev_sent));
        if (dt > 0.0 && dt < 500.0) {
            double dy = yaw - pl.prev_yaw;
            while (dy > 32768.0) dy -= 65536.0;
            while (dy < -32768.0) dy += 65536.0;
            yaw_rate = dy / dt;
        }
    }
    pl.have_prev = true; pl.prev_sent = sent; pl.prev_yaw = yaw;
    // how old the packet is: its trip above the fastest one seen, plus half the measured round trip (one way)
    double age = static_cast<double>(static_cast<std::int64_t>(now) - static_cast<std::int64_t>(sent) - pl.base);
    age += one_way_ms(src);
    const double lead_ms = std::clamp(age + frames * frame_ms_, 0.0, 400.0);
    const double vx = static_cast<double>(get_bits(data, 129, 14, true)) * 0.00748;    // m/s
    const double vmz = static_cast<double>(get_bits(data, 145, 14, true)) * 0.00748;   // m/s of the stored -z
    const std::int64_t x = get_bits(data, 64, 21, true) + static_cast<std::int64_t>(std::llround(vx * lead_ms));     // mm (m/s * ms = mm)
    const std::int64_t mz = get_bits(data, 86, 20, true) + static_cast<std::int64_t>(std::llround(vmz * lead_ms));
    put_bits(data, 64, 21, std::clamp<std::int64_t>(x, -(1 << 20), (1 << 20) - 1));
    put_bits(data, 86, 20, std::clamp<std::int64_t>(mz, -(1 << 19), (1 << 19) - 1));
    double ny = std::fmod(yaw + std::clamp(yaw_rate * lead_ms, -8192.0, 8192.0), 65536.0);
    if (ny < 0.0) ny += 65536.0;
    put_bits(data, 192, 16, static_cast<std::int64_t>(std::llround(ny)) & 0xffff);
}

std::uint64_t Node::pdp_playout_time(const Mac &src, std::uint32_t sent, std::uint64_t now) {
    static const bool off = [] { const char *e = std::getenv("RR_NET_JITTERBUF"); return e != nullptr && std::strcmp(e, "0") == 0; }();
    Playout &pl = playout_[src];
    if (off) {   // (still track the fastest trip, for the prediction)
        const std::int64_t d0 = static_cast<std::int64_t>(now) - static_cast<std::int64_t>(sent);
        pl.base = pl.n == 0 ? d0 : std::min(pl.base, d0); pl.n = 1;
        return now;
    }
    // d = arrival - send in mixed clocks: its minimum over the last 64 datagrams is the fastest trip seen; the spread above it
    // is the jitter.  Playout = send time + fastest trip + the 90th percentile of the spread (at most 100 ms extra).
    const std::int64_t d = static_cast<std::int64_t>(now) - static_cast<std::int64_t>(sent);
    pl.d[pl.pos] = d; pl.pos = (pl.pos + 1) % 64; if (pl.n < 64) ++pl.n;
    std::int64_t sorted[64];
    std::copy(pl.d, pl.d + pl.n, sorted);
    std::sort(sorted, sorted + pl.n);
    const std::int64_t base = sorted[0];
    pl.base = base;
    std::int64_t spread = std::min<std::int64_t>(100, sorted[(pl.n * 9) / 10] - base);
    // Below ~20 ms the spread is mostly our own sampling (packets are picked up when the game next calls the network, once
    // or so a frame), and the game reads once a frame anyway: holding packets for it only adds delay (measured +5..27 ms
    // on a clean line).  So only a real jitter (more than 20 ms) is smoothed out.
    if (spread <= 20) spread = 0;
    std::int64_t when = static_cast<std::int64_t>(sent) + base + spread;
    if (spread == 0) when = static_cast<std::int64_t>(now);   // (clean line: straight through)
    if (pl.n < 8) when = static_cast<std::int64_t>(now);   // (just started: no history yet)
    std::uint64_t release = when > static_cast<std::int64_t>(now) ? static_cast<std::uint64_t>(when) : now;
    release = std::max(release, pl.last_release);   // keep the order
    pl.last_release = release;
    return release;
}
double Node::data_age_ms() const {
    const std::uint64_t now = now_ms();
    double sum = 0.0; int n = 0;
    for (const auto &[mac, pl] : playout_) { (void)mac; if (pl.age_ema >= 0.0 && now - pl.age_at < 2000u) { sum += pl.age_ema; ++n; } }
    return n ? sum / n : 0.0;
}
void Node::pdp_release(std::uint64_t now) {
    for (auto &[id, pdp] : pdp_) {
        (void)id;
        while (!pdp.held.empty() && pdp.held.front().first <= now) {
            if (pdp.queue.size() < 256) pdp.queue.push_back(std::move(pdp.held.front().second));
            pdp.held.pop_front();
        }
    }
}
int Node::pdp_recv(int id, Mac &src, unsigned &src_port, void *buf, std::size_t capacity, std::size_t &len) {
    pdp_release(now_ms());
    const auto it = pdp_.find(id);
    if (it == pdp_.end() || it->second.queue.empty()) return -1;
    auto &front = it->second.queue.front();
    src = front.first.first;
    src_port = front.first.second;
    len = std::min(capacity, front.second.size());
    if (len != 0) std::memcpy(buf, front.second.data(), len);
    pdp_log("recv", src, front.second.data(), front.second.size(), static_cast<long long>(it->second.queue.size()));
    it->second.queue.pop_front();
    return 0;
}

// ---- matching ----
int Node::match_create(int mode, int max_members, unsigned port, std::uint32_t hello_us, std::uint32_t keepalive_us) {
    auto m = std::make_unique<Matching>();
    m->id = next_match_++;
    m->mode = mode;
    m->max = std::max(1, max_members);
    m->port = port;
    m->hello_ms = std::max<std::uint32_t>(100, hello_us / 1000);
    m->keepalive_ms = std::max<std::uint32_t>(2000, keepalive_us / 1000);
    const int id = m->id;
    matches_.push_back(std::move(m));
    return id;
}
int Node::match_start(int id, const void *opt, std::size_t opt_len) {
    Matching *m = find_match(id);
    if (m == nullptr) return -1;
    m->opt.assign(static_cast<const std::uint8_t *>(opt), static_cast<const std::uint8_t *>(opt) + (opt == nullptr ? 0 : opt_len));
    if (!m->opt.empty()) m->adv_opt = m->opt;
    m->started = true;
    m->last_hello = 0;
    if (online_) {
        if (m->mode == kModeHost || m->mode == kModeP2P) online_begin_host();
        if (m->mode == kModeClient || m->mode == kModeP2P) online_begin_client();
    }
    return 0;
}
void Node::match_set_hello_opt(int id, const void *opt, std::size_t opt_len) {
    if (Matching *m = find_match(id)) {
        m->opt.assign(static_cast<const std::uint8_t *>(opt), static_cast<const std::uint8_t *>(opt) + (opt == nullptr ? 0 : opt_len));
        if (!m->opt.empty()) m->adv_opt = m->opt;
    }
}
void Node::match_select_target(int id, const Mac &target, const void *opt, std::size_t opt_len) {
    Matching *m = find_match(id);
    if (m == nullptr) return;
    const auto *o = static_cast<const std::uint8_t *>(opt);
    std::vector<std::uint8_t> payload(o, o + (opt == nullptr ? 0 : opt_len));
    if (m->requesters.erase(target) != 0) {            // host accepting a request
        m->members[target] = now_ms();
        if (std::find(m->order.begin(), m->order.end(), target) == m->order.end()) { m->order.push_back(target); m->members_dirty = true; }
        m->accept_opt[target] = payload;
        m->confirmed.erase(target);
        send_packet(kAccept, target, m->port, payload.data(), payload.size());
    } else if (m->mode != kModeHost) {                 // joining someone's room
        m->pending.insert(target);
        m->request_opt[target] = payload;
        send_packet(kRequest, target, m->port, payload.data(), payload.size());
    }
}
void Node::match_cancel_target(int id, const Mac &target, const void *opt, std::size_t opt_len) {
    Matching *m = find_match(id);
    if (m == nullptr) return;
    const auto *o = static_cast<const std::uint8_t *>(opt);
    std::vector<std::uint8_t> payload(o, o + (opt == nullptr ? 0 : opt_len));
    m->request_opt.erase(target); m->accept_opt.erase(target); m->confirmed.erase(target);
    // (sent three times: there is no answer to these, so a lost one would leave the other side waiting)
    for (int k = 0; k < 3; ++k) {
        if (m->requesters.count(target) != 0u) send_packet(kDeny, target, m->port, payload.data(), payload.size());
        else if (m->pending.count(target) != 0u) send_packet(kCancel, target, m->port, payload.data(), payload.size());
        else if (m->members.count(target) != 0u) send_packet(kBye, target, m->port, nullptr, 0);
    }
    if (m->requesters.erase(target) == 0 && m->pending.erase(target) == 0) m->members.erase(target);
}
std::vector<Member> Node::match_members(int id) const {
    std::vector<Member> out;
    const Matching *m = find_match(id);
    if (m == nullptr) return out;
    out.push_back({mac_});
    if (m->mode == kModeHost || m->mode == kModeP2P) {   // host: us, then the joiners in the order they joined
        for (const Mac &x : m->order) if (m->members.count(x) != 0u) out.push_back({x});
        for (const auto &[peer, heard] : m->members) { (void)heard; if (std::find(m->order.begin(), m->order.end(), peer) == m->order.end()) out.push_back({peer}); }
        return out;
    }
    // joiner: us, the host, then the other joiners from the host's member list (as the PSP library reports them)
    for (const auto &[peer, heard] : m->members) { (void)heard; out.push_back({peer}); }
    for (const Mac &x : m->order) {
        if (x == mac_ || m->members.count(x) != 0u) continue;
        out.push_back({x});
    }
    return out;
}
void Node::match_stop(int id) {
    Matching *m = find_match(id);
    if (m == nullptr) return;
    for (int k = 0; k < 3; ++k) {   // (three times, see match_cancel_target)
        for (const auto &[peer, heard] : m->members) { (void)heard; send_packet(kBye, peer, m->port, nullptr, 0); }
        for (const auto &peer : m->pending) send_packet(kCancel, peer, m->port, nullptr, 0);
        for (const auto &peer : m->requesters) send_packet(kDeny, peer, m->port, nullptr, 0);
    }
    m->request_opt.clear(); m->accept_opt.clear(); m->confirmed.clear();
    m->members.clear(); m->pending.clear(); m->requesters.clear();
    m->order.clear();
    m->started = false;
    online_end();
}
void Node::match_delete(int id) {
    match_stop(id);
    matches_.erase(std::remove_if(matches_.begin(), matches_.end(), [id](const auto &m) { return m->id == id; }), matches_.end());
}
bool Node::next_match_event(MatchEvent &event) {
    if (match_events_.empty()) return false;
    event = std::move(match_events_.front());
    match_events_.pop_front();
    return true;
}


// RR_NET_ID=<6 hex digits> (tests): fixed last three bytes of our id, i.e. a known room code.
static void apply_test_id(Mac &mac) {
    const char *e = std::getenv("RR_NET_ID");
    if (e == nullptr || std::strlen(e) != 6) return;
    const unsigned long v = std::strtoul(e, nullptr, 16);
    mac[3] = static_cast<std::uint8_t>(v >> 16); mac[4] = static_cast<std::uint8_t>(v >> 8); mac[5] = static_cast<std::uint8_t>(v);
}

// ================= internet mode =================
#if RR_NET_SOCKETS

namespace {

void (*g_logger)(const std::string &) = nullptr;
std::FILE *g_log_file = nullptr;
void nlog(const std::string &s) {
    if (g_logger != nullptr) g_logger(s);
    if (g_log_file != nullptr) {
        const auto ms = static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() % 100000000ull);
        std::fprintf(g_log_file, "[%llu.%03llu] %s\n", ms / 1000, ms % 1000, s.c_str());
        std::fflush(g_log_file);
    }
}

using SockT = decltype(::socket(0, 0, 0));

std::string mac_hex(const Mac &m) {
    char b[13];
    std::snprintf(b, sizeof b, "%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    return b;
}
bool parse_mac(const std::string &s, Mac &m) {
    if (s.size() != 12) return false;
    for (int i = 0; i < 6; ++i) {
        unsigned v = 0;
        if (std::sscanf(s.c_str() + i * 2, "%2x", &v) != 1) return false;
        m[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v);
    }
    return true;
}
std::string hex_bytes(const char *p, int n) {
    std::string out;
    char b[3];
    for (int i = 0; i < n; ++i) { std::snprintf(b, sizeof b, "%02x", static_cast<unsigned char>(p[i])); out += b; }
    return out;
}
bool unhex_bytes(const std::string &s, char *out, int n) {
    if (static_cast<int>(s.size()) != n * 2) return false;
    for (int i = 0; i < n; ++i) {
        unsigned v = 0;
        if (std::sscanf(s.c_str() + i * 2, "%2x", &v) != 1) return false;
        out[i] = static_cast<char>(v);
    }
    return true;
}
bool parse_ep(const std::string &s, sockaddr_in &a) {
    const auto colon = s.rfind(':');
    if (colon == std::string::npos) return false;
    std::memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    if (inet_pton(AF_INET, s.substr(0, colon).c_str(), &a.sin_addr) != 1) return false;
    const int port = std::atoi(s.c_str() + colon + 1);
    if (port < 1 || port > 65535) return false;
    a.sin_port = htons(static_cast<std::uint16_t>(port));
    return true;
}
std::string ep_str(const sockaddr_in &a) {
    char ip[64] = {};
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof ip);
    return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}
bool same_ep(const sockaddr_in &a, const sockaddr_in &b) { return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port; }

std::vector<std::string> split(const std::string &s, char sep) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (;;) {
        const auto e = s.find(sep, start);
        if (e == std::string::npos) { if (start < s.size()) out.push_back(s.substr(start)); break; }
        if (e > start) out.push_back(s.substr(start, e - start));
        start = e + 1;
    }
    return out;
}

std::string url_encode(const std::string &v) {
    std::string out;
    char b[4];
    for (unsigned char c : v) {
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-' || c == '_' || c == '.' || c == '~') out.push_back(static_cast<char>(c));
        else { std::snprintf(b, sizeof b, "%%%02X", c); out += b; }
    }
    return out;
}
// One "room ..." line of the room service: room <mac> <ep> <lep> <name> <n> <max> [<vis> <info> <est> <open>]
bool parse_room_line(const std::vector<std::string> &t, RoomInfo &r) {
    if (t.size() < 7 || t[0] != "room") return false;
    if (!parse_mac(t[1], r.mac) || !unhex_bytes(t[4], r.name, 8)) return false;
    r.ep = t[2]; r.lep = t[3];
    r.players = std::atoi(t[5].c_str()); r.max = std::atoi(t[6].c_str());
    r.vis = t.size() > 7 ? std::atoi(t[7].c_str()) : 0;
    r.info.clear();
    if (t.size() > 8 && t[8] != "-" && t[8].size() % 2 == 0 && t[8].size() <= 128) {
        r.info.resize(t[8].size() / 2);
        if (!unhex_bytes(t[8], reinterpret_cast<char *>(r.info.data()), static_cast<int>(r.info.size()))) r.info.clear();
    }
    r.est_ms = t.size() > 9 ? std::atoi(t[9].c_str()) : -1;
    r.racing = t.size() > 10 && t[10] == "0";   // (open flag: 1 accepting entries, 0 a race is on)
    return true;
}

// One background thread for everything that can block (name lookups, HTTP).  A job runs there and returns a function
// that is then run on the game's thread from poll().
class Worker {
public:
    using Completion = std::function<void()>;
    Worker() : thread_([this] { run(); }) {}
    ~Worker() {
        { std::lock_guard<std::mutex> lock(m_); quit_ = true; }
        cv_.notify_all();
        thread_.join();
    }
    void post(std::function<Completion()> job) {
        { std::lock_guard<std::mutex> lock(m_); jobs_.push_back(std::move(job)); }
        cv_.notify_one();
    }
    void drain() {
        for (;;) {
            Completion c;
            { std::lock_guard<std::mutex> lock(m_); if (done_.empty()) return; c = std::move(done_.front()); done_.pop_front(); }
            if (c) c();
        }
    }
private:
    void run() {
        for (;;) {
            std::function<Completion()> job;
            {
                std::unique_lock<std::mutex> lock(m_);
                cv_.wait(lock, [this] { return quit_ || !jobs_.empty(); });
                if (quit_) return;
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            Completion c = job();
            std::lock_guard<std::mutex> lock(m_);
            done_.push_back(std::move(c));
        }
    }
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::function<Completion()>> jobs_;
    std::deque<Completion> done_;
    bool quit_{false};
    std::thread thread_;
};

constexpr std::uint32_t kStunCookie = 0x2112A442u;
constexpr std::uint64_t kPunchWindowMs = 30000;

}   // namespace

void set_logger(void (*fn)(const std::string &)) { g_logger = fn; }
void set_log_file(const std::string &path) {
    if (g_log_file != nullptr) { std::fclose(g_log_file); g_log_file = nullptr; }
    if (!path.empty()) g_log_file = std::fopen(path.c_str(), "w");
}
static void nlog_hook(const std::string &s) { nlog(s); }

// A peer address of "0.0.0.0:1" in the room list means "reachable only through the relay" (a browser player).
constexpr const char *kRelayOnlyEp = "0.0.0.0:1";

struct Node::Online {
    // (2: datagrams carry the sender clock, kPdpT; 3: relay for browser players / blocked routers; 4: server browser (room
    // descriptions, passwords, unlisted rooms).  Different tags never see each other's rooms.)
    std::string server, ver{RR_GAME_VERSION};
    bool update_logged{false};
    std::string lep, pep;                 // our address as seen locally / from outside ("ip:port")
    bool stun_enabled{true}, symmetric{false}, stun_requested{false};
    struct Stun {
        std::string host; int port;
        sockaddr_in addr{}; bool resolved{false}, answered{false}; sockaddr_in mapped{}; std::uint8_t tx[12]{};
    };
    Stun stun[2]{{"stun.l.google.com", 19302}, {"stun.cloudflare.com", 3478}};   // RR_NET_STUN1/2="host:port" override (tests)
    std::uint64_t last_stun{0}, stun_started{0};
    struct Peer {
        std::vector<sockaddr_in> cands;
        sockaddr_in locked{};
        bool heard{false}, dead{false};
        bool relay{false};                // talk through the relay instead of directly
        std::uint64_t punch_until{0}, last_punch{0};
    };
    std::unique_ptr<Relay> relay;         // opened when the first relayed peer appears (at once in the browser)
    bool relay_wanted{false}, relay_logged_ready{false}, relay_unsupported_logged{false};
    std::uint64_t relay_retry_at{0};
    std::string relay_url;
    std::map<Mac, Peer> peers;
    bool hosting{false}, host_inflight{false}, client{false}, list_inflight{false}, scan_inflight{false}, unreachable_logged{false};
    std::string name_hex;
    std::uint64_t last_host{0}, last_list{0};
    std::set<Mac> joined;
    // server browser
    int host_vis{0};                      // who may join our room (see set_host_options)
    bool host_was_accepting{false};       // (to notice the host starting a race)
    std::set<Mac> ignored_joiners;        // joiners turned away while a race was on (logged once)
    std::string host_pw;
    bool have_target{false};              // client: join only this room
    RoomInfo target;
    std::string target_pw;
    std::uint64_t target_last_join{0};
    bool browse_inflight{false}, lookup_inflight{false}, probe_inflight{false};
    std::uint32_t ping_token{1};
    std::map<std::uint32_t, std::pair<Mac, std::uint64_t>> pings_out;   // token -> (room, sent at)
    std::set<Mac> ping_pending;           // waiting for the relay to connect
    Worker worker;                        // keep last: its thread must stop before the rest is destroyed
};

namespace {
std::string local_ip() {
    if (const char *e = std::getenv("RR_NET_LOCAL_IP"); e != nullptr && e[0] != '\0') return e;
    const auto s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    std::string out = "127.0.0.1";
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &to.sin_addr);
    if (::connect(s, reinterpret_cast<sockaddr *>(&to), sizeof to) == 0) {   // no packet is sent; this just picks the route
        sockaddr_in me{};
#ifdef _WIN32
        int len = sizeof me;
#else
        socklen_t len = sizeof me;
#endif
        if (::getsockname(s, reinterpret_cast<sockaddr *>(&me), &len) == 0) {
            char ip[64] = {};
            inet_ntop(AF_INET, &me.sin_addr, ip, sizeof ip);
            out = ip;
        }
    }
#ifdef _WIN32
    closesocket(s);
#else
    ::close(s);
#endif
    return out;
}
}   // namespace

bool Node::start_online(const std::string &server, int port) {
    if (running()) return true;
    if (server.empty()) return false;
#if RR_NET_WEB
    (void)port;
    socket_ = -2;   // (no socket: everything goes through the relay)
    port_ = 0;
    {
        std::random_device rd;
        mac_ = Mac{0x02, 0x52, 0x52, static_cast<std::uint8_t>(rd()), static_cast<std::uint8_t>(rd()), static_cast<std::uint8_t>(rd())};
        apply_test_id(mac_);
    }
    online_ = std::make_unique<Online>();
    {
        Online &o = *online_;
        o.server = server;
        while (!o.server.empty() && o.server.back() == '/') o.server.pop_back();
        if (const char *v = std::getenv("RR_NET_VER"); v != nullptr && v[0] != '\0') o.ver = v;
        o.lep = o.pep = kRelayOnlyEp;
        o.stun_enabled = false;
        o.relay_wanted = true;
        nlog("online mode (browser: all players through the relay): our id " + mac_hex(mac_) + ", room service " + o.server);
    }
    sim_setup();
    return true;
#endif
#ifdef _WIN32
    static bool wsa = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }();
    if (!wsa) return false;
#endif
    const auto s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
    if (s == INVALID_SOCKET) return false;
#else
    if (s < 0) return false;
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0) {
#ifdef _WIN32
        closesocket(s);
#else
        ::close(s);
#endif
        return false;
    }
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    DWORD bytes = 0; BOOL off = FALSE;
    WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12), &off, sizeof off, nullptr, 0, &bytes, nullptr, nullptr);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
    sockaddr_in bound{};
#ifdef _WIN32
    int blen = sizeof bound;
#else
    socklen_t blen = sizeof bound;
#endif
    ::getsockname(s, reinterpret_cast<sockaddr *>(&bound), &blen);
    socket_ = static_cast<std::intptr_t>(s);
    port_ = ntohs(bound.sin_port);
    std::random_device rd;
    mac_ = Mac{0x02, 0x52, 0x52, static_cast<std::uint8_t>(rd()), static_cast<std::uint8_t>(rd()), static_cast<std::uint8_t>(rd())};
    apply_test_id(mac_);

    online_ = std::make_unique<Online>();
    Online &o = *online_;
    o.server = server;
    while (!o.server.empty() && o.server.back() == '/') o.server.pop_back();
    if (const char *v = std::getenv("RR_NET_VER"); v != nullptr && v[0] != '\0') o.ver = v;
    o.lep = local_ip() + ":" + std::to_string(port_);
    const char *st = std::getenv("RR_NET_STUN");
    o.stun_enabled = !(st != nullptr && std::strcmp(st, "off") == 0);
    for (int i = 0; i < 2; ++i) {
        const char *e = std::getenv(i == 0 ? "RR_NET_STUN1" : "RR_NET_STUN2");
        if (e == nullptr || e[0] == '\0') continue;
        const std::string v = e;
        const auto colon = v.rfind(':');
        if (colon == std::string::npos) continue;
        o.stun[i].host = v.substr(0, colon);
        o.stun[i].port = std::atoi(v.c_str() + colon + 1);
    }
    if (!o.stun_enabled) {
        o.pep = o.lep;
    }
    if (std::getenv("RR_NET_FORCE_RELAY") != nullptr) {   // (test: behave like a browser player - relay only)
        o.pep = o.lep = kRelayOnlyEp;
        o.stun_enabled = false;
        o.relay_wanted = true;
    }
    nlog("online mode: our id " + mac_hex(mac_) + ", local address " + o.lep + ", room service " + o.server + (o.stun_enabled ? "" : " (STUN off)"));
    sim_setup();
    return true;
}

void Node::online_stun(const std::uint8_t *buf, std::size_t len) {
    if (!online_ || len < 20 || buf[0] != 0x01 || buf[1] != 0x01) return;   // binding success response
    for (auto &s : online_->stun) {
        if (!s.resolved || std::memcmp(buf + 8, s.tx, 12) != 0) continue;
        std::size_t pos = 20;
        while (pos + 4 <= len) {
            const unsigned type = static_cast<unsigned>(buf[pos] << 8 | buf[pos + 1]);
            const std::size_t alen = static_cast<std::size_t>(buf[pos + 2] << 8 | buf[pos + 3]);
            if (pos + 4 + alen > len) break;
            const std::uint8_t *v = buf + pos + 4;
            if ((type == 0x0020 || type == 0x0001) && alen >= 8 && v[1] == 0x01) {
                unsigned port = static_cast<unsigned>(v[2] << 8 | v[3]);
                std::uint8_t ip[4] = {v[4], v[5], v[6], v[7]};
                if (type == 0x0020) {
                    port ^= 0x2112u;
                    ip[0] ^= 0x21; ip[1] ^= 0x12; ip[2] ^= 0xA4; ip[3] ^= 0x42;
                }
                std::memset(&s.mapped, 0, sizeof s.mapped);
                s.mapped.sin_family = AF_INET;
                s.mapped.sin_port = htons(static_cast<std::uint16_t>(port));
                std::memcpy(&s.mapped.sin_addr, ip, 4);
                s.answered = true;
                if (type == 0x0020) return;
            }
            pos += 4 + ((alen + 3) & ~static_cast<std::size_t>(3));
        }
        return;
    }
}

// The public-address lookup (name lookups + STUN) only starts once the player actually opens a room or a search.
void Node::online_need_stun() {
    if (!online_) return;
    Online &o = *online_;
    if (o.stun_requested || !o.stun_enabled) return;
    o.stun_requested = true;
    Online *op = &o;
        for (int i = 0; i < 2; ++i) {
            o.worker.post([op, i]() -> Worker::Completion {
                addrinfo hints{}, *res = nullptr;
                hints.ai_family = AF_INET;
                hints.ai_socktype = SOCK_DGRAM;
                sockaddr_in found{};
                bool ok = false;
                if (getaddrinfo(op->stun[i].host.c_str(), nullptr, &hints, &res) == 0 && res != nullptr) {
                    found = *reinterpret_cast<sockaddr_in *>(res->ai_addr);
                    found.sin_port = htons(static_cast<std::uint16_t>(op->stun[i].port));
                    ok = true;
                }
                if (res != nullptr) freeaddrinfo(res);
                return [op, i, found, ok] { if (ok) { op->stun[i].addr = found; op->stun[i].resolved = true; } };
            });
        }
}

void Node::online_send(const std::uint8_t *buf, std::size_t len, const Mac &dst) {
    Online &o = *online_;
    const auto raw = [&](const sockaddr_in &to) {
        if (RR_NET_WEB) return;
        ::sendto(static_cast<SockT>(socket_), reinterpret_cast<const char *>(buf), static_cast<int>(len), 0, reinterpret_cast<const sockaddr *>(&to), sizeof to);
    };
    const auto to_peer = [&](const Mac &m, Online::Peer &p) {
        if (p.relay || RR_NET_WEB) {
            o.relay_wanted = true;
            if (o.relay && o.relay->ready()) o.relay->send(m, buf, len);
            return;
        }
        if (p.dead) return;
        if (p.heard) raw(p.locked);
        else for (const auto &c : p.cands) raw(c);
    };
    if (dst == kBroadcast) { for (auto &[m, p] : o.peers) to_peer(m, p); return; }
    const auto it = o.peers.find(dst);
    if (it != o.peers.end()) { to_peer(it->first, it->second); return; }
    // another joiner of the room we are in: only the host knows how to reach them, so it passes the packet on
    if (const Mac *h = joined_host(); h != nullptr && *h != dst && o.peers.count(*h) != 0u && len <= kMaxPayload) send_packet(kFwd, *h, 0, buf, len);
}

bool Node::online_accept(const Mac &src, const void *from_addr) {
    Online &o = *online_;
    const auto it = o.peers.find(src);
    if (it == o.peers.end()) return false;      // only players the room service told us about
    const sockaddr_in &from = *static_cast<const sockaddr_in *>(from_addr);
    Online::Peer &p = it->second;
    if (!p.heard || !same_ep(p.locked, from)) nlog("peer " + mac_hex(src) + " reachable at " + ep_str(from));
    p.heard = true;
    p.dead = false;
    p.locked = from;
    return true;
}

static void add_peer(Node::Online &o, std::map<Mac, Node::Online::Peer> &peers, const Mac &mac, const std::string &ep, const std::string &lep, std::uint64_t now, bool &fresh);

void Node::online_relay_tick(std::uint64_t now) {
    Online &o = *online_;
    if (!o.relay_wanted) return;
    if (o.relay_url.empty()) {
        std::string u = o.server;
        if (u.rfind("https://", 0) == 0) u = "wss://" + u.substr(8);
        else if (u.rfind("http://", 0) == 0) u = "ws://" + u.substr(7);
        o.relay_url = u + "/relay?mac=" + mac_hex(mac_);
    }
    if (!o.relay) {
        o.relay = make_relay(o.relay_url);
        if (!o.relay) {
            if (!o.relay_unsupported_logged) {
                o.relay_unsupported_logged = true;
                nlog("this build cannot use the relay at " + o.relay_url + " (browser players and blocked routers will not connect)");
            }
            return;
        }
        o.relay->open(o.relay_url);
        o.relay_retry_at = now + 3000;
        nlog("connecting to the relay");
    } else if (o.relay->failed() && now >= o.relay_retry_at) {
        nlog("relay link " + o.relay->describe() + ": retrying");
        o.relay_logged_ready = false;
        o.relay->open(o.relay_url);
        o.relay_retry_at = now + 3000;
    }
    if (o.relay->ready() && !o.relay_logged_ready) { o.relay_logged_ready = true; nlog("relay connected"); }
    if (o.relay->ready() && !o.ping_pending.empty()) {   // server browser pings asked for before the relay was up
        const std::set<Mac> pending = std::move(o.ping_pending);
        o.ping_pending.clear();
        for (const Mac &m : pending) browse_ping(m);
    }
    Mac src{};
    std::vector<std::uint8_t> data;
    for (int guard = 0; guard < 512 && o.relay->recv(src, data); ++guard) {
        // Server browser ping ("RRBP" + token) and its answer ("RRBQ" + token): answered here, never given to the game.
        if (data.size() == 8 && std::memcmp(data.data(), "RRBP", 4) == 0) {
            std::uint8_t pong[8];
            std::memcpy(pong, "RRBQ", 4);
            std::memcpy(pong + 4, data.data() + 4, 4);
            o.relay->send(src, pong, sizeof pong);
            continue;
        }
        if (data.size() == 8 && std::memcmp(data.data(), "RRBQ", 4) == 0) {
            std::uint32_t token = 0;
            std::memcpy(&token, data.data() + 4, 4);
            const auto it = o.pings_out.find(token);
            if (it != o.pings_out.end() && it->second.first == src) {
                browse_.ping_ms[src] = static_cast<int>(now - it->second.second);
                o.pings_out.erase(it);
            }
            continue;
        }
        if (src == Mac{}) {   // from the service itself
            const std::string line(data.begin(), data.end());
            const auto t = split(line, ' ');
            Mac m{};
            if (t.size() >= 4 && t[0] == "joiner" && o.hosting && parse_mac(t[1], m) && m != mac_) {
                if (!host_accepting() && !session_peer(m)) {   // a race is on: the game would not take them
                    if (o.ignored_joiners.insert(m).second) nlog("player " + t[1] + " asked to join during the race: not now");
                    continue;
                }
                bool fresh = false;
                add_peer(o, o.peers, m, t[2], t[3], now, fresh);
                if (fresh) {
                    nlog("player " + t[1] + " wants to join (announced at once): " + (o.peers[m].relay ? std::string("through the relay") : "punching toward " + t[2] + " / " + t[3]));
                    for (auto &mp : matches_) mp->last_hello = 0;   // say hello right away
                }
            }
            continue;
        }
        relay_src_ = &src;
        receive(data.data(), data.size(), nullptr);
        relay_src_ = nullptr;
    }
}

bool Node::online_accept_relay(const Mac &src) {
    Online &o = *online_;
    auto it = o.peers.find(src);
    if (it == o.peers.end()) {
        // Not introduced by the room service yet (its list is polled every few seconds): accept while a room is open or
        // being searched for; the relay vouches for the sender id.
        if (!o.hosting && !o.client) return false;
        if (o.hosting && o.host_vis != 0) return false;   // (password / code-only rooms: only players the room service let in)
        if (o.hosting && !host_accepting()) return false;  // (a race is on: only its players)
        it = o.peers.emplace(src, Online::Peer{}).first;
        nlog("peer " + mac_hex(src) + " reached us through the relay");
    } else if (!it->second.relay) {
        nlog("peer " + mac_hex(src) + " now talks to us through the relay");
    }
    it->second.relay = true;
    it->second.heard = true;
    it->second.dead = false;
    return true;
}

static void add_peer(Node::Online &o, std::map<Mac, Node::Online::Peer> &peers, const Mac &mac, const std::string &ep, const std::string &lep, std::uint64_t now, bool &fresh) {
    sockaddr_in a{}, l{};
    const bool have_a = parse_ep(ep, a), have_l = parse_ep(lep, l);
    auto it = peers.find(mac);
    fresh = it == peers.end();
    Node::Online::Peer &p = peers[mac];
    const auto add = [&](const sockaddr_in &c) {
        for (const auto &x : p.cands) if (same_ep(x, c)) return;
        p.cands.push_back(c);
    };
    static const bool force_relay = std::getenv("RR_NET_FORCE_RELAY") != nullptr;   // (test: every player through the relay)
    if (ep == kRelayOnlyEp || lep == kRelayOnlyEp || RR_NET_WEB || force_relay) {   // a browser player (or we are one): no direct path
        if (!p.relay) nlog("peer " + mac_hex(mac) + " is reached through the relay");
        p.relay = true; p.dead = false;
        o.relay_wanted = true;
        return;
    }
    if (have_l) add(l);
    if (have_a) add(a);
    if (fresh || p.dead) { p.dead = false; p.punch_until = now + kPunchWindowMs; }
}

void Node::online_begin_host() {
    Online &o = *online_;
    o.hosting = true;
    o.relay_wanted = true;   // (the room service announces joiners over it at once)
    online_need_stun();
    o.last_host = 0;
    o.name_hex = hex_bytes(group_, 8);
    nlog("hosting: opening a room (group " + o.name_hex + ")");
}

void Node::online_begin_client() {
    Online &o = *online_;
    o.client = true;
    online_need_stun();
    o.last_list = 0;
    o.target_last_join = 0;
    o.joined.clear();
    o.name_hex = hex_bytes(group_, 8);
    nlog("searching for rooms in group " + o.name_hex);
}

void Node::online_end() {
    if (!online_) return;
    Online &o = *online_;
    if (o.hosting) {
        const std::string url = o.server + "/leave?mac=" + mac_hex(mac_);
        o.worker.post([url]() -> Worker::Completion { http_request("POST", url, 4000); return nullptr; });
    }
    o.hosting = false;
    o.client = false;
    o.have_target = false;
    o.target_pw.clear();
    o.peers.clear();
    o.joined.clear();
}

void Node::online_scan() {
    Online &o = *online_;
    if (o.scan_inflight) return;
    online_need_stun();
    o.scan_inflight = true;
    const std::string url = o.server + "/rooms?ver=" + o.ver;
    Online *op = &o;
    Node *self = this;
    o.worker.post([url, op, self]() -> Worker::Completion {
        const HttpResult r = http_request("GET", url, 5000);
        return [r, op, self] {
            op->scan_inflight = false;
            if (r.status != 200) {
                if (!op->unreachable_logged) { op->unreachable_logged = true; nlog("room service not reachable (status " + std::to_string(r.status) + ")"); }
                self->scan_until_ = 0;   // finish the scan with nothing found
                return;
            }
            op->unreachable_logged = false;
            self->scan_found_.clear();
            for (const auto &line : split(r.body, '\n')) {
                const auto t = split(line, ' ');
                if (t.size() < 7 || t[0] != "room") continue;
                ScanEntry e;
                Mac m{};
                char name[8];
                if (!parse_mac(t[1], m) || !unhex_bytes(t[4], name, 8)) continue;
                std::memcpy(e.name, name, 8);
                e.bssid = m;
                bool dup = false;
                for (const auto &f : self->scan_found_) if (std::memcmp(f.name, e.name, 8) == 0) dup = true;
                if (!dup) self->scan_found_.push_back(e);
            }
            nlog("room search: " + std::to_string(self->scan_found_.size()) + " group(s) found");
            self->scan_until_ = 0;
        };
    });
}

// The room service answers "update <newest>" to a game version it no longer serves (see rr_version.hpp).
static bool update_reply(const std::string &body, std::string &newest) {
    if (body.rfind("update", 0) != 0) return false;
    const auto t = split(split(body, '\n')[0], ' ');
    newest = t.size() >= 2 ? t[1] : std::string("a newer version");
    while (!newest.empty() && (newest.back() == '\r' || newest.back() == ' ')) newest.pop_back();
    return true;
}

// ---- server browser ----
void Node::browse_refresh() {
    if (!online_ || online_->browse_inflight) return;
    Online &o = *online_;
    o.browse_inflight = true;
    browse_.list_busy = true;
    if (!RR_NET_WEB) o.relay_wanted = true;   // (pings go through the relay)
    const std::string url = o.server + "/rooms?ver=" + o.ver;
    Online *op = &o;
    Node *self = this;
    o.worker.post([url, op, self]() -> Worker::Completion {
        const HttpResult r = http_request("GET", url, 6000);
        return [r, op, self] {
            op->browse_inflight = false;
            BrowseState &b = self->browse_;
            b.list_busy = false;
            b.list_status = r.status == 0 ? -1 : r.status;
            ++b.list_gen;
            if (r.status != 200) return;
            b.rooms.clear();
            if (update_reply(r.body, b.update_to)) { nlog("room service: this game version (" + op->ver + ") is out of date, newest is " + b.update_to); return; }
            for (const auto &line : split(r.body, '\n')) {
                RoomInfo ri;
                if (parse_room_line(split(line, ' '), ri) && ri.mac != self->mac_) b.rooms.push_back(std::move(ri));
            }
        };
    });
}

void Node::browse_lookup(const std::string &code) {
    if (!online_ || online_->lookup_inflight) return;
    Online &o = *online_;
    o.lookup_inflight = true;
    browse_.lookup_busy = true;
    std::string c;
    for (char ch : code) if (std::isxdigit(static_cast<unsigned char>(ch))) c.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    const std::string url = o.server + "/room?ver=" + o.ver + "&code=" + url_encode(c);
    Online *op = &o;
    Node *self = this;
    o.worker.post([url, op, self]() -> Worker::Completion {
        const HttpResult r = http_request("GET", url, 6000);
        return [r, op, self] {
            op->lookup_inflight = false;
            BrowseState &b = self->browse_;
            b.lookup_busy = false;
            b.lookup_status = r.status == 0 ? -1 : r.status;
            b.lookup_found = false;
            b.lookup_other_version.clear();
            ++b.lookup_gen;
            if (r.status != 200) return;
            if (update_reply(r.body, b.update_to)) return;
            if (r.body.rfind("version ", 0) == 0) {   // that code belongs to a race on another version of the game
                b.lookup_other_version = split(split(r.body, '\n')[0], ' ').back();
                return;
            }
            for (const auto &line : split(r.body, '\n')) {
                RoomInfo ri;
                if (parse_room_line(split(line, ' '), ri)) { b.lookup = std::move(ri); b.lookup_found = b.lookup.mac != self->mac_; break; }
            }
        };
    });
}

void Node::browse_probe(const Mac &room, const std::string &password) {
    if (!online_ || online_->probe_inflight) return;
    Online &o = *online_;
    o.probe_inflight = true;
    browse_.probe_busy = true;
    const std::string pep = o.pep.empty() ? o.lep : o.pep;
    const std::string url = o.server + "/join?room=" + mac_hex(room) + "&mac=" + mac_hex(mac_) + "&ep=" + pep + "&lep=" + o.lep + "&ver=" + o.ver + "&probe=1" +
                            (password.empty() ? std::string() : "&pw=" + url_encode(password));
    Online *op = &o;
    Node *self = this;
    o.worker.post([url, op, self]() -> Worker::Completion {
        const HttpResult r = http_request("POST", url, 6000);
        return [r, op, self] {
            op->probe_inflight = false;
            BrowseState &b = self->browse_;
            b.probe_busy = false;
            ++b.probe_gen;
            b.probe_ep.clear(); b.probe_lep.clear();
            if (r.status != 200) { b.probe_result = "error"; return; }
            std::string body = r.body;
            while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) body.pop_back();
            const auto t = split(body, ' ');
            b.probe_result = t.empty() ? "error" : t[0];
            if (b.probe_result == "update") b.update_to = t.size() >= 2 ? t[1] : std::string("a newer version");
            if (b.probe_result == "version") b.probe_ep = t.size() >= 2 ? t[1] : std::string();   // (the race's version)
            if (t.size() >= 3) { b.probe_ep = t[1]; b.probe_lep = t[2]; }
        };
    });
}

void Node::browse_ping(const Mac &room) {
    if (!online_) return;
    Online &o = *online_;
    o.relay_wanted = true;
    if (!o.relay || !o.relay->ready()) { o.ping_pending.insert(room); return; }
    const std::uint64_t now = now_ms();
    for (auto it = o.pings_out.begin(); it != o.pings_out.end();) it = now - it->second.second > 10000 ? o.pings_out.erase(it) : std::next(it);
    const std::uint32_t token = o.ping_token++;
    o.pings_out[token] = {room, now};
    std::uint8_t ping[8];
    std::memcpy(ping, "RRBP", 4);
    std::memcpy(ping + 4, &token, 4);
    o.relay->send(room, ping, sizeof ping);
}

void Node::set_join_target(const RoomInfo &room, const std::string &password) {
    if (!online_) return;
    online_->have_target = true;
    online_->target = room;
    online_->target_pw = password;
    online_->target_last_join = 0;
}
void Node::clear_join_target() {
    if (!online_) return;
    online_->have_target = false;
    online_->target_pw.clear();
}
void Node::set_host_options(int vis, const std::string &password) {
    if (!online_) return;
    online_->host_vis = std::clamp(vis, 0, 2);
    online_->host_pw = password.substr(0, 32);
    if (online_->host_vis == 1 && online_->host_pw.empty()) online_->host_vis = 0;
    online_->last_host = 0;   // (tell the room service at once if a room is open)
}
std::string Node::room_code() const {
    char b[8];
    std::snprintf(b, sizeof b, "%02X%02X%02X", mac_[3], mac_[4], mac_[5]);
    return b;
}
bool Node::host_accepting() const {
    for (const auto &m : matches_) if (m->started && (m->mode == kModeHost || m->mode == kModeP2P) && !m->opt.empty()) return true;
    return false;
}
// One-way trip from `src` (half its measured round trip).  Another joiner's data comes through the host: two hops, about
// the whole round trip to the host.
double Node::one_way_ms(const Mac &src) const {
    for (const auto &mp : matches_) if (auto it = mp->rtt_ms.find(src); it != mp->rtt_ms.end()) return it->second / 2.0;
    for (const auto &mp : matches_) {
        if (mp->mode != kModeClient || mp->members.empty()) continue;
        if (auto it = mp->rtt_ms.find(mp->members.begin()->first); it != mp->rtt_ms.end()) return static_cast<double>(it->second);
    }
    return 0.0;
}
bool Node::in_session() const {
    for (const auto &m : matches_) if (m->started && !m->members.empty()) return true;
    return false;
}
bool Node::session_peer(const Mac &mac) const {
    for (const auto &m : matches_) {
        if (m->members.count(mac) != 0u || m->requesters.count(mac) != 0u || m->pending.count(mac) != 0u || m->accept_opt.count(mac) != 0u) return true;
    }
    return false;
}

void Node::online_tick(std::uint64_t now) {
    Online &o = *online_;
    o.worker.drain();

    // our public address: ask two STUN servers through the game's own socket (same mapping the peers will see)
    if (o.stun_enabled && o.stun_requested) {
        if (o.stun_started == 0) o.stun_started = now;
        if (!o.pep.empty() && now - o.stun_started > 30000) {   // refresh: keeps the router's mapping warm and notices changes
            for (auto &s : o.stun) s.answered = false;
            o.stun_started = now;
        }
        const bool done = o.stun[0].answered && (o.stun[1].answered || now - o.stun_started > 3000);
        if (!done && now - o.stun_started < 15000 && now - o.last_stun >= 400) {
            o.last_stun = now;
            for (auto &s : o.stun) {
                if (!s.resolved || s.answered) continue;
                std::uint8_t req[20] = {0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42};
                std::random_device rd;
                for (int i = 0; i < 12; ++i) { if (s.tx[i] == 0) s.tx[i] = static_cast<std::uint8_t>(rd()); req[8 + i] = s.tx[i]; }
                ::sendto(static_cast<SockT>(socket_), reinterpret_cast<const char *>(req), 20, 0, reinterpret_cast<const sockaddr *>(&s.addr), sizeof s.addr);
            }
        }
        if (o.stun[0].answered && o.pep.empty()) {
            o.pep = ep_str(o.stun[0].mapped);
            nlog("public address " + o.pep);
        }
        if (o.stun[0].answered && o.stun[1].answered) {
            const bool sym = o.stun[0].mapped.sin_port != o.stun[1].mapped.sin_port;
            if (sym != o.symmetric) {
                o.symmetric = sym;
                if (sym) nlog("this router gives a different port per destination (symmetric NAT): direct connections may fail");
            }
        }
    }
    const bool addr_ready = !o.stun_enabled || !o.pep.empty() || (o.stun_started != 0 && now - o.stun_started > 4000);
    const std::string use_pep = o.pep.empty() ? o.lep : o.pep;

    // punching
    for (auto &[m, p] : o.peers) {
        if (p.heard || p.dead || p.relay) continue;
        static const bool no_relay = std::getenv("RR_NET_NO_RELAY") != nullptr;
        // With the relay available, 6 s without a reply is enough (the game's lobby gives up long before 30 s).
        const bool give_up = no_relay ? now > p.punch_until : now + kPunchWindowMs > p.punch_until + 6000;
        if (give_up) {
            if (!no_relay) {   // the other side gives up at about the same time and does the same
                p.relay = true;
                o.relay_wanted = true;
                nlog("no direct connection to peer " + mac_hex(m) + " (router may block it): switching to the relay");
            } else {
                p.dead = true;
                nlog("giving up on peer " + mac_hex(m) + " (no reply; router may block direct connections)");
            }
            continue;
        }
        if (now - p.last_punch < 250) continue;
        p.last_punch = now;
        std::uint8_t pkt[kHeader] = {};
        std::memcpy(pkt, "RRN1", 4);
        pkt[4] = kPunch;
        std::memcpy(pkt + 8, mac_.data(), 6);
        std::memcpy(pkt + 14, m.data(), 6);
        for (const auto &c : p.cands) ::sendto(static_cast<SockT>(socket_), reinterpret_cast<const char *>(pkt), static_cast<int>(kHeader), 0, reinterpret_cast<const sockaddr *>(&c), sizeof c);
    }

    Online *op = &o;
    Node *self = this;
    // host: when the game closes entries (the host started a race), players who asked to join but never got in are
    // dropped: otherwise they would receive the race's packets (the game broadcasts them to every known peer) and, if they
    // host a room of their own later, those packets would arrive in their lobby.
    const bool accepting = o.hosting && host_accepting();
    if (o.hosting && o.host_was_accepting && !accepting) {
        for (auto it = o.peers.begin(); it != o.peers.end();) {
            if (!session_peer(it->first)) { nlog("race started: dropping " + mac_hex(it->first) + " (not in the race)"); it = o.peers.erase(it); }
            else ++it;
        }
        o.last_host = 0;   // (tell the room service at once: the room shows as racing)
    }
    if (accepting != o.host_was_accepting) {
        if (accepting && o.hosting) { o.ignored_joiners.clear(); o.last_host = 0; }
        o.host_was_accepting = accepting;
    }
    // host: keep the room listed and learn who wants to join
    if (o.hosting && !o.host_inflight && addr_ready && (o.last_host == 0 || now - o.last_host >= 3000)) {   // (0 = at once)
        o.last_host = now;
        o.host_inflight = true;
        int players = 1;
        int maxp = 8;
        std::string info;
        for (const auto &m : matches_) if (m->mode == kModeHost || m->mode == kModeP2P) {
            players = 1 + static_cast<int>(m->members.size()); maxp = m->max;
            info = hex_bytes(reinterpret_cast<const char *>(m->adv_opt.data()), static_cast<int>(std::min<std::size_t>(m->adv_opt.size(), 64)));
        }
        std::string url = o.server + "/host?mac=" + mac_hex(mac_) + "&ep=" + use_pep + "&lep=" + o.lep + "&name=" + o.name_hex + "&ver=" + o.ver +
                          "&n=" + std::to_string(players) + "&max=" + std::to_string(maxp) + "&info=" + info + "&vis=" + std::to_string(o.host_vis) +
                          "&open=" + (accepting ? "1" : "0");
        if (o.host_vis == 1 && !o.host_pw.empty()) url += "&pw=" + url_encode(o.host_pw);
        o.worker.post([url, op, self]() -> Worker::Completion {
            const HttpResult r = http_request("POST", url, 5000);
            return [r, op, self] {
                op->host_inflight = false;
                if (r.status != 200) {
                    if (!op->unreachable_logged) { op->unreachable_logged = true; nlog("room service not reachable (status " + std::to_string(r.status) + ")"); }
                    return;
                }
                op->unreachable_logged = false;
                if (update_reply(r.body, self->browse_.update_to)) {
                    if (!op->update_logged) { op->update_logged = true; nlog("room service: this game version (" + op->ver + ") is out of date, newest is " + self->browse_.update_to + ": the room is not listed"); }
                    return;
                }
                if (!op->hosting) return;
                const auto t_now = self->now_ms();
                for (const auto &line : split(r.body, '\n')) {
                    const auto t = split(line, ' ');
                    if (t.size() < 4 || t[0] != "joiner") continue;
                    Mac m{};
                    if (!parse_mac(t[1], m)) continue;
                    if (!self->host_accepting() && !self->session_peer(m)) {   // a race is on: the game would not take them
                        if (op->ignored_joiners.insert(m).second) nlog("player " + t[1] + " asked to join during the race: not now");
                        continue;
                    }
                    bool fresh = false;
                    add_peer(*op, op->peers, m, t[2], t[3], t_now, fresh);
                    if (fresh) {
                        nlog("player " + t[1] + " wants to join: " + (op->peers[m].relay ? std::string("through the relay") : "punching toward " + t[2] + " / " + t[3]));
                        for (auto &mp : self->matches_) mp->last_hello = 0;   // say hello right away
                    }
                }
            };
        });
    }
    // client with a room picked in the server browser: ask to join that room (again every 20 s while the game waits: the
    // room service forgets join requests after a minute)
    // (not once we are in: then the game talks to the host directly and the room service has nothing to add)
    if (o.client && o.have_target && addr_ready && !o.list_inflight && !session_peer(o.target.mac) &&
        (o.target_last_join == 0 || now - o.target_last_join >= 20000)) {
        o.target_last_join = now;
        o.list_inflight = true;
        bool fresh = false;
        add_peer(o, o.peers, o.target.mac, o.target.ep, o.target.lep, now, fresh);
        if (fresh) nlog("joining room " + mac_hex(o.target.mac) + " (picked in the server browser)");
        const Mac tm = o.target.mac;
        const std::string url = o.server + "/join?room=" + mac_hex(tm) + "&mac=" + mac_hex(mac_) + "&ep=" + use_pep + "&lep=" + o.lep + "&ver=" + o.ver +
                                (o.target_pw.empty() ? std::string() : "&pw=" + url_encode(o.target_pw));
        o.worker.post([url, op, self, tm]() -> Worker::Completion {
            const HttpResult r = http_request("POST", url, 5000);
            return [r, op, self, tm] {
                op->list_inflight = false;
                if (!op->client) return;
                const auto t = split(r.body, ' ');
                if (r.status == 200 && t.size() >= 3 && t[0] == "ok") {
                    bool fresh2 = false;
                    std::string lep = t[2];
                    while (!lep.empty() && (lep.back() == '\n' || lep.back() == '\r')) lep.pop_back();
                    add_peer(*op, op->peers, tm, t[1], lep, self->now_ms(), fresh2);
                } else {
                    nlog("join request for room " + mac_hex(tm) + ": " + (r.status == 200 ? r.body : "no answer (status " + std::to_string(r.status) + ")"));
                }
            };
        });
    }
    // client: find rooms of our group and ask to join them
    if (o.client && !o.have_target && !o.list_inflight && (o.last_list == 0 || now - o.last_list >= 4000)) {   // (0 = at once)
        o.last_list = now;
        o.list_inflight = true;
        const std::string url = o.server + "/rooms?ver=" + o.ver + "&name=" + o.name_hex;
        o.worker.post([url, op, self, use_pep]() -> Worker::Completion {
            const HttpResult r = http_request("GET", url, 5000);
            std::vector<std::string> joins;   // POSTs to make after the list arrives
            return [r, op, self, use_pep] {
                op->list_inflight = false;
                if (r.status != 200) {
                    if (!op->unreachable_logged) { op->unreachable_logged = true; nlog("room service not reachable (status " + std::to_string(r.status) + ")"); }
                    return;
                }
                op->unreachable_logged = false;
                if (!op->client) return;
                const auto t_now = self->now_ms();
                for (const auto &line : split(r.body, '\n')) {
                    const auto t = split(line, ' ');
                    if (t.size() < 7 || t[0] != "room") continue;
                    Mac m{};
                    if (!parse_mac(t[1], m) || m == self->mac_) continue;
                    if (op->joined.count(m) != 0) continue;
                    op->joined.insert(m);
                    bool fresh = false;
                    add_peer(*op, op->peers, m, t[2], t[3], t_now, fresh);
                    nlog("found room " + t[1] + " (" + t[5] + "/" + t[6] + " players): asking to join");
                    const std::string url2 = op->server + "/join?room=" + t[1] + "&mac=" + mac_hex(self->mac_) + "&ep=" + use_pep + "&lep=" + op->lep;
                    op->worker.post([url2]() -> Worker::Completion { http_request("POST", url2, 5000); return nullptr; });
                }
            };
        });
    }
}

#else   // no sockets

struct Node::Online {};
void Node::browse_refresh() {}
void Node::browse_lookup(const std::string &) {}
void Node::browse_probe(const Mac &, const std::string &) {}
void Node::browse_ping(const Mac &) {}
void Node::set_join_target(const RoomInfo &, const std::string &) {}
void Node::clear_join_target() {}
void Node::set_host_options(int, const std::string &) {}
std::string Node::room_code() const { return "000000"; }

bool Node::start_online(const std::string &, int) { return false; }
void Node::online_begin_host() {}
void Node::online_begin_client() {}
void Node::online_end() {}
void Node::online_scan() {}
void Node::online_need_stun() {}
void Node::online_tick(std::uint64_t) {}
void Node::online_stun(const std::uint8_t *, std::size_t) {}
void Node::online_send(const std::uint8_t *, std::size_t, const Mac &) {}
void Node::online_relay_tick(std::uint64_t) {}
bool Node::online_accept_relay(const Mac &) { return false; }
bool Node::online_accept(const Mac &, const void *) { return false; }
void set_logger(void (*)(const std::string &)) {}
void set_log_file(const std::string &) {}
static void nlog_hook(const std::string &) {}

#endif

namespace {
int g_online_request = -1;   // -1 = not asked, 0 = off, 1 = on
}
void request_online(bool on) { g_online_request = on ? 1 : 0; }

bool networking_enabled() {
    if (g_online_request == 1 && RR_DEFAULT_ROOM_SERVER[0] != '\0') return true;
    if (g_online_request == 0) return false;
    const char *e = std::getenv("RR_NET");
    return e != nullptr && e[0] != '\0' && std::strcmp(e, "0") != 0;
}


Node *shared_node() {
    static std::unique_ptr<Node> node;
    static bool tried = false;
    if (!tried) {
        tried = true;
        if (networking_enabled()) {
            auto n = std::make_unique<Node>();
            const char *e = std::getenv("RR_NET");
            const bool env_set = e != nullptr && e[0] != '\0' && std::strcmp(e, "0") != 0;
            const bool online = (env_set && std::strcmp(e, "online") == 0) || (!env_set && g_online_request == 1);
            if (online) {
                const char *srv = std::getenv("RR_NET_SERVER");
                const char *port = std::getenv("RR_NET_PORT");
                if (n->start_online(srv != nullptr && srv[0] != '\0' ? srv : RR_DEFAULT_ROOM_SERVER, port != nullptr ? std::atoi(port) : 0)) node = std::move(n);
            } else {
                const int forced = e != nullptr ? std::atoi(e) : 0;
                if (n->start(forced > 1 ? forced : 0)) node = std::move(n);
                nlog_hook(node ? "same-PC test mode (RR_NET=1): listening on 127.0.0.1:" + std::to_string(node->port())
                               : std::string("same-PC test mode (RR_NET=1): could not open a UDP port in 47100-47131"));
            }
        } else {
            nlog_hook("networking is off: Wireless Battle will report a communication error (start with --online, or set RR_NET=1 for the same-PC test)");
        }
    }
    return node.get();
}

} // namespace rr::net
