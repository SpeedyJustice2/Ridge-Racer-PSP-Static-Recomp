#include "rr_settings.hpp"
#include "rr_iso.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <cstdint>
#endif

namespace rr {
namespace fs = std::filesystem;

namespace {

fs::path exe_dir() {
#ifdef _WIN32
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0u || n >= buf.size()) return fs::current_path();
    buf.resize(n);
    return fs::path(buf).parent_path();
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0) return fs::current_path();
    std::error_code ec;
    const fs::path p = fs::canonical(fs::path(buf), ec);
    return ec ? fs::current_path() : p.parent_path();
#else
    std::error_code ec;
    const fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::current_path() : p.parent_path();
#endif
}

std::string trim(const std::string &s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

int to_int(const std::string &v, int def, int lo, int hi) {
    char *end = nullptr;
    const long x = std::strtol(v.c_str(), &end, 10);
    if (end == v.c_str()) return def;
    return static_cast<int>(std::clamp<long>(x, lo, hi));
}

bool to_bool(const std::string &v, bool def) {
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    return def;
}

std::string fmt_float(float f) {
    std::ostringstream o;
    o << f;
    return o.str();
}

} // namespace

fs::path settings_file_path() { return exe_dir() / "ridgeracer.ini"; }

PlayerSettings load_settings(const fs::path &file) {
    PlayerSettings s;
    std::ifstream in(file);
    if (!in) return s;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (k == "game_path") s.game_path = v;
        else if (k == "show_launcher") s.show_launcher = to_bool(v, s.show_launcher);
        else if (k == "renderer") s.gpu = v != "software";
        else if (k == "render_scale") s.render_scale = to_int(v, s.render_scale, 1, 8);
        else if (k == "window_scale") s.window_scale = to_int(v, s.window_scale, 1, 8);
        else if (k == "fullscreen") s.fullscreen = to_bool(v, s.fullscreen);
        else if (k == "fps") s.fps = v == "monitor" ? 0 : to_int(v, s.fps, 0, 480);
        else if (k == "low_latency") s.low_latency = to_bool(v, s.low_latency);
        else if (k == "antialiasing") s.aa = v == "off" ? 0 : v == "fxaa" ? 1 : v == "msaa2" ? 2 : v == "msaa4" ? 3 : v == "msaa8" ? 4 : s.aa;
        else if (k == "anisotropic") s.aniso = to_int(v, s.aniso, 1, 16);
        else if (k == "mipmaps") s.mipmaps = to_bool(v, s.mipmaps);
        else if (k == "sharpen") s.sharpen = to_int(v, s.sharpen, 0, 3);
        else if (k == "hud_sharpness") s.ui_crisp = to_int(v, s.ui_crisp, 0, 2);
        else if (k == "smooth_cutouts") s.smooth_cutouts = to_bool(v, s.smooth_cutouts);
        else if (k == "volume") s.volume = to_int(v, s.volume, 0, 150);
        else if (k == "menu_volume") s.menu_volume = to_int(v, s.menu_volume, 0, 200);
    }
    return s;
}

bool save_settings(const fs::path &file, const PlayerSettings &s) {
    std::ofstream o(file, std::ios::trunc);
    if (!o) return false;
    static const char *const aa_names[] = {"off", "fxaa", "msaa2", "msaa4", "msaa8"};
    o << "# Ridge Racer settings. Written by the settings window (start the game with Shift held, or with --launcher, to open it).\n"
      << "game_path=" << s.game_path << "\n"
      << "show_launcher=" << (s.show_launcher ? 1 : 0) << "\n"
      << "\n[display]\n"
      << "renderer=" << (s.gpu ? "opengl" : "software") << "\n"
      << "render_scale=" << s.render_scale << "\n"
      << "window_scale=" << s.window_scale << "\n"
      << "fullscreen=" << (s.fullscreen ? 1 : 0) << "\n"
      << "fps=" << (s.fps == 0 ? std::string("monitor") : std::to_string(s.fps)) << "\n"
      << "low_latency=" << (s.low_latency ? 1 : 0) << "\n"
      << "\n[quality]\n"
      << "antialiasing=" << aa_names[std::clamp(s.aa, 0, 4)] << "\n"
      << "anisotropic=" << s.aniso << "\n"
      << "mipmaps=" << (s.mipmaps ? 1 : 0) << "\n"
      << "sharpen=" << s.sharpen << "\n"
      << "hud_sharpness=" << s.ui_crisp << "\n"
      << "smooth_cutouts=" << (s.smooth_cutouts ? 1 : 0) << "\n"
      << "\n[audio]\n"
      << "volume=" << s.volume << "\n"
      << "menu_volume=" << s.menu_volume << "\n";
    return static_cast<bool>(o);
}

