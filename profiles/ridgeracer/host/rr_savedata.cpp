#include "rr_savedata.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include "rr_log.hpp"
#include "rr_settings.hpp"   // executable_dir

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#endif

namespace fs = std::filesystem;

namespace rr::savedata {
namespace {

constexpr std::size_t kSaveSize = 16132u;        // the port's (plain) DATA.BIN
constexpr std::size_t kEncryptedSize = kSaveSize + 16u;   // a real PSP / PPSSPP save: 16-byte header, encrypted body

std::mutex g_mu;
fs::path g_root;
std::atomic<bool> g_busy{false};
bool g_has_result = false;
Result g_result;

std::uint32_t crc32(const std::uint8_t *p, std::size_t n) {
    static std::uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (std::uint32_t i = 0; i < 256; ++i) { std::uint32_t c = i; for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1; table[i] = c; }
        init = true;
    }
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 255u] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

bool read_all(const fs::path &p, std::vector<std::uint8_t> &out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff n = f.tellg();
    if (n < 0 || n > (1 << 20)) return false;
    f.seekg(0);
    out.resize(static_cast<std::size_t>(n));
    f.read(reinterpret_cast<char *>(out.data()), n);
    return static_cast<bool>(f);
}

bool write_all(const fs::path &p, const std::vector<std::uint8_t> &d) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.parent_path() / (p.filename().string() + ".tmp");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(reinterpret_cast<const char *>(d.data()), static_cast<std::streamsize>(d.size()));
        if (!f) return false;
    }
    fs::rename(tmp, p, ec);
    if (ec) { fs::remove(p, ec); ec.clear(); fs::rename(tmp, p, ec); }
    return !ec;
}

Check check_bytes(const std::vector<std::uint8_t> &d) {
    if (d.size() == kEncryptedSize) return Check::Encrypted;
    if (d.size() != kSaveSize) return Check::WrongSize;
    const std::uint32_t stored = static_cast<std::uint32_t>(d[0]) | (static_cast<std::uint32_t>(d[1]) << 8) | (static_cast<std::uint32_t>(d[2]) << 16) | (static_cast<std::uint32_t>(d[3]) << 24);
    return crc32(d.data() + 4, d.size() - 4) == stored ? Check::Ok : Check::BadChecksum;
}

std::string stamp(const char *fmt) {
    const std::time_t t = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char b[64];
    std::strftime(b, sizeof b, fmt, &tmv);
    return b;
}

std::string why(Check c) {
    switch (c) {
    case Check::Ok: return {};
    case Check::Missing: return "That file could not be read.";
    case Check::Encrypted: return "That is an encrypted save from a PSP or PPSSPP. Those can't be imported yet.";
    case Check::WrongSize: return "That file is not a Ridge Racer save (wrong size).";
    case Check::BadChecksum: return "That save is damaged (its checksum does not match).";
    }
    return {};
}

void finish(bool ok, bool restart, const std::string &msg) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_result = Result{ok, restart, msg};
    g_has_result = true;
    g_busy = false;
}

#if defined(_WIN32)
fs::path desktop_dir() {
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &p)) && p != nullptr) {
        fs::path d(p);
        CoTaskMemFree(p);
        std::error_code ec;
        if (fs::is_directory(d, ec)) return d;
    }
    return executable_dir();
}
// The standard Save / Open window.  Returns false when cancelled; `out` empty when no window could be shown.
bool ask_file(bool save, const std::wstring &title, const std::wstring &name, fs::path &out) {
    out.clear();
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool cancelled = false;
    IFileDialog *d = nullptr;
    HRESULT made = E_FAIL;
    if (save) { IFileSaveDialog *sd = nullptr; made = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sd)); d = sd; }
    else { IFileOpenDialog *od = nullptr; made = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&od)); d = od; }
    if (SUCCEEDED(made) && d != nullptr) {
        static const GUID kGuid = {0x5a3c9e21, 0x6d4b, 0x4f0e, {0x8a, 0x52, 0x52, 0x41, 0x56, 0x45, 0x44, 0x31}};   // (remembers the folder)
        d->SetClientGuid(kGuid);
        d->SetTitle(title.c_str());
        const COMDLG_FILTERSPEC types[] = {{L"Ridge Racer save (*.bin)", L"*.bin"}, {L"All files (*.*)", L"*.*"}};
        d->SetFileTypes(2, types);
        d->SetDefaultExtension(L"bin");
        if (!name.empty()) d->SetFileName(name.c_str());
        IShellItem *folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(desktop_dir().c_str(), nullptr, IID_PPV_ARGS(&folder))) && folder != nullptr) { d->SetDefaultFolder(folder); folder->Release(); }
        const HRESULT shown = d->Show(GetForegroundWindow());
        if (SUCCEEDED(shown)) {
            IShellItem *item = nullptr;
            if (SUCCEEDED(d->GetResult(&item)) && item != nullptr) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr) { out = fs::path(path); CoTaskMemFree(path); }
                item->Release();
            }
        } else if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) cancelled = true;
        d->Release();
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return !cancelled;
}
#endif

} // namespace

