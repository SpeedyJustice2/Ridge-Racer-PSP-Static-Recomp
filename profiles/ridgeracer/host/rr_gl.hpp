#pragma once

// OpenGL / OpenGL ES 3 backend for the PSP graphics engine.
//
// The software backend (rr_raster.cpp) draws straight into emulated VRAM at native resolution.  This backend keeps
// the large (512-pixel-stride) render targets - the screen buffers - on the GPU at an integer multiple of the PSP
// resolution, so geometry is rasterised at e.g. 3x or 4x while textures stay as the game authored them.  Everything
// else (small render-to-texture targets, palette strips) still goes through the software backend so emulated VRAM
// stays exact where the game reads it back.  Coherency between the two worlds is handled by the vram_read /
// vram_write hooks: a GPU target is copied back down to VRAM only when something other than the GPU needs to see it.
//
// The backend is context-agnostic: the platform layer creates a GL 3.3 core (Windows) or GLES 3.0 (Android, tests)
// context, makes it current on the emulation thread and passes a function loader.

#include "psprecomp/guest_memory.hpp"
#include "rr_gpu.hpp"
#include "rr_raster.hpp"

#include <bitset>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace psprecomp { class GuestMemory; }

namespace rr {

class GlBackend final : public GpuBackend {
public:
    using GetProc = void *(*)(const char *);

    // `gles` selects the GLSL dialect (#version 300 es vs 330 core).  `scale` is the internal resolution factor.
    GlBackend(psprecomp::GuestMemory &memory, GetProc get_proc, bool gles, int scale);
    ~GlBackend() override;
    GlBackend(const GlBackend &) = delete;
    GlBackend &operator=(const GlBackend &) = delete;

    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] const std::string &error() const { return error_; }
    [[nodiscard]] int scale() const { return scale_; }

    void draw(const GpuDraw &state, GpuPrim prim, const GpuVertex *vertices, std::size_t count) override;
    void vram_read(std::uint32_t address, std::uint32_t length) override;
    void vram_read_late_ok(std::uint32_t address, std::uint32_t length) override;
    void set_read_reason(const char *reason) override { rb_reason_ = reason; }
    void vram_write(std::uint32_t address, std::uint32_t length) override;
    void sync() override { flush(); }

    // Visual enhancements (all optional): FXAA + sharpening when the screen is presented, trilinear/anisotropic
    // filtering for 3D textures.  Call once after construction, with the GL context current.
    // The anti-aliasing post mode (none / FXAA / TAA / SSAA filter) lives in g_aa_mode (rr_tuning.hpp); `msaa` is the
    // requested MSAA sample count (0/1 = off), fixed for the run.
    struct Enhance { bool fxaa{false}; float sharpen{0.0f}; bool mipmaps{true}; int aniso{8}; float ui_crisp{0.8f}; bool smooth_cutouts{false}; int msaa{0}; };
    void set_enhance(const Enhance &e);

    // Call once per presented frame (ages caches and the "recently drawn" test used by has_target).
    void end_frame();

    // True when `address` is a screen buffer the GPU drew into in the last couple of frames.
    [[nodiscard]] bool has_target(std::uint32_t address) const;
    // Writes the target as a native-resolution (480x272) RGBA8 image; false if there is no such target.
    bool read_native_rgba(std::uint32_t address, std::uint8_t *rgba);
    // Writes the target at internal resolution (width = 480*scale, height = 272*scale), RGBA8, top row first.
    bool read_scaled_rgba(std::uint32_t address, std::vector<std::uint8_t> &rgba, int &width, int &height);
    // Blits the target, letterboxed and filtered, into the currently bound default framebuffer.
    bool present_target(std::uint32_t address, int win_w, int win_h);
    // Screenshot of exactly what the window shows (anti-aliasing, sharpening, letterboxing applied): call request_capture()
    // before the frame is presented, then take_capture() afterwards.  The image is the letterboxed picture area, top row first.
    void request_capture() { capture_pending_ = true; capture_ok_ = false; }
    bool take_capture(std::vector<std::uint8_t> &rgba, int &width, int &height);
    // Same for a CPU-side 480x272 RGBA frame (movies, loading screens drawn by the CPU, software fallbacks).
    void present_rgba(const std::uint8_t *rgba, int width, int height, int win_w, int win_h);

