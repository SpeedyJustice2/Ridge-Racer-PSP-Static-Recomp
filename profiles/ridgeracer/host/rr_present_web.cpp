// Browser output (Emscripten): the engine runs inside a Web Worker and renders with WebGL2 into an OffscreenCanvas.  Each
// finished frame is handed to the page as an ImageBitmap (postMessage), so the engine never has to return to the browser's
// event loop.  Keyboard / gamepad state, the canvas size and quit requests arrive from the page through a SharedArrayBuffer
// (see web/rr-web.js for the other side); sound goes out through a second shared ring buffer that an AudioWorklet plays.
//
// Shared control block (Int32Array `ctl`):
//   0 pad buttons (PSP bits)   1/2 stick x/y (0..255, 128 = centre)   3 quit   4/5 canvas width/height (0 = unset)
//   6 hot-key bits (bit0 mute, bit1 volume down, bit2 volume up, bit3 anti-aliasing, bit4 anisotropy)
//   7 audio frames written     8 audio frames read     9/10 typed-character ring write/read     11/12 frames posted/consumed
//   13 audio clock (the AudioWorklet adds 1 every 128 samples and wakes waiters; used for frame pacing)
//   14 PSP button bits from gamepads only (slot 0 also has the keyboard): typed letters must not count as buttons
//   15 controller kind used last (rr::pad::Family, bits 0-7) | left stick moved (bit 8)
//   16..47 typed-character ring (UTF-16 units)
//   48/49 pointer x/y over the picture in 1/16 units of 480x272 (-1 = outside)   50 left-button presses (counter)
//   51 wheel steps (counter, + = away from the user)
#include "rr_present.hpp"
#include "rr_pad.hpp"
#include "rr_audio.hpp"
#include "rr_env.hpp"
#include "rr_log.hpp"
#include "rr_crumb.hpp"
#include "rr_tuning.hpp"
#include "psprecomp/runtime.hpp"

#include <emscripten.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <string>
#include <vector>

extern "C" void *emscripten_GetProcAddress(const char *name);

EM_JS(int, rr_js_ctl, (int i), { return Atomics.load(Module.rrCtl, i); });
EM_JS(void, rr_js_ctl_store, (int i, int v), { Atomics.store(Module.rrCtl, i, v); });
EM_JS(int, rr_js_ctl_exchange0, (int i), { return Atomics.exchange(Module.rrCtl, i, 0); });
EM_JS(int, rr_js_have_ctl, (), { return (Module.rrCtl && Module.rrAudio) ? 1 : 0; });

EM_JS(int, rr_js_gl_create, (int w, int h), {
    try {
        var oc = new OffscreenCanvas(w, h);
        var ctx = oc.getContext('webgl2', {alpha: false, antialias: false, depth: true, stencil: true, premultipliedAlpha: false,
                                           preserveDrawingBuffer: false, powerPreference: 'high-performance', desynchronized: true});
        if (!ctx) return 0;
        var G = (typeof GL !== 'undefined') ? GL : Module.GL;
        var handle = G.registerContext(ctx, {majorVersion: 2, minorVersion: 0, enableExtensionsByDefault: true});
        G.makeContextCurrent(handle);
        ctx.getExtension('EXT_texture_filter_anisotropic');
        ctx.getExtension('EXT_color_buffer_float');
        ctx.getExtension('EXT_color_buffer_half_float');
        ctx.getExtension('OES_texture_float_linear');
        Module.rrCanvas = oc;
        oc.addEventListener('webglcontextlost', function(e) { postMessage({t: 'log', text: '[PRESENT] WebGL context LOST (the browser reset the GPU process)'}); });
        oc.addEventListener('webglcontextrestored', function() { postMessage({t: 'log', text: '[PRESENT] WebGL context restored'}); });
        Module.rrCtx = ctx;
        return 1;
    } catch (e) {
        console.error('WebGL2 setup failed', e);
        return 0;
    }
});

