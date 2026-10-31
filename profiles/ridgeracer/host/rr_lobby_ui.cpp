#include "rr_lobby_ui.hpp"
#include "rr_osk_ui.hpp"
#include "rr_ui_canvas.hpp"
#include "rr_pad_ui.hpp"
#include "rr_version.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace rr {
namespace {

using ui::Canvas; using ui::Rgb; using ui::rgb; using ui::mix; using ui::clamp01;

constexpr std::uint32_t kSelect = 0x0001u, kStart = 0x0008u, kUp = 0x0010u, kRight = 0x0020u, kDown = 0x0040u, kLeft = 0x0080u,
                        kL = 0x0100u, kR = 0x0200u, kTriangle = 0x1000u, kCircle = 0x2000u, kCross = 0x4000u, kSquare = 0x8000u;
constexpr std::uint32_t kDirs = kUp | kRight | kDown | kLeft;

const char *const kCourses[25] = {
    "?",
    "Seaside Route 765", "Ridge City Highway", "Sunset Drive", "Crystal Coast Highway", "Union Hill District", "Lakeside Parkway",
    "Crimsonrock Pass", "Diablo Canyon Road", "Midtown Expressway", "Downtown Rave City", "Greenpeak Highlands", "Silvercreek Dam",
    "Seaside Route 765 R", "Ridge City Highway R", "Sunset Drive R", "Crystal Coast Highway R", "Union Hill District R", "Lakeside Parkway R",
    "Crimsonrock Pass R", "Diablo Canyon Road R", "Midtown Expressway R", "Downtown Rave City R", "Greenpeak Highlands R", "Silvercreek Dam R",
};

// ---- colours (the game's Options screen)
const Rgb kInk = rgb(58, 58, 62), kFaint = rgb(206, 206, 210), kMid = rgb(130, 130, 136), kWhite = rgb(255, 255, 255);
const Rgb kRedTop = rgb(236, 28, 36), kRedBot = rgb(190, 0, 10);
const Rgb kPillTop = rgb(102, 102, 106), kPillBot = rgb(60, 60, 64);

// ---- widgets (ids double as focus targets)
enum Wid : int {
    W_NONE = 0,
    W_SEARCH, W_CLASS, W_FULL, W_LOCKED, W_SORT, W_REFRESH,   // toolbar
    W_COL_HOST, W_COL_COURSE, W_COL_CLASS, W_COL_HCAP, W_COL_PLAYERS, W_COL_PING,
    W_JOIN, W_DIRECT, W_BACK,                                 // bottom buttons
    W_SCROLL,
    W_VIS0, W_VIS1, W_VIS2, W_PASSWORD, W_OPEN,               // room setup
    W_BUSY_CANCEL,
    W_ROW = 1000,                                             // + visible row index
};
struct Widget { int id; float x, y, w, h; };

enum class Page { None, Browser, Host };
enum class Modal { None, Text, Busy };
enum class TextFor { Search, JoinPassword, Code, HostPassword };
enum class BusyFor { Probe, Lookup, Queue };
enum Sort : int { kSortPing, kSortPlayers, kSortHost, kSortCourse, kSortClass, kSortHandicap, kSortCount };
const char *const kSortNames[kSortCount] = {"Ping", "Players", "Host", "Course", "Class", "Handicap"};

constexpr float kListY = 76.0f, kRowH = 17.0f;
constexpr int kRowsVisible = 8;

struct State {
    Page page{Page::None};
    Modal modal{Modal::None};
    TextFor text_for{TextFor::Search};
    BusyFor busy_for{BusyFor::Probe};
    std::string busy_text;
    std::uint32_t frames{0}, prev_pad{0}, held{0}, events{0};
    bool armed{false};
    // pointer
    float mx{-1}, my{-1};
    bool mouse_in{false};
    int hover{W_NONE};
    std::uint32_t last_click_frame{0};
    int last_click_id{W_NONE};
    // toast
    std::string toast;
    std::uint32_t toast_until{0};
    // browser
    std::vector<LobbyRoom> rooms;
    std::vector<int> view;               // indices into rooms, filtered + sorted
    int status{0};                       // list status (see lobby_ui_set_rooms)
    bool have_list{false};
    std::uint32_t list_frame{0};         // frame the last list arrived
    std::uint32_t refresh_at{0};         // next automatic refresh
    int sel{0}, top{0};                  // selected row (index into view) and first visible row
    std::array<std::uint8_t, 6> sel_id{};
    bool sel_valid{false};
    int focus{W_ROW};                    // focused widget (rows: W_ROW)
    bool user_moved{false};              // the player has moved the focus (then it is not changed for them)
    std::u16string search;
    int class_filter{0};                 // 0 any, 1..4
    bool show_full{true}, show_locked{true};
    int sort{kSortPing};
    bool sort_desc{false};
    // join in progress
    LobbyRoom pending{};
    std::string pending_pw;
    std::uint32_t pending_seq{0};
    // waiting for a race in progress to end (BusyFor::Queue): the room is checked every few seconds
    std::uint32_t queue_since{0}, queue_next{0};
    std::uint32_t queue_gone_since{0};   // frame the room was first missing (0 = it is there): hosts re-open after "Select"
    std::string queue_note;              // second line of the waiting box
    // room setup
    LobbyRoom host_room{};
    std::string code;
    int host_focus{W_OPEN};
    // result handed out by update()
    LobbyRequest out{};
} g;

LobbyHostPrefs g_prefs;
std::array<std::uint8_t, 6> g_last_joined{};   // the room joined last (preselected when the browser opens again)
bool g_have_last_joined{false};

std::string lower(std::string s) { for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }
std::string narrow(const std::u16string &s) { std::string o; for (char16_t c : s) o.push_back(c < 128 ? static_cast<char>(c) : '?'); return o; }
std::u16string widen(const std::string &s) { std::u16string o; for (unsigned char c : s) o.push_back(c); return o; }

int ping_of(const LobbyRoom &r) { return r.ping_ms >= 0 ? r.ping_ms : (r.est_ms >= 0 ? r.est_ms : 99999); }

void toast(const std::string &t) { g.toast = t; g.toast_until = g.frames + 200u; }

void rebuild_view() {
    g.view.clear();
    const std::string q = lower(narrow(g.search));
    for (int i = 0; i < static_cast<int>(g.rooms.size()); ++i) {
        const LobbyRoom &r = g.rooms[static_cast<std::size_t>(i)];
        if (g.class_filter != 0 && r.cls != g.class_filter) continue;
        if (!g.show_full && r.players >= r.max) continue;
        if (!g.show_locked && r.vis == 1) continue;
        if (!q.empty()) {
            const std::string hay = lower(r.host) + "\n" + lower(lobby_course_name(r.course)) + "\n" + lower(lobby_room_code(r.id));
            if (hay.find(q) == std::string::npos) continue;
        }
        g.view.push_back(i);
    }
    const auto key = [](const LobbyRoom &r, int s) -> long long {
        switch (s) {
        case kSortPing: return ping_of(r);
        case kSortPlayers: return -(static_cast<long long>(r.players) * 16 + (r.max - r.players));   // fullest first, then most room left
        case kSortClass: return r.cls;
        case kSortHandicap: return r.handicap;
        default: return 0;
        }
    };
    const auto cmp = [&](int a, int b) {   // strict weak order, ascending
        const LobbyRoom &ra = g.rooms[static_cast<std::size_t>(a)], &rb = g.rooms[static_cast<std::size_t>(b)];
        if (g.sort == kSortHost) { const std::string ha = lower(ra.host), hb = lower(rb.host); if (ha != hb) return ha < hb; }
        else if (g.sort == kSortCourse) { if (ra.course != rb.course) return ra.course < rb.course; }
        else { const long long ka = key(ra, g.sort), kb = key(rb, g.sort); if (ka != kb) return ka < kb; }
        return ping_of(ra) < ping_of(rb);
    };
    std::stable_sort(g.view.begin(), g.view.end(), [&](int a, int b) { return g.sort_desc ? cmp(b, a) : cmp(a, b); });
    // keep the selected room selected
    g.sel = 0;
    if (!g.sel_valid && !g.user_moved && g_have_last_joined) {   // back from a race: the room just played in comes first
        for (int i : g.view) if (g.rooms[static_cast<std::size_t>(i)].id == g_last_joined) { g.sel_id = g_last_joined; g.sel_valid = true; break; }
    }
    if (g.sel_valid) for (int i = 0; i < static_cast<int>(g.view.size()); ++i) if (g.rooms[static_cast<std::size_t>(g.view[static_cast<std::size_t>(i)])].id == g.sel_id) { g.sel = i; break; }
    g.sel = std::clamp(g.sel, 0, std::max(0, static_cast<int>(g.view.size()) - 1));
    // an empty list has no row to show the focus on: put it on Refresh (and back on the list once races appear)
    if (g.view.empty() && g.focus == W_ROW) g.focus = W_REFRESH;
    else if (!g.view.empty() && g.focus == W_REFRESH && !g.user_moved) g.focus = W_ROW;
    g.top = std::clamp(g.top, 0, std::max(0, static_cast<int>(g.view.size()) - kRowsVisible));
    if (g.sel < g.top) g.top = g.sel;
    if (g.sel >= g.top + kRowsVisible) g.top = g.sel - kRowsVisible + 1;
}

void select_row(int i) {
    if (g.view.empty()) return;
    g.sel = std::clamp(i, 0, static_cast<int>(g.view.size()) - 1);
    g.sel_id = g.rooms[static_cast<std::size_t>(g.view[static_cast<std::size_t>(g.sel)])].id;
    g.sel_valid = true;
    if (g.sel < g.top) g.top = g.sel;
    if (g.sel >= g.top + kRowsVisible) g.top = g.sel - kRowsVisible + 1;
}

// ---- layout
void layout(std::vector<Widget> &ws) {
    ws.clear();
    if (g.page == Page::Browser) {
        const float y = 41.0f, h = 17.0f;
        ws.push_back({W_SEARCH, 8, y, 96, h});
        ws.push_back({W_CLASS, 108, y, 62, h});
        ws.push_back({W_FULL, 174, y, 62, h});
        ws.push_back({W_LOCKED, 240, y, 78, h});
        ws.push_back({W_SORT, 322, y, 92, h});
        ws.push_back({W_REFRESH, 418, y, 54, h});
        ws.push_back({W_COL_HOST, 20, 62, 82, 12});
        ws.push_back({W_COL_COURSE, 102, 62, 156, 12});
        ws.push_back({W_COL_CLASS, 258, 62, 36, 12});
        ws.push_back({W_COL_HCAP, 294, 62, 60, 12});
        ws.push_back({W_COL_PLAYERS, 354, 62, 52, 12});
        ws.push_back({W_COL_PING, 406, 62, 62, 12});
        const int n = std::min(kRowsVisible, static_cast<int>(g.view.size()) - g.top);
        for (int i = 0; i < n; ++i) ws.push_back({W_ROW + i, 8, kListY + i * kRowH, 458, kRowH});
        if (static_cast<int>(g.view.size()) > kRowsVisible) ws.push_back({W_SCROLL, 468, kListY, 6, kRowH * kRowsVisible});
        ws.push_back({W_JOIN, 8, 217, 84, 20});
        ws.push_back({W_DIRECT, 96, 217, 124, 20});
        ws.push_back({W_BACK, 224, 217, 70, 20});
    } else if (g.page == Page::Host) {
        ws.push_back({W_VIS0, 40, 124, 124, 20});
        ws.push_back({W_VIS1, 170, 124, 124, 20});
        ws.push_back({W_VIS2, 300, 124, 140, 20});
        ws.push_back({W_PASSWORD, 130, 154, 220, 20});
        ws.push_back({W_OPEN, 170, 212, 140, 22});
    }
    if (g.modal == Modal::Busy) ws.push_back({W_BUSY_CANCEL, 200, 156, 80, 18});
}
int hit(const std::vector<Widget> &ws, float x, float y) {
    for (auto it = ws.rbegin(); it != ws.rend(); ++it) if (x >= it->x && x < it->x + it->w && y >= it->y && y < it->y + it->h) return it->id;
    return W_NONE;
}

// ---- actions
void finish(LobbyRequestKind k) { g.out.kind = k; }

void open_text(TextFor f) {
    g.modal = Modal::Text;
    g.text_for = f;
    switch (f) {
    case TextFor::Search: osk_ui_begin(u"SEARCH", g.search, 20u); osk_ui_set_texts("SEARCH", "Type part of a host name, course or room code"); break;
    case TextFor::JoinPassword: osk_ui_begin(u"PASSWORD", u"", 16u); osk_ui_set_texts("JOIN A RACE", "Type the race's password"); break;
    case TextFor::Code: osk_ui_begin(u"ROOM CODE", u"", 6u); osk_ui_set_texts("DIRECT CONNECT", "Type the 6-character room code (0-9, A-F)"); break;
    case TextFor::HostPassword: osk_ui_begin(u"PASSWORD", widen(g_prefs.password), 16u); osk_ui_set_texts("HOST A RACE", "Choose a password for your race"); break;
    }
}

void start_probe(const LobbyRoom &r, const std::string &pw) {
    g.pending = r;
    g.pending_pw = pw;
    ++g.pending_seq;
    g.modal = Modal::Busy;
    g.busy_for = BusyFor::Probe;
    g.busy_text = "Joining " + (r.host.empty() ? std::string("the") : r.host + "'s") + " race...";
    g.out = LobbyRequest{LobbyRequestKind::Probe, r.id, pw, 0};
}

std::string queue_race_text(const LobbyRoom &r) {
    std::string t = r.host.empty() ? std::string("the host") : r.host;
    if (r.course > 0) t += " - " + std::string(lobby_course_name(r.course));
    return t;
}

void start_queue(const LobbyRoom &r, const std::string &pw) {
    g.pending = r;
    g.pending_pw = pw;
    ++g.pending_seq;
    g.modal = Modal::Busy;
    g.busy_for = BusyFor::Queue;
    g.busy_text = "Waiting: " + queue_race_text(r);
    g.queue_note = "You will join as soon as it takes entries again.";
    g.queue_since = g.frames;
    g.queue_next = g.frames + 180u;
    g.queue_gone_since = 0u;
}

void try_join(const LobbyRoom &r) {
    if (r.players >= r.max && !r.racing) { toast("That race is full."); g.events |= kOskCancel; return; }
    g.pending = r;
    g.events |= kOskAccept;
    if (r.vis == 1) open_text(TextFor::JoinPassword);   // (the password is checked first, also for a race in progress)
    else if (r.racing) start_queue(r, "");
    else start_probe(r, "");
}

void join_selected() {
    if (g.view.empty()) { toast("Pick a race first (or use Direct connect)."); g.events |= kOskCancel; return; }
    try_join(g.rooms[static_cast<std::size_t>(g.view[static_cast<std::size_t>(g.sel)])]);
}

void refresh() {
    g.out = LobbyRequest{LobbyRequestKind::Refresh};
    g.refresh_at = g.frames + 360u;
}

void sort_by(int s) {
    if (g.sort == s) g.sort_desc = !g.sort_desc;
    else { g.sort = s; g.sort_desc = false; }
    rebuild_view();
}

void activate(int id) {
    switch (id) {
    case W_SEARCH: g.events |= kOskAccept; open_text(TextFor::Search); break;
    case W_CLASS: g.class_filter = (g.class_filter + 1) % 5; g.events |= kOskMove; rebuild_view(); break;
    case W_FULL: g.show_full = !g.show_full; g.events |= kOskMove; rebuild_view(); break;
    case W_LOCKED: g.show_locked = !g.show_locked; g.events |= kOskMove; rebuild_view(); break;
    case W_SORT: g.sort = (g.sort + 1) % kSortCount; g.sort_desc = false; g.events |= kOskMove; rebuild_view(); break;
    case W_REFRESH: g.events |= kOskAccept; refresh(); break;
    case W_COL_HOST: g.events |= kOskMove; sort_by(kSortHost); break;
    case W_COL_COURSE: g.events |= kOskMove; sort_by(kSortCourse); break;
    case W_COL_CLASS: g.events |= kOskMove; sort_by(kSortClass); break;
    case W_COL_HCAP: g.events |= kOskMove; sort_by(kSortHandicap); break;
    case W_COL_PLAYERS: g.events |= kOskMove; sort_by(kSortPlayers); break;
    case W_COL_PING: g.events |= kOskMove; sort_by(kSortPing); break;
    case W_JOIN: join_selected(); break;
    case W_DIRECT: g.events |= kOskAccept; open_text(TextFor::Code); break;
    case W_BACK: g.events |= kOskCancel; finish(LobbyRequestKind::Cancelled); break;
    case W_VIS0: case W_VIS1: case W_VIS2:
        g_prefs.vis = id - W_VIS0; g.events |= kOskMove;
        if (g_prefs.vis == 1 && g_prefs.password.empty()) open_text(TextFor::HostPassword);
        break;
    case W_PASSWORD: if (g_prefs.vis == 1) { g.events |= kOskAccept; open_text(TextFor::HostPassword); } break;
    case W_OPEN:
        if (g_prefs.vis == 1 && g_prefs.password.empty()) { open_text(TextFor::HostPassword); break; }
        g.events |= kOskAccept;
        g.out = LobbyRequest{LobbyRequestKind::OpenRoom, {}, g_prefs.vis == 1 ? g_prefs.password : std::string(), g_prefs.vis};
        break;
    case W_BUSY_CANCEL: g.events |= kOskCancel; g.modal = Modal::None; ++g.pending_seq; break;
    default:
        if (id >= W_ROW) {
            select_row(g.top + (id - W_ROW));
            join_selected();
        }
        break;
    }
}

void text_done(bool accepted) {
    const std::string t = narrow(osk_ui_text());
    osk_ui_end();
    g.modal = Modal::None;
    switch (g.text_for) {
    case TextFor::Search:
        if (accepted) { g.search = widen(t); rebuild_view(); }
        break;
    case TextFor::JoinPassword:
        if (accepted) start_probe(g.pending, t);
        break;
    case TextFor::Code: {
        if (!accepted) break;
        std::string c;
        for (char ch : t) if (std::isxdigit(static_cast<unsigned char>(ch))) c.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
        if (c.size() != 6) { toast("A room code has 6 characters (0-9, A-F)."); break; }
        g.modal = Modal::Busy;
        g.busy_for = BusyFor::Lookup;
        g.busy_text = "Looking for room " + c + "...";
        ++g.pending_seq;
        g.out = LobbyRequest{LobbyRequestKind::Lookup, {}, c, 0};
        break;
    }
    case TextFor::HostPassword:
        if (accepted) g_prefs.password = t;
        if (g_prefs.password.empty() && g_prefs.vis == 1) g_prefs.vis = 0;
        break;
    }
}

// Pad navigation on the browser page.  Zones: toolbar (top), rows, bottom buttons.
void browser_nav(std::uint32_t nav, std::uint32_t pressed) {
    static const int toolbar[] = {W_SEARCH, W_CLASS, W_FULL, W_LOCKED, W_SORT, W_REFRESH};
    static const int buttons[] = {W_JOIN, W_DIRECT, W_BACK};
    const auto idx_of = [](const int *arr, int n, int id) { for (int i = 0; i < n; ++i) if (arr[i] == id) return i; return -1; };
    const int ti = idx_of(toolbar, 6, g.focus), bi = idx_of(buttons, 3, g.focus);
    const int nrows = static_cast<int>(g.view.size());
    if (nav & kDirs) { g.events |= kOskMove; g.user_moved = true; }
    if (ti >= 0) {
        if (nav & kLeft) g.focus = toolbar[(ti + 5) % 6];
        if (nav & kRight) g.focus = toolbar[(ti + 1) % 6];
        if (nav & kDown) g.focus = nrows > 0 ? W_ROW : W_JOIN;
        if (nav & kUp) g.focus = W_JOIN;
    } else if (bi >= 0) {
        if (nav & kLeft) g.focus = buttons[(bi + 2) % 3];
        if (nav & kRight) g.focus = buttons[(bi + 1) % 3];
        if (nav & kUp) g.focus = nrows > 0 ? W_ROW : W_SEARCH;
        if (nav & kDown) g.focus = W_SEARCH;
    } else {   // rows
        if (nav & kUp) { if (g.sel == 0 || nrows == 0) g.focus = W_SEARCH; else select_row(g.sel - 1); }
        if (nav & kDown) { if (g.sel >= nrows - 1) g.focus = W_JOIN; else select_row(g.sel + 1); }
        if (nav & kLeft) select_row(g.sel - kRowsVisible);
        if (nav & kRight) select_row(g.sel + kRowsVisible);
    }
    // (Start acts like A: Enter on a keyboard is Start, and it should press whatever is focused)
    if (pressed & (kCross | kStart)) activate(g.focus == W_ROW ? (g.view.empty() ? W_NONE : W_JOIN) : g.focus);
    if (pressed & kCircle) activate(W_BACK);
    if (pressed & kTriangle) activate(W_REFRESH);
    if (pressed & kSquare) activate(W_SEARCH);
    if (pressed & kSelect) activate(W_DIRECT);
    if (pressed & kL) { g.sort = (g.sort + kSortCount - 1) % kSortCount; g.sort_desc = false; g.events |= kOskMove; rebuild_view(); }
    if (pressed & kR) { g.sort = (g.sort + 1) % kSortCount; g.sort_desc = false; g.events |= kOskMove; rebuild_view(); }
}

void host_nav(std::uint32_t nav, std::uint32_t pressed) {
    if (nav & kDirs) g.events |= kOskMove;
    const bool in_vis = g.host_focus >= W_VIS0 && g.host_focus <= W_VIS2;
    if (in_vis) {
        if (nav & kLeft) g.host_focus = W_VIS0 + (g.host_focus - W_VIS0 + 2) % 3;
        if (nav & kRight) g.host_focus = W_VIS0 + (g.host_focus - W_VIS0 + 1) % 3;
        if (nav & kDown) g.host_focus = g_prefs.vis == 1 ? W_PASSWORD : W_OPEN;
        if (nav & kUp) g.host_focus = W_OPEN;
    } else if (g.host_focus == W_PASSWORD) {
        if (nav & kUp) g.host_focus = W_VIS0 + g_prefs.vis;
        if (nav & kDown) g.host_focus = W_OPEN;
    } else {
        if (nav & kUp) g.host_focus = g_prefs.vis == 1 ? W_PASSWORD : W_VIS0 + g_prefs.vis;
        if (nav & kDown) g.host_focus = W_VIS0 + g_prefs.vis;
    }
    if (pressed & kCross) activate(g.host_focus);
    if (pressed & kStart) activate(W_OPEN);
}

} // namespace

