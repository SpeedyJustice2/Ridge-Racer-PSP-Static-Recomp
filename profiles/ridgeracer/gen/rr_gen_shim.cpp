// Compiled into the game library together with the converted code.  Defines everything the converted code expects from the
// program (see rr_gen_abi.hpp) by forwarding through the table the program hands over.
#include "rr_gen_abi.hpp"

#include <string>

#if defined(_WIN32)
#define RR_GEN_EXPORT extern "C" __declspec(dllexport)
#else
#define RR_GEN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace psprecomp {
void register_generated_functions(Runtime &runtime);

bool g_runtime_chain_observers_active = false;
std::uint64_t g_runtime_starvation_interval_fast = 0u;
std::uint8_t *g_function_gate_table = nullptr;
FunctionGateHook g_function_gate_hook = nullptr;
}

namespace {
const rr::gen::Imports *g_in = nullptr;
}

namespace psprecomp {

// ---- AllegrexContext
void AllegrexContext::execute_vfpu_compare3(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d, std::uint32_t e) noexcept { g_in->vfpu_compare3(this, a, b, c, d, e); }
void AllegrexContext::execute_vfpu_cross_quat(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) noexcept { g_in->vfpu_cross_quat(this, a, b, c, d); }
void AllegrexContext::execute_vfpu_matrix_init(std::uint32_t a, std::uint32_t b, std::uint32_t c) noexcept { g_in->vfpu_matrix_init(this, a, b, c); }
void AllegrexContext::execute_vfpu_vhdp(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) noexcept { g_in->vfpu_vhdp(this, a, b, c, d); }
void AllegrexContext::execute_vfpu_vi2x(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) noexcept { g_in->vfpu_vi2x(this, a, b, c, d); }
void AllegrexContext::execute_vfpu_vminmax(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d, bool e) noexcept { g_in->vfpu_vminmax(this, a, b, c, d, e); }
void AllegrexContext::execute_vfpu_vmmov(std::uint32_t a, std::uint32_t b, std::uint32_t c) noexcept { g_in->vfpu_vmmov(this, a, b, c); }
void AllegrexContext::execute_vfpu_vrnd(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) noexcept { g_in->vfpu_vrnd(this, a, b, c, d); }
void AllegrexContext::execute_vfpu_vrot(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) noexcept { g_in->vfpu_vrot(this, a, b, c, d); }
void AllegrexContext::read_vfpu_matrix(float *d, std::uint32_t m, std::uint32_t s) const noexcept { g_in->read_vfpu_matrix(this, d, m, s); }
void AllegrexContext::write_vfpu_matrix(const float *s, std::uint32_t m, std::uint32_t side) noexcept { g_in->write_vfpu_matrix(this, s, m, side); }
void AllegrexContext::write_vfpu_vector_with_destination_prefix(const float *s, std::uint32_t r, std::uint32_t l) noexcept { g_in->write_vfpu_vector_dp(this, s, r, l); }

// ---- GuestMemory
std::uint8_t GuestMemory::aot_load8_slow(std::uint32_t a) const { return g_in->load8_slow(this, a); }
std::uint16_t GuestMemory::aot_load16_slow(std::uint32_t a) const { return g_in->load16_slow(this, a); }
std::uint32_t GuestMemory::aot_load32_slow(std::uint32_t a) const { return g_in->load32_slow(this, a); }
void GuestMemory::aot_store8_slow(std::uint32_t a, std::uint8_t v) { g_in->store8_slow(this, a, v); }
void GuestMemory::aot_store16_slow(std::uint32_t a, std::uint16_t v) { g_in->store16_slow(this, a, v); }
void GuestMemory::aot_store32_slow(std::uint32_t a, std::uint32_t v) { g_in->store32_slow(this, a, v); }
std::uint32_t GuestMemory::aot_load_word_left(std::uint32_t a, std::uint32_t e) const { return g_in->load_word_left(this, a, e); }
std::uint32_t GuestMemory::aot_load_word_right(std::uint32_t a, std::uint32_t e) const { return g_in->load_word_right(this, a, e); }
void GuestMemory::aot_store_word_left(std::uint32_t a, std::uint32_t v) { g_in->store_word_left(this, a, v); }
void GuestMemory::aot_store_word_right(std::uint32_t a, std::uint32_t v) { g_in->store_word_right(this, a, v); }
void GuestMemory::memory_barrier() const noexcept { g_in->memory_barrier(this); }

// ---- Runtime
void Runtime::arithmetic_overflow(std::uint32_t pc, std::uint32_t insn) { g_in->arithmetic_overflow(this, pc, insn); }
bool Runtime::invoke_chained_call(AllegrexContext &ctx, GuestMemory::AotFastView *v) { return g_in->invoke_chained_call(this, &ctx, v); }
bool Runtime::invoke_chained_unit(AllegrexContext &ctx, std::uint32_t unit, GuestMemory::AotFastView *v) { return g_in->invoke_chained_unit(this, &ctx, unit, v); }
void Runtime::invoke_import_cached(std::uint32_t slot, std::string_view lib, std::uint32_t nid, AllegrexContext &ctx) {
    g_in->invoke_import_cached(this, slot, lib.data(), lib.size(), nid, &ctx);
}
void Runtime::register_function(std::uint32_t address, RecompiledFunction fn, std::string name) { g_in->register_function(this, address, fn, name.c_str()); }
void Runtime::register_generated_unit(std::uint32_t idx, std::uint32_t addr, std::uint32_t span, RecompiledFunction fn, RecompiledEntryFunction entry) {
    g_in->register_generated_unit(this, idx, addr, span, fn, entry);
}
bool Runtime::run_starvation_boundary(AllegrexContext &ctx) { return g_in->run_starvation_boundary(this, &ctx); }
bool Runtime::stopped() const noexcept { return g_in->stopped(this); }
void Runtime::unsupported(std::uint32_t pc, std::uint32_t insn, const std::string &reason) { g_in->unsupported(this, pc, insn, reason.c_str()); }

RuntimeExecutionContextToken capture_runtime_execution_context() noexcept { return g_in->capture_context(); }
bool runtime_execution_context_matches(RuntimeExecutionContextToken t) noexcept { return g_in->context_matches(t); }

} // namespace psprecomp

RR_GEN_EXPORT std::uint32_t rr_gen_abi_version() { return rr::gen::kAbiVersion; }
RR_GEN_EXPORT void rr_gen_bind(const rr::gen::Imports *in) { g_in = in; }
RR_GEN_EXPORT void rr_gen_sync(bool chain_active, std::uint64_t starvation, std::uint8_t *gate_table, psprecomp::FunctionGateHook hook) {
    psprecomp::g_runtime_chain_observers_active = chain_active;
    psprecomp::g_runtime_starvation_interval_fast = starvation;
    psprecomp::g_function_gate_table = gate_table;
    psprecomp::g_function_gate_hook = hook;
}
RR_GEN_EXPORT void rr_gen_register(psprecomp::Runtime &runtime) { psprecomp::register_generated_functions(runtime); }
RR_GEN_EXPORT std::uint32_t rr_gen_layout() {
    return static_cast<std::uint32_t>(sizeof(psprecomp::Runtime)) * 31u + static_cast<std::uint32_t>(sizeof(psprecomp::AllegrexContext)) * 7u +
           static_cast<std::uint32_t>(sizeof(psprecomp::GuestMemory));
}
