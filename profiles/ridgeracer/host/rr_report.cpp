#include "rr_report.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "psprecomp/runtime.hpp"
#include "rr_crumb.hpp"
#include "rr_log.hpp"
#include "rr_pad.hpp"
#include "rr_png.hpp"
#include "rr_settings.hpp"
#include "rr_sysinfo.hpp"
#include "rr_version.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <shellapi.h>
#include <shlobj.h>
#elif !defined(__EMSCRIPTEN__)
#include <csignal>
#include <execinfo.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace rr::report {
namespace {

std::string (*g_state_fn)() = nullptr;

std::string stamp_now(const char *fmt) {
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

std::string hex(std::uint64_t v, int w = 8) { char b[32]; std::snprintf(b, sizeof b, "%0*llX", w, static_cast<unsigned long long>(v)); return b; }

// Which build: the version name below and the date the game file was made (compilers here refuse __DATE__).
constexpr const char *kVersionName = RR_GAME_VERSION_NAME;
std::string build_line() {
    std::string t = std::string("Version: ") + kVersionName;
    std::error_code ec;
#if defined(_WIN32)
    wchar_t exe[MAX_PATH];
    const fs::path self = GetModuleFileNameW(nullptr, exe, MAX_PATH) ? fs::path(exe) : fs::path();
#else
    const fs::path self = fs::read_symlink("/proc/self/exe", ec);
#endif
    const auto wt = fs::last_write_time(self, ec);
    if (!ec) {
        const auto sys = std::chrono::time_point_cast<std::chrono::seconds>(wt - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
        const std::time_t tt = std::chrono::system_clock::to_time_t(sys);
        std::tm tmv{};
#if defined(_WIN32)
        localtime_s(&tmv, &tt);
#else
        localtime_r(&tt, &tmv);
#endif
        char b[48]; std::strftime(b, sizeof b, "%Y-%m-%d %H:%M", &tmv);
        t += std::string(", built ") + b;
    }
    return t;
}

// What the game was doing: guest code address, GE/GL breadcrumbs, and the HLE layer's own summary.
std::string state_text() {
    std::string t = "Game code address (last dispatch): " + hex(*psprecomp::runtime_dispatch_pc_address()) + "\n";
    t += "Breadcrumbs: GE list " + hex(rr::g_rr_crumb[0]) + ", GE command " + hex(rr::g_rr_crumb[1]) + ", GL method " + std::to_string(rr::g_rr_crumb[2]) +
         ", marker " + std::to_string(rr::g_rr_crumb[3]) + "\n";
    if (g_state_fn != nullptr) t += g_state_fn();
    return t;
}

// ---- zip (deflate where it helps, CRC-32) ----
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
struct ZipEntry { std::string name; std::vector<std::uint8_t> data; };
void put16(std::vector<std::uint8_t> &o, std::uint32_t v) { o.push_back(v & 255u); o.push_back((v >> 8) & 255u); }
void put32(std::vector<std::uint8_t> &o, std::uint32_t v) { put16(o, v & 0xFFFFu); put16(o, v >> 16); }
bool write_zip(const fs::path &file, const std::vector<ZipEntry> &entries) {
    const std::time_t t = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    const std::uint32_t dtime = static_cast<std::uint32_t>((tmv.tm_hour << 11) | (tmv.tm_min << 5) | (tmv.tm_sec / 2));
    const std::uint32_t ddate = static_cast<std::uint32_t>(((tmv.tm_year - 80) << 9) | ((tmv.tm_mon + 1) << 5) | tmv.tm_mday);
    std::vector<std::uint8_t> out, central;
    for (const auto &e : entries) {
        const std::uint32_t crc = crc32(e.data.data(), e.data.size());
        std::vector<std::uint8_t> packed = e.data.size() > 64u ? deflate_raw(e.data.data(), e.data.size()) : std::vector<std::uint8_t>{};
        const bool deflated = !packed.empty() && packed.size() < e.data.size();
        const std::vector<std::uint8_t> &body = deflated ? packed : e.data;
        const std::uint32_t offset = static_cast<std::uint32_t>(out.size());
        put32(out, 0x04034b50u); put16(out, 20); put16(out, 0x0800u); put16(out, deflated ? 8 : 0); put16(out, dtime); put16(out, ddate);
        put32(out, crc); put32(out, static_cast<std::uint32_t>(body.size())); put32(out, static_cast<std::uint32_t>(e.data.size()));
        put16(out, static_cast<std::uint32_t>(e.name.size())); put16(out, 0);
        out.insert(out.end(), e.name.begin(), e.name.end());
        out.insert(out.end(), body.begin(), body.end());
        put32(central, 0x02014b50u); put16(central, 20); put16(central, 20); put16(central, 0x0800u); put16(central, deflated ? 8 : 0); put16(central, dtime); put16(central, ddate);
        put32(central, crc); put32(central, static_cast<std::uint32_t>(body.size())); put32(central, static_cast<std::uint32_t>(e.data.size()));
        put16(central, static_cast<std::uint32_t>(e.name.size())); put16(central, 0); put16(central, 0); put16(central, 0); put16(central, 0); put32(central, 0); put32(central, offset);
        central.insert(central.end(), e.name.begin(), e.name.end());
    }
    const std::uint32_t cd_offset = static_cast<std::uint32_t>(out.size());
    out.insert(out.end(), central.begin(), central.end());
    put32(out, 0x06054b50u); put16(out, 0); put16(out, 0); put16(out, static_cast<std::uint32_t>(entries.size())); put16(out, static_cast<std::uint32_t>(entries.size()));
    put32(out, static_cast<std::uint32_t>(central.size())); put32(out, cd_offset); put16(out, 0);
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char *>(out.data()), static_cast<std::streamsize>(out.size()));
    return static_cast<bool>(f);
}

// The last `max_bytes` of a file (a long session's log can be large; the end is what matters).
bool read_tail(const fs::path &p, std::size_t max_bytes, std::vector<std::uint8_t> &out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size < 0) return false;
    const std::streamoff start = size > static_cast<std::streamoff>(max_bytes) ? size - static_cast<std::streamoff>(max_bytes) : 0;
    f.seekg(start);
    out.resize(static_cast<std::size_t>(size - start));
    f.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(out.size()));
    if (start > 0) {
        const std::string note = "[... the first " + std::to_string(start) + " bytes are left out ...]\n";
        out.insert(out.begin(), note.begin(), note.end());
    }
    return true;
}

