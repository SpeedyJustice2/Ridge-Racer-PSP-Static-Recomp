#include "rr_ge_headless.hpp"

#include "psprecomp/guest_memory.hpp"
#include "rr_log.hpp"

#include <algorithm>
#include <map>
#include <sstream>
#include <string>

namespace rr {

using psprecomp::GuestMemory;

namespace {
constexpr std::uint32_t kMaxCommandsPerList = 1u << 20u;
}

namespace {
// Draw statistics: what the game actually sends to the GPU.  Used to decide
// what the real renderer must support.
struct GeStats {
    std::uint64_t commands{}, prims{}, vertices{}, clears{}, lists{}, finishes{};
    std::uint64_t texture_enabled_prims{}, through_prims{}, blend_prims{}, alphatest_prims{};
    std::uint64_t depth_prims{}, lit_prims{}, indexed_prims{};
    std::uint32_t vtype{}, tme{}, abe{}, ate{}, zte{}, lte{}, clear_mode{}, tpsm{}, fpf{}, fbp{}, fbw{};
    std::uint32_t texfunc{}, clut_fmt{};
    std::map<std::uint32_t, std::uint64_t> by_vtype_prim;   // (prim<<28)|vtype -> count
    std::map<std::uint32_t, std::uint64_t> by_tpsm;
    std::map<std::uint32_t, std::uint64_t> by_framebuf;     // fbp|fbw<<24 -> clear/prim count
    std::map<std::uint32_t, std::uint64_t> by_command;
    std::map<std::uint32_t, std::uint64_t> unknown_commands;
};
GeStats g_stats;

void stats_command(std::uint32_t command, std::uint32_t data) {
    GeStats &st = g_stats;
    ++st.commands;
    ++st.by_command[command];
    switch (command) {
    case 0x12u: st.vtype = data; break;
    case 0x1Eu: st.tme = data & 1u; break;
    case 0x21u: st.abe = data & 1u; break;
    case 0x22u: st.ate = data & 1u; break;
    case 0x23u: st.zte = data & 1u; break;
    case 0x17u: st.lte = data & 1u; break;
    case 0xC3u: st.tpsm = data & 0xFu; break;
    case 0xC9u: st.texfunc = data & 7u; break;
    case 0xC5u: st.clut_fmt = data & 3u; break;
    case 0xD2u: st.fpf = data & 3u; break;
    case 0x9Cu: st.fbp = data & 0xFFFFFFu; break;
    case 0x9Du: st.fbw = ((data & 0xFF0000u) << 8u) | (data & 0xFFFFu); break;
    case 0xD3u: // CLEARMODE
        st.clear_mode = data & 1u;
        if (st.clear_mode) ++st.clears;
        break;
    case 0x04u: { // PRIM
        const std::uint32_t prim = (data >> 16u) & 7u;
        const std::uint32_t count = data & 0xFFFFu;
        ++st.prims;
        st.vertices += count;
        ++st.by_vtype_prim[(prim << 28u) | (st.vtype & 0x0FFFFFFFu)];
        ++st.by_framebuf[st.fbp | (st.fbw << 24u)];
        if (st.vtype & (1u << 23u)) ++st.through_prims;
        if ((st.vtype >> 11u) & 3u) ++st.indexed_prims;
        if (!st.clear_mode) {
            if (st.tme) { ++st.texture_enabled_prims; ++st.by_tpsm[st.tpsm]; }
            if (st.abe) ++st.blend_prims;
            if (st.ate) ++st.alphatest_prims;
            if (st.zte) ++st.depth_prims;
            if (st.lte) ++st.lit_prims;
        }
        break;
    }
    default:
        if (command > 0xE9u) ++st.unknown_commands[command];
        break;
    }
}
} // namespace

void ge_stats_report() {
    const GeStats &st = g_stats;
    std::ostringstream out;
    out << "GE stats: lists=" << st.lists << " finishes=" << st.finishes << " commands=" << st.commands
        << " prims=" << st.prims << " vertices=" << st.vertices << " clears=" << st.clears;
    log_line(category::kGe, out.str());
    out.str("");
    out << "GE stats: textured=" << st.texture_enabled_prims << " through(2D)=" << st.through_prims
        << " blend=" << st.blend_prims << " alphatest=" << st.alphatest_prims << " depth=" << st.depth_prims
        << " lit=" << st.lit_prims << " indexed=" << st.indexed_prims;
    log_line(category::kGe, out.str());
    const char *prim_names[8] = {"points", "lines", "linestrip", "tris", "tristrip", "trifan", "sprites", "?"};
    std::size_t shown = 0;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> ordered;
    for (const auto &[key, count] : st.by_vtype_prim) ordered.emplace_back(count, key);
    std::sort(ordered.rbegin(), ordered.rend());
    for (const auto &[count, key] : ordered) {
        if (++shown > 24u) break;
        std::ostringstream line;
        line << "GE stats: prim=" << prim_names[(key >> 28u) & 7u] << " vtype=0x" << std::hex << (key & 0x0FFFFFFFu)
             << std::dec << " count=" << count;
        log_line(category::kGe, line.str());
    }
    for (const auto &[fmt, count] : st.by_tpsm) {
        std::ostringstream line;
        line << "GE stats: texture format " << fmt << " prims=" << count;
        log_line(category::kGe, line.str());
    }
    for (const auto &[fb, count] : st.by_framebuf) {
        std::ostringstream line;
        line << "GE stats: framebuffer fbp=0x" << std::hex << (fb & 0xFFFFFFu) << " fbw=0x" << (fb >> 24u)
             << std::dec << " prims=" << count;
        log_line(category::kGe, line.str());
    }
    std::vector<std::pair<std::uint64_t, std::uint32_t>> commands;
    for (const auto &[cmd, count] : st.by_command) commands.emplace_back(count, cmd);
    std::sort(commands.rbegin(), commands.rend());
    std::ostringstream line;
    line << "GE stats: top commands:";
    std::size_t n = 0;
    for (const auto &[count, cmd] : commands) {
        if (++n > 24u) break;
        line << " 0x" << std::hex << cmd << std::dec << "x" << count;
    }
    log_line(category::kGe, line.str());
}

namespace {
struct ExitCursor {
    std::uint32_t *out;
    const std::uint32_t &cursor;
    ~ExitCursor() { if (out) *out = cursor; }
};
}

std::vector<GeInterrupt> ge_headless_walk(const GuestMemory &memory, std::uint32_t address, std::uint32_t stall,
                                          std::uint32_t *stop_cursor, bool *finished) {
    std::vector<GeInterrupt> interrupts;
    ++g_stats.lists;
    if (finished) *finished = false;
    std::uint32_t base_high = 0u;
    std::uint32_t offset_address = 0u;
    std::uint32_t cursor = address;
    const ExitCursor exit_guard{stop_cursor, cursor};
    std::uint32_t previous_word = 0u;
    bool signal_pause = false;
    bool bounded = stall != 0u && stall > address;

    struct Frame {
        std::uint32_t pc, offset, base;
        bool bounded, restore_base;
    };
    Frame stack[16]{};
    std::size_t depth = 0u;

    const auto relative_address = [&](std::uint32_t data) {
        return (offset_address + ((base_high << 24u) | (data & 0xFFFFFFu))) & 0x0FFFFFFFu;
    };

    std::uint32_t consecutive_unknown = 0u;
    for (std::uint32_t step = 0u; step < kMaxCommandsPerList; ++step) {
        if (bounded && GuestMemory::canonical(cursor) == GuestMemory::canonical(stall)) break;
        if (!memory.contains(cursor, 4u)) break;
        const std::uint32_t word = memory.aot_load32(cursor);
        cursor += 4u;
        const std::uint32_t command = word >> 24u;
        const std::uint32_t data = word & 0xFFFFFFu;
        const std::uint32_t preceding = previous_word;
        previous_word = word;

        // A run of words that are not GE commands means the list end marker
        // was overwritten or we walked into data: stop instead of wandering.
        if (command > 0xE9u) {
            if (++consecutive_unknown > 32u) break;
        } else {
            consecutive_unknown = 0u;
        }

        stats_command(command, data);
        switch (command) {
        case 0x0Cu: { // END completes a preceding SIGNAL / FINISH
            if ((preceding >> 24u) == 0x0Fu) {
                interrupts.push_back(GeInterrupt{preceding & 0xFFFFu, cursor, !signal_pause});
                if (finished) *finished = true;
                ++g_stats.finishes;
                return interrupts;
            }
            if ((preceding >> 24u) != 0x0Eu) return interrupts;
            const std::uint32_t behavior = (preceding >> 16u) & 0xFFu;
            const std::uint32_t token = preceding & 0xFFFFu;
            if (behavior == 1u || behavior == 2u) {
                interrupts.push_back(GeInterrupt{token, cursor, false});
            } else if (behavior == 3u) {
                signal_pause = true;
            } else if (behavior == 8u) {
                // memory barrier: nothing to do headless
            } else if (behavior >= 0x10u && behavior <= 0x16u) {
                if (behavior == 0x12u) { // RET
                    if (depth == 0u) return interrupts;
                    const Frame frame = stack[--depth];
                    cursor = frame.pc;
                    offset_address = frame.offset;
                    if (frame.restore_base) base_high = frame.base;
                    bounded = frame.bounded;
                } else {
                    const std::uint32_t combined = ((token << 16u) | (data & 0xFFFFu)) & 0x0FFFFFFCu;
                    std::uint32_t target = combined;
                    if (behavior == 0x13u || behavior == 0x14u) target = (combined + cursor - 8u) & 0x0FFFFFFFu;
                    if (behavior == 0x15u || behavior == 0x16u) target = relative_address(combined);
                    if (behavior == 0x11u || behavior == 0x14u || behavior == 0x16u) { // call variants
                        if (depth == 16u) return interrupts;
                        stack[depth++] = Frame{cursor, offset_address, base_high, bounded, true};
                        bounded = false;
                    }
                    cursor = target;
                }
            } else {
                return interrupts;
            }
            break;
        }
        case 0x0Bu: { // RET
            if (depth == 0u) return interrupts;
            const Frame frame = stack[--depth];
            cursor = frame.pc;
            offset_address = frame.offset;
            if (frame.restore_base) base_high = frame.base;
            bounded = frame.bounded;
            break;
        }
        case 0x08u: { // JUMP
            const std::uint32_t target = relative_address(data & 0xFFFFFCu);
            if (target == cursor - 4u) return interrupts; // jump to self
            cursor = target;
            break;
        }
        case 0x0Au: // CALL
            if (depth == 16u) return interrupts;
            stack[depth++] = Frame{cursor, offset_address, base_high, bounded, false};
            cursor = relative_address(data & 0xFFFFFCu);
            bounded = false;
            break;
        case 0x10u: base_high = (data >> 16u) & 0x0Fu; break;      // BASE
        case 0x13u: offset_address = data << 8u; break;            // OFFSET
        case 0x14u: offset_address = cursor - 4u; break;           // ORIGIN
        default: break; // everything else is drawing state: ignored headless
        }
    }
    return interrupts;
}

} // namespace rr
