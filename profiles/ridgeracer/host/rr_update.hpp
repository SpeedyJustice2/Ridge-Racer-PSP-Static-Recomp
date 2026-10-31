// Built-in updater: asks GitHub for the newest release, downloads its Setup.exe, checks the SHA-256 and starts it.
// Windows only (the other platforms answer "no update").  Nothing is sent but the usual web request.
#pragma once
#include <atomic>
#include <string>

namespace rr::update {

struct Info {
    bool available{false};      // a newer release than this game exists
    std::string tag;            // the release's tag, e.g. "beta1.1"
    std::string url;            // download address of its Setup.exe
    std::string sha256;         // expected hash (lower case hex), empty = the release does not say
    std::string pkg_url;        // the small update package (RidgeRacer-Update.zip): the program without the game library
    std::string pkg_sha256;
    std::string notes;          // first lines of the release text
};

// Blocking (run it on a helper thread).  Returns false when GitHub could not be reached.
bool check(Info &out);
// True when version string `b` is newer than `a` (compares the numbers in them: "beta1" < "beta1.1" < "beta2").
bool newer(const std::string &a, const std::string &b);
// Downloads `url` to `dest`, updating `percent` (0..100), and verifies `sha256`.  Returns "" or an error message.
std::string download(const std::string &url, const std::string &sha256, const std::wstring &dest, std::atomic<int> &percent);
// Installs a downloaded update package into `install_dir` (the running program and SDL2.dll are renamed to *.old, which the
// next start removes).  If the package needs a newer game library than the installed one (its abi.txt differs) nothing is
// changed and `needs_setup` is set.  Returns "" or an error message.
std::string apply_package(const std::wstring &zip, const std::wstring &install_dir, bool &needs_setup);
// Starts the program again (after apply_package).
std::string restart(const std::wstring &install_dir);
// Deletes the *.old files an earlier update left next to the program.
void cleanup_old_files();
// Starts the downloaded Setup.exe with the current game folder and game path filled in.  Returns "" or an error message.
std::string run_setup(const std::wstring &setup, const std::wstring &install_dir, const std::wstring &game_path);

} // namespace rr::update
