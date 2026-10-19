// Window, keyboard / gamepad input, OpenGL context and frame pacing on SDL2 (Linux; the same file should also serve macOS
// and other SDL platforms later).  Same behaviour as rr_present_win32.cpp: key bindings, 59.94 Hz pacing, adaptive vsync.
#include "rr_present.hpp"
#include "rr_audio.hpp"
#include "rr_env.hpp"
#include "rr_log.hpp"
#include "rr_tuning.hpp"

#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace rr {

bool osk_active();   // rr_utility.cpp: the on-screen keyboard is open (Esc means Cancel there, not Quit)

namespace {

SDL_Window *g_window = nullptr;
SDL_GLContext g_gl = nullptr;
SDL_Renderer *g_renderer = nullptr;   // only when the software-raster frame path is used (no GL renderer)
SDL_Texture *g_frame_tex = nullptr;
int g_tex_w = 0, g_tex_h = 0;
bool g_quit = false;
bool g_throttle = true;
bool g_fullscreen = false;
int g_frame_period_us = 16683;
int g_scale = 3;
std::chrono::steady_clock::time_point g_next_frame{};
bool g_have_deadline = false;
bool g_low_latency = false, g_latency_log = false;
std::chrono::steady_clock::time_point g_wake_t{}, g_pad_read_t{};
bool g_pad_read_since_swap = false;
double g_lat_w2p_sum = 0, g_lat_w2p_max = 0, g_lat_p2s_sum = 0, g_lat_p2s_max = 0, g_lat_gpu_sum = 0, g_lat_gpu_max = 0;
int g_lat_frames = 0, g_lat_probes = 0, g_lat_counter = 0;
double g_swap_ms = 0.0, g_pace_ms = 0.0;
void (*g_gl_finish)() = nullptr;
int g_swap_interval = 0;
int g_vsync_check = 0;
std::vector<char16_t> g_typed;
SDL_GameController *g_pad = nullptr;
std::chrono::steady_clock::time_point g_pad_scan{};

void set_fullscreen(bool on) {
    if (g_window == nullptr) return;
    g_fullscreen = on;
    SDL_SetWindowFullscreen(g_window, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    SDL_ShowCursor(on ? SDL_DISABLE : SDL_ENABLE);
}

void push_utf8(const char *s) {
    // UTF-8 -> UTF-16 for the on-screen keyboard
    const auto *p = reinterpret_cast<const unsigned char *>(s);
    while (*p != 0) {
        std::uint32_t cp = 0;
        int extra = 0;
        if (*p < 0x80) { cp = *p; extra = 0; }
        else if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; extra = 1; }
        else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; extra = 2; }
        else if ((*p & 0xF8) == 0xF0) { cp = *p & 0x07; extra = 3; }
        else { ++p; continue; }
        ++p;
        for (int i = 0; i < extra && (*p & 0xC0) == 0x80; ++i, ++p) cp = (cp << 6) | (*p & 0x3F);
        if (g_typed.size() >= 256u) return;
        if (cp >= 0x10000u) {
            cp -= 0x10000u;
            g_typed.push_back(static_cast<char16_t>(0xD800u + (cp >> 10)));
            g_typed.push_back(static_cast<char16_t>(0xDC00u + (cp & 0x3FFu)));
        } else {
            g_typed.push_back(static_cast<char16_t>(cp));
        }
    }
}

bool key(SDL_Scancode sc) {
    const Uint8 *k = SDL_GetKeyboardState(nullptr);
    return k != nullptr && k[sc] != 0;
}
bool focused() { return g_window != nullptr && SDL_GetKeyboardFocus() == g_window; }

