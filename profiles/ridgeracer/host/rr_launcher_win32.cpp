// The settings window shown before the game starts (Windows).  Plain Win32 controls, no resource file.
#include "rr_settings.hpp"
#include "rr_report.hpp"
#include "rr_update.hpp"
#include "rr_version.hpp"
#include "rr_links.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

namespace rr {
namespace {
namespace fs = std::filesystem;

enum : int {
    kIdPath = 100, kIdBrowse, kIdBrowseIso, kIdStatus,
    kIdRenderer, kIdRenderScale, kIdWindowScale, kIdFps, kIdFullscreen, kIdLowLatency, kIdSmoothMenus,
    kIdAa, kIdAniso, kIdSharpen, kIdHud, kIdMipmaps, kIdCutouts,
    kIdVolume, kIdVolumeText, kIdMenuVolume, kIdMenuVolumeText,
    kIdShowAtStart, kIdDefaults, kIdPrompts, kIdTriggers, kIdBugReport, kIdUpdate, kIdDiscord,
};
const UINT kMsgChecked = WM_APP + 1, kMsgProgress = WM_APP + 2, kMsgDownloaded = WM_APP + 3;

const int kFpsValues[] = {0, 60, 90, 120, 144, 165, 180, 240};
const int kAnisoValues[] = {1, 2, 4, 8, 16};

std::wstring widen(const std::string &s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring &w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_ACP, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_ACP, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

struct Launcher {
    PlayerSettings s;
    HWND wnd = nullptr;
    HFONT font = nullptr, bold = nullptr;
    int dpi = 96;
    bool play = false;
    bool path_ok = false;
    std::vector<int> fps_items;   // value of each frame-rate entry
    int monitor_hz = 60;
    int max_window_scale = 8;
    // built-in updater
    rr::update::Info upd;
    std::atomic<int> pct{0};
    std::atomic<bool> working{false};
    std::string upd_err;
    std::wstring upd_file, upd_pkg, install_dir;
    std::atomic<int> mode{0};   // 1 = program replaced in place, 2 = full Setup downloaded
    std::thread helper;
    std::shared_ptr<rr::update::Info> checked;

    ~Launcher() { if (helper.joinable()) helper.join(); }

    void start_check() {
        if (std::getenv("RR_NO_UPDATE_CHECK") != nullptr) return;
        HWND w = wnd;
        checked = std::make_shared<rr::update::Info>();
        auto result = checked;
        std::thread([w, result] {   // (detached: it only touches what it owns, so closing the window never waits for the network)
            if (rr::update::check(*result) && result->available) PostMessageW(w, kMsgChecked, 1, 0);
        }).detach();
    }

    void start_download() {
        std::wstring who = widen(upd.tag);
        if (MessageBoxW(wnd, (L"A newer version is available: " + who + L"\n\nThe update is downloaded from the project's GitHub page and checked against its published SHA-256 hash. "
                              L"The game then restarts with the new version (a few seconds). If this version also changes the game library, the full Setup opens "
                              L"instead with your game folder filled in. Your settings and saves are kept.\n\nDownload and update now?").c_str(),
                        L"Ridge Racer - Update", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        std::wstring dir = std::wstring(tmp) + L"RidgeRacerUpdate";
        CreateDirectoryW(dir.c_str(), nullptr);
        upd_file = dir + L"\\Setup.exe";
        upd_pkg = dir + L"\\RidgeRacer-Update.zip";
        wchar_t exe[MAX_PATH * 2];
        const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
        install_dir = std::wstring(exe, n);
        install_dir = install_dir.substr(0, install_dir.find_last_of(L'\\'));
        if (helper.joinable()) helper.join();
        working = true;
        mode = 0;
        EnableWindow(item(kIdUpdate), FALSE);
        HWND w = wnd;
        helper = std::thread([this, w] {
            std::atomic<int> &p = pct;
            std::thread ticker([&p, w, this] { while (working) { PostMessageW(w, kMsgProgress, static_cast<WPARAM>(p.load()), 0); Sleep(200); } });
            upd_err.clear();
            // 1) the small package: only the program is replaced, the game library built on this PC stays
            if (!upd.pkg_url.empty()) {
                upd_err = rr::update::download(upd.pkg_url, upd.pkg_sha256, upd_pkg, pct);
                if (upd_err.empty()) {
                    bool needs_setup = false;
                    upd_err = rr::update::apply_package(upd_pkg, install_dir, needs_setup);
                    DeleteFileW(upd_pkg.c_str());
                    if (upd_err.empty() && !needs_setup) mode = 1;
                    else if (upd_err.empty()) upd_err = "needs setup";
                }
            } else upd_err = "needs setup";
            // 2) otherwise (this update changes the game library too) the full Setup
            if (mode == 0 && upd_err == "needs setup") {
                upd_err = rr::update::download(upd.url, upd.sha256, upd_file, pct);
                if (upd_err.empty()) mode = 2;
            }
            working = false;
            ticker.join();
            PostMessageW(w, kMsgDownloaded, 0, 0);
        });
    }

    void finish_download() {
        if (helper.joinable()) helper.join();
        if (!upd_err.empty()) {
            MessageBoxW(wnd, widen(upd_err).c_str(), L"Ridge Racer - Update", MB_OK | MB_ICONERROR);
            SetWindowTextW(item(kIdUpdate), (L"Update to " + widen(upd.tag)).c_str());
            EnableWindow(item(kIdUpdate), TRUE);
            return;
        }
        read_settings();
        if (mode == 1) {   // the program was replaced in place: start the new one and close this one
            const std::string e = rr::update::restart(install_dir);
            if (!e.empty()) { MessageBoxW(wnd, widen(e).c_str(), L"Ridge Racer - Update", MB_OK | MB_ICONINFORMATION); }
            play = false;
            DestroyWindow(wnd);
            return;
        }
        const std::string e = rr::update::run_setup(upd_file, install_dir, widen(s.game_path));
        if (!e.empty()) {
            MessageBoxW(wnd, widen(e).c_str(), L"Ridge Racer - Update", MB_OK | MB_ICONERROR);
            EnableWindow(item(kIdUpdate), TRUE);
            return;
        }
        play = false;
        DestroyWindow(wnd);   // (the game exe is replaced by Setup)
    }

    int px(int dip) const { return MulDiv(dip, dpi, 96); }
    HWND item(int id) const { return GetDlgItem(wnd, id); }

    HWND add(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0) {
        HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, px(x), px(y), px(w), px(h), wnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
        return c;
    }
    void label(const wchar_t *text, int x, int y, int w) { add(L"STATIC", text, SS_LEFT, x, y + 3, w, 18, -1); }
    HWND combo(int id, int x, int y, int w, const std::vector<std::wstring> &items) {
        HWND c = add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, x, y, w, 300, id);
        for (const auto &t : items) SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t.c_str()));
        return c;
    }
    void check(int id, const wchar_t *text, int x, int y, int w) { add(L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, x, y, w, 22, id); }
    void group(const wchar_t *text, int x, int y, int w, int h) {
        HWND g = add(L"BUTTON", text, BS_GROUPBOX, x, y, w, h, -1);
        SendMessageW(g, WM_SETFONT, reinterpret_cast<WPARAM>(bold), FALSE);
    }
    HWND slider(int id, int x, int y, int w, int max) {
        HWND t = add(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, x, y, w, 26, id);
        SendMessageW(t, TBM_SETRANGE, TRUE, MAKELPARAM(0, max));
        SendMessageW(t, TBM_SETPAGESIZE, 0, 10);
        return t;
    }

    void build() {
        const int W = 620;
        group(L"Game files", 12, 8, W - 24, 78);
        add(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 24, 30, W - 24 - 24 - 104 - 100, 24, kIdPath, WS_EX_CLIENTEDGE);
        add(L"BUTTON", L"ISO file...", BS_PUSHBUTTON | WS_TABSTOP, W - 24 - 96 - 96, 29, 84, 26, kIdBrowseIso);
        add(L"BUTTON", L"Folder...", BS_PUSHBUTTON | WS_TABSTOP, W - 24 - 96, 29, 84, 26, kIdBrowse);
        add(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 24, 60, W - 48, 18, kIdStatus);

        // Display
        const int gx = 12, gw = 292, gy = 96, lx = gx + 12, cx = gx + 128, cw = gw - 140;
        group(L"Display", gx, gy, gw, 222);
        label(L"Renderer", lx, gy + 24, 112);
        combo(kIdRenderer, cx, gy + 24, cw, {L"OpenGL (enhanced)", L"Software (original)"});
        label(L"Render resolution", lx, gy + 56, 112);
        {
            std::vector<std::wstring> v;
            for (int k = 1; k <= 8; ++k)
                v.push_back(std::to_wstring(k) + L"\u00D7  (" + std::to_wstring(480 * k) + L"\u00D7" + std::to_wstring(272 * k) + (k == 1 ? L", PSP)" : L")"));
            combo(kIdRenderScale, cx, gy + 56, cw, v);
        }
        label(L"Window size", lx, gy + 88, 112);
        {
            std::vector<std::wstring> v;
            for (int k = 1; k <= max_window_scale; ++k)
                v.push_back(std::to_wstring(k) + L"\u00D7  (" + std::to_wstring(480 * k) + L"\u00D7" + std::to_wstring(272 * k) + L")");
            combo(kIdWindowScale, cx, gy + 88, cw, v);
        }
        label(L"Frame rate", lx, gy + 120, 112);
        {
            std::vector<std::wstring> v;
            for (int f : kFpsValues) {
                fps_items.push_back(f);
                v.push_back(f == 0 ? L"Match monitor (" + std::to_wstring(monitor_hz) + L" Hz)" : f == 60 ? L"60 (original)" : std::to_wstring(f));
            }
            if (std::find(fps_items.begin(), fps_items.end(), s.fps) == fps_items.end()) { fps_items.push_back(s.fps); v.push_back(std::to_wstring(s.fps)); }
            combo(kIdFps, cx, gy + 120, cw, v);
        }
        check(kIdFullscreen, L"Fullscreen  (F11 or Alt+Enter in game)", lx, gy + 148, gw - 24);
        check(kIdSmoothMenus, L"Menus at the high frame rate too", lx, gy + 172, gw - 24);
        check(kIdLowLatency, L"Low input latency  (may stutter)", lx, gy + 196, gw - 24);

        // Image quality
        const int qx = 316, qlx = qx + 12, qcx = qx + 128, qcw = gw - 140;
        group(L"Image quality", qx, gy, gw, 222);
        label(L"Anti-aliasing", qlx, gy + 24, 112);
        combo(kIdAa, qcx, gy + 24, qcw, {L"Off", L"FXAA (light)", L"MSAA 2\u00D7", L"MSAA 4\u00D7", L"MSAA 8\u00D7"});
        label(L"Texture filtering", qlx, gy + 56, 112);
        combo(kIdAniso, qcx, gy + 56, qcw, {L"Bilinear", L"Anisotropic 2\u00D7", L"Anisotropic 4\u00D7", L"Anisotropic 8\u00D7", L"Anisotropic 16\u00D7"});
        label(L"Sharpening", qlx, gy + 88, 112);
        combo(kIdSharpen, qcx, gy + 88, qcw, {L"Off", L"Mild", L"Medium", L"Strong"});
        label(L"HUD and text", qlx, gy + 120, 112);
        combo(kIdHud, qcx, gy + 120, qcw, {L"Original (soft)", L"Sharper", L"Crispest"});
        check(kIdMipmaps, L"Mipmaps  (less shimmer in the distance)", qlx, gy + 148, gw - 24);
        check(kIdCutouts, L"Smooth edges on trees and fences", qlx, gy + 172, gw - 24);

        // Audio
        const int ay = 328;
        group(L"Audio", 12, ay, W - 24, 88);
        label(L"Volume", 24, ay + 24, 112);
        slider(kIdVolume, 136, ay + 22, W - 136 - 90, 150);
        add(L"STATIC", L"", SS_LEFT, W - 84, ay + 27, 60, 18, kIdVolumeText);
        label(L"Menu sounds", 24, ay + 54, 112);
        slider(kIdMenuVolume, 136, ay + 52, W - 136 - 90, 200);
        add(L"STATIC", L"", SS_LEFT, W - 84, ay + 57, 60, 18, kIdMenuVolumeText);

        // Controller
        const int cy = 424;
        group(L"Controller", 12, cy, W - 24, 58);
        label(L"Button pictures", 24, cy + 24, 112);
        combo(kIdPrompts, 136, cy + 24, 200, {L"Automatic (what you use)", L"Xbox", L"PlayStation", L"Nintendo Switch", L"Keyboard"});
        check(kIdTriggers, L"Triggers accelerate and brake", 352, cy + 24, W - 352 - 24);

        check(kIdShowAtStart, L"Show this window when the game starts  (hold Shift while starting to open it anyway)", 14, 492, W - 28);
        // Community
        add(L"BUTTON", L"Join our Discord", BS_PUSHBUTTON | WS_TABSTOP, 12, 520, 150, 32, kIdDiscord);
        add(L"STATIC", L"Get news and updates, and find opponents to race against.", SS_LEFT, 174, 520 + 8, W - 174 - 12, 20, 0);
        const int by = 564;
        add(L"BUTTON", L"Defaults", BS_PUSHBUTTON | WS_TABSTOP, 12, by, 90, 28, kIdDefaults);
        add(L"BUTTON", L"Bug report...", BS_PUSHBUTTON | WS_TABSTOP, 110, by, 110, 28, kIdBugReport);
        add(L"BUTTON", L"Update", BS_PUSHBUTTON | WS_TABSTOP, 228, by, 164, 28, kIdUpdate);
        ShowWindow(item(kIdUpdate), SW_HIDE);
        add(L"BUTTON", L"Quit", BS_PUSHBUTTON | WS_TABSTOP, W - 12 - 90 - 8 - 110, by, 90, 28, IDCANCEL);
        add(L"BUTTON", L"Play", BS_DEFPUSHBUTTON | WS_TABSTOP, W - 12 - 110, by, 110, 28, IDOK);
    }

    void set_combo(int id, int index) { SendMessageW(item(id), CB_SETCURSEL, static_cast<WPARAM>(std::max(index, 0)), 0); }
    int get_combo(int id) const { return static_cast<int>(SendMessageW(item(id), CB_GETCURSEL, 0, 0)); }
    void set_check(int id, bool on) { SendMessageW(item(id), BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }
    bool get_check(int id) const { return SendMessageW(item(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
    int get_slider(int id) const { return static_cast<int>(SendMessageW(item(id), TBM_GETPOS, 0, 0)); }

    void show_settings() {
        SetWindowTextW(item(kIdPath), widen(s.game_path).c_str());
        set_combo(kIdRenderer, s.gpu ? 0 : 1);
        set_combo(kIdRenderScale, std::clamp(s.render_scale, 1, 8) - 1);
        set_combo(kIdWindowScale, std::clamp(s.window_scale, 1, max_window_scale) - 1);
        set_combo(kIdFps, static_cast<int>(std::find(fps_items.begin(), fps_items.end(), s.fps) - fps_items.begin()));
        set_check(kIdFullscreen, s.fullscreen);
        set_check(kIdLowLatency, s.low_latency);
        set_check(kIdSmoothMenus, s.smooth_menus);
        set_combo(kIdAa, std::clamp(s.aa, 0, 4));
        set_combo(kIdAniso, static_cast<int>(std::find(std::begin(kAnisoValues), std::end(kAnisoValues), s.aniso) - std::begin(kAnisoValues)) % 5);
        set_combo(kIdSharpen, std::clamp(s.sharpen, 0, 3));
        set_combo(kIdHud, std::clamp(s.ui_crisp, 0, 2));
        set_check(kIdMipmaps, s.mipmaps);
        set_check(kIdCutouts, s.smooth_cutouts);
        SendMessageW(item(kIdVolume), TBM_SETPOS, TRUE, s.volume);
        SendMessageW(item(kIdMenuVolume), TBM_SETPOS, TRUE, s.menu_volume);
        set_check(kIdShowAtStart, s.show_launcher);
        set_combo(kIdPrompts, std::clamp(s.button_prompts, 0, 4));
        set_check(kIdTriggers, s.trigger_buttons);
        refresh();
    }

    void read_settings() {
        wchar_t buf[2048] = {};
        GetWindowTextW(item(kIdPath), buf, 2048);
        s.game_path = narrow(buf);
        s.gpu = get_combo(kIdRenderer) == 0;
        s.render_scale = get_combo(kIdRenderScale) + 1;
        s.window_scale = get_combo(kIdWindowScale) + 1;
        const int f = get_combo(kIdFps);
        s.fps = f >= 0 && f < static_cast<int>(fps_items.size()) ? fps_items[static_cast<std::size_t>(f)] : 0;
        s.fullscreen = get_check(kIdFullscreen);
        s.low_latency = get_check(kIdLowLatency);
        s.smooth_menus = get_check(kIdSmoothMenus);
        s.aa = get_combo(kIdAa);
        s.aniso = kAnisoValues[std::clamp(get_combo(kIdAniso), 0, 4)];
        s.sharpen = get_combo(kIdSharpen);
        s.ui_crisp = get_combo(kIdHud);
        s.mipmaps = get_check(kIdMipmaps);
        s.smooth_cutouts = get_check(kIdCutouts);
        s.volume = get_slider(kIdVolume);
        s.menu_volume = get_slider(kIdMenuVolume);
        s.show_launcher = get_check(kIdShowAtStart);
        s.button_prompts = std::clamp(get_combo(kIdPrompts), 0, 4);
        s.trigger_buttons = get_check(kIdTriggers);
    }

    void refresh() {
        // the game file
        wchar_t buf[2048] = {};
        GetWindowTextW(item(kIdPath), buf, 2048);
        const fs::path game = resolve_game_path(fs::path(buf));
        const std::string problem = check_game_file(game);
        path_ok = problem.empty();
        SetWindowTextW(item(kIdStatus), path_ok ? (L"Found: " + fs::path(game).make_preferred().wstring()).c_str() : widen(problem).c_str());
        InvalidateRect(item(kIdStatus), nullptr, TRUE);
        EnableWindow(item(IDOK), path_ok);
        // enhancements only apply to the OpenGL renderer
        const bool gl = get_combo(kIdRenderer) == 0;
        for (int id : {kIdRenderScale, kIdAa, kIdAniso, kIdSharpen, kIdHud, kIdMipmaps, kIdCutouts}) EnableWindow(item(id), gl);
        SetWindowTextW(item(kIdVolumeText), (std::to_wstring(get_slider(kIdVolume)) + L"%").c_str());
        SetWindowTextW(item(kIdMenuVolumeText), (std::to_wstring(get_slider(kIdMenuVolume)) + L"%").c_str());
    }

    void browse() {
        IFileOpenDialog *dlg = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dlg->SetTitle(L"Pick the folder that holds the game files (PSP_GAME)");
        if (SUCCEEDED(dlg->Show(wnd))) {
            IShellItem *res = nullptr;
            if (SUCCEEDED(dlg->GetResult(&res))) {
                PWSTR p = nullptr;
                if (SUCCEEDED(res->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                    const fs::path found = resolve_game_path(fs::path(p));
                    SetWindowTextW(item(kIdPath), found.empty() ? p : found.wstring().c_str());
                    CoTaskMemFree(p);
                }
                res->Release();
            }
        }
        dlg->Release();
        refresh();
    }

    void browse_iso() {
        IFileOpenDialog *dlg = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST);
        const COMDLG_FILTERSPEC types[] = {{L"PSP disc image (*.iso)", L"*.iso"}, {L"All files", L"*.*"}};
        dlg->SetFileTypes(2, types);
        dlg->SetTitle(L"Pick your Ridge Racer disc image (.iso)");
        if (SUCCEEDED(dlg->Show(wnd))) {
            IShellItem *res = nullptr;
            if (SUCCEEDED(dlg->GetResult(&res))) {
                PWSTR p = nullptr;
                if (SUCCEEDED(res->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                    SetWindowTextW(item(kIdPath), p);
                    CoTaskMemFree(p);
                }
                res->Release();
            }
        }
        dlg->Release();
        refresh();
    }

    // A disc image is copied out once (a few seconds to a minute); the window shows the progress.  False = failed.
    bool prepare_disc() {
        wchar_t buf[2048] = {};
        GetWindowTextW(item(kIdPath), buf, 2048);
        const fs::path picked(buf);
        if (!is_iso_path(picked)) return true;
        EnableWindow(item(IDOK), FALSE);
        const std::string err = prepare_game(picked, [this](int pct) {
            SetWindowTextW(item(kIdStatus), (L"Copying game files from the disc image (first start only)... " + std::to_wstring(pct) + L"%").c_str());
            UpdateWindow(item(kIdStatus));
        });
        if (!err.empty()) {
            MessageBoxW(wnd, widen(err).c_str(), L"Ridge Racer", MB_OK | MB_ICONERROR);
            refresh();
            return false;
        }
        return true;
    }

    LRESULT proc(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_COMMAND: {
            const int id = LOWORD(wp), code = HIWORD(wp);
            if (id == IDOK) { if (path_ok && prepare_disc()) { read_settings(); play = true; DestroyWindow(wnd); } return 0; }
            if (id == IDCANCEL) { if (!working) DestroyWindow(wnd); return 0; }
            if (id == kIdUpdate) { start_download(); return 0; }
            if (id == kIdDiscord) { ShellExecuteW(nullptr, L"open", RR_URL_DISCORD_W, nullptr, nullptr, SW_SHOWNORMAL); return 0; }
            if (id == kIdBrowse) { browse(); return 0; }
            if (id == kIdBrowseIso) { browse_iso(); return 0; }
            if (id == kIdBugReport) {   // logs of the last session, settings, crash reports and a PC summary in one zip
                SetCursor(LoadCursor(nullptr, IDC_WAIT));
                rr::report::announce(rr::report::make_bug_report("settings window"), false);
                return 0;
            }
            if (id == kIdDefaults) {
                read_settings();
                PlayerSettings d;
                d.game_path = s.game_path;
                d.show_launcher = s.show_launcher;
                s = d;
                show_settings();
                return 0;
            }
            if ((id == kIdPath && code == EN_CHANGE) || (id == kIdRenderer && code == CBN_SELCHANGE)) refresh();
            return 0;
        }
        case kMsgChecked:
            if (checked) upd = *checked;
            SetWindowTextW(item(kIdUpdate), (L"Update to " + widen(upd.tag)).c_str());
            ShowWindow(item(kIdUpdate), SW_SHOW);
            return 0;
        case kMsgProgress:
            if (working) SetWindowTextW(item(kIdUpdate), (L"Downloading " + std::to_wstring(static_cast<int>(wp)) + L"%").c_str());
            return 0;
        case kMsgDownloaded: finish_download(); return 0;
        case WM_HSCROLL: refresh(); return 0;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetBkMode(dc, TRANSPARENT);
            if (reinterpret_cast<HWND>(lp) == item(kIdStatus)) SetTextColor(dc, path_ok ? RGB(0, 120, 40) : RGB(190, 30, 30));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
        }
        case WM_CLOSE: if (working) return 0; DestroyWindow(wnd); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        default: return DefWindowProcW(wnd, msg, wp, lp);
        }
    }

    static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            auto *self = static_cast<Launcher *>(reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams);
            self->wnd = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        auto *self = reinterpret_cast<Launcher *>(GetWindowLongPtrW(h, GWLP_USERDATA));
        return self ? self->proc(msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
    }
};

// The game window is not DPI aware; the settings window is (sharp text at 125-200 % scaling).  Only for this thread
// while the window is open, so the game window behaves exactly as before.
struct ThreadDpiScope {
    using SetCtx = DPI_AWARENESS_CONTEXT(WINAPI *)(DPI_AWARENESS_CONTEXT);
    SetCtx set = nullptr;
    DPI_AWARENESS_CONTEXT old = nullptr;
    ThreadDpiScope() {
        set = reinterpret_cast<SetCtx>(reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext")));
        if (set) old = set(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    }
    ~ThreadDpiScope() { if (set && old) set(old); }
};

int system_dpi() {
    using GetDpi = UINT(WINAPI *)();
    auto f = reinterpret_cast<GetDpi>(reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem")));
    if (f) return static_cast<int>(f());
    HDC dc = GetDC(nullptr);
    const int d = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    return d;
}

} // namespace

bool launcher_shift_held() { return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0; }

int monitor_refresh_hz() {
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1u) return static_cast<int>(dm.dmDisplayFrequency);
    return 60;
}

void hide_own_console() {
    DWORD ids[2];
    if (GetConsoleWindow() != nullptr && GetConsoleProcessList(ids, 2) == 1u) FreeConsole();   // nobody else uses this console
}

void show_error_box(const std::string &message) {
    ThreadDpiScope dpi_scope;
    MessageBoxW(nullptr, widen(message).c_str(), L"Ridge Racer", MB_OK | MB_ICONERROR);
}

bool run_launcher(PlayerSettings &settings) {
    ThreadDpiScope dpi_scope;
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    Launcher L;
    L.s = settings;
    L.dpi = system_dpi();
    L.monitor_hz = monitor_refresh_hz();
    {
        // Window sizes that fit on the screen.  The game window is not DPI aware, so it is measured in scaled pixels.
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        const int w = MulDiv(work.right - work.left, 96, L.dpi), h = MulDiv(work.bottom - work.top, 96, L.dpi);
        L.max_window_scale = std::clamp(std::min(w / 480, h / 272), 2, 8);
    }
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfHeight = -MulDiv(9, L.dpi, 72);
    L.font = CreateFontIndirectW(&lf);
    lf.lfWeight = FW_SEMIBOLD;
    L.bold = CreateFontIndirectW(&lf);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = &Launcher::wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = L"RidgeRacerSettings";
    wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    if (wc.hIcon == nullptr) wc.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    RegisterClassExW(&wc);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r{0, 0, L.px(620), L.px(604)};
    AdjustWindowRectEx(&r, style, FALSE, 0);
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    const int sx = (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, sy = (GetSystemMetrics(SM_CYSCREEN) - wh) / 2;
    HWND h = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"Ridge Racer - Settings", style, std::max(sx, 0), std::max(sy, 0), ww, wh,
                             nullptr, nullptr, wc.hInstance, &L);
    if (h == nullptr) { if (SUCCEEDED(co)) CoUninitialize(); return true; }   // no window possible: just play
    L.build();
    L.show_settings();
    ShowWindow(h, SW_SHOW);
    L.start_check();
    SetForegroundWindow(h);
    SetFocus(GetDlgItem(h, IDOK));

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (IsWindow(L.wnd) && IsDialogMessageW(L.wnd, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    DeleteObject(L.font);
    DeleteObject(L.bold);
    if (SUCCEEDED(co)) CoUninitialize();
    if (L.play) settings = L.s;
    return L.play;
}

} // namespace rr