// Hands the finished frame to the page.  Waits (briefly) when the page is more than two frames behind, which keeps the
// engine in step with the display instead of piling up bitmaps.
EM_JS(int, rr_js_present, (), {
    var c = Module.rrCanvas, ctl = Module.rrCtl;
    if (!c) return 0;
    var t0 = performance.now();
    for (;;) {
        var cons = Atomics.load(ctl, 12);
        if (Atomics.load(ctl, 11) - cons <= 2) break;
        Atomics.wait(ctl, 12, cons, 4);
        if (performance.now() - t0 > 100) break;
    }
    var waited = Math.round((performance.now() - t0) * 1000);
    var mode = Module.rrPresentMode | 0;   // (test switch, URL ?presentmode=N: 1 = no hand-over at all, 2 = made and closed in the worker)
    if (mode === 1) { Atomics.add(ctl, 11, 1); Atomics.add(ctl, 12, 1); return waited; }
    var bm = c.transferToImageBitmap();
    if (mode === 2) { bm.close(); Atomics.add(ctl, 11, 1); Atomics.add(ctl, 12, 1); return waited; }
    postMessage({t: 'frame', bm: bm}, [bm]);
    Atomics.add(ctl, 11, 1);
    var nw = Atomics.load(ctl, 4), nh = Atomics.load(ctl, 5);
    if (nw > 0 && nh > 0 && (nw !== c.width || nh !== c.height)) { c.width = nw; c.height = nh; }
    return waited;
});

// Lets the worker's event loop run once (needs JSPI: the wasm stack is suspended meanwhile).  Chromium only frees a frame
// bitmap handed to the page when the worker that made it gets back to its event loop; a worker that never does keeps
// every frame alive in the GPU process (about 30 MB/s) until the GPU process dies.  A MessageChannel round trip is used
// rather than setTimeout(0), which browsers stretch to 4 ms when it is called again and again.
EM_ASYNC_JS(void, rr_js_yield, (), {
    if (Module.rrPresentMode === 3) return;   // (test switch ?presentmode=3: never yield, shows the leak)
    if (!Module.rrYieldCh) {
        var ch = new MessageChannel();
        Module.rrYieldCh = ch;
        Module.rrYieldWake = null;
        ch.port1.onmessage = function() { var w = Module.rrYieldWake; Module.rrYieldWake = null; if (w) w(); };
    }
    await new Promise(function(r) { Module.rrYieldWake = r; Module.rrYieldCh.port2.postMessage(0); });
});

// Waits until the audio worklet's next tick (it bumps ctl[13] every 128 samples, ~2.9 ms, on the browser's real-time audio
// thread) or the timeout.  Returns the tick counter.  Timer-only waits are no good for pacing: Chrome on Windows rounds
// them to the 15.6 ms system tick, so a 6 ms sleep can last 22 ms.
EM_JS(int, rr_js_tick_wait, (int cur, double timeout_ms), {
    var ctl = Module.rrCtl;
    if (timeout_ms > 0) Atomics.wait(ctl, 13, cur, timeout_ms);
    return Atomics.load(ctl, 13);
});
EM_JS(int, rr_js_tick, (), { return Atomics.load(Module.rrCtl, 13); });

EM_JS(int, rr_js_audio_space, (int target), {
    var ctl = Module.rrCtl;
    var queued = Atomics.load(ctl, 7) - Atomics.load(ctl, 8);
    return Math.max(0, target - queued);
});

EM_JS(void, rr_js_audio_write, (const void *pcm, int frames), {
    var ctl = Module.rrCtl, ring = Module.rrAudio;
    var cap = ring.length >> 1;
    var w = Atomics.load(ctl, 7);
    var src = HEAP16.subarray(pcm >> 1, (pcm >> 1) + frames * 2);
    for (var i = 0; i < frames; ++i) {
        var at = ((w + i) % cap) * 2;
        ring[at] = src[i * 2];
        ring[at + 1] = src[i * 2 + 1];
    }
    Atomics.store(ctl, 7, w + frames);
});

// Posts the savedata folder to the page (which keeps it in IndexedDB) whenever it has changed.
EM_JS(void, rr_js_sync_saves, (), {
    try {
        var F = (typeof FS !== 'undefined') ? FS : Module.FS;
        if (!F.analyzePath('/savedata').exists) return;
        var files = [];
        var walk = function(d) {
            F.readdir(d).forEach(function(n) {
                if (n === '.' || n === '..') return;
                var p = d + '/' + n, st = F.stat(p);
                if (F.isDir(st.mode)) walk(p); else files.push([p, st.size, +st.mtime]);
            });
        };
        walk('/savedata');
        var sig = files.map(function(f) { return f.join(':'); }).join('|');
        if (sig === Module.rrSaveSig) return;
        Module.rrSaveSig = sig;
        postMessage({t: 'save', files: files.map(function(f) { return {path: f[0], data: F.readFile(f[0])}; })});
    } catch (e) {
        console.warn('save sync failed', e);
    }
});

// Shares the engine's memory with the page and tells it where the guest's dispatch address lives, so the page can say where
// the game is stuck if it ever stops presenting frames.
EM_JS(void, rr_js_publish_state, (const void *pc_address, const void *crumbs), { postMessage({t: 'mem', buf: HEAPU8.buffer, pc: pc_address, crumb: crumbs}); });