// ================= public =================

void lobby_decode_info(const std::vector<std::uint8_t> &info, LobbyRoom &r) {
    if (info.size() < 19) return;
    r.host.clear();
    for (int i = 0; i < 10 && info[static_cast<std::size_t>(i)] != 0; ++i) {
        const unsigned char c = info[static_cast<std::size_t>(i)];
        r.host.push_back(c >= 32 && c < 127 ? static_cast<char>(c) : '?');
    }
    r.course = info[11];
    r.handicap = info[18] & 3;
    r.cls = (info[18] >> 2) & 7;
}
const char *lobby_course_name(int course) { return (course >= 1 && course <= 24) ? kCourses[course] : "Unknown course"; }
std::string lobby_room_code(const std::array<std::uint8_t, 6> &id) {
    char b[8];
    std::snprintf(b, sizeof b, "%02X%02X%02X", id[3], id[4], id[5]);
    return b;
}
LobbyHostPrefs &lobby_host_prefs() { return g_prefs; }

void lobby_ui_begin_browser() {
    const auto keep_search = g.search; const int keep_class = g.class_filter, keep_sort = g.sort;
    const bool keep_full = g.show_full, keep_locked = g.show_locked, keep_desc = g.sort_desc;
    g = State{};
    g.search = keep_search; g.class_filter = keep_class; g.sort = keep_sort; g.show_full = keep_full; g.show_locked = keep_locked; g.sort_desc = keep_desc;
    g.page = Page::Browser;
    g.focus = W_ROW;
    g.refresh_at = 0u;   // (first list at once)
}
void lobby_ui_begin_host(const LobbyRoom &room, const std::string &code) {
    const auto keep_search = g.search;
    g = State{};
    g.search = keep_search;
    g.page = Page::Host;
    g.host_room = room;
    g.code = code;
    g.host_focus = W_OPEN;
}
bool lobby_ui_active() { return g.page != Page::None; }
void lobby_ui_end() {
    if (g.modal == Modal::Text) osk_ui_end();
    g.page = Page::None;
    g.modal = Modal::None;
}
std::uint32_t lobby_ui_take_events() { const std::uint32_t e = g.events; g.events = 0u; return e; }

