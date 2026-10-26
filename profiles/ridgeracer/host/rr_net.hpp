// Network layer for the game's wireless multiplayer ("Wireless Battle"): an emulation of the PSP's ad-hoc networking
// (group scan/connect, datagrams, and the lobby library) on top of UDP, so two copies of the port can find each other
// and race.  This first stage links copies that run on the same PC (loopback); the internet transport plugs in below
// the Transport functions later.  The game-facing glue (reading arguments from the PSP's registers, calling the game's
// handlers) lives in rr_hle.cpp; this file knows nothing about the PSP's memory, so it can be tested on its own.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace rr::net {

using Mac = std::array<std::uint8_t, 6>;
inline constexpr Mac kBroadcast{0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

// Events the game's handlers are told about (numbers are the PSP's own).
enum CtlEvent : int { kCtlError = 0, kCtlConnected = 1, kCtlDisconnected = 2, kCtlScan = 3 };
enum MatchEventKind : int {
    kMatchHello = 1, kMatchRequest = 2, kMatchLeave = 3, kMatchDeny = 4, kMatchCancel = 5, kMatchAccept = 6,
    kMatchEstablished = 7, kMatchTimeout = 8, kMatchError = 9, kMatchBye = 10, kMatchData = 11
};
enum MatchMode : int { kModeHost = 1, kModeClient = 2, kModeP2P = 3 };

struct ScanEntry {
    char name[8]{};
    Mac bssid{};
    std::uint32_t channel{11};
};

struct MatchEvent {
    int id{};
    int event{};
    Mac mac{};
    std::vector<std::uint8_t> opt;
};

struct Member { Mac mac{}; };

// Server browser (internet mode): the room list with each room's description, and the steps to join one.
struct RoomInfo {
    Mac mac{};
    std::string ep, lep;                 // the host's addresses ("-" when the room service withholds them: password rooms)
    char name[8]{};                      // the group the host's game created
    int players{1}, max{8};
    int vis{0};                          // 0 listed, 1 listed with a password, 2 not listed (joined by room code)
    std::vector<std::uint8_t> info;      // the game's 28-byte room description (host name, course, class, handicap)
    int est_ms{-1};                      // round trip estimated by the room service from the two locations (-1 = unknown)
    bool racing{false};                  // the host's game has closed entries (a race is on); it opens again afterwards
};
struct BrowseState {
    // room list (browse_refresh)
    int list_gen{0};                     // bumped whenever a list (or an error) arrives
    bool list_busy{false};
    int list_status{0};                  // HTTP status of the last list (0 = none yet, -1 = no answer)
    std::vector<RoomInfo> rooms;
    std::map<Mac, int> ping_ms;          // measured round trips through the relay
    // one room by its code (browse_lookup)
    int lookup_gen{0};
    bool lookup_busy{false}, lookup_found{false};
    int lookup_status{0};
    RoomInfo lookup;
    // join check (browse_probe): "ok", "badpw", "gone", "full", "busy", "racing", "update", "version", "error"
    int probe_gen{0};
    bool probe_busy{false};
    std::string probe_result, probe_ep, probe_lep;
    // the room service said this game version is out of date: the newest version's name (empty = fine)
    std::string update_to;
    // a room code that belongs to a race on another game version: that version (browse_lookup)
    std::string lookup_other_version;
};

class Node {
public:
    Node();
    ~Node();
    Node(const Node &) = delete;
    Node &operator=(const Node &) = delete;

    // Opens the UDP socket.  `port` 0 = pick the first free port of the local range.  Returns false if it cannot.
    bool start(int port = 0);
    // Internet mode: the same node, but its peers come from the online room-list service (`server` = its base URL) and
    // the connections are made by UDP hole punching.  Returns false if the socket cannot open.
    bool start_online(const std::string &server, int port = 0);
    bool online() const { return online_ != nullptr; }
    void stop();
    bool running() const { return socket_ != -1; }
    int port() const { return port_; }
    Mac mac() const { return mac_; }

    // Drains incoming packets and runs the timers; call often (every frame is enough).
    void poll();

    // ---- ad-hoc control: groups ("rooms" on the wireless network) ----
    void ctl_scan();                                     // result arrives as kCtlScan after about a second
    std::vector<ScanEntry> ctl_scan_results() const;
    void ctl_connect(const char name[8]);                // result arrives as kCtlConnected
    void ctl_disconnect();                               // result arrives as kCtlDisconnected
    bool ctl_connected() const { return connected_; }
    bool next_ctl_event(int &event);

    // ---- datagrams (PDP) ----
    int pdp_create(unsigned port);                       // id >= 1, or -1
    void pdp_delete(int id);
    int pdp_send(int id, const Mac &dst, unsigned dst_port, const void *data, std::size_t len);   // 0 ok
    // 0 = got one (mac/port/len filled in); -1 = nothing waiting.  `capacity` is the size of buf.
    int pdp_recv(int id, Mac &src, unsigned &src_port, void *buf, std::size_t capacity, std::size_t &len);

    // ---- matching (the lobby: hello / request / accept) ----
    int match_create(int mode, int max_members, unsigned port, std::uint32_t hello_us, std::uint32_t keepalive_us);
    int match_start(int id, const void *opt, std::size_t opt_len);
    void match_set_hello_opt(int id, const void *opt, std::size_t opt_len);
    void match_select_target(int id, const Mac &target, const void *opt, std::size_t opt_len);
    void match_cancel_target(int id, const Mac &target, const void *opt, std::size_t opt_len);
    std::vector<Member> match_members(int id) const;     // the local player first, then the others
    void match_stop(int id);
    void match_delete(int id);
    bool next_match_event(MatchEvent &event);

    // ---- server browser (internet mode only; results arrive in browse() over the following frames) ----
    void browse_refresh();                                     // fetch the room list
    void browse_lookup(const std::string &code);               // one room by its 6-digit code (also unlisted rooms)
    void browse_probe(const Mac &room, const std::string &password);   // can we join? (checks password / room full)
    void browse_ping(const Mac &room);                         // measure the round trip through the relay
    const BrowseState &browse() const { return browse_; }
    // The client's matching then joins only this room (instead of every room of the group), with this password.
    void set_join_target(const RoomInfo &room, const std::string &password);
    void clear_join_target();
    // Who may join the next room this node opens: 0 anyone (listed), 1 with `password` (listed), 2 by room code only.
    void set_host_options(int vis, const std::string &password);
    std::string room_code() const;                              // our room code: the last 6 hex digits of our id
    // Hosting and the game accepts entries (its hello message is not empty; the game empties it when the host starts a race).
    bool host_accepting() const;
    // In a game session with at least one other player (lobby or race).
    bool in_session() const;
    double one_way_ms(const Mac &src) const;
    // `mac` is part of our current game session (a member, or in the middle of joining).
    bool session_peer(const Mac &mac) const;
    // The game's group scan, held open while the player picks a room (the game keeps showing "Searching..."), then
    // finished with what to report.
    void ctl_scan_hold();
    void ctl_scan_finish(const std::vector<ScanEntry> &found);

    struct Online;   // internet-mode state (defined in rr_net.cpp)

private:
    struct Packet;
    struct Matching;
    struct Pdp {
        unsigned port{};
        std::deque<std::pair<std::pair<Mac, unsigned>, std::vector<std::uint8_t>>> queue;
        // jitter buffer: datagrams waiting for their playout time (ms, our clock), released into `queue` in order
        std::deque<std::pair<std::uint64_t, std::pair<std::pair<Mac, unsigned>, std::vector<std::uint8_t>>>> held;
    };
    // Jitter buffer per sender: the game's datagrams carry the sender's clock, and are handed to the game spaced as they
    // were sent (plus a small delay sized to the recent jitter) instead of as they arrived.  RR_NET_JITTERBUF=0 turns it off.
    struct Playout { std::int64_t d[64]{}; int n{0}, pos{0}; std::uint64_t last_release{0};
                     bool have_prev{false}; std::uint32_t prev_sent{0}; double prev_yaw{0.0}; std::int64_t base{0};
                     double age_ema{-1.0}; std::uint64_t age_at{0}; };
    std::map<Mac, Playout> playout_;
    void pdp_release(std::uint64_t now);
    // Prediction (RR_NET_PREDICT, default on): the race packets of the other player are moved forward in time before the
    // game reads them (position by the packet's velocity, heading by its turn rate), by the age of the packet plus the
    // game's own display delay (RR_NET_PREDICT_FRAMES frames, default 8), so the other car is shown where it is now.
    double frame_ms_{1000.0 / 59.94};
    void predict(std::vector<std::uint8_t> &data, const Mac &src, std::uint32_t sent, std::uint64_t now);
public:
    void set_frame_ms(double ms) { frame_ms_ = ms; }
    // How old the other players' race data is when the game gets it (ms, recent average: one-way trip estimated as half
    // the round trip, plus the time it waited in the jitter buffer).  0 when unknown.
    double data_age_ms() const;
private:
    std::uint64_t pdp_playout_time(const Mac &src, std::uint32_t sent, std::uint64_t now);

    void send_packet(int type, const Mac &dst, unsigned port, const void *payload, std::size_t len);
    void receive(const std::uint8_t *buf, std::size_t got, const void *from_addr);   // one datagram that arrived
    static bool parse_packet(const std::uint8_t *buf, std::size_t got, Packet &p, std::size_t &total);
    // More than two players online: joiners reach only the host, which passes on what they send each other (kFwd).
    bool host_member(const Mac &mac) const;            // a joiner of a room we host
    const Mac *joined_host() const;                    // the host of the room we joined (nullptr: none)
    void forward_between_joiners(const Mac &from, const Mac &dst, const std::uint8_t *raw, std::size_t len);
    void handle_forwarded(const Packet &outer);
    // Relay (browser players, routers that block direct connections): see rr_relay.hpp.  While a relayed datagram is
    // handed to receive(), relay_src_ names the sender the relay vouched for (from_addr is unused then).
    const Mac *relay_src_{nullptr};
    void online_relay_tick(std::uint64_t now);
    bool online_accept_relay(const Mac &src);
    // Bad-connection simulator (tests): RR_NET_SIM_DELAY / _JITTER (ms), _LOSS / _DUP (percent), _SPIKE="every_s,ms[,first_s]".
    // It works on what arrives at this copy, so set it on both copies for both directions.
    struct Sim {
        bool on{false};
        int delay_ms{0}, jitter_ms{0}, spike_every_ms{0}, spike_ms{0}, spike_start_ms{0};
        double loss{0.0}, dup{0.0};
        std::uint64_t last_due{0}, last_report{0};
        std::uint64_t passed{0}, dropped{0}, duplicated{0}, held_in_spike{0};
        std::uint32_t rng{0x12345678u};
    };
    struct Held { std::uint64_t due; std::vector<std::uint8_t> data; std::array<std::uint8_t, 16> from; };
    void sim_setup();
    void sim_arrived(const std::uint8_t *buf, std::size_t got, const void *from_addr);
    void sim_release(std::uint64_t now);
    void handle(const Packet &p);
    void handle_match(Matching &m, const Packet &p);
    Matching *find_match(int id);
    const Matching *find_match(int id) const;
    Matching *match_for_port(unsigned port);
    void beacon();
    void online_begin_host();
    void online_begin_client();
    void online_end();
    void online_tick(std::uint64_t now);
    void online_send(const std::uint8_t *buf, std::size_t len, const Mac &dst);
    bool online_accept(const Mac &src, const void *from_addr);
    void online_scan();
    void online_need_stun();
    void online_stun(const std::uint8_t *buf, std::size_t len);
    std::uint64_t now_ms() const;

    std::intptr_t socket_{-1};
    int port_{0};
    Mac mac_{};
    std::uint64_t t0_{};
    std::map<Mac, int> peer_port_;                       // mac -> udp port, learned from what arrives
    // control
    bool connected_{false};
    char group_[8]{};
    std::uint64_t last_beacon_{0};
    bool scanning_{false};
    std::uint64_t scan_until_{0};
    std::vector<ScanEntry> scan_found_, scan_result_;
    std::deque<int> ctl_events_;
    // pdp
    std::map<int, Pdp> pdp_;
    int next_pdp_{1};
    // matching
    std::vector<std::unique_ptr<Matching>> matches_;
    int next_match_{1};
    std::deque<MatchEvent> match_events_;
    std::unique_ptr<Online> online_;
    BrowseState browse_;
    Sim sim_;
    std::deque<Held> held_;                              // simulator: datagrams waiting for their delivery time
};

// Where the node logs to (the game's log file); optional.
void set_logger(void (*fn)(const std::string &));
// Also write the node's messages (connection steps, public address, round-trip times) to this text file; empty = off.
void set_log_file(const std::string &path);

// The process-wide node the game talks to (null until the game first needs the network and networking is enabled).
Node *shared_node();
// Turns internet play on or off for this run (the --online / --no-online options, fed from the settings file).  On only
// takes effect when a room service address is built in (rr_online_config.hpp).  Call before the game first uses the network.
void request_online(bool on);
bool networking_enabled();   // RR_NET=1 (or a port number) = same-PC test mode; RR_NET=online = internet mode (RR_NET_SERVER = room service URL)

} // namespace rr::net
