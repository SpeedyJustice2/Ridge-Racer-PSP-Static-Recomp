#pragma once

// Interface between the PSP graphics-engine (GE) command processor and a
// drawing backend.  The processor decodes display lists, tracks GE state and
// transforms/clips geometry; a backend only turns finished primitives into
// pixels.  The software backend (rr_raster.cpp) reproduces the PSP at native
// resolution inside emulated VRAM.  A GPU backend can implement the same
// interface and render at a higher internal resolution.

#include <cstddef>
#include <cstdint>

namespace rr {

enum class GpuPrim : std::uint8_t { Points, Lines, Triangles, Sprites };

// A vertex after transform, in PSP screen space.  x/y are pixel coordinates
// (integers are pixel edges), z is the 0..65535 depth value, inv_w is 1/clip.w
// (1.0 in through mode) and u/v are in texel units of mip level 0.
struct GpuVertex {
    float x{}, y{}, z{}, inv_w{1.0f};
    float u{}, v{};
    float r{255.0f}, g{255.0f}, b{255.0f}, a{255.0f};
    float ub[4]{-1e30f, -1e30f, 1e30f, 1e30f};   // sprites: the sprite's own uv rect (umin, vmin, umax, vmax); sentinel = unbounded
    float fog{1.0f};   // fog coefficient: 1 = no fog, 0 = fully fogged (per vertex, 1.0 when fog is off)
};

struct GpuDraw {
    // Render target.
    std::uint32_t fb_addr{};      // absolute guest address (0x04000000 based)
    std::uint32_t fb_stride{512}; // pixels
    std::uint32_t fb_format{3};   // 0=565 1=5551 2=4444 3=8888
    std::uint32_t z_addr{};
    std::uint32_t z_stride{512};
    bool through{};
    // Clear mode: sprites overwrite color/alpha/depth without any other test.
    bool clear_mode{}, clear_color{}, clear_alpha{}, clear_depth{};
    bool stencil_test{};
    std::uint32_t stencil_func{1}, stencil_ref{}, stencil_mask{0xFF};
    std::uint32_t stencil_sfail{}, stencil_zfail{}, stencil_zpass{}; // 0 keep 1 zero 2 replace 3 invert 4 incr 5 decr
    // Fog (applied after texturing, before the alpha test): colour = colour * fog + fog_color * (1 - fog).
    bool fog{};
    std::uint32_t fog_color{};     // 0x00BBGGRR
    // Texture.
    bool tex_enable{};
    std::uint32_t tex_addr{}, tex_stride{}, tex_width{1}, tex_height{1};
    std::uint32_t tex_format{};    // 0..7 as GE, 8..10 DXT1/3/5
    bool tex_swizzled{}, tex_linear{}, clamp_u{}, clamp_v{};
    std::uint32_t tex_func{};      // 0 modulate 1 decal 2 blend 3 replace 4 add
    bool tex_alpha{}, tex_double{};
    std::uint32_t tex_env{};       // 0x00BBGGRR
    std::uint32_t clut_format{}, clut_shift{}, clut_mask{0xFF}, clut_start{};
    const std::uint8_t *clut{};    // 1024 bytes
    // Per-pixel tests and blending.
    bool blend{};
    std::uint32_t blend_eq{}, blend_src{}, blend_dst{}, fix_a{}, fix_b{};
    bool alpha_test{};
    std::uint32_t atest_func{}, atest_ref{}, atest_mask{0xFF};
    bool color_test{};                                  // per-pixel test of the (textured, fogged) source colour
    std::uint32_t ctest_func{}, ctest_ref{}, ctest_mask{0xFFFFFF};   // 0 never 1 always 2 equal 3 notequal; colours 0x00BBGGRR
    bool depth_test{}, depth_write{true};
    std::uint32_t depth_func{};
    std::uint32_t mask_rgb{}, mask_alpha{};
    std::int32_t scissor_x0{}, scissor_y0{}, scissor_x1{479}, scissor_y1{271};
};

// Diagnostics a backend fills in while drawing (used to find rendering bugs).
struct GpuDiag {
    std::uint64_t purple_pixels{};   // output pixels with red and blue both well above green
    bool have_sample{};              // first purple pixel of the current draw
    std::uint32_t sample_texel{};    // texture colour 0xAABBGGRR (0 if untextured)
    float sample_vr{}, sample_vg{}, sample_vb{};
    int sample_out_r{}, sample_out_g{}, sample_out_b{};
    float sample_u{}, sample_v{};
};

class GpuBackend {
public:
    GpuDiag diag;
    virtual ~GpuBackend() = default;
    // `count` vertices: points 1 each, lines 2 each, triangles 3 each,
    // sprites 2 each (top-left, bottom-right).
    virtual void draw(const GpuDraw &state, GpuPrim prim, const GpuVertex *vertices, std::size_t count) = 0;
    // Coherency hooks for backends that keep render targets outside emulated VRAM.  `vram_read` is called before
    // the CPU/GE reads a VRAM range (make memory current); `vram_write` after a non-draw write (memory is newer).
    virtual void vram_read(std::uint32_t /*address*/, std::uint32_t /*length*/) {}
    // Same, for data where a frame-old copy is acceptable (a palette the game renders for an effect): the backend may
    // hand back pixels from an earlier frame instead of waiting for the GPU.
    virtual void vram_read_late_ok(std::uint32_t address, std::uint32_t length) { vram_read(address, length); }
    // Diagnostics: name the next vram_read callers ("xfer" for block transfers, "ge" otherwise).
    virtual void set_read_reason(const char * /*reason*/) {}
    virtual void vram_write(std::uint32_t /*address*/, std::uint32_t /*length*/) {}
    // Called when the GE stops executing (list finished, stalled or paused): submit any batched work.
    virtual void sync() {}
};

} // namespace rr