void handle_key_down(const SDL_KeyboardEvent &e) {
    const bool rep = e.repeat != 0;
    const bool alt = (e.keysym.mod & KMOD_ALT) != 0;
    switch (e.keysym.sym) {
    case SDLK_F11: if (!rep) set_fullscreen(!g_fullscreen); break;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        if (alt && !rep) { set_fullscreen(!g_fullscreen); return; }
        if (g_typed.size() < 256u) g_typed.push_back(13);
        break;
    case SDLK_BACKSPACE: if (g_typed.size() < 256u) g_typed.push_back(8); break;
    case SDLK_ESCAPE:
        if (g_typed.size() < 256u) g_typed.push_back(27);
        if (!rep) {
            static const bool esc_quit_off = std::getenv("RR_ESC_QUIT_OFF") != nullptr;
            if (!esc_quit_off && !rr::osk_active()) g_quit = true;
        }
        break;
    case SDLK_F1: if (!rep) rr::g_aa_mode = (rr::g_aa_mode.load() + 1) % 4; break;
    case SDLK_END: if (!rep) { const int a = rr::g_aniso.load(); rr::g_aniso = a >= 16 ? 1 : a * 2; } break;
    case SDLK_PAGEUP: rr::g_taa_blend = std::clamp(rr::g_taa_blend.load() + 2, 4, 100); break;
    case SDLK_PAGEDOWN: rr::g_taa_blend = std::clamp(rr::g_taa_blend.load() - 2, 4, 100); break;
    case SDLK_F9: rr::audio_volume_step(-1); break;
    case SDLK_F10: rr::audio_volume_step(1); break;
    case SDLK_F8: if (!rep) rr::audio_mute_toggle(); break;
    default: break;
    }
    if (!rr::g_rr_dev || rep) return;   // developer keys only with --dev
    switch (e.keysym.sym) {
    case SDLK_F2:
    case SDLK_F3: {
        const int n = rr::g_gate_count.load();
        int s = rr::g_gate_sel.load();
        if (s < 0) s = 0;
        const auto unusable = [n](int v) { return v == 5 || v == 6 || (v >= 15 && v <= n); };
        const int dir = e.keysym.sym == SDLK_F2 ? 1 : -1;
        do { s += dir; if (s > n + 1) s = 0; if (s < 0) s = n + 1; } while (unusable(s));
        rr::g_gate_sel = s == 0 ? -1 : s;
        break;
    }
    case SDLK_F7: {
        const int s = rr::g_gate_sel.load();
        if (s <= 0 || s > rr::g_gate_count.load()) rr::g_gate_kept = 0ull;
        else rr::g_gate_kept.fetch_xor(1ull << (s - 1));
        break;
    }
    case SDLK_INSERT: rr::g_audio_mark.fetch_add(1); break;
    case SDLK_DELETE: rr::g_audio_silence.fetch_add(1); break;
    case SDLK_F4: rr::g_hide_class = (rr::g_hide_class.load() + 1) % 8; break;
    case SDLK_F5:
    case SDLK_F6: rr::g_alpha_cutoff = std::clamp(rr::g_alpha_cutoff.load() + (e.keysym.sym == SDLK_F6 ? 4 : -4), 0, 160); break;
    default: break;
    }
}

// Hot-key edge detector for the polled "requested" functions.
bool edge(SDL_Scancode sc, bool &was_down) {
    const bool down = key(sc);
    const bool fire = down && !was_down;
    was_down = down;
    return fire;
}

void pad_scan() {
    const auto now = std::chrono::steady_clock::now();
    if (g_pad != nullptr) {
        if (!SDL_GameControllerGetAttached(g_pad)) { SDL_GameControllerClose(g_pad); g_pad = nullptr; }
        return;
    }
    if (now - g_pad_scan < std::chrono::seconds(1)) return;
    g_pad_scan = now;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) { g_pad = SDL_GameControllerOpen(i); if (g_pad != nullptr) break; }
    }
}

