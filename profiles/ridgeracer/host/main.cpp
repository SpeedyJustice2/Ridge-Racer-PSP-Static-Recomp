// Ridge Racer (PSP, 2004) - Stage 1 "boot-only" host.
//
// Stage 2: the kernel layer (threads, semaphores, event flags, callbacks,
// file I/O against disc0:/, memory partitions, UMD, timing) is installed.
// Graphics (sceGe/sceDisplay), controls, video and ATRAC audio are NOT yet
// implemented: the framework stops with an "[HLE MISSING]" report at the first
// such import, and that report is the to-do list for the next stage.
//
// Usage:
//   RidgeRacerNative <decrypted ELF> [disc root] [--max-dispatches N] [--log FILE] [--quiet] [--trace-all] [--no-window] [--scale N] [--gpu] [--internal-scale N] [--low-latency] [--no-low-latency] [--latency-log] [--fps N] [--fps-mult X] [--fps-patch [all|physics|car]] [--fps-everywhere] [--fps-gate ADDR]... [--trace-tools] [--shot-dir DIR] [--shot-mode window|native|both|raw] [--aa none|fxaa|taa|ssaa|msaa] [--msaa N] [--ssaa-factor X] [--fxaa] [--no-fxaa] [--supersample] [--sharpen X] [--no-mipmaps] [--aniso N] [--ui-crisp X] [--smooth-cutouts] [--online|--no-online] [--button-prompts auto|xbox|playstation|nintendo|keyboard] [--triggers|--no-triggers] [--perf-report FILE] [--draw-distance N] [--frame-skip N] [--alpha-cutoff N] [--volume PERCENT] [--menu-volume PERCENT] [--fullscreen] [--unthrottled] [--screenshots DIR EVERY_N_FRAMES [MAX]] [--replay-input LOG] [--stop-after-frame N]
//
// <decrypted ELF>  e.g. ...\RidgeRacerPSPFiles\PSP_GAME\SYSDIR\BOOT.BIN
// [disc root]      folder that contains PSP_GAME (default: derived from the
//                  ELF path, three levels up from BOOT.BIN)

#include "rr_hle.hpp"
#include "rr_env.hpp"
#include "rr_gen_loader.hpp"
#include "rr_log.hpp"
#include "rr_net.hpp"
#include "rr_pad.hpp"
#include "rr_report.hpp"
#include "rr_settings.hpp"
#include "rr_update.hpp"
#include "rr_sysinfo.hpp"
#include <cstdio>

#include "psprecomp/common.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/runtime.hpp"
#include "psprecomp/sha256.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

void log(const std::string &message) { rr::log_line("RR", message); std::cout.flush(); }

bool g_from_launcher = false;   // started by a double-click / --launcher: errors also go to a message box

