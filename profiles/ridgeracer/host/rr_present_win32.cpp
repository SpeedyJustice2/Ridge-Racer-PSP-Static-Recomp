#include "rr_present.hpp"
#include "rr_pad.hpp"
#include "rr_audio.hpp"
#include "rr_tuning.hpp"
#include "rr_log.hpp"
#include "rr_env.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <xinput.h>

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

HWND g_window = nullptr;
bool g_quit = false;
bool g_throttle = true;
int g_frame_period_us = 16683;
int g_scale = 3;
std::vector<std::uint8_t> g_bgra;
std::chrono::steady_clock::time_point g_next_frame{};
bool g_have_deadline = false;
bool g_low_latency = false, g_latency_log = false;
std::chrono::steady_clock::time_point g_wake_t{}, g_pad_read_t{};
bool g_pad_read_since_swap = false;
double g_lat_w2p_sum = 0, g_lat_w2p_max = 0, g_lat_p2s_sum = 0, g_lat_p2s_max = 0, g_lat_gpu_sum = 0, g_lat_gpu_max = 0;
int g_lat_frames = 0, g_lat_probes = 0, g_lat_counter = 0;
void (*g_gl_finish)() = nullptr;
// Vertical sync (anti-tearing).  RR_VSYNC=0 off, =1 on, unset = auto: on when the monitor's refresh rate is the --fps rate
// (or a whole multiple of it), so frames are shown on vblanks without slowing the game; off otherwise (pacing alone, may tear).
bool g_cpu_pacing = false;         // frame skip: pace on the CPU clock, no vsync wait
int g_swap_interval = 0;            // what the swap chain / GL context currently uses
HMONITOR g_vsync_monitor = nullptr; // monitor the interval was chosen for
int g_vsync_check = 0;
BOOL(WINAPI *g_wgl_swap_interval)(int) = nullptr;
bool g_swap_tear = false;   // WGL_EXT_swap_control_tear (adaptive vsync)
void update_vsync(bool force);

std::vector<char16_t> g_typed;
int g_mouse_px = 0, g_mouse_py = 0, g_mouse_clicks = 0, g_mouse_wheel_acc = 0;
bool g_mouse_seen = false;

bool g_fullscreen = false;
WINDOWPLACEMENT g_windowed_placement{sizeof(WINDOWPLACEMENT)};
LONG_PTR g_windowed_style = 0;

