#include "rr_gl.hpp"
#include "rr_png.hpp"
#include "rr_tuning.hpp"
#include "rr_prof.hpp"

#include "psprecomp/guest_memory.hpp"

#include <algorithm>
#include <unordered_map>
#include <map>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#define RR_GLAPI __stdcall
#else
#define RR_GLAPI
#endif

namespace rr {

namespace {

using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;
using GLfloat = float;
using GLsizeiptr = std::ptrdiff_t;
using GLintptr = std::ptrdiff_t;

constexpr GLenum GL_TRIANGLES = 0x0004, GL_LINES = 0x0001, GL_POINTS = 0x0000;
constexpr GLenum GL_ARRAY_BUFFER = 0x8892, GL_STREAM_DRAW = 0x88E0;
constexpr GLenum GL_FLOAT = 0x1406, GL_UNSIGNED_BYTE = 0x1401;
constexpr GLenum GL_TEXTURE_2D = 0x0DE1, GL_TEXTURE0 = 0x84C0, GL_TEXTURE1 = 0x84C1;
constexpr GLenum GL_RGBA = 0x1908, GL_RGBA8 = 0x8058;
constexpr GLenum GL_NEAREST = 0x2600, GL_LINEAR = 0x2601;
constexpr GLenum GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_MAG_FILTER = 0x2800;
constexpr GLenum GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803;
constexpr GLenum GL_REPEAT = 0x2901, GL_CLAMP_TO_EDGE = 0x812F;
constexpr GLenum GL_FRAMEBUFFER = 0x8D40, GL_READ_FRAMEBUFFER = 0x8CA8, GL_DRAW_FRAMEBUFFER = 0x8CA9;
constexpr GLenum GL_COLOR_ATTACHMENT0 = 0x8CE0, GL_DEPTH_ATTACHMENT = 0x8D00, GL_RENDERBUFFER = 0x8D41;
constexpr GLenum GL_FRAMEBUFFER_COMPLETE = 0x8CD5;
constexpr GLenum GL_DEPTH_COMPONENT32F = 0x8CAC, GL_DEPTH32F_STENCIL8 = 0x8CAD, GL_DEPTH_STENCIL_ATTACHMENT = 0x821A;
constexpr GLenum GL_STENCIL_TEST = 0x0B90, GL_KEEP = 0x1E00, GL_REPLACE = 0x1E01, GL_INCR = 0x1E02, GL_DECR = 0x1E03, GL_INVERT = 0x150A;
constexpr GLenum GL_VERTEX_SHADER = 0x8B31, GL_FRAGMENT_SHADER = 0x8B30;
constexpr GLenum GL_COMPILE_STATUS = 0x8B81, GL_LINK_STATUS = 0x8B82;
constexpr GLenum GL_BLEND = 0x0BE2, GL_DEPTH_TEST = 0x0B71, GL_SCISSOR_TEST = 0x0C11, GL_CULL_FACE = 0x0B44;
constexpr GLenum GL_DITHER = 0x0BD0;
constexpr GLenum GL_ZERO = 0, GL_ONE = 1, GL_SRC_COLOR = 0x0300, GL_ONE_MINUS_SRC_COLOR = 0x0301, GL_SRC_ALPHA = 0x0302,
                 GL_ONE_MINUS_SRC_ALPHA = 0x0303, GL_DST_ALPHA = 0x0304, GL_ONE_MINUS_DST_ALPHA = 0x0305,
                 GL_DST_COLOR = 0x0306, GL_ONE_MINUS_DST_COLOR = 0x0307, GL_CONSTANT_COLOR = 0x8001, GL_CONSTANT_ALPHA = 0x8003;
constexpr GLenum GL_FUNC_ADD = 0x8006, GL_MIN = 0x8007, GL_MAX = 0x8008, GL_FUNC_SUBTRACT = 0x800A, GL_FUNC_REVERSE_SUBTRACT = 0x800B;
constexpr GLenum GL_NEVER = 0x0200, GL_LESS = 0x0201, GL_EQUAL = 0x0202, GL_LEQUAL = 0x0203, GL_GREATER = 0x0204,
                 GL_NOTEQUAL = 0x0205, GL_GEQUAL = 0x0206, GL_ALWAYS = 0x0207;
constexpr GLbitfield GL_COLOR_BUFFER_BIT = 0x4000, GL_DEPTH_BUFFER_BIT = 0x0100, GL_STENCIL_BUFFER_BIT = 0x0400;
constexpr GLenum GL_PACK_ALIGNMENT = 0x0D05, GL_UNPACK_ALIGNMENT = 0x0CF5;
constexpr GLenum GL_VERSION = 0x1F02, GL_RENDERER = 0x1F01;
constexpr GLenum GL_MAX_SAMPLES = 0x8D57;
constexpr GLenum GL_LINEAR_MIPMAP_LINEAR = 0x2703, GL_TEXTURE_MAX_ANISOTROPY = 0x84FE, GL_MAX_TEXTURE_MAX_ANISOTROPY = 0x84FF;

#define RR_GL_FUNCTIONS(X)                                                                                              \
    X(GLuint, CreateShader, (GLenum))                                                                                    \
    X(void, ShaderSource, (GLuint, GLsizei, const char *const *, const GLint *))                                         \
    X(void, CompileShader, (GLuint))                                                                                     \
    X(void, GetShaderiv, (GLuint, GLenum, GLint *))                                                                      \
    X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei *, char *))                                                      \
    X(void, DeleteShader, (GLuint))                                                                                      \
    X(GLuint, CreateProgram, ())                                                                                         \
    X(void, AttachShader, (GLuint, GLuint))                                                                              \
    X(void, BindAttribLocation, (GLuint, GLuint, const char *))                                                          \
    X(void, LinkProgram, (GLuint))                                                                                       \
    X(void, GetProgramiv, (GLuint, GLenum, GLint *))                                                                     \
    X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei *, char *))                                                     \
    X(void, UseProgram, (GLuint))                                                                                        \
    X(GLint, GetUniformLocation, (GLuint, const char *))                                                                 \
    X(void, Uniform1i, (GLint, GLint))                                                                                   \
    X(void, Uniform2f, (GLint, GLfloat, GLfloat))                                                                        \
    X(void, Uniform3f, (GLint, GLfloat, GLfloat, GLfloat))                                                               \
    X(void, GenBuffers, (GLsizei, GLuint *))                                                                             \
    X(void, BindBuffer, (GLenum, GLuint))                                                                                \
    X(void, BufferData, (GLenum, GLsizeiptr, const void *, GLenum))                                                      \
    X(void *, MapBufferRange, (GLenum, GLintptr, GLsizeiptr, GLbitfield))                                                \
    X(GLboolean, UnmapBuffer, (GLenum))                                                                                  \
    X(void, GenVertexArrays, (GLsizei, GLuint *))                                                                        \
    X(void, BindVertexArray, (GLuint))                                                                                   \
    X(void, EnableVertexAttribArray, (GLuint))                                                                           \
    X(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *))                              \
    X(void, DrawArrays, (GLenum, GLint, GLsizei))                                                                        \
    X(void, GenTextures, (GLsizei, GLuint *))                                                                            \
    X(void, DeleteTextures, (GLsizei, const GLuint *))                                                                   \
    X(void, BindTexture, (GLenum, GLuint))                                                                               \
    X(void, ActiveTexture, (GLenum))                                                                                     \
    X(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *))                   \
    X(void, TexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *))                \
    X(void, TexParameteri, (GLenum, GLenum, GLint))                                                                      \
    X(void, GenSamplers, (GLsizei, GLuint *))                                                                            \
    X(void, BindSampler, (GLuint, GLuint))                                                                               \
    X(void, SamplerParameteri, (GLuint, GLenum, GLint))                                                                  \
    X(void, SamplerParameterf, (GLuint, GLenum, GLfloat))                                                                \
    X(void, GenerateMipmap, (GLenum))                                                                                    \
    X(void, GetFloatv, (GLenum, GLfloat *))                                                                              \
    X(void, GetIntegerv, (GLenum, GLint *))                                                                              \
    X(void, RenderbufferStorageMultisample, (GLenum, GLsizei, GLenum, GLsizei, GLsizei))                                 \
    X(void, Uniform1f, (GLint, GLfloat))                                                                                 \
    X(void, GenFramebuffers, (GLsizei, GLuint *))                                                                        \
    X(void, DeleteFramebuffers, (GLsizei, const GLuint *))                                                               \
    X(void, BindFramebuffer, (GLenum, GLuint))                                                                           \
    X(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))                                               \
    X(void, FramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint))                                                   \
    X(GLenum, CheckFramebufferStatus, (GLenum))                                                                          \
    X(void, GenRenderbuffers, (GLsizei, GLuint *))                                                                       \
    X(void, DeleteRenderbuffers, (GLsizei, const GLuint *))                                                              \
    X(void, BindRenderbuffer, (GLenum, GLuint))                                                                          \
    X(void, RenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei))                                                     \
    X(void, Viewport, (GLint, GLint, GLsizei, GLsizei))                                                                  \
    X(void, Scissor, (GLint, GLint, GLsizei, GLsizei))                                                                   \
    X(void, Enable, (GLenum))                                                                                            \
    X(void, Disable, (GLenum))                                                                                           \
    X(void, BlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum))                                                         \
    X(void, BlendEquationSeparate, (GLenum, GLenum))                                                                     \
    X(void, BlendColor, (GLfloat, GLfloat, GLfloat, GLfloat))                                                            \
    X(void, ColorMask, (GLboolean, GLboolean, GLboolean, GLboolean))                                                     \
    X(void, DepthMask, (GLboolean))                                                                                      \
    X(void, DepthFunc, (GLenum))                                                                                         \
    X(void, StencilFunc, (GLenum, GLint, GLuint))                                                                        \
    X(void, StencilOp, (GLenum, GLenum, GLenum))                                                                         \
    X(void, StencilMask, (GLuint))                                                                                       \
    X(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))                                                            \
    X(void, Clear, (GLbitfield))                                                                                         \
    X(void, ReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))                                        \
    X(void, BlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum))               \
    X(void, PixelStorei, (GLenum, GLint))                                                                                \
    X(GLenum, GetError, ())                                                                                              \
    X(const unsigned char *, GetString, (GLenum))

const char *kVertexShader = R"(
layout(location = 0) in vec4 a_pos;   // x, y, z (0..65535), 1/w
layout(location = 1) in vec2 a_uv;    // texel units
layout(location = 2) in vec4 a_col;   // 0..255
layout(location = 3) in float a_fog;  // fog coefficient (1 = none)
layout(location = 4) in vec4 a_ub;    // sprite uv rect (sentinel = unbounded)
uniform vec2 u_rt;                    // render target size in PSP pixels
uniform vec2 u_pt;                    // x > 0: drawing points; x = internal scale (a point covers one PSP pixel)
uniform vec2 u_jit;                   // TAA sub-pixel jitter in PSP pixels (0 = none)
out vec2 v_uv;
out vec4 v_col;
out float v_iw;
out float v_fog;
flat out vec4 v_ub;
void main() {
    float iw = a_pos.w;
    vec2 p = a_pos.xy;
    if (u_pt.x > 0.0) p = floor(p) + 0.5;
    p += u_jit;
    float x = p.x / u_rt.x * 2.0 - 1.0;
    float y = p.y / u_rt.y * 2.0 - 1.0;
    float z = clamp(a_pos.z / 65535.0 * 2.0 - 1.0, -1.0, 1.0);
    gl_Position = vec4(x, y, z, 1.0);
    v_uv = a_uv * iw;
    v_col = a_col * iw;
    v_iw = iw;
    v_fog = a_fog * iw;
    v_ub = a_ub;
    gl_PointSize = u_pt.x > 0.0 ? u_pt.x : 1.0;
}
)";

const char *kFragmentShader = R"(
in vec2 v_uv;
in vec4 v_col;
in float v_iw;
in float v_fog;
flat in vec4 v_ub;
uniform int u_fog;
uniform vec3 u_fog_color;
uniform sampler2D u_tex;
uniform int u_clear;
uniform int u_tex_enable, u_tex_func, u_tex_alpha, u_tex_double;
uniform vec3 u_tex_env;
uniform vec2 u_tex_size;
uniform int u_rt_psp_filter;   // 1: a stretched-up render target is filtered between the PSP's own texels (glow buffers)
uniform vec2 u_tex_off;
uniform float u_ui_ramp;    // > 0: crisp magnification for 2D sprites - the blend between two texels is squeezed into this fraction of a texel
uniform float u_dst_scale;  // > 0: the texture is a scaled render target; shrinking draws average the whole source footprint
uniform int u_tex_snap;     // 1: nearest sampling of a scaled render target - read the centre of the texel's pixel block
uniform int u_atest, u_atest_func, u_atest_ref, u_atest_mask;
uniform vec3 u_dbg;
uniform int u_acut;   // blended fragments below this alpha (0-255) are dropped   // debug: tint draws by class (RR_DEBUG_CLASS)
uniform int u_ctest, u_ct_func, u_ct_ref, u_ct_mask;   // colour test (24-bit 0xBBGGRR)
uniform int u_alpha_mode;   // 0 as is, 1 = clamp(2a), 2 = clamp(1 - 2a); 3/4: alpha*mode1/2 (alpha-only blend pass)
out vec4 o_color;

bool cmp(int f, int a, int b) {
    if (f == 0) return false;
    if (f == 1) return true;
    if (f == 2) return a == b;
    if (f == 3) return a != b;
    if (f == 4) return a < b;
    if (f == 5) return a <= b;
    if (f == 6) return a > b;
    return a >= b;
}

void main() {
    float w = 1.0 / v_iw;
    vec2 uv = v_uv * w;
    vec4 c = v_col * w / 255.0;
    if (u_clear != 0) { o_color = clamp(c, 0.0, 1.0); return; }
    vec3 rgb = c.rgb;
    float a = c.a;
    if (u_tex_enable != 0) {
        vec2 tuv = u_tex_snap != 0 ? floor(uv) + 0.5 : uv;
        if (u_ui_ramp > 0.0 && u_tex_snap == 0) {
            // Text and icons are small bitmaps that bilinear filtering turns to mush when they are drawn large: keep
            // each texel's interior solid and blend only across a narrow band at its edge (never narrower than a pixel).
            vec2 r = clamp(max(vec2(u_ui_ramp), fwidth(uv)), vec2(0.02), vec2(1.0));
            vec2 q = uv - 0.5;
            vec2 qi = floor(q);
            tuv = qi + clamp((q - qi - 0.5) / r + 0.5, 0.0, 1.0) + 0.5;
        }
        // a sprite never samples beyond its own texel rect (scaled up, bilinear would pull in the atlas neighbours)
        if (v_ub.z - v_ub.x > 1.0 && v_ub.w - v_ub.y > 1.0 && v_ub.z < 1e29) tuv = clamp(tuv, v_ub.xy + 0.5, v_ub.zw - 0.5);
        vec4 t;
        vec2 fp = (u_dst_scale > 0.0 && u_tex_snap == 0) ? max(fwidth(uv) * u_dst_scale - 1.0, 0.0) : vec2(0.0);
        if (fp.x > 0.05 || fp.y > 0.05) {
            // a shrinking copy (the glow effect halves the picture several times): at a high render scale one bilinear tap
            // covers a fraction of the original pixel, so average a grid of taps over the destination pixel's footprint
            vec4 acc = vec4(0.0);
            vec2 lo = vec2(-1e9), hi = vec2(1e9);
            if (v_ub.z - v_ub.x > 1.0 && v_ub.w - v_ub.y > 1.0 && v_ub.z < 1e29) { lo = v_ub.xy + 0.5; hi = v_ub.zw - 0.5; }
            for (int j = 0; j < 4; ++j)
                for (int i = 0; i < 4; ++i) {
                    vec2 o = (vec2(float(i), float(j)) / 3.0 - 0.5) * fp;
                    acc += texture(u_tex, (clamp(tuv + o, lo, hi) + u_tex_off) / u_tex_size);
                }
            t = acc / 16.0;
        } else if (u_dst_scale > 0.0 && u_tex_snap == 0 && u_rt_psp_filter != 0 && all(lessThan(fwidth(uv) * u_dst_scale, vec2(0.5)))) {
            // a small buffer (the glow is 60x34) stretched up many times: at a high render scale the buffer is stored as flat
            // blocks, and filtering between those blocks would draw them as boxes. Filter between the PSP's own texels instead.
            vec2 lo = vec2(-1e9), hi = vec2(1e9);
            if (v_ub.z - v_ub.x > 1.0 && v_ub.w - v_ub.y > 1.0 && v_ub.z < 1e29) { lo = v_ub.xy + 0.5; hi = v_ub.zw - 0.5; }
            vec2 q = tuv - 0.5;
            vec2 i0 = floor(q);
            vec2 fr = q - i0;
            vec2 p0 = i0 + 0.5;
            vec4 t00 = textureLod(u_tex, (clamp(p0, lo, hi) + u_tex_off) / u_tex_size, 0.0);
            vec4 t10 = textureLod(u_tex, (clamp(p0 + vec2(1.0, 0.0), lo, hi) + u_tex_off) / u_tex_size, 0.0);
            vec4 t01 = textureLod(u_tex, (clamp(p0 + vec2(0.0, 1.0), lo, hi) + u_tex_off) / u_tex_size, 0.0);
            vec4 t11 = textureLod(u_tex, (clamp(p0 + vec2(1.0, 1.0), lo, hi) + u_tex_off) / u_tex_size, 0.0);
            t = mix(mix(t00, t10, fr.x), mix(t01, t11, fr.x), fr.y);
        } else {
            t = texture(u_tex, (tuv + u_tex_off) / u_tex_size);
        }
        if (u_tex_func == 0) {            // modulate
            rgb = t.rgb * rgb;
            if (u_tex_alpha != 0) a = t.a * a;
        } else if (u_tex_func == 1) {     // decal
            if (u_tex_alpha != 0) rgb = t.rgb * t.a + rgb * (1.0 - t.a);
            else rgb = t.rgb;
        } else if (u_tex_func == 2) {     // blend with environment colour
            rgb = rgb * (1.0 - t.rgb) + u_tex_env * t.rgb;
            if (u_tex_alpha != 0) a = t.a * a;
        } else if (u_tex_func == 3) {     // replace
            rgb = t.rgb;
            if (u_tex_alpha != 0) a = t.a;
        } else {                          // add
            rgb = min(rgb + t.rgb, 1.0);
            if (u_tex_alpha != 0) a = t.a * a;
        }
        if (u_tex_double != 0) rgb = min(rgb * 2.0, 1.0);
    }
    if (u_fog != 0) { float k = clamp(v_fog * w, 0.0, 1.0); rgb = mix(u_fog_color, clamp(rgb, 0.0, 1.0), k); }
    rgb = clamp(rgb, 0.0, 1.0);
    a = clamp(a, 0.0, 1.0);
    if (u_atest != 0) {
        int ia = int(a * 255.0 + 0.5);
        if (!cmp(u_atest_func, ia & u_atest_mask, u_atest_ref & u_atest_mask)) discard;
        if (ia < u_acut) discard;
    } else if (u_acut > 0) {
        if (int(a * 255.0 + 0.5) < u_acut) discard;   // 2D faint-blend cut (RR_HUD_FAINT_CUT)
    }
    if (u_ctest != 0) {
        ivec3 ic = ivec3(rgb * 255.0 + 0.5);
        int cc = (ic.r | (ic.g << 8) | (ic.b << 16)) & u_ct_mask;
        int cr = u_ct_ref & u_ct_mask;
        if (u_ct_func == 0 || (u_ct_func == 2 && cc != cr) || (u_ct_func == 3 && cc == cr)) discard;
    }
    if (u_dbg.x + u_dbg.y + u_dbg.z > 0.0) rgb = mix(rgb, u_dbg, 0.55);
    if (u_alpha_mode == 1) a = min(a * 2.0, 1.0);
    else if (u_alpha_mode == 2) a = clamp(1.0 - a * 2.0, 0.0, 1.0);
    else if (u_alpha_mode == 3) a = a * min(a * 2.0, 1.0);                 // alpha-only pass: src alpha times its own factor
    else if (u_alpha_mode == 4) a = a * clamp(1.0 - a * 2.0, 0.0, 1.0);
    o_color = vec4(rgb, a);
}
)";

const char *kPostVertexShader = R"(
uniform float u_vis;                      // visible fraction of the texture width (render targets are 512 wide, 480 shown)
out vec2 v_uv;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    v_uv = vec2(p.x * u_vis, 1.0 - p.y);  // texture row 0 is the top of the PSP image
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// FXAA (Lottes' compact variant) followed by an optional unsharp-mask sharpen, applied to the finished frame only.
const char *kPostFragmentShader = R"(
in vec2 v_uv;
uniform sampler2D u_src;
uniform vec2 u_texel;
uniform float u_fxaa;
uniform float u_sharp;
uniform float u_radius;     // sharpen radius in source texels
uniform float u_ss;         // > 1: supersampling downscale - average a grid of taps over each output pixel's footprint (source texels per output pixel)
uniform float u_xmax;       // last visible u (the columns beyond hold the game's palette strips, never to be sampled)
out vec4 o_color;
float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
vec3 tap(vec2 uv) { return texture(u_src, vec2(min(uv.x, u_xmax), uv.y)).rgb; }
void main() {
    vec3 m = tap(v_uv);
    if (u_ss > 1.001) {
        int n = int(clamp(ceil(u_ss), 2.0, 6.0));
        vec2 fp = u_texel * u_ss;
        vec3 acc = vec3(0.0);
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i)
                acc += tap(v_uv + ((vec2(float(i), float(j)) + 0.5) / float(n) - 0.5) * fp);
        m = acc / float(n * n);
    }
    vec3 col = m;
    vec3 nw = tap(v_uv + vec2(-1.0, -1.0) * u_texel);
    vec3 ne = tap(v_uv + vec2( 1.0, -1.0) * u_texel);
    vec3 sw = tap(v_uv + vec2(-1.0,  1.0) * u_texel);
    vec3 se = tap(v_uv + vec2( 1.0,  1.0) * u_texel);
    float lm = luma(m), lnw = luma(nw), lne = luma(ne), lsw = luma(sw), lse = luma(se);
    float lmin = min(lm, min(min(lnw, lne), min(lsw, lse)));
    float lmax = max(lm, max(max(lnw, lne), max(lsw, lse)));
    if (u_fxaa > 0.5) {
        vec2 dir = vec2(-((lnw + lne) - (lsw + lse)), (lnw + lsw) - (lne + lse));
        float reduce = max((lnw + lne + lsw + lse) * (0.25 / 8.0), 1.0 / 128.0);
        float rcp = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
        dir = clamp(dir * rcp, vec2(-8.0), vec2(8.0)) * u_texel;
        vec3 a = 0.5 * (tap(v_uv + dir * (1.0 / 3.0 - 0.5)) + tap(v_uv + dir * (2.0 / 3.0 - 0.5)));
        vec3 b = a * 0.5 + 0.25 * (tap(v_uv + dir * -0.5) + tap(v_uv + dir * 0.5));
        float lb = luma(b);
        col = (lb < lmin || lb > lmax) ? a : b;
    }
    if (u_sharp > 0.0) {
        // Bring back texture detail, but leave hard edges (geometry silhouettes) to the anti-aliasing: the sharpening
        // fades out where the local contrast is high, otherwise it would re-sharpen the stair-steps FXAA just smoothed.
        vec2 r = u_texel * u_radius;
        vec3 blur = 0.25 * (tap(v_uv + vec2(r.x, 0.0)) + tap(v_uv - vec2(r.x, 0.0)) + tap(v_uv + vec2(0.0, r.y)) + tap(v_uv - vec2(0.0, r.y)));
        float k = u_sharp * (1.0 - smoothstep(0.10, 0.35, lmax - lmin));
        col += k * (m - blur);
    }
    o_color = vec4(clamp(col, 0.0, 1.0), 1.0);
}
)";

