// Player settings for the launcher window (ridgeracer.ini next to the executable).
//
// The settings are turned into ordinary command-line options (to_args) that are placed in front of any options typed on
// the command line, so a typed option still wins.  The launcher is used only when the game is started with no arguments
// at all (a double-click) or with --launcher; play.ps1 and the test rig, which pass the game path, never see it.
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace rr {

struct PlayerSettings {
    std::string game_path;        // the decrypted BOOT.BIN (or a folder that holds PSP_GAME)
    bool show_launcher = true;    // open the settings window at start (holding Shift opens it anyway)
    bool gpu = true;              // OpenGL renderer; false = the software renderer (original look, 480x272)
    int render_scale = 4;         // internal resolution, x 480x272 (1-8)
    int window_scale = 3;         // window size, x 480x272
    bool fullscreen = false;      // borderless fullscreen (F11 / Alt+Enter switch in game)
    int fps = 0;                  // race frame rate; 0 = match the monitor, 60 = the PSP's own rate
    bool low_latency = false;     // wait for the GPU after every frame (less input delay, can stutter at high settings)
    int aa = 3;                   // 0 off, 1 FXAA, 2 MSAA 2x, 3 MSAA 4x, 4 MSAA 8x
    int aniso = 16;               // 1 (off), 2, 4, 8, 16
    bool mipmaps = true;
    int sharpen = 0;              // 0 off, 1 mild, 2 medium, 3 strong
    int ui_crisp = 2;             // 0 off (the game's own soft HUD), 1 sharper, 2 crispest
    bool smooth_cutouts = true;   // smooth the edges of trees, fences and other cut-out art
    int volume = 100;             // percent (0-150)
    int menu_volume = 100;        // percent (0-200)
};

std::filesystem::path settings_file_path();   // ridgeracer.ini beside the executable
PlayerSettings load_settings(const std::filesystem::path &file);
bool save_settings(const std::filesystem::path &file, const PlayerSettings &s);

// Find BOOT.BIN from what the player picked (the file itself, a folder holding PSP_GAME, PSP_GAME, SYSDIR, or a
// folder holding RidgeRacerPSPFiles).  Empty if not found.
std::filesystem::path resolve_game_path(const std::filesystem::path &picked);
// Search the executable's folder and the folders above it for the game files.  Empty if not found.
std::filesystem::path find_game_files();
// The folder the executable lives in (where settings and the default savedata folder go).
std::filesystem::path executable_dir();
// A picked path that is a disc image (.iso).  The game files are copied out of it once, into a "game" folder next to the
// executable (see prepare_game); resolve_game_path then points at the copy.
bool is_iso_path(const std::filesystem::path &p);
// Copies the files out of the .iso when that has not been done for this image yet (progress gets 0..100).  Does nothing for
// folders / BOOT.BIN paths.  Returns an empty string on success, otherwise a short reason for the player.
std::string prepare_game(const std::filesystem::path &picked, const std::function<void(int)> &progress);
// Empty when the file is usable, otherwise a short reason for the player.
std::string check_game_file(const std::filesystem::path &boot_bin);

// The command-line options these settings stand for (the game path first).  monitor_hz is used for fps = 0.
std::vector<std::string> to_args(const PlayerSettings &s, int monitor_hz);

// Implemented per platform (rr_launcher_win32.cpp / rr_launcher_null.cpp).
// Shows the settings window; returns false if the player closed it without pressing Play.
bool run_launcher(PlayerSettings &s);
bool launcher_shift_held();
int monitor_refresh_hz();     // the main monitor's refresh rate (60 when unknown)
// A double-clicked game gets a console window of its own: hide it (logging then goes to ridgeracer.log).
void hide_own_console();
// A message box for players who started the game without a console (does nothing elsewhere).
void show_error_box(const std::string &message);

} // namespace rr