// Borderless full screen on the monitor the window is on, and back to the previous size and position.
void set_fullscreen(HWND hwnd, bool on) {
    if (hwnd == nullptr || on == g_fullscreen) return;
    if (on) {
        MONITORINFO monitor{sizeof(MONITORINFO)};
        if (!GetWindowPlacement(hwnd, &g_windowed_placement) ||
            !GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &monitor))
            return;
        g_windowed_style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        SetWindowLongPtrW(hwnd, GWL_STYLE, g_windowed_style & ~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW));
        SetWindowPos(hwnd, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top, monitor.rcMonitor.right - monitor.rcMonitor.left,
                     monitor.rcMonitor.bottom - monitor.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fullscreen = true;
    } else {
        SetWindowLongPtrW(hwnd, GWL_STYLE, g_windowed_style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd, &g_windowed_placement);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fullscreen = false;
    }
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CLOSE:
        g_quit = true;
        return 0;
    case WM_DESTROY:
        g_window = nullptr;
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_KEYDOWN:
        // F11 toggles full screen (Alt+Enter below does too).
        if (wparam == VK_F11 && (lparam & 0x40000000) == 0) set_fullscreen(hwnd, !g_fullscreen);
        // Esc closes the game, the same as the window's close button.  While the on-screen keyboard (name entry) is open
        // Esc is its Cancel key, so it is left alone then.  RR_ESC_QUIT_OFF=1 turns this off.
        if (wparam == VK_ESCAPE && (lparam & 0x40000000) == 0) {
            static const bool esc_quit_off = std::getenv("RR_ESC_QUIT_OFF") != nullptr;
            if (!esc_quit_off && !rr::osk_active()) { g_quit = true; return 0; }
        }
        // F1 cycles the anti-aliasing mode: none -> FXAA -> TAA -> SSAA filter (MSAA is chosen at start-up).
        if (wparam == VK_F1 && (lparam & 0x40000000) == 0) { rr::g_aa_mode = (rr::g_aa_mode.load() + 1) % 4; return 0; }
        // End cycles anisotropic filtering: 1x -> 2x -> 4x -> 8x -> 16x.
        if (wparam == VK_END && (lparam & 0x40000000) == 0) { const int a = rr::g_aniso.load(); rr::g_aniso = a >= 16 ? 1 : a * 2; return 0; }
        // PageUp / PageDown change how much of the new frame TAA mixes in (shown in the title bar; lower = smoother but more ghosting).
        if (wparam == VK_PRIOR) rr::g_taa_blend = std::clamp(rr::g_taa_blend.load() + 2, 4, 100);
        if (wparam == VK_NEXT) rr::g_taa_blend = std::clamp(rr::g_taa_blend.load() - 2, 4, 100);
        // F9 / F10 lower / raise the volume (hold to repeat), F8 mutes.
        if (wparam == VK_F9) rr::audio_volume_step(-1);
        if (wparam == VK_F8 && (lparam & 0x40000000) == 0) rr::audio_mute_toggle();
        // Developer keys (F2-F7, Insert, Delete): only with --dev.
        if (rr::g_rr_dev) {
            // F2 / F3 select the next / previous gate candidate (off -> 1 .. N -> all race systems -> off), F7 keeps the
            // selected one gated (on "off" or "all" it clears every kept gate).  See rr_tuning.hpp g_gate_sel.
            if ((wparam == VK_F2 || wparam == VK_F3) && (lparam & 0x40000000) == 0) {
                const int n = rr::g_gate_count.load();
                int s = rr::g_gate_sel.load();
                if (s < 0) s = 0;
                // Candidates 5 and 6 (sound system, car getter) crash when skipped, and 15..21 end the race at once: not offered.
                const auto unusable = [n](int v) { return v == 5 || v == 6 || (v >= 15 && v <= n); };
                const int dir = wparam == VK_F2 ? 1 : -1;
                do {
                    s += dir;
                    if (s > n + 1) s = 0;
                    if (s < 0) s = n + 1;
                } while (unusable(s));
                rr::g_gate_sel = s == 0 ? -1 : s;
            }
            if (wparam == VK_F7 && (lparam & 0x40000000) == 0) {
                const int s = rr::g_gate_sel.load();
                if (s <= 0 || s > rr::g_gate_count.load()) rr::g_gate_kept = 0ull;
                else rr::g_gate_kept.fetch_xor(1ull << (s - 1));
            }
            if (wparam == VK_INSERT && (lparam & 0x40000000) == 0) rr::g_audio_mark.fetch_add(1);
            if (wparam == VK_DELETE && (lparam & 0x40000000) == 0) rr::g_audio_silence.fetch_add(1);
            if (wparam == VK_F4 && (lparam & 0x40000000) == 0) rr::g_hide_class = (rr::g_hide_class.load() + 1) % 8;
            // F5 / F6 lower / raise the faint-blend cut-off (see rr_tuning.hpp).
            if (wparam == VK_F5 || wparam == VK_F6) {
                const int step = wparam == VK_F6 ? 4 : -4;
                rr::g_alpha_cutoff = std::clamp(rr::g_alpha_cutoff.load() + step, 0, 160);
            }
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    case WM_SYSKEYDOWN:
        // Windows delivers F10 as a "system" key and would open the (absent) menu bar, freezing the game in a modal loop.
        if (wparam == VK_F10) { rr::audio_volume_step(1); return 0; }
        if (wparam == VK_RETURN) {
            if ((lparam & 0x40000000) == 0) set_fullscreen(hwnd, !g_fullscreen);
            return 0;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    case WM_SYSKEYUP:
        if (wparam == VK_F10) return 0;
        return DefWindowProcW(hwnd, message, wparam, lparam);
    case WM_SYSCOMMAND:
        if ((wparam & 0xFFF0) == SC_KEYMENU) return 0;   // F10 / Alt alone must not enter menu mode
        return DefWindowProcW(hwnd, message, wparam, lparam);
    case WM_SYSCHAR:
        if (wparam == VK_RETURN) return 0;   // no error beep for Alt+Enter
        return DefWindowProcW(hwnd, message, wparam, lparam);
    case WM_SETCURSOR:
        // (hidden in full screen, except while one of the port's menu pages - keyboard, server browser - is open)
        if (g_fullscreen && LOWORD(lparam) == HTCLIENT && !rr::osk_active()) { SetCursor(nullptr); return TRUE; }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    case WM_MOUSEMOVE:
        g_mouse_px = static_cast<short>(LOWORD(lparam)); g_mouse_py = static_cast<short>(HIWORD(lparam)); g_mouse_seen = true;
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        g_mouse_px = static_cast<short>(LOWORD(lparam)); g_mouse_py = static_cast<short>(HIWORD(lparam)); g_mouse_seen = true;
        ++g_mouse_clicks;
        return 0;
    case WM_MOUSEWHEEL:
        g_mouse_wheel_acc += static_cast<short>(HIWORD(wparam));
        return 0;
    case WM_CHAR:
        if (g_typed.size() < 256u) g_typed.push_back(static_cast<char16_t>(wparam));
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

bool key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// ---- Gamepad (XInput, loaded at run time so there is no import-library dependency) -------------------
using XInputGetStateFn = DWORD(WINAPI *)(DWORD, XINPUT_STATE *);
XInputGetStateFn g_xinput_get = nullptr;
bool g_xinput_tried = false;
XINPUT_STATE g_pad_state{};
bool g_pad_connected = false;
std::chrono::steady_clock::time_point g_pad_polled{}, g_pad_missing_since{};

// Latest state of the first connected controller.  Polled at most every 2 ms; a missing controller is only re-checked once
// a second (XInputGetState on an empty slot is slow).
void pad_poll() {
    if (!g_xinput_tried) {
        g_xinput_tried = true;
        for (const char *dll : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"}) {
            if (HMODULE m = LoadLibraryA(dll)) {
                g_xinput_get = reinterpret_cast<XInputGetStateFn>(GetProcAddress(m, "XInputGetState"));
                if (g_xinput_get != nullptr) break;
            }
        }
    }
    if (g_xinput_get == nullptr) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - g_pad_polled < std::chrono::milliseconds(2)) return;
    if (!g_pad_connected && now - g_pad_missing_since < std::chrono::seconds(1)) return;
    g_pad_polled = now;
    XINPUT_STATE st{};
    bool found = false;
    for (DWORD i = 0; i < 4 && !found; ++i) {
        if (g_xinput_get(i, &st) == ERROR_SUCCESS) found = true;
    }
    g_pad_connected = found;
    if (found) g_pad_state = st; else { g_pad_state = XINPUT_STATE{}; g_pad_missing_since = now; }
}

std::uint32_t pad_buttons() {
    if (rr::pad::sdl_available()) return rr::pad::sdl_buttons();   // every kind of controller (SDL2.dll next to the game)
    pad_poll();
    if (!g_pad_connected) return 0u;
    const WORD b = g_pad_state.Gamepad.wButtons;
    std::uint32_t o = 0;
    if (b & XINPUT_GAMEPAD_START) o |= 0x0008u;
    if (b & XINPUT_GAMEPAD_BACK) o |= 0x0001u;
    if (b & XINPUT_GAMEPAD_DPAD_UP) o |= 0x0010u;
    if (b & XINPUT_GAMEPAD_DPAD_RIGHT) o |= 0x0020u;
    if (b & XINPUT_GAMEPAD_DPAD_DOWN) o |= 0x0040u;
    if (b & XINPUT_GAMEPAD_DPAD_LEFT) o |= 0x0080u;
    if (b & XINPUT_GAMEPAD_LEFT_SHOULDER) o |= 0x0100u;    // L
    if (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) o |= 0x0200u;   // R
    if (b & XINPUT_GAMEPAD_Y) o |= 0x1000u;                // TRIANGLE
    if (b & XINPUT_GAMEPAD_B) o |= 0x2000u;                // CIRCLE
    if (b & XINPUT_GAMEPAD_A) o |= 0x4000u;                // CROSS
    if (b & XINPUT_GAMEPAD_X) o |= 0x8000u;                // SQUARE
    if (rr::pad::triggers()) {                             // right trigger = CROSS (accelerate), left = SQUARE (brake)
        if (g_pad_state.Gamepad.bRightTrigger > 128) o |= 0x4000u;
        if (g_pad_state.Gamepad.bLeftTrigger > 128) o |= 0x8000u;
    }
    return o;
}

// Left stick -> PSP analog nub (0..255, 128 = centre) with a radial dead zone.
bool pad_stick(std::uint8_t &lx, std::uint8_t &ly) {
    if (rr::pad::sdl_available()) return rr::pad::sdl_stick(lx, ly);
    pad_poll();
    if (!g_pad_connected) return false;
    const float x = static_cast<float>(g_pad_state.Gamepad.sThumbLX) / 32767.0f;
    const float y = static_cast<float>(g_pad_state.Gamepad.sThumbLY) / 32767.0f;
    const float mag = std::sqrt(x * x + y * y);
    if (mag < 0.2f) return false;
    const float k = std::min(1.0f, (mag - 0.2f) / 0.8f) / mag;
    const auto conv = [](float v) { return static_cast<std::uint8_t>(std::clamp(128.0f + v * 127.5f, 0.0f, 255.0f)); };
    lx = conv(x * k);
    ly = conv(-y * k);       // PSP y grows downwards
    return true;
}

// ---- Direct3D 11 presenter -------------------------------------------------------------------------
// Uploads the 480x272 frame into a texture and draws it, letterboxed and bilinearly filtered, on the GPU.
// This replaces the (very slow) GDI HALFTONE StretchDIBits path; GDI remains as a fallback.
ID3D11Device *g_dev = nullptr;
ID3D11DeviceContext *g_ctx = nullptr;
IDXGISwapChain *g_swap = nullptr;
ID3D11RenderTargetView *g_rtv = nullptr;
ID3D11Texture2D *g_frame_tex = nullptr;
ID3D11ShaderResourceView *g_frame_srv = nullptr;
ID3D11VertexShader *g_vs = nullptr;
ID3D11PixelShader *g_ps = nullptr;
ID3D11SamplerState *g_sampler = nullptr;
int g_tex_w = 0, g_tex_h = 0;
UINT g_bb_w = 0, g_bb_h = 0;
bool g_d3d_ok = false;

const char *kShader = R"(
Texture2D frame : register(t0);
SamplerState smp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs_main(uint id : SV_VertexID) {
    VSOut o;
    float2 p = float2((id << 1) & 2, id & 2);
    o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = p;
    return o;
}
float4 ps_main(VSOut i) : SV_Target { return float4(frame.Sample(smp, i.uv).rgb, 1); }
)";