std::uint32_t pad_buttons() {
    pad_scan();
    if (g_pad == nullptr) return 0u;
    const auto b = [](SDL_GameControllerButton x) { return SDL_GameControllerGetButton(g_pad, x) != 0; };
    std::uint32_t o = 0;
    if (b(SDL_CONTROLLER_BUTTON_START)) o |= 0x0008u;
    if (b(SDL_CONTROLLER_BUTTON_BACK)) o |= 0x0001u;
    if (b(SDL_CONTROLLER_BUTTON_DPAD_UP)) o |= 0x0010u;
    if (b(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) o |= 0x0020u;
    if (b(SDL_CONTROLLER_BUTTON_DPAD_DOWN)) o |= 0x0040u;
    if (b(SDL_CONTROLLER_BUTTON_DPAD_LEFT)) o |= 0x0080u;
    if (b(SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) o |= 0x0100u;    // L
    if (b(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) o |= 0x0200u;   // R
    if (b(SDL_CONTROLLER_BUTTON_Y)) o |= 0x1000u;               // TRIANGLE
    if (b(SDL_CONTROLLER_BUTTON_B)) o |= 0x2000u;               // CIRCLE
    if (b(SDL_CONTROLLER_BUTTON_A)) o |= 0x4000u;               // CROSS
    if (b(SDL_CONTROLLER_BUTTON_X)) o |= 0x8000u;               // SQUARE
    return o;
}

bool pad_stick(std::uint8_t &lx, std::uint8_t &ly) {
    pad_scan();
    if (g_pad == nullptr) return false;
    const float x = static_cast<float>(SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX)) / 32767.0f;
    const float y = static_cast<float>(SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTY)) / 32767.0f;   // SDL: down is positive
    const float mag = std::sqrt(x * x + y * y);
    if (mag < 0.2f) return false;
    const float k = std::min(1.0f, (mag - 0.2f) / 0.8f) / mag;
    const auto conv = [](float v) { return static_cast<std::uint8_t>(std::clamp(128.0f + v * 127.5f, 0.0f, 255.0f)); };
    lx = conv(x * k);
    ly = conv(y * k);
    return true;
}

void update_vsync(bool force) {
    if (g_gl == nullptr) return;
    if (!force && ++g_vsync_check < 60) return;
    g_vsync_check = 0;
    int hz = 0;
    SDL_DisplayMode dm{};
    if (SDL_GetWindowDisplayMode(g_window, &dm) == 0) hz = dm.refresh_rate;
    const double fps = 1.0e6 / static_cast<double>(g_frame_period_us);
    int interval = 0;
    std::string why;
    const char *env = std::getenv("RR_VSYNC");
    if (env != nullptr && env[0] == '0') why = "RR_VSYNC=0";
    else if (env != nullptr && env[0] != '\0') { interval = std::max(1, std::atoi(env)); why = "RR_VSYNC"; }
    else if (!g_throttle) why = "unthrottled";
    else if (hz > 0) {
        const double n = static_cast<double>(hz) / fps, rn = std::round(n);
        if (rn >= 1.0 && std::fabs(n - rn) <= 0.03 * rn) interval = -static_cast<int>(rn);   // adaptive when the driver has it
        why = interval ? "refresh matches the frame rate" : "refresh does not match the frame rate";
    } else why = "refresh rate unknown";
    if (interval == g_swap_interval && !force) return;
    if (SDL_GL_SetSwapInterval(interval) != 0 && interval < 0) { interval = -interval; SDL_GL_SetSwapInterval(interval); }
    g_swap_interval = interval;
    log_line("PRESENT", "VSYNC interval=" + std::to_string(interval) + " monitor=" + std::to_string(hz) + " Hz fps=" +
                            std::to_string(static_cast<int>(fps + 0.5)) + " (" + why + ")");
}

void pace_frame() {
    const auto pace_t0 = std::chrono::steady_clock::now();
    struct PaceTimer { std::chrono::steady_clock::time_point t0; ~PaceTimer() { g_pace_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); } } pace_timer{pace_t0};
    if (g_throttle && g_swap_interval != 0) {
        g_have_deadline = false;   // the swap waits for the vblank
    } else if (g_throttle) {
        using clock = std::chrono::steady_clock;
        const auto period = std::chrono::microseconds(g_frame_period_us);
        const auto now = clock::now();
        if (!g_have_deadline || now > g_next_frame + period) g_next_frame = now;
        else g_next_frame += period;
        g_have_deadline = true;
        if (g_next_frame > now) {
            const auto wait = g_next_frame - now;
            if (wait > std::chrono::milliseconds(3)) std::this_thread::sleep_for(wait - std::chrono::milliseconds(2));
            while (clock::now() < g_next_frame) std::this_thread::yield();
        }
    }
    g_wake_t = std::chrono::steady_clock::now();
}

} // namespace

bool present_open(const PresentOptions &options) {
    if (!options.window) return false;
    g_throttle = options.throttle;
    g_frame_period_us = std::max(1000, options.frame_period_us);
    g_low_latency = options.low_latency; g_latency_log = options.latency_log;
    g_scale = options.scale < 1 ? 1 : options.scale;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        log_line("PRESENT", std::string("SDL_Init failed: ") + SDL_GetError());
        return false;
    }
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    g_window = SDL_CreateWindow("Ridge Racer (native recompilation)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                480 * g_scale, 272 * g_scale, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (g_window == nullptr) {
        log_line("PRESENT", std::string("SDL_CreateWindow failed: ") + SDL_GetError());
        return false;
    }
    SDL_StartTextInput();
    if (options.fullscreen) set_fullscreen(true);
    return true;
}

