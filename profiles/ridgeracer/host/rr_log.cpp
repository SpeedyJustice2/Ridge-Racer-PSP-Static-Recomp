#include "rr_log.hpp"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace rr {
namespace {
std::mutex g_log_mutex;
std::FILE *g_log_file = nullptr;
std::string g_log_path;
std::deque<std::string> g_recent;   // the newest lines, for crash and bug reports
constexpr std::size_t kRecentLines = 400;
} // namespace

namespace {
// Per-frame diagnostics that drown the useful lines; set RR_VERBOSE=1 to log them again.
bool is_noise(std::string_view category, std::string_view m) {
    if (category == "CALLBACK") return true;
    if (category == "THREAD") return m.starts_with("wake ");
    if (category != "GE") return false;
    for (const std::string_view p : {"GELITCLOUD", "GLTEX addr", "GECLUT", "GESTATE", "GECMD", "GEMARGIN"})
        if (m.starts_with(p)) return true;
    return false;
}
} // namespace

void log_line(std::string_view category, std::string_view message) {
    static const bool verbose = std::getenv("RR_VERBOSE") != nullptr;
    if (!verbose && is_noise(category, message)) return;
    std::lock_guard<std::mutex> guard(g_log_mutex);
    std::cout << '[' << category << "] " << message << '\n';
    if (g_recent.size() >= kRecentLines) g_recent.pop_front();
    g_recent.emplace_back(std::string("[") + std::string(category) + "] " + std::string(message.substr(0, 600)));
    if (g_log_file != nullptr) {
        std::fprintf(g_log_file, "[%.*s] %.*s\n", static_cast<int>(category.size()), category.data(),
                     static_cast<int>(message.size()), message.data());
        std::fflush(g_log_file);
    }
}

bool open_log_file(const char *path) {
    std::lock_guard<std::mutex> guard(g_log_mutex);
    if (g_log_file != nullptr) { std::fclose(g_log_file); g_log_file = nullptr; }
    {   // keep the previous session's log (ridgeracer-previous.log): after a crash the player usually starts the game again
        std::error_code ec;
        const std::filesystem::path p(path), prev = p.parent_path() / (p.stem().string() + "-previous" + p.extension().string());
        if (std::filesystem::is_regular_file(p, ec)) { std::filesystem::remove(prev, ec); std::filesystem::rename(p, prev, ec); }
    }
    g_log_file = std::fopen(path, "wb");
    g_log_path = g_log_file != nullptr ? path : "";
    return g_log_file != nullptr;
}

void close_log_file() {
    std::lock_guard<std::mutex> guard(g_log_mutex);
    if (g_log_file != nullptr) { std::fclose(g_log_file); g_log_file = nullptr; }
}

std::string log_file_path() {
    std::lock_guard<std::mutex> guard(g_log_mutex);
    return g_log_path;
}

std::vector<std::string> recent_log_lines(bool crashing) {
    // While crashing the lock may be held by the thread that failed: then take the lines without it (a torn last line at
    // worst) rather than wait forever.
    std::unique_lock<std::mutex> lock(g_log_mutex, std::defer_lock);
    if (crashing) { for (int i = 0; i < 50 && !lock.try_lock(); ++i) {} }
    else lock.lock();
    return std::vector<std::string>(g_recent.begin(), g_recent.end());
}

} // namespace rr