void lobby_ui_set_rooms(const std::vector<LobbyRoom> &rooms, int status, bool fresh) {
    if (status == 200) {
        if (fresh || !g.have_list) g.list_frame = g.frames;   // ("updated N s ago" counts from the last answer, not from ping updates)
        g.have_list = true;
        g.rooms = rooms;
    }
    if (status != 0) g.status = status;
    rebuild_view();
}

namespace { std::string g_update_to; }   // (kept across pages: the room service refuses this game version)
void lobby_ui_set_update(const std::string &newest) { g_update_to = newest; }


void lobby_ui_lookup_result(bool found, const LobbyRoom &room, int status, const std::string &other_version) {
    if (g.modal == Modal::Busy && g.busy_for == BusyFor::Queue) {   // waiting in line: follow the room as the host changes it
        if (status != 200) { lobby_ui_probe_result("error"); return; }
        if (!other_version.empty()) { g.modal = Modal::None; toast("That race is on another version of the game (" + other_version + ")."); g.events |= kOskCancel; return; }
        if (!found) { lobby_ui_probe_result("gone"); return; }
        g.queue_gone_since = 0u;
        const bool changed = room.course != g.pending.course || room.cls != g.pending.cls || room.handicap != g.pending.handicap || room.host != g.pending.host;
        g.pending = room;
        g.busy_text = "Waiting: " + queue_race_text(room);
        if (changed) g.events |= kOskMove;
        if (room.racing) { g.queue_note = "Race in progress. You join when the host returns to the lobby."; return; }
        if (room.players >= room.max) { g.queue_note = "The race is full: waiting for a free place."; return; }
        g.queue_note = "Joining: " + queue_race_text(room);
        g.out = LobbyRequest{LobbyRequestKind::Probe, room.id, g.pending_pw, 0};
        return;
    }
    if (g.modal != Modal::Busy || g.busy_for != BusyFor::Lookup) return;
    g.modal = Modal::None;
    if (status != 200) { toast("Could not reach the room service."); g.events |= kOskCancel; return; }
    if (!g_update_to.empty()) { toast("Your game is out of date: get " + g_update_to + " to play online."); g.events |= kOskCancel; return; }
    if (!other_version.empty()) { toast("That race is on another version of the game (" + other_version + ")."); g.events |= kOskCancel; return; }
    if (!found) { toast("No race with that room code right now."); g.events |= kOskCancel; return; }
    try_join(room);
}

