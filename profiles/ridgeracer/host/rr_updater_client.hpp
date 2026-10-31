// The game's side of the updater: starts RidgeRacerUpdater.exe (rr_updater_main.cpp) and reads its status file.  The game
// program itself has no download or install code (see rr_updater_main.cpp for why).  Windows only; elsewhere nothing works
// and updater_available() is false.
#pragma once
#include <map>
#include <string>

namespace rr {

// RidgeRacerUpdater.exe is next to the game.
bool updater_available();
// Starts "check" / "install" (the updater writes its progress to a new status file whose path is returned in `status`).
bool updater_start_check(std::string &status);
bool updater_start_install(std::string &status, const std::string &game_path_utf8);
// Starts the updater to restart the game once this process has closed (settings that need a restart).
bool updater_start_restart();
// Reads the status file ("key=value" lines).  False while it does not exist yet.
bool updater_read(const std::string &status, std::map<std::string, std::string> &out);
// Deletes the files an update left behind (*.old next to the program, update-staging).
void cleanup_update_leftovers();

} // namespace rr
