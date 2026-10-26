#pragma once

// Server browser and room setup pages for internet play, drawn in the style of the port's on-screen keyboard (the game's
// own Options look).  Pure logic + a CPU renderer, no network or platform code: the caller (rr_hle.cpp) feeds it the room
// list and input once a frame and carries out what it asks for (refresh, look up a room code, check a join, ...).
//
//  - Browser: opened while the game shows "Searching for a race open for entries..."; lists the rooms with host, course,
//    class, handicap, players, ping and a lock for password rooms; search, filters, sorting, refresh, join, direct connect
//    by room code.
//  - Room setup: opened while the game shows "Announcing race..."; who may join (anyone / password / room code only).

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace rr {

struct LobbyRoom {
    std::array<std::uint8_t, 6> id{};
    std::string host;                    // the host's player name
    int course{0}, cls{0}, handicap{0};  // course 1..24, class 1..4, handicap 0..3 (0 = off)
    int players{1}, max{8};
    int vis{0};                          // 0 open, 1 password, 2 unlisted (found by its code)
    int ping_ms{-1};                     // measured through the relay (-1 = not yet)
    int est_ms{-1};                      // estimated from the two locations (-1 = unknown)
    bool racing{false};                  // a race is on: entries open again when it is over (players can wait for that)
};
// Fills host / course / class / handicap from the game's 28-byte room description.
void lobby_decode_info(const std::vector<std::uint8_t> &info, LobbyRoom &room);
const char *lobby_course_name(int course);
std::string lobby_room_code(const std::array<std::uint8_t, 6> &id);   // last 6 hex digits, upper case

struct LobbyInput {
    std::vector<char16_t> typed;   // characters typed since the last frame (8 backspace, 13 Enter, 27 Esc)
    std::uint32_t pad{0};          // PSP button bits from the keyboard and gamepad (menu navigation)
    std::uint32_t gamepad{0};      // PSP button bits from the gamepad only (while typing text, letters must not be buttons)
    bool mouse_inside{false};      // pointer over the picture; position in 480x272 page units
    float mouse_x{0.0f}, mouse_y{0.0f};
    int clicks{0};                 // left-button presses since the last frame
    int wheel{0};                  // wheel steps since the last frame (+ = away from the user)
};

enum class LobbyRequestKind {
    None,
    Refresh,     // fetch the room list again
    Lookup,      // find the room with code `text`
    Probe,       // check whether room `id` can be joined with password `text` (answer: lobby_ui_probe_result)
    Joined,      // done: join room `id` with password `text`
    Cancelled,   // done: the player backed out of the browser
    OpenRoom,    // done (room setup): open the room for `vis` (0 anyone, 1 password `text`, 2 room code only)
};
struct LobbyRequest {
    LobbyRequestKind kind{LobbyRequestKind::None};
    std::array<std::uint8_t, 6> id{};
    std::string text;
    int vis{0};
};

void lobby_ui_begin_browser();
void lobby_ui_begin_host(const LobbyRoom &room, const std::string &code);
[[nodiscard]] bool lobby_ui_active();
void lobby_ui_end();
// Room list for the browser page; status 200 = list arrived, 0 = still loading, anything else = error (HTTP status, -1 = no answer).
// `fresh` = a new answer from the room service (false when only the pings changed).
void lobby_ui_set_rooms(const std::vector<LobbyRoom> &rooms, int status, bool fresh = true);
void lobby_ui_lookup_result(bool found, const LobbyRoom &room, int status, const std::string &other_version = {});
// The room service refuses this game version: `newest` = the version to get (empty = all fine).  Shown on both pages.
void lobby_ui_set_update(const std::string &newest);
void lobby_ui_probe_result(const std::string &result);   // "ok", "badpw", "gone", "full", "racing", "busy", "update", "version <v>", "error"
// Once a frame while active.
LobbyRequest lobby_ui_update(const LobbyInput &in);
// What happened since the last call, for menu sounds (same bits as the keyboard: kOskMove / Insert / Delete / Accept / Cancel).
[[nodiscard]] std::uint32_t lobby_ui_take_events();
// Draws the current page into a w x h RGBA image (opaque).
void lobby_ui_render(std::uint8_t *rgba, int w, int h);
// Changes whenever the picture would change.
[[nodiscard]] std::uint64_t lobby_ui_state_key();

// Whichever full-screen page of the port is open (server browser or on-screen keyboard): drawing and its change key.
void ui_page_render(std::uint8_t *rgba, int w, int h);
[[nodiscard]] std::uint64_t ui_page_state_key();

// Persisted between runs by the caller (optional): room setup choices.
struct LobbyHostPrefs { int vis{0}; std::string password; };
LobbyHostPrefs &lobby_host_prefs();

} // namespace rr
