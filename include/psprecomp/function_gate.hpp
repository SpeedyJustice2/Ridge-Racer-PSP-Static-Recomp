#pragma once

// Function gates (psp_recomp --function-gates).
//
// Generated code checks every discovered guest function entry against a byte table before running it.  When the
// host has flagged the entry and its hook returns true, the call is skipped: the function behaves as an immediate
// `jr $ra`, leaving every register (including $v0) unchanged.  With no table installed the check is one load and a
// predictable branch.  Profiles use this to change how often whole game subsystems run (for example to keep frame-
// counting HUD logic at 60 Hz while the rest of the game runs faster) without patching translated code.

#include "psprecomp/allegrex_context.hpp"

#include <cstdint>

namespace psprecomp {

inline constexpr std::uint32_t kFunctionGateBase = 0x08800000u;   // first guest address the table covers
inline constexpr std::uint32_t kFunctionGateSpan = 0x00400000u;   // bytes of guest code covered (one table byte per word)

using FunctionGateHook = bool (*)(std::uint32_t address, AllegrexContext &ctx);

// kFunctionGateSpan / 4 bytes; entry (address - kFunctionGateBase) / 4 non-zero = ask the hook.  Null = gates off.
extern std::uint8_t *g_function_gate_table;
extern FunctionGateHook g_function_gate_hook;

inline bool runtime_function_gate(std::uint32_t address, AllegrexContext &ctx) {
    return g_function_gate_hook != nullptr && g_function_gate_hook(address, ctx);
}

} // namespace psprecomp
