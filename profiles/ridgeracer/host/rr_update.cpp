#include "rr_update.hpp"
#include "rr_http.hpp"
#include "rr_version.hpp"
#include "../gen/rr_gen_abi_version.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#ifdef _MSC_VER
#pragma comment(lib, "bcrypt.lib")
#endif
#endif

namespace rr::update {
namespace {

const char *kLatestUrl = "https://api.github.com/repos/ngrshow/Ridge-Racer-PSP-Static-Recomp/releases/latest";

std::vector<int> numbers_of(const std::string &s) {
    std::vector<int> v;
    for (std::size_t i = 0; i < s.size();) {
        if (std::isdigit(static_cast<unsigned char>(s[i]))) {
            int n = 0;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) { n = std::min(n * 10 + (s[i] - '0'), 100000); ++i; }
            v.push_back(n);
        } else ++i;
    }
    return v;
}

// The text of a JSON string value `"key":"..."` at or after `from` (very small parser: the release data is plain).
std::string json_string(const std::string &j, const std::string &key, std::size_t from, std::size_t *end = nullptr) {
    const std::string k = "\"" + key + "\"";
    std::size_t p = j.find(k, from);
    if (p == std::string::npos) return {};
    p = j.find(':', p + k.size());
    if (p == std::string::npos) return {};
    p = j.find('"', p);
    if (p == std::string::npos) return {};
    std::string out;
    for (++p; p < j.size() && j[p] != '"'; ++p) {
        if (j[p] == '\\' && p + 1 < j.size()) {
            ++p;
            out.push_back(j[p] == 'n' ? '\n' : j[p] == 'r' ? '\r' : j[p] == 't' ? ' ' : j[p]);
        } else out.push_back(j[p]);
    }
    if (end) *end = p;
    return out;
}

bool is_hex64(const std::string &s) {
    if (s.size() != 64) return false;
    return std::all_of(s.begin(), s.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
}

std::string lower(std::string s) { for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }

}   // namespace

bool newer(const std::string &a, const std::string &b) {
    const auto x = numbers_of(a), y = numbers_of(b);
    for (std::size_t i = 0; i < std::max(x.size(), y.size()); ++i) {
        const int p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
        if (p != q) return q > p;
    }
    return false;
}

bool check(Info &out) {
    out = {};
    const char *over = std::getenv("RR_UPDATE_URL");   // (tests)
    const net::HttpResult r = net::http_request("GET", over != nullptr ? over : kLatestUrl, 8000);
    if (r.status != 200) return false;
    const std::string &j = r.body;
    out.tag = json_string(j, "tag_name", 0);
    if (out.tag.empty()) return false;
    // the assets: Setup.exe (full install) and RidgeRacer-Update.zip (program only)
    std::size_t at = 0, prev_end = 0;
    for (;;) {
        const std::size_t p = j.find("\"browser_download_url\"", at);
        if (p == std::string::npos) break;
        std::size_t end = p;
        const std::string url = json_string(j, "browser_download_url", p, &end);
        at = end + 1;
        std::string hash;
        // (the asset's "digest":"sha256:<hex>" comes before its download address)
        const std::size_t d = j.rfind("\"digest\"", p);
        if (d != std::string::npos && d >= prev_end) {
            const std::string dg = lower(json_string(j, "digest", d));
            if (dg.rfind("sha256:", 0) == 0 && is_hex64(dg.substr(7))) hash = dg.substr(7);
        }
        prev_end = at;
        if (url.rfind("https://", 0) != 0) continue;
        const std::string tail = lower(url.substr(url.find_last_of('/') + 1));
        if (tail == "setup.exe") { out.url = url; out.sha256 = hash; }
        else if (tail == "ridgeracer-update.zip") { out.pkg_url = url; out.pkg_sha256 = hash; }
    }
    const std::string body = json_string(j, "body", 0);
    if (out.sha256.empty()) {   // fall back to the hash line in the release text
        for (std::size_t i = 0; i + 64 <= body.size(); ++i) {
            if (is_hex64(body.substr(i, 64)) && (i == 0 || !std::isxdigit(static_cast<unsigned char>(body[i - 1]))) &&
                (i + 64 == body.size() || !std::isxdigit(static_cast<unsigned char>(body[i + 64])))) { out.sha256 = lower(body.substr(i, 64)); break; }
        }
    }
    out.notes = body.substr(0, 600);
    out.available = (!out.url.empty() || !out.pkg_url.empty()) && newer(RR_GAME_VERSION, out.tag);
    return true;
}

#ifdef _WIN32
namespace {

std::string sha256_of_file(const std::wstring &path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 && BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) >= 0) {
        std::vector<unsigned char> buf(1u << 20);
        DWORD n = 0;
        bool ok = true;
        while (ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr) && n > 0)
            if (BCryptHashData(h, buf.data(), n, 0) < 0) { ok = false; break; }
        unsigned char dg[32];
        if (ok && BCryptFinishHash(h, dg, 32, 0) >= 0) {
            static const char *digits = "0123456789abcdef";
            for (unsigned char c : dg) { hex.push_back(digits[c >> 4]); hex.push_back(digits[c & 15]); }
        }
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return hex;
}

}   // namespace