namespace {
std::string lower_ext(const fs::path &p) {
    std::string e = p.extension().string();
    for (char &ch : e) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return e;
}
fs::path iso_cache_dir(const fs::path &iso) {
    std::string stem = iso.stem().string();
    for (char &ch : stem)
        if (std::string("\\/:*?\"<>|").find(ch) != std::string::npos) ch = '_';
    if (stem.empty()) stem = "disc";
    return exe_dir() / "game" / stem;
}
std::string iso_stamp(const fs::path &iso) {
    std::error_code ec;
    const auto size = fs::file_size(iso, ec);
    const auto t = fs::last_write_time(iso, ec).time_since_epoch().count();
    return std::to_string(static_cast<unsigned long long>(size)) + " " + std::to_string(static_cast<long long>(t));
}
bool iso_cache_valid(const fs::path &iso) {
    const fs::path dir = iso_cache_dir(iso);
    std::error_code ec;
    if (!fs::is_regular_file(dir / "PSP_GAME" / "SYSDIR" / "BOOT.BIN", ec)) return false;
    std::ifstream in(dir / ".rr-iso-stamp");
    std::string line;
    std::getline(in, line);
    return line == iso_stamp(iso);
}
} // namespace

bool is_iso_path(const fs::path &p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec) && lower_ext(p) == ".iso";
}

std::string prepare_game(const fs::path &picked, const std::function<void(int)> &progress) {
    if (!is_iso_path(picked) || iso_cache_valid(picked)) return {};
    const fs::path dir = iso_cache_dir(picked);
    std::error_code ec;
    fs::remove_all(dir, ec);
    std::string err;
    int last = -1;
    const bool ok = iso_extract_all(picked, dir, err, [&](std::uint64_t done, std::uint64_t total) {
        const int pct = total ? static_cast<int>(done * 100u / total) : 100;
        if (pct != last) { last = pct; if (progress) progress(pct); }
    });
    if (!ok) { fs::remove_all(dir, ec); return err; }
    std::ofstream stamp(dir / ".rr-iso-stamp", std::ios::trunc);
    stamp << iso_stamp(picked) << "\n";
    return stamp ? std::string() : std::string("Could not write into ") + dir.string();
}

fs::path resolve_game_path(const fs::path &picked) {
    if (picked.empty()) return {};
    std::error_code ec;
    if (is_iso_path(picked)) {
        const fs::path boot = iso_cache_dir(picked) / "PSP_GAME" / "SYSDIR" / "BOOT.BIN";
        return iso_cache_valid(picked) ? boot : picked;
    }
    if (fs::is_regular_file(picked, ec)) return picked;
    static const char *const tails[] = {"PSP_GAME/SYSDIR/BOOT.BIN", "SYSDIR/BOOT.BIN", "BOOT.BIN", "RidgeRacerPSPFiles/PSP_GAME/SYSDIR/BOOT.BIN"};
    for (const char *t : tails) {
        const fs::path p = picked / fs::path(t);
        if (fs::is_regular_file(p, ec)) return p;
    }
    return {};
}

fs::path find_game_files() {
    fs::path d = exe_dir();
    for (int up = 0; up < 7 && !d.empty(); ++up) {
        if (fs::path p = resolve_game_path(d); !p.empty()) return p;
        const fs::path parent = d.parent_path();
        if (parent == d) break;
        d = parent;
    }
    return {};
}

