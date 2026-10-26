#include "rr_pad.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <vector>

#include "rr_log.hpp"
#include "rr_settings.hpp"
#include <cctype>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif !defined(__EMSCRIPTEN__)
#include <dlfcn.h>
#endif

namespace rr::pad {
namespace {

// PSP button bits (sceCtrl).
constexpr std::uint32_t kSelect = 0x0001u, kStart = 0x0008u, kUp = 0x0010u, kRight = 0x0020u, kDown = 0x0040u, kLeft = 0x0080u,
                        kL = 0x0100u, kR = 0x0200u, kTriangle = 0x1000u, kCircle = 0x2000u, kCross = 0x4000u, kSquare = 0x8000u;

std::atomic<int> g_prompts{static_cast<int>(Prompts::Auto)};
std::atomic<bool> g_triggers{true};
std::atomic<int> g_glyphs{static_cast<int>(Glyphs::Keyboard)};
std::atomic<unsigned> g_generation{1u};
bool g_seen_input = false;
Family g_last_pad = Family::None;

bool contains_ci(const char *hay, const char *needle) {
    if (hay == nullptr) return false;
    const std::size_t n = std::strlen(needle);
    for (const char *p = hay; *p != '\0'; ++p) {
        std::size_t i = 0;
        while (i < n && p[i] != '\0' && std::tolower(static_cast<unsigned char>(p[i])) == std::tolower(static_cast<unsigned char>(needle[i]))) ++i;
        if (i == n) return true;
    }
    return false;
}

Glyphs glyphs_for(Family f) {
    switch (f) {
    case Family::PlayStation: return Glyphs::PlayStation;
    case Family::Nintendo: return Glyphs::Nintendo;
    default: return Glyphs::Xbox;   // Xbox and every other maker: most third-party pads use the A/B/X/Y names
    }
}

void set_glyphs(Glyphs g) {
    if (g_glyphs.exchange(static_cast<int>(g)) != static_cast<int>(g)) {
        g_generation.fetch_add(1u);
        char b[96];
        std::snprintf(b, sizeof b, "[PAD] button pictures: %s", glyphs_name(g));
        log_line(category::kHle, b);
    }
}

} // namespace

Family family_from_ids(std::uint16_t vendor, std::uint16_t product, const char *name) {
    (void)product;
    if (contains_ci(name, "xbox") || contains_ci(name, "x-box") || contains_ci(name, "xinput")) return Family::Xbox;
    if (contains_ci(name, "dualsense") || contains_ci(name, "dualshock") || contains_ci(name, "playstation") || contains_ci(name, "ps3") ||
        contains_ci(name, "ps4") || contains_ci(name, "ps5")) return Family::PlayStation;
    if (contains_ci(name, "switch") || contains_ci(name, "joy-con") || contains_ci(name, "joycon") || contains_ci(name, "pro controller") ||
        contains_ci(name, "nintendo")) return Family::Nintendo;
    switch (vendor) {
    case 0x045Eu: return Family::Xbox;          // Microsoft
    case 0x054Cu: return Family::PlayStation;   // Sony
    case 0x057Eu: return Family::Nintendo;      // Nintendo
    default: break;
    }
    if (vendor == 0u && name == nullptr) return Family::None;
    return Family::Generic;
}

const char *family_name(Family f) {
    switch (f) {
    case Family::Xbox: return "Xbox";
    case Family::PlayStation: return "PlayStation";
    case Family::Nintendo: return "Nintendo";
    case Family::Generic: return "other";
    default: return "none";
    }
}

const char *glyphs_name(Glyphs g) {
    switch (g) {
    case Glyphs::Xbox: return "Xbox";
    case Glyphs::PlayStation: return "PlayStation";
    case Glyphs::Nintendo: return "Nintendo Switch";
    default: return "keyboard";
    }
}

void set_prompts(Prompts p) { g_prompts = static_cast<int>(p); g_generation.fetch_add(1u); }
Prompts prompts() { return static_cast<Prompts>(g_prompts.load()); }
Prompts parse_prompts(const std::string &s, Prompts fallback) {
    std::string t;
    for (char c : s) t.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (t == "auto" || t == "automatic" || t == "0") return Prompts::Auto;
    if (t == "xbox" || t == "1") return Prompts::Xbox;
    if (t == "playstation" || t == "ps" || t == "2") return Prompts::PlayStation;
    if (t == "nintendo" || t == "switch" || t == "3") return Prompts::Nintendo;
    if (t == "keyboard" || t == "4") return Prompts::Keyboard;
    return fallback;
}
void set_triggers(bool on) { g_triggers = on; }
bool triggers() { return g_triggers.load(); }

void frame(std::uint32_t all_buttons, std::uint32_t pad_buttons, bool pad_stick_moved, Family pad_family) {
    // RR_BUTTON_PROMPTS (test) wins over the setting.
    static const Prompts env = [] { const char *e = std::getenv("RR_BUTTON_PROMPTS"); return e ? parse_prompts(e, Prompts::Auto) : Prompts::Auto; }();
    const Prompts p = env != Prompts::Auto ? env : prompts();
    switch (p) {
    case Prompts::Xbox: set_glyphs(Glyphs::Xbox); return;
    case Prompts::PlayStation: set_glyphs(Glyphs::PlayStation); return;
    case Prompts::Nintendo: set_glyphs(Glyphs::Nintendo); return;
    case Prompts::Keyboard: set_glyphs(Glyphs::Keyboard); return;
    default: break;
    }
    if (pad_family != Family::None) g_last_pad = pad_family;
    const std::uint32_t keys = all_buttons & ~pad_buttons;
    if (pad_buttons != 0u || pad_stick_moved) { g_seen_input = true; set_glyphs(glyphs_for(pad_family != Family::None ? pad_family : g_last_pad)); }
    else if (keys != 0u) { g_seen_input = true; set_glyphs(Glyphs::Keyboard); }
    else if (!g_seen_input) set_glyphs(pad_family != Family::None ? glyphs_for(pad_family) : Glyphs::Keyboard);   // nothing pressed yet
}

Glyphs glyphs() { return static_cast<Glyphs>(g_glyphs.load()); }
unsigned generation() { return g_generation.load(); }

// =====================================================================================================================
// SDL2 controllers.  The few functions used are looked up by name, so nothing here needs SDL's headers or import library.
// =====================================================================================================================
#if defined(__EMSCRIPTEN__)
bool sdl_available() { return false; }
std::uint32_t sdl_buttons() { return 0u; }
bool sdl_stick(std::uint8_t &, std::uint8_t &) { return false; }
Family sdl_family() { return Family::None; }
std::string sdl_describe() { return "browser"; }
#else
namespace {

struct GC;   // SDL_GameController
struct RW;   // SDL_RWops
using Fn_Init = int (*)(std::uint32_t);
using Fn_SetHint = int (*)(const char *, const char *);
using Fn_NumJoysticks = int (*)();
using Fn_IsGameController = int (*)(int);
using Fn_DeviceInstance = std::int32_t (*)(int);
using Fn_Open = GC *(*)(int);
using Fn_Close = void (*)(GC *);
using Fn_Attached = int (*)(GC *);
using Fn_Button = std::uint8_t (*)(GC *, int);
using Fn_Axis = std::int16_t (*)(GC *, int);
using Fn_Type = int (*)(GC *);
using Fn_Name = const char *(*)(GC *);
using Fn_U16 = std::uint16_t (*)(GC *);
using Fn_Update = void (*)();
using Fn_Flush = void (*)(std::uint32_t, std::uint32_t);
using Fn_RWFromFile = RW *(*)(const char *, const char *);
using Fn_AddMappingsRW = int (*)(RW *, int);
using Fn_GetError = const char *(*)();
using Fn_Version = void (*)(std::uint8_t *);

struct Sdl {
    bool tried = false, ok = false, own_events = false;
    Fn_Init init_sub{};
    Fn_SetHint set_hint{};
    Fn_NumJoysticks num{};
    Fn_IsGameController is_gc{};
    Fn_DeviceInstance dev_instance{};
    Fn_Open open{};
    Fn_Close close{};
    Fn_Attached attached{};
    Fn_Button button{};
    Fn_Axis axis{};
    Fn_Type type{};            // SDL 2.0.12+
    Fn_Name name{};
    Fn_U16 vendor{}, product{};
    Fn_Update update{};
    Fn_Flush flush{};
    Fn_RWFromFile rw_from_file{};
    Fn_AddMappingsRW add_mappings{};
    Fn_GetError get_error{};
};
Sdl g_sdl;

struct Pad {
    GC *gc{};
    std::int32_t id{-1};
    Family family{Family::None};
    std::string name;
    std::uint32_t last_buttons{};
    bool stick_active{};
};
std::vector<Pad> g_pads;
std::chrono::steady_clock::time_point g_scan{};
std::chrono::steady_clock::time_point g_polled{};
std::uint32_t g_buttons = 0u;
std::uint8_t g_lx = 128, g_ly = 128;
bool g_stick = false;
Family g_family = Family::None;
std::recursive_mutex g_mu;

template <class F> bool sym(void *lib, const char *n, F &out) {
#if defined(_WIN32)
    out = reinterpret_cast<F>(reinterpret_cast<void *>(GetProcAddress(static_cast<HMODULE>(lib), n)));
#else
    out = reinterpret_cast<F>(dlsym(lib, n));
#endif
    return out != nullptr;
}

// SDL_GameControllerType -> family (0 unknown, 1 360, 2 One, 3 PS3, 4 PS4, 5 Switch Pro, 6 virtual, 7 PS5, 8 Luna,
// 9 Stadia, 10 Shield, 11/12/13 Joy-Con left/right/pair).
Family family_from_type(int t) {
    switch (t) {
    case 1: case 2: return Family::Xbox;
    case 3: case 4: case 7: return Family::PlayStation;
    case 5: case 11: case 12: case 13: return Family::Nintendo;
    default: return Family::None;
    }
}

void load_sdl() {
    Sdl &s = g_sdl;
    if (s.tried) return;
    s.tried = true;
    if (std::getenv("RR_NO_SDL_PAD") != nullptr) { log_line(category::kHle, "[PAD] SDL controllers off (RR_NO_SDL_PAD)"); return; }
    void *lib = nullptr;
#if defined(_WIN32)
    // Only the copy next to the game: never pick up some other program's SDL2.dll from the search path.
    const std::wstring dll = (executable_dir() / "SDL2.dll").wstring();
    lib = reinterpret_cast<void *>(LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH));
    if (lib == nullptr) { log_line(category::kHle, "[PAD] SDL2.dll not found next to the game: Xbox-style (XInput) controllers only"); return; }
    s.own_events = true;
#else
    lib = dlopen(nullptr, RTLD_NOW);   // the game is linked with SDL2
    if (lib == nullptr || dlsym(lib, "SDL_GameControllerOpen") == nullptr) lib = dlopen("libSDL2-2.0.so.0", RTLD_NOW);
    if (lib == nullptr) { log_line(category::kHle, "[PAD] SDL2 not available: no controller support"); return; }
#endif
    bool all = sym(lib, "SDL_InitSubSystem", s.init_sub) && sym(lib, "SDL_SetHint", s.set_hint) && sym(lib, "SDL_NumJoysticks", s.num) &&
               sym(lib, "SDL_IsGameController", s.is_gc) && sym(lib, "SDL_JoystickGetDeviceInstanceID", s.dev_instance) &&
               sym(lib, "SDL_GameControllerOpen", s.open) && sym(lib, "SDL_GameControllerClose", s.close) &&
               sym(lib, "SDL_GameControllerGetAttached", s.attached) && sym(lib, "SDL_GameControllerGetButton", s.button) &&
               sym(lib, "SDL_GameControllerGetAxis", s.axis) && sym(lib, "SDL_GameControllerName", s.name) &&
               sym(lib, "SDL_GameControllerUpdate", s.update) && sym(lib, "SDL_FlushEvents", s.flush) && sym(lib, "SDL_GetError", s.get_error);
    sym(lib, "SDL_GameControllerGetType", s.type);
    sym(lib, "SDL_GameControllerGetVendor", s.vendor);
    sym(lib, "SDL_GameControllerGetProduct", s.product);
    sym(lib, "SDL_RWFromFile", s.rw_from_file);
    sym(lib, "SDL_GameControllerAddMappingsFromRW", s.add_mappings);
    if (!all) { log_line(category::kHle, "[PAD] SDL2 is too old (functions missing): controllers through it are off"); return; }
    // Buttons by position (south = CROSS) on every controller, Nintendo ones included, so the layout matches the PSP's.
    s.set_hint("SDL_GAMECONTROLLER_USE_BUTTON_LABELS", "0");
    s.set_hint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
#if defined(_WIN32)
    s.set_hint("SDL_JOYSTICK_THREAD", "1");   // no SDL window: device messages are handled on SDL's own thread
#endif
    s.set_hint("SDL_JOYSTICK_HIDAPI_PS4_RUMBLE", "0");
    s.set_hint("SDL_JOYSTICK_HIDAPI_PS5_PLAYER_LED", "1");
    if (s.init_sub(0x00002000u /* SDL_INIT_GAMECONTROLLER */) != 0) {
        log_line(category::kHle, std::string("[PAD] SDL controller start failed: ") + (s.get_error ? s.get_error() : "?"));
        return;
    }
    s.ok = true;
    // Extra mappings for unusual pads: gamecontrollerdb.txt (the community list) next to the game, if present.
    if (s.rw_from_file != nullptr && s.add_mappings != nullptr) {
        const std::filesystem::path db = executable_dir() / "gamecontrollerdb.txt";
        std::error_code ec;
        if (std::filesystem::exists(db, ec)) {
            if (RW *rw = s.rw_from_file(db.string().c_str(), "rb")) {
                const int n = s.add_mappings(rw, 1);
                log_line(category::kHle, "[PAD] gamecontrollerdb.txt: " + std::to_string(n) + " mappings added");
            }
        }
    }
    log_line(category::kHle, "[PAD] SDL controllers on");
}

void scan(bool force) {
    Sdl &s = g_sdl;
    const auto now = std::chrono::steady_clock::now();
    // closed or unplugged
    for (auto it = g_pads.begin(); it != g_pads.end();) {
        if (!s.attached(it->gc)) {
            log_line(category::kHle, "[PAD] controller removed: " + it->name);
            s.close(it->gc);
            it = g_pads.erase(it);
        } else ++it;
    }
    if (!force && now - g_scan < std::chrono::milliseconds(700)) return;
    g_scan = now;
    const int n = s.num();
    for (int i = 0; i < n; ++i) {
        if (!s.is_gc(i)) continue;
        const std::int32_t id = s.dev_instance(i);
        if (std::any_of(g_pads.begin(), g_pads.end(), [&](const Pad &p) { return p.id == id; })) continue;
        GC *gc = s.open(i);
        if (gc == nullptr) continue;
        Pad p;
        p.gc = gc; p.id = id;
        const char *nm = s.name(gc);
        p.name = nm != nullptr ? nm : "controller";
        const std::uint16_t vid = s.vendor ? s.vendor(gc) : 0u, pid = s.product ? s.product(gc) : 0u;
        p.family = s.type != nullptr ? family_from_type(s.type(gc)) : Family::None;
        if (p.family == Family::None) p.family = family_from_ids(vid, pid, nm);
        char b[256];
        std::snprintf(b, sizeof b, "[PAD] controller connected: %s (%04X:%04X, %s)", p.name.c_str(), vid, pid, family_name(p.family));
        log_line(category::kHle, b);
        if (g_family == Family::None) g_family = p.family;
        g_pads.push_back(std::move(p));
    }
}

std::uint32_t read_pad(const Pad &p) {
    Sdl &s = g_sdl;
    const auto b = [&](int x) { return s.button(p.gc, x) != 0; };
    std::uint32_t o = 0;
    // SDL_GameControllerButton: A 0, B 1, X 2, Y 3, BACK 4, GUIDE 5, START 6, LSTICK 7, RSTICK 8, LSHOULDER 9, RSHOULDER 10,
    // DPAD up 11 down 12 left 13 right 14, MISC1 15, PADDLE1-4 16-19, TOUCHPAD 20.
    if (b(0)) o |= kCross;
    if (b(1)) o |= kCircle;
    if (b(2)) o |= kSquare;
    if (b(3)) o |= kTriangle;
    if (b(4) || b(20)) o |= kSelect;   // View / Share-Create / Minus, and the PlayStation touchpad click
    if (b(6)) o |= kStart;
    if (b(9)) o |= kL;
    if (b(10)) o |= kR;
    if (b(11)) o |= kUp;
    if (b(12)) o |= kDown;
    if (b(13)) o |= kLeft;
    if (b(14)) o |= kRight;
    if (triggers()) {
        // Analog triggers past halfway: right = CROSS (accelerate in the default layout), left = SQUARE (brake).
        if (s.axis(p.gc, 5) > 16000) o |= kCross;
        if (s.axis(p.gc, 4) > 16000) o |= kSquare;
    }
    return o;
}

void poll() {
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    load_sdl();
    if (!g_sdl.ok) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - g_polled < std::chrono::milliseconds(2)) return;
    g_polled = now;
    g_sdl.update();
    if (g_sdl.own_events) g_sdl.flush(0u, 0xFFFFFFFFu);   // nothing reads SDL's event queue on Windows: keep it empty
    scan(false);
    std::uint32_t all = 0u;
    bool stick = false;
    std::uint8_t lx = 128, ly = 128;
    for (auto &p : g_pads) {
        const std::uint32_t bt = read_pad(p);
        if (bt != 0u && bt != p.last_buttons) g_family = p.family;   // the controller pressed last decides the pictures
        p.last_buttons = bt;
        all |= bt;
        const float x = static_cast<float>(g_sdl.axis(p.gc, 0)) / 32767.0f, y = static_cast<float>(g_sdl.axis(p.gc, 1)) / 32767.0f;
        const float mag = std::sqrt(x * x + y * y);
        const bool active = mag >= 0.2f;
        if (active && !stick) {
            const float k = std::min(1.0f, (mag - 0.2f) / 0.8f) / mag;
            const auto conv = [](float v) { return static_cast<std::uint8_t>(std::clamp(128.0f + v * 127.5f, 0.0f, 255.0f)); };
            lx = conv(x * k); ly = conv(y * k);   // SDL: down is positive, like the PSP
            stick = true;
            if (!p.stick_active) g_family = p.family;
        }
        p.stick_active = active;
    }
    if (g_pads.empty()) g_family = Family::None;
    g_buttons = all; g_stick = stick; g_lx = lx; g_ly = ly;
}

} // namespace

bool sdl_available() { std::lock_guard<std::recursive_mutex> lock(g_mu); load_sdl(); return g_sdl.ok; }
std::uint32_t sdl_buttons() { poll(); std::lock_guard<std::recursive_mutex> lock(g_mu); return g_buttons; }
bool sdl_stick(std::uint8_t &lx, std::uint8_t &ly) {
    poll();
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    if (!g_stick) return false;
    lx = g_lx; ly = g_ly;
    return true;
}
Family sdl_family() { poll(); std::lock_guard<std::recursive_mutex> lock(g_mu); return g_family; }
std::string sdl_describe() {
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    if (!g_sdl.ok) return "SDL controllers off";
    std::string t = std::to_string(g_pads.size()) + (g_pads.size() == 1 ? " controller" : " controllers");
    for (std::size_t i = 0; i < g_pads.size(); ++i) t += (i == 0 ? ": " : ", ") + g_pads[i].name + " (" + family_name(g_pads[i].family) + ")";
    return t;
}
#endif

} // namespace rr::pad