template <class T> void release(T *&p) { if (p) { p->Release(); p = nullptr; } }

bool d3d_make_rtv() {
    release(g_rtv);
    ID3D11Texture2D *back = nullptr;
    if (FAILED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&back)))) return false;
    const HRESULT hr = g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
    D3D11_TEXTURE2D_DESC desc{};
    back->GetDesc(&desc);
    g_bb_w = desc.Width; g_bb_h = desc.Height;
    back->Release();
    return SUCCEEDED(hr);
}

bool d3d_open(int width, int height) {
    RECT client{};
    GetClientRect(g_window, &client);
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = static_cast<UINT>(client.right > 0 ? client.right : 1);
    sd.BufferDesc.Height = static_cast<UINT>(client.bottom > 0 ? client.bottom : 1);
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_window;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                               &sd, &g_swap, &g_dev, &got, &g_ctx);
    if (FAILED(hr)) return false;
    if (!d3d_make_rtv()) return false;
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "vs_main", "vs_4_0", 0, 0, &vsb, &err))) { release(err); return false; }
    if (FAILED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "ps_main", "ps_4_0", 0, 0, &psb, &err))) { release(err); vsb->Release(); return false; }
    hr = g_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs);
    const HRESULT hr2 = g_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps);
    vsb->Release(); psb->Release();
    if (FAILED(hr) || FAILED(hr2)) return false;
    D3D11_SAMPLER_DESC smp{};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    if (FAILED(g_dev->CreateSamplerState(&smp, &g_sampler))) return false;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(width); td.Height = static_cast<UINT>(height);
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_frame_tex))) return false;
    if (FAILED(g_dev->CreateShaderResourceView(g_frame_tex, nullptr, &g_frame_srv))) return false;
    g_tex_w = width; g_tex_h = height;
    return true;
}