std::string check_game_file(const fs::path &boot_bin) {
    std::error_code ec;
    if (is_iso_path(boot_bin)) {   // a disc image that has not been copied yet: look inside it
        IsoImage img;
        std::string err;
        if (!img.open(boot_bin, err)) return err;
        const IsoEntry *e = img.find("PSP_GAME/SYSDIR/BOOT.BIN");
        if (!e) return "This disc image has no PSP_GAME/SYSDIR/BOOT.BIN - is it a PSP game?";
        const auto head = img.read_head(*e, 4);
        if (head.size() == 4 && head[0] == '~' && head[1] == 'P' && head[2] == 'S' && head[3] == 'P')
            return "This disc image is encrypted. A decrypted dump of your own game is needed (see the README).";
        if (head.size() != 4 || !(head[0] == 0x7f && head[1] == 'E' && head[2] == 'L' && head[3] == 'F'))
            return "BOOT.BIN inside the disc image is not a PSP program.";
        return {};
    }
    if (boot_bin.empty() || !fs::is_regular_file(boot_bin, ec)) return "BOOT.BIN not found. Pick the folder that holds PSP_GAME.";
    std::ifstream in(boot_bin, std::ios::binary);
    char magic[4] = {};
    in.read(magic, 4);
    if (in.gcount() != 4) return "BOOT.BIN could not be read.";
    if (magic[0] == '~' && magic[1] == 'P' && magic[2] == 'S' && magic[3] == 'P')
        return "This BOOT.BIN is still encrypted. The decrypted one is needed.";
    if (!(magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F')) return "BOOT.BIN is not a PSP program.";
    if (!fs::is_directory(boot_bin.parent_path().parent_path(), ec)) return "The PSP_GAME folder is missing.";
    return {};
}

std::vector<std::string> to_args(const PlayerSettings &s, int monitor_hz) {
    std::vector<std::string> a;
    fs::path game = resolve_game_path(fs::path(s.game_path));
    if (game.empty()) game = fs::path(s.game_path);
    a.push_back(game.string());
    a.push_back("--scale"); a.push_back(std::to_string(std::max(1, s.window_scale)));
    if (s.fullscreen) a.push_back("--fullscreen");
    if (s.gpu) {
        a.push_back("--gpu");
        a.push_back("--internal-scale"); a.push_back(std::to_string(std::clamp(s.render_scale, 1, 8)));
        switch (s.aa) {
        case 1: a.push_back("--fxaa"); break;
        case 2: a.push_back("--msaa"); a.push_back("2"); break;
        case 3: a.push_back("--msaa"); a.push_back("4"); break;
        case 4: a.push_back("--msaa"); a.push_back("8"); break;
        default: break;
        }
        a.push_back("--aniso"); a.push_back(std::to_string(std::clamp(s.aniso, 1, 16)));
        if (!s.mipmaps) a.push_back("--no-mipmaps");
        static const float sharpen_levels[] = {0.0f, 0.35f, 0.6f, 1.0f};
        if (s.sharpen > 0) { a.push_back("--sharpen"); a.push_back(fmt_float(sharpen_levels[std::clamp(s.sharpen, 0, 3)])); }
        static const float crisp_levels[] = {0.0f, 0.8f, 1.0f};
        a.push_back("--ui-crisp"); a.push_back(fmt_float(crisp_levels[std::clamp(s.ui_crisp, 0, 2)]));
        if (s.smooth_cutouts) a.push_back("--smooth-cutouts");
    }
    int fps = s.fps == 0 ? monitor_hz : s.fps;
    if (fps < 62) fps = 60;   // 59.94 / 60 Hz monitors: the game's own rate
    fps = std::min(fps, 240);
    if (fps != 60) { a.push_back("--fps"); a.push_back(std::to_string(fps)); a.push_back("--fps-patch"); a.push_back("car"); }
    a.push_back(s.low_latency ? "--low-latency" : "--no-low-latency");
    a.push_back("--volume"); a.push_back(std::to_string(s.volume));
    a.push_back("--menu-volume"); a.push_back(std::to_string(s.menu_volume));
    return a;
}

std::filesystem::path executable_dir() { return exe_dir(); }

} // namespace rr