void lobby_ui_probe_result(const std::string &result) {
    if (g.modal != Modal::Busy || (g.busy_for != BusyFor::Probe && g.busy_for != BusyFor::Queue)) return;
    if (g.busy_for == BusyFor::Queue || result == "racing") {   // waiting for a race to end
        // After a race the host may pick "Select": its game closes the room and opens a new one a little later (same room
        // code), so a missing room is only given up on after a minute.
        if (g.busy_for == BusyFor::Queue && result == "gone") {
            if (g.queue_gone_since == 0u) g.queue_gone_since = g.frames;
            if (g.frames - g.queue_gone_since < 3600u) { g.queue_note = "The host is setting up the next race..."; return; }
        }
        if (result != "gone") g.queue_gone_since = 0u;
        if (result == "racing" || result == "full" || result == "busy" || result == "error") {
            if (g.busy_for != BusyFor::Queue) { start_queue(g.pending, g.pending_pw); g.events |= kOskMove; return; }
            g.queue_note = result == "racing" ? "Race in progress. You join when the host returns to the lobby."
                         : result == "full" ? "The race is full: waiting for a free place."
                         : "Can't reach the room service right now: still trying.";
            return;
        }
    }
    g.modal = Modal::None;
    if (result == "ok") {
        g.out = LobbyRequest{LobbyRequestKind::Joined, g.pending.id, g.pending_pw, 0};
        g.events |= kOskAccept;
        g_last_joined = g.pending.id; g_have_last_joined = true;
        return;
    }
    g.events |= kOskCancel;
    if (result == "update") toast("Your game is out of date: get " + (g_update_to.empty() ? std::string("the newest version") : g_update_to) + " to play online.");
    else if (result.rfind("version", 0) == 0) toast("That race is on another version of the game" + (result.size() > 8 ? " (" + result.substr(8) + ")." : std::string(".")));
    else if (result == "badpw") toast("Wrong password.");
    else if (result == "full") toast("That race is full.");
    else if (result == "gone") { toast("That race has closed."); refresh(); }
    else toast("Could not reach the room service.");
}

