// Setup.exe stub: the release zip (kit + compiler) is appended to this exe.
// It unpacks the payload to a temp folder, runs the setup window (installer.ps1) and removes the temp folder afterwards.
// Windows only. Contains no game data.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <string>
#include <cstring>
#include <vector>

static std::wstring g_logpath;
static void logf(const std::wstring& m) {
  if (g_logpath.empty()) return;
  HANDLE h = CreateFileW(g_logpath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  std::wstring t = std::to_wstring(GetTickCount64() / 1000) + L"s " + m + L"\r\n";
  int n = WideCharToMultiByte(CP_UTF8, 0, t.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string u(n, 0); WideCharToMultiByte(CP_UTF8, 0, t.c_str(), -1, &u[0], n, nullptr, nullptr);
  DWORD w; WriteFile(h, u.c_str(), (DWORD)strlen(u.c_str()), &w, nullptr); CloseHandle(h);
}
static HWND g_wnd = nullptr, g_lbl = nullptr;
static LRESULT CALLBACK wproc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_CLOSE) return 0;   // not closable while unpacking
  return DefWindowProcW(h, m, w, l);
}
static void show_wait(const wchar_t* text) {
  if (!g_wnd) {
    WNDCLASSW wc{}; wc.lpfnWndProc = wproc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"RRSetupWait";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    int W = 420, H = 110, x = (GetSystemMetrics(SM_CXSCREEN) - W) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - H) / 2;
    g_wnd = CreateWindowW(L"RRSetupWait", L"Ridge Racer setup", WS_CAPTION | WS_VISIBLE, x, y, W, H, nullptr, nullptr, wc.hInstance, nullptr);
    g_lbl = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_CENTER, 10, 30, 380, 24, g_wnd, nullptr, wc.hInstance, nullptr);
    HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT); SendMessageW(g_lbl, WM_SETFONT, (WPARAM)f, TRUE);
  } else SetWindowTextW(g_lbl, text);
  MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
}
static void hide_wait() { if (g_wnd) { DestroyWindow(g_wnd); g_wnd = nullptr; } }

static void fail(const std::wstring& m) {
  logf(L"FAIL: " + m); hide_wait();
  MessageBoxW(nullptr, m.c_str(), L"Ridge Racer setup", MB_OK | MB_ICONERROR);
  ExitProcess(1);
}

static std::wstring self_path() {
  wchar_t b[MAX_PATH * 2];
  DWORD n = GetModuleFileNameW(nullptr, b, MAX_PATH * 2);
  return std::wstring(b, n);
}

// Size of the exe image itself (end of the last PE section); everything after that is the zip payload.
static unsigned long long image_end(const unsigned char* d, unsigned long long size) {
  if (size < 0x40 || d[0] != 'M' || d[1] != 'Z') return 0;
  unsigned pe = *(const unsigned*)(d + 0x3C);
  if (pe + 24 > size || memcmp(d + pe, "PE\0\0", 4) != 0) return 0;
  unsigned short nsec = *(const unsigned short*)(d + pe + 6);
  unsigned short optsz = *(const unsigned short*)(d + pe + 20);
  unsigned long long sec = pe + 24 + optsz, end = 0;
  for (unsigned i = 0; i < nsec; ++i) {
    const unsigned char* s = d + sec + i * 40ull;
    if (sec + (i + 1) * 40ull > size) return 0;
    unsigned long long e = (unsigned long long)*(const unsigned*)(s + 20) + *(const unsigned*)(s + 16);
    if (e > end) end = e;
  }
  return end;
}