// Temporal AA resolve.  The 3D geometry is rendered with a sub-pixel jitter that changes every frame; here the new frame is
// blended into an accumulated history.  There are no motion vectors in a PSP game, so the history is only trusted where it
// agrees with the new frame's own neighbourhood (variance clipping): moving content falls back to the new frame, static
// content converges to a smooth, anti-aliased image.
const char *kTaaVertexShader = R"(
out vec2 v_uv;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    v_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char *kTaaFragmentShader = R"(
in vec2 v_uv;
uniform sampler2D u_cur;
uniform sampler2D u_hist;
uniform vec2 u_texel;
uniform float u_blend;
uniform float u_reset;
out vec4 o_color;
void main() {
    vec3 c = texture(u_cur, v_uv).rgb;
    if (u_reset > 0.5) { o_color = vec4(c, 1.0); return; }
    vec3 m1 = vec3(0.0), m2 = vec3(0.0), mn = vec3(1e9), mx = vec3(-1e9);
    for (int j = -1; j <= 1; ++j)
        for (int i = -1; i <= 1; ++i) {
            vec3 s = texture(u_cur, v_uv + vec2(float(i), float(j)) * u_texel).rgb;
            m1 += s; m2 += s * s; mn = min(mn, s); mx = max(mx, s);
        }
    m1 /= 9.0; m2 /= 9.0;
    vec3 sigma = sqrt(max(m2 - m1 * m1, vec3(0.0)));
    vec3 lo = max(mn, m1 - 1.5 * sigma), hi = min(mx, m1 + 1.5 * sigma);
    lo = min(lo, c); hi = max(hi, c);
    vec3 h = clamp(texture(u_hist, v_uv).rgb, lo, hi);
    o_color = vec4(mix(h, c, u_blend), 1.0);
}
)";

std::uint64_t hash_bytes(const std::uint8_t *p, std::size_t n, std::uint64_t h) {
    std::size_t i = 0;
    for (; i + 8u <= n; i += 8u) {
        std::uint64_t v;
        std::memcpy(&v, p + i, 8);
        h = (h ^ v) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 32u;
    }
    for (; i < n; ++i) { h = (h ^ p[i]) * 0x100000001B3ull; }
    return h;
}

std::uint32_t texel_bits_of(std::uint32_t format) {
    switch (format) {
    case 0: case 1: case 2: case 6: return 16;
    case 3: case 7: return 32;
    case 4: return 4;
    case 5: return 8;
    default: return 0;
    }
}

bool vis_log_enabled() { static const bool on = std::getenv("RR_VIS_LOG") != nullptr; return on; }

bool ranges_overlap(std::uint32_t a, std::uint32_t alen, std::uint32_t b, std::uint32_t blen) {
    return a < b + blen && b < a + alen;
}

// Fully transparent texels keep whatever colour the artist left behind, usually black. Smoothing (bilinear, and above all the
// mipmaps added for high resolution) blends that colour into the visible edge, so leaves and other cut-out sprites get dark
// halos and boxes. Give those texels the colour of their nearest visible neighbours instead; their alpha stays 0.
void bleed_transparent_texels(std::vector<std::uint32_t> &px, std::uint32_t w, std::uint32_t h) {
    const std::size_t n = static_cast<std::size_t>(w) * h;
    if (w == 0u || h == 0u || px.size() < n) return;
    std::vector<std::uint8_t> filled(n);
    std::size_t hidden = 0;
    for (std::size_t i = 0; i < n; ++i) { filled[i] = (px[i] >> 24u) != 0u; hidden += filled[i] ? 0u : 1u; }
    if (hidden == 0u || hidden == n) return;
    if ((n - hidden) * 32u >= hidden) {
        // Not very sparse: scanning the whole image per pass is cheaper than tracking a frontier.
        std::vector<std::uint8_t> next;
        for (int pass = 0; pass < 8 && hidden > 0u; ++pass) {
            next = filled;
            std::size_t done = 0;
            for (std::uint32_t y = 0; y < h; ++y) {
                for (std::uint32_t x = 0; x < w; ++x) {
                    const std::size_t i = static_cast<std::size_t>(y) * w + x;
                    if (filled[i]) continue;
                    std::uint32_t r = 0, gr = 0, b = 0, cnt = 0;
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int nx = static_cast<int>(x) + dx, ny = static_cast<int>(y) + dy;
                            if (nx < 0 || ny < 0 || nx >= static_cast<int>(w) || ny >= static_cast<int>(h)) continue;
                            const std::size_t j = static_cast<std::size_t>(ny) * w + static_cast<std::size_t>(nx);
                            if (!filled[j]) continue;
                            r += px[j] & 255u; gr += (px[j] >> 8u) & 255u; b += (px[j] >> 16u) & 255u; ++cnt;
                        }
                    }
                    if (cnt == 0u) continue;
                    px[i] = (r / cnt) | ((gr / cnt) << 8u) | ((b / cnt) << 16u);   // alpha stays 0
                    next[i] = 1; ++done;
                }
            }
            filled.swap(next);
            hidden -= done;
            if (done == 0u) break;
        }
        return;
    }
    // Each pass gives every hidden texel that touches a filled one the average colour of its filled neighbours (alpha stays
    // 0), so linear filtering at a cut-out edge does not pull in the hidden texels' colour.  Only 8 passes are made.  The
    // first pass looks at every hidden texel; later passes only at the hidden neighbours of the texels filled by the pass
    // before (a frontier), which gives the same result as scanning the whole image again but costs far less on mostly
    // transparent textures (the sun glare ramp took ~14 ms per frame here).
    std::vector<std::size_t> frontier;       // texels filled by the previous pass
    std::vector<std::size_t> cand;           // hidden texels to evaluate this pass
    std::vector<std::uint32_t> value;
    std::vector<std::uint8_t> queued(n, 0);
    const auto average = [&](std::size_t i, std::uint32_t &out) {
        const int x = static_cast<int>(i % w), y = static_cast<int>(i / w);
        std::uint32_t r = 0, gr = 0, b = 0, cnt = 0;
        for (int dy = -1; dy <= 1; ++dy) {
            const int ny = y + dy;
            if (ny < 0 || ny >= static_cast<int>(h)) continue;
            for (int dx = -1; dx <= 1; ++dx) {
                const int nx = x + dx;
                if (nx < 0 || nx >= static_cast<int>(w)) continue;
                const std::size_t j = static_cast<std::size_t>(ny) * w + static_cast<std::size_t>(nx);
                if (!filled[j]) continue;
                r += px[j] & 255u; gr += (px[j] >> 8u) & 255u; b += (px[j] >> 16u) & 255u; ++cnt;
            }
        }
        if (cnt == 0u) return false;
        out = (r / cnt) | ((gr / cnt) << 8u) | ((b / cnt) << 16u);
        return true;
    };
    const auto add_neighbours = [&](std::size_t i) {
        const int x = static_cast<int>(i % w), y = static_cast<int>(i / w);
        for (int dy = -1; dy <= 1; ++dy) {
            const int ny = y + dy;
            if (ny < 0 || ny >= static_cast<int>(h)) continue;
            for (int dx = -1; dx <= 1; ++dx) {
                const int nx = x + dx;
                if (nx < 0 || nx >= static_cast<int>(w)) continue;
                const std::size_t j = static_cast<std::size_t>(ny) * w + static_cast<std::size_t>(nx);
                if (!filled[j] && !queued[j]) { queued[j] = 1; cand.push_back(j); }
            }
        }
    };
    // Pass 1: the hidden texels that touch a filled one.  Few filled texels (a sparse sprite): start from them; otherwise
    // test every hidden texel.
    for (std::size_t i = 0; i < n; ++i) if (filled[i]) add_neighbours(i);
    for (const std::size_t j : cand) queued[j] = 0;
    for (int pass = 0; pass < 8 && !cand.empty(); ++pass) {
        value.assign(cand.size(), 0u);
        std::vector<std::size_t> done;
        done.reserve(cand.size() / 4u + 16u);
        std::vector<std::uint8_t> ok(cand.size(), 0);
        for (std::size_t k = 0; k < cand.size(); ++k) {
            std::uint32_t c;
            if (average(cand[k], c)) { value[k] = c; ok[k] = 1; }
        }
        for (std::size_t k = 0; k < cand.size(); ++k) {
            if (!ok[k]) continue;
            const std::size_t i = cand[k];
            px[i] = value[k];                 // alpha stays 0
            filled[i] = 1;
            done.push_back(i);
        }
        if (done.empty()) break;
        // Next pass: hidden neighbours of what was just filled.
        cand.clear();
        for (const std::size_t i : done) add_neighbours(i);
        for (const std::size_t j : cand) queued[j] = 0;
    }
}

} // namespace

struct GlBackend::Gl {
#define X(ret, name, args) ret(RR_GLAPI *name) args = nullptr;
    RR_GL_FUNCTIONS(X)
#undef X
    // optional (GL 3.2 / GLES 3.0 sync objects): readbacks that may use old pixels never wait for the GPU
    void *(RR_GLAPI *FenceSync)(GLenum, GLbitfield) = nullptr;
    GLenum(RR_GLAPI *ClientWaitSync)(void *, GLbitfield, unsigned long long) = nullptr;
    void(RR_GLAPI *DeleteSync)(void *) = nullptr;
    unsigned program{}, vao{}, vbo{};
    int loc_rt{-1}, loc_pt{-1}, loc_clear{-1}, loc_tex_enable{-1}, loc_tex_func{-1}, loc_tex_alpha{-1}, loc_tex_double{-1}, loc_tex_env{-1},
        loc_tex_size{-1}, loc_tex_off{-1}, loc_tex_snap{-1}, loc_dst_scale{-1}, loc_rt_psp_filter{-1}, loc_ui_ramp{-1}, loc_dbg{-1}, loc_acut{-1}, loc_atest{-1}, loc_atest_func{-1}, loc_atest_ref{-1}, loc_atest_mask{-1}, loc_ctest{-1}, loc_ct_func{-1}, loc_ct_ref{-1}, loc_ct_mask{-1}, loc_alpha_mode{-1}, loc_tex{-1}, loc_fog{-1}, loc_fog_color{-1};
    unsigned samplers[2][4]{};  // [filter linear][clamp_u + 2*clamp_v]
    unsigned mip_samplers[4]{}; // trilinear (+ anisotropic) for 3D textures
    bool mip_ready{false};
    float max_aniso{1.0f};
    unsigned post_program{}, post_vao{};
    int post_src{-1}, post_texel{-1}, post_fxaa{-1}, post_sharp{-1}, post_radius{-1}, post_vis{-1}, post_xmax{-1}, post_ss{-1};
    int loc_jit{-1};
    // temporal AA
    unsigned taa_prog{}, taa_tex[2]{}, taa_fbo[2]{};
    int taa_w{}, taa_h{}, taa_cur{};
    bool taa_hist_ok{};
    int taa_loc_cur{-1}, taa_loc_hist{-1}, taa_loc_texel{-1}, taa_loc_blend{-1}, taa_loc_reset{-1};
    unsigned scratch_tex{}, scratch_fbo{}, snap_tex{}, snap_fbo{};
    int snap_w{}, snap_h{};
    int scratch_w{}, scratch_h{};
    unsigned rb_tex{}, rb_fbo{};   // 1x-size copy of a scaled render target: reading that back moves scale^2 times less data
    int rb_w{}, rb_h{};
    unsigned up_tex{}, up_fbo{};   // native-size staging copy used to upload CPU-written pixels, then blitted up to the scaled target
    int up_w{}, up_h{};
};