namespace {
void update_impl(const LobbyInput &in);
}
LobbyRequest lobby_ui_update(const LobbyInput &in) {
    // (results that arrived since the last frame - lobby_ui_probe_result and friends - may already have set g.out)
    if (g.page != Page::None) update_impl(in);
    const LobbyRequest r = g.out;
    g.out = LobbyRequest{};
    return r;
}
namespace {
void update_impl(const LobbyInput &in) {
    ++g.frames;

    if (g.modal == Modal::Text) {
        // (letters typed on a keyboard must not count as buttons, but its arrow keys move around the letter grid)
        const OskState st = osk_ui_update(in.typed, in.gamepad | (in.pad & kDirs));
        g.events |= osk_ui_take_events();
        if (st != OskState::Active) text_done(st == OskState::Accepted);
        g.prev_pad = in.pad;
        g.armed = false;   // (the button that closed the keyboard must be released before the page reacts)
        return;
    }

    // pointer
    std::vector<Widget> ws;
    layout(ws);
    // (the first position seen is only remembered: a pointer resting over a button must not grab the focus when the page opens)
    const bool moved = in.mouse_inside && g.mouse_in && (std::fabs(in.mouse_x - g.mx) > 0.01f || std::fabs(in.mouse_y - g.my) > 0.01f);
    g.mouse_in = in.mouse_inside;
    g.mx = in.mouse_x; g.my = in.mouse_y;
    g.hover = in.mouse_inside ? hit(ws, g.mx, g.my) : W_NONE;
    if (moved && g.hover != W_NONE && g.modal == Modal::None) {
        if (g.page == Page::Browser && g.hover != W_SCROLL && !(g.hover >= W_COL_HOST && g.hover <= W_COL_PING)) g.focus = g.hover >= W_ROW ? W_ROW : g.hover;
        if (g.page == Page::Host) g.host_focus = g.hover;
    }
    if (in.wheel != 0 && g.page == Page::Browser && g.modal == Modal::None) {
        const int maxtop = std::max(0, static_cast<int>(g.view.size()) - kRowsVisible);
        g.top = std::clamp(g.top - in.wheel * 2, 0, maxtop);
        if (g.sel < g.top) select_row(g.top);
        if (g.sel >= g.top + kRowsVisible) select_row(g.top + kRowsVisible - 1);
    }
    for (int c = 0; c < in.clicks; ++c) {
        const int id = g.hover;
        if (g.modal == Modal::Busy) { if (id == W_BUSY_CANCEL) activate(id); continue; }
        if (id >= W_ROW) {
            const int row = g.top + (id - W_ROW);
            const bool dbl = g.last_click_id == id && g.frames - g.last_click_frame < 30u && g.sel == row;
            select_row(row);
            g.focus = W_ROW;
            g.events |= kOskMove;
            if (dbl) join_selected();
            g.last_click_id = id; g.last_click_frame = g.frames;
        } else if (id == W_SCROLL) {
            const float t = (g.my - kListY) / (kRowH * kRowsVisible);
            const int maxtop = std::max(0, static_cast<int>(g.view.size()) - kRowsVisible);
            g.top = std::clamp(static_cast<int>(std::lround(t * maxtop)), 0, maxtop);
        } else if (id != W_NONE) {
            activate(id);
        }
        if (g.out.kind != LobbyRequestKind::None && g.out.kind != LobbyRequestKind::Refresh) break;
    }

    // keyboard: Esc backs out (cancels a wait first)
    for (char16_t c : in.typed) {
        if (c != 27) continue;
        if (g.modal == Modal::Busy) activate(W_BUSY_CANCEL);
        else if (g.page == Page::Browser) activate(W_BACK);
    }

    // pad
    if (!g.armed) {
        if (in.pad == 0u) g.armed = true;
        g.prev_pad = in.pad;
    } else {
        const std::uint32_t pressed = in.pad & ~g.prev_pad;
        std::uint32_t nav = pressed;
        if (in.pad & kDirs) {
            ++g.held;
            if (g.held > 20u && (g.held % 5u) == 0u) nav |= in.pad & kDirs;   // auto-repeat
        } else {
            g.held = 0u;
        }
        g.prev_pad = in.pad;
        if (g.modal == Modal::Busy) {
            if (pressed & (kCircle | kSelect)) activate(W_BUSY_CANCEL);
        } else if (g.page == Page::Browser) {
            browser_nav(nav, pressed);
        } else if (g.page == Page::Host) {
            host_nav(nav, pressed);
        }
    }

    // waiting for a race to end: ask again every 3 s
    if (g.modal == Modal::Busy && g.busy_for == BusyFor::Queue && g.out.kind == LobbyRequestKind::None && g.frames >= g.queue_next) {
        g.queue_next = g.frames + 180u;
        g.out = LobbyRequest{LobbyRequestKind::Lookup, g.pending.id, lobby_room_code(g.pending.id), 0};   // (live room info first, then the join check)
    }
    // automatic refresh while the list is on screen
    if (g.page == Page::Browser && g.modal == Modal::None && g.out.kind == LobbyRequestKind::None && g.frames >= g.refresh_at) refresh();
    return;
}
}   // namespace

std::uint64_t lobby_ui_state_key() {
    std::uint64_t k = 1469598103934665603ull;
    const auto mixin = [&](std::uint64_t v) { k = (k ^ v) * 1099511628211ull; };
    if (g.modal == Modal::Text) return osk_ui_state_key() ^ 0x5a5a5a5aull;
    mixin(static_cast<std::uint64_t>(g.page)); mixin(static_cast<std::uint64_t>(g.modal)); mixin(pad::generation()); mixin(g_update_to.size());
    mixin(static_cast<std::uint64_t>(g.focus)); mixin(static_cast<std::uint64_t>(g.host_focus)); mixin(static_cast<std::uint64_t>(g.hover));
    mixin(static_cast<std::uint64_t>(g.sel)); mixin(static_cast<std::uint64_t>(g.top)); mixin(static_cast<std::uint64_t>(g.status));
    mixin(static_cast<std::uint64_t>(g.class_filter)); mixin(g.show_full ? 1u : 0u); mixin(g.show_locked ? 1u : 0u);
    mixin(static_cast<std::uint64_t>(g.sort)); mixin(g.sort_desc ? 1u : 0u); mixin(static_cast<std::uint64_t>(g_prefs.vis));
    mixin(g_prefs.password.size()); mixin(g.frames < g.toast_until ? 1u : 0u);
    for (char c : g.toast) mixin(static_cast<unsigned char>(c));
    for (char16_t c : g.search) mixin(c);
    for (const auto &r : g.rooms) { for (auto b : r.id) mixin(b); mixin(static_cast<std::uint64_t>(r.players)); mixin(static_cast<std::uint64_t>(r.ping_ms + 7)); mixin(static_cast<std::uint64_t>(r.est_ms + 7)); mixin(static_cast<std::uint64_t>(r.course)); }
    mixin((g.frames - g.list_frame) / 60u);   // "updated N s ago"
    if (g.modal == Modal::Busy || (!g.have_list && g.page == Page::Browser)) mixin(g.frames / 3u);   // spinner
    for (char c : g.queue_note) mixin(static_cast<unsigned char>(c));
    if (g.modal == Modal::Busy && g.busy_for == BusyFor::Queue) mixin((g.frames - g.queue_since) / 60u);
    for (const auto &r : g.rooms) mixin(r.racing ? 3u : 4u);
    return k;
}

