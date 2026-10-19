#pragma once

// Headless GE display-list walker.  It follows the same command/control-flow
// stream a real GE would (BASE/OFFSET/ORIGIN addressing, JUMP, CALL, RET,
// SIGNAL/FINISH + END pairs) but fetches no vertices and draws nothing.  Its
// only output is the list of interrupts the guest's GE callbacks must receive.
// Later stages replace this with a real renderer behind the same interface.

#include <cstdint>
#include <vector>

namespace psprecomp { class GuestMemory; }

namespace rr {

struct GeInterrupt {
    std::uint32_t token{};
    std::uint32_t next_pc{};
    bool finish{};
};

// Walks one submitted list from `address`.  `stall` bounds the initial linear
// run (0 or <= address means unbounded).
[[nodiscard]] std::vector<GeInterrupt> ge_headless_walk(const psprecomp::GuestMemory &memory,
                                                        std::uint32_t address, std::uint32_t stall,
                                                        std::uint32_t *stop_cursor = nullptr,
                                                        bool *finished = nullptr);

// Logs the accumulated draw statistics (prim types, vertex formats, textures).
void ge_stats_report();

} // namespace rr