std::string download(const std::string &url, const std::string &sha256, const std::wstring &dest, std::atomic<int> &percent) {
    percent = 0;
    if (url.empty()) return "The release has no download for this.";
    if (sha256.empty()) return "The release does not list a SHA-256 hash, so the download cannot be checked.";
    const std::wstring wurl(url.begin(), url.end());
    URL_COMPONENTSW uc{};
    uc.dwStructSize = sizeof uc;
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2047;
    uc.dwSchemeLength = 1;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return "Bad download address.";
    std::string err = "Could not download the update.";
    HINTERNET session = WinHttpOpen(L"RidgeRacerPort/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return err;
    WinHttpSetTimeouts(session, 15000, 15000, 30000, 30000);
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;   // (GitHub sends the file from another host over https)
    WinHttpSetOption(session, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof redirect);
    HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0);
    if (conn) {
        HINTERNET req = WinHttpOpenRequest(conn, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
        if (req) {
            if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr)) {
                DWORD status = 0, sz = sizeof status;
                WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
                DWORD64 total = 0;
                wchar_t lenbuf[32] = {};
                sz = sizeof lenbuf;
                if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lenbuf, &sz, WINHTTP_NO_HEADER_INDEX)) total = _wcstoui64(lenbuf, nullptr, 10);
                HANDLE out = CreateFileW(dest.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (status != 200) err = "The download failed (HTTP " + std::to_string(status) + ").";
                else if (out == INVALID_HANDLE_VALUE) err = "Could not write to the temp folder.";
                else {
                    DWORD64 got_total = 0;
                    bool ok = true;
                    std::vector<char> buf(1u << 16);
                    for (;;) {
                        DWORD n = 0;
                        if (!WinHttpReadData(req, buf.data(), static_cast<DWORD>(buf.size()), &n)) { ok = false; break; }
                        if (n == 0) break;
                        DWORD w = 0;
                        if (!WriteFile(out, buf.data(), n, &w, nullptr) || w != n) { ok = false; err = "Not enough free space in the temp folder."; break; }
                        got_total += n;
                        if (total > 0) percent = static_cast<int>(std::min<DWORD64>(99, got_total * 100 / total));
                    }
                    CloseHandle(out);
                    out = INVALID_HANDLE_VALUE;
                    if (ok) {
                        const std::string have = sha256_of_file(dest);
                        if (have == lower(sha256)) { err.clear(); percent = 100; }
                        else { DeleteFileW(dest.c_str()); err = "The downloaded file does not match the published SHA-256 hash, so it was thrown away."; }
                    } else if (err == "Could not download the update.") err = "The download was interrupted.";
                }
                if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
            }
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(conn);
    }
    WinHttpCloseHandle(session);
    return err;
}