void set_root(const fs::path &r) { std::lock_guard<std::mutex> lk(g_mu); g_root = r; }
fs::path root() { std::lock_guard<std::mutex> lk(g_mu); return g_root.empty() ? executable_dir() / "savedata" : g_root; }
fs::path data_file() { return root() / "ULUS10001000" / "DATA.BIN"; }

Check check_file(const fs::path &file) {
    std::vector<std::uint8_t> d;
    if (!read_all(file, d)) return Check::Missing;
    return check_bytes(d);
}

bool busy() { return g_busy.load(); }

bool export_to(const fs::path &file, std::string &message) {
    std::vector<std::uint8_t> d;
    if (!read_all(data_file(), d)) { message = "There is no save to export yet (play until the game has saved once)."; return false; }
    if (check_bytes(d) != Check::Ok) { message = "Your save file looks damaged, so it was not exported."; return false; }
    if (!write_all(file, d)) { message = "The file could not be written there."; return false; }
    message = "Save exported: " + file.filename().string();
    log_line(category::kHle, "SAVE export: " + file.string());
    return true;
}

bool import_from(const fs::path &file, std::string &message) {
    std::vector<std::uint8_t> d;
    if (!read_all(file, d)) { message = why(Check::Missing); return false; }
    if (const Check c = check_bytes(d); c != Check::Ok) { message = why(c); log_line(category::kHle, "SAVE import refused (" + file.string() + "): " + message); return false; }
    const fs::path cur = data_file();
    std::error_code ec;
    if (fs::exists(cur, ec)) {
        std::vector<std::uint8_t> old;
        if (read_all(cur, old) && old == d) { message = "That save is the one you already have."; return false; }
        const fs::path bak = root() / "backups" / ("DATA-" + stamp("%Y-%m-%d_%H-%M-%S") + ".BIN");
        if (!old.empty() && !write_all(bak, old)) { message = "Your current save could not be backed up, so nothing was changed."; return false; }
        log_line(category::kHle, "SAVE import: current save backed up to " + bak.string());
    }
    if (!write_all(cur, d)) { message = "The save could not be written."; return false; }
    message = "Save imported. Restarting to load it...";
    log_line(category::kHle, "SAVE import: " + file.string() + " -> " + cur.string());
    return true;
}

void export_async() {
    if (g_busy.exchange(true)) return;
    std::thread([] {
        const std::string name = "RidgeRacer-save-" + stamp("%Y-%m-%d") + ".bin";
        fs::path file;
#if defined(_WIN32)
        if (!ask_file(true, L"Export your Ridge Racer save", fs::path(name).wstring(), file)) { finish(false, false, {}); return; }
        if (file.empty()) file = desktop_dir() / name;
#else
        file = executable_dir() / "save-exports" / name;
#endif
        std::string msg;
        const bool ok = export_to(file, msg);
        finish(ok, false, msg);
    }).detach();
}

void import_async() {
    if (g_busy.exchange(true)) return;
    std::thread([] {
        fs::path file;
#if defined(_WIN32)
        if (!ask_file(false, L"Import a Ridge Racer save (your current save is backed up first)", L"", file)) { finish(false, false, {}); return; }
        if (file.empty()) { finish(false, false, "The file window could not be opened."); return; }
#else
        file = executable_dir() / "save-import" / "DATA.BIN";
        std::error_code ec;
        if (!fs::exists(file, ec)) { finish(false, false, "Put the save to import at save-import/DATA.BIN next to the game."); return; }
#endif
        std::string msg;
        const bool ok = import_from(file, msg);
        finish(ok, ok, msg);
    }).detach();
}

bool take_result(Result &out) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_has_result) return false;
    out = g_result;
    g_has_result = false;
    return true;
}

} // namespace rr::savedata