std::string os_version() {
#if defined(_WIN32)
    using RtlGetVersionFn = LONG(WINAPI *)(OSVERSIONINFOW *);
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll")) {
        if (auto fn = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void *>(GetProcAddress(nt, "RtlGetVersion")))) {
            OSVERSIONINFOW v{}; v.dwOSVersionInfoSize = sizeof v;
            if (fn(&v) == 0) return "Windows " + std::to_string(v.dwMajorVersion) + "." + std::to_string(v.dwMinorVersion) + " build " + std::to_string(v.dwBuildNumber) + (v.dwBuildNumber >= 22000 ? " (Windows 11)" : " (Windows 10)");
        }
    }
    return "Windows (version unknown)";
#elif defined(__linux__)
    std::ifstream f("/etc/os-release");
    std::string line;
    while (std::getline(f, line)) if (line.rfind("PRETTY_NAME=", 0) == 0) return line.substr(12);
    return "Linux";
#else
    return "unknown";
#endif
}

fs::path desktop_dir() {
#if defined(_WIN32)
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &p)) && p != nullptr) {
        fs::path d(p);
        CoTaskMemFree(p);
        std::error_code ec;
        if (fs::is_directory(d, ec)) return d;
    }
#endif
    return executable_dir();
}

// ---- crash handling ----
std::atomic<bool> g_crashing{false};

void write_text(const fs::path &p, const std::string &t) { std::ofstream f(p, std::ios::binary | std::ios::trunc); f << t; }

std::string crash_common(const std::string &what) {
    std::string t = "Ridge Racer crash report\n" + build_line() + "\nTime: " + stamp_now("%Y-%m-%d %H:%M:%S") + "\nSystem: " + os_version() + "\n\n";
    t += what + "\n";
    t += state_text() + "\n";
    t += "Last log lines:\n";
    for (const auto &l : recent_log_lines(true)) t += l + "\n";
    return t;
}

void start_report_process(const char *reason) {
#if defined(_WIN32)
    wchar_t exe[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0) return;
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --bug-report ";
    for (const char *c = reason; *c != '\0'; ++c) cmd.push_back(static_cast<wchar_t>(*c));
    STARTUPINFOW si{}; si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
#else
    (void)reason;
#endif
}

