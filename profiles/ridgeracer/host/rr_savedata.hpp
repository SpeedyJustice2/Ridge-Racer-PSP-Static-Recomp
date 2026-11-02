#pragma once

// Save data export / import (Options > RECOMP OPTIONS > SAVE DATA).
//
// The game keeps one save: <savedata folder>\ULUS10001000\DATA.BIN (16132 bytes: CRC-32 of the rest, then the data).
//  - Export writes a copy of it to a file the player chooses (Windows: a Save window starting at the Desktop).
//  - Import reads a file the player chooses (an exported save, or DATA.BIN from another copy of the port / an older install),
//    checks it, backs the current save up to <savedata folder>\backups\, writes it, and asks for a restart (the game holds its
//    save in memory and would write the old progress back).
//  - Encrypted saves from a real PSP or PPSSPP are recognised and refused with a message (not supported yet).
// The windows run on a background thread; the caller polls take_result() once a frame.

#include <filesystem>
#include <string>

namespace rr::savedata {

// Set once at start (the folder the savedata HLE uses).
void set_root(const std::filesystem::path &root);
[[nodiscard]] std::filesystem::path root();
[[nodiscard]] std::filesystem::path data_file();   // root/ULUS10001000/DATA.BIN

enum class Check { Ok, Missing, Encrypted, WrongSize, BadChecksum };
[[nodiscard]] Check check_file(const std::filesystem::path &file);

[[nodiscard]] bool busy();
void export_async();
void import_async();

struct Result {
    bool ok{false};
    bool restart{false};    // a save was imported: restart the game now
    std::string message;    // for the page's toast
};
// True once when an export / import finished (or was cancelled: ok = false, message empty).
bool take_result(Result &out);

// Without windows (tests, other platforms): export to / import from these files.
bool export_to(const std::filesystem::path &file, std::string &message);
bool import_from(const std::filesystem::path &file, std::string &message);

} // namespace rr::savedata