void d3d_close() {
    release(g_frame_srv); release(g_frame_tex); release(g_sampler); release(g_ps); release(g_vs); release(g_rtv);
    if (g_ctx) g_ctx->ClearState();
    release(g_swap); release(g_ctx); release(g_dev);
    g_d3d_ok = false;
}

bool d3d_present(const std::uint8_t *rgba, int width, int height) {
    if (!g_d3d_ok) return false;
    if (g_frame_tex == nullptr || width != g_tex_w || height != g_tex_h) return false;
    RECT client{};
    GetClientRect(g_window, &client);
    const UINT cw = static_cast<UINT>(client.right > 0 ? client.right : 1), ch = static_cast<UINT>(client.bottom > 0 ? client.bottom : 1);
    if (cw != g_bb_w || ch != g_bb_h) {
        release(g_rtv);
        if (FAILED(g_swap->ResizeBuffers(0, cw, ch, DXGI_FORMAT_UNKNOWN, 0)) || !d3d_make_rtv()) return false;
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(g_ctx->Map(g_frame_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
    for (int y = 0; y < height; ++y)
        std::memcpy(static_cast<std::uint8_t *>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch,
                    rgba + static_cast<std::size_t>(y) * width * 4u, static_cast<std::size_t>(width) * 4u);
    g_ctx->Unmap(g_frame_tex, 0);

    const float clear[4] = {0, 0, 0, 1};
    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_ctx->ClearRenderTargetView(g_rtv, clear);
    // Letterbox to the frame's aspect ratio.
    const float target_aspect = static_cast<float>(width) / static_cast<float>(height);
    float vw = static_cast<float>(g_bb_w), vh = vw / target_aspect;
    if (vh > static_cast<float>(g_bb_h)) { vh = static_cast<float>(g_bb_h); vw = vh * target_aspect; }
    D3D11_VIEWPORT vp{(static_cast<float>(g_bb_w) - vw) * 0.5f, (static_cast<float>(g_bb_h) - vh) * 0.5f, vw, vh, 0.0f, 1.0f};
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->VSSetShader(g_vs, nullptr, 0);
    g_ctx->PSSetShader(g_ps, nullptr, 0);
    g_ctx->PSSetShaderResources(0, 1, &g_frame_srv);
    g_ctx->PSSetSamplers(0, 1, &g_sampler);
    g_ctx->Draw(3, 0);
    ID3D11ShaderResourceView *none = nullptr;
    g_ctx->PSSetShaderResources(0, 1, &none);
    update_vsync(false);
    g_swap->Present(static_cast<UINT>(std::clamp(g_swap_interval, 0, 4)), 0);
    return true;
}

} // namespace

bool present_open(const PresentOptions &options) {
    if (!options.window) return false;
    g_throttle = options.throttle;
    g_frame_period_us = std::max(1000, options.frame_period_us);
    g_low_latency = options.low_latency; g_latency_log = options.latency_log;
    g_scale = options.scale < 1 ? 1 : options.scale;
    WNDCLASSW wc{};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = L"RidgeRacerNativeWindow";
    RegisterClassW(&wc);
    RECT rect{0, 0, 480 * g_scale, 272 * g_scale};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    g_window = CreateWindowExW(0, wc.lpszClassName, L"Ridge Racer (native recompilation)", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
                               nullptr, nullptr, wc.hInstance, nullptr);
    if (g_window != nullptr && options.fullscreen) set_fullscreen(g_window, true);
    return g_window != nullptr;
}

void present_pump() {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

namespace {
double g_swap_ms = 0.0, g_pace_ms = 0.0;

// Windows rounds Sleep() up to its timer tick (about 15.6 ms) unless a finer resolution is requested, which turns a
// 14 ms wait into 31 ms every few frames.  Ask for 1 ms for the lifetime of the process.
void request_fine_timer() {
    static bool done = false;
    if (done) return;
    done = true;
    if (HMODULE winmm = LoadLibraryW(L"winmm.dll")) {
        using Fn = UINT(WINAPI *)(UINT);
        if (auto begin = reinterpret_cast<Fn>(GetProcAddress(winmm, "timeBeginPeriod"))) begin(1);
    }
}

int monitor_refresh_hz(HMONITOR mon) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    if (mon == nullptr || !GetMonitorInfoW(mon, &mi)) return 0;
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) return 0;
    return dm.dmDisplayFrequency > 1 ? static_cast<int>(dm.dmDisplayFrequency) : 0;
}

// Chooses the swap interval for the monitor the window is on (re-checked every 60 swaps, so moving the window or toggling
// full screen onto another monitor picks it up).
void update_vsync(bool force) {
    if (g_window == nullptr) return;
    if (!force && ++g_vsync_check < 60) return;
    g_vsync_check = 0;
    HMONITOR mon = MonitorFromWindow(g_window, MONITOR_DEFAULTTONEAREST);
    if (!force && mon == g_vsync_monitor) return;
    g_vsync_monitor = mon;
    const int hz = monitor_refresh_hz(mon);
    const double fps = 1.0e6 / static_cast<double>(g_frame_period_us);
    int interval = 0;
    const char *env = std::getenv("RR_VSYNC");
    std::string why;
    if (env != nullptr && env[0] == '0') { why = "RR_VSYNC=0"; }
    else if (env != nullptr && env[0] != '\0') { interval = std::max(1, std::atoi(env)); why = "RR_VSYNC"; }
    else if (!g_throttle) { why = "unthrottled"; }
    else if (g_cpu_pacing) { why = "frame skip (CPU pacing)"; }
    else if (hz > 0) {
        // whole multiple n of the frame rate within 3 % (144 Hz at --fps 144 -> 1, 120 Hz at --fps 60 -> 2)
        const double n = static_cast<double>(hz) / fps;
        const double rn = std::round(n);
        if (rn >= 1.0 && std::fabs(n - rn) <= 0.03 * rn) interval = static_cast<int>(rn);
        why = interval ? "refresh matches the frame rate" : "refresh does not match the frame rate";
        // Strict vsync made every frame that missed a vblank by a fraction of a millisecond wait a whole extra refresh
        // (6.9 ms -> 13.9 ms at 144 Hz), and since the game advances one step per frame it ran 10-20 % slow in heavy
        // scenes.  Adaptive vsync (negative interval, WGL_EXT_swap_control_tear) waits for the vblank when the frame is on
        // time and presents at once (a brief tear) when it is late.  Without it: no vsync, as before.
        if (interval > 0) {
            if (g_swap_tear) { interval = -interval; why += ", adaptive"; }
            else { interval = 0; why += ", but adaptive vsync is not available (RR_VSYNC=1 forces strict)"; }
        }
    } else {
        why = "refresh rate unknown";
    }
    g_swap_interval = interval;
    if (g_wgl_swap_interval != nullptr) g_wgl_swap_interval(interval);
    log_line("PRESENT", "VSYNC interval=" + std::to_string(interval) + " monitor=" + std::to_string(hz) + " Hz fps=" +
                            std::to_string(static_cast<int>(fps + 0.5)) + " (" + why + ")");
}

void pace_frame() {
    request_fine_timer();
    const auto pace_t0 = std::chrono::steady_clock::now();
    struct PaceTimer { std::chrono::steady_clock::time_point t0; ~PaceTimer() { g_pace_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); } } pace_timer{pace_t0};
    if (g_throttle && g_swap_interval != 0) {   // the swap waits for the vblank (strict or adaptive); a second CPU schedule would fight it
        // the blocking swap already waits for the vblank; a second (CPU) schedule against it only adds stutter
        g_have_deadline = false;
    } else if (g_throttle) {
        using clock = std::chrono::steady_clock;
        const auto period = std::chrono::microseconds(g_frame_period_us);
        const auto now = clock::now();
        // After a slow frame start over from "now" instead of rushing the following frames to catch up: bursts of
        // fast frames look like the game lunging forward.
        // When far behind, restart the schedule from "now" with no wait for this frame (adding a full period here
        // used to make every late frame wait an extra 16 ms, which doubled slow frames).
        // (Making up several late frames in a row was tried: it shows as the game lurching forward after each hiccup.)
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

// ---- OpenGL context ---------------------------------------------------------------------------------
HDC g_gl_dc = nullptr;
HGLRC g_gl_rc = nullptr;
HMODULE g_gl_lib = nullptr;

void *gl_loader(const char *name) {
    using Fn = PROC(WINAPI *)(LPCSTR);
    static Fn get = reinterpret_cast<Fn>(GetProcAddress(g_gl_lib, "wglGetProcAddress"));
    void *p = get != nullptr ? reinterpret_cast<void *>(get(name)) : nullptr;
    const auto v = reinterpret_cast<std::intptr_t>(p);
    if (p == nullptr || v == 1 || v == 2 || v == 3 || v == -1) p = reinterpret_cast<void *>(GetProcAddress(g_gl_lib, name));
    return p;
}
} // namespace

GlProc present_gl_open() {
    if (g_window == nullptr) return nullptr;
    g_gl_lib = LoadLibraryW(L"opengl32.dll");
    if (g_gl_lib == nullptr) return nullptr;
    using CreateFn = HGLRC(WINAPI *)(HDC);
    using DeleteFn = BOOL(WINAPI *)(HGLRC);
    using MakeFn = BOOL(WINAPI *)(HDC, HGLRC);
    const auto wgl_create = reinterpret_cast<CreateFn>(GetProcAddress(g_gl_lib, "wglCreateContext"));
    const auto wgl_delete = reinterpret_cast<DeleteFn>(GetProcAddress(g_gl_lib, "wglDeleteContext"));
    const auto wgl_make = reinterpret_cast<MakeFn>(GetProcAddress(g_gl_lib, "wglMakeCurrent"));
    if (wgl_create == nullptr || wgl_delete == nullptr || wgl_make == nullptr) return nullptr;
    g_gl_dc = GetDC(g_window);
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 0;
    const int format = ChoosePixelFormat(g_gl_dc, &pfd);
    if (format == 0 || !SetPixelFormat(g_gl_dc, format, &pfd)) return nullptr;
    HGLRC legacy = wgl_create(g_gl_dc);
    if (legacy == nullptr || !wgl_make(g_gl_dc, legacy)) return nullptr;
    using AttribFn = HGLRC(WINAPI *)(HDC, HGLRC, const int *);
    const auto create_attribs = reinterpret_cast<AttribFn>(gl_loader("wglCreateContextAttribsARB"));
    if (create_attribs != nullptr) {
        // WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091, MINOR 0x2092, PROFILE_MASK 0x9126, CORE 0x1
        const int attribs[] = {0x2091, 3, 0x2092, 3, 0x9126, 0x1, 0};
        g_gl_rc = create_attribs(g_gl_dc, nullptr, attribs);
    }
    if (g_gl_rc == nullptr) { wgl_make(nullptr, nullptr); wgl_delete(legacy); return nullptr; }
    wgl_make(nullptr, nullptr);
    wgl_delete(legacy);
    if (!wgl_make(g_gl_dc, g_gl_rc)) return nullptr;
    using SwapIntervalFn = BOOL(WINAPI *)(int);
    g_wgl_swap_interval = reinterpret_cast<SwapIntervalFn>(gl_loader("wglSwapIntervalEXT"));
    {
        using ExtFn = const char *(WINAPI *)();
        using ExtArbFn = const char *(WINAPI *)(HDC);
        const char *ext = nullptr;
        if (const auto arb = reinterpret_cast<ExtArbFn>(gl_loader("wglGetExtensionsStringARB"))) ext = arb(g_gl_dc);
        if (ext == nullptr) if (const auto e = reinterpret_cast<ExtFn>(gl_loader("wglGetExtensionsStringEXT"))) ext = e();
        g_swap_tear = ext != nullptr && std::strstr(ext, "WGL_EXT_swap_control_tear") != nullptr;
    }
    if (g_wgl_swap_interval != nullptr) g_wgl_swap_interval(0);
    update_vsync(true);
    g_gl_finish = reinterpret_cast<void (*)()>(gl_loader("glFinish"));
    return &gl_loader;
}

void present_gl_size(int &width, int &height) {
    RECT client{};
    if (g_window != nullptr) GetClientRect(g_window, &client);
    width = client.right > 0 ? static_cast<int>(client.right) : 1;
    height = client.bottom > 0 ? static_cast<int>(client.bottom) : 1;
}

void present_note_pad_read() {
    g_pad_read_t = std::chrono::steady_clock::now();
    g_pad_read_since_swap = true;
}

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

void present_take_timing(double &swap_ms, double &pace_ms) {
    swap_ms = g_swap_ms; pace_ms = g_pace_ms;
    g_swap_ms = g_pace_ms = 0.0;
}

void present_gl_swap() {
    present_pump();
    if (g_gl_dc != nullptr) {
        update_vsync(false);
        const auto t0 = std::chrono::steady_clock::now();
        SwapBuffers(g_gl_dc);
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
        // Low-latency mode waits for the GPU after every frame; the measurement waits on one frame in 20 only (that frame's
        // wait shows how far behind the GPU was, i.e. how many frames the driver would otherwise have queued).
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

void present_skip_frame() { present_pump(); pace_frame(); }
void present_skip_frame_fast() { present_pump(); }
void present_set_cpu_pacing(bool on) { if (on != g_cpu_pacing) { g_cpu_pacing = on; g_vsync_monitor = nullptr; g_vsync_check = 60; } }

void present_set_frame_period_us(int period_us) {
    if (std::max(1000, period_us) != g_frame_period_us) g_vsync_monitor = nullptr;   // re-choose the swap interval
    g_frame_period_us = std::max(1000, period_us);
}

bool present_difftrace_requested() {
    if (!rr::g_rr_dev) return false;
    static bool was_down = false;
    const bool down = key(VK_HOME);
    const bool fire = down && !was_down;
    was_down = down;
    return fire;
}

bool present_callprofile_requested() {
    if (!rr::g_rr_dev) return false;
    static bool was_down = false;
    const bool down = key(VK_SCROLL);
    const bool fire = down && !was_down;
    was_down = down;
    return fire;
}

bool present_cartrace_requested() {
    if (!rr::g_rr_dev) return false;
    static bool was_down = false;
    const bool down = key(VK_DELETE);
    const bool fire = down && !was_down;
    was_down = down;
    return fire;
}

bool present_ramdump_requested() {
    if (!rr::g_rr_dev) return false;
    static bool was_down = false;
    const bool down = key(VK_INSERT);
    const bool fire = down && !was_down;
    was_down = down;
    return fire;
}

bool present_bug_report_requested() {
    static bool was_down = false;
    const bool down = key(VK_F12) && key(VK_CONTROL) && g_window != nullptr && GetForegroundWindow() == g_window;
    const bool fire = down && !was_down;
    was_down = down;
    return fire;
}

bool present_screenshot_requested() {
    static bool was_down = false;
    const bool down = key(VK_F12) && !key(VK_CONTROL);
    const bool fire = down && !was_down;
    was_down = down;
    return fire;
}

void present_set_title(const char *title) {
    if (g_window != nullptr) SetWindowTextA(g_window, title);
}

void present_frame(const std::uint8_t *rgba, int width, int height) {
    if (g_window != nullptr) {
        present_pump();
        static bool d3d_tried = false;
        if (!d3d_tried && g_window != nullptr) {
            d3d_tried = true;
            const char *disable = std::getenv("RR_GDI_PRESENT");
            if (disable == nullptr) {
                g_d3d_ok = d3d_open(width, height);
                if (!g_d3d_ok) d3d_close();
            }
        }
        if (g_window != nullptr && g_d3d_ok && d3d_present(rgba, width, height)) {
            // presented on the GPU
        } else if (g_window != nullptr) {
            g_bgra.resize(static_cast<std::size_t>(width) * height * 4u);
            for (std::size_t i = 0; i < static_cast<std::size_t>(width) * height; ++i) {
                g_bgra[i * 4u + 0u] = rgba[i * 4u + 2u];
                g_bgra[i * 4u + 1u] = rgba[i * 4u + 1u];
                g_bgra[i * 4u + 2u] = rgba[i * 4u + 0u];
                g_bgra[i * 4u + 3u] = 255u;
            }
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = width;
            info.bmiHeader.biHeight = -height;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            RECT client{};
            GetClientRect(g_window, &client);
            HDC dc = GetDC(g_window);
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, nullptr);
            StretchDIBits(dc, 0, 0, client.right, client.bottom, 0, 0, width, height, g_bgra.data(), &info,
                          DIB_RGB_COLORS, SRCCOPY);
            ReleaseDC(g_window, dc);
        }
    }
    pace_frame();
}

bool present_quit_requested() { return g_quit; }

std::uint32_t present_pad_buttons() {
    if (g_window == nullptr || GetForegroundWindow() != g_window) return 0u;
    std::uint32_t buttons = 0;
    if (key(VK_RETURN)) buttons |= 0x0008u;        // START
    if (key(VK_RSHIFT) || key(VK_BACK)) buttons |= 0x0001u;  // SELECT
    if (key(VK_UP)) buttons |= 0x0010u;
    if (key(VK_RIGHT)) buttons |= 0x0020u;
    if (key(VK_DOWN)) buttons |= 0x0040u;
    if (key(VK_LEFT)) buttons |= 0x0080u;
    if (key('Q')) buttons |= 0x0100u;              // L
    if (key('E')) buttons |= 0x0200u;              // R
    if (key('S')) buttons |= 0x1000u;              // TRIANGLE
    if (key('X')) buttons |= 0x2000u;              // CIRCLE
    if (key('Z')) buttons |= 0x4000u;              // CROSS
    if (key('A')) buttons |= 0x8000u;              // SQUARE
    return buttons | pad_buttons();
}

void present_pad_analog(std::uint8_t &lx, std::uint8_t &ly) {
    lx = 128;
    ly = 128;
    if (g_window == nullptr || GetForegroundWindow() != g_window) return;
    if (key('J')) lx = 0;
    if (key('L')) lx = 255;
    if (key('I')) ly = 0;
    if (key('K')) ly = 255;
    pad_stick(lx, ly);   // a controller stick outside its dead zone wins over the keyboard
}

void present_take_typed(std::vector<char16_t> &out) {
    out.clear();
    out.swap(g_typed);
}

std::uint32_t present_gamepad_buttons() { return pad_buttons(); }
int present_pad_family() {
    if (rr::pad::sdl_available()) return static_cast<int>(rr::pad::sdl_family());
    pad_poll();
    return g_pad_connected ? static_cast<int>(rr::pad::Family::Xbox) : 0;   // XInput: Xbox-style controllers
}
bool present_pad_stick_active() { std::uint8_t x = 128, y = 128; return pad_stick(x, y); }

void present_take_mouse(PresentMouse &out) {
    out = PresentMouse{};
    if (g_window == nullptr) return;
    RECT client{};
    GetClientRect(g_window, &client);
    if (g_mouse_seen) {
        POINT pt{};
        if (GetCursorPos(&pt) && ScreenToClient(g_window, &pt)) { g_mouse_px = pt.x; g_mouse_py = pt.y; }   // (also when it left the window)
        out.inside = present_map_pointer(g_mouse_px, g_mouse_py, client.right, client.bottom, out.x, out.y);
    }
    out.clicks = g_mouse_clicks;
    out.wheel = g_mouse_wheel_acc / WHEEL_DELTA;
    g_mouse_wheel_acc -= out.wheel * WHEEL_DELTA;
    g_mouse_clicks = 0;
}

void present_close() {
    d3d_close();
    if (g_window != nullptr) {
        DestroyWindow(g_window);
        g_window = nullptr;
    }
}

} // namespace rr