namespace {
bool run_hidden(const std::wstring &cmd, DWORD &code) {
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> c(cmd.begin(), cmd.end());
    c.push_back(0);
    if (!CreateProcessW(nullptr, c.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, 120000);
    code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}
}   // namespace

std::string apply_package(const std::wstring &zip, const std::wstring &install_dir, bool &needs_setup) {
    needs_setup = false;
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path stage = fs::path(install_dir) / L"update-staging";
    fs::remove_all(stage, ec);
    fs::create_directories(stage, ec);
    if (ec) return "Could not create a temporary folder next to the game.";
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    DWORD code = 1;
    if (!run_hidden(std::wstring(L"\"") + sys + L"\\tar.exe\" -xf \"" + zip + L"\" -C \"" + stage.wstring() + L"\"", code) || code != 0) {
        fs::remove_all(stage, ec);
        return "Could not unpack the update (Windows 10 or newer is required).";
    }
    // the package says which game library it works with
    std::uint32_t abi = 0;
    {
        FILE *f = _wfopen((stage / L"abi.txt").c_str(), L"rb");
        if (f != nullptr) { char b[32] = {}; const std::size_t n = std::fread(b, 1, sizeof b - 1, f); std::fclose(f); (void)n; abi = static_cast<std::uint32_t>(std::strtoul(b, nullptr, 10)); }
    }
    if (abi == 0 || !fs::exists(stage / L"RidgeRacerNative.exe", ec)) { fs::remove_all(stage, ec); return "The update package is damaged."; }
    if (abi != rr::gen::kAbiVersion) { fs::remove_all(stage, ec); needs_setup = true; return {}; }
    // swap: the running program cannot be overwritten but can be renamed
    std::vector<std::pair<fs::path, bool>> done;   // (target, had an old file)
    for (const auto &e : fs::directory_iterator(stage, ec)) {
        if (!e.is_regular_file(ec)) continue;
        const std::wstring name = e.path().filename().wstring();
        if (name == L"abi.txt") continue;
        const fs::path target = fs::path(install_dir) / name;
        const fs::path old = target.wstring() + L".old";
        bool had = false;
        if (fs::exists(target, ec)) {
            DeleteFileW(old.c_str());
            if (!MoveFileExW(target.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                for (const auto &[t, h] : done) { const fs::path o = t.wstring() + L".old"; DeleteFileW(t.c_str()); if (h) MoveFileExW(o.c_str(), t.c_str(), MOVEFILE_REPLACE_EXISTING); }
                fs::remove_all(stage, ec);
                return "Could not replace " + std::string(name.begin(), name.end()) + " (is it in use?).";
            }
            had = true;
        }
        if (!MoveFileExW(e.path().c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            if (had) MoveFileExW(old.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING);
            for (const auto &[t, h] : done) { const fs::path o = t.wstring() + L".old"; DeleteFileW(t.c_str()); if (h) MoveFileExW(o.c_str(), t.c_str(), MOVEFILE_REPLACE_EXISTING); }
            fs::remove_all(stage, ec);
            return "Could not install " + std::string(name.begin(), name.end()) + ".";
        }
        done.emplace_back(target, had);
    }
    fs::remove_all(stage, ec);
    return {};
}

std::string restart(const std::wstring &install_dir) {
    const std::wstring exe = install_dir + L"\\RidgeRacerNative.exe";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmd(exe.begin(), exe.end());
    cmd.insert(cmd.begin(), L'"'); cmd.push_back(L'"'); cmd.push_back(0);
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, install_dir.c_str(), &si, &pi)) return "The update is installed, but the game could not be started again. Please start it yourself.";
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return {};
}

void cleanup_old_files() {
    wchar_t exe[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::path(std::wstring(exe, n)).parent_path();
    for (const auto &e : std::filesystem::directory_iterator(dir, ec))
        if (e.path().extension() == L".old") DeleteFileW(e.path().c_str());
    std::filesystem::remove_all(dir / L"update-staging", ec);
}

std::string run_setup(const std::wstring &setup, const std::wstring &install_dir, const std::wstring &game_path) {
    // Setup.exe picks these up and fills in its pages (installer.ps1).
    SetEnvironmentVariableW(L"RR_UPDATE_DIR", install_dir.c_str());
    SetEnvironmentVariableW(L"RR_UPDATE_GAME", game_path.c_str());
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmd(setup.begin(), setup.end());
    cmd.insert(cmd.begin(), L'"'); cmd.push_back(L'"'); cmd.push_back(0);
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return "Could not start the downloaded Setup.exe.";
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return {};
}
#else
std::string download(const std::string &, const std::string &, const std::wstring &, std::atomic<int> &) { return "Updating is only built in on Windows."; }
std::string apply_package(const std::wstring &, const std::wstring &, bool &) { return "Updating is only built in on Windows."; }
std::string restart(const std::wstring &) { return "Updating is only built in on Windows."; }
void cleanup_old_files() {}
std::string run_setup(const std::wstring &, const std::wstring &, const std::wstring &) { return "Updating is only built in on Windows."; }
#endif

} // namespace rr::update
