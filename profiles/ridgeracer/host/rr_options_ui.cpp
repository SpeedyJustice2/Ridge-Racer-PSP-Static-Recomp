#include "rr_options_ui.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include "rr_osk_ui.hpp"   // kOskMove / kOskAccept / kOskCancel
#include "rr_pad.hpp"
#include "rr_pad_ui.hpp"
#include "rr_ui_canvas.hpp"
#include "rr_ui_repeat.hpp"

namespace rr {
namespace {

using ui::Canvas;
using ui::Rgb;
using ui::rgb;
using ui::mix;
using ui::clamp01;

constexpr std::uint32_t kSelect = 0x0001u, kStart = 0x0008u, kUp = 0x0010u, kRight = 0x0020u, kDown = 0x0040u, kLeft = 0x0080u,
                        kTriangle = 0x1000u, kCircle = 0x2000u, kCross = 0x4000u, kSquare = 0x8000u;
constexpr std::uint32_t kDirs = kUp | kRight | kDown | kLeft;

const Rgb kInk = rgb(58, 58, 62), kFaint = rgb(206, 206, 210), kMid = rgb(130, 130, 136), kWhite = rgb(255, 255, 255);
const Rgb kRedTop = rgb(236, 28, 36), kRedBot = rgb(190, 0, 10);

const int kFpsValues[] = {0, 60, 90, 120, 144, 165, 180, 240};

enum class Kind { Choice, Toggle, Slider, Action };
enum class Act { None, Update, BugReport, Issues, Discord, Screenshots, ExportSave, ImportSave, SaveFolder };

struct Row {
    const char *label;
    const char *help;
    Kind kind;
    std::vector<std::string> choices;            // Choice
    std::function<int(const PlayerSettings &)> get;
    std::function<void(PlayerSettings &, int)> set;
    bool restart{false};                         // takes effect after a restart
    int lo{0}, hi{0}, step{5};                   // Slider (percent)
    Act act{Act::None};
};

struct Category {
    const char *name;
    const char *help;
    std::vector<Row> rows;
};

enum class Modal { None, Restart, ConfirmUpdate, DrawDistance };

struct State {
    bool active{false};
    PlayerSettings s, running;
    OptionsContext ctx;
    std::vector<Category> cats;
    int cat{0};          // category cursor (cats.size() = BACK)
    int row{0};
    bool in_rows{false};
    int hover_cat{-1}, hover_row{-1};
    Modal modal{Modal::None};
    int modal_btn{0};
    int pending_dd{0};   // (draw distance waiting for the warning to be accepted)
    bool armed{false};
    std::uint32_t prev_pad{0};
    ui::NavRepeat repeat;
    ui::Ticks60 ticks;
    std::uint32_t events{0};
    bool live_change{false};
    OptionsRequest out{OptionsRequest::None};
    OptionsUpdateView upd;
    std::string toast;
    std::uint32_t frames{0}, toast_until{0};
} g;

std::string pct(int v) { return std::to_string(v) + "%"; }

std::vector<Category> build(const OptionsContext &ctx) {
    std::vector<Category> c;
    {
        Category d{"DISPLAY", "Renderer, resolution, window and frame rate.", {}};
        std::vector<std::string> res;
        for (int k = 1; k <= 8; ++k) res.push_back(std::to_string(k) + "x  (" + std::to_string(480 * k) + "x" + std::to_string(272 * k) + (k == 1 ? ", PSP)" : ")"));
        std::vector<std::string> win;
        for (int k = 1; k <= 8; ++k) win.push_back(std::to_string(k) + "x  (" + std::to_string(480 * k) + "x" + std::to_string(272 * k) + ")");
        std::vector<std::string> fps;
        for (int f : kFpsValues)
            fps.push_back(f == 0 ? "Match monitor (" + std::to_string(ctx.monitor_hz) + " Hz)" : f == 60 ? "60 (original)" : f > ctx.monitor_hz + 2 ? std::to_string(f) + "  (above monitor)" : std::to_string(f));
        d.rows.push_back({"Renderer", "OpenGL draws at a higher resolution with the image options below. Software is the original 480x272 look.", Kind::Choice,
                          {"OpenGL (enhanced)", "Software (original)"}, [](const PlayerSettings &s) { return s.gpu ? 0 : 1; }, [](PlayerSettings &s, int v) { s.gpu = v == 0; }, true});
        d.rows.push_back({"Render resolution", "The resolution the game is drawn at (OpenGL). Higher is sharper and needs a faster graphics card.", Kind::Choice, res,
                          [](const PlayerSettings &s) { return std::clamp(s.render_scale, 1, 8) - 1; }, [](PlayerSettings &s, int v) { s.render_scale = v + 1; }, true});
        d.rows.push_back({"Window size", "The size of the game window when it is not full screen.", Kind::Choice, win,
                          [](const PlayerSettings &s) { return std::clamp(s.window_scale, 1, 8) - 1; }, [](PlayerSettings &s, int v) { s.window_scale = v + 1; }, true});
        d.rows.push_back({"Full screen", "Borderless full screen. F11 or Alt+Enter also switch it at any time.", Kind::Toggle, {},
                          [](const PlayerSettings &s) { return s.fullscreen ? 1 : 0; }, [](PlayerSettings &s, int v) { s.fullscreen = v != 0; }, false});
        d.rows.push_back({"Frame rate", "Races run at this rate. Match monitor is smoothest; a rate above the monitor's refresh rate looks uneven (it cannot show every picture).", Kind::Choice, fps,
                          [](const PlayerSettings &s) {
                              for (int i = 0; i < static_cast<int>(std::size(kFpsValues)); ++i) if (kFpsValues[i] == s.fps) return i;
                              return 0;
                          },
                          [](PlayerSettings &s, int v) { s.fps = kFpsValues[std::clamp(v, 0, static_cast<int>(std::size(kFpsValues)) - 1)]; }, true});
        d.rows.push_back({"Frame skip", "Automatic skips pictures only while the PC cannot keep up (the game keeps its speed).", Kind::Choice,
                          {"Automatic", "Off", "Every 2nd picture", "Every 3rd picture"},
                          [](const PlayerSettings &s) { return std::clamp(s.frame_skip, 0, 3); }, [](PlayerSettings &s, int v) { s.frame_skip = v; }, true});
        d.rows.push_back({"Smooth menus", "Menus run at the high frame rate too (their animations keep their normal speed).", Kind::Toggle, {},
                          [](const PlayerSettings &s) { return s.smooth_menus ? 1 : 0; }, [](PlayerSettings &s, int v) { s.smooth_menus = v != 0; }, true});
        d.rows.push_back({"Low input latency", "Waits for the graphics card after every picture: less input delay, may stutter at high settings.", Kind::Toggle, {},
                          [](const PlayerSettings &s) { return s.low_latency ? 1 : 0; }, [](PlayerSettings &s, int v) { s.low_latency = v != 0; }, true});
        c.push_back(std::move(d));
    }
    {
        Category q{"GRAPHICS", "Anti-aliasing, texture filtering, sharpening and HUD.", {}};
        q.rows.push_back({"Anti-aliasing", "Smooths jagged edges. MSAA is the best quality; FXAA is light on the graphics card.", Kind::Choice,
                          {"Off", "FXAA (light)", "MSAA 2x", "MSAA 4x", "MSAA 8x"},
                          [](const PlayerSettings &s) { return std::clamp(s.aa, 0, 4); }, [](PlayerSettings &s, int v) { s.aa = v; }, true});
        q.rows.push_back({"Texture filtering", "Keeps the road and walls sharp at a distance.", Kind::Choice,
                          {"Bilinear", "Anisotropic 2x", "Anisotropic 4x", "Anisotropic 8x", "Anisotropic 16x"},
                          [](const PlayerSettings &s) { const int a = std::clamp(s.aniso, 1, 16); return a >= 16 ? 4 : a >= 8 ? 3 : a >= 4 ? 2 : a >= 2 ? 1 : 0; },
                          [](PlayerSettings &s, int v) { static const int m[] = {1, 2, 4, 8, 16}; s.aniso = m[std::clamp(v, 0, 4)]; }, false});
        q.rows.push_back({"Mipmaps", "Less shimmer on distant textures.", Kind::Toggle, {},
                          [](const PlayerSettings &s) { return s.mipmaps ? 1 : 0; }, [](PlayerSettings &s, int v) { s.mipmaps = v != 0; }, true});
        q.rows.push_back({"Sharpening", "Sharpens the finished picture.", Kind::Choice, {"Off", "Mild", "Medium", "Strong"},
                          [](const PlayerSettings &s) { return std::clamp(s.sharpen, 0, 3); }, [](PlayerSettings &s, int v) { s.sharpen = v; }, false});
        q.rows.push_back({"HUD and text", "How the game's menus, text and HUD are enlarged.", Kind::Choice, {"Original (soft)", "Sharper", "Crispest"},
                          [](const PlayerSettings &s) { return std::clamp(s.ui_crisp, 0, 2); }, [](PlayerSettings &s, int v) { s.ui_crisp = v; }, false});
        q.rows.push_back({"Draw distance", "EXPERIMENTAL: draws scenery further down the track, but some scenery may be drawn wrong. Further costs some speed.", Kind::Choice,
                          {"Original", "Far", "Farther", "Very far", "Farthest"},
                          [](const PlayerSettings &s) { return std::clamp((s.draw_distance + 1) / 2, 0, 4); },
                          [](PlayerSettings &s, int v) { s.draw_distance = std::clamp(v, 0, 4) * 2; }, true});
        // (Car textures: Original / Sharp / Pixel / HD - work in progress, hidden for now: --car-textures on the command line)
        q.rows.push_back({"Smooth cut-out edges", "Smooths the edges of trees, fences and other cut-out art.", Kind::Toggle, {},
                          [](const PlayerSettings &s) { return s.smooth_cutouts ? 1 : 0; }, [](PlayerSettings &s, int v) { s.smooth_cutouts = v != 0; }, false});
        c.push_back(std::move(q));
    }
    {
        Category a{"SOUND", "Overall volume and the port's menu sounds.", {}};
        Row vol{"Volume", "The overall volume (100% is the game's own level). F9 / F10 change it and F8 mutes at any time.", Kind::Slider, {},
                [](const PlayerSettings &s) { return s.volume; }, [](PlayerSettings &s, int v) { s.volume = v; }, false};
        vol.lo = 0; vol.hi = 150;
        a.rows.push_back(vol);
        Row menu{"Menu sounds", "The clicks of the port's own pages (on-screen keyboard, server browser, this page).", Kind::Slider, {},
                 [](const PlayerSettings &s) { return s.menu_volume; }, [](PlayerSettings &s, int v) { s.menu_volume = v; }, false};
        menu.lo = 0; menu.hi = 200;
        a.rows.push_back(menu);
        c.push_back(std::move(a));
    }
    {
        Category k{"CONTROLS", "Button pictures and controller triggers.", {}};
        k.rows.push_back({"Button pictures", "Which buttons the game shows. Automatic follows the controller or keyboard you used last.", Kind::Choice,
                          {"Automatic", "Xbox", "PlayStation", "Nintendo Switch", "Keyboard"},
                          [](const PlayerSettings &s) { return std::clamp(s.button_prompts, 0, 4); }, [](PlayerSettings &s, int v) { s.button_prompts = v; }, false});
        k.rows.push_back({"Triggers as pedals", "Right trigger accelerates and left trigger brakes, as well as the face buttons.", Kind::Toggle, {},
                          [](const PlayerSettings &s) { return s.trigger_buttons ? 1 : 0; }, [](PlayerSettings &s, int v) { s.trigger_buttons = v != 0; }, false});
        c.push_back(std::move(k));
    }
    {
        Category o{"ONLINE & SYSTEM", "Internet races, performance overlay and report, screenshots, the old settings window.", {}};
        o.rows.push_back({"Online play", "Wireless Battle races over the internet (server browser and room codes).", Kind::Toggle, {},
                          [](const PlayerSettings &s) { return s.online ? 1 : 0; }, [](PlayerSettings &s, int v) { s.online = v != 0; }, true});
        o.rows.push_back({"Performance overlay", "Shows the frame rate (and with Details the frame times and renderer) over the game picture.", Kind::Choice,
                          {"Off", "Frame rate", "Details"},
                          [](const PlayerSettings &s) { return std::clamp(s.perf_overlay, 0, 2); }, [](PlayerSettings &s, int v) { s.perf_overlay = v; }, false});
        o.rows.push_back({"Performance report", "Writes performance-report.txt next to the game (frame times, CPU and GPU) for bug reports.", Kind::Toggle, {},
                          [](const PlayerSettings &s) { return s.perf_report ? 1 : 0; }, [](PlayerSettings &s, int v) { s.perf_report = v != 0; }, true});
        o.rows.push_back({"Settings window at start", "Shows the old settings window before the game starts (holding Shift while starting also opens it).", Kind::Toggle, {},
                          [](const PlayerSettings &s) { return s.show_launcher ? 1 : 0; }, [](PlayerSettings &s, int v) { s.show_launcher = v != 0; }, false});
        Row shots{"Screenshots folder", "Opens the folder where F12 saves screenshots.", Kind::Action, {}, {}, {}, false};
        shots.act = Act::Screenshots;
        o.rows.push_back(shots);
        c.push_back(std::move(o));
    }
    {
        Category sv{"SAVE DATA", "Back up your progress, move it to another PC or bring it over from another copy.", {}};
        Row ex{"Export save", "Saves a copy of your progress to a file you choose (a backup, or to take to another PC).", Kind::Action, {}, {}, {}, false};
        ex.act = Act::ExportSave;
        sv.rows.push_back(ex);
        Row im{"Import save", "Loads progress from an exported save or from DATA.BIN in another copy's savedata folder. Your current save is backed up first, then the game restarts.", Kind::Action, {}, {}, {}, false};
        im.act = Act::ImportSave;
        sv.rows.push_back(im);
        Row fo{"Save folder", "Opens the folder that holds your save and its backups (savedata next to the game). Updates never touch it.", Kind::Action, {}, {}, {}, false};
        fo.act = Act::SaveFolder;
        sv.rows.push_back(fo);
        c.push_back(std::move(sv));
    }
    {
        Category h{"UPDATES & HELP", "New versions, bug reports and the community.", {}};
        Row up{"Version", "Looks for a newer version of the port on GitHub and installs it (the game restarts).", Kind::Action, {}, {}, {}, false};
        up.act = Act::Update;
        if (!ctx.can_update) up.help = "Updates are installed with the Windows version's built-in updater.";
        h.rows.push_back(up);
        Row bug{"Make a bug report", "Saves the logs, settings and a PC summary in one zip file, then offers the GitHub bug report page.", Kind::Action, {}, {}, {}, false};
        bug.act = Act::BugReport;
        h.rows.push_back(bug);
        Row iss{"Report a bug on GitHub", "Opens the bug report page in your browser. Attach the bug report zip there.", Kind::Action, {}, {}, {}, false};
        iss.act = Act::Issues;
        h.rows.push_back(iss);
        Row dis{"Join our Discord", "News, updates and opponents to race against: opens the community Discord in your browser.", Kind::Action, {}, {}, {}, false};
        dis.act = Act::Discord;
        h.rows.push_back(dis);
        c.push_back(std::move(h));
    }
    return c;
}

int ncats() { return static_cast<int>(g.cats.size()) + 1; }   // + BACK
bool on_back() { return g.cat >= static_cast<int>(g.cats.size()); }
std::vector<Row> &rows() { static std::vector<Row> none; return on_back() ? none : g.cats[static_cast<std::size_t>(g.cat)].rows; }

bool row_needs_restart(const Row &r) { return r.restart && r.get && r.get(g.s) != r.get(g.running); }

std::string action_value(const Row &r) {
    switch (r.act) {
    case Act::Update:
        if (!g.ctx.can_update) return g.ctx.version;
        switch (g.upd.state) {
        case OptionsUpdateView::Idle: return g.ctx.version + "  -  Check for updates";
        case OptionsUpdateView::Checking: return "Checking...";
        case OptionsUpdateView::UpToDate: return g.ctx.version + "  -  Up to date";
        case OptionsUpdateView::Available: return "Install " + g.upd.tag;
        case OptionsUpdateView::Downloading: return "Downloading " + std::to_string(g.upd.percent) + "%";
        case OptionsUpdateView::Failed: return "Update failed";
        case OptionsUpdateView::Unreachable: return "Can't reach GitHub";
        }
        return {};
    case Act::BugReport: return "Save...";
    case Act::Issues: return "Open page";
    case Act::Discord: return "Open invite";
    case Act::Screenshots: return "Open";
    case Act::ExportSave: return "Save...";
    case Act::ImportSave: return "Choose...";
    case Act::SaveFolder: return "Open";
    default: return {};
    }
}

std::string value_text(const Row &r) {
    switch (r.kind) {
    case Kind::Choice: { const int v = r.get(g.s); return v >= 0 && v < static_cast<int>(r.choices.size()) ? r.choices[static_cast<std::size_t>(v)] : std::string("?"); }
    case Kind::Toggle: return r.get(g.s) ? "ON" : "OFF";
    case Kind::Slider: return pct(r.get(g.s));
    case Kind::Action: return action_value(r);
    }
    return {};
}

void changed(const Row &r) {
    if (!r.restart) g.live_change = true;
    g.events |= kOskMove;
}

bool is_draw_distance(const Row &r) { return std::string(r.label) == "Draw distance"; }

void step_value(Row &r, int dir) {
    switch (r.kind) {
    case Kind::Choice: {
        const int n = static_cast<int>(r.choices.size());
        if (n <= 1) return;
        const int v = (r.get(g.s) + dir + n) % n;
        if (is_draw_distance(r) && v != 0 && !g.s.draw_distance_ack) {
            // experimental: say so first (once; accepting is remembered in ridgeracer.ini)
            PlayerSettings t = g.s; r.set(t, v); g.pending_dd = t.draw_distance;
            g.modal = Modal::DrawDistance; g.modal_btn = 1;
            g.events |= kOskMove;
            return;
        }
        r.set(g.s, v);
        changed(r);
        break;
    }
    case Kind::Toggle: r.set(g.s, r.get(g.s) ? 0 : 1); changed(r); break;
    case Kind::Slider: {
        const int v = std::clamp(r.get(g.s) + dir * r.step, r.lo, r.hi);
        if (v != r.get(g.s)) { r.set(g.s, v); changed(r); }
        break;
    }
    case Kind::Action: break;
    }
}

void activate_row(Row &r) {
    if (r.kind == Kind::Toggle || r.kind == Kind::Choice) { step_value(r, +1); return; }
    if (r.kind != Kind::Action) return;
    g.events |= kOskAccept;
    switch (r.act) {
    case Act::Update:
        if (!g.ctx.can_update) return;
        if (g.upd.state == OptionsUpdateView::Available) { g.modal = Modal::ConfirmUpdate; g.modal_btn = 0; }
        else if (g.upd.state != OptionsUpdateView::Checking && g.upd.state != OptionsUpdateView::Downloading) g.out = OptionsRequest::CheckUpdate;
        break;
    case Act::BugReport: g.out = OptionsRequest::BugReport; break;
    case Act::Issues: g.out = OptionsRequest::OpenIssues; break;
    case Act::Discord: g.out = OptionsRequest::OpenDiscord; break;
    case Act::Screenshots: g.out = OptionsRequest::OpenScreenshots; break;
    case Act::ExportSave: g.out = OptionsRequest::ExportSave; break;
    case Act::ImportSave: g.out = OptionsRequest::ImportSave; break;
    case Act::SaveFolder: g.out = OptionsRequest::OpenSaveFolder; break;
    default: break;
    }
}

void try_close() {
    if (g.upd.state == OptionsUpdateView::Downloading) { options_ui_toast("Please wait for the download to finish"); return; }
    g.events |= kOskCancel;
    if (options_needs_restart(g.s, g.running)) { g.modal = Modal::Restart; g.modal_btn = 0; return; }
    g.out = OptionsRequest::Close;
}

int modal_buttons() {
    if (g.modal == Modal::Restart) return g.ctx.can_restart ? 2 : 1;
    if (g.modal == Modal::ConfirmUpdate || g.modal == Modal::DrawDistance) return 2;
    return 0;
}
void modal_press(int b) {
    if (g.modal == Modal::Restart) {
        g.modal = Modal::None;
        g.out = (g.ctx.can_restart && b == 0) ? OptionsRequest::Restart : OptionsRequest::Close;
        g.events |= kOskAccept;
    } else if (g.modal == Modal::ConfirmUpdate) {
        g.modal = Modal::None;
        if (b == 0) g.out = OptionsRequest::Update;
        g.events |= b == 0 ? kOskAccept : kOskCancel;
    } else if (g.modal == Modal::DrawDistance) {
        g.modal = Modal::None;
        if (b == 0) { g.s.draw_distance = g.pending_dd; g.s.draw_distance_ack = true; g.events |= kOskAccept; }
        else g.events |= kOskCancel;
    }
}
void modal_cancel() {
    if (g.modal == Modal::Restart) { g.modal = Modal::None; g.events |= kOskCancel; }   // (stay on the page)
    else if (g.modal == Modal::ConfirmUpdate || g.modal == Modal::DrawDistance) modal_press(1);
}

// ---- layout (480x272 units) ----
constexpr float kCatX = 0.0f, kCatW = 150.0f, kCatY = 58.0f, kCatH = 22.0f;
constexpr float kRowX = 160.0f, kRowW = 312.0f, kRowY = 52.0f, kRowH = 21.0f;
constexpr float kValL = 330.0f, kValR = 468.0f;   // value box (arrows at the ends)

int cat_at(float x, float y) {
    if (x < kCatX || x >= kCatX + kCatW) return -1;
    const int i = static_cast<int>(std::floor((y - kCatY) / kCatH));
    return (i >= 0 && i < ncats()) ? i : -1;
}
int row_at(float x, float y) {
    if (x < kRowX || x >= kRowX + kRowW || on_back()) return -1;
    const int i = static_cast<int>(std::floor((y - kRowY) / kRowH));
    return (i >= 0 && i < static_cast<int>(rows().size())) ? i : -1;
}
// Modal box and its buttons.
constexpr float kMx = 90.0f, kMy = 82.0f, kMw = 300.0f, kMh = 108.0f;
void modal_button_rect(int i, int n, float &x, float &y, float &w, float &h) {
    w = 120.0f; h = 22.0f; y = kMy + kMh - 32.0f;
    const float total = n * w + (n - 1) * 12.0f;
    x = 240.0f - total * 0.5f + i * (w + 12.0f);
}

void handle_mouse(const LobbyInput &in) {
    if (!in.mouse_inside) { g.hover_cat = g.hover_row = -1; return; }
    const float mx = in.mouse_x, my = in.mouse_y;
    if (g.modal != Modal::None) {
        const int n = modal_buttons();
        for (int i = 0; i < n; ++i) {
            float x, y, w, h;
            modal_button_rect(i, n, x, y, w, h);
            if (mx >= x && mx < x + w && my >= y && my < y + h) {
                if (g.modal_btn != i) { g.modal_btn = i; g.events |= kOskMove; }
                if (in.clicks > 0) modal_press(i);
            }
        }
        return;
    }
    g.hover_cat = cat_at(mx, my);
    g.hover_row = row_at(mx, my);
    if (in.wheel != 0 && g.in_rows && !rows().empty()) {
        const int n = static_cast<int>(rows().size());
        g.row = std::clamp(g.row - in.wheel, 0, n - 1);
        g.events |= kOskMove;
    }
    if (in.clicks <= 0) return;
    if (g.hover_cat >= 0) {
        g.cat = g.hover_cat; g.row = 0; g.in_rows = false;
        g.events |= kOskMove;
        if (on_back()) try_close();
        return;
    }
    if (g.hover_row >= 0) {
        g.in_rows = true; g.row = g.hover_row;
        Row &r = rows()[static_cast<std::size_t>(g.row)];
        if (r.kind == Kind::Choice || r.kind == Kind::Slider) {
            if (mx >= kValL - 4.0f && mx < kValL + 12.0f) step_value(r, -1);
            else if (mx >= kValR - 12.0f && mx < kValR + 4.0f) step_value(r, +1);
            else if (r.kind == Kind::Slider && mx >= kValL + 14.0f && mx < kValR - 46.0f) {   // click on the bar: jump there
                const float t = clamp01((mx - (kValL + 14.0f)) / (kValR - 46.0f - kValL - 14.0f));
                const int v = static_cast<int>(std::lround((r.lo + t * (r.hi - r.lo)) / r.step)) * r.step;
                if (v != r.get(g.s)) { r.set(g.s, std::clamp(v, r.lo, r.hi)); changed(r); }
            } else step_value(r, +1);
        } else {
            activate_row(r);
        }
    }
}

void handle_pad(const LobbyInput &in) {
    // keyboard: Esc / Backspace back out
    for (char16_t c : in.typed) {
        if (c != 27 && c != 8) continue;
        if (g.modal != Modal::None) modal_cancel();
        else if (g.in_rows) { g.in_rows = false; g.events |= kOskCancel; }
        else try_close();
    }
    if (!g.armed) {   // (the press that opened the page must be released first)
        if (in.pad == 0u) g.armed = true;
        g.prev_pad = in.pad;
        return;
    }
    const std::uint32_t pressed = in.pad & ~g.prev_pad;
    std::uint32_t nav = pressed;
    nav |= g.repeat.step(in.pad, kDirs);   // auto-repeat (timed, see rr_ui_repeat.hpp)
    g.prev_pad = in.pad;
    const bool ok = (pressed & (kCross | kStart)) != 0u, back = (pressed & (kCircle | kTriangle | kSelect)) != 0u;
    if (g.modal != Modal::None) {
        const int n = modal_buttons();
        if (nav & kLeft) { g.modal_btn = (g.modal_btn + n - 1) % n; g.events |= kOskMove; }
        if (nav & kRight) { g.modal_btn = (g.modal_btn + 1) % n; g.events |= kOskMove; }
        if (ok) modal_press(g.modal_btn);
        else if (back) modal_cancel();
        return;
    }
    if (!g.in_rows) {
        if (nav & kUp) { g.cat = (g.cat + ncats() - 1) % ncats(); g.row = 0; g.events |= kOskMove; }
        if (nav & kDown) { g.cat = (g.cat + 1) % ncats(); g.row = 0; g.events |= kOskMove; }
        if ((nav & kRight) || ok) {
            if (on_back()) { if (ok) try_close(); }
            else if (!rows().empty()) { g.in_rows = true; g.row = 0; g.events |= ok ? kOskAccept : kOskMove; }
        }
        if (back) try_close();
        return;
    }
    auto &rs = rows();
    const int n = static_cast<int>(rs.size());
    if (n == 0) { g.in_rows = false; return; }
    g.row = std::clamp(g.row, 0, n - 1);
    Row &r = rs[static_cast<std::size_t>(g.row)];
    if (nav & kUp) { g.row = (g.row + n - 1) % n; g.events |= kOskMove; }
    if (nav & kDown) { g.row = (g.row + 1) % n; g.events |= kOskMove; }
    if (r.kind != Kind::Action) {
        if (nav & kLeft) step_value(r, -1);
        if (nav & kRight) step_value(r, +1);
    } else if (nav & kLeft) {
        g.in_rows = false; g.events |= kOskMove;
    }
    if (ok) activate_row(r);
    if (back) { g.in_rows = false; g.events |= kOskCancel; }
}

// ---- drawing ----
void red_pill(const Canvas &cv, float x, float y, float w, float h) {
    cv.vgrad(x, y, w, h, h * 0.5f, kRedTop, kRedBot);
    cv.rrect(x + 1.5f, y + 0.8f, w - 3.0f, h * 0.45f, h * 0.22f, [&](float, float, float &a) { a = 0.20f; return kWhite; });
}

void arrow(const Canvas &cv, float cx, float cy, bool right, Rgb c, float a = 1.0f) {
    const float xs_r[3] = {cx - 2.5f, cx + 3.0f, cx - 2.5f}, xs_l[3] = {cx + 2.5f, cx - 3.0f, cx + 2.5f}, ys[3] = {cy - 4.0f, cy, cy + 4.0f};
    cv.polygon(right ? xs_r : xs_l, ys, 3, c, a);
}

void render_page(const Canvas &cv) {
    // Page: white, grey fade from the top, faint plus marks; the left column a little greyer (like the game's Options list).
    cv.fill_all([&](float x, float y) {
        float v = 255.0f;
        v -= 52.0f * clamp01(1.0f - y / 62.0f) * clamp01(1.0f - y / 62.0f);
        if (x < kCatW) v -= 26.0f * clamp01(1.0f - x / kCatW) + 6.0f;
        const float mx = std::fmod(x + 7.0f, 30.0f), my = std::fmod(y + 11.0f, 30.0f);
        const bool plus = (std::fabs(mx - 15.0f) < 0.45f && std::fabs(my - 15.0f) < 2.2f) || (std::fabs(my - 15.0f) < 0.45f && std::fabs(mx - 15.0f) < 2.2f);
        if (plus) v -= 16.0f;
        return Rgb{v, v, v};
    });
    cv.text(10.0f, 19.0f, "RECOMP OPTIONS", 11.0f, kInk);
    cv.text_right(470.0f, 34.0f, "RECOMP OPTIONS", 24.0f, kFaint);

    // Categories (left).
    for (int i = 0; i < ncats(); ++i) {
        const float y = kCatY + i * kCatH;
        const bool sel = i == g.cat;
        const char *name = i < static_cast<int>(g.cats.size()) ? g.cats[static_cast<std::size_t>(i)].name : "BACK";
        if (sel) {   // red bar that fades out to the right, as in the game's lists
            cv.rrect(0.0f, y + 1.0f, kCatW + 8.0f, kCatH - 2.0f, 0.0f, [&](float u, float v, float &a) {
                a = (g.in_rows ? 0.55f : 1.0f) * (u < 0.72f ? 1.0f : clamp01(1.0f - (u - 0.72f) / 0.28f));
                return mix(kRedTop, kRedBot, v);
            });
        } else if (i == g.hover_cat && g.modal == Modal::None) {
            cv.solid(0.0f, y + 1.0f, kCatW, kCatH - 2.0f, 0.0f, kRedTop, 0.12f);
        }
        cv.text(8.0f, y + 15.5f, name, 12.5f, sel ? kWhite : kInk);
    }

    // Rows (right): the chosen category (dimmed while the cursor is still on the categories).
    if (!on_back()) {
        const auto &rs = rows();
        const float dim = g.in_rows ? 1.0f : 0.55f;
        for (int i = 0; i < static_cast<int>(rs.size()); ++i) {
            const Row &r = rs[static_cast<std::size_t>(i)];
            const float y = kRowY + i * kRowH;
            const bool foc = g.in_rows && i == g.row;
            if (foc) red_pill(cv, kRowX, y + 1.0f, kRowW, kRowH - 2.0f);
            else if (i == g.hover_row && g.modal == Modal::None) cv.frame(kRowX, y + 1.0f, kRowW, kRowH - 2.0f, (kRowH - 2.0f) * 0.5f, 1.2f, kRedTop, 0.7f);
            const Rgb tc = foc ? kWhite : kInk;
            cv.text(kRowX + 10.0f, y + 14.5f, r.label, 11.0f, tc, dim);
            if (row_needs_restart(r)) cv.text_right(kValL - 6.0f, y + 13.5f, "RESTART", 7.0f, foc ? kWhite : kRedTop, dim);
            const float vc = (kValL + kValR) * 0.5f;
            if (r.kind == Kind::Slider) {
                const int v = r.get(g.s);
                const float bx = kValL + 14.0f, bw = kValR - 46.0f - bx, by = y + kRowH * 0.5f - 2.0f;
                cv.solid(bx, by, bw, 4.0f, 2.0f, foc ? rgb(255, 170, 170) : kFaint, dim);
                const float t = static_cast<float>(v - r.lo) / static_cast<float>(std::max(1, r.hi - r.lo));
                cv.solid(bx, by, std::max(4.0f, bw * t), 4.0f, 2.0f, foc ? kWhite : kRedTop, dim);
                cv.circle(bx + bw * t, by + 2.0f, 4.0f, foc ? kWhite : kInk, dim);
                cv.text_right(kValR - 12.0f, y + 14.5f, pct(v), 10.0f, tc, dim);
                arrow(cv, kValL + 4.0f, y + kRowH * 0.5f, false, tc, dim);
                arrow(cv, kValR - 4.0f, y + kRowH * 0.5f, true, tc, dim);
            } else if (r.kind == Kind::Action) {
                const std::string t = cv.fit(value_text(r), 10.0f, kValR - kValL + 20.0f);
                const bool hot = r.act == Act::Update && g.upd.state == OptionsUpdateView::Available;
                cv.text_right(kValR - 2.0f, y + 14.5f, t, 10.0f, foc ? kWhite : (hot ? kRedTop : kMid), dim);
            } else {
                const std::string t = cv.fit(value_text(r), 10.5f, kValR - kValL - 24.0f);
                cv.text_centered(vc, y + 14.5f, t, 10.5f, tc, dim);
                arrow(cv, kValL + 4.0f, y + kRowH * 0.5f, false, tc, dim);
                arrow(cv, kValR - 4.0f, y + kRowH * 0.5f, true, tc, dim);
            }
        }
        if (rs.size() < 8u && !g.in_rows) {
            cv.text(kRowX + 10.0f, kRowY + rs.size() * kRowH + 18.0f, g.cats[static_cast<std::size_t>(g.cat)].help, 9.0f, kMid);
        }
    } else {
        cv.text(kRowX + 10.0f, kRowY + 16.0f, "Back to the Options screen.", 11.0f, kMid);
        if (options_needs_restart(g.s, g.running)) cv.text(kRowX + 10.0f, kRowY + 34.0f, "Some changes take effect after a restart.", 10.0f, kRedTop);
    }

    // Info bar.
    cv.rrect(0.0f, 240.0f, 480.0f, 40.0f, 0.0f, [&](float, float v, float &) { return mix(rgb(176, 176, 180), rgb(136, 136, 140), v); });
    std::string help;
    if (g.modal == Modal::Restart) help = "Restarting now applies every change.";
    else if (g.modal == Modal::ConfirmUpdate) help = "The update comes from the project's GitHub page and is checked against its published SHA-256 hash.";
    else if (g.modal == Modal::DrawDistance) help = "Original is the game's own draw distance. You can set it back at any time.";
    else if (on_back()) help = "Return to the Options screen.";
    else if (g.in_rows && !rows().empty()) {
        const Row &r = rows()[static_cast<std::size_t>(std::clamp(g.row, 0, static_cast<int>(rows().size()) - 1))];
        help = r.help;
        if (r.act == Act::Update && g.upd.state == OptionsUpdateView::Failed && !g.upd.message.empty()) help = g.upd.message;
        else if (r.act == Act::Update && g.upd.state == OptionsUpdateView::Unreachable) help = "GitHub could not be reached. Check the internet connection and try again.";
    }
    else help = g.cats[static_cast<std::size_t>(g.cat)].help;
    {   // (a long line gets a slightly smaller font before it is shortened)
        float em = 10.5f;
        while (em > 8.6f && cv.text_width(help, em) > 460.0f) em -= 0.25f;
        cv.text(12.0f, 255.0f, cv.fit(help, em, 460.0f), em, kWhite);
    }
    {
        using ui::PadBtn;
        const pad::Glyphs gl = pad::glyphs();
        std::vector<ui::Hint> hints;
        if (g.modal != Modal::None) hints = {{{PadBtn::Dpad}, "Choose", "Arrows"}, {{PadBtn::Cross}, "OK", "Enter"}, {{PadBtn::Circle}, "Cancel", "Esc"}};
        else if (g.in_rows) {
            const bool action = !rows().empty() && rows()[static_cast<std::size_t>(std::clamp(g.row, 0, static_cast<int>(rows().size()) - 1))].kind == Kind::Action;
            if (action) hints = {{{PadBtn::Dpad}, "Move", "Arrows"}, {{PadBtn::Cross}, "Select", "Enter"}, {{PadBtn::Circle}, "Back", "Esc"}};
            else hints = {{{PadBtn::Dpad}, "Move / change", "Arrows"}, {{PadBtn::Cross}, "Change", "Enter"}, {{PadBtn::Circle}, "Back", "Esc"}};
        } else hints = {{{PadBtn::Dpad}, "Move", "Arrows"}, {{PadBtn::Cross}, "Select", "Enter"}, {{PadBtn::Circle}, "Done", "Esc"}};
        ui::pad_hints(cv, 10.0f, 264.5f, hints, 10.0f, 10.0f, kWhite, gl);
    }

    // Modal box.
    if (g.modal != Modal::None) {
        cv.solid(0.0f, 0.0f, 480.0f, 240.0f, 0.0f, rgb(40, 40, 44), 0.35f);
        cv.solid(kMx + 2.0f, kMy + 3.0f, kMw, kMh, 8.0f, rgb(0, 0, 0), 0.25f);
        cv.vgrad(kMx, kMy, kMw, kMh, 8.0f, rgb(250, 250, 252), rgb(226, 226, 230));
        cv.solid(kMx, kMy, kMw, 3.0f, 1.5f, kRedTop);
        const char *title = g.modal == Modal::Restart ? "RESTART NEEDED" : g.modal == Modal::DrawDistance ? "EXPERIMENTAL SETTING" : "INSTALL UPDATE";
        cv.text(kMx + 14.0f, kMy + 22.0f, title, 12.0f, kInk);
        std::string l1, l2;
        if (g.modal == Modal::Restart) {
            l1 = "Some changes take effect after the game restarts.";
            l2 = g.ctx.can_restart ? "Your settings are saved either way." : "Please restart the game to apply them.";
        } else if (g.modal == Modal::DrawDistance) {
            l1 = "A higher draw distance is experimental and has bugs:";
            l2 = "some scenery may be drawn wrong or in the wrong place.";
        } else {
            l1 = "Download and install " + g.upd.tag + " now?";
            l2 = "The game restarts afterwards. Saves and settings stay.";
        }
        cv.text(kMx + 14.0f, kMy + 44.0f, cv.fit(l1, 10.0f, kMw - 28.0f), 10.0f, kInk);
        cv.text(kMx + 14.0f, kMy + 58.0f, cv.fit(l2, 10.0f, kMw - 28.0f), 10.0f, kMid);
        const int n = modal_buttons();
        for (int i = 0; i < n; ++i) {
            float x, y, w, h;
            modal_button_rect(i, n, x, y, w, h);
            const char *lab = g.modal == Modal::Restart ? (n == 1 ? "OK" : (i == 0 ? "RESTART NOW" : "LATER")) : g.modal == Modal::DrawDistance ? (i == 0 ? "TURN ON" : "CANCEL") : (i == 0 ? "UPDATE" : "CANCEL");
            if (i == g.modal_btn) red_pill(cv, x, y, w, h);
            else cv.vgrad(x, y, w, h, h * 0.5f, rgb(102, 102, 106), rgb(60, 60, 64));
            cv.text_centered(x + w * 0.5f, y + 15.0f, lab, 11.0f, kWhite);
        }
    }

    // Toast.
    if (g.frames < g.toast_until && !g.toast.empty()) {
        const float w = std::min(440.0f, cv.text_width(g.toast, 11.0f) + 30.0f);
        cv.solid(240.0f - w * 0.5f, 196.0f, w, 22.0f, 11.0f, rgb(40, 40, 44), 0.88f);
        cv.text_centered(240.0f, 211.0f, g.toast, 11.0f, kWhite);
    }
}

} // namespace

bool options_needs_restart(const PlayerSettings &s, const PlayerSettings &r) {
    return s.gpu != r.gpu || s.render_scale != r.render_scale || s.window_scale != r.window_scale || s.fps != r.fps || s.frame_skip != r.frame_skip ||
           s.smooth_menus != r.smooth_menus || s.low_latency != r.low_latency || s.aa != r.aa || s.mipmaps != r.mipmaps || s.online != r.online ||
           s.perf_report != r.perf_report || s.draw_distance != r.draw_distance;
}

void options_ui_begin(const PlayerSettings &now, const PlayerSettings &running, const OptionsContext &ctx) {
    const OptionsUpdateView keep = g.upd;
    g = State{};
    g.upd = keep;
    g.active = true;
    g.s = now;
    g.running = running;
    g.ctx = ctx;
    g.cats = build(ctx);
}

bool options_ui_active() { return g.active; }
const PlayerSettings &options_ui_settings() { return g.s; }
bool options_ui_take_live_change() { const bool c = g.live_change; g.live_change = false; return c; }
void options_ui_set_update(const OptionsUpdateView &v) { g.upd = v; }
void options_ui_toast(const std::string &text) { g.toast = text; g.toast_until = g.frames + 180u; }
void options_ui_end() { g.active = false; g.modal = Modal::None; }
std::uint32_t options_ui_take_events() { const std::uint32_t e = g.events; g.events = 0u; return e; }

OptionsRequest options_ui_update(const LobbyInput &in) {
    if (!g.active) return OptionsRequest::None;
    g.frames = g.ticks.next();
    g.out = OptionsRequest::None;
    handle_mouse(in);
    if (g.out == OptionsRequest::None) handle_pad(in);
    return g.out;
}

std::uint64_t options_ui_state_key() {
    std::uint64_t k = 1469598103934665603ull;
    const auto mixin = [&](std::uint64_t v) { k = (k ^ v) * 1099511628211ull; };
    mixin(g.active ? 1u : 0u); mixin(static_cast<std::uint64_t>(g.cat)); mixin(static_cast<std::uint64_t>(g.row)); mixin(g.in_rows ? 1u : 0u);
    mixin(static_cast<std::uint64_t>(g.hover_cat + 2)); mixin(static_cast<std::uint64_t>(g.hover_row + 2)); mixin(static_cast<std::uint64_t>(g.modal));
    mixin(static_cast<std::uint64_t>(g.modal_btn)); mixin(pad::generation()); mixin(static_cast<std::uint64_t>(g.upd.state)); mixin(static_cast<std::uint64_t>(g.upd.percent));
    mixin(g.frames < g.toast_until ? 1u : 0u);
    for (char c : g.toast) mixin(static_cast<unsigned char>(c));
    for (const auto &cat : g.cats) for (const auto &r : cat.rows) if (r.get) mixin(static_cast<std::uint64_t>(r.get(g.s) + 1000));
    return k;
}

void options_ui_render(std::uint8_t *rgba, int w, int h) {
    if (!g.active || rgba == nullptr || w < 480 || h < 272) return;
    static std::vector<std::uint8_t> native(480u * 272u * 4u);
    const Canvas cv{native.data(), 480, 272, 1.0f};
    render_page(cv);
    ui::upscale_crisp(native.data(), rgba, w, h);
}

} // namespace rr