// ================= drawing =================
namespace {

void page_background(const Canvas &cv) {
    cv.fill_all([&](float x, float y) {
        float v = 255.0f;
        v -= 52.0f * clamp01(1.0f - y / 62.0f) * clamp01(1.0f - y / 62.0f);   // top fade
        v -= 30.0f * clamp01(1.0f - x / 80.0f);                                // left fade
        const float mx = std::fmod(x + 7.0f, 30.0f), my = std::fmod(y + 11.0f, 30.0f);
        const bool plus = (std::fabs(mx - 15.0f) < 0.45f && std::fabs(my - 15.0f) < 2.2f) || (std::fabs(my - 15.0f) < 0.45f && std::fabs(mx - 15.0f) < 2.2f);
        if (plus) v -= 16.0f;
        return Rgb{v, v, v};
    });
}
void header(const Canvas &cv, const char *small, const char *big) {
    cv.text(10.0f, 19.0f, small, 11.0f, kInk);
    cv.text(470.0f - cv.text_width(std::string(big), 26.0f), 34.0f, big, 26.0f, kFaint);
}
// Info bar whose first line shows the buttons as pictures of the controller in use (or the keyboard keys).
void info_bar(const Canvas &cv, const std::vector<ui::Hint> &hints, const char *l2) {
    cv.rrect(0.0f, 241.0f, 480.0f, 40.0f, 0.0f, [&](float, float v, float &) { return mix(rgb(176, 176, 180), rgb(136, 136, 140), v); });
    ui::pad_hints(cv, 10.0f, 251.5f, hints, 10.0f, 9.5f, kWhite, pad::glyphs());
    cv.text(12.0f, 268.0f, l2, 10.0f, kWhite);
}
void red_pill(const Canvas &cv, float x, float y, float w, float h) {
    cv.vgrad(x, y, w, h, h * 0.5f, kRedTop, kRedBot);
    cv.rrect(x + 1.5f, y + 0.8f, w - 3.0f, h * 0.45f, h * 0.22f, [&](float, float, float &a) { a = 0.20f; return kWhite; });   // gloss
}
void grey_pill(const Canvas &cv, float x, float y, float w, float h, float alpha = 1.0f) { cv.vgrad(x, y, w, h, h * 0.5f, kPillTop, kPillBot, alpha); }
// Capsule button: red when focused, dark grey otherwise; light grey outline when the pointer is over it.
void button(const Canvas &cv, const Widget &wd, const std::string &label, bool focused, bool hovered, float em = 10.0f, bool enabled = true) {
    if (focused) red_pill(cv, wd.x, wd.y, wd.w, wd.h);
    else grey_pill(cv, wd.x, wd.y, wd.w, wd.h, enabled ? 1.0f : 0.45f);
    if (hovered && !focused) cv.frame(wd.x - 1.0f, wd.y - 1.0f, wd.w + 2.0f, wd.h + 2.0f, wd.h * 0.5f + 1.0f, 1.2f, kRedTop, 0.8f);
    const std::string t = cv.fit(label, em, wd.w - 10.0f);
    cv.text_centered(wd.x + wd.w * 0.5f, wd.y + wd.h * 0.5f + em * 0.36f, t, em, kWhite, enabled ? 1.0f : 0.6f);
}
void lock_icon(const Canvas &cv, float x, float y, Rgb c, float alpha = 1.0f) {   // (x, y) = top-left of a ~8x10 box
    cv.ring(x + 4.0f, y + 4.0f, 2.4f, 1.2f, c, alpha, y + 4.5f);
    cv.solid(x + 0.8f, y + 4.2f, 6.4f, 5.2f, 1.0f, c, alpha);
}
void key_icon(const Canvas &cv, float x, float y, Rgb c, float alpha = 1.0f) {     // unlisted room (joined by code)
    cv.ring(x + 2.6f, y + 4.8f, 2.0f, 1.2f, c, alpha);
    cv.solid(x + 4.2f, y + 4.2f, 4.6f, 1.3f, 0.5f, c, alpha);
    cv.solid(x + 7.0f, y + 4.2f, 1.3f, 3.0f, 0.4f, c, alpha);
}
void stars(const Canvas &cv, float x, float base, int n, Rgb c) {
    if (n <= 0) { cv.text(x, base, "OFF", 9.5f, c); return; }
    for (int i = 0; i < n; ++i) cv.star(x + 4.5f + i * 9.5f, base - 3.6f, 4.2f, c);
}
void spinner(const Canvas &cv, float cx, float cy, float r) {
    const float a0 = static_cast<float>(g.frames / 3u % 12u) * 0.5236f;
    for (int i = 0; i < 12; ++i) {
        const float a = a0 + i * 0.5236f;
        cv.circle(cx + std::cos(a) * r, cy + std::sin(a) * r, 1.6f, kRedTop, 0.12f + 0.07f * i);
    }
}
void sort_arrow(const Canvas &cv, float x, float y, bool desc) {
    const float xs[3] = {x, x + 5.0f, x + 2.5f};
    const float ys_up[3] = {y + 4.0f, y + 4.0f, y}, ys_dn[3] = {y, y, y + 4.0f};
    cv.polygon(xs, desc ? ys_dn : ys_up, 3, kRedTop);
}

void render_browser(const Canvas &cv) {
    page_background(cv);
    header(cv, "WIRELESS BATTLE  -  ONLINE", "FIND A RACE");
    std::vector<Widget> ws;
    layout(ws);
    const auto find = [&](int id) { for (const auto &w : ws) if (w.id == id) return w; return Widget{W_NONE, 0, 0, 0, 0}; };
    const bool modal = g.modal != Modal::None;
    const auto focused = [&](int id) { return !modal && g.focus == id; };
    const auto hovered = [&](int id) { return !modal && g.hover == id; };

    // toolbar
    {
        const Widget s = find(W_SEARCH);
        const bool f = focused(W_SEARCH);
        if (f) red_pill(cv, s.x, s.y, s.w, s.h);
        else { cv.solid(s.x - 1, s.y - 1, s.w + 2, s.h + 2, s.h * 0.5f + 1, rgb(210, 210, 214)); cv.vgrad(s.x, s.y, s.w, s.h, s.h * 0.5f, rgb(96, 96, 100), rgb(52, 52, 56)); }
        if (hovered(W_SEARCH) && !f) cv.frame(s.x - 1, s.y - 1, s.w + 2, s.h + 2, s.h * 0.5f + 1, 1.2f, kRedTop, 0.8f);
        // magnifier
        cv.ring(s.x + 10.0f, s.y + 7.6f, 3.2f, 1.3f, kWhite);
        { const float xs[4] = {s.x + 12.0f, s.x + 13.0f, s.x + 16.4f, s.x + 15.4f}, ys[4] = {s.y + 10.6f, s.y + 9.6f, s.y + 13.0f, s.y + 14.0f}; cv.polygon(xs, ys, 4, kWhite); }
        const std::string q = narrow(g.search);
        if (q.empty()) cv.text(s.x + 20.0f, s.y + 12.2f, "Search", 10.0f, kWhite, 0.55f);
        else cv.text(s.x + 20.0f, s.y + 12.2f, cv.fit(q, 10.0f, s.w - 28.0f), 10.0f, kWhite);
    }
    {
        char b[32];
        std::snprintf(b, sizeof b, g.class_filter == 0 ? "Class: Any" : "Class: %d", g.class_filter);
        button(cv, find(W_CLASS), b, focused(W_CLASS), hovered(W_CLASS));
        button(cv, find(W_FULL), g.show_full ? "Full: Show" : "Full: Hide", focused(W_FULL), hovered(W_FULL));
        button(cv, find(W_LOCKED), g.show_locked ? "Locked: Show" : "Locked: Hide", focused(W_LOCKED), hovered(W_LOCKED));
        std::snprintf(b, sizeof b, "Sort: %s", kSortNames[g.sort]);
        button(cv, find(W_SORT), b, focused(W_SORT), hovered(W_SORT));
        button(cv, find(W_REFRESH), "Refresh", focused(W_REFRESH), hovered(W_REFRESH));
    }

    // column headers
    {
        struct Col { int id; const char *label; int sort; float tx; bool centre; };
        const Col cols[] = {{W_COL_HOST, "HOST", kSortHost, 24, false}, {W_COL_COURSE, "COURSE", kSortCourse, 104, false},
                            {W_COL_CLASS, "CLASS", kSortClass, 276, true}, {W_COL_HCAP, "HANDICAP", kSortHandicap, 298, false},
                            {W_COL_PLAYERS, "PLAYERS", kSortPlayers, 380, true}, {W_COL_PING, "PING", kSortPing, 464, false}};
        for (const Col &c : cols) {
            const bool on = g.sort == c.sort;
            const Rgb col = hovered(c.id) ? kRedTop : (on ? kInk : kMid);
            float x0;
            if (c.id == W_COL_PING) { x0 = c.tx - cv.text_width(std::string(c.label), 8.5f); cv.text(x0, 71.0f, c.label, 8.5f, col); }
            else if (c.centre) { x0 = c.tx - cv.text_width(std::string(c.label), 8.5f) * 0.5f; cv.text(x0, 71.0f, c.label, 8.5f, col); }
            else { x0 = c.tx; cv.text(x0, 71.0f, c.label, 8.5f, col); }
            if (on) sort_arrow(cv, x0 + cv.text_width(std::string(c.label), 8.5f) + 2.0f, 65.5f, g.sort_desc);
        }
        cv.solid(8.0f, 74.0f, 466.0f, 0.8f, 0.0f, rgb(180, 180, 186));
    }

    // rows
    const int n = static_cast<int>(g.view.size());
    for (int i = 0; i < kRowsVisible && g.top + i < n; ++i) {
        const LobbyRoom &r = g.rooms[static_cast<std::size_t>(g.view[static_cast<std::size_t>(g.top + i)])];
        const float y = kListY + i * kRowH;
        const bool sel = g.top + i == g.sel;
        const bool rowfocus = sel && !modal && g.focus == W_ROW;
        const bool full = r.players >= r.max && !r.racing;
        if (rowfocus) red_pill(cv, 8.0f, y + 0.5f, 458.0f, kRowH - 1.0f);
        else if (sel) cv.solid(8.0f, y + 0.5f, 458.0f, kRowH - 1.0f, (kRowH - 1.0f) * 0.5f, rgb(196, 196, 202));
        else if (hovered(W_ROW + i)) cv.solid(8.0f, y + 0.5f, 458.0f, kRowH - 1.0f, (kRowH - 1.0f) * 0.5f, rgb(222, 222, 228));
        else if (i % 2 == 1) cv.solid(8.0f, y + 0.5f, 458.0f, kRowH - 1.0f, (kRowH - 1.0f) * 0.5f, rgb(236, 236, 240), 0.7f);
        const Rgb ink = rowfocus ? kWhite : (full ? kMid : kInk);
        const float base = y + 12.0f;
        if (r.vis == 1) lock_icon(cv, 12.5f, y + 3.5f, ink);
        else if (r.vis == 2) key_icon(cv, 11.5f, y + 3.0f, ink);
        cv.text(24.0f, base, cv.fit(r.host.empty() ? std::string("(no name)") : r.host, 10.5f, 76.0f), 10.5f, ink);
        cv.text(104.0f, base, cv.fit(lobby_course_name(r.course), 10.0f, 150.0f), 10.0f, ink);
        cv.text_centered(276.0f, base, r.cls >= 1 ? std::to_string(r.cls) : std::string("-"), 10.0f, ink);
        stars(cv, 298.0f, base, r.handicap, ink);
        {
            char b[16];
            std::snprintf(b, sizeof b, "%d/%d", r.players, r.max);
            cv.text_centered(r.racing ? 368.0f : 380.0f, base, b, 10.0f, ink);
            if (r.racing) cv.text(384.0f, base - 1.0f, "RACING", 7.0f, rowfocus ? kWhite : rgb(226, 140, 0));
            else if (full) cv.text(395.0f, base - 1.0f, "FULL", 7.0f, rowfocus ? kWhite : kRedTop);
        }
        {
            const int p = r.ping_ms >= 0 ? r.ping_ms : r.est_ms;
            std::string t = p < 0 ? std::string("--") : (r.ping_ms >= 0 ? std::to_string(p) : "~" + std::to_string(p));
            if (p >= 0) t += " ms";
            cv.text_right(464.0f, base, t, 10.0f, ink);
            if (p >= 0) {
                const Rgb dot = p < 80 ? rgb(52, 168, 72) : (p < 150 ? rgb(226, 160, 0) : rgb(220, 40, 40));
                cv.circle(464.0f - cv.text_width(t, 10.0f) - 6.0f, base - 3.4f, 2.6f, rowfocus ? kWhite : dot);
            }
        }
    }
    if (n > kRowsVisible) {
        const float th = kRowH * kRowsVisible, knob = std::max(14.0f, th * kRowsVisible / n);
        const float ky = kListY + (th - knob) * (static_cast<float>(g.top) / std::max(1, n - kRowsVisible));
        cv.solid(469.5f, kListY, 3.0f, th, 1.5f, rgb(214, 214, 218));
        cv.solid(469.0f, ky, 4.0f, knob, 2.0f, hovered(W_SCROLL) ? kRedTop : kMid);
    }
    if (n == 0) {
        const float cy = kListY + kRowH * kRowsVisible * 0.5f;
        if (!g_update_to.empty()) {
            cv.text_centered(240.0f, cy - 9.0f, "This version of the game (" RR_GAME_VERSION ") is out of date for online play.", 11.0f, kInk);
            cv.text_centered(240.0f, cy + 6.0f, "Download " + g_update_to + " from the game's release page, then try again.", 9.5f, kMid);
            cv.text_centered(240.0f, cy + 19.0f, "Your saves and settings stay as they are.", 9.5f, kMid);
        }
        else if (!g.have_list && (g.status == 0 || g.status == 200)) { spinner(cv, 240.0f, cy - 12.0f, 7.0f); cv.text_centered(240.0f, cy + 12.0f, "Looking for races...", 11.0f, kMid); }
        else if (g.status != 200 && !g.have_list) {
            cv.text_centered(240.0f, cy - 2.0f, "Can't reach the room service.", 11.0f, kInk);
            cv.text_centered(240.0f, cy + 12.0f, "Check your internet connection, then Refresh.", 9.5f, kMid);
        } else if (g.rooms.empty()) {
            cv.text_centered(240.0f, cy - 2.0f, "No races open right now.", 11.0f, kInk);
            cv.text_centered(240.0f, cy + 12.0f, "Host one from the Wireless Battle menu, or Refresh in a moment.", 9.5f, kMid);
        } else {
            cv.text_centered(240.0f, cy - 2.0f, "No races match your search or filters.", 11.0f, kInk);
            cv.text_centered(240.0f, cy + 12.0f, std::to_string(g.rooms.size()) + (g.rooms.size() == 1 ? " race is" : " races are") + " hidden by them.", 9.5f, kMid);
        }
    }

    // bottom buttons + status
    {
        const bool racing = !g.view.empty() && g.rooms[static_cast<std::size_t>(g.view[static_cast<std::size_t>(g.sel)])].racing;
        button(cv, find(W_JOIN), racing ? "WAIT" : "JOIN", focused(W_JOIN), hovered(W_JOIN), 10.5f, !g.view.empty());
    }
    button(cv, find(W_DIRECT), "DIRECT CONNECT", focused(W_DIRECT), hovered(W_DIRECT), 10.5f);
    button(cv, find(W_BACK), "BACK", focused(W_BACK), hovered(W_BACK), 10.5f);
    {
        std::string st;
        if (g.have_list) {
            st = std::to_string(g.view.size()) + (g.view.size() == g.rooms.size() ? "" : " of " + std::to_string(g.rooms.size())) + (g.rooms.size() == 1 ? " race" : " races");
            const unsigned age = (g.frames - g.list_frame) / 60u;
            st += age == 0 ? "  -  just updated" : "  -  updated " + std::to_string(age) + " s ago";
            if (g.status != 200) st += "  (offline)";
        }
        cv.text_right(472.0f, 231.0f, st, 9.0f, kMid);
    }
    {
        using ui::PadBtn;
        const bool kb = pad::glyphs() == pad::Glyphs::Keyboard;
        info_bar(cv, {{{PadBtn::Dpad}, "Move", "Arrows"}, {{PadBtn::Cross}, "Select", "Enter"}, {{PadBtn::Circle}, "Back", "Esc"},
                      {{PadBtn::Triangle}, "Refresh"}, {{PadBtn::Square}, "Search"}, {{PadBtn::L, PadBtn::R}, "Sort"},
                      {{PadBtn::Select}, "Direct connect"}},
                 kb ? "Z also selects.  Mouse: click, double-click a race, the wheel scrolls the list."
                    : "Mouse: click, double-click a race, the wheel scrolls the list.");
    }
}

void render_host(const Canvas &cv) {
    page_background(cv);
    header(cv, "WIRELESS BATTLE  -  ONLINE", "HOST A RACE");
    std::vector<Widget> ws;
    layout(ws);
    const auto find = [&](int id) { for (const auto &w : ws) if (w.id == id) return w; return Widget{W_NONE, 0, 0, 0, 0}; };
    const bool modal = g.modal != Modal::None;
    // summary card
    cv.solid(39.0f, 45.0f, 402.0f, 56.0f, 10.0f, rgb(210, 210, 214));
    cv.vgrad(40.0f, 46.0f, 400.0f, 54.0f, 9.0f, rgb(96, 96, 100), rgb(52, 52, 56));
    {
        const LobbyRoom &r = g.host_room;
        cv.text(54.0f, 66.0f, (r.host.empty() ? std::string("Your") : r.host + "'s") + " race", 13.0f, kWhite);
        char b[64];
        std::snprintf(b, sizeof b, "CLASS %d", r.cls);
        cv.text(54.0f, 86.0f, b, 10.5f, kWhite);
        cv.text(110.0f, 86.0f, lobby_course_name(r.course), 10.5f, kWhite);
        cv.text(290.0f, 86.0f, "Handicap", 10.5f, kWhite);
        stars(cv, 340.0f, 86.0f, r.handicap, kWhite);
    }
    cv.text(40.0f, 118.0f, "Who can join", 10.0f, kMid);
    const char *labels[3] = {"Anyone", "With password", "Room code only"};
    for (int i = 0; i < 3; ++i) {
        const Widget w = find(W_VIS0 + i);
        const bool chosen = g_prefs.vis == i, foc = !modal && g.host_focus == W_VIS0 + i;
        if (chosen) red_pill(cv, w.x, w.y, w.w, w.h);
        else grey_pill(cv, w.x, w.y, w.w, w.h);
        if (foc) cv.frame(w.x - 2.0f, w.y - 2.0f, w.w + 4.0f, w.h + 4.0f, w.h * 0.5f + 2.0f, 1.6f, chosen ? kInk : kRedTop);
        else if (!modal && g.hover == W_VIS0 + i) cv.frame(w.x - 1.0f, w.y - 1.0f, w.w + 2.0f, w.h + 2.0f, w.h * 0.5f + 1.0f, 1.2f, kRedTop, 0.8f);
        cv.text_centered(w.x + w.w * 0.5f, w.y + 14.0f, labels[i], 10.5f, kWhite);
    }
    {
        const Widget w = find(W_PASSWORD);
        const bool on = g_prefs.vis == 1, foc = !modal && g.host_focus == W_PASSWORD;
        cv.text(40.0f, w.y + 14.0f, "Password", 10.5f, on ? kInk : kFaint);
        if (foc) red_pill(cv, w.x, w.y, w.w, w.h);
        else grey_pill(cv, w.x, w.y, w.w, w.h, on ? 1.0f : 0.35f);
        if (!modal && g.hover == W_PASSWORD && on && !foc) cv.frame(w.x - 1.0f, w.y - 1.0f, w.w + 2.0f, w.h + 2.0f, w.h * 0.5f + 1.0f, 1.2f, kRedTop, 0.8f);
        const std::string t = !on ? std::string("(not used)") : (g_prefs.password.empty() ? std::string("Choose a password") : std::string(g_prefs.password.size(), '*') + "   (" + ui::button_name(ui::PadBtn::Cross, pad::glyphs()) + " to change)");
        cv.text(w.x + 12.0f, w.y + 14.0f, t, 10.5f, kWhite, on ? 1.0f : 0.6f);
    }
    cv.text(40.0f, 196.0f, "Room code", 10.5f, kInk);
    cv.text(130.0f, 198.0f, g.code, 18.0f, kRedTop);
    cv.text(214.0f, 191.0f, g_prefs.vis == 2 ? "Only players with this code can join:" : "Friends can also join with this code:", 9.0f, kMid);
    cv.text(214.0f, 202.0f, "Direct connect in their server browser.", 9.0f, kMid);
    button(cv, find(W_OPEN), "OPEN ROOM", !modal && g.host_focus == W_OPEN, !modal && g.hover == W_OPEN, 11.5f);
    if (!g_update_to.empty()) {   // the room would not be listed
        cv.solid(36.0f, 181.0f, 408.0f, 28.0f, 7.0f, rgb(226, 140, 0));   // (over the room code: it would not be listed)
        cv.text_centered(240.0f, 199.0f, "Out of date (" RR_GAME_VERSION "): get " + g_update_to + " to host online races.", 10.0f, kWhite);
    }
    {
        using ui::PadBtn;
        info_bar(cv, {{{PadBtn::Dpad}, "Move", "Arrows"}, {{PadBtn::Cross}, "Choose"}, {{PadBtn::Start}, "Open the room"}},
                 "Mouse: click.  Your choice is remembered.");
    }
}

void render_busy(const Canvas &cv) {
    cv.solid(0.0f, 0.0f, 480.0f, 272.0f, 0.0f, rgb(0, 0, 0), 0.45f);
    if (g.busy_for == BusyFor::Queue) {   // waiting for a race in progress to finish
        cv.solid(79.0f, 89.0f, 322.0f, 102.0f, 14.0f, rgb(210, 210, 214));
        cv.vgrad(80.0f, 90.0f, 320.0f, 100.0f, 13.0f, rgb(96, 96, 100), rgb(46, 46, 50));
        spinner(cv, 240.0f, 110.0f, 7.0f);
        const unsigned sec = (g.frames - g.queue_since) / 60u;
        char t[16]; std::snprintf(t, sizeof t, "%u:%02u", sec / 60u, sec % 60u);
        cv.text_right(390.0f, 104.0f, t, 9.0f, rgb(200, 200, 204));
        cv.text_centered(240.0f, 135.0f, cv.fit(g.busy_text, 11.0f, 300.0f), 11.0f, kWhite);
        cv.text_centered(240.0f, 149.0f, cv.fit(g.queue_note, 9.0f, 300.0f), 9.0f, rgb(210, 210, 214));
    } else {
        cv.solid(119.0f, 99.0f, 242.0f, 82.0f, 14.0f, rgb(210, 210, 214));
        cv.vgrad(120.0f, 100.0f, 240.0f, 80.0f, 13.0f, rgb(96, 96, 100), rgb(46, 46, 50));
        spinner(cv, 240.0f, 120.0f, 7.0f);
        cv.text_centered(240.0f, 147.0f, cv.fit(g.busy_text, 11.0f, 220.0f), 11.0f, kWhite);
    }
    std::vector<Widget> ws;
    layout(ws);
    for (const auto &w : ws) if (w.id == W_BUSY_CANCEL) button(cv, w, "CANCEL", true, g.hover == W_BUSY_CANCEL, 10.0f);
}

void render_toast(const Canvas &cv) {
    if (g.frames >= g.toast_until || g.toast.empty()) return;
    const float a = std::min(1.0f, static_cast<float>(g.toast_until - g.frames) / 30.0f);
    const float w = std::min(440.0f, cv.text_width(g.toast, 11.0f) + 30.0f);
    cv.solid(240.0f - w * 0.5f, 186.0f, w, 22.0f, 11.0f, rgb(40, 40, 44), 0.92f * a);
    cv.solid(240.0f - w * 0.5f + 6.0f, 192.0f, 3.0f, 10.0f, 1.5f, kRedTop, a);
    cv.text_centered(240.0f + 3.0f, 201.0f, g.toast, 11.0f, kWhite, a);
}

} // namespace

void lobby_ui_render(std::uint8_t *rgba, int w, int h) {
    if (g.page == Page::None || rgba == nullptr || w < 480 || h < 272) return;
    if (g.modal == Modal::Text) { osk_ui_render(rgba, w, h); return; }
    static std::vector<std::uint8_t> native(480u * 272u * 4u);
    const Canvas cv{native.data(), 480, 272, 1.0f};
    if (g.page == Page::Browser) render_browser(cv);
    else render_host(cv);
    render_toast(cv);
    if (g.modal == Modal::Busy) render_busy(cv);
    ui::upscale_crisp(native.data(), rgba, w, h);
}

void ui_page_render(std::uint8_t *rgba, int w, int h) {
    if (lobby_ui_active()) lobby_ui_render(rgba, w, h);
    else osk_ui_render(rgba, w, h);
}
std::uint64_t ui_page_state_key() { return lobby_ui_active() ? lobby_ui_state_key() : osk_ui_state_key(); }

} // namespace rr
