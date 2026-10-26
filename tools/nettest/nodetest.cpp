// nodetest ROLE SERVER SECONDS: drives one network node through the lobby like the game does, then exchanges datagrams.
#include "rr_net.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
using namespace rr::net;
static std::string hx(const Mac &m) { char b[16]; std::snprintf(b, sizeof b, "%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3], m[4], m[5]); return b; }
int main(int argc, char **argv) {
    const bool host = std::string(argv[1]) == "host";
    const double secs = std::atof(argv[3]);
    set_logger([](const std::string &s) { std::printf("  [net] %s\n", s.c_str()); std::fflush(stdout); });
    Node n;
    if (!n.start_online(argv[2])) { std::puts("start_online failed"); return 1; }
    const char group[8] = {'R', 'R', 'T', 'E', 'S', 'T', 0, 0};
    const auto t0 = std::chrono::steady_clock::now();
    auto now_s = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    int match = -1, pdp = -1, stage = 0; Mac peer{}; bool asked = false;
    int sent = 0, got = 0; double last_send = 0, first_data = -1, joined_at = -1;
    if (host) { n.ctl_connect(group); } else { n.ctl_scan(); }
    while (now_s() < secs) {
        n.poll();
        int ev;
        while (n.next_ctl_event(ev)) {
            std::printf("%.2fs ctl event %d\n", now_s(), ev);
            if (ev == kCtlScan && !host) {
                auto r = n.ctl_scan_results();
                std::printf("%.2fs scan: %zu group(s)\n", now_s(), r.size());
                if (r.empty()) n.ctl_scan(); else n.ctl_connect(r[0].name);
            }
            if (ev == kCtlConnected) {
                match = n.match_create(host ? kModeHost : kModeClient, 8, 1, 1000000, 5000000);
                n.match_start(match, host ? "H" : "C", 1);
                pdp = n.pdp_create(0x3039);
            }
        }
        MatchEvent me;
        while (n.next_match_event(me)) {
            std::printf("%.2fs match event %d from %s\n", now_s(), me.event, hx(me.mac).c_str());
            if (host && me.event == kMatchRequest) n.match_select_target(match, me.mac, "ok", 2);
            if (!host && me.event == kMatchHello && !asked) { asked = true; n.match_select_target(match, me.mac, "join", 4); }
            if (me.event == kMatchEstablished || me.event == kMatchAccept) { if (joined_at < 0) joined_at = now_s(); peer = me.mac; stage = 1; }
        }
        if (stage == 1 && pdp > 0 && now_s() - last_send >= 0.05) {
            last_send = now_s();
            char msg[32]; std::snprintf(msg, sizeof msg, "%s %d", host ? "H" : "C", sent++);
            n.pdp_send(pdp, peer, 0x3039, msg, std::strlen(msg));
        }
        if (pdp > 0) {
            Mac src; unsigned sp; char buf[256]; std::size_t len;
            while (n.pdp_recv(pdp, src, sp, buf, sizeof buf, len) == 0) { if (first_data < 0) first_data = now_s(); ++got; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::printf("RESULT %s: joined at %.2fs, first data %.2fs, sent %d, received %d\n", host ? "host" : "client", joined_at, first_data, sent, got);
    return got > 0 ? 0 : 2;
}