#if defined(_WIN32)
std::string module_of(DWORD64 addr) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(addr), &m) || m == nullptr) return hex(addr, 16);
    wchar_t name[MAX_PATH];
    const DWORD n = GetModuleFileNameW(m, name, MAX_PATH);
    std::string s;
    for (DWORD i = 0; i < n; ++i) s.push_back(name[i] < 128 ? static_cast<char>(name[i]) : '?');
    const std::size_t slash = s.find_last_of("\\/");
    if (slash != std::string::npos) s = s.substr(slash + 1);
    return s + "+0x" + hex(addr - reinterpret_cast<DWORD64>(m), 1);
}

const char *exception_name(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation (bad memory read or write)";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error (file or disk problem)";
    case 0xC0000409u: return "stack buffer overrun / fast fail";
    case 0xE06D7363u: return "C++ exception not caught";
    default: return "unknown";
    }
}

LONG WINAPI on_unhandled(EXCEPTION_POINTERS *ep) {
    if (g_crashing.exchange(true)) return EXCEPTION_EXECUTE_HANDLER;
    std::error_code ec;
    const fs::path dir = reports_dir();
    fs::create_directories(dir, ec);
    const std::string base = "crash-" + stamp_now("%Y-%m-%d_%H-%M-%S");
    std::string what;
    {
        const EXCEPTION_RECORD *er = ep->ExceptionRecord;
        what = "Exception " + hex(er->ExceptionCode) + " (" + exception_name(er->ExceptionCode) + ") at " + module_of(reinterpret_cast<DWORD64>(er->ExceptionAddress)) + "\n";
        if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
            what += std::string("  tried to ") + (er->ExceptionInformation[0] == 0 ? "read" : er->ExceptionInformation[0] == 1 ? "write" : "execute") + " address " + hex(er->ExceptionInformation[1], 16) + "\n";
        what += "Thread " + std::to_string(GetCurrentThreadId()) + "\nCall stack:\n";
#if defined(_M_X64) || defined(__x86_64__)
        CONTEXT ctx = *ep->ContextRecord;
        for (int i = 0; i < 48 && ctx.Rip != 0; ++i) {
            what += "  " + module_of(ctx.Rip) + "\n";
            DWORD64 image = 0;
            PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(ctx.Rip, &image, nullptr);
            if (fe == nullptr) {   // leaf function: the return address is on top of the stack
                if (ctx.Rsp == 0) break;
                ctx.Rip = *reinterpret_cast<DWORD64 *>(ctx.Rsp);
                ctx.Rsp += 8;
                continue;
            }
            void *handler_data = nullptr; DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, fe, &ctx, &handler_data, &frame, nullptr);
        }
#endif
    }
    write_text(dir / (base + ".txt"), crash_common(what));
    // Minidump (dbghelp is part of Windows; loaded here so the game does not depend on it otherwise).
    if (HMODULE dh = LoadLibraryW(L"dbghelp.dll")) {
        using WriteDumpFn = BOOL(WINAPI *)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
        if (auto write_dump = reinterpret_cast<WriteDumpFn>(reinterpret_cast<void *>(GetProcAddress(dh, "MiniDumpWriteDump")))) {
            const std::wstring dmp = (dir / (base + ".dmp")).wstring();
            HANDLE f = CreateFileW(dmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
                write_dump(GetCurrentProcess(), GetCurrentProcessId(), f, static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo), &mei, nullptr, nullptr);
                CloseHandle(f);
            }
        }
    }
    log_line(category::kError, "CRASH: " + what.substr(0, what.find('\n')) + " (report: " + (dir / (base + ".txt")).string() + ")");
    close_log_file();
    start_report_process("crash");
    return EXCEPTION_EXECUTE_HANDLER;   // the process ends
}
#elif !defined(__EMSCRIPTEN__)
void on_signal(int sig) {
    if (g_crashing.exchange(true)) _exit(128 + sig);
    std::error_code ec;
    const fs::path dir = reports_dir();
    fs::create_directories(dir, ec);
    std::string what = "Signal " + std::to_string(sig) + (sig == SIGSEGV ? " (segmentation fault)" : sig == SIGABRT ? " (abort)" : sig == SIGFPE ? " (arithmetic)" : sig == SIGBUS ? " (bus error)" : sig == SIGILL ? " (illegal instruction)" : "") + "\nCall stack:\n";
    void *frames[64];
    const int n = backtrace(frames, 64);
    if (char **names = backtrace_symbols(frames, n)) { for (int i = 0; i < n; ++i) what += std::string("  ") + names[i] + "\n"; }
    write_text(dir / ("crash-" + stamp_now("%Y-%m-%d_%H-%M-%S") + ".txt"), crash_common(what));
    std::fprintf(stderr, "Ridge Racer crashed; a report was written to %s. Make a bug report with: RidgeRacerNative --bug-report crash\n", dir.string().c_str());
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}
#endif

