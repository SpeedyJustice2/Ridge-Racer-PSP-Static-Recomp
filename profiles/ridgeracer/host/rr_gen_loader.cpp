#include "rr_gen_loader.hpp"

#include "psprecomp/runtime.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif
#include <filesystem>

namespace rr {
std::string executable_directory() {
#if defined(_WIN32)
    wchar_t b[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, b, MAX_PATH * 2);
    return std::filesystem::path(std::wstring(b, n)).parent_path().string();
#else
    char b[4096];
    const ssize_t n = readlink("/proc/self/exe", b, sizeof b - 1);
    return n > 0 ? std::filesystem::path(std::string(b, static_cast<std::size_t>(n))).parent_path().string() : std::string(".");
#endif
}
}

#ifdef RR_GAME_LIBRARY
#include "../gen/rr_gen_abi.hpp"

#include <cstdlib>
#include <cstring>

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

namespace psprecomp {
// friend of GuestMemory / Runtime: the table entries below need their private slow paths
struct GenBridge {
    static std::uint8_t l8(const GuestMemory *m, std::uint32_t a) { return m->aot_load8_slow(a); }
    static std::uint16_t l16(const GuestMemory *m, std::uint32_t a) { return m->aot_load16_slow(a); }
    static std::uint32_t l32(const GuestMemory *m, std::uint32_t a) { return m->aot_load32_slow(a); }
    static void s8(GuestMemory *m, std::uint32_t a, std::uint8_t v) { m->aot_store8_slow(a, v); }
    static void s16(GuestMemory *m, std::uint32_t a, std::uint16_t v) { m->aot_store16_slow(a, v); }
    static void s32(GuestMemory *m, std::uint32_t a, std::uint32_t v) { m->aot_store32_slow(a, v); }
    static bool starve(Runtime *r, AllegrexContext *c) { return r->run_starvation_boundary(*c); }
};
}

namespace rr {
namespace {
using namespace psprecomp;
using rr::gen::Imports;

rr::gen::SyncFn g_sync = nullptr;

void push_globals() noexcept {
    if (g_sync != nullptr) g_sync(g_runtime_chain_observers_active, g_runtime_starvation_interval_fast, g_function_gate_table, g_function_gate_hook);
}

Imports make_imports() {
    Imports i{};
    i.abi = rr::gen::kAbiVersion;
    i.size = sizeof(Imports);
    i.vfpu_compare3 = [](AllegrexContext *c, std::uint32_t a, std::uint32_t b, std::uint32_t d, std::uint32_t e, std::uint32_t f) { c->execute_vfpu_compare3(a, b, d, e, f); };
    i.vfpu_cross_quat = [](AllegrexContext *c, std::uint32_t a, std::uint32_t b, std::uint32_t d, std::uint32_t e) { c->execute_vfpu_cross_quat(a, b, d, e); };
    i.vfpu_matrix_init = [](AllegrexContext *c, std::uint32_t a, std::uint32_t b, std::uint32_t d) { c->execute_vfpu_matrix_init(a, b, d); };
    i.vfpu_vhdp = [](AllegrexContext *c, std::uint32_t a, std::uint32_t b, std::uint32_t d, std::uint32_t e) { c->execute_vfpu_vhdp(a, b, d, e); };
    i.vfpu_vi2x = [](AllegrexContext *c, std::uint32_t a, std::uint32_t b, std::uint32_t d, std::uint32_t e) { c->execute_vfpu_vi2x(a, b, d, e); };
    i.vfpu_vminmax = [](AllegrexContext *c, std::uint32_t a, std::uint32_t b, std::uint32_t d, std::uint32_t e, bool f) { c->execute_vfpu_vminmax(a, b, d, e, f); };
    i.vfpu_vmmov = [](AllegrexContext *c, std::uint32_t a, std::uint32_t b, std::uint32_t d) { c->execute_vfpu_vmmov(a, b, d); };
    i.vfpu_vrnd = [](AllegrexContext *c, std::uint32_t a, std::uint32_t b, std::uint32_t d, std::uint32_t e) { c->execute_vfpu_vrnd(a, b, d, e); };
    i.vfpu_vrot = [](AllegrexContext *c, std::uint32_t a, std::uint32_t b, std::uint32_t d, std::uint32_t e) { c->execute_vfpu_vrot(a, b, d, e); };
    i.read_vfpu_matrix = [](const AllegrexContext *c, float *d, std::uint32_t m, std::uint32_t s) { c->read_vfpu_matrix(d, m, s); };
    i.write_vfpu_matrix = [](AllegrexContext *c, const float *d, std::uint32_t m, std::uint32_t s) { c->write_vfpu_matrix(d, m, s); };
    i.write_vfpu_vector_dp = [](AllegrexContext *c, const float *d, std::uint32_t r, std::uint32_t l) { c->write_vfpu_vector_with_destination_prefix(d, r, l); };
    i.load8_slow = [](const GuestMemory *m, std::uint32_t a) { return GenBridge::l8(m, a); };
    i.load16_slow = [](const GuestMemory *m, std::uint32_t a) { return GenBridge::l16(m, a); };
    i.load32_slow = [](const GuestMemory *m, std::uint32_t a) { return GenBridge::l32(m, a); };
    i.store8_slow = [](GuestMemory *m, std::uint32_t a, std::uint8_t v) { GenBridge::s8(m, a, v); };
    i.store16_slow = [](GuestMemory *m, std::uint32_t a, std::uint16_t v) { GenBridge::s16(m, a, v); };
    i.store32_slow = [](GuestMemory *m, std::uint32_t a, std::uint32_t v) { GenBridge::s32(m, a, v); };
    i.load_word_left = [](const GuestMemory *m, std::uint32_t a, std::uint32_t e) { return m->aot_load_word_left(a, e); };
    i.load_word_right = [](const GuestMemory *m, std::uint32_t a, std::uint32_t e) { return m->aot_load_word_right(a, e); };
    i.store_word_left = [](GuestMemory *m, std::uint32_t a, std::uint32_t v) { m->aot_store_word_left(a, v); };
    i.store_word_right = [](GuestMemory *m, std::uint32_t a, std::uint32_t v) { m->aot_store_word_right(a, v); };
    i.memory_barrier = [](const GuestMemory *m) { m->memory_barrier(); };
    i.arithmetic_overflow = [](Runtime *r, std::uint32_t pc, std::uint32_t insn) { r->arithmetic_overflow(pc, insn); };
    i.invoke_chained_call = [](Runtime *r, AllegrexContext *c, GuestMemory::AotFastView *v) { return r->invoke_chained_call(*c, v); };
    i.invoke_chained_unit = [](Runtime *r, AllegrexContext *c, std::uint32_t u, GuestMemory::AotFastView *v) { return r->invoke_chained_unit(*c, u, v); };
    i.invoke_import_cached = [](Runtime *r, std::uint32_t slot, const char *lib, std::size_t n, std::uint32_t nid, AllegrexContext *c) {
        r->invoke_import_cached(slot, std::string_view(lib, n), nid, *c);
    };
    i.register_function = [](Runtime *r, std::uint32_t a, void (*fn)(Runtime &, AllegrexContext &), const char *name) { r->register_function(a, fn, name); };
    i.register_generated_unit = [](Runtime *r, std::uint32_t idx, std::uint32_t addr, std::uint32_t span, void (*fn)(Runtime &, AllegrexContext &),
                                   void (*entry)(Runtime &, AllegrexContext &, std::uint16_t, GuestMemory::AotFastView &)) {
        r->register_generated_unit(idx, addr, span, fn, entry);
    };
    i.run_starvation_boundary = [](Runtime *r, AllegrexContext *c) { return GenBridge::starve(r, c); };
    i.stopped = [](const Runtime *r) { return r->stopped(); };
    i.unsupported = [](Runtime *r, std::uint32_t pc, std::uint32_t insn, const char *why) { r->unsupported(pc, insn, why); };
    i.capture_context = []() { return capture_runtime_execution_context(); };
    i.context_matches = [](RuntimeExecutionContextToken t) { return runtime_execution_context_matches(t); };
    return i;
}

#if defined(_WIN32)
void *open_library(const std::string &path) {
    std::wstring w(path.begin(), path.end());
    return reinterpret_cast<void *>(LoadLibraryW(w.c_str()));
}
void *symbol(void *lib, const char *name) { return reinterpret_cast<void *>(GetProcAddress(static_cast<HMODULE>(lib), name)); }
const char kLibName[] = "RidgeRacerGame.dll";
#else
void *open_library(const std::string &path) { return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL); }
void *symbol(void *lib, const char *name) { return dlsym(lib, name); }
const char kLibName[] = "libRidgeRacerGame.so";
#endif

} // namespace

