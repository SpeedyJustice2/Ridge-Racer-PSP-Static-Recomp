#pragma once

// Crash reports and one-file bug reports for testers.
//
//  - A crash (an unhandled exception in the game itself) writes reports\crash-<time>.txt (what failed and where, the call
//    stack, what the game was doing, the last log lines) and, on Windows, reports\crash-<time>.dmp (a minidump for a
//    debugger), then starts the game once more in report mode, which packs everything into a bug report and says where it is.
//  - A bug report is one zip on the Desktop (Windows; next to the game elsewhere): logs (this and the previous session),
//    settings (password removed), performance report, online log, the newest crash reports, a system summary and, from the
//    in-game hot key, a screenshot.  Made from the settings window ("Bug report..."), with Ctrl+F12 in game, after a crash
//    and after the game stops on an error.

#include <filesystem>
#include <string>

namespace rr::report {

// Call once, early in main.
void install_crash_handler();
// Directory for crash files (reports\ beside the game).
std::filesystem::path reports_dir();
// What the game was doing, for crash files and system.txt (set by the HLE layer; must be safe to call at any time).
void set_state_text_provider(std::string (*fn)());
// Builds the zip and returns its path (empty on failure).  `screenshot` is added when not empty.
std::filesystem::path make_bug_report(const std::string &reason, const std::filesystem::path &screenshot = {});
// The same on a background thread, then a message with the file's place (Windows: a message box, the folder opens).
void make_bug_report_async(const std::string &reason, const std::filesystem::path &screenshot = {});
// Tells the player where the report is (message box on Windows, stderr elsewhere); `crashed` changes the wording.
void announce(const std::filesystem::path &zip, bool crashed);
// The game showed nothing new for `seconds`: reports\freeze-<time>.txt (state and the last log lines).
void write_freeze_report(unsigned seconds);
// "RidgeRacerNative --bug-report <reason>": builds a report, announces it, returns the exit code.
int run_report_mode(const std::string &reason);

} // namespace rr::report
