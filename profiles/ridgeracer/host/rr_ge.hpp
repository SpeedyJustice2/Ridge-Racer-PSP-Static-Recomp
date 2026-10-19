#pragma once
#include <unordered_map>

// PSP graphics-engine command processor.  Display lists are executed
// synchronously when they are enqueued or their stall address moves.  All GE
// state (registers, matrices, CLUT) persists across lists exactly like the real
// hardware; only the program counter and call stack belong to a list.

#include "rr_gpu.hpp"

#include <array>
#include <string>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace psprecomp { class GuestMemory; }

namespace rr {

struct GeInterrupt {
    std::uint32_t token{};
    bool finish{};          // false = SIGNAL callback, true = FINISH callback
    std::int32_t callback_id{};
};

enum class GeListStatus : std::uint32_t { Completed = 0, Queued = 1, Drawing = 2, Stalling = 3, Paused = 4 };

class GeProcessor {
public:
    GeProcessor(psprecomp::GuestMemory &memory, GpuBackend &backend);

    std::int32_t enqueue(std::uint32_t address, std::uint32_t stall, std::int32_t callback_id, bool head);
    void update_stall(std::int32_t list_id, std::uint32_t stall);
    void resume();                                  // sceGeContinue
    [[nodiscard]] GeListStatus status(std::int32_t list_id) const;
    [[nodiscard]] bool idle() const { return queue_.empty(); }
    // Interrupts raised since the last call, in order.
    std::vector<GeInterrupt> take_interrupts();

    void report() const;
    void set_frame(std::uint64_t frame);
    [[nodiscard]] double peek_draw_ms() const { return draw_ms_; }
    [[nodiscard]] double take_draw_ms() { const double v = draw_ms_; draw_ms_ = 0.0; return v; }
    [[nodiscard]] std::uint64_t take_frame_purple() { const auto n = frame_purple_; frame_purple_ = 0u; return n; }
    // Diagnostics: one-line counters, and a trace of the next N commands.
    [[nodiscard]] std::string brief() const;
    void trace_commands(std::uint32_t count) { trace_budget_ = count; }

private:
    std::unordered_map<std::uint32_t, std::uint32_t> tex_cur_, tex_prev_;   // RR_TEXTIME: textures used by 3D draws this frame / last frame
    std::uint32_t trace_budget_{};
    std::string trace_line_;
    struct Frame { std::uint32_t pc, offset, base; };
    struct List {
        std::int32_t id{};
        std::uint32_t pc{}, stall{};
        std::int32_t callback_id{};
        bool paused{}, stalled{}, done{};
        std::vector<Frame> stack;
    };
    struct Vtype;
    struct Stats {
        std::uint64_t lists{}, finishes{}, signals{}, commands{}, prims{}, vertices{}, clears{};
        std::uint64_t triangles{}, sprites{}, lines{}, points{}, culled{}, clipped{};
        std::uint64_t unsupported_bezier{}, unsupported_lighting{}, unsupported_texgen{}, unsupported_tex_format{};
        std::uint64_t skinned{}, textured{}, through{}, lit_prims{};
    } stats_;

    void run_queue();
    void run_list(List &list);
    void execute_prim(std::uint32_t data);
    void load_matrix_data(std::uint32_t command, std::uint32_t data);
    void load_clut(std::uint32_t data);
    void block_transfer();
    [[nodiscard]] std::uint32_t relative_address(std::uint32_t data) const;
    [[nodiscard]] GpuDraw build_draw() const;
    [[nodiscard]] float reg_float(std::uint32_t command) const;

    std::uint64_t frame_{};
    std::uint64_t frame_purple_{};
    double draw_ms_{};
    std::uint32_t clut_src_{};
    std::uint64_t clut_hash_{};
    std::uint32_t purple_draws_logged_{};
    std::uint64_t purple_frame_logged_for_{};
    struct XferRec { std::uint64_t frame; std::uint32_t src, dst, dst_end, w, h, bpp, skipped; };
    std::deque<XferRec> xfers_;
    struct TargetRec { std::uint32_t fb_addr, stride, format; std::uint64_t frame; };
    std::vector<TargetRec> targets_;
    [[nodiscard]] std::string provenance(std::uint32_t address, std::uint32_t length) const;
    std::uint32_t purple_in_frame_{};
    std::map<std::string, std::uint64_t> seen_states_;
    psprecomp::GuestMemory &memory_;
    GpuBackend &backend_;
    std::array<std::uint32_t, 256> reg_{};
    std::array<float, 96> bones_{};
    std::array<float, 12> world_{}, view_{}, tex_matrix_{};
    std::array<float, 16> proj_{};
    std::uint32_t bone_cursor_{}, world_cursor_{}, view_cursor_{}, proj_cursor_{}, tex_cursor_{};
    std::array<std::uint8_t, 1024> clut_{};
    std::uint32_t vertex_addr_{}, index_addr_{};
    std::uint32_t call_site_{}, call_target_{};   // last GE CALL (diagnostics)
    std::uint32_t offset_{};
    std::deque<List> queue_;
    std::vector<GeInterrupt> interrupts_;
    std::int32_t next_list_id_{1};
    std::vector<GpuVertex> scratch_;
};

} // namespace rr
