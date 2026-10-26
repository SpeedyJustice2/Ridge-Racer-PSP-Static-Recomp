#include "rr_sysinfo.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#define RR_SYSINFO_X86 1
#endif
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

namespace rr {
namespace {

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\r' || s.back() == '\0')) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && s[i] == ' ') ++i;
    std::string out = s.substr(i);
    // collapse runs of spaces
    std::string r;
    for (char c : out) if (!(c == ' ' && !r.empty() && r.back() == ' ')) r += c;
    return r;
}

std::string cpu_name() {
#if defined(RR_SYSINFO_X86)
    char brand[49] = {};
    unsigned regs[4] = {};
#if defined(_MSC_VER) && !defined(__clang__)
    int r[4];
    __cpuid(r, static_cast<int>(0x80000000u));
    if (static_cast<unsigned>(r[0]) >= 0x80000004u) {
        for (unsigned i = 0; i < 3; ++i) {
            __cpuid(r, static_cast<int>(0x80000002u + i));
            std::memcpy(brand + 16 * i, r, 16);
        }
        return trim(brand);
    }
#else
    if (__get_cpuid(0x80000000u, &regs[0], &regs[1], &regs[2], &regs[3]) && regs[0] >= 0x80000004u) {
        for (unsigned i = 0; i < 3; ++i) {
            __get_cpuid(0x80000002u + i, &regs[0], &regs[1], &regs[2], &regs[3]);
            std::memcpy(brand + 16 * i, regs, 16);
        }
        return trim(brand);
    }
#endif
#endif
#if defined(__linux__)
    std::ifstream in("/proc/cpuinfo");
    std::string line;
    while (std::getline(in, line))
        if (line.rfind("model name", 0) == 0 || line.rfind("Model", 0) == 0 || line.rfind("Hardware", 0) == 0) {
            const auto colon = line.find(':');
            if (colon != std::string::npos) return trim(line.substr(colon + 1));
        }
#endif
#if defined(__APPLE__)
    char buf[128] = {};
    std::size_t len = sizeof buf;
    if (sysctlbyname("machdep.cpu.brand_string", buf, &len, nullptr, 0) == 0) return trim(buf);
#endif
    return "unknown CPU";
}

std::string lower(std::string s) { for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }
bool has(const std::string &s, const char *needle) { return s.find(needle) != std::string::npos; }
bool has_digits_then_m(const std::string &s) {   // "radeon 610m", "radeon(tm) 780m"
    for (std::size_t i = 0; i + 3 < s.size(); ++i)
        if (std::isdigit(static_cast<unsigned char>(s[i])) && std::isdigit(static_cast<unsigned char>(s[i + 1])) && std::isdigit(static_cast<unsigned char>(s[i + 2])) && s[i + 3] == 'm' &&
            (i + 4 >= s.size() || !std::isalnum(static_cast<unsigned char>(s[i + 4])))) return true;
    return false;
}

} // namespace

std::vector<std::string> gpu_adapter_names() {
    std::vector<std::string> out;
#if defined(_WIN32)
    for (DWORD i = 0; i < 16; ++i) {
        DISPLAY_DEVICEA d{};
        d.cb = sizeof d;
        if (!EnumDisplayDevicesA(nullptr, i, &d, 0)) break;
        std::string n = trim(d.DeviceString);
        if (!n.empty() && std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
    }
#endif
    return out;
}

bool weak_graphics_only() {
    const auto names = gpu_adapter_names();
    if (names.empty()) return false;
    bool weak = false;
    for (const std::string &raw : names) {
        const std::string n = lower(raw);
        const bool discrete = has(n, "nvidia") || has(n, "geforce") || has(n, "rtx") || has(n, "gtx") || has(n, "quadro") || has(n, " arc ") || has(n, "arc(tm)") ||
                              has(n, "radeon rx") || has(n, "radeon pro") || has(n, "radeon(tm) rx") || has(n, "radeon r9") || has(n, "radeon hd 7") || has(n, "radeon vii");
        if (discrete) return false;
        if (has(n, "microsoft basic") || has(n, "uhd") || has(n, "hd graphics") || has(n, "iris") || has(n, "vega") || has(n, "intel") ||
            (has(n, "radeon") && (has(n, "graphics") || has_digits_then_m(n)))) weak = true;
    }
    return weak;
}

std::string system_description() {
    std::string out = "CPU: " + cpu_name() + " (" + std::to_string(std::thread::hardware_concurrency()) + " threads)";
#if defined(_WIN32)
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof mem;
    if (GlobalMemoryStatusEx(&mem)) out += "\nMemory: " + std::to_string(mem.ullTotalPhys / (1024ull * 1024ull * 1024ull)) + " GB installed";
    SYSTEM_POWER_STATUS ps{};
    if (GetSystemPowerStatus(&ps)) {
        std::string p = ps.ACLineStatus == 1 ? "plugged in" : ps.ACLineStatus == 0 ? "ON BATTERY" : "unknown";
        if (ps.BatteryLifePercent <= 100) p += ", battery " + std::to_string(static_cast<int>(ps.BatteryLifePercent)) + "%";
        if (ps.SystemStatusFlag == 1) p += ", battery saver ON";
        out += "\nPower: " + p;
    }
    {
        const auto adapters = gpu_adapter_names();
        std::string a;
        for (const std::string &n : adapters) a += (a.empty() ? "" : "; ") + n;
        if (!a.empty()) out += "\nGraphics adapters: " + a;
    }
#elif defined(__linux__)
    std::ifstream in("/proc/meminfo");
    std::string line;
    while (std::getline(in, line))
        if (line.rfind("MemTotal:", 0) == 0) {
            unsigned long long kb = 0;
            std::sscanf(line.c_str() + 9, "%llu", &kb);
            out += "\nMemory: " + std::to_string(kb / (1024ull * 1024ull)) + " GB installed";
            break;
        }
#endif
    return out;
}

} // namespace rr
