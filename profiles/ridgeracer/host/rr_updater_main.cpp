// RidgeRacerUpdater.exe: the port's updater as a program of its own (Windows).
//
// The game itself never talks to GitHub or downloads anything: an unsigned game that does got stopped by antivirus heuristics
// (Windows Defender "Bearfoos.A!ml", 2026-10-31).  The game starts this helper and reads what it writes to a status file:
//
//   RidgeRacerUpdater.exe check   <status file>
//   RidgeRacerUpdater.exe install <status file> <game process id> <game path>
//   RidgeRacerUpdater.exe restart <game process id>
//
// Status file: "key=value" lines, replaced as a whole (written to a temporary file, then renamed):
//   state=checking | available | uptodate | unreachable | downloading | restarting | setup | failed | done
//   tag=<newest version>   percent=<0..100>   message=<why it failed>
// install: download + check + install the update package (or download the full Setup.exe), then wait for the game to close
// (it closes when it reads "restarting" / "setup") and start the new game (or the Setup) with the game folder filled in.
// restart: wait for the game to close, then start it again (settings that need a restart).
//
// RR_UPDATE_URL (tests) asks another release than GitHub's latest one; it is not passed on to the restarted game.

#include "rr_update.hpp"

#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace {

std::wstring g_status;

void write_status(const std::string &text) {
    if (g_status.empty()) return;
    const std::wstring tmp = g_status + L".tmp";
    FILE *f = _wfopen(tmp.c_str(), L"wb");
    if (f == nullptr) return;
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    MoveFileExW(tmp.c_str(), g_status.c_str(), MOVEFILE_REPLACE_EXISTING);
}

std::string one_line(std::string s) {
    for (char &c : s) if (c == '\r' || c == '\n') c = ' ';
    return s;
}

void wait_for_exit(DWORD pid, DWORD ms) {
    HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (p == nullptr) return;   // (already gone)
    WaitForSingleObject(p, ms);
    CloseHandle(p);
}

int run_check() {
    write_status("state=checking\n");
    rr::update::Info info;
    if (!rr::update::check(info)) { write_status("state=unreachable\n"); return 1; }
    const std::string src = "source=" + rr::update::source_url() + "\n";
    if (info.available) write_status("state=available\ntag=" + info.tag + "\n" + src);
    else write_status("state=uptodate\ntag=" + info.tag + "\n" + src);
    return 0;
}

int run_install(DWORD pid, const std::wstring &game_path) {
    write_status("state=checking\n");
    rr::update::Info info;
    if (!rr::update::check(info)) { write_status("state=failed\nmessage=GitHub could not be reached.\n"); return 1; }
    if (!info.available) { write_status("state=uptodate\ntag=" + info.tag + "\n"); return 0; }
    std::atomic<int> percent{0};
    std::atomic<bool> busy{true};
    std::thread ticker([&] {
        while (busy) {
            write_status("state=downloading\ntag=" + info.tag + "\npercent=" + std::to_string(percent.load()) + "\n");
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    });
    bool replaced = false;
    std::wstring setup;
    const std::string err = rr::update::install(info, percent, replaced, setup);
    busy = false;
    ticker.join();
    if (!err.empty()) { write_status("state=failed\ntag=" + info.tag + "\nmessage=" + one_line(err) + "\n"); return 1; }
    const std::wstring dir = rr::update::program_dir();
    if (replaced) {
        DeleteFileW((dir + L"\\update-test-url.txt").c_str());   // (a test: the new game asks for the real latest release again)
        write_status("state=restarting\ntag=" + info.tag + "\n");
        wait_for_exit(pid, 60000);
        const std::string e = rr::update::restart(dir);
        write_status(e.empty() ? "state=done\ntag=" + info.tag + "\n" : "state=failed\nmessage=" + one_line(e) + "\n");
        return e.empty() ? 0 : 1;
    }
    write_status("state=setup\ntag=" + info.tag + "\n");
    wait_for_exit(pid, 60000);
    const std::string e = rr::update::run_setup(setup, dir, game_path);
    write_status(e.empty() ? "state=done\ntag=" + info.tag + "\n" : "state=failed\nmessage=" + one_line(e) + "\n");
    return e.empty() ? 0 : 1;
}

int run_restart(DWORD pid) {
    wait_for_exit(pid, 60000);
    return rr::update::restart(rr::update::program_dir()).empty() ? 0 : 1;
}

} // namespace

int main() {
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) return 2;
    rr::update::cleanup_old_files();
    if (argc < 2) return 2;
    const std::wstring mode = argv[1];
    if (mode == L"check" && argc >= 3) { g_status = argv[2]; return run_check(); }
    if (mode == L"install" && argc >= 4) {
        g_status = argv[2];
        return run_install(static_cast<DWORD>(std::wcstoul(argv[3], nullptr, 10)), argc >= 5 ? std::wstring(argv[4]) : std::wstring());
    }
    if (mode == L"restart" && argc >= 3) return run_restart(static_cast<DWORD>(std::wcstoul(argv[2], nullptr, 10)));
    return 2;
}