void present_pump() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT: g_quit = true; break;
        case SDL_WINDOWEVENT: if (e.window.event == SDL_WINDOWEVENT_CLOSE) g_quit = true; break;
        case SDL_KEYDOWN: handle_key_down(e.key); break;
        case SDL_TEXTINPUT: push_utf8(e.text.text); break;
        default: break;
        }
    }
}

GlProc present_gl_open() {
    if (g_window == nullptr) return nullptr;
    g_gl = SDL_GL_CreateContext(g_window);
    if (g_gl == nullptr) {
        log_line("PRESENT", std::string("OpenGL 3.3 core context failed: ") + SDL_GetError());
        return nullptr;
    }
    SDL_GL_MakeCurrent(g_window, g_gl);
    SDL_GL_SetSwapInterval(0);
    update_vsync(true);
    g_gl_finish = reinterpret_cast<void (*)()>(SDL_GL_GetProcAddress("glFinish"));
    return [](const char *name) -> void * { return SDL_GL_GetProcAddress(name); };
}

void present_gl_size(int &width, int &height) {
    int w = 1, h = 1;
    if (g_window != nullptr) SDL_GL_GetDrawableSize(g_window, &w, &h);
    width = w > 0 ? w : 1;
    height = h > 0 ? h : 1;
}

void present_note_pad_read() { g_pad_read_t = std::chrono::steady_clock::now(); g_pad_read_since_swap = true; }

void present_take_latency(PresentLatency &o) {
    o = PresentLatency{};
    if (g_lat_frames > 0) {
        o.wake_to_pad_avg = g_lat_w2p_sum / g_lat_frames; o.wake_to_pad_max = g_lat_w2p_max;
        o.pad_to_swap_avg = g_lat_p2s_sum / g_lat_frames; o.pad_to_swap_max = g_lat_p2s_max;
    }
    if (g_lat_probes > 0) { o.gpu_backlog_avg = g_lat_gpu_sum / g_lat_probes; o.gpu_backlog_max = g_lat_gpu_max; }
    o.frames = g_lat_frames; o.probes = g_lat_probes;
    g_lat_w2p_sum = g_lat_w2p_max = g_lat_p2s_sum = g_lat_p2s_max = g_lat_gpu_sum = g_lat_gpu_max = 0;
    g_lat_frames = g_lat_probes = 0;
}

void present_take_timing(double &swap_ms, double &pace_ms) { swap_ms = g_swap_ms; pace_ms = g_pace_ms; g_swap_ms = g_pace_ms = 0.0; }

void present_gl_swap() {
    present_pump();
    if (g_gl != nullptr) {
        update_vsync(false);
        const auto t0 = std::chrono::steady_clock::now();
        SDL_GL_SwapWindow(g_window);
        const auto t1 = std::chrono::steady_clock::now();
        g_swap_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
        if (g_latency_log && g_pad_read_since_swap) {
            const double p2s = std::chrono::duration<double, std::milli>(t1 - g_pad_read_t).count();
            const double w2p = std::chrono::duration<double, std::milli>(g_pad_read_t - g_wake_t).count();
            g_lat_p2s_sum += p2s; g_lat_p2s_max = std::max(g_lat_p2s_max, p2s);
            g_lat_w2p_sum += w2p; g_lat_w2p_max = std::max(g_lat_w2p_max, w2p);
            ++g_lat_frames;
        }
        g_pad_read_since_swap = false;
        const bool probe = g_latency_log && !g_low_latency && (++g_lat_counter % 20) == 0;
        if ((g_low_latency || probe) && g_gl_finish != nullptr) {
            g_gl_finish();
            if (probe) {
                const double gpu = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
                g_lat_gpu_sum += gpu; g_lat_gpu_max = std::max(g_lat_gpu_max, gpu); ++g_lat_probes;
            }
        }
    }
    pace_frame();
}

void present_set_frame_period_us(int period_us) {
    const int p = std::max(1000, period_us);
    if (p != g_frame_period_us) { g_frame_period_us = p; update_vsync(true); }
}