// Started with no arguments (a double-click) or with --launcher: read ridgeracer.ini, show the settings window when
// wanted, and turn the settings into command-line options.  Options typed after --launcher come later and win.
// Returns false when the player quit from the settings window.
bool apply_launcher(int &argc, char **&argv, std::vector<std::string> &store, std::vector<char *> &ptrs) {
    bool launcher_flag = false, log_given = false;
    std::vector<std::string> rest;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--launcher") { launcher_flag = true; continue; }
        if (a == "--log") log_given = true;
        rest.push_back(a);
    }
    const bool double_click = argc == 1;
    if (!double_click && !launcher_flag) return true;
    g_from_launcher = true;
    if (double_click) rr::hide_own_console();
    rr::update::cleanup_old_files();   // (files an update replaced while the game was running)
    const std::filesystem::path ini = rr::settings_file_path();
    std::error_code ec;
    const bool ini_exists = std::filesystem::is_regular_file(ini, ec);
    rr::PlayerSettings s = rr::load_settings(ini);
    if (!rr::check_game_file(rr::resolve_game_path(s.game_path)).empty()) {
        const std::filesystem::path found = rr::find_game_files();
        if (!found.empty()) s.game_path = found.string();
    }
    if (!ini_exists && rr::weak_graphics_only()) {   // first run on an integrated / basic graphics chip: start with light settings
        s.render_scale = 2; s.aa = 0; s.aniso = 1; s.ui_crisp = 1; s.smooth_cutouts = false; s.frame_skip = 0;
    }
    const bool game_ok = rr::check_game_file(rr::resolve_game_path(s.game_path)).empty();
    if (launcher_flag || !ini_exists || s.show_launcher || !game_ok || rr::launcher_shift_held()) {
        if (!rr::run_launcher(s)) return false;
        rr::save_settings(ini, s);
    }
    store = rr::to_args(s, rr::monitor_refresh_hz());
    if (double_click && !log_given) { store.push_back("--log"); store.push_back((ini.parent_path() / "ridgeracer.log").string()); }
    store.insert(store.end(), rest.begin(), rest.end());
    ptrs.clear();
    ptrs.push_back(argv[0]);
    for (auto &a : store) ptrs.push_back(a.data());
    ptrs.push_back(nullptr);
    argc = static_cast<int>(ptrs.size()) - 1;
    argv = ptrs.data();
    return true;
}

} // namespace

#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
#define RR_HAVE_SAMPLER 1
// Dev-only sampling profiler (RR_SAMPLER=file): SIGPROF every 1 ms of CPU time, records the interrupted instruction address and thread.
#include <csignal>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <link.h>
namespace {
struct Sample { unsigned long long ip; int tid; };
Sample g_samples[400000];
volatile int g_nsamples = 0;
const char *g_sampler_path = nullptr;
void sampler_handler(int, siginfo_t *, void *uc) {
    const int n = g_nsamples;
    if (n >= 400000) return;
    g_samples[n].ip = static_cast<unsigned long long>(static_cast<ucontext_t *>(uc)->uc_mcontext.gregs[REG_RIP]);
    g_samples[n].tid = static_cast<int>(syscall(SYS_gettid));
    g_nsamples = n + 1;
}
void sampler_dump() {
    if (!g_sampler_path) return;
    FILE *f = std::fopen(g_sampler_path, "w");
    if (!f) return;
    dl_iterate_phdr([](struct dl_phdr_info *info, size_t, void *arg) -> int { std::fprintf(static_cast<FILE *>(arg), "BASE %llx\n", static_cast<unsigned long long>(info->dlpi_addr)); return 1; }, f);
    for (int i = 0; i < g_nsamples; ++i) std::fprintf(f, "%d %llx\n", g_samples[i].tid, g_samples[i].ip);
    std::fclose(f);
}
void sampler_start() {
    g_sampler_path = std::getenv("RR_SAMPLER");
    if (!g_sampler_path || !*g_sampler_path) { g_sampler_path = nullptr; return; }
    struct sigaction sa; std::memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = sampler_handler; sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, nullptr);
    itimerval it; it.it_interval.tv_sec = 0; it.it_interval.tv_usec = 1000; it.it_value = it.it_interval;
    setitimer(ITIMER_PROF, &it, nullptr);
    std::atexit(sampler_dump);
}
}
#endif

int main(int argc, char **argv) {
#ifdef RR_HAVE_SAMPLER
    sampler_start();
#endif
    for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == "--dev") rr::g_rr_dev = true;
#ifdef _WIN32
    if (!rr::g_rr_dev) {
        // Normal runs ignore the RR_* diagnostic switches.
        std::vector<std::string> names;
        for (char **e = _environ; e && *e; ++e)
            if (std::strncmp(*e, "RR_", 3) == 0) { std::string s = *e; names.push_back(s.substr(0, s.find('='))); }
        for (const auto &n : names) _putenv_s(n.c_str(), "");
    }
