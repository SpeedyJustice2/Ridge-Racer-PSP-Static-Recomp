// The boundary between the game program (RidgeRacerNative.exe, built from this project's own source and published) and the
// game library (RidgeRacerGame.dll / .so, the code converted from the player's own copy of the game, built on their PC).
//
// The library calls back into the program only through the table below, so the two parts can be updated separately: a new
// program works with an old library as long as kAbiVersion is the same.  Bump kAbiVersion whenever this table, the layout of
// Runtime / AllegrexContext / GuestMemory (include/psprecomp) or the code generator's output format changes; the updater
// then rebuilds the library instead of only replacing the program.
#pragma once
#include "rr_gen_abi_version.hpp"
#include "psprecomp/function_gate.hpp"
#include "psprecomp/runtime.hpp"

#include <cstdint>

namespace rr::gen {

using psprecomp::AllegrexContext;
using psprecomp::GuestMemory;
using psprecomp::Runtime;
using u32 = std::uint32_t;

struct Imports {
    std::uint32_t abi;
    std::uint32_t size;   // sizeof(Imports) of the program that filled it in
    // layout check values, computed by both sides from the shared headers
    std::uint32_t sizeof_runtime, sizeof_context, sizeof_memory;

    // AllegrexContext
    void (*vfpu_compare3)(AllegrexContext *, u32, u32, u32, u32, u32);
    void (*vfpu_cross_quat)(AllegrexContext *, u32, u32, u32, u32);
    void (*vfpu_matrix_init)(AllegrexContext *, u32, u32, u32);
    void (*vfpu_vhdp)(AllegrexContext *, u32, u32, u32, u32);
    void (*vfpu_vi2x)(AllegrexContext *, u32, u32, u32, u32);
    void (*vfpu_vminmax)(AllegrexContext *, u32, u32, u32, u32, bool);
    void (*vfpu_vmmov)(AllegrexContext *, u32, u32, u32);
    void (*vfpu_vrnd)(AllegrexContext *, u32, u32, u32, u32);
    void (*vfpu_vrot)(AllegrexContext *, u32, u32, u32, u32);
    void (*read_vfpu_matrix)(const AllegrexContext *, float *, u32, u32);
    void (*write_vfpu_matrix)(AllegrexContext *, const float *, u32, u32);
    void (*write_vfpu_vector_dp)(AllegrexContext *, const float *, u32, u32);
    // GuestMemory
    std::uint8_t (*load8_slow)(const GuestMemory *, u32);
    std::uint16_t (*load16_slow)(const GuestMemory *, u32);
    u32 (*load32_slow)(const GuestMemory *, u32);
    void (*store8_slow)(GuestMemory *, u32, std::uint8_t);
    void (*store16_slow)(GuestMemory *, u32, std::uint16_t);
    void (*store32_slow)(GuestMemory *, u32, u32);
    u32 (*load_word_left)(const GuestMemory *, u32, u32);
    u32 (*load_word_right)(const GuestMemory *, u32, u32);
    void (*store_word_left)(GuestMemory *, u32, u32);
    void (*store_word_right)(GuestMemory *, u32, u32);
    void (*memory_barrier)(const GuestMemory *);
    // Runtime
    void (*arithmetic_overflow)(Runtime *, u32, u32);
    bool (*invoke_chained_call)(Runtime *, AllegrexContext *, GuestMemory::AotFastView *);
    bool (*invoke_chained_unit)(Runtime *, AllegrexContext *, u32, GuestMemory::AotFastView *);
    void (*invoke_import_cached)(Runtime *, u32 slot, const char *library, std::size_t library_len, u32 nid, AllegrexContext *);
    void (*register_function)(Runtime *, u32 address, void (*fn)(Runtime &, AllegrexContext &), const char *name);
    void (*register_generated_unit)(Runtime *, u32, u32, u32, void (*fn)(Runtime &, AllegrexContext &),
                                    void (*entry)(Runtime &, AllegrexContext &, std::uint16_t, GuestMemory::AotFastView &));
    bool (*run_starvation_boundary)(Runtime *, AllegrexContext *);
    bool (*stopped)(const Runtime *);
    void (*unsupported)(Runtime *, u32, u32, const char *);
    psprecomp::RuntimeExecutionContextToken (*capture_context)();
    bool (*context_matches)(psprecomp::RuntimeExecutionContextToken);
};

// What the library exports (C names).
//   uint32_t rr_gen_abi_version();                       // = kAbiVersion it was built with
//   void rr_gen_bind(const rr::gen::Imports *);           // before anything else
//   void rr_gen_sync(bool chain_active, uint64_t starvation, uint8_t *gate_table, FunctionGateHook hook);
//   void rr_gen_register(psprecomp::Runtime &);           // registers every converted function
using AbiVersionFn = std::uint32_t (*)();
using BindFn = void (*)(const Imports *);
using SyncFn = void (*)(bool, std::uint64_t, std::uint8_t *, psprecomp::FunctionGateHook);
using RegisterFn = void (*)(psprecomp::Runtime &);

} // namespace rr::gen