GlBackend::GlBackend(psprecomp::GuestMemory &memory, GetProc get_proc, bool gles, int scale)
    : memory_(memory), gl_(std::make_unique<Gl>()), sw_(memory), gles_(gles), scale_(std::max(1, std::min(scale, 8))) {
    if (const char *e = std::getenv("RR_GL_EVENTS")) events_budget_ = static_cast<std::uint32_t>(std::max(0, std::atoi(e)));   // diagnostic line budget (default 3000)
    if (const char *e = std::getenv("RR_GPU_VERIFY")) verify_every_ = static_cast<std::uint32_t>(std::max(0, std::atoi(e)));
    if (const char *e = std::getenv("RR_GPU_VERIFY_SMALL")) { verify_every_ = static_cast<std::uint32_t>(std::max(0, std::atoi(e))); verify_small_only_ = true; }   // only the small (non-screen) targets
    if (get_proc == nullptr) { error_ = "no GL loader"; return; }
    bool missing = false;
#define X(ret, name, args)                                                                                              \
    gl_->name = reinterpret_cast<ret(RR_GLAPI *) args>(get_proc("gl" #name));                                           \
    if (gl_->name == nullptr) { error_ += " gl" #name; missing = true; }
    RR_GL_FUNCTIONS(X)
#undef X
    if (missing) { error_ = "missing GL functions:" + error_; return; }
    gl_->FenceSync = reinterpret_cast<void *(RR_GLAPI *)(GLenum, GLbitfield)>(get_proc("glFenceSync"));
    gl_->ClientWaitSync = reinterpret_cast<GLenum(RR_GLAPI *)(void *, GLbitfield, unsigned long long)>(get_proc("glClientWaitSync"));
    gl_->DeleteSync = reinterpret_cast<void(RR_GLAPI *)(void *)>(get_proc("glDeleteSync"));
    if (!compile_programs()) return;
    compile_post();   // optional: without it the screen is presented with a plain filtered blit

    Gl &g = *gl_;
    g.GenVertexArrays(1, &g.vao);
    g.BindVertexArray(g.vao);
    g.GenBuffers(1, &g.vbo);
    g.BindBuffer(GL_ARRAY_BUFFER, g.vbo);
    const GLsizei stride = static_cast<GLsizei>(sizeof(GpuVertex));
    g.EnableVertexAttribArray(0);
    g.VertexAttribPointer(0, 4, GL_FLOAT, 0, stride, reinterpret_cast<const void *>(offsetof(GpuVertex, x)));
    g.EnableVertexAttribArray(1);
    g.VertexAttribPointer(1, 2, GL_FLOAT, 0, stride, reinterpret_cast<const void *>(offsetof(GpuVertex, u)));
    g.EnableVertexAttribArray(2);
    g.VertexAttribPointer(2, 4, GL_FLOAT, 0, stride, reinterpret_cast<const void *>(offsetof(GpuVertex, r)));
    g.EnableVertexAttribArray(3);
    g.VertexAttribPointer(3, 1, GL_FLOAT, 0, stride, reinterpret_cast<const void *>(offsetof(GpuVertex, fog)));
    g.EnableVertexAttribArray(4);
    g.VertexAttribPointer(4, 4, GL_FLOAT, 0, stride, reinterpret_cast<const void *>(offsetof(GpuVertex, ub)));
    // x, y, z, inv_w are the first four floats of GpuVertex, in that order.
    static_assert(offsetof(GpuVertex, x) == 0 && offsetof(GpuVertex, inv_w) == 3 * sizeof(float), "vertex layout");

    g.GenSamplers(8, &g.samplers[0][0]);
    for (int linear = 0; linear < 2; ++linear)
        for (int clamp = 0; clamp < 4; ++clamp) {
            const unsigned s = g.samplers[linear][clamp];
            g.SamplerParameteri(s, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
            g.SamplerParameteri(s, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
            g.SamplerParameteri(s, GL_TEXTURE_WRAP_S, (clamp & 1) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
            g.SamplerParameteri(s, GL_TEXTURE_WRAP_T, (clamp & 2) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        }
    ok_ = g.GetError() == 0u;
    if (!ok_) error_ = "GL error during setup";
}

GlBackend::~GlBackend() {
    if (!gl_ || !gl_->DeleteTextures) return;
    for (auto &rt : rts_) destroy_rt(*rt);
    for (auto &[key, t] : textures_) gl_->DeleteTextures(1, &t.tex);
}

bool GlBackend::compile_programs() {
    Gl &g = *gl_;
    const std::string header = gles_ ? "#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\n"
                                     : "#version 330 core\n";
    const auto compile = [&](GLenum type, const char *body, unsigned &out) {
        const std::string src = header + body;
        const char *ptr = src.c_str();
        out = g.CreateShader(type);
        g.ShaderSource(out, 1, &ptr, nullptr);
        g.CompileShader(out);
        GLint status = 0;
        g.GetShaderiv(out, GL_COMPILE_STATUS, &status);
        if (!status) {
            char log[2048]{};
            g.GetShaderInfoLog(out, sizeof log - 1, nullptr, log);
            error_ = std::string(type == GL_VERTEX_SHADER ? "vertex" : "fragment") + " shader: " + log;
            return false;
        }
        return true;
    };
    unsigned vs = 0, fs = 0;
    if (!compile(GL_VERTEX_SHADER, kVertexShader, vs) || !compile(GL_FRAGMENT_SHADER, kFragmentShader, fs)) return false;
    g.program = g.CreateProgram();
    g.AttachShader(g.program, vs);
    g.AttachShader(g.program, fs);
    g.BindAttribLocation(g.program, 0, "a_pos");
    g.BindAttribLocation(g.program, 1, "a_uv");
    g.BindAttribLocation(g.program, 2, "a_col");
    g.LinkProgram(g.program);
    GLint status = 0;
    g.GetProgramiv(g.program, GL_LINK_STATUS, &status);
    if (!status) {
        char log[2048]{};
        g.GetProgramInfoLog(g.program, sizeof log - 1, nullptr, log);
        error_ = std::string("link: ") + log;
        return false;
    }
    g.DeleteShader(vs);
    g.DeleteShader(fs);
    const auto loc = [&](const char *name) { return g.GetUniformLocation(g.program, name); };
    g.loc_rt = loc("u_rt"); g.loc_pt = loc("u_pt"); g.loc_clear = loc("u_clear"); g.loc_tex_enable = loc("u_tex_enable"); g.loc_tex_func = loc("u_tex_func");
    g.loc_tex_alpha = loc("u_tex_alpha"); g.loc_tex_double = loc("u_tex_double"); g.loc_tex_env = loc("u_tex_env");
    g.loc_tex_size = loc("u_tex_size"); g.loc_tex_off = loc("u_tex_off"); g.loc_tex_snap = loc("u_tex_snap"); g.loc_dst_scale = loc("u_dst_scale"); g.loc_rt_psp_filter = loc("u_rt_psp_filter"); g.loc_ui_ramp = loc("u_ui_ramp"); g.loc_dbg = loc("u_dbg"); g.loc_acut = loc("u_acut"); g.loc_atest = loc("u_atest"); g.loc_atest_func = loc("u_atest_func");
    g.loc_ctest = loc("u_ctest"); g.loc_ct_func = loc("u_ct_func"); g.loc_ct_ref = loc("u_ct_ref"); g.loc_ct_mask = loc("u_ct_mask");
    g.loc_atest_ref = loc("u_atest_ref"); g.loc_atest_mask = loc("u_atest_mask"); g.loc_alpha_mode = loc("u_alpha_mode"); g.loc_fog = loc("u_fog"); g.loc_fog_color = loc("u_fog_color");
    g.loc_tex = loc("u_tex"); g.loc_jit = loc("u_jit");
    return true;
}

namespace { void letterbox(int win_w, int win_h, int &x, int &y, int &w, int &h); }

bool GlBackend::compile_post() {
    Gl &g = *gl_;
    const std::string header = gles_ ? "#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\n" : "#version 330 core\n";
    const auto compile = [&](GLenum type, const char *body, unsigned &out) {
        const std::string src = header + body;
        const char *ptr = src.c_str();
        out = g.CreateShader(type);
        g.ShaderSource(out, 1, &ptr, nullptr);
        g.CompileShader(out);
        GLint status = 0;
        g.GetShaderiv(out, GL_COMPILE_STATUS, &status);
        if (!status) {
            char log[1024]{};
            g.GetShaderInfoLog(out, sizeof log - 1, nullptr, log);
            events_.push_back(std::string("post-process shader: ") + log);
        }
        return status != 0;
    };
    unsigned vs = 0, fs = 0;
    if (!compile(GL_VERTEX_SHADER, kPostVertexShader, vs) || !compile(GL_FRAGMENT_SHADER, kPostFragmentShader, fs)) return false;
    g.post_program = g.CreateProgram();
    g.AttachShader(g.post_program, vs);
    g.AttachShader(g.post_program, fs);
    g.LinkProgram(g.post_program);
    GLint status = 0;
    g.GetProgramiv(g.post_program, GL_LINK_STATUS, &status);
    g.DeleteShader(vs);
    g.DeleteShader(fs);
    if (!status) { g.post_program = 0; return false; }
    g.post_src = g.GetUniformLocation(g.post_program, "u_src");
    g.post_texel = g.GetUniformLocation(g.post_program, "u_texel");
    g.post_fxaa = g.GetUniformLocation(g.post_program, "u_fxaa");
    g.post_sharp = g.GetUniformLocation(g.post_program, "u_sharp");
    g.post_radius = g.GetUniformLocation(g.post_program, "u_radius");
    g.post_vis = g.GetUniformLocation(g.post_program, "u_vis");
    g.post_xmax = g.GetUniformLocation(g.post_program, "u_xmax");
    g.post_ss = g.GetUniformLocation(g.post_program, "u_ss");
    g.GenVertexArrays(1, &g.post_vao);
    // Temporal anti-aliasing resolve (optional: without it the TAA mode just shows the plain image).
    {
        unsigned tvs = 0, tfs = 0;
        if (compile(GL_VERTEX_SHADER, kTaaVertexShader, tvs) && compile(GL_FRAGMENT_SHADER, kTaaFragmentShader, tfs)) {
            g.taa_prog = g.CreateProgram();
            g.AttachShader(g.taa_prog, tvs);
            g.AttachShader(g.taa_prog, tfs);
            g.LinkProgram(g.taa_prog);
            GLint tstatus = 0;
            g.GetProgramiv(g.taa_prog, GL_LINK_STATUS, &tstatus);
            if (!tstatus) g.taa_prog = 0;
            else {
                g.taa_loc_cur = g.GetUniformLocation(g.taa_prog, "u_cur");
                g.taa_loc_hist = g.GetUniformLocation(g.taa_prog, "u_hist");
                g.taa_loc_texel = g.GetUniformLocation(g.taa_prog, "u_texel");
                g.taa_loc_blend = g.GetUniformLocation(g.taa_prog, "u_blend");
                g.taa_loc_reset = g.GetUniformLocation(g.taa_prog, "u_reset");
            }
        }
        if (tvs) g.DeleteShader(tvs);
        if (tfs) g.DeleteShader(tfs);
    }
    return true;
}

void GlBackend::set_enhance(const Enhance &e) {
    enhance_ = e;
    if (!ok_) return;
    Gl &g = *gl_;
    if (!g.mip_ready) {
        g.GetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &g.max_aniso);
        if (g.GetError() != 0u || g.max_aniso < 1.0f) g.max_aniso = 1.0f;   // extension missing: plain trilinear
        g.GenSamplers(4, g.mip_samplers);
        for (int clamp = 0; clamp < 4; ++clamp) {
            const unsigned s = g.mip_samplers[clamp];
            g.SamplerParameteri(s, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            g.SamplerParameteri(s, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            g.SamplerParameteri(s, GL_TEXTURE_WRAP_S, (clamp & 1) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
            g.SamplerParameteri(s, GL_TEXTURE_WRAP_T, (clamp & 2) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        }
        g.mip_ready = true;
    }
    g_aniso = std::clamp(e.aniso, 1, 16);
    if (g.max_aniso > 1.0f) {
        const float level = std::min(g.max_aniso, static_cast<float>(std::max(1, e.aniso)));
        for (int clamp = 0; clamp < 4; ++clamp) g.SamplerParameterf(g.mip_samplers[clamp], GL_TEXTURE_MAX_ANISOTROPY, level);
        (void)g.GetError();
    }
    if (e.msaa > 1 && msaa_ == 0) {
        GLint maxs = 0;
        g.GetIntegerv(GL_MAX_SAMPLES, &maxs);
        (void)g.GetError();
        if (gles_) {
            events_.emplace_back("MSAA: not available in the OpenGL ES build (it needs single->multisample blits); ignored");
        } else if (maxs < 2) {
            events_.emplace_back("MSAA: the graphics driver offers no multisampling; ignored");
        } else {
            msaa_ = std::min<int>(e.msaa, maxs);
            char mb[100];
            std::snprintf(mb, sizeof mb, "MSAA: %dx (driver maximum %d)", msaa_, static_cast<int>(maxs));
            events_.emplace_back(mb);
        }
    }
    g_msaa_samples = msaa_;
    char b[200];
    std::snprintf(b, sizeof b, "enhancements: aa_mode=%d msaa=%d sharpen=%.2f mipmaps=%d anisotropy=%.0fx smooth_cutouts=%d%s", g_aa_mode.load(), msaa_, static_cast<double>(e.sharpen),
                  e.mipmaps ? 1 : 0, e.mipmaps ? static_cast<double>(std::min(g.max_aniso, static_cast<float>(std::max(1, e.aniso)))) : 1.0, e.smooth_cutouts ? 1 : 0, g.post_program == 0u ? " (post-process shader unavailable)" : "");
    events_.emplace_back(b);
}

bool GlBackend::post_wanted(bool cpu_frame) const {
    if (gl_->post_program == 0u) return false;
    const int m = g_aa_mode.load(std::memory_order_relaxed);
    if (enhance_.sharpen > 0.0f) return true;
    if (m == 1 || m == 3) return true;
    return m == 2 && !cpu_frame && gl_->taa_prog != 0u;   // TAA output goes through the post pass
}

unsigned GlBackend::taa_pass(Rt &rt) {
    Gl &g = *gl_;
    if (g.taa_prog == 0u) return rt.tex;
    if (g.taa_tex[0] == 0u || g.taa_w != rt.w || g.taa_h != rt.h) {
        for (int i = 0; i < 2; ++i) {
            if (g.taa_fbo[i]) g.DeleteFramebuffers(1, &g.taa_fbo[i]);
            if (g.taa_tex[i]) g.DeleteTextures(1, &g.taa_tex[i]);
            g.GenTextures(1, &g.taa_tex[i]);
            g.BindTexture(GL_TEXTURE_2D, g.taa_tex[i]);
            g.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            g.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            g.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            g.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            g.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, rt.w, rt.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            g.GenFramebuffers(1, &g.taa_fbo[i]);
            g.BindFramebuffer(GL_FRAMEBUFFER, g.taa_fbo[i]);
            g.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g.taa_tex[i], 0);
        }
        g.taa_w = rt.w; g.taa_h = rt.h; g.taa_cur = 0; g.taa_hist_ok = false;
    }
    if (taa_frame_ == frame_) return g.taa_tex[g.taa_cur];   // already blended for this frame (the screen was presented twice)
    taa_frame_ = frame_;
    const int out = g.taa_cur ^ 1;
    g.Disable(GL_SCISSOR_TEST);
    g.Disable(GL_BLEND);
    g.Disable(GL_DEPTH_TEST);
    g.Disable(GL_STENCIL_TEST);
    g.ColorMask(1, 1, 1, 1);
    g.BindFramebuffer(GL_FRAMEBUFFER, g.taa_fbo[out]);
    g.Viewport(0, 0, rt.w, rt.h);
    g.UseProgram(g.taa_prog);
    g.BindVertexArray(g.post_vao);
    g.BindSampler(0, 0u);
    g.BindSampler(1, 0u);
    g.ActiveTexture(GL_TEXTURE0);
    g.BindTexture(GL_TEXTURE_2D, rt.tex);
    g.ActiveTexture(GL_TEXTURE1);
    g.BindTexture(GL_TEXTURE_2D, g.taa_tex[g.taa_cur]);
    g.ActiveTexture(GL_TEXTURE0);
    g.Uniform1i(g.taa_loc_cur, 0);
    g.Uniform1i(g.taa_loc_hist, 1);
    g.Uniform2f(g.taa_loc_texel, 1.0f / static_cast<float>(rt.w), 1.0f / static_cast<float>(rt.h));
    g.Uniform1f(g.taa_loc_blend, std::clamp(static_cast<float>(g_taa_blend.load(std::memory_order_relaxed)) / 100.0f, 0.02f, 1.0f));
    g.Uniform1f(g.taa_loc_reset, g.taa_hist_ok ? 0.0f : 1.0f);
    g.DrawArrays(GL_TRIANGLES, 0, 3);
    g.taa_cur = out;
    g.taa_hist_ok = true;
    return g.taa_tex[out];
}

// Draws `tex` (top row first, source_w x source_h texels) into the letterboxed area of the default framebuffer.
void GlBackend::post_pass(unsigned tex, int src_w, int src_h, int vis_w, int win_w, int win_h) {
    Gl &g = *gl_;
    int lx, ly, lw, lh;
    letterbox(win_w, win_h, lx, ly, lw, lh);
    g.Disable(GL_SCISSOR_TEST);
    g.ColorMask(1, 1, 1, 1);
    g.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    g.Disable(GL_STENCIL_TEST);
    g.Disable(GL_BLEND);
    g.Disable(GL_DEPTH_TEST);
    g.Viewport(0, 0, win_w, win_h);
    g.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    g.Clear(GL_COLOR_BUFFER_BIT);
    g.Viewport(lx, ly, lw, lh);
    g.UseProgram(g.post_program);
    g.BindVertexArray(g.post_vao);
    g.ActiveTexture(GL_TEXTURE0);
    g.BindTexture(GL_TEXTURE_2D, tex);
    g.BindSampler(0, 0u);   // use the texture's own (linear, clamped) parameters
    g.Uniform1i(g.post_src, 0);
    g.Uniform2f(g.post_texel, 1.0f / static_cast<float>(src_w), 1.0f / static_cast<float>(src_h));
    const int aa_mode = g_aa_mode.load(std::memory_order_relaxed);
    g.Uniform1f(g.post_fxaa, aa_mode == 1 ? 1.0f : 0.0f);
    // SSAA filter: how many source pixels land in one output pixel (only meaningful when the frame is rendered larger than shown)
    g.Uniform1f(g.post_ss, aa_mode == 3 && lw > 0 ? static_cast<float>(vis_w) / static_cast<float>(lw) : 0.0f);
    g.Uniform1f(g.post_sharp, std::max(0.0f, enhance_.sharpen));
    const float vis = static_cast<float>(vis_w) / static_cast<float>(src_w);
    g.Uniform1f(g.post_vis, vis);
    g.Uniform1f(g.post_xmax, vis - 0.5f / static_cast<float>(src_w));
    g.Uniform1f(g.post_radius, std::max(1.0f, static_cast<float>(vis_w) / 480.0f * 0.6f));
    g.DrawArrays(GL_TRIANGLES, 0, 3);
}

bool GlBackend::gl_target(const GpuDraw &d) const {
    static const bool only_screen = std::getenv("RR_NO_SMALL_TARGETS") != nullptr;   // A/B switch for debugging
    static const bool no_short_rt = std::getenv("RR_NO_SHORT_RT") != nullptr;
    if (d.fb_stride < 32u || d.fb_stride > 512u || (d.fb_stride & 7u) != 0u || d.fb_addr < 0x04000000u || d.fb_addr >= 0x04200000u) return false;
    if (only_screen && d.fb_stride != 512u) return false;
    // Only buffers that fit in VRAM completely.  A buffer that runs off the end of VRAM is not contiguous in host
    // memory; the software renderer drops draws to it, and so do we (keep the two identical).
    if (d.fb_format > 3u || rows_for(d.fb_addr, d.fb_stride, d.fb_format) < (no_short_rt ? 272u : 1u)) return false;
    {
        const auto partial = [](std::uint32_t m, int sh) { const std::uint32_t c = (m >> sh) & 0xFFu; return c != 0u && c != 0xFFu; };
        if (partial(d.mask_rgb, 0) || partial(d.mask_rgb, 8) || partial(d.mask_rgb, 16) || (d.mask_alpha != 0u && d.mask_alpha != 0xFFu)) return false;
    }
    if (d.blend && !d.clear_mode) {
        // Combinations GL fixed-function blending cannot express go to the software path.
        const std::uint32_t s = d.blend_src, t = d.blend_dst;
        if (d.blend_eq > 2u || s == 8u || s == 9u || t == 8u || t == 9u) return false;
        const auto alpha_kind = [](std::uint32_t f) { return f == 6u ? 1 : (f == 7u ? 2 : (f == 2u || f == 3u ? 3 : 0)); };
        const int ks = alpha_kind(s), kt = alpha_kind(t);
        if ((ks == 1 || ks == 2) && kt != 0 && kt != ks) return false;
        if ((kt == 1 || kt == 2) && ks != 0 && ks != kt) return false;
        if (s >= 10u && t >= 10u && d.fix_a != d.fix_b) return false;
        // A 32-bit target blends its alpha too; doubled-alpha factors that also feed the other side's alpha factor stay in software.
        if (d.fb_format == 3u && (((ks == 1 || ks == 2) && (t == 0u || t == 1u)) || kt == 1 || kt == 2)) return false;
        if ((ks != 0 && ks != 3 || kt != 0 && kt != 3) && (s == 4u || s == 5u || t == 4u || t == 5u)) return false;
    }
    return true;
}

std::uint32_t GlBackend::rows_for(std::uint32_t addr, std::uint32_t stride, std::uint32_t format) {
    const std::uint32_t row_bytes = stride * (format == 3u ? 4u : 2u);
    if (addr < 0x04000000u || addr >= 0x04200000u || row_bytes == 0u) return 0u;
    return std::min<std::uint32_t>(272u, (0x04200000u - addr) / row_bytes);
}

GlBackend::Rt *GlBackend::find_rt(std::uint32_t address) {
    for (auto &rt : rts_) if (rt->addr == address) return rt.get();
    return nullptr;
}
const GlBackend::Rt *GlBackend::find_rt(std::uint32_t address) const {
    for (const auto &rt : rts_) if (rt->addr == address) return rt.get();
    return nullptr;
}

void GlBackend::destroy_rt(Rt &rt) {
    Gl &g = *gl_;
    if (rt.ms_fbo) g.DeleteFramebuffers(1, &rt.ms_fbo);
    if (rt.ms_color) g.DeleteRenderbuffers(1, &rt.ms_color);
    if (rt.ms_depth) g.DeleteRenderbuffers(1, &rt.ms_depth);
    rt.ms_fbo = rt.ms_color = rt.ms_depth = 0; rt.ms_dirty = rt.ms_depth_dirty = rt.ms_fill = false;
    if (rt.fbo) g.DeleteFramebuffers(1, &rt.fbo);
    if (rt.depth) g.DeleteRenderbuffers(1, &rt.depth);
    if (rt.tex) g.DeleteTextures(1, &rt.tex);
    rt.fbo = rt.depth = rt.tex = 0;
}

GlBackend::Rt *GlBackend::get_rt(const GpuDraw &d) {
    if (Rt *existing = find_rt(d.fb_addr)) {
        if (existing->stride == d.fb_stride) return existing;
        // The game re-used this memory with a different layout: write the old contents back and start over.
        flush();
        rb_reason_ = "other";
        readback(*existing);
        rb_reason_ = "ge";
        destroy_rt(*existing);
        for (auto it = rts_.begin(); it != rts_.end(); ++it) if (it->get() == existing) { rts_.erase(it); break; }
    }
    Gl &g = *gl_;
    auto rt = std::make_unique<Rt>();
    rt->addr = d.fb_addr; rt->stride = d.fb_stride; rt->format = d.fb_format;
    rt->rows = rows_for(d.fb_addr, d.fb_stride, d.fb_format);
    rt->w = static_cast<int>(rt->stride) * scale_; rt->h = 272 * scale_;
    g.GenTextures(1, &rt->tex);
    g.BindTexture(GL_TEXTURE_2D, rt->tex);
    g.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    g.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    g.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    g.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, rt->w, rt->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    g.GenRenderbuffers(1, &rt->depth);
    g.BindRenderbuffer(GL_RENDERBUFFER, rt->depth);
    g.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH32F_STENCIL8, rt->w, rt->h);
    g.GenFramebuffers(1, &rt->fbo);
    g.BindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
    g.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt->tex, 0);
    g.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rt->depth);
    if (g.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        error_ = "render target framebuffer incomplete";
        destroy_rt(*rt);
        return nullptr;
    }
    g.Disable(GL_SCISSOR_TEST);
    g.DepthMask(1);
    g.StencilMask(0xFF);
    g.Clear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    if (msaa_ > 1 && rt->stride == 512u) {
        // Multisampled twin of the target: all drawing goes here, resolve_rt() copies it into tex / fbo when something reads.
        (void)g.GetError();
        g.GenRenderbuffers(1, &rt->ms_color);
        g.BindRenderbuffer(GL_RENDERBUFFER, rt->ms_color);
        g.RenderbufferStorageMultisample(GL_RENDERBUFFER, msaa_, GL_RGBA8, rt->w, rt->h);
        g.GenRenderbuffers(1, &rt->ms_depth);
        g.BindRenderbuffer(GL_RENDERBUFFER, rt->ms_depth);
        g.RenderbufferStorageMultisample(GL_RENDERBUFFER, msaa_, GL_DEPTH32F_STENCIL8, rt->w, rt->h);
        g.GenFramebuffers(1, &rt->ms_fbo);
        g.BindFramebuffer(GL_FRAMEBUFFER, rt->ms_fbo);
        g.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rt->ms_color);
        g.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rt->ms_depth);
        if (g.GetError() != 0u || g.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            events_.emplace_back("MSAA: could not create a multisampled render target - this target stays unsmoothed");
            if (rt->ms_fbo) g.DeleteFramebuffers(1, &rt->ms_fbo);
            if (rt->ms_color) g.DeleteRenderbuffers(1, &rt->ms_color);
            if (rt->ms_depth) g.DeleteRenderbuffers(1, &rt->ms_depth);
            rt->ms_fbo = rt->ms_color = rt->ms_depth = 0;
            (void)g.GetError();
        } else {
            g.Disable(GL_SCISSOR_TEST);
            g.Clear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        }
    }
    rts_.push_back(std::move(rt));
    Rt *created = rts_.back().get();
    created->cpu_row0 = 0u; created->cpu_row1 = created->rows - 1u; created->cpu_dirty = true;
    upload_from_vram(*created);
    return created;
}

// Records what memory looked like when the GPU copy and memory last agreed.  If memory later differs for a row the GPU
// wants to write back, the CPU (or a DMA) wrote there in between and memory is the newer copy: the guest keeps textures
// in VRAM next to / underneath stale render targets, and writing the stale target back would wipe them.
void GlBackend::sync_rows(Rt &rt, const std::uint8_t *mem, std::uint32_t row0, std::uint32_t row1) {
    if (mem == nullptr) return;
    if (rt.row_hash.size() != rt.rows) { rt.row_hash.assign(rt.rows, 0u); rt.row_hash_ok.assign(rt.rows, 0u); }
    const std::uint32_t bpp = rt.format == 3u ? 4u : 2u;
    const std::uint32_t row_bytes = rt.stride * bpp, visible = std::min<std::uint32_t>(480u, rt.stride) * bpp;
    for (std::uint32_t r = row0; r <= row1 && r < rt.rows; ++r) {
        rt.row_hash[r] = hash_bytes(mem + static_cast<std::size_t>(r) * row_bytes, visible, 0x1234567ull);
        rt.row_hash_ok[r] = 1u;
    }
}

void GlBackend::resolve_rt(Rt &rt, bool with_depth) {
    if (rt.ms_fbo == 0u) return;
    const bool color = rt.ms_dirty, depth = with_depth && rt.ms_depth_dirty;
    if (!color && !depth) return;
    Gl &g = *gl_;
    g.Disable(GL_SCISSOR_TEST);
    g.ColorMask(1, 1, 1, 1);
    g.BindFramebuffer(GL_READ_FRAMEBUFFER, rt.ms_fbo);
    g.BindFramebuffer(GL_DRAW_FRAMEBUFFER, rt.fbo);
    GLbitfield mask = 0;
    if (color) mask |= GL_COLOR_BUFFER_BIT;
    if (depth) mask |= GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT;
    g.BlitFramebuffer(0, 0, rt.w, rt.h, 0, 0, rt.w, rt.h, mask, GL_NEAREST);
    if (color) rt.ms_dirty = false;
    if (depth) rt.ms_depth_dirty = false;
}

void GlBackend::upload_from_vram(Rt &rt) {
    const prof::Scope prof_scope(prof::kUpload);
    Gl &g = *gl_;
    resolve_rt(rt, false);   // keep what the multisampled buffer holds for the rows that are not replaced
    const std::uint32_t last_row = rt.rows - 1u;
    const std::uint32_t row0 = std::min<std::uint32_t>(rt.cpu_row0, last_row), row1 = std::min<std::uint32_t>(std::max(rt.cpu_row1, row0), last_row);
    const std::uint8_t *src = static_cast<const psprecomp::GuestMemory &>(memory_).raw_pointer(rt.addr, rt.bytes());
    const int rows = static_cast<int>(row1 - row0 + 1u) * scale_;
    // Fast path: expand the PSP pixels at native size (480x272 instead of 8x8 times that), upload them to a small staging
    // texture and let the GPU scale them into the target with a nearest-neighbour blit.  The old loop built the whole
    // scaled image on the CPU (~8M texels per frame at 8x) - ~17 ms for every frame of the intro movie, which the game
    // writes into the frame buffer from the CPU.  RR_UPLOAD_SLOW=1 uses the old path.
    static const bool slow_upload = std::getenv("RR_UPLOAD_SLOW") != nullptr;
    const bool fast_up = !slow_upload && scale_ > 1 && src != nullptr && g.BlitFramebuffer != nullptr && g.GenFramebuffers != nullptr;
    std::vector<std::uint32_t> pixels;
    if (!fast_up) pixels.assign(static_cast<std::size_t>(rt.w) * rows, 0u);
    if (fast_up) {
        const int w1 = rt.w / scale_, nrows = static_cast<int>(row1 - row0 + 1u);
        const std::uint32_t bpp = rt.format == 3u ? 4u : 2u;
        std::vector<std::uint32_t> nat(static_cast<std::size_t>(w1) * nrows);
        for (int y = 0; y < nrows; ++y) {
            const std::uint8_t *line = src + static_cast<std::size_t>(row0 + static_cast<std::uint32_t>(y)) * rt.stride * bpp;
            std::uint32_t *out = nat.data() + static_cast<std::size_t>(y) * w1;
            if (bpp == 4u) std::memcpy(out, line, static_cast<std::size_t>(w1) * 4u);
            else for (int x = 0; x < w1; ++x) { std::uint16_t px; std::memcpy(&px, line + static_cast<std::size_t>(x) * 2u, 2); out[x] = pixel_expand(rt.format, px); }
        }
        if (g.up_tex == 0u || g.up_w < w1 || g.up_h < nrows) {
            if (g.up_fbo) g.DeleteFramebuffers(1, &g.up_fbo);
            if (g.up_tex) g.DeleteTextures(1, &g.up_tex);
            g.up_w = std::max(w1, g.up_w); g.up_h = std::max(std::max(nrows, 272), g.up_h);
            g.GenTextures(1, &g.up_tex);
            g.BindTexture(GL_TEXTURE_2D, g.up_tex);
            g.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            g.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            g.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g.up_w, g.up_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            g.GenFramebuffers(1, &g.up_fbo);
            g.BindFramebuffer(GL_FRAMEBUFFER, g.up_fbo);
            g.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g.up_tex, 0);
        }
        g.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
        g.BindTexture(GL_TEXTURE_2D, g.up_tex);
        g.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w1, nrows, GL_RGBA, GL_UNSIGNED_BYTE, nat.data());
        g.Disable(GL_SCISSOR_TEST);
        g.ColorMask(1, 1, 1, 1);
        g.BindFramebuffer(GL_READ_FRAMEBUFFER, g.up_fbo);
        g.BindFramebuffer(GL_DRAW_FRAMEBUFFER, rt.fbo);
        g.BlitFramebuffer(0, 0, w1, nrows, 0, static_cast<GLint>(row0 * scale_), rt.w, static_cast<GLint>((row0 + static_cast<std::uint32_t>(nrows)) * scale_),
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
    } else if (src != nullptr) {
        const std::uint32_t bpp = rt.format == 3u ? 4u : 2u;
        for (int y = 0; y < rows; ++y) {
            const std::uint32_t gy = row0 + static_cast<std::uint32_t>(y / scale_);
            for (int x = 0; x < rt.w; ++x) {
                const std::uint32_t gx = static_cast<std::uint32_t>(x / scale_);
                const std::size_t at = (static_cast<std::size_t>(gy) * rt.stride + gx) * bpp;
                std::uint32_t c;
                if (bpp == 4u) std::memcpy(&c, src + at, 4);
                else { std::uint16_t p; std::memcpy(&p, src + at, 2); c = pixel_expand(rt.format, p); }
                pixels[static_cast<std::size_t>(y) * rt.w + x] = c;
            }
        }
    }
    if (!fast_up) {
        g.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
        g.BindTexture(GL_TEXTURE_2D, rt.tex);
        g.TexSubImage2D(GL_TEXTURE_2D, 0, 0, static_cast<GLint>(row0 * scale_), rt.w, rows, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    }
    if (rt.ms_fbo != 0u) {   // the multisampled colour must pick these rows up before the next draw
        const int y0 = static_cast<int>(row0) * scale_, y1 = y0 + rows;
        rt.ms_fill_y0 = rt.ms_fill ? std::min(rt.ms_fill_y0, y0) : y0;
        rt.ms_fill_y1 = rt.ms_fill ? std::max(rt.ms_fill_y1, y1) : y1;
        rt.ms_fill = true;
    }
    if (src != nullptr) sync_rows(rt, src, row0, row1);
    rt.cpu_dirty = false;
    rt.cpu_row0 = 272u; rt.cpu_row1 = 0u;
    for (std::uint32_t r = row0; r <= row1; ++r) rt.rows_dirty.reset(r);   // memory is authoritative for these rows now
    rt.gpu_dirty = rt.rows_dirty.any();
    ++stats_.uploads;
}

void GlBackend::readback(Rt &rt, std::uint32_t row0, std::uint32_t row1) {
    const prof::Scope prof_scope(prof::kReadback);
    // RR_RB_TIME=ms: log every readback that takes longer than that (wall time, includes the wait for the GPU).
    static const double rb_time_ms = [] { const char *e = std::getenv("RR_RB_TIME"); return e ? std::atof(e) : -1.0; }();
    struct RbTimer {
        GlBackend *self; Rt &rt; std::uint32_t r0, r1; std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        ~RbTimer() {
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            self->rb_total_ms_ += ms;
            if (rb_time_ms < 0.0) return;
            static int n = 0;
            if (ms >= rb_time_ms && n < 1500 && !(std::strcmp(self->rb_reason_, "ge") == 0 && ms < 5.0 && std::getenv("RR_RB_TIME_ALL") == nullptr)) {
                ++n; char b[200];
                std::snprintf(b, sizeof b, "GLRBTIME %.2fms [stencil %.2f resolve %.2f blit %.2f readpixels %.2f] reason=%s dst=%08X lag=%d rt=%08X fmt=%u w=%d h=%d rows=%u-%u ms=%d", ms, self->rb_stage_ms_[0], self->rb_stage_ms_[1], self->rb_stage_ms_[2], self->rb_stage_ms_[3], self->rb_reason_, self->rb_dst_, self->rb_lag_ ? 1 : 0, rt.addr, rt.format, rt.w, rt.h, r0, r1, rt.ms_fbo != 0u ? 1 : 0);
                self->events_.emplace_back(b);
            }
        }
    } rb_timer{this, rt, row0, row1};
    std::uint8_t *dst = memory_.raw_pointer(rt.addr, rt.bytes());
    if (dst == nullptr) { rt.gpu_dirty = false; rt.rows_dirty.reset(); return; }
    row1 = std::min<std::uint32_t>(row1, rt.rows - 1u);
    // Only rows where the GPU is actually ahead of memory need copying.
    // Rows whose memory changed since the last sync were written by the CPU: memory is newer, so don't clobber it.
    if (rt.row_hash.size() == rt.rows) {
        const std::uint32_t bpp = rt.format == 3u ? 4u : 2u;
        const std::uint32_t row_bytes = rt.stride * bpp, vis_bytes = std::min<std::uint32_t>(480u, rt.stride) * bpp;
        for (std::uint32_t r = row0; r <= row1; ++r) {
            if (!rt.rows_dirty.test(r) || !rt.row_hash_ok[r]) continue;
            if (hash_bytes(dst + static_cast<std::size_t>(r) * row_bytes, vis_bytes, 0x1234567ull) != rt.row_hash[r]) {
                rt.rows_dirty.reset(r);
                if (!rt.cpu_dirty) { rt.cpu_row0 = r; rt.cpu_row1 = r; }
                else { rt.cpu_row0 = std::min(rt.cpu_row0, r); rt.cpu_row1 = std::max(rt.cpu_row1, r); }
                rt.cpu_dirty = true;
                ++stats_.readback_skipped;
            }
        }
    }
    const std::uint32_t visible = std::min<std::uint32_t>(480u, rt.stride);
    std::uint32_t r = row0;
    bool any = false;
    while (r <= row1) {
        if (!rt.rows_dirty.test(r)) { ++r; continue; }
        std::uint32_t last = r;
        while (last + 1u <= row1 && rt.rows_dirty.test(last + 1u)) ++last;
        readback_to(rt, dst, visible, r, last);
        sync_rows(rt, dst, r, last);
        for (std::uint32_t k = r; k <= last; ++k) rt.rows_dirty.reset(k);
        stats_.readback_rows += last - r + 1u;
        if (!any) { any = true; ++stats_.readbacks; }
        r = last + 1u;
    }
    const std::uint32_t first = row0, last = row1;
    if (!any) { rt.gpu_dirty = rt.rows_dirty.any(); return; }
    rt.gpu_dirty = rt.rows_dirty.any();
    {
        const std::string r = rb_reason_;
        const int idx = r == "fb" ? 1 : r == "tex" ? 2 : r == "z" ? 3 : r == "gltex" ? 4 : (r == "other" || r == "xfer") ? 5 : 0;
        ++rb_by_reason_[idx];
        static std::uint32_t logged = 0u;
        if (logged < 60u && idx != 0 && events_budget_ != 0u) {
            ++logged; --events_budget_;
            char b[200];
            std::snprintf(b, sizeof b, "GLREADBACK reason=%s rt=%08X fmt=%u rows=%u-%u", rb_reason_, rt.addr, rt.format, first, last);
            events_.emplace_back(b);
        }
    }
}


static void vis_log(std::vector<std::string> &events, std::uint32_t addr, std::uint32_t stride, const std::uint8_t *dst, std::uint32_t row0, std::uint32_t row1) {
    static const bool on = vis_log_enabled();
    if (!on || (addr != 0x04195140u && addr != 0x041FC000u) || row0 != 0u) return;
    static int n = 0; if (n > 3000) return; ++n;
    std::uint32_t p[4]{}; unsigned nza = 0, nzr = 0, maxa = 0, maxr = 0;
    for (std::uint32_t x = 0; x < 256u; ++x) {
        std::uint32_t c; std::memcpy(&c, dst + static_cast<std::size_t>(x) * 4u, 4);
        if (x < 4u) p[x] = c;
        const unsigned r = c & 0xFFu, a = c >> 24u;
        nzr += r != 0; nza += a != 0; maxr = std::max(maxr, r); maxa = std::max(maxa, a);
    }
    char b[220]; std::snprintf(b, sizeof b, "GLVIS addr=%08X row0 px0-3=%08X,%08X,%08X,%08X nz_r=%u nz_a=%u max_r=%u max_a=%u", addr, p[0], p[1], p[2], p[3], nzr, nza, maxr, maxa);
    events.emplace_back(b);
}

// The PSP keeps stencil in the framebuffer alpha and games read it back (sun-visibility tests sample the frame's alpha
// as a texture, for instance).  Here stencil lives in the RT's stencil buffer, so before alpha is read it is rebuilt
// from there: alpha = stencil value (0/255 for 5551, replicated nibbles for 4444).
void GlBackend::resolve_stencil(Rt &rt) {
    static const bool off = std::getenv("RR_NO_RESOLVE") != nullptr;   // A/B switch
    if (!rt.alpha_stale) return;
    rt.alpha_stale = false;
    if (off) return;
    if (rt.format == 0u) return;
    Gl &g = *gl_;
    resolve_rt(rt, true);
    g.BindFramebuffer(GL_FRAMEBUFFER, rt.fbo);
    g.Viewport(0, 0, rt.w, rt.h);
    g.Disable(GL_SCISSOR_TEST);
    g.Disable(GL_DEPTH_TEST);
    g.DepthMask(0);
    g.Disable(GL_CULL_FACE);
    g.Disable(GL_BLEND);
    g.ColorMask(0, 0, 0, 1);
    g.Disable(GL_STENCIL_TEST);
    g.ClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    g.Clear(GL_COLOR_BUFFER_BIT);
    g.UseProgram(g.program);
    g.BindVertexArray(g.vao);
    g.Uniform2f(g.loc_rt, static_cast<float>(rt.stride), 272.0f);
    g.Uniform2f(g.loc_pt, 0.0f, 0.0f);
    g.Uniform1i(g.loc_clear, 1);
    g.Uniform1i(g.loc_alpha_mode, 0);
    g.Uniform1i(g.loc_fog, 0);
    g.Uniform2f(g.loc_jit, 0.0f, 0.0f);
    const float x1 = static_cast<float>(rt.stride), y1 = 272.0f;
    const auto pass = [&](GLenum func, GLint ref, GLuint mask, float alpha) {
        GpuVertex v[6];
        const float px[6] = {0, x1, 0, x1, x1, 0}, py[6] = {0, 0, y1, 0, y1, y1};
        for (int i = 0; i < 6; ++i) { v[i].x = px[i]; v[i].y = py[i]; v[i].z = 0.0f; v[i].inv_w = 1.0f; v[i].r = v[i].g = v[i].b = 0.0f; v[i].a = alpha; v[i].fog = 1.0f; }
        g.Enable(GL_STENCIL_TEST);
        g.StencilFunc(func, ref, mask);
        g.StencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        g.StencilMask(0);
        g.BindBuffer(GL_ARRAY_BUFFER, g.vbo);
        g.BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof v), v, GL_STREAM_DRAW);
        g.DrawArrays(GL_TRIANGLES, 0, 6);
    };
    if (rt.format == 1u) {
        pass(GL_NOTEQUAL, 0, 0xFFu, 255.0f);
    } else {
        g.Enable(GL_BLEND);
        g.BlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        g.BlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE);
        for (int b = 0; b < 8; ++b) pass(GL_EQUAL, 1 << b, 1u << b, static_cast<float>(1 << b));
        g.Disable(GL_BLEND);
    }
    g.Disable(GL_STENCIL_TEST);
    g.ColorMask(1, 1, 1, 1);
    g.DepthMask(1);
    if (rt.ms_fbo != 0u) { rt.ms_fill = true; rt.ms_fill_y0 = 0; rt.ms_fill_y1 = rt.h; }   // the rebuilt alpha lives in tex only
}

// Copies rows [row0, row1] of the GPU target, sampled back down to native resolution, into `dst` laid out like
// emulated VRAM.
void GlBackend::readback_to(Rt &rt, std::uint8_t *dst, std::uint32_t visible, std::uint32_t row0, std::uint32_t row1) {
    const auto st0 = std::chrono::steady_clock::now();
    resolve_stencil(rt);
    const auto st1 = std::chrono::steady_clock::now();
    resolve_rt(rt, false);
    const auto st2 = std::chrono::steady_clock::now();
    rb_stage_ms_[0] = std::chrono::duration<double, std::milli>(st1 - st0).count();
    rb_stage_ms_[1] = std::chrono::duration<double, std::milli>(st2 - st1).count();
    Gl &g = *gl_;
    const std::uint32_t rows = row1 - row0 + 1u;
    g.PixelStorei(GL_PACK_ALIGNMENT, 1);
    // Only one pixel of every scale x scale block is ever used, so let the GPU pick those (nearest-neighbour blit down to
    // the PSP's own resolution) and read back the small image instead of the whole scaled one.
    const bool shrink = scale_ > 1 && g.BlitFramebuffer != nullptr;
    const int w1 = rt.w / scale_;
    if (shrink) {
        const int need_h = static_cast<int>(std::max<std::uint32_t>(rows, 272u));
        if (g.rb_tex == 0u || g.rb_w < w1 || g.rb_h < need_h) {
            if (g.rb_fbo) g.DeleteFramebuffers(1, &g.rb_fbo);
            if (g.rb_tex) g.DeleteTextures(1, &g.rb_tex);
            g.rb_w = std::max(w1, g.rb_w); g.rb_h = std::max(need_h, g.rb_h);
            g.GenTextures(1, &g.rb_tex);
            g.BindTexture(GL_TEXTURE_2D, g.rb_tex);
            g.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            g.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            g.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g.rb_w, g.rb_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            g.GenFramebuffers(1, &g.rb_fbo);
            g.BindFramebuffer(GL_FRAMEBUFFER, g.rb_fbo);
            g.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g.rb_tex, 0);
        }
        g.Disable(GL_SCISSOR_TEST);
        g.ColorMask(1, 1, 1, 1);
        g.BindFramebuffer(GL_READ_FRAMEBUFFER, rt.fbo);
        g.BindFramebuffer(GL_DRAW_FRAMEBUFFER, g.rb_fbo);
        g.BlitFramebuffer(0, static_cast<GLint>(row0 * scale_), rt.w, static_cast<GLint>((row0 + rows) * scale_), 0, 0, w1,
                          static_cast<GLint>(rows), GL_COLOR_BUFFER_BIT, GL_NEAREST);
        readback_scratch_.resize(static_cast<std::size_t>(w1) * rows * 4u);
        g.BindFramebuffer(GL_READ_FRAMEBUFFER, g.rb_fbo);
        const auto st3 = std::chrono::steady_clock::now();
        rb_stage_ms_[2] = std::chrono::duration<double, std::milli>(st3 - st2).count();
        // The sun-visibility probe (small target 0x041FC000, sampled as a texture by the next draw) is read back every
        // frame while the sun is in view.  A synchronous glReadPixels makes the CPU wait for all the frame's queued GPU
        // work (~3 ms each time), which at 120 fps at max quality breaks the frame budget.  Read it through a pair of pixel
        // buffers instead and use the previous frame's pixels (one frame = 8 ms late, invisible in a brightness probe).
        // RR_RB_LAG=0 turns this off.
        static const bool lag_on = std::getenv("RR_RB_LAG") == nullptr || std::string(std::getenv("RR_RB_LAG")) != "0";
        const bool lag = lag_on && std::strcmp(rb_reason_, "gltex") == 0 && g.MapBufferRange != nullptr && g.UnmapBuffer != nullptr &&
                         (rt.addr == 0x041FC000u || rb_lag_);
        if (lag) {
            constexpr GLenum kPack = 0x88EB, kStreamRead = 0x88E1;
            constexpr GLbitfield kMapRead = 0x0001;
            const std::size_t bytes = static_cast<std::size_t>(w1) * rows * 4u;
            static const bool fences = std::getenv("RR_RB_FENCE_OFF") == nullptr;
            if (fences && g.FenceSync != nullptr && g.ClientWaitSync != nullptr && g.DeleteSync != nullptr) {
                // Ring of pixel buffers, each with a fence: queue this frame's read, then take the newest read the GPU has
                // already finished (never waits).  Before any is finished, the last pixels taken are used again.  Only the
                // first read (or after a pause of more than 3 frames) waits, so a probe never shows stale-for-long data.
                constexpr int kRing = 4;
                struct Ring { GLuint pbo[kRing]{}; void *fence[kRing]{}; std::uint64_t when[kRing]{}; std::size_t bytes{0}; int cur{0};
                              std::vector<std::uint8_t> last; std::uint64_t last_frame{0}; };
                static std::map<std::uint64_t, Ring> rings;
                const std::uint64_t key = (static_cast<std::uint64_t>(rt.addr) << 24u) ^ (static_cast<std::uint64_t>(row0) << 12u) ^ rows;
                Ring &R = rings[key];
                if (R.pbo[0] == 0u) g.GenBuffers(kRing, R.pbo);
                const bool stale = frame_ > R.last_frame + 3u;   // not read for a while (the sun left the view): old pixels are no use
                if (bytes != R.bytes || stale) {
                    for (int i = 0; i < kRing; ++i) {
                        if (R.fence[i]) { g.DeleteSync(R.fence[i]); R.fence[i] = nullptr; }
                        if (bytes != R.bytes) { g.BindBuffer(kPack, R.pbo[i]); g.BufferData(kPack, static_cast<GLsizeiptr>(bytes), nullptr, kStreamRead); }
                    }
                    R.bytes = bytes; R.last.clear();
                }
                const int slot = R.cur;
                if (R.fence[slot]) { g.DeleteSync(R.fence[slot]); R.fence[slot] = nullptr; }
                g.BindBuffer(kPack, R.pbo[slot]);
                g.ReadPixels(0, 0, w1, static_cast<GLsizei>(rows), GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                R.fence[slot] = g.FenceSync(0x9117 /*GL_SYNC_GPU_COMMANDS_COMPLETE*/, 0);
                R.when[slot] = ++rb_ring_seq_;
                R.cur = (slot + 1) % kRing;
                // newest finished read
                int best = -1;
                for (int i = 0; i < kRing; ++i) {
                    if (!R.fence[i] || (best >= 0 && R.when[i] < R.when[best])) continue;
                    static const bool first_wait = std::getenv("RR_RB_FIRST_WAIT") != nullptr;
                    const bool want_wait = first_wait && R.last.empty() && i == slot;   // nothing to show yet: wait for this one
                    const GLenum r = g.ClientWaitSync(R.fence[i], 0x1 /*FLUSH*/, want_wait ? 50000000ull : 0ull);
                    if (r == 0x911A || r == 0x911C) best = i;   // ALREADY_SIGNALED / CONDITION_SATISFIED
                }
                static const bool first_wait2 = std::getenv("RR_RB_FIRST_WAIT") != nullptr;
                if (best < 0 && R.last.empty() && first_wait2) best = slot;   // driver gave up waiting: map anyway
                if (best >= 0) {
                    g.BindBuffer(kPack, R.pbo[best]);
                    const void *mapped = g.MapBufferRange(kPack, 0, static_cast<GLsizeiptr>(bytes), kMapRead);
                    if (mapped != nullptr) { R.last.assign(static_cast<const std::uint8_t *>(mapped), static_cast<const std::uint8_t *>(mapped) + bytes); g.UnmapBuffer(kPack); }
                    // older reads than the one taken are no longer needed
                    for (int i = 0; i < kRing; ++i) if (R.fence[i] && R.when[i] <= R.when[best]) { g.DeleteSync(R.fence[i]); R.fence[i] = nullptr; }
                }
                g.BindBuffer(kPack, 0);
                // Nothing finished yet (the sun just came into view): black for this frame instead of waiting ~6 ms for
                // the GPU (the glare then starts one or two frames later).  RR_RB_FIRST_WAIT=1 waits as before.
                if (R.last.size() == bytes) std::memcpy(readback_scratch_.data(), R.last.data(), bytes);
                else std::memset(readback_scratch_.data(), 0, bytes);
                R.last_frame = frame_;
            } else {
            struct LagPbo { GLuint pbo[2]{0, 0}; std::size_t bytes{0}; int cur{0}; bool have_prev{false}; std::uint64_t last_frame{0}; };
            static std::map<std::uint64_t, LagPbo> lags;
            const std::uint64_t key = (static_cast<std::uint64_t>(rt.addr) << 24u) ^ (static_cast<std::uint64_t>(row0) << 12u) ^ rows;
            LagPbo &L = lags[key];
            if (L.pbo[0] == 0u) g.GenBuffers(2, L.pbo);
            if (bytes != L.bytes) {
                for (int i = 0; i < 2; ++i) { g.BindBuffer(kPack, L.pbo[i]); g.BufferData(kPack, static_cast<GLsizeiptr>(bytes), nullptr, kStreamRead); }
                L.bytes = bytes; L.have_prev = false;
            }
            if (frame_ > L.last_frame + 3u) L.have_prev = false;   // stale (sun just came into view): take this frame's pixels
            g.BindBuffer(kPack, L.pbo[L.cur]);
            g.ReadPixels(0, 0, w1, static_cast<GLsizei>(rows), GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            const int src = L.have_prev ? L.cur ^ 1 : L.cur;
            g.BindBuffer(kPack, L.pbo[src]);
            const void *mapped = g.MapBufferRange(kPack, 0, static_cast<GLsizeiptr>(bytes), kMapRead);
            if (mapped != nullptr) { std::memcpy(readback_scratch_.data(), mapped, bytes); g.UnmapBuffer(kPack); }
            g.BindBuffer(kPack, 0);
            L.cur ^= 1; L.have_prev = true; L.last_frame = frame_;
            }
        } else
        g.ReadPixels(0, 0, w1, static_cast<GLsizei>(rows), GL_RGBA, GL_UNSIGNED_BYTE, readback_scratch_.data());
        rb_stage_ms_[3] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - st3).count();
    } else {
        readback_scratch_.resize(static_cast<std::size_t>(rt.w) * rows * scale_ * 4u);
        g.BindFramebuffer(GL_READ_FRAMEBUFFER, rt.fbo);
        g.ReadPixels(0, static_cast<GLint>(row0 * scale_), rt.w, static_cast<GLsizei>(rows * scale_), GL_RGBA, GL_UNSIGNED_BYTE, readback_scratch_.data());
    }
    const std::uint32_t bpp = rt.format == 3u ? 4u : 2u;
    const int half = scale_ / 2;
    for (std::uint32_t y = row0; y <= row1; ++y) {
        for (std::uint32_t x = 0; x < visible; ++x) {
            const std::size_t src_at = shrink ? (static_cast<std::size_t>(y - row0) * w1 + x) * 4u
                                              : (static_cast<std::size_t>((y - row0) * scale_ + half) * rt.w + x * scale_ + half) * 4u;
            std::uint32_t c;
            std::memcpy(&c, readback_scratch_.data() + src_at, 4);
            const std::size_t at = (static_cast<std::size_t>(y) * rt.stride + x) * bpp;
            if (bpp == 4u) std::memcpy(dst + at, &c, 4);
            else { const std::uint16_t p = static_cast<std::uint16_t>(pixel_compress(rt.format, c)); std::memcpy(dst + at, &p, 2); }
        }
    }
    if (bpp == 4u) vis_log(events_, rt.addr, rt.stride, dst, row0, row1);
}

bool GlBackend::hits_columns(const Rt &rt, std::uint32_t address, std::uint32_t length, std::uint32_t x0, std::uint32_t x1) {
    // Does [address, address+length) touch any pixel with column in [x0, x1) of the target?
    const std::uint32_t bpp = rt.format == 3u ? 4u : 2u;
    if (!ranges_overlap(address, length, rt.addr, rt.bytes()) || x1 <= x0) return false;
    const std::uint32_t row_bytes = rt.stride * bpp;
    const std::uint32_t begin = address > rt.addr ? address - rt.addr : 0u;
    const std::uint32_t end = std::min<std::uint32_t>(address + length - rt.addr, rt.bytes());
    const std::uint32_t row0 = begin / row_bytes, row1 = (end - 1u) / row_bytes;
    if (row1 > row0 + 1u) return true;
    for (std::uint32_t row = row0; row <= row1; ++row) {
        const std::uint32_t lo = std::max(begin, row * row_bytes + x0 * bpp), hi = std::min(end, row * row_bytes + x1 * bpp);
        if (lo < hi) return true;
    }
    return false;
}

void GlBackend::vram_read(std::uint32_t address, std::uint32_t length) {
    const prof::Scope prof_scope(prof::kVramRead);
    if (!ok_) return;
    if (batch_active_) {
        // Pending draws must land before memory is looked at or changed where they draw or sample.
        const std::uint32_t tex_bytes = texture_bytes_of(batch_d_);
        if (ranges_overlap(address, length, batch_rt_->addr, batch_rt_->bytes()) ||
            (tex_bytes != 0u && ranges_overlap(address, length, batch_d_.tex_addr, tex_bytes))) {
            // Drawing the batch can run texture look-ups that set and then reset the readback reason / late flag;
            // keep the caller's (a late-allowed palette read was turned into a synchronous one here).
            const char *reason = rb_reason_; const bool lag = rb_lag_; const std::uint32_t dst = rb_dst_;
            flush();
            rb_reason_ = reason; rb_lag_ = lag; rb_dst_ = dst;
        }
    }
    for (auto &rt : rts_) {
        if (!rt->gpu_dirty) continue;
        const std::uint32_t visible = std::min<std::uint32_t>(480u, rt->stride);
        if (!hits_columns(*rt, address, length, 0u, visible)) continue;
        const std::uint32_t row_bytes = rt->stride * (rt->format == 3u ? 4u : 2u);
        const std::uint32_t begin = address > rt->addr ? address - rt->addr : 0u;
        const std::uint32_t end = std::min<std::uint32_t>(address + length - rt->addr, rt->bytes());
        readback(*rt, begin / row_bytes, (end - 1u) / row_bytes);
    }
}

void GlBackend::vram_read_late_ok(std::uint32_t address, std::uint32_t length) {
    // The sun glare renders its brightness into a 256-entry palette row (0x04195140) and loads it as a CLUT in the same
    // frame.  A synchronous read stops the CPU until the GPU has finished the frame so far (CPU and GPU then run one after
    // the other: ~125 fps instead of 144 while the sun is on screen).  Take it through the late-read ring instead.
    static const bool off = std::getenv("RR_CLUT_LAG_OFF") != nullptr;
    if (off) { vram_read(address, length); return; }
    const char *reason = rb_reason_;
    const bool lag = rb_lag_;
    rb_reason_ = "gltex"; rb_lag_ = true; rb_dst_ = address;
    vram_read(address, length);
    rb_reason_ = reason; rb_lag_ = lag;
}

void GlBackend::vram_write(std::uint32_t address, std::uint32_t length) {
    if (!ok_ || length == 0u) return;
    flush();
    for (auto &rt : rts_) {
        const std::uint32_t visible = std::min<std::uint32_t>(480u, rt->stride);
        if (!hits_columns(*rt, address, length, 0u, visible)) continue;
        const std::uint32_t row_bytes = rt->stride * (rt->format == 3u ? 4u : 2u);
        const std::uint32_t begin = address > rt->addr ? address - rt->addr : 0u;
        const std::uint32_t end = std::min<std::uint32_t>(address + length - rt->addr, rt->bytes());
        const std::uint32_t r0 = begin / row_bytes, r1 = std::min<std::uint32_t>((end - 1u) / row_bytes, rt->rows - 1u);
        if (!rt->cpu_dirty) { rt->cpu_row0 = r0; rt->cpu_row1 = r1; }
        else { rt->cpu_row0 = std::min(rt->cpu_row0, r0); rt->cpu_row1 = std::max(rt->cpu_row1, r1); }
        rt->cpu_dirty = true;
    }
}

void GlBackend::end_frame() {
    flush();
    if (const int a = g_aniso.load(std::memory_order_relaxed); a != aniso_applied_ && gl_->mip_ready) {
        aniso_applied_ = a;
        if (gl_->max_aniso > 1.0f) {
            const float level = std::min(gl_->max_aniso, static_cast<float>(std::max(1, a)));
            for (int clamp = 0; clamp < 4; ++clamp) gl_->SamplerParameterf(gl_->mip_samplers[clamp], GL_TEXTURE_MAX_ANISOTROPY, level);
            (void)gl_->GetError();
        }
    }
    ++frame_;
    if ((frame_ & 63u) == 0u) purge_textures();
}

void GlBackend::purge_textures() {
    constexpr std::uint64_t kLimit = 192ull * 1024 * 1024;
    if (texture_bytes_ <= kLimit / 2) return;
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (it->second.last_use + 120u < frame_ || texture_bytes_ > kLimit) {
            gl_->DeleteTextures(1, &it->second.tex);
            texture_bytes_ -= it->second.bytes;
            it = textures_.erase(it);
        } else ++it;
    }
}

