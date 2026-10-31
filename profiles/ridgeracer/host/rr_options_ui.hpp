#pragma once

// RECOMP OPTIONS: the port's own settings page, opened from the game's Options screen (the row that replaces BACK, see
// rr_hle.cpp "recomp options row").  It holds what the old settings window (launcher) held: display, graphics, sound,
// controls, online, updates and help.  Drawn in the game's Options look like the on-screen keyboard and the server
// browser.  Pure logic + a CPU renderer: the caller feeds input once a frame, applies the changes and carries out requests.

#include <cstdint>
#include <string>

#include "rr_lobby_ui.hpp"   // LobbyInput
#include "rr_settings.hpp"

namespace rr {

struct OptionsContext {
    int monitor_hz{60};
    bool can_restart{false};   // the game was started from its shortcut (settings come from ridgeracer.ini): it can restart itself
    bool can_update{false};    // the built-in updater works on this platform
    std::string version;       // "beta 1.2"
};

enum class OptionsRequest {
    None,
    Close,          // done: save the settings and go back to the game
    Restart,        // done: save the settings and restart the game (changes that need a restart)
    CheckUpdate,    // look for a newer version
    Update,         // download and install the newer version (the player confirmed)
    BugReport,      // make a bug report (asks where to save it)
    OpenIssues,     // open the GitHub bug report page
    OpenDiscord,    // open the community Discord invite
    OpenScreenshots,   // open the screenshots folder
};

// What the updater is doing (shown on the Updates rows).
struct OptionsUpdateView {
    enum State { Idle, Checking, UpToDate, Available, Downloading, Failed, Unreachable } state{Idle};
    std::string tag;       // newer version (Available / Downloading)
    int percent{0};
    std::string message;   // Failed: why
};

// `now` = the settings to edit (ridgeracer.ini), `running` = what the game started with (decides "needs a restart").
void options_ui_begin(const PlayerSettings &now, const PlayerSettings &running, const OptionsContext &ctx);
[[nodiscard]] bool options_ui_active();
OptionsRequest options_ui_update(const LobbyInput &in);
// The edited settings.
[[nodiscard]] const PlayerSettings &options_ui_settings();
// True once after a setting changed that takes effect at once (volume, button pictures, ...): apply options_ui_settings().
[[nodiscard]] bool options_ui_take_live_change();
void options_ui_set_update(const OptionsUpdateView &v);
// A short message in the middle of the page for a few seconds (e.g. "Bug report saved").
void options_ui_toast(const std::string &text);
void options_ui_end();
// Sound feedback, like osk_ui_take_events (kOskMove / kOskAccept / kOskCancel).
[[nodiscard]] std::uint32_t options_ui_take_events();
void options_ui_render(std::uint8_t *rgba, int w, int h);
[[nodiscard]] std::uint64_t options_ui_state_key();
// True when a setting differs from what the game is running with and only takes effect after a restart.
[[nodiscard]] bool options_needs_restart(const PlayerSettings &s, const PlayerSettings &running);

} // namespace rr