EM_JS(void, rr_js_title, (const char *text), { postMessage({t: 'title', text: UTF8ToString(text)}); });

namespace rr {
namespace {

bool g_open = false;
bool g_throttle = true;
int g_frame_period_us = 16683;
int g_scale = 3;
bool g_gl_ready = false;
std::chrono::steady_clock::time_point g_next_frame{}, g_last_save_sync{};
bool g_have_deadline = false;
double g_pace_ms = 0.0;

// Where each frame's time goes in the browser (RR_WEBPACE=0 turns the log lines off).  Per frame: game = from the end of
// the previous hand-over to this one (game code + drawing commands), wait = waiting for the page to take frames,
// yield = the trip through the worker's event loop, audio = mixing, pace = sleeping/spinning to the frame deadline.
// A WEBPACE line every 300 frames (sums and maxima) and a WEBSLOW line for each frame over 22 ms (first 300).
struct WebPace {
    double game = 0, wait = 0, yield = 0, audio = 0, pace = 0, total = 0;
    double max_game = 0, max_wait = 0, max_yield = 0, max_audio = 0, max_pace = 0, max_total = 0, sleep_over_ms = 0, gap_ms = 0, timer_late_ms = 0;
    int frames = 0, slow = 0;
} g_wp;
std::chrono::steady_clock::time_point g_wp_last_end{};
int g_wp_slow_logged = 0;
void webpace_frame(double game, double wait, double yield, double audio, double pace, double total) {
    static const bool on = [] { const char *e = std::getenv("RR_WEBPACE"); return e == nullptr || std::string(e) != "0"; }();
    if (!on) return;
    auto &w = g_wp;
    w.game += game; w.wait += wait; w.yield += yield; w.audio += audio; w.pace += pace; w.total += total;
    w.max_game = std::max(w.max_game, game); w.max_wait = std::max(w.max_wait, wait); w.max_yield = std::max(w.max_yield, yield);
    w.max_audio = std::max(w.max_audio, audio); w.max_pace = std::max(w.max_pace, pace); w.max_total = std::max(w.max_total, total);
    ++w.frames;
    if (total > 22.0) {
        ++w.slow;
        if (g_wp_slow_logged < 300) {
            ++g_wp_slow_logged;
            char b[200];
            std::snprintf(b, sizeof b, "WEBSLOW %.1fms: game %.1f, wait for page %.1f, yield %.1f, audio %.1f, pace %.1f", total, game, wait, yield, audio, pace);
            log_line("PRESENT", b);
        }
    }
    if (w.frames >= 300) {
        char b[512];
        std::snprintf(b, sizeof b, "WEBPACE %d frames, avg/max ms: game %.1f/%.1f wait %.1f/%.1f yield %.2f/%.1f audio %.2f/%.1f pace %.1f/%.1f total %.1f/%.1f, over 22 ms: %d, deadline overshoot max %.1f, clock gap %.1f, timer late %.1f",
                      w.frames, w.game / w.frames, w.max_game, w.wait / w.frames, w.max_wait, w.yield / w.frames, w.max_yield, w.audio / w.frames, w.max_audio,
                      w.pace / w.frames, w.max_pace, w.total / w.frames, w.max_total, w.slow, w.sleep_over_ms, w.gap_ms, w.timer_late_ms);
        log_line("PRESENT", b);
        const double keep_gap = w.gap_ms, keep_late = w.timer_late_ms;
        w = WebPace{};
        w.gap_ms = keep_gap; w.timer_late_ms = keep_late;
    }
}

void pace_frame() {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    if (g_throttle) {
        const auto period = std::chrono::microseconds(g_frame_period_us);
        const auto now = clock::now();
        if (!g_have_deadline || now > g_next_frame + period) g_next_frame = now;
        else g_next_frame += period;
        g_have_deadline = true;
        // Two ways to wait, both learned while running:
        //  - the shared clock ctl[13], bumped by the audio worklet (every 128 samples, delivered in bursts by the audio
        //    device) and by the page on every display refresh.  `gap_ms` = longest time seen between bumps; a wait for the
        //    next bump is only started while more than that is left, so it always wakes before the deadline.
        //  - plain timer sleeps (when the clock is not moving), `timer_late_ms` = how late they wake (Chrome on Windows
        //    rounds them to the 15.6 ms system tick).
        // Whatever is left is spun.  RR_WEB_PACE=1: timer sleeps only (the fix12 behaviour).
        static double timer_late_ms = 2.0, gap_ms = 8.0;
        static int last_tick = -1; static clock::time_point last_change{};
        static const int mode = [] { const char *e = std::getenv("RR_WEB_PACE"); return e ? std::atoi(e) : 0; }();
        // RR_WEB_COARSE_TIMER=1 (test): timer waits end on the next 15.625 ms boundary, like Chrome on Windows
        static const bool coarse = std::getenv("RR_WEB_COARSE_TIMER") != nullptr;
        const auto coarse_ms = [&](double ms) {
            if (!coarse) return ms;
            const double now_ms = std::chrono::duration<double, std::milli>(clock::now().time_since_epoch()).count();
            return std::ceil((now_ms + ms) / 15.625) * 15.625 - now_ms;
        };
        bool watching = false;   // gaps only count while this loop has been watching the clock without interruption
        for (;;) {
            const auto t = clock::now();
            if (t >= g_next_frame) break;
            const double rem = std::chrono::duration<double, std::milli>(g_next_frame - t).count();
            const int tick = rr_js_tick();
            if (tick != last_tick) {
                if (watching) gap_ms = std::min(20.0, std::max(std::chrono::duration<double, std::milli>(t - last_change).count(), gap_ms * 0.99));
                last_tick = tick; last_change = t; watching = true;
            }
            const bool clock_moving = mode != 1 && t - last_change < std::chrono::milliseconds(60);
            if (clock_moving && rem > gap_ms + 0.5) {
                rr_js_tick_wait(tick, coarse_ms(rem - gap_ms));   // woken by the next bump, normally long before the timeout
            } else if (!clock_moving && rem > timer_late_ms + 1.0) {
                const double want = rem - timer_late_ms - 1.0;
                const auto s0 = clock::now();
                std::this_thread::sleep_for(std::chrono::microseconds(static_cast<long long>(coarse_ms(want) * 1000.0)));
                const double late = std::chrono::duration<double, std::milli>(clock::now() - s0).count() - want;
                timer_late_ms = std::max(late, timer_late_ms * 0.995);
                watching = false;
            }
            // else: spin for the last part
        }
        g_wp.gap_ms = gap_ms; g_wp.timer_late_ms = timer_late_ms;
        const double miss = std::chrono::duration<double, std::milli>(clock::now() - g_next_frame).count();
        g_wp.sleep_over_ms = std::max(g_wp.sleep_over_ms, miss);
    }
    g_pace_ms += std::chrono::duration<double, std::milli>(clock::now() - t0).count();
}

// Keeps about 70 ms of sound queued for the AudioWorklet.
void audio_pump() {
    constexpr int kChunk = 512, kTarget = 3072;
    static std::int16_t buf[kChunk * 2];
    for (int guard = 0; guard < 8; ++guard) {
        if (rr_js_audio_space(kTarget) < kChunk) break;
        audio_mix(buf, kChunk);
        rr_js_audio_write(buf, kChunk);
    }
}

void handle_hotkeys() {
    const int bits = rr_js_ctl_exchange0(6);
    if (bits == 0) return;
    if (bits & 1) audio_mute_toggle();
    if (bits & 2) audio_volume_step(-1);
    if (bits & 4) audio_volume_step(1);
    if (bits & 8) g_aa_mode = (g_aa_mode.load() + 1) % 4;
    if (bits & 16) { const int a = g_aniso.load(); g_aniso = a >= 16 ? 1 : a * 2; }
}

} // namespace

bool present_open(const PresentOptions &options) {
    if (!options.window) return false;
    if (!rr_js_have_ctl()) {
        log_line("PRESENT", "the page did not provide the shared control block; no output");
        return false;
    }
    g_throttle = options.throttle;
    g_frame_period_us = std::max(1000, options.frame_period_us);
    g_scale = options.scale < 1 ? 1 : options.scale;
    g_open = true;
    rr_js_publish_state(psprecomp::runtime_dispatch_pc_address(), const_cast<const std::uint32_t *>(g_rr_crumb));
    return true;
}

void present_set_frame_period_us(int period_us) { g_frame_period_us = std::max(1000, period_us); }

void present_pump() { if (g_open) handle_hotkeys(); }

GlProc present_gl_open() {
    if (!g_open) return nullptr;
    int w = rr_js_ctl(4), h = rr_js_ctl(5);
    if (w <= 0 || h <= 0) { w = 480 * g_scale; h = 272 * g_scale; }
    if (!rr_js_gl_create(w, h)) {
        log_line("PRESENT", "could not create a WebGL2 context");
        return nullptr;
    }
    g_gl_ready = true;
    return [](const char *name) -> void * { return emscripten_GetProcAddress(name); };
}

void present_gl_size(int &width, int &height) {
    int w = rr_js_ctl(4), h = rr_js_ctl(5);
    if (w <= 0 || h <= 0) { w = 480 * g_scale; h = 272 * g_scale; }
    width = w;
    height = h;
}

void present_gl_swap() {
    using clock = std::chrono::steady_clock;
    const auto ms = [](clock::time_point a, clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    const auto t0 = clock::now();
    const double game = g_wp_last_end.time_since_epoch().count() != 0 ? ms(g_wp_last_end, t0) : 0.0;
    handle_hotkeys();
    double wait = 0.0;
    auto t1 = t0, t2 = t0;
    if (g_gl_ready) { wait = rr_js_present() / 1000.0; t1 = clock::now(); rr_js_yield(); t2 = clock::now(); }
    audio_pump();
    const auto t3 = clock::now();
    if (t3 - g_last_save_sync > std::chrono::seconds(2)) { g_last_save_sync = t3; rr_js_sync_saves(); }
    const auto t4 = clock::now();
    pace_frame();
    const auto t5 = clock::now();
    g_wp_last_end = t5;
    webpace_frame(game, wait, g_gl_ready ? ms(t1, t2) : 0.0, ms(t2, t3), ms(t4, t5), game + ms(t0, t5));
}

void present_skip_frame() {
    handle_hotkeys();
    audio_pump();
    pace_frame();
}
void present_skip_frame_fast() {
    handle_hotkeys();
    audio_pump();
}
void present_set_cpu_pacing(bool) {}

// Software-raster path (only when the GL renderer is not running): not supported in the browser build.
void present_frame(const std::uint8_t *, int, int) {
    handle_hotkeys();
    audio_pump();
    pace_frame();
}

bool present_quit_requested() { return g_open && rr_js_ctl(3) != 0; }

std::uint32_t present_pad_buttons() { return g_open ? static_cast<std::uint32_t>(rr_js_ctl(0)) : 0u; }

void present_pad_analog(std::uint8_t &lx, std::uint8_t &ly) {
    lx = 128; ly = 128;
    if (!g_open) return;
    lx = static_cast<std::uint8_t>(std::clamp(rr_js_ctl(1), 0, 255));
    ly = static_cast<std::uint8_t>(std::clamp(rr_js_ctl(2), 0, 255));
}

void present_take_typed(std::vector<char16_t> &out) {
    out.clear();
    if (!g_open) return;
    int r = rr_js_ctl(10);
    const int w = rr_js_ctl(9);
    while (r != w && out.size() < 64u) {
        out.push_back(static_cast<char16_t>(rr_js_ctl(16 + (r & 31))));
        ++r;
    }
    rr_js_ctl_store(10, r);
}

std::uint32_t present_gamepad_buttons() { return g_open ? static_cast<std::uint32_t>(rr_js_ctl(14)) : 0u; }
// ctl[15]: rr::pad::Family of the controller used last (bits 0-7), left stick out of the dead zone (bit 8); set by rr-web.js.
int present_pad_family() { return g_open ? (rr_js_ctl(15) & 0xFF) : 0; }
bool present_pad_stick_active() { return g_open && (rr_js_ctl(15) & 0x100) != 0; }

void present_take_mouse(PresentMouse &out) {
    out = PresentMouse{};
    if (!g_open) return;
    static int last_clicks = 0, last_wheel = 0;
    static bool first = true;
    const int x = rr_js_ctl(48), y = rr_js_ctl(49), c = rr_js_ctl(50), wl = rr_js_ctl(51);
    if (first) { first = false; last_clicks = c; last_wheel = wl; }
    out.inside = x >= 0 && y >= 0;
    out.x = static_cast<float>(x) / 16.0f; out.y = static_cast<float>(y) / 16.0f;
    out.clicks = std::clamp(c - last_clicks, 0, 8); out.wheel = std::clamp(wl - last_wheel, -20, 20);
    last_clicks = c; last_wheel = wl;
}

void present_close() {
    g_open = false;
    g_gl_ready = false;
}

void present_take_timing(double &swap_ms, double &pace_ms) { swap_ms = 0.0; pace_ms = g_pace_ms; g_pace_ms = 0.0; }
void present_note_pad_read() {}
void present_take_latency(PresentLatency &out) { out = PresentLatency{}; }
void present_set_title(const char *title) { if (g_open) rr_js_title(title); }
bool present_bug_report_requested() { return false; }
bool present_screenshot_requested() { return false; }
bool present_ramdump_requested() { return false; }
bool present_cartrace_requested() { return false; }
bool present_difftrace_requested() { return false; }
bool present_callprofile_requested() { return false; }

} // namespace rr
