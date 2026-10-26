# nettest - online lobby test without the game

`nodetest ROLE SERVER SECONDS` drives one network node (host/rr_net.cpp) through Wireless Battle's lobby the way the
game does (host opens a room / client searches, sees the host's hello, asks to join, host accepts), then both send a
datagram every 50 ms and report what arrived.

    g++ -std=c++20 -O1 -pthread -I profiles/ridgeracer/host tools/nettest/nodetest.cpp profiles/ridgeracer/host/rr_net.cpp \
        profiles/ridgeracer/host/rr_http.cpp profiles/ridgeracer/host/rr_relay.cpp -o nodetest
    (cd online/worker && npx wrangler dev --local --port 8787)          # the room service, locally
    RR_NET_STUN=off ./nodetest host http://127.0.0.1:8787 20 &  RR_NET_STUN=off ./nodetest client http://127.0.0.1:8787 19

RR_NET_FORCE_RELAY=1 makes a node behave like a browser player (relay only); RR_NET_SIM_LOSS=100 on both blocks all
direct packets (tests the switch to the relay); RR_NET_TRACE=1 logs the handshake packets.