unsigned GlBackend::get_texture(const GpuDraw &d, bool &valid) {
    const prof::Scope prof_scope(prof::kTexture);
    valid = false;
    using Clk = std::chrono::steady_clock;
    const auto tt0 = Clk::now();
    double tt_rb = 0.0, tt_decode = 0.0, tt_upload = 0.0;
    struct TexLog {
        GlBackend *self; const GpuDraw &d; const Clk::time_point &t0; double &rb, &dec, &up;
        ~TexLog() {
            static const double lim = [] { const char *e = std::getenv("RR_TEX_TIME"); return e ? std::atof(e) : -1.0; }();
            if (lim < 0.0) return;
            const double ms = std::chrono::duration<double, std::milli>(Clk::now() - t0).count();
            static int n = 0;
            if (ms >= lim && n < 600) {
                ++n; char b[220];
                std::snprintf(b, sizeof b, "GLTEXTIME %.2fms [readback %.2f decode %.2f upload+mips %.2f] tex=%08X f%u %ux%u", ms, rb, dec, up, d.tex_addr, d.tex_format, d.tex_width, d.tex_height);
                self->events_.emplace_back(b);
            }
        }
    } tex_log{this, d, tt0, tt_rb, tt_decode, tt_upload};
    const std::uint32_t bits = texel_bits_of(d.tex_format);
    if (bits == 0u || d.tex_width == 0u || d.tex_height == 0u) return 0u;
    const std::size_t bytes = static_cast<std::size_t>(std::max(d.tex_stride, d.tex_width)) * d.tex_height * bits / 8u;
    // Anything the GPU has drawn into the source range must be visible in VRAM first.
    rb_reason_ = "gltex";
    // Sun-visibility probe: the game copies the scene into the small target 0x041FC000 to measure brightness.  That copy
    // can use the previous frame's scene (see readback_to), so the CPU need not wait for the GPU.
    rb_dst_ = d.fb_addr;
    // The same goes for the next stages: the scene copied into the 1x1 brightness pixel 0x041FFF00, and that pixel (it lives inside
    // the 16-bit target 0x041FE000, so it cannot be sampled from the GPU directly) used as the texture of the sun glare.  Read
    // synchronously these cost ~3 ms of GPU wait per frame whenever the sun was in view (144 fps dropped to ~125 on Seaside).
    // RR_SUN_LAG_OFF=1 restores the synchronous reads for those two.
    static const bool sun_lag = std::getenv("RR_SUN_LAG_OFF") == nullptr;
    const bool scene_tex = d.tex_addr == 0x04044000u || d.tex_addr == 0x04000000u || d.tex_addr == 0x04088000u;
    rb_lag_ = (d.fb_addr == 0x041FC000u && scene_tex) ||
              (sun_lag && ((d.fb_addr == 0x041FFF00u && scene_tex) || (d.tex_addr == 0x041FFF00u && d.tex_width == 1u && d.tex_height == 1u)));
    const std::uint64_t rb_before = stats_.readbacks;
    vram_read(d.tex_addr, static_cast<std::uint32_t>(bytes));
    bool from_render_target = stats_.readbacks != rb_before;   // the GPU drew this texture (screen copy etc.)
    if (!from_render_target)
        for (const auto &r : rts_) if (ranges_overlap(d.tex_addr, static_cast<std::uint32_t>(bytes), r->addr, r->bytes())) { from_render_target = true; break; }
    tt_rb = std::chrono::duration<double, std::milli>(Clk::now() - tt0).count();
    rb_lag_ = false;
    rb_reason_ = "ge";
    const std::uint8_t *src = static_cast<const psprecomp::GuestMemory &>(memory_).raw_pointer(d.tex_addr, bytes);
    if (src == nullptr) return 0u;
    if (d.tex_addr == 0x041FC000u && d.tex_format == 3u && vis_log_enabled()) {
        static int n = 0;
        if (n++ < 3000) {
            std::uint32_t px[4]{}; std::memcpy(px, src, 16);
            char b[160]; std::snprintf(b, sizeof b, "GLVISTEX 041FC000 as texture px0-3=%08X,%08X,%08X,%08X", px[0], px[1], px[2], px[3]);
            events_.emplace_back(b);
        }
    }
    std::uint64_t h = hash_bytes(src, bytes, 0xCBF29CE484222325ull);
    std::uint64_t params = (static_cast<std::uint64_t>(d.tex_format) << 56u) ^ (static_cast<std::uint64_t>(d.tex_width) << 40u) ^
                           (static_cast<std::uint64_t>(d.tex_height) << 28u) ^ (static_cast<std::uint64_t>(d.tex_stride) << 12u) ^
                           (d.tex_swizzled ? 1u : 0u);
    if (d.tex_format >= 4u && d.tex_format <= 7u) {
        params ^= (static_cast<std::uint64_t>(d.clut_format) << 3u) ^ (static_cast<std::uint64_t>(d.clut_shift) << 8u) ^
                  (static_cast<std::uint64_t>(d.clut_mask) << 16u) ^ (static_cast<std::uint64_t>(d.clut_start) << 24u);
        if (d.clut != nullptr) h = hash_bytes(d.clut, 1024u, h ^ 0x51ED270B07A5E1ull);
    }
    h = (h ^ params) * 0x9E3779B97F4A7C15ull;
    h ^= h >> 31u;
    auto it = textures_.find(h);
    if (it != textures_.end()) {
        it->second.last_use = frame_;
        ++stats_.texture_hits;
        valid = true;
        return it->second.tex;
    }
    const auto td0 = Clk::now();
    if (!decode_texture_rgba(d, memory_, decode_scratch_)) return 0u;
    tt_decode = std::chrono::duration<double, std::milli>(Clk::now() - td0).count();
    {
        // RR_DEBUG_CLASS: alpha histogram of every alpha-tested texture (before the colour bleed), to see what the "transparent" texels hold.
        static const bool dbg = std::getenv("RR_DEBUG_CLASS") != nullptr;
        static int logged = 0;
        if (dbg && d.tex_alpha && logged < 300) {
            ++logged;
            std::uint64_t h0 = 0, h1 = 0, h2 = 0, h3 = 0, h4 = 0, dark_low = 0;
            const std::size_t texels = static_cast<std::size_t>(d.tex_width) * d.tex_height;
            for (std::size_t i = 0; i < texels && i < decode_scratch_.size(); ++i) {
                const std::uint32_t c = decode_scratch_[i], al = c >> 24u;
                const std::uint32_t lum = ((c & 255u) + ((c >> 8u) & 255u) + ((c >> 16u) & 255u)) / 3u;
                if (al == 0u) ++h0; else if (al < 32u) ++h1; else if (al < 128u) ++h2; else if (al < 255u) ++h3; else ++h4;
                if (al > 0u && al < 128u && lum < 24u) ++dark_low;
            }
            char b[300];
            std::snprintf(b, sizeof b, "GLALPHA tex=%08X f%u %ux%u lin=%d blend=%d atest=%d func=%u ref=%u blend(eq%u src%u dst%u) alpha: 0=%llu 1-31=%llu 32-127=%llu 128-254=%llu 255=%llu darklow=%llu",
                          d.tex_addr, d.tex_format, d.tex_width, d.tex_height, d.tex_linear ? 1 : 0, d.blend ? 1 : 0, d.alpha_test ? 1 : 0, d.atest_func, d.atest_ref,
                          d.blend_eq, d.blend_src, d.blend_dst, (unsigned long long)h0, (unsigned long long)h1, (unsigned long long)h2, (unsigned long long)h3, (unsigned long long)h4, (unsigned long long)dark_low);
            events_.emplace_back(b);
        }
    }
    if (d.tex_alpha) {
        // A large texture whose contents change nearly every frame (the animated sun-glare palette: 512x512, a new
        // palette each frame) was re-bled every frame, ~14 ms each at this size.  Keep the edge bleed for textures that
        // are uploaded rarely, skip it for big ones that keep changing.  RR_BLEED_ALWAYS=1 restores the old behaviour.
        static const bool always = std::getenv("RR_BLEED_ALWAYS") != nullptr;
        struct Churn { std::uint64_t window_start{0}; std::uint32_t count{0}; };
        static std::unordered_map<std::uint32_t, Churn> churn;
        Churn &c = churn[d.tex_addr];
        if (frame_ > c.window_start + 30u) { c.window_start = frame_; c.count = 0; }
        ++c.count;
        const bool dynamic_big = c.count > 8u && static_cast<std::size_t>(d.tex_width) * d.tex_height >= 128u * 128u;
        // A screen/render-target copy used as a texture is not cut-out art: its alpha is the stencil bit, mostly 0, so the
        // bleed would smear colour into nearly the whole 512x512 image (~15 ms) for nothing.
        if (always || (!dynamic_big && !from_render_target)) bleed_transparent_texels(decode_scratch_, d.tex_width, d.tex_height);
    }
    Gl &g = *gl_;
    const auto tu0 = Clk::now();
    CachedTexture entry;
    g.GenTextures(1, &entry.tex);
    g.BindTexture(GL_TEXTURE_2D, entry.tex);
    g.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    g.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    g.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    g.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(d.tex_width), static_cast<GLsizei>(d.tex_height), 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, decode_scratch_.data());
    if (enhance_.mipmaps && d.tex_width > 1u && d.tex_height > 1u) g.GenerateMipmap(GL_TEXTURE_2D);
    tt_upload = std::chrono::duration<double, std::milli>(Clk::now() - tu0).count();
    if (events_budget_ != 0u) {
        std::uint64_t sr = 0, sg = 0, sb = 0, sa = 0;
        const std::size_t texels = static_cast<std::size_t>(d.tex_width) * d.tex_height;
        for (std::size_t i = 0; i < texels && i < decode_scratch_.size(); ++i) {
            const std::uint32_t c = decode_scratch_[i];
            sr += c & 255u; sg += (c >> 8u) & 255u; sb += (c >> 16u) & 255u; sa += c >> 24u;
        }
        const std::size_t n = std::max<std::size_t>(texels, 1u);
        char b[256];
        std::snprintf(b, sizeof b, "GLTEX addr=%08X fmt=%u %ux%u stride=%u swz=%d clutfmt=%u avg=%llu,%llu,%llu,%llu%s", d.tex_addr, d.tex_format,
                      d.tex_width, d.tex_height, d.tex_stride, d.tex_swizzled ? 1 : 0, d.clut_format, (unsigned long long)(sr / n),
                      (unsigned long long)(sg / n), (unsigned long long)(sb / n), (unsigned long long)(sa / n),
                      (sr + sg + sb) / n < 6u && sa / n > 100u ? " BLACK" : "");
        events_.emplace_back(b);
        --events_budget_;
        // RR_TEX_DUMP=hexaddr : full diagnostic of one texture (palette set-up, first palette entries, raw bytes, decoded texels).
        static std::uint32_t dump_addr = [] { const char *e = std::getenv("RR_TEX_DUMP"); return e ? static_cast<std::uint32_t>(std::strtoul(e, nullptr, 16)) : 0u; }();
        if (dump_addr != 0u && d.tex_addr == dump_addr) {
            char l[600];
            std::snprintf(l, sizeof l, "GLTEXDUMP addr=%08X fmt=%u %ux%u stride=%u swz=%d lin=%d clamp=%d%d clutfmt=%u start=%u shift=%u mask=%02X tfunc=%u talpha=%d tdouble=%d",
                          d.tex_addr, d.tex_format, d.tex_width, d.tex_height, d.tex_stride, d.tex_swizzled ? 1 : 0, d.tex_linear ? 1 : 0,
                          d.clamp_u ? 1 : 0, d.clamp_v ? 1 : 0, d.clut_format, d.clut_start, d.clut_shift, d.clut_mask, d.tex_func, d.tex_alpha ? 1 : 0, d.tex_double ? 1 : 0);
            events_.emplace_back(l);
            if (d.clut != nullptr) {
                std::string pal = "GLTEXDUMP clut[0..31]:";
                for (int i = 0; i < 32; ++i) {
                    std::uint32_t e = 0u; std::memcpy(&e, d.clut + i * 4, 4);
                    char t[16]; std::snprintf(t, sizeof t, " %08X", e); pal += t;
                }
                events_.emplace_back(pal);
            }
            std::string raw = "GLTEXDUMP raw bytes:";
            const std::uint32_t nbytes = std::min<std::uint32_t>(128u, texture_bytes_of(d));
            for (std::uint32_t i = 0; i < nbytes && memory_.contains(d.tex_addr + i, 1u); ++i) { char t[8]; std::snprintf(t, sizeof t, " %02X", memory_.load8(d.tex_addr + i)); raw += t; }
            events_.emplace_back(raw);
            for (std::uint32_t y = 0; y < std::min<std::uint32_t>(d.tex_height, 16u); ++y) {
                std::string row = "GLTEXDUMP row" + std::to_string(y) + ":";
                for (std::uint32_t x = 0; x < std::min<std::uint32_t>(d.tex_width, 16u); ++x) { char t[16]; std::snprintf(t, sizeof t, " %08X", decode_scratch_[y * d.tex_width + x]); row += t; }
                events_.emplace_back(row);
            }
        }
    }
    entry.last_use = frame_;
    entry.bytes = d.tex_width * d.tex_height * 4u;
    texture_bytes_ += entry.bytes;
    textures_.emplace(h, entry);
    ++stats_.texture_uploads;
    valid = true;
    return entry.tex;
}