    struct Stats { std::uint64_t gl_draws{}, sw_draws{}, texture_uploads{}, texture_hits{}, readbacks{}, uploads{}, tex_failed{}, readback_rows{}, merged_draws{}, rt_tex{}, readback_skipped{}; };
    [[nodiscard]] const Stats &stats() const { return stats_; }
    // Milliseconds spent submitting batched draws to the GL driver since the last call.
    [[nodiscard]] double rb_total_ms() const { return rb_total_ms_; }
    double take_flush_ms() { const double v = flush_ms_; flush_ms_ = 0.0; return v; }
    [[nodiscard]] std::string brief() const;
    // Counters for the per-frame spike log: (readbacks, gl draws, sw draws, texture uploads).
    [[nodiscard]] std::uint64_t spike_counter(int which) const { return which == 0 ? stats_.readbacks : which == 1 ? stats_.gl_draws : which == 2 ? stats_.sw_draws : stats_.texture_uploads; }
    // Diagnostic lines queued since the last call (new textures, textures that failed to load, ...).
    std::vector<std::string> take_events() { std::vector<std::string> out; out.swap(events_); return out; }

private:
    struct Gl;                               // function table (rr_gl.cpp)
    struct Rt {
        std::uint32_t addr{}, stride{}, format{};
        int w{}, h{};
        unsigned tex{}, fbo{}, depth{};
        // MSAA: drawing goes to ms_fbo (multisampled colour + depth/stencil); `tex`/`fbo` hold the resolved copy.
        unsigned ms_fbo{}, ms_color{}, ms_depth{};
        bool ms_dirty{}, ms_depth_dirty{};               // multisampled buffers are ahead of tex / fbo's depth
        bool ms_fill{};                                  // tex was changed under the multisampled colour: copy rows ms_fill_y0..ms_fill_y1 back in before drawing
        int ms_fill_y0{}, ms_fill_y1{};
        bool gpu_dirty{}, cpu_dirty{}, margin_dirty{}, margin_stale{};
        bool stencil_used{}, alpha_stale{};            // 5551/4444 target whose stencil buffer was written: RT alpha must be refreshed from it before being read
        std::uint32_t rows{272u};                    // rows of the target that exist in VRAM (small buffers near the end are shorter)
        std::bitset<272> rows_dirty;                 // rows where the GPU copy is ahead of memory
        std::uint32_t cpu_row0{272u}, cpu_row1{0u};   // rows memory has changed under the GPU copy (valid while cpu_dirty)
        std::uint64_t last_draw{};
        std::vector<std::uint64_t> row_hash;          // hash of each row's visible memory bytes at the last GPU<->memory sync
        std::vector<std::uint8_t> row_hash_ok;
        [[nodiscard]] std::uint32_t bytes() const { return stride * rows * (format == 3u ? 4u : 2u); }
    };
    struct CachedTexture { unsigned tex{}; std::uint64_t last_use{}; std::uint32_t bytes{}; };