// hide_ui=false for the setup window: SW_HIDE would be inherited by the first window the child creates and hide it.
static bool run_wait(std::wstring cmd, const std::wstring& dir, DWORD* code, bool hide_ui = true) {
  STARTUPINFOW si{}; si.cb = sizeof si;
  if (hide_ui) { si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE; }
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> c(cmd.begin(), cmd.end()); c.push_back(0);
  if (!CreateProcessW(nullptr, c.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.empty() ? nullptr : dir.c_str(), &si, &pi)) return false;
  for (;;) {   // keep the little window alive while waiting
    DWORD r = MsgWaitForMultipleObjects(1, &pi.hProcess, FALSE, INFINITE, QS_ALLINPUT);
    if (r == WAIT_OBJECT_0) break;
    MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
  }
  GetExitCodeProcess(pi.hProcess, code);
  CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
  return true;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
  std::wstring me = self_path();
  g_logpath = me.substr(0, me.find_last_of(L'\\') + 1) + L"Setup-log.txt";
  DeleteFileW(g_logpath.c_str());
  logf(L"start");
  show_wait(L"Preparing setup, please wait...");
  HANDLE f = CreateFileW(me.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) fail(L"Could not open the setup file.");
  LARGE_INTEGER sz{}; GetFileSizeEx(f, &sz);
  HANDLE map = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
  const unsigned char* d = map ? (const unsigned char*)MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0) : nullptr;
  if (!d) fail(L"Could not read the setup file.");
  unsigned long long total = (unsigned long long)sz.QuadPart, start = image_end(d, total);
  if (!start || start + 4 > total || memcmp(d + start, "PK", 2) != 0) fail(L"This setup file is damaged (no payload). Please download it again.");

  wchar_t tmp[MAX_PATH];
  GetTempPathW(MAX_PATH, tmp);
  std::wstring root = std::wstring(tmp) + L"RidgeRacerSetup_" + std::to_wstring(GetCurrentProcessId());
  CreateDirectoryW(root.c_str(), nullptr);
  std::wstring zip = root + L"\\payload.zip", kit = root + L"\\kit";
  CreateDirectoryW(kit.c_str(), nullptr);

  HANDLE o = CreateFileW(zip.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (o == INVALID_HANDLE_VALUE) fail(L"Could not write to the temp folder.");
  unsigned long long pos = start;
  while (pos < total) {
    DWORD n = (DWORD)((total - pos) > (8u << 20) ? (8u << 20) : (total - pos)), w = 0;
    if (!WriteFile(o, d + pos, n, &w, nullptr) || w != n) { CloseHandle(o); fail(L"Not enough free space in the temp folder."); }
    pos += n;
  }
  CloseHandle(o);
  logf(L"payload written");
  UnmapViewOfFile(d); CloseHandle(map); CloseHandle(f);

  wchar_t sysd[MAX_PATH]; GetSystemDirectoryW(sysd, MAX_PATH);
  std::wstring sys = sysd;
  DWORD code = 1;
  // tar.exe (bsdtar) ships with Windows 10 and 11 and reads zip files.
  if (!run_wait(L"\"" + sys + L"\\tar.exe\" -xf \"" + zip + L"\" -C \"" + kit + L"\"", root, &code) || code != 0)
    fail(L"Could not unpack the setup files (Windows 10 or newer is required).");
  DeleteFileW(zip.c_str());
  logf(L"unpacked");

  hide_wait();
  std::wstring ps = sys + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
  // the setup window is a separate hidden-console PowerShell; wait for it so the temp files stay until it is closed
  if (GetFileAttributesW((kit + L"\\installer.ps1").c_str()) == INVALID_FILE_ATTRIBUTES)
    fail(L"installer.ps1 was not found after unpacking. Files are in:\n" + kit);
  if (!run_wait(L"\"" + ps + L"\" -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" + kit + L"\\installer.ps1\"", kit, &code, false))
    fail(L"Could not start PowerShell.");
  if (code != 0) {
    MessageBoxW(nullptr, (L"The setup window closed with an error (code " + std::to_wstring(code) + L").\nFiles were left in:\n" + kit).c_str(), L"Ridge Racer setup", MB_OK | MB_ICONERROR);
    return 1;
  }

  logf(L"setup window exited code " + std::to_wstring(code));
  show_wait(L"Cleaning up...");
  run_wait(L"\"" + sys + L"\\cmd.exe\" /d /c rmdir /s /q \"" + root + L"\"", tmp, &code);
  hide_wait();
  return 0;
}
