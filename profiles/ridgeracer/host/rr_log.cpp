#include "rr_log.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>

namespace rr {
namespace {
std::mutex g_log_mutex;
std::FILE *g_log_file = nullptr;
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
    if (g_log_file != nullptr) {
        std::fprintf(g_log_file, "[%.*s] %.*s\n", static_cast<int>(category.size()), category.data(),
                     static_cast<int>(message.size()), message.data());
        std::fflush(g_log_file);
    }
}

bool open_log_file(const char *path) {
    std::lock_guard<std::mutex> guard(g_log_mutex);
    if (g_log_file != nullptr) { std::fclose(g_log_file); g_log_file = nullptr; }
    g_log_file = std::fopen(path, "wb");
    return g_log_file != nullptr;
}

void close_log_file() {
    std::lock_guard<std::mutex> guard(g_log_mutex);
    if (g_log_file != nullptr) { std::fclose(g_log_file); g_log_file = nullptr; }
}

} // namespace rr