    [[nodiscard]] bool gl_target(const GpuDraw &d) const;
    [[nodiscard]] static std::uint32_t rows_for(std::uint32_t addr, std::uint32_t stride, std::uint32_t format);
    Rt *find_source_rt(const GpuDraw &d, const Rt &dst, std::uint32_t &row0, const GpuVertex *v, std::size_t n, bool allow_self);
    Rt *find_rt(std::uint32_t address);
    const Rt *find_rt(std::uint32_t address) const;
    Rt *get_rt(const GpuDraw &d);
    void destroy_rt(Rt &rt);
    void capture_default_fb(int win_w, int win_h);   // copies the letterboxed picture from the window framebuffer when a capture is pending
    void resolve_rt(Rt &rt, bool with_depth);   // MSAA: resolves the multisampled colour (and depth/stencil) into tex / fbo
    unsigned taa_pass(Rt &rt);                  // temporal AA: blends the screen into the history, returns the texture to present
    [[nodiscard]] bool post_wanted(bool cpu_frame) const;
    void resolve_stencil(Rt &rt);   // writes the stencil buffer into the RT alpha channel (the PSP keeps stencil in framebuffer alpha)
    void sync_rows(Rt &rt, const std::uint8_t *mem, std::uint32_t row0, std::uint32_t row1);
    void readback(Rt &rt, std::uint32_t row0 = 0u, std::uint32_t row1 = 271u);
    void readback_to(Rt &rt, std::uint8_t *dst, std::uint32_t columns, std::uint32_t row0 = 0u, std::uint32_t row1 = 271u);
    void draw_gl(const GpuDraw &state, GpuPrim prim, const GpuVertex *vertices, std::size_t count);
    void submit_gl(const GpuDraw &state, unsigned mode, const GpuVertex *vertices, std::size_t count, Rt &rt);
    void queue_gl(const GpuDraw &state, GpuPrim prim, const GpuVertex *vertices, std::size_t count);
    void flush();
    [[nodiscard]] static bool same_state(const GpuDraw &a, const GpuDraw &b);
    [[nodiscard]] static std::uint32_t texture_bytes_of(const GpuDraw &d);
    void verify_draw(const GpuDraw &state, GpuPrim prim, const GpuVertex *vertices, std::size_t count);
    [[nodiscard]] static bool hits_columns(const Rt &rt, std::uint32_t address, std::uint32_t length, std::uint32_t x0, std::uint32_t x1);
    void upload_from_vram(Rt &rt);   // uploads the rows recorded in cpu_row0..cpu_row1
    unsigned get_texture(const GpuDraw &d, bool &valid);
    void fallback_software(const GpuDraw &d, GpuPrim prim, const GpuVertex *v, std::size_t count);
    bool compile_programs();
    bool compile_post();
    void post_pass(unsigned tex, int src_w, int src_h, int vis_w, int win_w, int win_h);
    void purge_textures();

    psprecomp::GuestMemory &memory_;
    std::unique_ptr<Gl> gl_;
    SoftwareBackend sw_;
    bool gles_{};
    Enhance enhance_{};
    int scale_{1};
    bool capture_pending_{false}, capture_ok_{false};
    int capture_w_{0}, capture_h_{0};
    std::vector<std::uint8_t> capture_;
    int aniso_applied_{0};
    int msaa_{0};                // active MSAA sample count (0 = off)
    std::uint64_t taa_frame_{~0ull};
    bool ok_{};
    std::string error_;
    std::uint64_t frame_{};
    std::vector<std::unique_ptr<Rt>> rts_;
    std::unordered_map<std::uint64_t, CachedTexture> textures_;
    std::uint64_t texture_bytes_{};
    std::vector<std::uint32_t> decode_scratch_;
    std::vector<GpuVertex> quad_scratch_;
    std::vector<std::uint8_t> readback_scratch_;
    Stats stats_;
    // Consecutive draws with identical state are merged into one GPU draw call.
    double flush_ms_{0.0};
    bool batch_active_{false};
    GpuDraw batch_d_;
    std::uint8_t batch_clut_[1024]{};
    std::vector<GpuVertex> batch_v_;
    Rt *batch_rt_{nullptr};
    const char *rb_reason_{"ge"};
    double rb_stage_ms_[4]{};
    double rb_total_ms_{0.0};
    bool rb_lag_{false};
    bool src_onepx_{false};
    std::uint64_t rb_ring_seq_{0};
    std::uint32_t rb_dst_{0};
    std::uint64_t rb_by_reason_[6]{};
    std::vector<std::string> events_;
    // Self-check (RR_GPU_VERIFY=N): every Nth draw is also rendered by the software renderer on a scratch copy of
    // memory and the two results are compared.
    std::uint64_t verify_bad_{0}, verify_total_{0};
    std::uint32_t verify_every_{0};
    bool verify_small_only_{false};
    std::uint64_t verify_counter_{0};
    std::unique_ptr<psprecomp::GuestMemory> shadow_;
    std::unique_ptr<SoftwareBackend> shadow_sw_;
    std::uint32_t events_budget_{3000};
};

} // namespace rr