#endif
    // "--bug-report <reason>": pack logs, settings and crash files into a zip on the Desktop and say where it is (started by
    // the crash handler after a crash; also usable by hand).
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--bug-report") return rr::report::run_report_mode(i + 1 < argc ? argv[i + 1] : "by hand");
    rr::report::install_crash_handler();
    // Used by the installer: copy the files out of a disc image (once) and print progress lines, then exit.
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) != "--prepare-game") continue;
        const std::string prep_err = rr::prepare_game(argv[i + 1], [](int pct) { std::printf("@@PCT %d 100\n", pct); std::fflush(stdout); });
        if (!prep_err.empty()) { std::printf("@@ERROR %s\n", prep_err.c_str()); std::fflush(stdout); return 1; }
        return 0;
    }
    std::vector<std::string> launcher_args;
    std::vector<char *> launcher_ptrs;
    try {
        if (!apply_launcher(argc, argv, launcher_args, launcher_ptrs)) return 0;
        std::vector<std::string> positional;
        std::uint64_t max_dispatches = 50'000'000u;
        bool max_dispatches_given = false;
        bool trace = true, trace_all = false;
        bool window = true, throttle = true;
        int window_scale = 3;
        bool use_gpu = false;
        int internal_scale = 3;
        int aa_mode = 0, msaa = 0;   // 0 none, 1 FXAA, 2 TAA, 3 SSAA filter; MSAA samples
        float ssaa_factor = 1.5f;
        bool internal_given = false, aa_given = false;
        bool mipmaps = true;   // the default look is plain resolution scaling: no smoothing or sharpening filters
        float sharpen = 0.0f;
        bool low_latency = true, latency_log = false;   // low-latency on by default (measured: about 1.2 ms less delay, no fps cost)
        bool supersample = false, sharpen_given = false;
        int aniso = 8;
        float ui_crisp = 0.8f;
        bool smooth_cutouts = false;
        int draw_cells = 0;
        int frame_skip = 0;   // automatic: pictures are skipped only while the PC cannot keep the game at full speed
        float volume = 1.0f, menu_volume = 1.0f;
        int alpha_cutoff = 0;
        bool fullscreen = false;
        double fps_mult = 1.0;
        int fps_patch = 0;
        bool trace_tools = false;
        bool fps_everywhere = false;
        bool menu_interp = std::getenv("RR_MENU_INTERP") != nullptr;
        bool menu_native = [] { const char *e = std::getenv("RR_MENU_NATIVE"); return e == nullptr || std::string(e) != "0"; }();   // on by default (--no-menu-native / RR_MENU_NATIVE=0: menus at 60)
        std::vector<std::uint32_t> fps_gates;
        std::string shot_dir = "screenshots";
        int shot_mode = 0;
        std::string screenshot_dir;
        std::uint64_t screenshot_every = 0, screenshot_max = 50;
        std::string replay_input;
        std::uint64_t stop_after_frame = 0;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--dev") {
                continue;
            } else if (arg == "--max-dispatches") {
                if (i + 1 >= argc) throw psprecomp::Error("--max-dispatches needs a number");
                max_dispatches = std::strtoull(argv[++i], nullptr, 0);
                max_dispatches_given = true;
            } else if (arg == "--quiet") {
                trace = false;
            } else if (arg == "--trace-all") {
                trace_all = true;
            } else if (arg == "--no-window") {
                window = false;
            } else if (arg == "--unthrottled") {
                throttle = false;
            } else if (arg == "--gpu") {
                use_gpu = true;
            } else if (arg == "--internal-scale") {
                if (i + 1 >= argc) throw psprecomp::Error("--internal-scale needs a number");
                internal_scale = std::clamp(std::atoi(argv[++i]), 1, 8);
                internal_given = true;
                use_gpu = true;
            } else if (arg == "--no-fxaa") {
                if (aa_mode == 1) aa_mode = 0;
                aa_given = true;
            } else if (arg == "--low-latency") {
                low_latency = true;
            } else if (arg == "--no-low-latency") {
                low_latency = false;
            } else if (arg == "--latency-log") {
                latency_log = true;
            } else if (arg == "--fxaa") {
                aa_mode = 1; aa_given = true;
            } else if (arg == "--aa") {
                if (i + 1 >= argc) throw psprecomp::Error("--aa needs none, fxaa, taa, ssaa or msaa");
                const std::string m = argv[++i];
                aa_given = true;
                if (m == "none") { aa_mode = 0; msaa = 0; }
                else if (m == "fxaa") aa_mode = 1;
                else if (m == "taa") aa_mode = 2;
                else if (m == "ssaa") aa_mode = 3;
                else if (m == "msaa") { if (msaa < 2) msaa = 4; }
                else throw psprecomp::Error("--aa needs none, fxaa, taa, ssaa or msaa");
                use_gpu = true;
            } else if (arg == "--msaa") {
                if (i + 1 >= argc) throw psprecomp::Error("--msaa needs a sample count (0 = off, 2, 4 or 8)");
                msaa = std::clamp(std::atoi(argv[++i]), 0, 16);
                aa_given = true; use_gpu = true;
            } else if (arg == "--ssaa-factor") {
                if (i + 1 >= argc) throw psprecomp::Error("--ssaa-factor needs a number (1.5 default, 2 = four samples per pixel)");
                ssaa_factor = std::clamp(static_cast<float>(std::atof(argv[++i])), 1.0f, 4.0f);
            } else if (arg == "--fps") {
                // Target frame rate during races, e.g. 120, 144, 165, 240 (the PSP runs at 59.94).
                if (i + 1 >= argc) throw psprecomp::Error("--fps needs a frame rate such as 120, 144 or 240");
                fps_mult = std::clamp(std::atof(argv[++i]) / 59.94, 0.25, 8.0);
            } else if (arg == "--fps-mult") {
                if (i + 1 >= argc) throw psprecomp::Error("--fps-mult needs a number (1 = normal, 2 = 120 Hz)");
                fps_mult = std::clamp(std::atof(argv[++i]), 0.25, 8.0);
            } else if (arg == "--fps-gate") {
                if (i + 1 >= argc) throw psprecomp::Error("--fps-gate needs a game function address (hex)");
                fps_gates.push_back(static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 0)));
            } else if (arg == "--fps-everywhere") {
                fps_everywhere = true;
            } else if (arg == "--menu-interp" || arg == "--no-menu-interp") {
                menu_interp = arg == "--menu-interp";
            } else if (arg == "--menu-native" || arg == "--no-menu-native") {
                menu_native = arg == "--menu-native";
            } else if (arg == "--trace-tools") {
                trace_tools = true;
            } else if (arg == "--fps-patch") {
                fps_patch = 1;
                if (i + 1 < argc && std::string(argv[i + 1]) == "all") { fps_patch = 2; ++i; }
                else if (i + 1 < argc && std::string(argv[i + 1]) == "physics") { fps_patch = 3; ++i; }
                else if (i + 1 < argc && std::string(argv[i + 1]) == "car") { fps_patch = 4; ++i; }
            } else if (arg == "--shot-dir") {
                if (i + 1 >= argc) throw psprecomp::Error("--shot-dir needs a folder name");
                shot_dir = argv[++i];
            } else if (arg == "--shot-mode") {
                if (i + 1 >= argc) throw psprecomp::Error("--shot-mode needs window, native, both or raw");
                const std::string m = argv[++i];
                if (m == "window") shot_mode = 0; else if (m == "native") shot_mode = 1; else if (m == "both") shot_mode = 2; else if (m == "raw") shot_mode = 3;
                else throw psprecomp::Error("--shot-mode needs window, native, both or raw");
            } else if (arg == "--supersample") {
                supersample = true;
            } else if (arg == "--sharpen") {
                if (i + 1 >= argc) throw psprecomp::Error("--sharpen needs a number (0 = off and the default, 0.35 mild, 1 = strong)");
                sharpen = std::clamp(static_cast<float>(std::atof(argv[++i])), 0.0f, 2.0f); sharpen_given = true;
            } else if (arg == "--no-mipmaps") {
                mipmaps = false;
            } else if (arg == "--fullscreen") {
                fullscreen = true;
            } else if (arg == "--volume") {
                if (i + 1 >= argc) throw psprecomp::Error("--volume needs a percentage (0-150, 100 = the game's own level)");
                volume = std::clamp(static_cast<float>(std::atof(argv[++i])) / 100.0f, 0.0f, 1.5f);
            } else if (arg == "--alpha-cutoff") {
                if (i + 1 >= argc) throw psprecomp::Error("--alpha-cutoff needs a number 0-160 (0 = the game's own behaviour)");
                alpha_cutoff = std::clamp(std::atoi(argv[++i]), 0, 160);
            } else if (arg == "--menu-volume") {
                if (i + 1 >= argc) throw psprecomp::Error("--menu-volume needs a percentage (0-200)");
                menu_volume = std::clamp(static_cast<float>(std::atof(argv[++i])) / 100.0f, 0.0f, 2.0f);
            } else if (arg == "--draw-distance") {
                if (i + 1 >= argc) throw psprecomp::Error("--draw-distance needs a number (0 = the game's own, 2-8 = draw scenery from that many track cells further)");
                draw_cells = std::clamp(std::atoi(argv[++i]), 0, 64);
            } else if (arg == "--frame-skip") {
                if (i + 1 >= argc) throw psprecomp::Error("--frame-skip needs a number (1 = off, 2 = draw every 2nd frame in races, 3 = every 3rd) or auto (skip pictures only while the game runs behind real time)");
                { const std::string v = argv[++i]; frame_skip = (v == "auto") ? 0 : std::clamp(std::atoi(v.c_str()), 0, 4); }
            } else if (arg == "--smooth-cutouts") {
                smooth_cutouts = true;
            } else if (arg == "--perf-report") {
                if (i + 1 >= argc) throw psprecomp::Error("--perf-report needs a file name");
                rr::set_perf_report(argv[++i]);
            } else if (arg == "--button-prompts") {
                if (i + 1 >= argc) throw psprecomp::Error("--button-prompts needs auto, xbox, playstation, nintendo or keyboard");
                rr::pad::set_prompts(rr::pad::parse_prompts(argv[++i], rr::pad::Prompts::Auto));
            } else if (arg == "--triggers" || arg == "--no-triggers") {
                rr::pad::set_triggers(arg == "--triggers");
            } else if (arg == "--online" || arg == "--no-online") {
                rr::net::request_online(arg == "--online");
                if (arg == "--online") rr::net::set_log_file((rr::executable_dir() / "online-log.txt").string());
            } else if (arg == "--ui-crisp") {
                if (i + 1 >= argc) throw psprecomp::Error("--ui-crisp needs a number (0 = off, 0.8 default, 1 = crispest)");
                ui_crisp = std::clamp(static_cast<float>(std::atof(argv[++i])), 0.0f, 1.0f);
            } else if (arg == "--aniso") {
                if (i + 1 >= argc) throw psprecomp::Error("--aniso needs a number (1, 2, 4, 8 or 16)");
                aniso = std::clamp(std::atoi(argv[++i]), 1, 16);
            } else if (arg == "--scale") {
                if (i + 1 >= argc) throw psprecomp::Error("--scale needs a number");
                window_scale = std::max(1, std::atoi(argv[++i]));
            } else if (arg == "--replay-input") {
                if (i + 1 >= argc) throw psprecomp::Error("--replay-input needs a log file");
                replay_input = argv[++i];
            } else if (arg == "--stop-after-frame") {
                if (i + 1 >= argc) throw psprecomp::Error("--stop-after-frame needs a number");
                stop_after_frame = std::strtoull(argv[++i], nullptr, 0);
            } else if (arg == "--screenshots") {
                // --screenshots DIR EVERY_N_FRAMES [MAX]
                if (i + 2 >= argc) throw psprecomp::Error("--screenshots needs DIR and EVERY_N_FRAMES");
                screenshot_dir = argv[++i];
                screenshot_every = std::strtoull(argv[++i], nullptr, 0);
                if (i + 1 < argc && argv[i + 1][0] != '-') screenshot_max = std::strtoull(argv[++i], nullptr, 0);
            } else if (arg == "--log") {
                if (i + 1 >= argc) throw psprecomp::Error("--log needs a file name");
                if (!rr::open_log_file(argv[++i])) throw psprecomp::Error("Cannot open log file");
            } else if (!arg.empty() && arg[0] != '-') {
                positional.push_back(arg);
            } else {
                throw psprecomp::Error("Unknown option: " + arg);
            }
        }
        // A window means the person wants to play: run until it is closed.
        if (window && !max_dispatches_given) max_dispatches = ~0ull;
        if (positional.empty()) {
            std::cerr << "Usage: RidgeRacerNative <decrypted ELF> [disc root] [--max-dispatches N] [--log FILE] [--quiet] [--trace-all] [--no-window] [--scale N] [--gpu] [--internal-scale N] [--low-latency] [--no-low-latency] [--latency-log] [--fps N] [--fps-mult X] [--fps-patch [all|physics|car]] [--fps-everywhere] [--fps-gate ADDR]... [--trace-tools] [--shot-dir DIR] [--shot-mode window|native|both|raw] [--aa none|fxaa|taa|ssaa|msaa] [--msaa N] [--ssaa-factor X] [--fxaa] [--no-fxaa] [--supersample] [--sharpen X] [--no-mipmaps] [--aniso N] [--ui-crisp X] [--volume PERCENT] [--menu-volume PERCENT] [--fullscreen] [--unthrottled] [--screenshots DIR EVERY_N_FRAMES [MAX]] [--replay-input LOG] [--stop-after-frame N] [--launcher]\n";
            return 2;
        }

        if (rr::is_iso_path(positional[0])) {   // a disc image given directly: copy its files out first (once)
            const std::string iso_err = rr::prepare_game(positional[0], [](int pct) {
                std::cerr << "\rCopying game files from the disc image... " << pct << "%" << std::flush;
            });
            std::cerr << "\n";
            if (!iso_err.empty()) throw psprecomp::Error(iso_err);
            positional[0] = rr::resolve_game_path(positional[0]).string();
        }
        const std::filesystem::path elf_path = positional[0];
        std::filesystem::path disc_root = positional.size() > 1
            ? std::filesystem::path(positional[1])
            : elf_path.parent_path().parent_path().parent_path();

        std::error_code ec;
        if (!std::filesystem::is_regular_file(elf_path, ec))
            throw psprecomp::Error("Executable not found: " + elf_path.string());

        psprecomp::Elf32Image elf = psprecomp::Elf32Image::from_file(elf_path);
        log("ELF type=" + psprecomp::hex32(elf.type()) + (elf.is_psp_prx() ? " (PSP PRX)" : " (plain ELF)") +
            " entry=" + psprecomp::hex32(elf.runtime_entry()));
        log("SHA-256 " + psprecomp::sha256_file(elf_path));

        if (trace_tools) {
            // Route every guest store through the checked path so the car trace (Delete key) can watch memory at run time.
#ifdef _WIN32
            _putenv_s("PSPRECOMP_WATCH_WRITE", "0");
#else
            setenv("PSPRECOMP_WATCH_WRITE", "0", 1);
#endif
        }
        psprecomp::Runtime runtime(32u * 1024u * 1024u);
        runtime.set_game_root(disc_root);
        log("disc0:/ -> " + disc_root.string() +
            (std::filesystem::is_directory(disc_root / "PSP_GAME", ec) ? "" : "   (WARNING: no PSP_GAME folder here)"));

        const auto relocations =
            elf.load_and_relocate(runtime.memory(), psprecomp::kDefaultPspUserLoadBase);
        std::uint64_t image_end = 0u;
        for (std::size_t index = 0; index < elf.segments().size(); ++index) {
            const auto &segment = elf.segments()[index];
            if (segment.type != 1u) continue;
            const std::uint64_t start = elf.segment_runtime_address(index, psprecomp::kDefaultPspUserLoadBase);
            if (start + segment.memory_size > image_end) image_end = start + segment.memory_size;
        }
        if (image_end == 0u || image_end > 0x0A000000ull)
            throw psprecomp::Error("Invalid PSP ELF load image extent");
        const std::uint32_t user_arena_start = static_cast<std::uint32_t>((image_end + 0xFFu) & ~0xFFull);
        log("image end=" + psprecomp::hex32(static_cast<std::uint32_t>(image_end)) +
            " heap start=" + psprecomp::hex32(user_arena_start));
        log("relocations total=" + std::to_string(relocations.total) +
            " invalid=" + std::to_string(relocations.invalid) +
            " unsupported=" + std::to_string(relocations.unsupported));

        {
            const std::string err = rr::load_game_code(runtime, rr::executable_directory());
            if (!err.empty()) { log("ERROR: " + err); rr::show_error_box(err); return 3; }
        }
        log("linked recompiled functions/entry points: " + std::to_string(runtime.function_count()));
        if (runtime.function_count() == 0u) {
            log("ERROR: the generated corpus is empty. Check PSPRECOMP_RIDGERACER_GENERATED_DIR.");
            return 3;
        }

        const auto module = elf.find_module_info(runtime.memory(), psprecomp::kDefaultPspUserLoadBase);
        if (!module) throw psprecomp::Error("PSP module info not found after relocation");
        log("module \"" + module->name + "\" gp=" + psprecomp::hex32(module->gp));

        rr::HleOptions hle_options;
        hle_options.disc_root = disc_root;
        hle_options.trace_imports = trace;
        hle_options.trace_filesystem = trace;
        hle_options.window = window;
        hle_options.window_scale = window_scale;
        hle_options.gpu = use_gpu;
        hle_options.internal_scale = internal_scale;
        if (supersample) {   // preset: FXAA + mild sharpening, meant for an internal scale above the window scale
            if (!aa_given) aa_mode = 1;
            if (!sharpen_given) sharpen = 0.35f;
        }
        if (aa_mode == 3 && !internal_given) {   // SSAA: render larger than the window shows, then filter down
            internal_scale = std::clamp(static_cast<int>(std::ceil(static_cast<float>(window_scale) * ssaa_factor)), 1, 8);
            use_gpu = true;
        }
        hle_options.low_latency = low_latency; hle_options.latency_log = latency_log;
        hle_options.aa_mode = aa_mode;
        hle_options.msaa = msaa;
        hle_options.sharpen = sharpen;
        hle_options.mipmaps = mipmaps;
        hle_options.aniso = aniso;
        hle_options.ui_crisp = ui_crisp;
        hle_options.smooth_cutouts = smooth_cutouts;
        hle_options.draw_cells = draw_cells;
        hle_options.frame_skip = frame_skip;
        hle_options.volume = volume;
        hle_options.alpha_cutoff = alpha_cutoff;
        hle_options.menu_volume = menu_volume;
        hle_options.fullscreen = fullscreen;
        hle_options.fps_mult = fps_mult;
        hle_options.fps_patch = fps_patch;
        hle_options.fps_race_only = !fps_everywhere;
        hle_options.menu_interp = menu_interp;
        hle_options.menu_native = menu_native;
        hle_options.fps_gates = fps_gates;
        hle_options.trace_tools = trace_tools;
        hle_options.shot_dir = shot_dir;
        hle_options.shot_mode = shot_mode;
        hle_options.throttle = throttle;
        hle_options.screenshot_dir = screenshot_dir;
        hle_options.screenshot_every = screenshot_every;
        hle_options.screenshot_max = screenshot_max;
        hle_options.replay_input = replay_input;
        hle_options.stop_after_frame = stop_after_frame;
        if (trace_all) hle_options.import_trace_limit = 0;
        rr::install_hle(runtime, user_arena_start, hle_options);

        // module_start context, copied from the working MotorStorm profile:
        // top-of-RAM stack, gp from the module info, and $ra = 0.  The game
        // receives (size, argp) where argp points at its own boot path string.
        constexpr std::uint32_t kUserStackTop = 0x0A000000u;
        constexpr std::uint32_t kKernelContext = kUserStackTop - 0x100u;
        const std::string launch_path = "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN";
        const std::uint32_t launch_size = static_cast<std::uint32_t>(launch_path.size() + 1u);
        const std::uint32_t launch_argp = 0x09FFFC00u;
        runtime.memory().copy_in(
            launch_argp,
            std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(launch_path.c_str()), launch_size));

        auto &cpu = runtime.cpu();
        cpu.set_gpr(26, kKernelContext);
        cpu.set_gpr(28, module->gp);
        cpu.set_gpr(29, kKernelContext);
        cpu.set_gpr(30, kKernelContext);
        cpu.set_gpr(31, 0u);
        cpu.set_gpr(4, launch_size);
        cpu.set_gpr(5, launch_argp);
        cpu.set_gpr(6, 0u);
        cpu.set_gpr(7, 0u);

        const std::uint32_t entry = elf.runtime_entry(psprecomp::kDefaultPspUserLoadBase);
        if (!runtime.has_function(entry))
            log("ERROR: entry " + psprecomp::hex32(entry) + " is not covered by the generated code");
        log("entering guest at " + psprecomp::hex32(entry) + " (max dispatches " + std::to_string(max_dispatches) + ")");

        int exit_code = 0;
        try {
            runtime.run(entry, max_dispatches);
        } catch (const std::exception &error) {
            log(std::string("RUNTIME FAULT: ") + error.what());
            try {
                // Words on the faulting stack that look like code addresses: the call chain that led to the fault.
                auto &cpu = runtime.cpu();
                const std::uint32_t sp = cpu.gpr[29];
                std::string chain;
                for (std::uint32_t off = 0u; off < 0x400u; off += 4u) {
                    const std::uint32_t w = runtime.memory().load32(sp + off);
                    if (w >= 0x08804000u && w < 0x08A00000u && (w & 3u) == 0u) chain += " +" + psprecomp::hex32(off) + ":" + psprecomp::hex32(w);
                }
                log("FAULT STACK sp=" + psprecomp::hex32(sp) + " ra=" + psprecomp::hex32(cpu.gpr[31]) + chain);
                std::string regs;
                for (int r = 2; r < 26; ++r) regs += " r" + std::to_string(r) + "=" + psprecomp::hex32(cpu.gpr[r]);
                log("FAULT REGS" + regs);
            } catch (const std::exception &) {}
            exit_code = 4;
        }
        rr::report_summary();
        if (exit_code == 4 && g_from_launcher) rr::report::announce(rr::report::make_bug_report("error"), true);   // (the game stopped on an error)

        if (runtime.stopped()) log("stopped: " + runtime.stop_reason());
        else log("run ended (dispatch limit reached) without a stop request");
        rr::close_log_file();
        return exit_code;
    } catch (const std::exception &error) {
        std::cerr << "[RR] fatal: " << error.what() << "\n";
        if (g_from_launcher) rr::show_error_box(std::string("Ridge Racer could not start:\n\n") + error.what());
        return 1;
    }
}
