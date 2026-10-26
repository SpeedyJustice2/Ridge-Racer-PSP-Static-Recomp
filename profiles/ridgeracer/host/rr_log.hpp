#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace rr {

namespace category {
inline constexpr std::string_view kBoot = "BOOT";
inline constexpr std::string_view kDispatch = "DISPATCH";
inline constexpr std::string_view kHle = "HLE";
inline constexpr std::string_view kImport = "IMPORT";
inline constexpr std::string_view kFilesystem = "FILESYSTEM";
inline constexpr std::string_view kFile = "FILE";
inline constexpr std::string_view kModule = "MODULE";
inline constexpr std::string_view kGe = "GE";
inline constexpr std::string_view kMemory = "MEMORY";
inline constexpr std::string_view kError = "ERROR";
inline constexpr std::string_view kUmd = "UMD";
inline constexpr std::string_view kCallback = "CALLBACK";
inline constexpr std::string_view kThread = "THREAD";
} // namespace category

// Thread-safe line logger: stdout plus an optional log file.
void log_line(std::string_view category, std::string_view message);
bool open_log_file(const char *path);
void close_log_file();
std::string log_file_path();                              // the file opened with open_log_file ("" if none)
std::vector<std::string> recent_log_lines(bool crashing);  // the newest ~400 lines (for crash and bug reports)

} // namespace rr
