#include "rr_present.hpp"

// Test rig: RR_EGL=1 gives the headless build an off-screen OpenGL 3.3 core context (EGL pbuffer, Mesa surfaceless /
// llvmpipe), so the GPU renderer (--gpu) can run without a window.  EGL is loaded at run time; no headers needed.
#include <cstdlib>
#include <dlfcn.h>
namespace {
using EGLDisplay = void *; using EGLConfig = void *; using EGLSurface = void *; using EGLContext = void *;
void *g_egl = nullptr; EGLDisplay g_dpy = nullptr; EGLSurface g_surf = nullptr; EGLContext g_ctx = nullptr;
void *(*p_getproc)(const char *) = nullptr;
int g_w = 480, g_h = 272;
bool egl_wanted() { return std::getenv("RR_EGL") != nullptr; }
}

namespace rr {
bool present_open(const PresentOptions &o) {
    if (!egl_wanted()) return false;
    g_w = 480 * (o.scale > 0 ? o.scale : 1); g_h = 272 * (o.scale > 0 ? o.scale : 1);
    return true;
}
void present_frame(const std::uint8_t *, int, int) {}
void present_pump() {}
bool present_quit_requested() { return false; }
std::uint32_t present_pad_buttons() { return 0u; }
void present_pad_analog(std::uint8_t &lx, std::uint8_t &ly) { lx = 128; ly = 128; }
void present_take_typed(std::vector<char16_t> &out) { out.clear(); }
void present_take_mouse(PresentMouse &out) { out = PresentMouse{}; }
std::uint32_t present_gamepad_buttons() { return 0u; }
int present_pad_family() { return 0; }
bool present_pad_stick_active() { return false; }
void present_close() {}
GlProc present_gl_open() {
    if (!egl_wanted()) return nullptr;
    g_egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (g_egl == nullptr) return nullptr;
    auto sym = [](const char *n) { return dlsym(g_egl, n); };
    auto getPlat = reinterpret_cast<EGLDisplay (*)(unsigned, void *, const long *)>(sym("eglGetPlatformDisplay"));
    auto getDpy = reinterpret_cast<EGLDisplay (*)(void *)>(sym("eglGetDisplay"));
    auto init = reinterpret_cast<unsigned (*)(EGLDisplay, int *, int *)>(sym("eglInitialize"));
    auto choose = reinterpret_cast<unsigned (*)(EGLDisplay, const int *, EGLConfig *, int, int *)>(sym("eglChooseConfig"));
    auto pbuf = reinterpret_cast<EGLSurface (*)(EGLDisplay, EGLConfig, const int *)>(sym("eglCreatePbufferSurface"));
    auto bind = reinterpret_cast<unsigned (*)(unsigned)>(sym("eglBindAPI"));
    auto mkctx = reinterpret_cast<EGLContext (*)(EGLDisplay, EGLConfig, EGLContext, const int *)>(sym("eglCreateContext"));
    auto cur = reinterpret_cast<unsigned (*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext)>(sym("eglMakeCurrent"));
    p_getproc = reinterpret_cast<void *(*)(const char *)>(sym("eglGetProcAddress"));
    if (!init || !choose || !pbuf || !bind || !mkctx || !cur || !p_getproc) return nullptr;
    if (getPlat) g_dpy = getPlat(0x31DDu /* EGL_PLATFORM_SURFACELESS_MESA */, nullptr, nullptr);
    if (g_dpy == nullptr && getDpy) g_dpy = getDpy(nullptr);
    int ma = 0, mi = 0;
    if (g_dpy == nullptr || !init(g_dpy, &ma, &mi)) return nullptr;
    const int cfg_attr[] = {0x3033, 0x0001, 0x3040, 0x0008, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3025, 24, 0x3026, 8, 0x3038};
    EGLConfig cfg = nullptr; int n = 0;
    if (!choose(g_dpy, cfg_attr, &cfg, 1, &n) || n < 1) return nullptr;
    const int sattr[] = {0x3057, g_w, 0x3056, g_h, 0x3038};
    g_surf = pbuf(g_dpy, cfg, sattr);
    bind(0x30A2 /* EGL_OPENGL_API */);
    const int cattr[] = {0x3098, 3, 0x30FB, 3, 0x30FD, 1, 0x3038};
    g_ctx = mkctx(g_dpy, cfg, nullptr, cattr);
    if (g_ctx == nullptr || !cur(g_dpy, g_surf, g_surf, g_ctx)) return nullptr;
    return [](const char *name) -> void * {
        if (void *f = p_getproc(name)) return f;
        static void *gl = dlopen("libGL.so.1", RTLD_NOW | RTLD_GLOBAL);
        return gl ? dlsym(gl, name) : nullptr;
    };
}
void present_gl_size(int &w, int &h) { w = g_w; h = g_h; }
void present_gl_swap() {}
void present_skip_frame() {}
void present_skip_frame_fast() {}
void present_set_cpu_pacing(bool) {}
void present_take_timing(double &swap_ms, double &pace_ms) { swap_ms = pace_ms = 0.0; }
void present_note_pad_read() {}
void present_take_latency(PresentLatency &out) { out = PresentLatency{}; }
void present_set_title(const char *) {}
void present_set_fullscreen(bool) {}
bool present_fullscreen() { return false; }
bool present_bug_report_requested() { return false; }
bool present_screenshot_requested() { return false; }
bool present_ramdump_requested() { return false; }
void present_set_frame_period_us(int) {}
bool present_difftrace_requested() { return false; }
bool present_callprofile_requested() { return false; }
bool present_cartrace_requested() { return false; }
} // namespace rr