void GlBackend::fallback_software(const GpuDraw &d, GpuPrim prim, const GpuVertex *v, std::size_t count) {
    const prof::Scope prof_scope(prof::kSoftware);
    flush();
    const std::uint32_t target_bytes = d.fb_stride * 272u * (d.fb_format == 3u ? 4u : 2u);
    // The software renderer ignores a draw whose target is not contiguous memory: skip the readbacks it would cost.
    if (static_cast<const psprecomp::GuestMemory &>(memory_).raw_pointer(d.fb_addr, target_bytes) == nullptr) return;
    rb_reason_ = "fb";
    vram_read(d.fb_addr, target_bytes);
    rb_reason_ = "tex";
    if (d.tex_enable && !d.clear_mode) {
        const std::uint32_t bits = texel_bits_of(d.tex_format);
        if (bits != 0u) vram_read(d.tex_addr, std::max(d.tex_stride, d.tex_width) * d.tex_height * bits / 8u);
    }
    rb_reason_ = "z";
    if (d.depth_test || d.clear_depth) vram_read(d.z_addr, d.z_stride * 272u * 2u);
    rb_reason_ = "ge";
    sw_.diag = diag;
    sw_.draw(d, prim, v, count);
    diag = sw_.diag;
    if (d.fb_addr == 0x041FC000u && d.fb_format == 3u && vis_log_enabled()) {
        static int n = 0;
        if (n++ < 3000) {
            const std::uint8_t *p = static_cast<const psprecomp::GuestMemory &>(memory_).raw_pointer(d.fb_addr, 16u);
            std::uint32_t px[4]{}; if (p != nullptr) std::memcpy(px, p, 16);
            char b[260]; std::snprintf(b, sizeof b, "GLVISSW prim=%d n=%zu clear=%d mask=%06X/%02X st=%d(f%u r%02X op%u%u%u) at=%d(%u,%u) tex=%08X f%u px0-3 after=%08X,%08X,%08X,%08X",
                static_cast<int>(prim), count, d.clear_mode ? 1 : 0, d.mask_rgb, d.mask_alpha, d.stencil_test ? 1 : 0, d.stencil_func, d.stencil_ref, d.stencil_sfail, d.stencil_zfail, d.stencil_zpass,
                d.alpha_test ? 1 : 0, d.atest_func, d.atest_ref, d.tex_addr, d.tex_format, px[0], px[1], px[2], px[3]);
            events_.emplace_back(b);
        }
    }
    vram_write(d.fb_addr, target_bytes);
    ++stats_.sw_draws;
    static std::uint32_t logged = 0u;
    if (logged < 80u && events_budget_ != 0u) {
        ++logged; --events_budget_;
        char b[240];
        std::snprintf(b, sizeof b, "GLFALLBACK fb=%08X stride=%u fmt=%u prim=%d n=%zu tex=%d blend=%d(%u,%u,%u) mask=%06X/%02X clear=%d", d.fb_addr, d.fb_stride,
                      d.fb_format, static_cast<int>(prim), count, d.tex_enable ? 1 : 0, d.blend ? 1 : 0, d.blend_eq, d.blend_src, d.blend_dst,
                      d.mask_rgb, d.mask_alpha, d.clear_mode ? 1 : 0);
        events_.emplace_back(b);
    }
}

namespace {
GLenum map_depth_func(std::uint32_t f) {
    static const GLenum table[8] = {GL_NEVER, GL_ALWAYS, GL_EQUAL, GL_NOTEQUAL, GL_LESS, GL_LEQUAL, GL_GREATER, GL_GEQUAL};
    return table[f & 7u];
}
} // namespace

std::uint32_t GlBackend::texture_bytes_of(const GpuDraw &d) {
    if (!d.tex_enable) return 0u;
    const std::uint32_t bits = texel_bits_of(d.tex_format);
    return std::max(d.tex_stride, d.tex_width) * d.tex_height * bits / 8u;
}

bool GlBackend::same_state(const GpuDraw &a, const GpuDraw &b) {
    if (a.clear_mode && a.clear_alpha) return false;   // each clear sets the stencil from its own vertex alpha
    if (a.fb_addr != b.fb_addr || a.fb_stride != b.fb_stride || a.fb_format != b.fb_format || a.z_addr != b.z_addr ||
        a.z_stride != b.z_stride || a.through != b.through || a.clear_mode != b.clear_mode || a.clear_color != b.clear_color ||
        a.clear_alpha != b.clear_alpha || a.clear_depth != b.clear_depth || a.stencil_test != b.stencil_test ||
        (a.stencil_test && (a.stencil_func != b.stencil_func || a.stencil_ref != b.stencil_ref || a.stencil_mask != b.stencil_mask ||
                            a.stencil_sfail != b.stencil_sfail || a.stencil_zfail != b.stencil_zfail || a.stencil_zpass != b.stencil_zpass)) ||
        a.tex_enable != b.tex_enable || a.blend != b.blend || a.alpha_test != b.alpha_test || a.color_test != b.color_test || a.depth_test != b.depth_test ||
        a.depth_write != b.depth_write || a.fog != b.fog || (a.fog && a.fog_color != b.fog_color) || a.mask_rgb != b.mask_rgb || a.mask_alpha != b.mask_alpha ||
        a.scissor_x0 != b.scissor_x0 || a.scissor_y0 != b.scissor_y0 || a.scissor_x1 != b.scissor_x1 || a.scissor_y1 != b.scissor_y1)
        return false;
    if (a.blend && (a.blend_eq != b.blend_eq || a.blend_src != b.blend_src || a.blend_dst != b.blend_dst || a.fix_a != b.fix_a || a.fix_b != b.fix_b))
        return false;
    if (a.color_test && (a.ctest_func != b.ctest_func || a.ctest_ref != b.ctest_ref || a.ctest_mask != b.ctest_mask)) return false;
    if (a.alpha_test && (a.atest_func != b.atest_func || a.atest_ref != b.atest_ref || a.atest_mask != b.atest_mask)) return false;
    if (a.depth_test && a.depth_func != b.depth_func) return false;
    if (a.tex_enable) {
        if (a.tex_addr != b.tex_addr || a.tex_stride != b.tex_stride || a.tex_width != b.tex_width || a.tex_height != b.tex_height ||
            a.tex_format != b.tex_format || a.tex_swizzled != b.tex_swizzled || a.tex_linear != b.tex_linear || a.clamp_u != b.clamp_u ||
            a.clamp_v != b.clamp_v || a.tex_func != b.tex_func || a.tex_alpha != b.tex_alpha || a.tex_double != b.tex_double ||
            a.tex_env != b.tex_env)
            return false;
        if (a.tex_format >= 4u && a.tex_format <= 7u &&
            (a.clut_format != b.clut_format || a.clut_shift != b.clut_shift || a.clut_mask != b.clut_mask || a.clut_start != b.clut_start))
            return false;
    }
    return true;
}