void on_terminate() {
#if defined(_WIN32)
    // An exception nobody caught: report it like a crash.
    std::string what = "std::terminate (an error nobody handled)";
    try { if (auto e = std::current_exception()) std::rethrow_exception(e); } catch (const std::exception &e) { what += ": " + std::string(e.what()); } catch (...) {}
    if (!g_crashing.exchange(true)) {
        std::error_code ec;
        fs::create_directories(reports_dir(), ec);
        write_text(reports_dir() / ("crash-" + stamp_now("%Y-%m-%d_%H-%M-%S") + ".txt"), crash_common(what));
        close_log_file();
        start_report_process("crash");
    }
#endif
    std::abort();
}

} // namespace

fs::path reports_dir() { return executable_dir() / "reports"; }

void write_freeze_report(unsigned seconds) {
    std::error_code ec;
    fs::create_directories(reports_dir(), ec);
    std::string t = crash_common("The game showed no new picture for " + std::to_string(seconds) + " s (frozen or stuck loading).");
    t.replace(0, std::strlen("Ridge Racer crash report"), "Ridge Racer freeze report");
    write_text(reports_dir() / ("freeze-" + stamp_now("%Y-%m-%d_%H-%M-%S") + ".txt"), t);
}
void set_state_text_provider(std::string (*fn)()) { g_state_fn = fn; }

void install_crash_handler() {
    if (std::getenv("RR_NO_CRASH_HANDLER") != nullptr) return;
#if defined(_WIN32)
    SetUnhandledExceptionFilter(on_unhandled);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);   // our report instead of the "has stopped working" box
#elif !defined(__EMSCRIPTEN__)
    for (int s : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) std::signal(s, on_signal);
#endif
    std::set_terminate(on_terminate);
}

