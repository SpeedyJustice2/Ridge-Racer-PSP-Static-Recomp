#pragma once

// Optional lightweight profiler (RR_PROF=1): accumulates wall time per section and prints it with the 300-frame
// heartbeat.  Sections are inclusive and may nest (a GL draw call happens inside a submit, inside a flush, inside a
// queue, inside a prim, inside a list ...).  When RR_PROF is not set every scope costs one predictable branch.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace rr::prof {

enum Id : unsigned {
    kGeQueue,     // GeProcessor::run_queue (everything the GE does)
    kGePrim,      // execute_prim: vertex fetch/transform + draw
    kBackendDraw, // backend draw() entry (queue/margin/software)
    kQueue,       // queue_gl (batching)
    kSubmit,      // batch flush -> submit_gl
    kTexture,     // get_texture (hash + cache + decode/upload)
    kVramRead,    // vram_read (checks / GPU->CPU readbacks)
    kReadback,    // readback of a render target
    kUpload,      // upload_from_vram
    kGlCall,      // BufferData + DrawArrays
    kDirect,      // draw_gl (unbatched path)
    kSoftware,    // fallback_software
    kPrimSetup,   // execute_prim: build_draw + state hashes
    kPrimVerts,   // execute_prim: vertex fetch / transform / lighting loop
    kPrimAssemble,// execute_prim: clipping, culling, triangle assembly
    kCount
};

inline bool enabled = [] { return std::getenv("RR_PROF") != nullptr; }();
inline double ms[kCount]{};
inline std::uint64_t calls[kCount]{};
inline const char *const names[kCount] = {"ge_queue", "ge_prim", "be_draw", "queue", "submit", "texture", "vram_read",
                                          "readback", "upload", "gl_call", "direct", "software", "prim_setup", "prim_verts", "prim_asm"};

struct Scope {
    Id id;
    std::chrono::steady_clock::time_point t0;
    explicit Scope(Id i) : id(i) { if (enabled) t0 = std::chrono::steady_clock::now(); }
    ~Scope() {
        if (!enabled) return;
        ms[id] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ++calls[id];
    }
};

// One line for the heartbeat; resets the counters.  Empty when disabled.
inline std::string take_report() {
    if (!enabled) return {};
    std::string out = "HEARTBEAT PROF (ms per 300 frames / calls):";
    char b[96];
    for (unsigned i = 0; i < kCount; ++i) {
        std::snprintf(b, sizeof b, " %s=%.0f/%llu", names[i], ms[i], static_cast<unsigned long long>(calls[i]));
        out += b;
        ms[i] = 0.0; calls[i] = 0;
    }
    return out;
}

} // namespace rr::prof