void GlBackend::flush() {
    if (!batch_active_) return;
    batch_active_ = false;
    Rt *rt = batch_rt_;
    batch_d_.clut = batch_clut_;
    const auto t0 = std::chrono::steady_clock::now();
    const prof::Scope prof_scope(prof::kSubmit);
    if (rt != nullptr && !batch_v_.empty()) submit_gl(batch_d_, GL_TRIANGLES, batch_v_.data(), batch_v_.size(), *rt);
    batch_v_.clear();
    flush_ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

namespace {
// 2D (through-mode) triangles - the HUD is built from textured quads split in two - get the same "never sample outside
// your own texel rect" bound as sprites: a quad's two triangles each span the quad's full uv rectangle.
void bound_through_triangles(GpuVertex *t, std::size_t n) {
    for (std::size_t i = 0; i + 2 < n; i += 3) {
        const float u0 = std::min({t[i].u, t[i + 1].u, t[i + 2].u}), u1 = std::max({t[i].u, t[i + 1].u, t[i + 2].u});
        const float v0 = std::min({t[i].v, t[i + 1].v, t[i + 2].v}), v1 = std::max({t[i].v, t[i + 1].v, t[i + 2].v});
        for (int k = 0; k < 3; ++k) { t[i + k].ub[0] = u0; t[i + k].ub[1] = v0; t[i + k].ub[2] = u1; t[i + k].ub[3] = v1; }
    }
}
} // namespace

void GlBackend::queue_gl(const GpuDraw &d, GpuPrim prim, const GpuVertex *v, std::size_t count) {
    const prof::Scope prof_scope(prof::kQueue);
    if (count == 0u) return;
    if (!ok_ || !gl_target(d)) { flush(); fallback_software(d, prim, v, count); return; }
    if (prim == GpuPrim::Lines || prim == GpuPrim::Points) { flush(); draw_gl(d, prim, v, count); return; }
    Rt *rt = get_rt(d);
    if (rt == nullptr) { flush(); fallback_software(d, prim, v, count); return; }
    if (d.tex_enable && !d.clear_mode && d.tex_addr >= rt->addr && d.tex_addr < rt->addr + rt->bytes()) {
        // A draw that reads the very buffer it writes (the game blurs its glow buffer in place with a shifted copy of
        // itself) depends on the draws before it: no batching.  The GPU samples a snapshot of the buffer taken before
        // the draw; a layout the snapshot cannot express goes to the software renderer.
        flush();
        std::uint32_t row0 = 0u;
        static const bool self_sw = std::getenv("RR_SELF_TEXTURE_SW") != nullptr;   // A/B switch: old exact software behaviour
        if (!self_sw && find_source_rt(d, *rt, row0, v, count, true) == rt) draw_gl(d, prim, v, count);
        else fallback_software(d, prim, v, count);
        return;
    }
    const bool uses_clut = d.tex_enable && d.tex_format >= 4u && d.tex_format <= 7u && d.clut != nullptr;
    const bool same = batch_active_ && batch_rt_ == rt && batch_v_.size() < 90000u && same_state(batch_d_, d) &&
                      (!uses_clut || std::memcmp(batch_clut_, d.clut, sizeof batch_clut_) == 0);
    if (!same) flush();
    else ++stats_.merged_draws;
    rt->format = d.fb_format;
    if (rt->cpu_dirty) { flush(); upload_from_vram(*rt); }
    if (!batch_active_) {
        batch_d_ = d;
        if (d.clut != nullptr) std::memcpy(batch_clut_, d.clut, sizeof batch_clut_);
        batch_d_.clut = batch_clut_;
        batch_rt_ = rt;
        batch_active_ = true;
    }
    if (prim == GpuPrim::Sprites) {
        for (std::size_t i = 0; i + 1 < count; i += 2) {
            GpuVertex a = v[i], b = v[i + 1];
            if (a.x > b.x) { std::swap(a.x, b.x); std::swap(a.u, b.u); }
            if (a.y > b.y) { std::swap(a.y, b.y); std::swap(a.v, b.v); }
            if (b.x - a.x <= 0.0f || b.y - a.y <= 0.0f) continue;
            GpuVertex q[4];
            for (GpuVertex &p : q) {
                p = b; p.inv_w = 1.0f;
                p.ub[0] = std::min(a.u, b.u); p.ub[1] = std::min(a.v, b.v); p.ub[2] = std::max(a.u, b.u); p.ub[3] = std::max(a.v, b.v);
            }
            q[0].x = a.x; q[0].y = a.y; q[0].u = a.u; q[0].v = a.v;
            q[1].x = b.x; q[1].y = a.y; q[1].u = b.u; q[1].v = a.v;
            q[2].x = a.x; q[2].y = b.y; q[2].u = a.u; q[2].v = b.v;
            q[3].x = b.x; q[3].y = b.y; q[3].u = b.u; q[3].v = b.v;
            batch_v_.push_back(q[0]); batch_v_.push_back(q[1]); batch_v_.push_back(q[2]);
            batch_v_.push_back(q[1]); batch_v_.push_back(q[3]); batch_v_.push_back(q[2]);
        }
    } else {
        const std::size_t first = batch_v_.size();
        batch_v_.insert(batch_v_.end(), v, v + count);
        if (prim == GpuPrim::Triangles && d.through) bound_through_triangles(batch_v_.data() + first, count);
    }
    rt->gpu_dirty = true;
    rt->last_draw = frame_;
}

void GlBackend::draw(const GpuDraw &d, GpuPrim prim, const GpuVertex *v, std::size_t count) {
    if (count == 0u) return;
    if (ok_ && gl_target(d) && d.scissor_x1 >= 480 && d.fb_stride > 480u) {
        // The columns beyond the visible 480 hold the game's palette strips and are read by the CPU/GE straight from
        // memory.  They never live on the GPU: that part of the draw is done by the software renderer directly in
        // memory, the visible part (if any) by the GPU.
        flush();
        GpuDraw margin = d;
        margin.scissor_x0 = std::max<std::int32_t>(d.scissor_x0, 480);
        if (d.tex_enable && !d.clear_mode) {
            // The texture of a palette-strip draw is often a row the GPU itself rendered this frame (the sun glare's
            // brightness row 0x04195140): a frame-old copy is fine and does not stall the CPU on the GPU.
            static const bool sync_margin = std::getenv("RR_MARGIN_SYNC") != nullptr;
            const std::uint32_t bits = texel_bits_of(d.tex_format);
            const std::uint32_t tb = std::max(d.tex_stride, d.tex_width) * d.tex_height * bits / 8u;
            if (bits != 0u) { if (sync_margin) vram_read(d.tex_addr, tb); else vram_read_late_ok(d.tex_addr, tb); }
        }
        sw_.diag = diag;
        sw_.draw(margin, prim, v, count);
        diag = sw_.diag;
        ++stats_.sw_draws;
        if (d.scissor_x0 >= 480) return;
        GpuDraw visible = d;
        visible.scissor_x1 = 479;
        queue_gl(visible, prim, v, count);
        return;
    }
    if (verify_every_ != 0u && ok_ && gl_target(d) && !d.clear_mode && (!verify_small_only_ || d.fb_stride != 512u) && (++verify_counter_ % verify_every_) == 0u) {
        verify_draw(d, prim, v, count);
        return;
    }
    queue_gl(d, prim, v, count);
}

void GlBackend::draw_gl(const GpuDraw &d, GpuPrim prim, const GpuVertex *v, std::size_t count) {
    const prof::Scope prof_scope(prof::kDirect);
    if (count == 0u) return;
    if (!ok_ || !gl_target(d)) { fallback_software(d, prim, v, count); return; }
    Rt *rt = get_rt(d);
    if (rt == nullptr) { fallback_software(d, prim, v, count); return; }
    rt->format = d.fb_format;
    if (rt->cpu_dirty) upload_from_vram(*rt);

    // Build the vertex list the GPU will draw.
    const GpuVertex *data = v;
    std::size_t n = count;
    GLenum mode = GL_TRIANGLES;
    if (prim == GpuPrim::Sprites) {
        quad_scratch_.clear();
        for (std::size_t i = 0; i + 1 < count; i += 2) {
            GpuVertex a = v[i], b = v[i + 1];
            if (a.x > b.x) { std::swap(a.x, b.x); std::swap(a.u, b.u); }
            if (a.y > b.y) { std::swap(a.y, b.y); std::swap(a.v, b.v); }
            if (b.x - a.x <= 0.0f || b.y - a.y <= 0.0f) continue;
            GpuVertex q[4];
            for (GpuVertex &p : q) {
                p = b; p.inv_w = 1.0f;
                p.ub[0] = std::min(a.u, b.u); p.ub[1] = std::min(a.v, b.v); p.ub[2] = std::max(a.u, b.u); p.ub[3] = std::max(a.v, b.v);
            }
            q[0].x = a.x; q[0].y = a.y; q[0].u = a.u; q[0].v = a.v;
            q[1].x = b.x; q[1].y = a.y; q[1].u = b.u; q[1].v = a.v;
            q[2].x = a.x; q[2].y = b.y; q[2].u = a.u; q[2].v = b.v;
            q[3].x = b.x; q[3].y = b.y; q[3].u = b.u; q[3].v = b.v;
            quad_scratch_.push_back(q[0]); quad_scratch_.push_back(q[1]); quad_scratch_.push_back(q[2]);
            quad_scratch_.push_back(q[1]); quad_scratch_.push_back(q[3]); quad_scratch_.push_back(q[2]);
        }
        data = quad_scratch_.data();
        n = quad_scratch_.size();
        if (n == 0u) return;
    } else if (prim == GpuPrim::Triangles && d.through) {
        quad_scratch_.assign(v, v + count);
        bound_through_triangles(quad_scratch_.data(), quad_scratch_.size());
        data = quad_scratch_.data();
    } else if (prim == GpuPrim::Lines) {
        mode = GL_LINES;
    } else if (prim == GpuPrim::Points) {
        mode = GL_POINTS;
    }

    submit_gl(d, mode, data, n, *rt);
}

// A texture that is a rectangle of rows of a GPU render target (the game renders the screen into a small buffer,
// reads the screen back as a texture for effects, blurs a buffer with a shifted copy of itself) is sampled straight
// from that target: no copy back to memory.  The game declares such textures with a generous height (512 rows of a
// 272-row screen), so what counts is the part the vertices actually reach.  With `allow_self` the source may be the
// target being drawn (the caller then samples a snapshot of it).
GlBackend::Rt *GlBackend::find_source_rt(const GpuDraw &d, const Rt &dst, std::uint32_t &row0, const GpuVertex *v, std::size_t n, bool allow_self) {
    static const bool disabled = std::getenv("RR_NO_RT_TEXTURE") != nullptr;   // A/B switch for debugging
    if (disabled || !d.tex_enable || d.clear_mode || d.tex_format > 3u || d.tex_swizzled || d.tex_width == 0u || d.tex_height == 0u) return nullptr;
    float umax = 0.0f, vmax = 0.0f, umin = 0.0f, vmin = 0.0f;
    if (n != 0u) {
        umin = umax = v[0].u; vmin = vmax = v[0].v;
        for (std::size_t i = 1; i < n; ++i) { umin = std::min(umin, v[i].u); umax = std::max(umax, v[i].u); vmin = std::min(vmin, v[i].v); vmax = std::max(vmax, v[i].v); }
    }
    // Targets can overlap in memory (the 1x1 sun brightness probe 0x041FFF00 sits in the last rows of the 64x64 reflection
    // map 0x041FE000): a target that starts exactly at the texture address is the one the game drew the texture into, so
    // it is tried first.
    static const bool exact_first = std::getenv("RR_RT_EXACT_FIRST") != nullptr;   // experiment (a46 default made a readback every frame)
    for (int pass = exact_first ? 0 : 1; pass < 2; ++pass)
    for (auto &up : rts_) {
        Rt *src = up.get();
        if (pass == 0 && src->addr != d.tex_addr) continue;
        if ((src == &dst && !allow_self) || src->format != d.tex_format) continue;
        if (d.tex_addr < src->addr) continue;
        if (src->stride != d.tex_stride) {
            // A one-row texture inside another render target (the sun-glare brightness probe 0x041FFF00 is a 1x1
            // texture read from its own 512-wide target): sample it on the GPU instead of reading it back.
            static const bool onepx_off = std::getenv("RR_SUN_DIRECT_OFF") != nullptr;
            static const bool any_row = std::getenv("RR_ONEROW_ANY") != nullptr;   // experiment: wider one-row textures too
            if (onepx_off || d.tex_height != 1u || (any_row ? d.tex_width > std::min<std::uint32_t>(src->stride, 480u) : d.tex_width != 1u) || src == &dst) continue;
            const std::uint32_t rb = src->stride * (src->format == 3u ? 4u : 2u);
            const std::uint32_t o = d.tex_addr - src->addr;
            if (o % rb != 0u || o / rb >= src->rows) continue;
            row0 = o / rb;
            src_onepx_ = d.tex_width == 1u;   // 1x1: every coordinate means that one pixel (it wraps)
            return src;
        }
        const std::uint32_t row_bytes = src->stride * (src->format == 3u ? 4u : 2u);
        const std::uint32_t off = d.tex_addr - src->addr;
        if (off % row_bytes != 0u) continue;
        const std::uint32_t r0 = off / row_bytes;
        if (r0 >= src->rows) continue;
        const std::uint32_t cols = std::min<std::uint32_t>(src->stride, 480u);
        const bool rows_ok = r0 + d.tex_height <= src->rows || (n != 0u && vmin >= 0.0f && vmax <= static_cast<float>(src->rows - r0) + 2.0f);
        const bool cols_ok = d.tex_width <= cols || (n != 0u && umin >= 0.0f && umax <= static_cast<float>(cols) + 2.0f);
        if (!rows_ok || !cols_ok) continue;
        row0 = r0;
        return src;
    }
    return nullptr;
}

void GlBackend::submit_gl(const GpuDraw &d, unsigned mode, const GpuVertex *data, std::size_t n, Rt &rt_ref) {
    if (const int hide = g_hide_class.load(std::memory_order_relaxed); hide != 0 && !d.clear_mode) {
        const bool through = d.through;
        const bool blend = d.blend, atest = d.alpha_test;
        const bool hit = hide == 5 ? through
                       : hide == 6 ? (through && d.blend && d.blend_src == 10u && d.fb_addr != 0x04088000u)
                       : hide == 7 ? (through && d.fb_addr == 0x04088000u)
                       : through ? false
                       : hide == 1 ? (blend && atest)
                       : hide == 2 ? (blend && !atest)
                       : hide == 3 ? (!blend && atest)
                       : (!blend && !atest && d.tex_enable);
        if (hit) return;
    }
    {
        // RR_HUD_BOX_LOG=N: log (at most N lines) every 2D draw whose vertices cover most of the bottom-right 128x128 box of
        // the screen (x 352..480, y 144..272 - where the speedometer sprite sits), to find why that box is faintly tinted.
        static const int box_log = [] { const char *e = std::getenv("RR_HUD_BOX_LOG"); return e ? std::max(0, std::atoi(e)) : 0; }();
        static int box_logged = 0;
        static std::uint64_t box_seen[64]; static int box_seen_n = 0;   // each distinct draw kind is logged once (the first test's budget went on one animated sprite)
        if (box_log > 0 && box_logged < box_log && box_seen_n < 64 && d.through && !d.clear_mode && n >= 3u) {
            float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
            for (std::size_t k = 0; k < n; ++k) { x0 = std::min(x0, data[k].x); y0 = std::min(y0, data[k].y); x1 = std::max(x1, data[k].x); y1 = std::max(y1, data[k].y); }
            const float ox = std::max(0.0f, std::min(x1, 480.0f) - std::max(x0, 352.0f));
            const float oy = std::max(0.0f, std::min(y1, 272.0f) - std::max(y0, 144.0f));
            // (the first 40 lines of the first test were all full-screen fades from the intro: only draws that fit the corner count)
            bool fresh = false;
            if (ox >= 40.0f && oy >= 40.0f && (x1 - x0) <= 300.0f && (y1 - y0) <= 300.0f) {
                std::uint64_t sig = 1469598103934665603ull;
                for (const std::uint64_t part : {static_cast<std::uint64_t>(d.tex_enable ? d.tex_addr : 0u), static_cast<std::uint64_t>(d.tex_format), static_cast<std::uint64_t>(d.tex_width),
                                                 static_cast<std::uint64_t>(d.tex_height), static_cast<std::uint64_t>(d.blend_src), static_cast<std::uint64_t>(d.blend_dst),
                                                 static_cast<std::uint64_t>(d.blend ? 1 : 0), static_cast<std::uint64_t>(d.alpha_test ? 1 : 0), static_cast<std::uint64_t>(d.tex_func),
                                                 static_cast<std::uint64_t>(d.clut_format)}) sig = (sig ^ part) * 1099511628211ull;
                fresh = true;
                for (int q = 0; q < box_seen_n; ++q) if (box_seen[q] == sig) { fresh = false; break; }
                if (fresh) box_seen[box_seen_n++] = sig;
            }
            if (fresh) {
                ++box_logged;
                char b[420];
                std::snprintf(b, sizeof b,
                              "HUDBOX f=%llu n=%zu bbox=%.1f,%.1f-%.1f,%.1f tex=%d addr=%08X fmt=%u %ux%u stride=%u lin=%d tfunc=%u talpha=%d blend=%d eq=%u src=%u dst=%u fixa=%06X fixb=%06X atest=%d func=%u ref=%u col0=%.0f,%.0f,%.0f,%.0f fb=%08X clut=%u tdouble=%d swz=%d",
                              static_cast<unsigned long long>(frame_), n, static_cast<double>(x0), static_cast<double>(y0), static_cast<double>(x1), static_cast<double>(y1),
                              d.tex_enable ? 1 : 0, d.tex_addr, d.tex_format, d.tex_width, d.tex_height, d.tex_stride, d.tex_linear ? 1 : 0, d.tex_func, d.tex_alpha ? 1 : 0,
                              d.blend ? 1 : 0, d.blend_eq, d.blend_src, d.blend_dst, d.fix_a & 0xFFFFFFu, d.fix_b & 0xFFFFFFu, d.alpha_test ? 1 : 0, d.atest_func, d.atest_ref,
                              static_cast<double>(data[0].r), static_cast<double>(data[0].g), static_cast<double>(data[0].b), static_cast<double>(data[0].a), d.fb_addr, d.clut_format, d.tex_double ? 1 : 0, d.tex_swizzled ? 1 : 0);
                events_.emplace_back(b);
            }
        }
    }
    {
        // RR_BIGDRAW_LOG=N: log (at most N distinct kinds of draw using the glare texture 0x04195540; the first version's budget went on intro fades) big blended textured draws into the scene buffers (bbox >= 200x100 px): the
        // glare/light-shaft layer over the road looks blocky, and this shows which texture/buffer it is sampled from.
        static const int gl_log = [] { const char *e = std::getenv("RR_BIGDRAW_LOG"); return e ? std::max(0, std::atoi(e)) : 0; }();
        static int gl_logged = 0;
        static std::uint64_t gl_seen[64]; static int gl_seen_n = 0;
        if (gl_log > 0 && gl_logged < gl_log && gl_seen_n < 64 && d.tex_enable && !d.clear_mode && n >= 2u) {
            float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
            for (std::size_t k = 0; k < n; ++k) { x0 = std::min(x0, data[k].x); y0 = std::min(y0, data[k].y); x1 = std::max(x1, data[k].x); y1 = std::max(y1, data[k].y); }
            if (d.tex_addr == 0x04195540u) {
                std::uint64_t sig = 1469598103934665603ull;
                for (const std::uint64_t part : {static_cast<std::uint64_t>(d.tex_addr), static_cast<std::uint64_t>(d.tex_format), static_cast<std::uint64_t>(d.tex_width),
                                                 static_cast<std::uint64_t>(d.tex_height), static_cast<std::uint64_t>(d.blend_src), static_cast<std::uint64_t>(d.blend_dst),
                                                 static_cast<std::uint64_t>(d.through ? 1 : 0), static_cast<std::uint64_t>(d.tex_func), static_cast<std::uint64_t>(d.tex_linear ? 1 : 0),
                                                 static_cast<std::uint64_t>(d.fb_addr)}) sig = (sig ^ part) * 1099511628211ull;
                bool fresh = true;
                for (int q = 0; q < gl_seen_n; ++q) if (gl_seen[q] == sig) { fresh = false; break; }
                if (fresh) {
                    gl_seen[gl_seen_n++] = sig; ++gl_logged;
                    char b[360];
                    std::snprintf(b, sizeof b, "BIGDRAW f=%llu n=%zu prim=%d through=%d bbox=%.0f,%.0f-%.0f,%.0f tex=%08X fmt=%u %ux%u stride=%u lin=%d tfunc=%u blend eq=%u src=%u dst=%u atest=%d fb=%08X z=%d col0=%.0f,%.0f,%.0f,%.0f",
                                  static_cast<unsigned long long>(frame_), n, static_cast<int>(mode), d.through ? 1 : 0, static_cast<double>(x0), static_cast<double>(y0), static_cast<double>(x1), static_cast<double>(y1),
                                  d.tex_addr, d.tex_format, d.tex_width, d.tex_height, d.tex_stride, d.tex_linear ? 1 : 0, d.tex_func, d.blend_eq, d.blend_src, d.blend_dst, d.alpha_test ? 1 : 0, d.fb_addr,
                                  d.depth_test ? 1 : 0, static_cast<double>(data[0].r), static_cast<double>(data[0].g), static_cast<double>(data[0].b), static_cast<double>(data[0].a));
                    events_.emplace_back(b);
                }
            }
        }
    }
    if (d.through && n == 48u && d.tex_enable && d.tex_width == 64u && d.tex_height == 256u && std::getenv("RR_DEBUG_CLASS") != nullptr) {
        static int rays = 0;
        if (rays < 6 && (frame_ % 7u) == 0u) {
            ++rays;
            for (std::size_t k = 0; k < n; ++k) {
                char b[200];
                std::snprintf(b, sizeof b, "GLRAY f=%llu v%zu xy=%.1f,%.1f uv=%.1f,%.1f col=%.0f,%.0f,%.0f,%.0f iw=%.4f z=%.1f", static_cast<unsigned long long>(frame_), k,
                              data[k].x, data[k].y, data[k].u, data[k].v, data[k].r, data[k].g, data[k].b, data[k].a, data[k].inv_w, data[k].z);
                events_.emplace_back(b);
            }
        }
    }
    if (const int tp = g_thru_pos.load(std::memory_order_relaxed); tp >= 0 && d.through && !d.clear_mode) {
        static std::uint64_t cur_frame = ~0ull; static int idx = 0;
        if (frame_ != cur_frame) { cur_frame = frame_; idx = 0; }
        const int w = g_thru_width.load(std::memory_order_relaxed);
        const int i = idx++;
        if (i >= tp * w && i < tp * w + w) {
            static int shown = 0;
            if (w == 1 && (frame_ % 20u) == 0u && shown < 600) {
                ++shown;
                float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
                for (std::size_t k = 0; k < n; ++k) { x0 = std::min(x0, data[k].x); x1 = std::max(x1, data[k].x); y0 = std::min(y0, data[k].y); y1 = std::max(y1, data[k].y);
                                                       u0 = std::min(u0, data[k].u); u1 = std::max(u1, data[k].u); v0 = std::min(v0, data[k].v); v1 = std::max(v1, data[k].v); }
                char b[400];
                std::snprintf(b, sizeof b, "GLHIDE f=%llu idx=%d mode=%u n=%zu xy=%.0f,%.0f-%.0f,%.0f uv=%.0f,%.0f-%.0f,%.0f fb=%08X tex=%d@%08X f%u %ux%u lin=%d tfunc=%d blend=%d(eq%u s%u d%u fa%06X fb%06X) atest=%d(f%u r%u) col=%d,%d,%d,%d",
                              static_cast<unsigned long long>(frame_), i, mode, n, x0, y0, x1, y1, u0, v0, u1, v1, d.fb_addr, d.tex_enable ? 1 : 0, d.tex_addr, d.tex_format, d.tex_width, d.tex_height,
                              d.tex_linear ? 1 : 0, static_cast<int>(d.tex_func), d.blend ? 1 : 0, d.blend_eq, d.blend_src, d.blend_dst, d.fix_a, d.fix_b, d.alpha_test ? 1 : 0, d.atest_func, d.atest_ref,
                              static_cast<int>(data[0].r), static_cast<int>(data[0].g), static_cast<int>(data[0].b), static_cast<int>(data[0].a));
                events_.emplace_back(b);
            }
            return;
        }
    }
    {
        // RR_DEBUG_CLASS: list the screen-space ("through") draws of a few frames to find which effect draws full-screen strips.
        static const bool dbg = std::getenv("RR_DEBUG_CLASS") != nullptr;
        static int logged = 0;
        static std::uint64_t last_frame = ~0ull; static int in_frame = 0;
        if (dbg && frame_ != last_frame) { last_frame = frame_; in_frame = 0; }
        if (dbg && d.through && !d.clear_mode && (frame_ % 300u) == 150u && logged < 4000 && in_frame < 80 && n > 0u) {
            ++logged; ++in_frame;
            float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
            for (std::size_t i = 0; i < n; ++i) { x0 = std::min(x0, data[i].x); y0 = std::min(y0, data[i].y); x1 = std::max(x1, data[i].x); y1 = std::max(y1, data[i].y); }
            char b[360];
            std::snprintf(b, sizeof b, "GLTHRU f=%llu mode=%u n=%zu xy=%.0f,%.0f-%.0f,%.0f fb=%08X tex=%d@%08X f%u %ux%u lin=%d tfunc=%u talpha=%d blend=%d(eq%u s%u d%u fa%06X fb%06X) atest=%d(f%u r%u) col=%.0f,%.0f,%.0f,%.0f uv0=%.1f,%.1f dw=%d",
                          (unsigned long long)frame_, mode, n, x0, y0, x1, y1, d.fb_addr, d.tex_enable ? 1 : 0, d.tex_addr, d.tex_format, d.tex_width, d.tex_height,
                          d.tex_linear ? 1 : 0, d.tex_func, d.tex_alpha ? 1 : 0, d.blend ? 1 : 0, d.blend_eq, d.blend_src, d.blend_dst, d.fix_a, d.fix_b,
                          d.alpha_test ? 1 : 0, d.atest_func, d.atest_ref, data[0].r, data[0].g, data[0].b, data[0].a, data[0].u, data[0].v, d.depth_write ? 1 : 0);
            events_.emplace_back(b);
        }
    }
    Rt *rt = &rt_ref;
    Gl &g = *gl_;
    bool tex_valid = false;
    unsigned tex = 0u;
    std::uint32_t src_row0 = 0u;
    src_onepx_ = false;
    Rt *src_rt = find_source_rt(d, *rt, src_row0, data, n, true);
    const bool src_onepx = src_onepx_;
    if (static const bool envlog = std::getenv("RR_ENVMAP_LOG") != nullptr; envlog && d.tex_enable && d.tex_addr >= 0x041F0000u && d.tex_addr < 0x04200000u) {
        static int shown = 0;
        if (shown < 400) {
            ++shown;
            float u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
            for (std::size_t k = 0; k < n; ++k) { u0 = std::min(u0, data[k].u); u1 = std::max(u1, data[k].u); v0 = std::min(v0, data[k].v); v1 = std::max(v1, data[k].v); }
            char b[300];
            std::snprintf(b, sizeof b, "ENVMAP f=%llu tex=%08X %ux%u stride=%u fmt=%u fb=%08X n=%zu uv=%.1f,%.1f-%.1f,%.1f src_rt=%s%08X stride=%u fmt=%u row0=%u",
                          static_cast<unsigned long long>(frame_), d.tex_addr, d.tex_width, d.tex_height, d.tex_stride, d.tex_format, d.fb_addr, n, u0, v0, u1, v1,
                          src_rt ? "" : "none ", src_rt ? src_rt->addr : 0u, src_rt ? src_rt->stride : 0u, src_rt ? src_rt->format : 0u, src_row0);
            events_.emplace_back(b);
        }
    }
    if (src_rt != nullptr) {
        if (src_rt->cpu_dirty) upload_from_vram(*src_rt);
        resolve_stencil(*src_rt);
        resolve_rt(*src_rt, false);
        if (src_rt->addr == 0x041FC000u && vis_log_enabled() && src_rt->format == 3u) {
            static std::vector<std::uint8_t> tmp(512u * 4u);
            readback_to(*src_rt, tmp.data(), std::min<std::uint32_t>(480u, src_rt->stride), 0u, 0u);   // logs via vis_log
        }
        tex = src_rt->tex; tex_valid = true;
        ++stats_.rt_tex;
        if (const char *dump = std::getenv("RR_RT_DUMP")) {   // RR_RT_DUMP=addr,frame: save that render target (as sampled) to rtdump_<addr>_<frame>.png
            unsigned da = 0; unsigned long long df = 0;
            static unsigned long long done_frame = ~0ull;
            if (std::sscanf(dump, "%x,%llu", &da, &df) == 2 && src_rt->addr == da && frame_ == df && done_frame != df) {
                done_frame = df;
                const int cw = static_cast<int>(std::min<std::uint32_t>(src_rt->stride, 480u)) * scale_;
                const int chh = static_cast<int>(std::min<std::uint32_t>(src_rt->rows, 272u)) * scale_;
                std::vector<std::uint8_t> px(static_cast<std::size_t>(cw) * chh * 4u), flip(px.size());
                g.BindFramebuffer(GL_READ_FRAMEBUFFER, src_rt->fbo);
                g.ReadPixels(0, 0, cw, chh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                for (int y = 0; y < chh; ++y) std::memcpy(&flip[static_cast<std::size_t>(y) * cw * 4u], &px[static_cast<std::size_t>(y) * cw * 4u], static_cast<std::size_t>(cw) * 4u);
                for (std::size_t i = 3; i < flip.size(); i += 4) flip[i] = 255;
                char name[64]; std::snprintf(name, sizeof name, "rtdump_%08X_%llu.png", da, df);
                write_png(name, flip.data(), static_cast<std::uint32_t>(cw), static_cast<std::uint32_t>(chh));
            }
        }
        if (src_rt == rt) {
            // The draw reads the buffer it writes: sample a snapshot taken before the draw.
            if (g.snap_tex == 0u || g.snap_w != rt->w || g.snap_h != rt->h) {
                if (g.snap_fbo) g.DeleteFramebuffers(1, &g.snap_fbo);
                if (g.snap_tex) g.DeleteTextures(1, &g.snap_tex);
                g.GenTextures(1, &g.snap_tex);
                g.BindTexture(GL_TEXTURE_2D, g.snap_tex);
                g.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                g.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                g.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, rt->w, rt->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                g.GenFramebuffers(1, &g.snap_fbo);
                g.BindFramebuffer(GL_FRAMEBUFFER, g.snap_fbo);
                g.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g.snap_tex, 0);
                g.snap_w = rt->w; g.snap_h = rt->h;
            }
            g.Disable(GL_SCISSOR_TEST);
            g.ColorMask(1, 1, 1, 1);
            g.BindFramebuffer(GL_READ_FRAMEBUFFER, rt->fbo);
            g.BindFramebuffer(GL_DRAW_FRAMEBUFFER, g.snap_fbo);
            g.BlitFramebuffer(0, 0, rt->w, rt->h, 0, 0, rt->w, rt->h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            tex = g.snap_tex;
        }
    } else if (d.tex_enable && !d.clear_mode) {
        {
            // Diagnostics: a texture that lives inside a render target but could not be sampled from it directly.
            static std::uint32_t miss_logged = 0u;
            const std::uint32_t tb = texture_bytes_of(d);
            if (miss_logged < 60u && events_budget_ != 0u && tb != 0u)
                for (auto &o : rts_)
                    if (o->gpu_dirty && ranges_overlap(d.tex_addr, tb, o->addr, o->bytes())) {
                        float umax = 0, vmax = 0, umin = 1e9f, vmin = 1e9f;
                        for (std::size_t i = 0; i < n; ++i) { umax = std::max(umax, data[i].u); vmax = std::max(vmax, data[i].v); umin = std::min(umin, data[i].u); vmin = std::min(vmin, data[i].v); }
                        char b[300];
                        std::snprintf(b, sizeof b, "GLTEXMISS tex=%08X f%u %ux%u stride=%u swz=%d lin=%d uv=%.0f,%.0f-%.0f,%.0f rt=%08X stride=%u fmt=%u rows=%u dst=%08X", d.tex_addr, d.tex_format, d.tex_width, d.tex_height, d.tex_stride, d.tex_swizzled ? 1 : 0, d.tex_linear ? 1 : 0, umin, vmin, umax, vmax, o->addr, o->stride, o->format, o->rows, rt->addr);
                        events_.emplace_back(b); --events_budget_; ++miss_logged;
                        break;
                    }
        }
        tex = get_texture(d, tex_valid);
        if (!tex_valid) {
            ++stats_.tex_failed;
            if (events_budget_ != 0u && stats_.tex_failed <= 200u) {
                char b[200];
                std::snprintf(b, sizeof b, "GLTEXFAIL addr=%08X fmt=%u %ux%u stride=%u", d.tex_addr, d.tex_format, d.tex_width, d.tex_height, d.tex_stride);
                events_.emplace_back(b);
                --events_budget_;
            }
        }
    }

    if (rt->ms_fbo != 0u) {
        if (rt->ms_fill) {   // rows changed in tex since the last draw: replicate them into every sample
            g.Disable(GL_SCISSOR_TEST);
            g.ColorMask(1, 1, 1, 1);
            g.BindFramebuffer(GL_READ_FRAMEBUFFER, rt->fbo);
            g.BindFramebuffer(GL_DRAW_FRAMEBUFFER, rt->ms_fbo);
            const int y0 = std::max(0, rt->ms_fill_y0), y1 = std::min(rt->h, rt->ms_fill_y1);
            if (y1 > y0) g.BlitFramebuffer(0, y0, rt->w, y1, 0, y0, rt->w, y1, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            rt->ms_fill = false;
        }
        rt->ms_dirty = true; rt->ms_depth_dirty = true;
        g.BindFramebuffer(GL_FRAMEBUFFER, rt->ms_fbo);
    } else {
        g.BindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
    }
    g.Viewport(0, 0, rt->w, rt->h);
    g.UseProgram(g.program);
    g.BindVertexArray(g.vao);
    {
        // TAA: 3D geometry gets a sub-pixel offset that changes every frame (a Halton(2,3) sequence); screen-space draws stay put.
        float jx = 0.0f, jy = 0.0f;
        if (g_aa_mode.load(std::memory_order_relaxed) == 2 && g.taa_prog != 0u && !d.through && !d.clear_mode && rt->stride == 512u && mode != GL_POINTS) {
            const auto halton = [](unsigned i, unsigned base) { float f = 1.0f, r = 0.0f; while (i > 0u) { f /= static_cast<float>(base); r += f * static_cast<float>(i % base); i /= base; } return r; };
            const unsigned idx = static_cast<unsigned>(frame_ % 8u) + 1u;
            jx = (halton(idx, 2u) - 0.5f) / static_cast<float>(scale_);
            jy = (halton(idx, 3u) - 0.5f) / static_cast<float>(scale_);
        }
        g.Uniform2f(g.loc_jit, jx, jy);
    }
    g.Uniform2f(g.loc_rt, static_cast<float>(rt->stride), 272.0f);
    g.Uniform2f(g.loc_pt, mode == GL_POINTS ? static_cast<float>(scale_) : 0.0f, 0.0f);
    g.Uniform1i(g.loc_clear, d.clear_mode ? 1 : 0);
    g.Uniform1i(g.loc_tex, 0);
    const bool use_tex = tex_valid && !d.clear_mode;
    g.Uniform1i(g.loc_tex_enable, use_tex ? 1 : 0);
    if (use_tex) {
        g.ActiveTexture(GL_TEXTURE0);
        g.BindTexture(GL_TEXTURE_2D, tex);
        // 3D textures that are filtered, opaque-or-blended (not alpha-tested, whose coverage mipmaps would thin out) and
        // not render targets get trilinear + anisotropic sampling.
        const int clamp_bits = src_rt != nullptr ? 3 : ((d.clamp_u ? 1 : 0) | (d.clamp_v ? 2 : 0));
        const bool mip = enhance_.mipmaps && g.mip_ready && src_rt == nullptr && d.tex_linear && !d.through && !d.alpha_test && mode == GL_TRIANGLES &&
                         d.tex_width > 1u && d.tex_height > 1u;
        // Cut-out textures (trees, fences) the game samples with nearest filtering show as large dark texel squares at high
        // resolution; optionally smooth them with bilinear filtering (the alpha test then rounds the edges).
        const bool smooth_cut = enhance_.smooth_cutouts && d.alpha_test && !d.through && src_rt == nullptr && mode == GL_TRIANGLES;
        // RR_RT_NEAREST_SMOOTH=1: 2D copies of a render target that the game samples with NEAREST filtering (glow/glare buffers
        // stretched up) are filtered smoothly instead (A/B test for the glare looking blocky).  RR_RT_NEAREST_LOG=N logs them.
        static const bool rt_near_smooth = std::getenv("RR_RT_NEAREST_SMOOTH") != nullptr;
        static const int rt_near_log = [] { const char *e = std::getenv("RR_RT_NEAREST_LOG"); return e ? std::atoi(e) : 0; }();
        const bool force_lin = rt_near_smooth && src_rt != nullptr && !src_onepx && d.through && !d.tex_linear;
        if (rt_near_log > 0 && src_rt != nullptr && !src_onepx && d.through && !d.tex_linear) {
            static int logged = 0;
            static std::uint64_t seen[32]; static int nseen = 0;
            const std::uint64_t sig = (static_cast<std::uint64_t>(d.tex_addr) << 24u) ^ (static_cast<std::uint64_t>(d.tex_width) << 12u) ^ d.tex_height ^ (static_cast<std::uint64_t>(d.fb_addr) << 40u);
            bool dup = false; for (int i = 0; i < nseen; ++i) dup |= seen[i] == sig;
            if (!dup && nseen < 32 && logged < rt_near_log) {
                seen[nseen++] = sig; ++logged;
                char lb[300];
                std::snprintf(lb, sizeof lb, "RTNEAR tex=%08X %ux%u stride=%u -> fb=%08X blend=%d src=%u dst=%u func=%u sprite=%d", d.tex_addr, d.tex_width, d.tex_height, d.tex_stride,
                             d.fb_addr, d.blend ? 1 : 0, d.blend_src, d.blend_dst, d.tex_func, mode == GL_TRIANGLES ? 0 : 1);
                events_.emplace_back(lb);
            }
        }
        g.BindSampler(0, mip ? g.mip_samplers[clamp_bits] : g.samplers[(d.tex_linear || smooth_cut || force_lin) ? 1 : 0][clamp_bits]);
        // The sun glare (texture 0x04195540: a 64x256 gradient stretched ~8x on screen) is not HUD art: the crisp-texel ramp turned its
        // smooth gradient into flat 8-pixel steps (RR_GLARE_CRISP=1 restores that for comparison).
        static const bool glare_crisp = std::getenv("RR_GLARE_CRISP") != nullptr;
        if (!glare_crisp && src_rt == nullptr && d.through && d.tex_linear && d.tex_alpha && (d.tex_addr == 0x04195540u || d.blend_src == 6u)) {
            static bool told = false;
            if (!told) { told = true; char b[160]; std::snprintf(b, sizeof b, "GLARERAMP off for tex=%08X src=%u dst=%u tex_alpha=%d ui_crisp=%.2f", d.tex_addr, d.blend_src, d.blend_dst, d.tex_alpha ? 1 : 0, static_cast<double>(enhance_.ui_crisp)); events_.emplace_back(b); }
        }
        g.Uniform1f(g.loc_ui_ramp, (enhance_.ui_crisp > 0.0f && src_rt == nullptr && d.through && d.tex_linear && d.tex_alpha && (glare_crisp || (d.tex_addr != 0x04195540u && d.blend_src != 6u)))
                                       ? 1.0f - 0.85f * std::min(enhance_.ui_crisp, 1.0f) : 0.0f);
        g.Uniform1i(g.loc_tex_func, static_cast<GLint>(d.tex_func));
        g.Uniform1i(g.loc_tex_alpha, d.tex_alpha ? 1 : 0);
        g.Uniform1i(g.loc_tex_double, d.tex_double ? 1 : 0);
        g.Uniform3f(g.loc_tex_env, static_cast<float>(d.tex_env & 255u) / 255.0f, static_cast<float>((d.tex_env >> 8u) & 255u) / 255.0f,
                    static_cast<float>((d.tex_env >> 16u) & 255u) / 255.0f);
        if (src_rt != nullptr && src_onepx) {
            // every coordinate lands on the probe pixel's top-left corner (texel 0 of its scaled block, clamped)
            g.Uniform2f(g.loc_tex_size, 1.0e9f, 1.0e9f);
            g.Uniform2f(g.loc_tex_off, 0.0f, static_cast<float>(src_row0) * 1.0e9f / 272.0f);
            g.Uniform1i(g.loc_tex_snap, 0);
            g.Uniform1f(g.loc_dst_scale, 0.0f);
        } else if (src_rt != nullptr) {
            g.Uniform2f(g.loc_tex_size, static_cast<float>(src_rt->stride), 272.0f);
            g.Uniform2f(g.loc_tex_off, 0.0f, static_cast<float>(src_row0));
            g.Uniform1i(g.loc_tex_snap, (d.tex_linear || force_lin) ? 0 : 1);
            g.Uniform1f(g.loc_dst_scale, static_cast<float>(rt->w) / static_cast<float>(rt->stride));
            // A 3D surface mapped with a render target (the cars' 64x64 reflection map 0x041FE000): keep the target's full
            // internal resolution.  2D copies (the glow buffers) still filter between PSP texels to avoid blocky boxes.
            static const bool env_sharp_off = std::getenv("RR_ENVMAP_SHARP_OFF") != nullptr;
            // RR_RT_PSP_FILTER_OFF=1: never filter between PSP texels (A/B test for the sun glare looking soft; the glow
            // buffers may then show as blocks).
            static const bool psp_filter_off = std::getenv("RR_RT_PSP_FILTER_OFF") != nullptr;
            g.Uniform1i(g.loc_rt_psp_filter, (!psp_filter_off && (d.through || env_sharp_off)) ? 1 : 0);
        } else {
            g.Uniform2f(g.loc_tex_size, static_cast<float>(d.tex_width), static_cast<float>(d.tex_height));
            g.Uniform2f(g.loc_tex_off, 0.0f, 0.0f);
            g.Uniform1i(g.loc_tex_snap, 0);
            g.Uniform1f(g.loc_dst_scale, 0.0f);
        }
    }
    g.Uniform1i(g.loc_fog, d.fog && !d.clear_mode ? 1 : 0);
    g.Uniform3f(g.loc_fog_color, static_cast<float>(d.fog_color & 255u) / 255.0f, static_cast<float>((d.fog_color >> 8u) & 255u) / 255.0f,
                static_cast<float>((d.fog_color >> 16u) & 255u) / 255.0f);
    const bool atest = d.alpha_test && !d.clear_mode;
    g.Uniform1i(g.loc_atest, atest ? 1 : 0);
    // RR_HUD_FAINT_CUT=n (1..40): in 2D blended draws, fragments whose alpha is below n/255 are dropped (some HUD sprites carry
    // an almost transparent but not empty background that shows as a faint box).
    static const int hud_faint_cut = [] { const char *e = std::getenv("RR_HUD_FAINT_CUT"); return e ? std::clamp(std::atoi(e), 0, 80) : 0; }();
    // default (no env var): only the speedometer's 128x128 render-target sprite (0x041D3FC0), whose empty texels carry alpha 1-5; the
    // title screen's big faint circle got a hard edge when the cut covered every 2D blend.
    static const bool hud_cut_explicit = std::getenv("RR_HUD_FAINT_CUT") != nullptr;
    g.Uniform1i(g.loc_acut, (atest && d.blend && !d.through && src_rt == nullptr) ? g_alpha_cutoff.load(std::memory_order_relaxed)
                           : (hud_cut_explicit ? (hud_faint_cut > 0 && d.through && d.blend && d.tex_enable && !d.clear_mode && src_rt == nullptr && d.tex_addr != 0x04195540u && d.blend_src != 6u)
                                                  : (d.through && d.blend && d.tex_enable && !d.clear_mode && src_rt == nullptr && d.tex_addr == 0x041D3FC0u && d.tex_width == 128u && d.tex_height == 128u)) ? (hud_cut_explicit ? hud_faint_cut : 8) : 0);
    {
        // RR_DEBUG_CLASS=1: tint 3D draws by kind so a screenshot shows what trees etc. really are.
        // red = alpha-tested + nearest, yellow = alpha-tested + bilinear, blue = blended + nearest, cyan = blended + bilinear.
        static const bool dbg = std::getenv("RR_DEBUG_CLASS") != nullptr;
        float r = 0, gg = 0, b = 0;
        if (dbg && use_tex && !d.through && src_rt == nullptr) {
            const bool lin = d.tex_linear || (enhance_.smooth_cutouts && d.alpha_test);
            // (almost every 3D draw is alpha-tested, so only blended draws are marked: blue = nearest, cyan = bilinear;
            //  blended draws that are also alpha-tested are marked red / yellow)
            if (d.blend) { if (atest) { r = 1; gg = lin ? 1.0f : 0.0f; } else { b = 1; gg = lin ? 1.0f : 0.0f; } }
        }
        g.Uniform3f(g.loc_dbg, r, gg, b);
    }
    g.Uniform1i(g.loc_atest_func, static_cast<GLint>(d.atest_func));
    g.Uniform1i(g.loc_atest_ref, static_cast<GLint>(d.atest_ref));
    g.Uniform1i(g.loc_atest_mask, static_cast<GLint>(d.atest_mask));
    g.Uniform1i(g.loc_ctest, d.color_test && !d.clear_mode ? 1 : 0);
    g.Uniform1i(g.loc_ct_func, static_cast<GLint>(d.ctest_func));
    g.Uniform1i(g.loc_ct_ref, static_cast<GLint>(d.ctest_ref));
    g.Uniform1i(g.loc_ct_mask, static_cast<GLint>(d.ctest_mask));

    // Scissor (in internal-resolution pixels).
    const int sx0 = std::max(d.scissor_x0, 0), sy0 = std::max(d.scissor_y0, 0);
    const int sx1 = std::min<int>(d.scissor_x1, static_cast<int>(rt->stride) - 1), sy1 = std::min<int>(d.scissor_y1, static_cast<int>(rt->rows) - 1);
    if (sx1 < sx0 || sy1 < sy0) return;
    g.Enable(GL_SCISSOR_TEST);
    g.Scissor(sx0 * scale_, sy0 * scale_, (sx1 - sx0 + 1) * scale_, (sy1 - sy0 + 1) * scale_);
    g.Disable(GL_CULL_FACE);
    g.Disable(GL_DITHER);

    // Colour write masks.
    const auto channel_masked = [](std::uint32_t mask, int shift) { return ((mask >> shift) & 0xFFu) == 0xFFu; };
    bool wr = !channel_masked(d.mask_rgb, 0), wg = !channel_masked(d.mask_rgb, 8), wb = !channel_masked(d.mask_rgb, 16);
    bool wa = d.mask_alpha != 0xFFu && (d.stencil_test || (d.fb_format == 3u && !d.blend));
    if (d.clear_mode) {
        wr = wr && d.clear_color; wg = wg && d.clear_color; wb = wb && d.clear_color;
        wa = (d.mask_alpha != 0xFFu) && d.clear_alpha;
    }
    g.ColorMask(wr, wg, wb, wa);

    // Depth.
    if (d.clear_mode) {
        if (d.clear_depth) { g.Enable(GL_DEPTH_TEST); g.DepthFunc(GL_ALWAYS); g.DepthMask(1); }
        else { g.Disable(GL_DEPTH_TEST); g.DepthMask(0); }
        if (!d.clear_color && !d.clear_alpha && !d.clear_depth) return;
    } else if (d.depth_test) {
        g.Enable(GL_DEPTH_TEST);
        g.DepthFunc(map_depth_func(d.depth_func));
        g.DepthMask(d.depth_write ? 1 : 0);
    } else {
        g.Disable(GL_DEPTH_TEST);
        g.DepthMask(0);
    }

    // Stencil (the PSP keeps it in the framebuffer alpha; here it lives in the RT's stencil buffer).
    {
        const auto quant = [&](std::uint32_t v) -> GLint {
            if (d.fb_format == 1u) return (v & 0x80u) != 0u ? 255 : 0;
            if (d.fb_format == 2u) return static_cast<GLint>((v & 0xF0u) | (v >> 4u));
            return static_cast<GLint>(v & 0xFFu);
        };
        const auto op = [](std::uint32_t o) -> GLenum {
            switch (o) { case 1: return GL_ZERO; case 2: return GL_REPLACE; case 3: return GL_INVERT; case 4: return GL_INCR; case 5: return GL_DECR; default: return GL_KEEP; }
        };
        if (d.clear_mode) {
            if (d.clear_alpha && d.fb_format != 0u && d.mask_alpha != 0xFFu && n > 0u) {
                g.Enable(GL_STENCIL_TEST);
                g.StencilFunc(GL_ALWAYS, quant(static_cast<std::uint32_t>(std::clamp(data[0].a, 0.0f, 255.0f) + 0.5f)), 0xFFu);
                g.StencilOp(GL_REPLACE, GL_REPLACE, GL_REPLACE);
                g.StencilMask(0xFFu);
                if (d.fb_format != 0u) { rt->stencil_used = true; rt->alpha_stale = true; }
            } else g.Disable(GL_STENCIL_TEST);
        } else if (d.stencil_test && d.fb_format != 0u) {
            g.Enable(GL_STENCIL_TEST);
            g.StencilFunc(map_depth_func(d.stencil_func == 0u ? 0u : d.stencil_func), quant(d.stencil_ref), d.stencil_mask);
            g.StencilOp(op(d.stencil_sfail), op(d.stencil_zfail), op(d.stencil_zpass));
            g.StencilMask(~d.mask_alpha & 0xFFu);
            if (d.fb_format != 0u) { rt->stencil_used = true; rt->alpha_stale = true; }
        } else g.Disable(GL_STENCIL_TEST);
    }

    // Blending.
    int alpha_mode = 0;
    GLenum alpha_fs = GL_ONE, alpha_fd = GL_ZERO, alpha_eq = GL_FUNC_ADD, col_fs = GL_ONE, col_fd = GL_ZERO, col_eq = GL_FUNC_ADD;
    if (d.blend && !d.clear_mode) {
        const auto factor = [&](std::uint32_t f, bool is_src, std::uint32_t fix) -> GLenum {
            switch (f) {
            case 0: return is_src ? GL_DST_COLOR : GL_SRC_COLOR;
            case 1: return is_src ? GL_ONE_MINUS_DST_COLOR : GL_ONE_MINUS_SRC_COLOR;
            case 2: return GL_SRC_ALPHA;
            case 3: return GL_ONE_MINUS_SRC_ALPHA;
            case 4: return GL_DST_ALPHA;
            case 5: return GL_ONE_MINUS_DST_ALPHA;
            case 6: alpha_mode = 1; return GL_SRC_ALPHA;
            case 7: alpha_mode = 2; return GL_SRC_ALPHA;
            case 8: return GL_DST_ALPHA;
            case 9: return GL_ONE_MINUS_DST_ALPHA;
            default: (void)fix; return GL_CONSTANT_COLOR;
            }
        };
        const GLenum fs = factor(d.blend_src, true, d.fix_a), fd = factor(d.blend_dst, false, d.fix_b);
        // The single GL blend constant serves whichever side uses a fixed colour.
        const std::uint32_t fix = d.blend_src >= 10u ? d.fix_a : d.fix_b;
        g.BlendColor(static_cast<float>(fix & 255u) / 255.0f, static_cast<float>((fix >> 8u) & 255u) / 255.0f,
                     static_cast<float>((fix >> 16u) & 255u) / 255.0f, 1.0f);
        GLenum eq = GL_FUNC_ADD;
        switch (d.blend_eq) {
        case 1: eq = GL_FUNC_SUBTRACT; break;
        case 2: eq = GL_FUNC_REVERSE_SUBTRACT; break;
        case 3: eq = GL_MIN; break;
        case 4: eq = GL_MAX; break;
        default: break;
        }
        // A 32-bit target blends its alpha with the alpha counterparts of the same factors (a constant colour has alpha 1).
        const auto alpha_of = [](GLenum f) -> GLenum {
            switch (f) {
            case GL_SRC_COLOR: return GL_SRC_ALPHA;
            case GL_ONE_MINUS_SRC_COLOR: return GL_ONE_MINUS_SRC_ALPHA;
            case GL_DST_COLOR: return GL_DST_ALPHA;
            case GL_ONE_MINUS_DST_COLOR: return GL_ONE_MINUS_DST_ALPHA;
            case GL_CONSTANT_COLOR: return GL_CONSTANT_ALPHA;
            default: return f;
            }
        };
        const bool blend_alpha = d.fb_format == 3u;
        alpha_fs = blend_alpha ? alpha_of(fs) : GL_ONE;
        alpha_fd = blend_alpha ? alpha_of(fd) : GL_ZERO;
        alpha_eq = blend_alpha ? eq : GL_FUNC_ADD;
        col_fs = fs; col_fd = fd; col_eq = eq;
        g.Enable(GL_BLEND);
        g.BlendEquationSeparate(eq, alpha_eq);
        g.BlendFuncSeparate(fs, fd, alpha_fs, alpha_fd);
    } else {
        g.Disable(GL_BLEND);
    }
    g.Uniform1i(g.loc_alpha_mode, alpha_mode);
    if (alpha_mode != 0) {
        // The shader's alpha is a blend factor here, not the value to store: write the real alpha in a separate
        // pass first (alpha channel only, no blending, no depth writes), then the colour pass below.
        if (wa) {
            const bool blended_alpha = d.fb_format == 3u;
            if (blended_alpha) {
                // src alpha factor is applied in the shader (mode 3/4); the destination side uses the GL factor.
                g.Uniform1i(g.loc_alpha_mode, alpha_mode + 2);
                g.BlendEquationSeparate(GL_FUNC_ADD, alpha_eq);
                g.BlendFuncSeparate(GL_ZERO, GL_ZERO, GL_ONE, alpha_fd);
            } else {
                g.Uniform1i(g.loc_alpha_mode, 0);
                g.Disable(GL_BLEND);
            }
            g.ColorMask(0, 0, 0, 1);
            const bool dw = d.depth_test && d.depth_write;
            if (dw) g.DepthMask(0);
            g.BindBuffer(GL_ARRAY_BUFFER, g.vbo);
            g.BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(n * sizeof(GpuVertex)), data, GL_STREAM_DRAW);
            g.DrawArrays(mode, 0, static_cast<GLsizei>(n));
            if (dw) g.DepthMask(1);
            g.Uniform1i(g.loc_alpha_mode, alpha_mode);
            g.Enable(GL_BLEND);
            g.BlendEquationSeparate(col_eq, alpha_eq);
            g.BlendFuncSeparate(col_fs, col_fd, alpha_fs, alpha_fd);
        }
        g.ColorMask(wr, wg, wb, 0);
    }

    const prof::Scope gl_scope(prof::kGlCall);
    g.BindBuffer(GL_ARRAY_BUFFER, g.vbo);
    g.BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(n * sizeof(GpuVertex)), data, GL_STREAM_DRAW);
    // Scale vertex coordinates to the render target: the vertex shader divides by the logical size, so no change.
    g.DrawArrays(mode, 0, static_cast<GLsizei>(n));
    {
        float ymin = 1e9f, ymax = -1e9f;
        for (std::size_t i = 0; i < n; ++i) { ymin = std::min(ymin, data[i].y); ymax = std::max(ymax, data[i].y); }
        // Rows the primitives can touch: floor(ymin) .. ceil(ymax)-1 (a point lands on floor(y)).  No extra margin: rows
        // next to a thin target (a palette row, say) may hold CPU-written data that a stale readback would clobber.
        static const bool old_margin = std::getenv("RR_OLD_MARGIN") != nullptr;   // A/B switch
        const int top = static_cast<int>(std::floor(ymin)) - (old_margin ? 1 : 0), bot = old_margin ? static_cast<int>(std::ceil(ymax)) + 1 : (mode == GL_POINTS ? static_cast<int>(std::floor(ymax)) : static_cast<int>(std::ceil(ymax)) - 1);
        const int r0 = std::max(std::max(d.scissor_y0, 0), top);
        const int r1 = std::min(std::min<int>(std::min(d.scissor_y1, 271), static_cast<int>(rt->rows) - 1), std::max(top, bot));
        for (int r = r0; r <= r1; ++r) rt->rows_dirty.set(static_cast<std::size_t>(r));
    }
    rt->gpu_dirty = rt->rows_dirty.any();
    rt->last_draw = frame_;
    ++stats_.gl_draws;
}

void GlBackend::verify_draw(const GpuDraw &d, GpuPrim prim, const GpuVertex *v, std::size_t count) {
    flush();
    Rt *rt = get_rt(d);
    if (rt == nullptr) { draw_gl(d, prim, v, count); return; }
    rt->format = d.fb_format;
    if (rt->cpu_dirty) upload_from_vram(*rt);
    if (!shadow_) { shadow_ = std::make_unique<psprecomp::GuestMemory>(32u * 1024u * 1024u); shadow_sw_ = std::make_unique<SoftwareBackend>(*shadow_); }
    const std::uint32_t bpp = rt->format == 3u ? 4u : 2u;
    std::uint8_t *sfb = shadow_->raw_pointer(rt->addr, rt->bytes());
    if (sfb == nullptr) { draw_gl(d, prim, v, count); return; }
    // Make memory current for the texture, then copy everything the draw reads into the scratch memory.
    std::uint32_t tex_bytes = 0u;
    if (d.tex_enable) {
        const std::uint32_t bits = texel_bits_of(d.tex_format);
        tex_bytes = std::max(d.tex_stride, d.tex_width) * d.tex_height * bits / 8u;
        vram_read(d.tex_addr, tex_bytes);
        const std::uint8_t *ts = static_cast<const psprecomp::GuestMemory &>(memory_).raw_pointer(d.tex_addr, tex_bytes);
        std::uint8_t *td = shadow_->raw_pointer(d.tex_addr, tex_bytes);
        if (ts != nullptr && td != nullptr) std::memcpy(td, ts, tex_bytes);
    }
    readback_to(*rt, sfb, std::min<std::uint32_t>(480u, rt->stride));
    if (d.depth_test) {
        // Copy the GPU depth buffer into the scratch memory so the software draw sees the same depth state.
        std::uint16_t *sz = reinterpret_cast<std::uint16_t *>(shadow_->raw_pointer(d.z_addr, d.z_stride * 272u * 2u));
        if (sz != nullptr) {
            std::vector<float> depth(static_cast<std::size_t>(rt->w) * rt->h);
            resolve_rt(*rt, true);
            gl_->BindFramebuffer(GL_READ_FRAMEBUFFER, rt->fbo);
            gl_->PixelStorei(GL_PACK_ALIGNMENT, 1);
            gl_->ReadPixels(0, 0, rt->w, rt->h, 0x1902 /*GL_DEPTH_COMPONENT*/, 0x1406 /*GL_FLOAT*/, depth.data());
            const int half = scale_ / 2;
            for (std::uint32_t y = 0; y < rt->rows; ++y)
                for (std::uint32_t x = 0; x < std::min<std::uint32_t>(480u, rt->stride); ++x) {
                    const float f = depth[static_cast<std::size_t>(y * scale_ + half) * rt->w + x * scale_ + half];
                    sz[y * d.z_stride + x] = static_cast<std::uint16_t>(std::min(65535.0f, std::max(0.0f, f * 65535.0f + 0.5f)));
                }
        }
    }
    std::vector<std::uint8_t> before(sfb, sfb + rt->bytes());
    shadow_sw_->draw(d, prim, v, count);
    draw_gl(d, prim, v, count);
    std::vector<std::uint8_t> after(rt->bytes());
    readback_to(*rt, after.data(), std::min<std::uint32_t>(480u, rt->stride));
    int bad = 0, touched = 0, bx0 = 9999, by0 = 9999, bx1 = -1, by1 = -1;
    std::uint32_t sample_x = 0, sample_y = 0, sample_sw = 0, sample_gl = 0;
    for (std::uint32_t y = 0; y < rt->rows; ++y) {
        for (std::uint32_t x = 0; x < std::min<std::uint32_t>(480u, rt->stride); ++x) {
            const std::size_t at = (static_cast<std::size_t>(y) * rt->stride + x) * bpp;
            std::uint32_t p0 = 0, p1 = 0, p2 = 0;
            std::memcpy(&p0, before.data() + at, bpp); std::memcpy(&p1, sfb + at, bpp); std::memcpy(&p2, after.data() + at, bpp);
            if (p0 != p1) ++touched;
            if (p1 == p2) continue;
            const std::uint32_t e1 = pixel_expand(rt->format, p1), e2 = pixel_expand(rt->format, p2);
            int md = 0;
            for (int k = 0; k < 4; ++k) md = std::max(md, std::abs(static_cast<int>((e1 >> (8 * k)) & 255u) - static_cast<int>((e2 >> (8 * k)) & 255u)));
            if (md <= 24) continue;
            if (bad == 0) { sample_x = x; sample_y = y; sample_sw = e1; sample_gl = e2; }
            ++bad;
            bx0 = std::min<int>(bx0, x); by0 = std::min<int>(by0, y); bx1 = std::max<int>(bx1, x); by1 = std::max<int>(by1, y);
        }
    }
    ++verify_total_;
    if (bad >= 20) {
        ++verify_bad_;
        static std::uint32_t logged = 0u;
        if (logged < 250u && events_budget_ != 0u) {
            ++logged; --events_budget_;
            char b[700];
            int n = std::snprintf(b, sizeof b,
                "GLVERIFY depth=%d bad=%d sw_touched=%d bbox=%d,%d-%d,%d first=(%u,%u sw=%08X gl=%08X) fb=%08X fmt=%u prim=%d n=%zu tex=%d(%08X f%u %ux%u s%u swz=%d lin=%d clamp=%d%d clutfmt=%u/%u/%02X/%u) tf=%u talpha=%d tdbl=%d "
                "blend=%d(%u,%u,%u) at=%d(%u,%u) mask=%06X/%02X stencil=%d through=%d scis=%d,%d-%d,%d",
                d.depth_test ? static_cast<int>(d.depth_func) + 1 : 0, bad, touched, bx0, by0, bx1, by1, sample_x, sample_y, sample_sw, sample_gl, d.fb_addr, d.fb_format, static_cast<int>(prim), count,
                d.tex_enable ? 1 : 0, d.tex_addr, d.tex_format, d.tex_width, d.tex_height, d.tex_stride, d.tex_swizzled ? 1 : 0, d.tex_linear ? 1 : 0,
                d.clamp_u ? 1 : 0, d.clamp_v ? 1 : 0, d.clut_format, d.clut_shift, d.clut_mask, d.clut_start, d.tex_func, d.tex_alpha ? 1 : 0,
                d.tex_double ? 1 : 0, d.blend ? 1 : 0, d.blend_eq, d.blend_src, d.blend_dst, d.alpha_test ? 1 : 0, d.atest_func, d.atest_ref,
                d.mask_rgb, d.mask_alpha, d.stencil_test ? 1 : 0, d.through ? 1 : 0, d.scissor_x0, d.scissor_y0, d.scissor_x1, d.scissor_y1);
            std::string line(b, static_cast<std::size_t>(std::max(n, 0)));
            for (std::size_t i = 0; i < count && i < 6u; ++i) {
                char vb[160];
                std::snprintf(vb, sizeof vb, " v%zu=(%.1f,%.1f z%.0f w%.4f uv%.1f,%.1f c%.0f,%.0f,%.0f,%.0f)", i, v[i].x, v[i].y, v[i].z, v[i].inv_w, v[i].u, v[i].v, v[i].r, v[i].g, v[i].b, v[i].a);
                line += vb;
            }
            events_.emplace_back(std::move(line));
        }
    }
}

bool GlBackend::has_target(std::uint32_t address) const {
    const_cast<GlBackend *>(this)->flush();
    const Rt *rt = find_rt(address);
    return rt != nullptr && rt->last_draw + 2u >= frame_ && !rt->cpu_dirty;
}

bool GlBackend::read_native_rgba(std::uint32_t address, std::uint8_t *rgba) {
    flush();
    Rt *rt = find_rt(address);
    if (rt == nullptr) return false;
    Gl &g = *gl_;
    resolve_rt(*rt, false);
    readback_scratch_.resize(static_cast<std::size_t>(rt->w) * rt->h * 4u);
    g.BindFramebuffer(GL_READ_FRAMEBUFFER, rt->fbo);
    g.PixelStorei(GL_PACK_ALIGNMENT, 1);
    g.ReadPixels(0, 0, rt->w, rt->h, GL_RGBA, GL_UNSIGNED_BYTE, readback_scratch_.data());
    const int half = scale_ / 2;
    for (int y = 0; y < 272; ++y)
        for (int x = 0; x < 480; ++x) {
            const std::size_t src_at = (static_cast<std::size_t>(y * scale_ + half) * rt->w + x * scale_ + half) * 4u;
            std::uint8_t *o = rgba + (static_cast<std::size_t>(y) * 480u + x) * 4u;
            o[0] = readback_scratch_[src_at]; o[1] = readback_scratch_[src_at + 1]; o[2] = readback_scratch_[src_at + 2]; o[3] = 255u;
        }
    return true;
}

bool GlBackend::read_scaled_rgba(std::uint32_t address, std::vector<std::uint8_t> &rgba, int &width, int &height) {
    flush();
    Rt *rt = find_rt(address);
    if (rt == nullptr) return false;
    Gl &g = *gl_;
    resolve_rt(*rt, false);
    readback_scratch_.resize(static_cast<std::size_t>(rt->w) * rt->h * 4u);
    g.BindFramebuffer(GL_READ_FRAMEBUFFER, rt->fbo);
    g.PixelStorei(GL_PACK_ALIGNMENT, 1);
    g.ReadPixels(0, 0, rt->w, rt->h, GL_RGBA, GL_UNSIGNED_BYTE, readback_scratch_.data());
    width = 480 * scale_; height = 272 * scale_;
    rgba.resize(static_cast<std::size_t>(width) * height * 4u);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const std::uint8_t *s = readback_scratch_.data() + (static_cast<std::size_t>(y) * rt->w + x) * 4u;
            std::uint8_t *o = rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4u;
            o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = 255u;
        }
    return true;
}