fs::path make_bug_report(const std::string &reason, const fs::path &screenshot) {
    std::vector<ZipEntry> entries;
    const fs::path dir = executable_dir();
    std::error_code ec;
    const auto add_file = [&](const fs::path &p, const std::string &name, std::size_t max_bytes) {
        ZipEntry e; e.name = name;
        if (!fs::is_regular_file(p, ec) || !read_tail(p, max_bytes, e.data)) return;
        entries.push_back(std::move(e));
    };
    // logs: the current one (wherever --log put it) and the previous session's
    fs::path log = log_file_path();
    if (log.empty()) log = dir / "ridgeracer.log";
    add_file(log, "ridgeracer.log", 12u << 20);
    add_file(log.parent_path() / (log.stem().string() + "-previous" + log.extension().string()), "ridgeracer-previous.log", 6u << 20);
    add_file(dir / "online-log.txt", "online-log.txt", 6u << 20);
    add_file(dir / "performance-report.txt", "performance-report.txt", 2u << 20);
    // settings without the room password
    {
        std::ifstream f(settings_file_path());
        if (f) {
            std::string line, out;
            while (std::getline(f, line)) {
                if (line.rfind("room_password=", 0) == 0) line = "room_password=(removed)";
                out += line + "\n";
            }
            entries.push_back({"ridgeracer.ini", std::vector<std::uint8_t>(out.begin(), out.end())});
        }
    }
    // the newest crash reports (the last 2 crashes: .txt and .dmp)
    {
        std::vector<std::string> stems, freezes;
        for (const auto &de : fs::directory_iterator(reports_dir(), ec)) {
            const std::string n = de.path().filename().string();
            if (n.rfind("crash-", 0) == 0 && de.path().extension() == ".txt") stems.push_back(de.path().stem().string());
            if (n.rfind("freeze-", 0) == 0 && de.path().extension() == ".txt") freezes.push_back(de.path().stem().string());
        }
        std::sort(stems.rbegin(), stems.rend());   // names hold the date and time: newest first
        std::sort(freezes.rbegin(), freezes.rend());
        for (std::size_t i = 0; i < stems.size() && i < 2u; ++i) {
            add_file(reports_dir() / (stems[i] + ".txt"), "crash/" + stems[i] + ".txt", 4u << 20);
            add_file(reports_dir() / (stems[i] + ".dmp"), "crash/" + stems[i] + ".dmp", 64u << 20);
        }
        for (std::size_t i = 0; i < freezes.size() && i < 2u; ++i) add_file(reports_dir() / (freezes[i] + ".txt"), "crash/" + freezes[i] + ".txt", 4u << 20);
    }
    if (!screenshot.empty()) add_file(screenshot, "screenshot.png", 16u << 20);
    // system summary
    {
        std::string t = "Ridge Racer bug report\n" + build_line() + "\nMade: " + stamp_now("%Y-%m-%d %H:%M:%S") + " (" + reason + ")\n";
        {   // fingerprint of the game file (testers' installs are built the same way: lets crash addresses be matched to a build)
#if defined(_WIN32)
            wchar_t exe[MAX_PATH];
            const fs::path self = GetModuleFileNameW(nullptr, exe, MAX_PATH) ? fs::path(exe) : fs::path();
#else
            const fs::path self = fs::read_symlink("/proc/self/exe", ec);
#endif
            std::ifstream f(self, std::ios::binary);
            std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (!bytes.empty()) t += "Game file: " + std::to_string(bytes.size()) + " bytes, CRC-32 " + hex(crc32(bytes.data(), bytes.size())) + "\n";
        }
        t += "System: " + os_version() + "\n" + system_description() + "\n";
        for (const auto &g : gpu_adapter_names()) t += "Graphics: " + g + "\n";
        t += "Monitor: " + std::to_string(monitor_refresh_hz()) + " Hz\n";
        t += "Controllers: " + (g_state_fn != nullptr ? pad::sdl_describe() : std::string("(not checked: game not running)")) + "; button pictures: " + pad::glyphs_name(pad::glyphs()) + "\n\n";
        if (g_state_fn != nullptr) t += state_text();   // (made while the game runs)
        t += "\nPlease also tell the developer what you were doing and what went wrong.\n";
        entries.insert(entries.begin(), {"system.txt", std::vector<std::uint8_t>(t.begin(), t.end())});
    }
    const fs::path zip = desktop_dir() / ("RidgeRacer-bug-report-" + stamp_now("%Y-%m-%d_%H-%M-%S") + ".zip");
    if (!write_zip(zip, entries)) return {};
    log_line(category::kHle, "bug report written: " + zip.string());
    return zip;
}

void announce(const fs::path &zip, bool crashed) {
#if defined(_WIN32)
    std::wstring msg;
    if (zip.empty()) msg = L"The bug report could not be written.  Please send the files in the game's folder instead (ridgeracer.log and the reports folder).";
    else {
        msg = crashed ? L"Sorry - Ridge Racer crashed.\n\n" : L"";
        msg += L"A bug report was saved on your Desktop:\n\n" + zip.filename().wstring() +
               L"\n\nPlease send this file to the developer, with a few words about what you were doing.  It holds the game's logs, your settings "
               L"(without your room password), a summary of your PC and, if the game crashed, a crash report.\n\nOpen the folder now?";
    }
    const int r = MessageBoxW(nullptr, msg.c_str(), crashed ? L"Ridge Racer crashed" : L"Ridge Racer bug report",
                              (zip.empty() ? MB_OK | MB_ICONWARNING : MB_YESNO | (crashed ? MB_ICONERROR : MB_ICONINFORMATION)) | MB_TOPMOST | MB_SETFOREGROUND);
    if (r == IDYES) {
        const std::wstring args = L"/select,\"" + zip.wstring() + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }
#else
    if (zip.empty()) std::fprintf(stderr, "The bug report could not be written.\n");
    else std::fprintf(stderr, "%sA bug report was saved: %s\nPlease send it to the developer.\n", crashed ? "Ridge Racer crashed. " : "", zip.string().c_str());
#endif
}

void make_bug_report_async(const std::string &reason, const fs::path &screenshot) {
    std::thread([reason, screenshot] { announce(make_bug_report(reason, screenshot), false); }).detach();
}

int run_report_mode(const std::string &reason) {
    const bool crashed = reason == "crash" || reason == "error";
    announce(make_bug_report(reason), crashed);
    return 0;
}

} // namespace rr::report