bool present_difftrace_requested() { static bool w = false; return edge(SDL_SCANCODE_HOME, w); }
bool present_callprofile_requested() { static bool w = false; return edge(SDL_SCANCODE_SCROLLLOCK, w); }
bool present_cartrace_requested() { static bool w = false; return edge(SDL_SCANCODE_DELETE, w); }
bool present_ramdump_requested() { static bool w = false; return edge(SDL_SCANCODE_INSERT, w); }
bool present_screenshot_requested() { static bool w = false; return edge(SDL_SCANCODE_F12, w); }

void present_set_title(const char *title) { if (g_window != nullptr) SDL_SetWindowTitle(g_window, title); }

// Software-raster path: shows a 480x272 RGBA frame (only used when the GL renderer is not running).
void present_frame(const std::uint8_t *rgba, int width, int height) {
    if (g_window != nullptr) {
        present_pump();
        if (g_gl == nullptr && g_renderer == nullptr) g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
        if (g_renderer != nullptr) {
            if (g_frame_tex == nullptr || g_tex_w != width || g_tex_h != height) {
                if (g_frame_tex != nullptr) SDL_DestroyTexture(g_frame_tex);
                SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
                g_frame_tex = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, width, height);
                g_tex_w = width; g_tex_h = height;
            }
            if (g_frame_tex != nullptr) {
                SDL_UpdateTexture(g_frame_tex, nullptr, rgba, width * 4);
                SDL_RenderSetLogicalSize(g_renderer, width, height);   // letterboxed
                SDL_RenderClear(g_renderer);
                SDL_RenderCopy(g_renderer, g_frame_tex, nullptr, nullptr);
                SDL_RenderPresent(g_renderer);
            }
        }
    }
    pace_frame();
}

bool present_quit_requested() { return g_quit; }

std::uint32_t present_pad_buttons() {
    if (!focused()) return 0u;
    std::uint32_t buttons = 0;
    if (key(SDL_SCANCODE_RETURN)) buttons |= 0x0008u;   // START
    if (key(SDL_SCANCODE_RSHIFT) || key(SDL_SCANCODE_BACKSPACE)) buttons |= 0x0001u;   // SELECT
    if (key(SDL_SCANCODE_UP)) buttons |= 0x0010u;
    if (key(SDL_SCANCODE_RIGHT)) buttons |= 0x0020u;
    if (key(SDL_SCANCODE_DOWN)) buttons |= 0x0040u;
    if (key(SDL_SCANCODE_LEFT)) buttons |= 0x0080u;
    if (key(SDL_SCANCODE_Q)) buttons |= 0x0100u;   // L
    if (key(SDL_SCANCODE_E)) buttons |= 0x0200u;   // R
    if (key(SDL_SCANCODE_S)) buttons |= 0x1000u;   // TRIANGLE
    if (key(SDL_SCANCODE_X)) buttons |= 0x2000u;   // CIRCLE
    if (key(SDL_SCANCODE_Z)) buttons |= 0x4000u;   // CROSS
    if (key(SDL_SCANCODE_A)) buttons |= 0x8000u;   // SQUARE
    return buttons | pad_buttons();
}

void present_pad_analog(std::uint8_t &lx, std::uint8_t &ly) {
    lx = 128; ly = 128;
    if (!focused()) return;
    if (key(SDL_SCANCODE_J)) lx = 0;
    if (key(SDL_SCANCODE_L)) lx = 255;
    if (key(SDL_SCANCODE_I)) ly = 0;
    if (key(SDL_SCANCODE_K)) ly = 255;
    pad_stick(lx, ly);   // a stick outside its dead zone wins over the keyboard
}

void present_take_typed(std::vector<char16_t> &out) { out.clear(); out.swap(g_typed); }

std::uint32_t present_gamepad_buttons() { return pad_buttons(); }

void present_close() {
    if (g_frame_tex != nullptr) { SDL_DestroyTexture(g_frame_tex); g_frame_tex = nullptr; }
    if (g_renderer != nullptr) { SDL_DestroyRenderer(g_renderer); g_renderer = nullptr; }
    if (g_gl != nullptr) { SDL_GL_DeleteContext(g_gl); g_gl = nullptr; }
    if (g_pad != nullptr) { SDL_GameControllerClose(g_pad); g_pad = nullptr; }
    if (g_window != nullptr) { SDL_DestroyWindow(g_window); g_window = nullptr; }
    SDL_Quit();
}

} // namespace rr
