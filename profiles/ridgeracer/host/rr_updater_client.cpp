#include "rr_updater_client.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace rr {
namespace fs = std::filesystem;

#if defined(_WIN32)
namespace {
fs::path program_dir() {
    wchar_t exe[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    return fs::path(std::wstring(exe, n)).parent_path();
}
fs::path helper() { return program_dir() / L"RidgeRacerUpdater.exe"; }

bool launch(const std::wstring &args) {
    std::wstring cmd = L"\"" + helper().wstring() + L"\" " + args;
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::wstring dir = program_dir().wstring();
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

std::wstring new_status_file() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const fs::path dir = fs::path(tmp) / L"RidgeRacerUpdate";
    std::error_code ec;
    fs::create_directories(dir, ec);
    static unsigned n = 0;
    const fs::path f = dir / (L"status-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++n) + L".txt");
    fs::remove(f, ec);
    return f.wstring();
}
std::string narrow(const std::wstring &w) { const std::u8string u = fs::path(w).u8string(); return std::string(u.begin(), u.end()); }
}   // namespace

bool updater_available() { std::error_code ec; return fs::is_regular_file(helper(), ec); }

bool updater_start_check(std::string &status) {
    if (!updater_available()) return false;
    const std::wstring f = new_status_file();
    status = narrow(f);
    return launch(L"check \"" + f + L"\"");
}

bool updater_start_install(std::string &status, const std::string &game_path_utf8) {
    if (!updater_available()) return false;
    const std::wstring f = new_status_file();
    status = narrow(f);
    const std::wstring gp = fs::path(std::u8string(game_path_utf8.begin(), game_path_utf8.end())).wstring();
    return launch(L"install \"" + f + L"\" " + std::to_wstring(GetCurrentProcessId()) + L" \"" + gp + L"\"");
}

bool updater_start_restart() {
    if (!updater_available()) return false;
    return launch(L"restart " + std::to_wstring(GetCurrentProcessId()));
}

void cleanup_update_leftovers() {
    std::error_code ec;
    const fs::path dir = program_dir();
    for (const auto &e : fs::directory_iterator(dir, ec))
        if (e.path().extension() == L".old") DeleteFileW(e.path().c_str());
    fs::remove_all(dir / L"update-staging", ec);
}
#else
bool updater_available() { return false; }
bool updater_start_check(std::string &) { return false; }
bool updater_start_install(std::string &, const std::string &) { return false; }
bool updater_start_restart() { return false; }
void cleanup_update_leftovers() {}
#endif

bool updater_read(const std::string &status, std::map<std::string, std::string> &out) {
    out.clear();
    std::ifstream in(fs::path(std::u8string(status.begin(), status.end())));
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto eq = line.find('=');
        if (eq != std::string::npos) out[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return !out.empty();
}

} // namespace rr