namespace {
void letterbox(int win_w, int win_h, int &x, int &y, int &w, int &h) {
    const float aspect = 480.0f / 272.0f;
    float vw = static_cast<float>(win_w), vh = vw / aspect;
    if (vh > static_cast<float>(win_h)) { vh = static_cast<float>(win_h); vw = vh * aspect; }
    w = static_cast<int>(vw); h = static_cast<int>(vh);
    x = (win_w - w) / 2; y = (win_h - h) / 2;
}
} // namespace

bool GlBackend::present_target(std::uint32_t address, int win_w, int win_h) {
    flush();
    Rt *rt = find_rt(address);
    if (rt == nullptr) return false;
    if (rt->cpu_dirty) upload_from_vram(*rt);
    resolve_rt(*rt, false);
    Gl &g = *gl_;
    if (post_wanted(false)) {
        const unsigned shown = g_aa_mode.load(std::memory_order_relaxed) == 2 ? taa_pass(*rt) : rt->tex;
        post_pass(shown, rt->w, rt->h, 480 * scale_, win_w, win_h);
        capture_default_fb(win_w, win_h);
        return true;
    }
    int lx, ly, lw, lh;
    letterbox(win_w, win_h, lx, ly, lw, lh);
    g.Disable(GL_SCISSOR_TEST);
    g.ColorMask(1, 1, 1, 1);
    g.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    g.Disable(GL_STENCIL_TEST);   // the window framebuffer may carry its own stencil bits
    g.Viewport(0, 0, win_w, win_h);
    g.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    g.Clear(GL_COLOR_BUFFER_BIT);
    g.BindFramebuffer(GL_READ_FRAMEBUFFER, rt->fbo);
    // Source row 0 is the top of the PSP image; the window origin is bottom-left, so flip while blitting.
    g.BlitFramebuffer(0, 0, 480 * scale_, 272 * scale_, lx, ly + lh, lx + lw, ly, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    capture_default_fb(win_w, win_h);
    return true;
}

void GlBackend::present_rgba(const std::uint8_t *rgba, int width, int height, int win_w, int win_h) {
    Gl &g = *gl_;
    if (g.scratch_tex == 0u || g.scratch_w != width || g.scratch_h != height) {
        if (g.scratch_fbo) g.DeleteFramebuffers(1, &g.scratch_fbo);
        if (g.scratch_tex) g.DeleteTextures(1, &g.scratch_tex);
        g.GenTextures(1, &g.scratch_tex);
        g.BindTexture(GL_TEXTURE_2D, g.scratch_tex);
        g.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        g.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        g.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        g.GenFramebuffers(1, &g.scratch_fbo);
        g.BindFramebuffer(GL_FRAMEBUFFER, g.scratch_fbo);
        g.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g.scratch_tex, 0);
        g.scratch_w = width; g.scratch_h = height;
    }
    g.BindTexture(GL_TEXTURE_2D, g.scratch_tex);
    g.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    g.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    if (width > 480 && post_wanted(true)) {
        // A frame rendered at internal resolution (the on-screen keyboard overlay) is processed like the normal screen.
        post_pass(g.scratch_tex, width, height, width, win_w, win_h);
        capture_default_fb(win_w, win_h);
        return;
    }
    int lx, ly, lw, lh;
    letterbox(win_w, win_h, lx, ly, lw, lh);
    g.Disable(GL_SCISSOR_TEST);
    g.ColorMask(1, 1, 1, 1);
    g.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    g.Disable(GL_STENCIL_TEST);   // the window framebuffer may carry its own stencil bits
    g.Viewport(0, 0, win_w, win_h);
    g.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    g.Clear(GL_COLOR_BUFFER_BIT);
    g.BindFramebuffer(GL_READ_FRAMEBUFFER, g.scratch_fbo);
    g.BlitFramebuffer(0, 0, width, height, lx, ly + lh, lx + lw, ly, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    capture_default_fb(win_w, win_h);
}

void GlBackend::capture_default_fb(int win_w, int win_h) {
    if (!capture_pending_) return;
    capture_pending_ = false;
    Gl &g = *gl_;
    int lx, ly, lw, lh;
    letterbox(win_w, win_h, lx, ly, lw, lh);
    if (lw <= 0 || lh <= 0) return;
    std::vector<std::uint8_t> raw(static_cast<std::size_t>(lw) * lh * 4u);
    g.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    g.PixelStorei(GL_PACK_ALIGNMENT, 1);
    g.ReadPixels(lx, ly, lw, lh, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());
    if (g.GetError() != 0u) return;
    capture_.resize(raw.size());
    for (int y = 0; y < lh; ++y) {   // the window origin is bottom-left: flip to top row first
        const std::uint8_t *src = raw.data() + static_cast<std::size_t>(lh - 1 - y) * lw * 4u;
        std::uint8_t *dst = capture_.data() + static_cast<std::size_t>(y) * lw * 4u;
        for (int x = 0; x < lw; ++x) { dst[x * 4] = src[x * 4]; dst[x * 4 + 1] = src[x * 4 + 1]; dst[x * 4 + 2] = src[x * 4 + 2]; dst[x * 4 + 3] = 255u; }
    }
    capture_w_ = lw; capture_h_ = lh; capture_ok_ = true;
}

bool GlBackend::take_capture(std::vector<std::uint8_t> &rgba, int &width, int &height) {
    capture_pending_ = false;
    if (!capture_ok_) return false;
    capture_ok_ = false;
    rgba.swap(capture_);
    width = capture_w_; height = capture_h_;
    return true;
}

std::string GlBackend::brief() const {
    char b[384];
    std::snprintf(b, sizeof b, "rt_tex=%llu gl_draws=%llu sw_draws=%llu tex_up=%llu tex_hit=%llu readbacks=%llu uploads=%llu merged=%llu tex_failed=%llu verify=%llu/%llu rb_rows=%llu rb[ge,fb,tex,z,gltex,other]=%llu,%llu,%llu,%llu,%llu,%llu scale=%d",
                  static_cast<unsigned long long>(stats_.rt_tex), static_cast<unsigned long long>(stats_.gl_draws), static_cast<unsigned long long>(stats_.sw_draws),
                  static_cast<unsigned long long>(stats_.texture_uploads), static_cast<unsigned long long>(stats_.texture_hits),
                  static_cast<unsigned long long>(stats_.readbacks), static_cast<unsigned long long>(stats_.uploads), static_cast<unsigned long long>(stats_.merged_draws), static_cast<unsigned long long>(stats_.tex_failed), static_cast<unsigned long long>(verify_bad_), static_cast<unsigned long long>(verify_total_), static_cast<unsigned long long>(stats_.readback_rows), (unsigned long long)rb_by_reason_[0], (unsigned long long)rb_by_reason_[1], (unsigned long long)rb_by_reason_[2], (unsigned long long)rb_by_reason_[3], (unsigned long long)rb_by_reason_[4], (unsigned long long)rb_by_reason_[5], scale_);
    return b;
}

} // namespace rr