bool game_library_split() { return true; }

std::string load_game_code(Runtime &runtime, const std::string &exe_dir) {
    std::string path = exe_dir;
    if (!path.empty() && path.back() != '/' && path.back() != '\\') path += '/';
    path += kLibName;
    void *lib = open_library(path);
    if (lib == nullptr) return std::string("The game library (") + kLibName + ") is missing or could not be loaded. Run Setup again to rebuild it.";
    const auto abi = reinterpret_cast<rr::gen::AbiVersionFn>(symbol(lib, "rr_gen_abi_version"));
    const auto bind = reinterpret_cast<rr::gen::BindFn>(symbol(lib, "rr_gen_bind"));
    const auto reg = reinterpret_cast<rr::gen::RegisterFn>(symbol(lib, "rr_gen_register"));
    const auto layout = reinterpret_cast<std::uint32_t (*)()>(symbol(lib, "rr_gen_layout"));
    g_sync = reinterpret_cast<rr::gen::SyncFn>(symbol(lib, "rr_gen_sync"));
    if (!abi || !bind || !reg || !layout || !g_sync) return "The game library is damaged or from another version. Run Setup again to rebuild it.";
    if (abi() != rr::gen::kAbiVersion) return "The game library does not match this version of the game (library " + std::to_string(abi()) + ", game " +
                                              std::to_string(rr::gen::kAbiVersion) + "). Run Setup again to rebuild it.";
    const std::uint32_t mine = static_cast<std::uint32_t>(sizeof(Runtime)) * 31u + static_cast<std::uint32_t>(sizeof(AllegrexContext)) * 7u +
                               static_cast<std::uint32_t>(sizeof(GuestMemory));
    if (layout() != mine) return "The game library was built for a different engine layout. Run Setup again to rebuild it.";
    static const Imports imports = make_imports();
    bind(&imports);
    g_runtime_globals_changed = &push_globals;
    push_globals();
    reg(runtime);
    return {};
}

} // namespace rr

#else   // single-file build: the converted code is linked in

namespace psprecomp { void register_generated_functions(Runtime &runtime); }

namespace rr {
bool game_library_split() { return false; }
std::string load_game_code(psprecomp::Runtime &runtime, const std::string &) {
    psprecomp::register_generated_functions(runtime);
    return {};
}
}

#endif
