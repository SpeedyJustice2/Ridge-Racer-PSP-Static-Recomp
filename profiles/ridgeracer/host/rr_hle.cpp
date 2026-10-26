
#include "rr_env.hpp"
#include "rr_atrac.hpp"
#include "rr_audio.hpp"
#include "rr_tuning.hpp"
#include "rr_mpeg.hpp"
#include "rr_menusound.hpp"
#include "rr_osk_ui.hpp"
#include "rr_lobby_ui.hpp"
#include "rr_utility.hpp"
#include "rr_ge.hpp"
#include "rr_png.hpp"
#include "rr_present.hpp"
#include "rr_pad.hpp"
#include "rr_report.hpp"
#include "rr_settings.hpp"
#include "rr_raster.hpp"
#include "rr_gl.hpp"
#include "rr_hle.hpp"
#include "rr_log.hpp"
#include "rr_net.hpp"
#include "rr_prof.hpp"
#include "rr_sysinfo.hpp"
#include "rr_arena.hpp"

#include "psprecomp/common.hpp"
#include "psprecomp/hle_audio_output2.hpp"
#include "psprecomp/hle_savedata.hpp"
#include "psprecomp/hle_sas.hpp"
#include "psprecomp/runtime.hpp"
#include "psprecomp/guest_memory.hpp"
namespace psprecomp { std::uint32_t runtime_watch_register(std::uint32_t index) noexcept; }
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#ifdef __EMSCRIPTEN__
#define RR_TITLE_PREFIX "Ridge Racer (browser)"
#else
#define RR_TITLE_PREFIX "Ridge Racer (native)"
#endif
#include <memory>
#include <system_error>

#include <algorithm>
#include <cctype>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <map>
#include <mutex>
#include <thread>
#include <array>
#include <bit>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <tuple>
#include <span>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rr {

void dump_scheduler(const char *why);
std::atomic<bool> g_dump_request{false};

namespace {

using psprecomp::AllegrexContext;
using psprecomp::GuestMemory;
using psprecomp::Runtime;
using psprecomp::hex32;

constexpr std::uint32_t kUserRamTop = 0x0A000000u;
constexpr std::uint32_t kModuleThreadStackSize = 0x10000u;

// ---------------------------------------------------------------------------
// Lazily installed options / configuration
// ---------------------------------------------------------------------------

HleOptions g_options;
bool g_installed = false;
psprecomp::Runtime *g_dump_rt = nullptr;   // for guest-memory diagnostics

// ---------------------------------------------------------------------------
// Diagnostics helpers
// ---------------------------------------------------------------------------

void hle_line(const std::string &message) {
    log_line(category::kHle, message);
}

void dispatch_line(const std::string &message) {
    log_line(category::kDispatch, message);
}

void error_line(const std::string &message) {
    log_line(category::kError, message);
}

// ---------------------------------------------------------------------------
// Import tracing
// ---------------------------------------------------------------------------

std::unordered_map<std::string, std::uint64_t> g_import_calls;
std::unordered_map<std::string, std::uint64_t> g_import_snapshot;   // taken at every presented frame
std::unordered_map<std::int32_t, const std::string *> g_last_import; // most recent import per thread

void register_import(Runtime &runtime, const char *library, std::uint32_t nid, const char *name,
                     Runtime::HleFunction function) {
    const std::string label = std::string(library) + "::" + name;
    runtime.register_hle(
        library, nid,
        [label, function = std::move(function)](Runtime &rt, AllegrexContext &ctx) {
            const std::uint64_t count = ++g_import_calls[label];
            g_last_import[psprecomp::runtime_thread_uid()] = &label;
            if (g_options.trace_imports &&
                (g_options.import_trace_limit == 0u || count <= g_options.import_trace_limit)) {
                std::ostringstream out;
                out << label << " thread=" << psprecomp::runtime_thread_uid()
                    << " pc=" << hex32(ctx.pc) << " ra=" << hex32(ctx.gpr[31]);
                log_line(category::kImport, out.str());
            }
            function(rt, ctx);
        });
}

// ---------------------------------------------------------------------------
// Cooperative PSP scheduler
// ---------------------------------------------------------------------------

enum class ThreadState { Created, Ready, Running, Delayed, Waiting, Suspended, Completed };
enum class CallbackWaitKind { None, Semaphore, EventFlag, ThreadEnd, Vblank };

struct SemaWaiter {
    std::int32_t uid{};
    std::int32_t amount{1};
};

struct FlagWaiter {
    std::int32_t uid{};
    std::uint32_t requested{};
    std::uint32_t mode{};
    std::uint32_t result_ptr{};
};

struct Joiner {
    std::int32_t uid{};
    std::uint32_t status_ptr{};
};

struct ThreadRecord {
    std::int32_t uid{};
    std::string name;
    std::uint32_t entry{};
    std::uint32_t priority{32};
    std::uint32_t stack_size{};
    std::uint32_t stack_top{};
    std::uint32_t stack_bottom{};
    std::uint32_t kernel_context{};
    ThreadState state{ThreadState::Created};
    AllegrexContext suspended{};
    std::uint64_t delay_until_us{};
    std::uint64_t vblank_deadline_us{};
    std::uint64_t ready_sequence{};
    std::uint32_t wakeup_count{};
    std::uint32_t exit_status{};
    bool suspended_flag{};
    bool callback_wait{};
    std::uint32_t callback_wait_pc{};
    CallbackWaitKind callback_wait_kind{CallbackWaitKind::None};
    std::int32_t callback_wait_object{-1};
    // Generic wait bookkeeping (semaphores / event flags / thread joins).
    std::uint64_t wait_deadline_us{};
    bool wait_has_deadline{};
    std::uint32_t wait_timeout_result{0x800201C9u};
};

struct SemaRecord {
    std::string name;
    std::int32_t count{};
    std::int32_t initial_count{};
    std::int32_t maximum{};
    // Diagnostic ownership for the middleware's binary semaphore mutexes.
    // PSP semaphores themselves do not release automatically on thread exit.
    std::int32_t mutex_owner{-1};
    std::vector<SemaWaiter> waiters;
};

struct EventFlagRecord {
    std::string name;
    std::uint32_t initial_pattern{};
    std::uint32_t current_pattern{};
    std::vector<FlagWaiter> waiters;
};

struct MutexWaiter {
    std::int32_t uid{};
    std::int32_t count{1};
};

struct MutexRecord {
    std::string name;
    std::int32_t lock_count{};
    std::int32_t owner_uid{-1};
    std::vector<MutexWaiter> waiters;
};

struct CallbackRecord {
    std::string name;
    std::uint32_t entry{};
    std::uint32_t arg{};   // commonArgument passed as a2 on every invocation
    std::int32_t owner_uid{};
    std::uint32_t notify_count{}, notify_arg{};
};

struct BlockRecord {
    std::string name;
    std::uint32_t address{};
    std::uint32_t size{};
    std::uint32_t memory_uid{};
};

// One nested guest callback execution on a PSP thread.  `saved` is the caller
// context to restore when the callback returns to the trampoline; its PC is the
// return address of the import that triggered the callback.
struct CallbackFrame {
    AllegrexContext saved{};
    std::int32_t callback_uid{};
    std::uint32_t notify_arg{};
    std::uint32_t entry{};
    const char *name{"callback"};
};

// PSP callback ABI: a0 = pending notification count, a1 = notify argument,
// a2 = the common argument given to sceKernelCreateCallback.  Address 4 is the
// kernel-style return trampoline for a callback invocation.
constexpr std::uint32_t kCallbackReturnAddress = 0x00000004u;

// UMD drive-state bits (pspumd.h).
constexpr std::uint32_t kUmdNotPresent = 0x0001u;
constexpr std::uint32_t kUmdPresent = 0x0002u;
constexpr std::uint32_t kUmdChanged = 0x0004u;
constexpr std::uint32_t kUmdNotReady = 0x0008u;
constexpr std::uint32_t kUmdReady = 0x0010u;
constexpr std::uint32_t kUmdReadable = 0x0020u;

struct UmdWaiter {
    std::int32_t uid{};
    std::uint32_t stat{};
};

std::int32_t g_current_uid = 0;
std::int32_t g_next_thread_uid = 1;
std::unordered_map<std::int32_t, ThreadRecord> g_threads;
std::vector<std::int32_t> g_ready;
std::uint64_t g_ready_sequence = 1;
std::unordered_map<std::int32_t, std::vector<Joiner>> g_joiners;
std::unordered_map<std::int32_t, SemaRecord> g_semas;
std::int32_t g_next_sema_uid = 0x100;
std::unordered_map<std::int32_t, EventFlagRecord> g_event_flags;
std::int32_t g_next_event_flag_uid = 0x200;
std::unordered_map<std::int32_t, MutexRecord> g_mutexes;
std::int32_t g_next_mutex_uid = 0x500;

bool g_dispatch_enabled = true;
bool g_interrupts_enabled = true;
std::unordered_map<std::int32_t, CallbackRecord> g_callbacks;
std::int32_t g_next_callback_uid = 0x300;
std::unordered_map<std::int32_t, std::vector<CallbackFrame>> g_callback_frames;
std::unordered_map<std::int32_t, BlockRecord> g_blocks;
std::int32_t g_next_block_uid = 0x400;
ArenaState g_arena;  // guest memory arena (see motorstorm_arena.hpp)
std::uint32_t g_stack_next_top{};
std::uint64_t g_virtual_time_us{};
std::uint64_t g_virtual_time_us_fwd() { return g_virtual_time_us; }
std::uint64_t g_music_next_trace{};
std::uint32_t g_ctrl_requested_cycle{}, g_ctrl_requested_mode{};
std::uint32_t g_last_pad_buttons{};
std::uint64_t g_delay_sequence = 1;

// UMD virtual drive: the host supplies a disc, so the drive boots inserted and
// ready, and becomes readable when the guest activates disc0:.  Every state
// change wakes matching waiters and notifies the registered UMD callback.
std::uint32_t g_umd_state = kUmdPresent | kUmdReady;
bool g_volatile_mem_locked = false;
// PSP volatile RAM: 4 MiB at 0x08400000 (well inside the 32 MiB main RAM the
// recompiler provides).
constexpr std::uint32_t kVolatileMemAddress = 0x08400000u;
constexpr std::uint32_t kVolatileMemSize = 0x00400000u;
bool g_umd_activated = false;
std::int32_t g_umd_callback_uid = 0;
std::uint64_t g_umd_transitions = 0;
std::vector<UmdWaiter> g_umd_waiters;

// Utility firmware-module bookkeeping (sceUtilityLoadModule/UnloadModule).
std::unordered_map<std::uint32_t, bool> g_utility_modules;

std::uint64_t g_switch_count = 0;
std::uint64_t g_delay_count = 0;
std::uint64_t g_preempt_count = 0;
std::uint64_t g_deadlock_count = 0;
std::uint32_t g_ge_list_logs = 0;

// Headless display / GE state (Stage 3).  No pixels are produced yet; the GE
// walker only reproduces the interrupts a real GE would raise.
std::uint64_t g_vblank_period_us = 16683u;   // 59.94 Hz (divided by --fps-mult)
// The vblank grid is anchored so the period can change while running (--fps-mult only during races): vblank number
// g_vb_base_count fell at virtual time g_vb_base_vt, later ones every g_vblank_period_us.
std::uint64_t g_vb_base_vt = 0, g_vb_base_count = 0;
std::uint64_t vblank_count_at(std::uint64_t vt) { return g_vb_base_count + (vt - g_vb_base_vt) / g_vblank_period_us; }
std::uint64_t next_vblank_after(std::uint64_t vt) { return g_vb_base_vt + ((vt - g_vb_base_vt) / g_vblank_period_us + 1u) * g_vblank_period_us; }
std::uint64_t g_virtual_time_us_fwd();
void set_vblank_period(std::uint64_t period) {
    if (period == g_vblank_period_us || period == 0u) return;
    const std::uint64_t now = g_virtual_time_us_fwd();
    const std::uint64_t n = (now - g_vb_base_vt) / g_vblank_period_us;
    g_vb_base_count += n;
    g_vb_base_vt += n * g_vblank_period_us;
    g_vblank_period_us = period;
    present_set_frame_period_us(static_cast<int>(period));
}
struct DisplayState {
    std::uint32_t mode{}, width{480}, height{272};
    std::uint32_t frame_top{}, frame_width{512}, frame_format{3};
    std::uint64_t frame_sets{};
} g_display;
struct GeCallbackSlot {
    std::uint32_t signal_func{}, signal_arg{}, finish_func{}, finish_arg{};
    bool used{};
};
std::unordered_map<std::int32_t, GeCallbackSlot> g_ge_callbacks;
std::int32_t g_ge_callback_id = 0;
struct GePending {
    std::uint32_t token{};
    bool finish{};
    std::int32_t callback_id{};
};
std::vector<GePending> g_ge_pending;
std::unique_ptr<SoftwareBackend> g_backend;
std::unique_ptr<GlBackend> g_gl;
// Frame skip (--frame-skip N): during a race only every Nth frame is drawn and shown.  The game logic runs every frame;
// the skipped frames' drawing commands are decoded as usual but not turned into pixels.
bool g_skip_draws = false;       // the frame being built right now is not drawn
bool g_no_draw = false;          // RR_NO_DRAW (test rig): nothing is ever drawn
std::uint64_t g_auto_skipped = 0, g_auto_race_frames = 0;   // automatic frame skip statistics
std::string g_gpu_status;        // empty = the requested renderer is running; otherwise why the software renderer is used
class SkipBackend final : public GpuBackend {
public:
    explicit SkipBackend(GpuBackend &inner) : inner_(inner) {}
    void draw(const GpuDraw &s, GpuPrim p, const GpuVertex *v, std::size_t n) override { if (!g_skip_draws && !g_no_draw) inner_.draw(s, p, v, n); }
    void vram_read(std::uint32_t a, std::uint32_t l) override { inner_.vram_read(a, l); }
    void vram_read_late_ok(std::uint32_t a, std::uint32_t l) override { inner_.vram_read_late_ok(a, l); }
    void set_read_reason(const char *r) override { inner_.set_read_reason(r); }
    void vram_write(std::uint32_t a, std::uint32_t l) override { inner_.vram_write(a, l); }
    void sync() override { inner_.sync(); }
private:
    GpuBackend &inner_;
};
std::unique_ptr<SkipBackend> g_skip_backend;
std::unique_ptr<GeProcessor> g_ge;
HleOptions g_hle_options;
std::uint64_t g_frames_presented = 0, g_screenshots_taken = 0;
std::uint32_t g_mail_watch_addr = 0u;   // RR_MAILLOG: physical address of the pending-event word being watched
std::uint64_t g_ge_lists_submitted = 0, g_ge_finish_interrupts = 0, g_ge_signal_interrupts = 0;
std::uint64_t g_vblank_waits = 0, g_ctrl_reads = 0;
std::uint32_t g_thread_wake_logs = 0;

[[nodiscard]] ThreadRecord *current_thread() {
    const auto found = g_threads.find(g_current_uid);
    return found != g_threads.end() ? &found->second : nullptr;
}

[[nodiscard]] std::uint32_t thread_priority(std::int32_t uid) {
    const auto found = g_threads.find(uid);
    return found != g_threads.end() ? found->second.priority : 0xFFFFFFFFu;
}

[[nodiscard]] std::int32_t best_ready_thread() {
    std::int32_t best = -1;
    std::uint32_t best_priority = 0xFFFFFFFFu;
    std::uint64_t best_sequence = 0;
    for (const std::int32_t uid : g_ready) {
        const auto found = g_threads.find(uid);
        if (found == g_threads.end() || found->second.state != ThreadState::Ready) continue;
        const std::uint32_t priority = found->second.priority;
        if (best == -1 || priority < best_priority ||
            (priority == best_priority && found->second.ready_sequence < best_sequence)) {
            best = uid;
            best_priority = priority;
            best_sequence = found->second.ready_sequence;
        }
    }
    return best;
}

void make_ready(ThreadRecord &thread, std::uint32_t v0) {
    thread.suspended.set_gpr(2, v0);
    thread.state = ThreadState::Ready;
    thread.ready_sequence = g_ready_sequence++;
    thread.wait_has_deadline = false;
    g_ready.push_back(thread.uid);
}

// Diagnostic bring-up helper (PSPRECOMP_RIDGERACER_START_ALL_CREATED=1): makes
// a dormant thread runnable with the entry convention of sceKernelStartThread
// with no argument block.  Used to test whether work hidden behind a
// never-started thread explains a stalled guest state machine.
void start_created_thread(ThreadRecord &thread) {
    std::uint32_t sp = thread.kernel_context;
    AllegrexContext next{};
    next.set_gpr(4, 0u);
    next.set_gpr(5, 0u);
    sp -= 64u;
    next.set_gpr(26, thread.kernel_context);
    next.set_gpr(29, sp);
    next.set_gpr(30, sp);
    next.set_gpr(31, 0u);
    next.pc = thread.entry;
    make_ready(thread, 0u);
    thread.suspended = next;
    hle_line("start_created_thread uid=" + std::to_string(thread.uid) + " name=\"" +
             thread.name + "\" entry=" + hex32(thread.entry));
}

// Saves the current guest context as the continuation of `thread`, whose PC is
// the caller's $ra: the import wrapper's "pc = RA" rewrite is deliberately
// skipped when the HLE switches threads, so the saved continuation must carry
// the return address itself.
void save_continuation(ThreadRecord &thread, const AllegrexContext &ctx) {
    thread.suspended = ctx;
    thread.suspended.pc = ctx.gpr[31];
}

void promote_expired_delays();
bool service_owned_callback(Runtime &,AllegrexContext &,std::uint32_t resume_pc=0,bool check=false);
bool ge_deliver(Runtime &, AllegrexContext &, std::uint32_t resume_pc = 0u);

[[nodiscard]] bool activate_next(Runtime &runtime, AllegrexContext &ctx, const char *reason) {
    promote_expired_delays();

    // Drop stale ready entries.
    g_ready.erase(std::remove_if(g_ready.begin(), g_ready.end(),
        [](std::int32_t uid) {
            const auto found = g_threads.find(uid);
            return found == g_threads.end() || found->second.state != ThreadState::Ready;
        }),
        g_ready.end());

    if (g_ready.empty()) {
        // Any delayed thread? Advance execution-driven time to the earliest
        // deadline and retry.  This mirrors the PSP timer interrupt.
        std::uint64_t earliest = 0;
        bool any_delayed = false;
        for (const auto &[uid, thread] : g_threads) {
            (void)uid;
            if (thread.state == ThreadState::Delayed &&
                (!any_delayed || thread.delay_until_us < earliest)) {
                earliest = thread.delay_until_us;
                any_delayed = true;
            }
        }
        if (any_delayed) {
            g_virtual_time_us = std::max(g_virtual_time_us, earliest);
            promote_expired_delays();
        }
    }
    if (g_ready.empty()) return false;

    const std::int32_t selected = best_ready_thread();
    if (selected == -1) return false;
    ThreadRecord &thread = g_threads.at(selected);
    thread.state = ThreadState::Running;
    g_current_uid = selected;
    if (g_dump_request.exchange(false)) dump_scheduler("watchdog: no frames presented");
    ctx = thread.suspended;
    psprecomp::set_runtime_thread_identity(selected, thread.name);
    if (thread.callback_wait) {
        thread.callback_wait=false;
        (void)service_owned_callback(runtime,ctx,ctx.pc);
    }
    (void)ge_deliver(runtime, ctx, ctx.pc);
    ++g_switch_count;
    // Switch logging is a bring-up diagnostic, not a per-event trace: an idle
    // guest loop that delays by a frame switches thousands of times per second
    // and once produced an 18 GiB log.  Keep a bounded prefix, and allow the
    // full stream only on request.
    static const bool full_switch_log = std::getenv("PSPRECOMP_RIDGERACER_TRACE_SWITCH") != nullptr;
    static std::uint64_t switch_logs = 0u;
    if (g_options.trace_imports && (full_switch_log || switch_logs < 256u)) {
        ++switch_logs;
        std::ostringstream out;
        out << "switch reason=" << reason << " uid=" << selected << " name=\"" << thread.name
            << "\" pc=" << hex32(ctx.pc) << " sp=" << hex32(ctx.gpr[29])
            << " ra=" << hex32(ctx.gpr[31]);
        dispatch_line(out.str());
    }
    return true;
}

void promote_expired_delays() {
    for (auto &[uid, thread] : g_threads) {
        if (thread.state == ThreadState::Delayed && thread.delay_until_us <= g_virtual_time_us) {
            // Only the display wait itself consumes this deadline. A callback
            // may delay too, retaining the original vblank target on reentry.
            if (thread.callback_wait && thread.callback_wait_kind == CallbackWaitKind::Vblank)
                thread.callback_wait_kind = CallbackWaitKind::None;
            make_ready(thread, 0u);
            continue;
        }
        if (thread.state == ThreadState::Waiting && thread.wait_has_deadline &&
            thread.wait_deadline_us <= g_virtual_time_us) {
            make_ready(thread, thread.wait_timeout_result);
        }
    }
}

// Blocks the current thread and switches to the next runnable one.  The caller
// must have set $v0 to the value the guest should observe on wake-up.
[[nodiscard]] bool block_current(Runtime &runtime, AllegrexContext &ctx, ThreadState state,
                                 const char *reason) {
    ThreadRecord *thread = current_thread();
    if (thread == nullptr) {
        error_line("block_current with no current thread record");
        runtime.stop("PSP scheduler lost the current thread record");
        return false;
    }
    save_continuation(*thread, ctx);
    thread->state = state;
    if (!activate_next(runtime, ctx, reason)) {
        ++g_deadlock_count;
        runtime.stop(std::string("PSP scheduler deadlock: all threads blocked (") + reason + ")");
        return false;
    }
    return true;
}

void complete_thread(Runtime &runtime, AllegrexContext &ctx, std::int32_t uid,
                     std::uint32_t status, bool delete_record) {
    const auto found = g_threads.find(uid);
    if (found != g_threads.end()) {
        found->second.state = ThreadState::Completed;
        found->second.exit_status = status;
    }
    g_ready.erase(std::remove(g_ready.begin(), g_ready.end(), uid), g_ready.end());

    const auto joiners = g_joiners.find(uid);
    if (joiners != g_joiners.end()) {
        std::vector<Joiner> waiting = joiners->second;
        g_joiners.erase(joiners);
        std::sort(waiting.begin(), waiting.end(), [](const Joiner &left, const Joiner &right) {
            const std::uint32_t left_priority = thread_priority(left.uid);
            const std::uint32_t right_priority = thread_priority(right.uid);
            if (left_priority != right_priority) return left_priority < right_priority;
            return left.uid < right.uid;
        });
        for (const Joiner &joiner : waiting) {
            const auto waiter = g_threads.find(joiner.uid);
            if (waiter == g_threads.end()) continue;
            if (joiner.status_ptr != 0u && runtime.memory().contains(joiner.status_ptr, 4u))
                runtime.memory().store32(joiner.status_ptr, status);
            if (waiter->second.state == ThreadState::Waiting) make_ready(waiter->second, 0u);
        }
    }

    if (delete_record && found != g_threads.end()) g_threads.erase(found);

    if (g_current_uid == uid) {
        if (!activate_next(runtime, ctx, "thread-complete")) {
            runtime.stop("All PSP threads completed");
        }
    }
}

// ---------------------------------------------------------------------------
// Thread wake logging and callback delivery
// ---------------------------------------------------------------------------

void log_thread_wake(std::int32_t uid, std::uint32_t value, const char *reason) {
    if (g_thread_wake_logs >= 128u) return;
    ++g_thread_wake_logs;
    const auto found = g_threads.find(uid);
    std::ostringstream out;
    out << "wake uid=" << uid << " name=\""
        << (found != g_threads.end() ? found->second.name : std::string("?"))
        << "\" value=" << hex32(value) << " reason=" << reason;
    log_line(category::kThread, out.str());
}

void wake_thread(ThreadRecord &thread, std::uint32_t v0, const char *reason) {
    make_ready(thread, v0);
    log_thread_wake(thread.uid, v0, reason);
}

// Sets `bits` in an event flag's pattern and wakes every waiter whose
// requested bits now match (honouring the any/clear-on-exit wait modes).
void signal_event_flag_bits(Runtime &runtime, EventFlagRecord &flag, std::uint32_t bits) {
    // Diagnostic: PSPRECOMP_RIDGERACER_TRACE_FLAGS=1 traces every event-flag
    // signal (name, bits, resulting pattern).  Off by default; capped so a hot
    // producer loop cannot grow the log without bound.
    static const bool trace_flags = std::getenv("PSPRECOMP_RIDGERACER_TRACE_FLAGS") != nullptr;
    if (trace_flags) {
        static std::uint64_t trace_count = 0;
        if (trace_count++ < 20000u)
            log_line("FLAGS", "set name=\"" + flag.name + "\" bits=" + hex32(bits) +
                                  " pattern=" + hex32(flag.current_pattern) +
                                  " thread=" + std::to_string(g_current_uid));
    }
    flag.current_pattern |= bits;
    std::vector<std::size_t> woken;
    for (std::size_t index = 0; index < flag.waiters.size(); ++index) {
        const FlagWaiter &waiter = flag.waiters[index];
        const bool any = (waiter.mode & 1u) != 0u;
        const bool matches = any ? (flag.current_pattern & waiter.requested) != 0u
                                 : (flag.current_pattern & waiter.requested) == waiter.requested;
        if (!matches) continue;
        if (waiter.result_ptr != 0u && runtime.memory().contains(waiter.result_ptr, 4u))
            runtime.memory().store32(waiter.result_ptr, flag.current_pattern);
        if ((waiter.mode & 0x20u) != 0u) flag.current_pattern &= ~waiter.requested;
        if ((waiter.mode & 0x10u) != 0u) flag.current_pattern = 0u;
        const auto thread = g_threads.find(waiter.uid);
        if (thread != g_threads.end()) wake_thread(thread->second, 0u, "event-flag-set");
        woken.push_back(index);
    }
    for (auto it = woken.rbegin(); it != woken.rend(); ++it)
        flag.waiters.erase(flag.waiters.begin() + static_cast<std::ptrdiff_t>(*it));
}

// Delivers a raw guest-function invocation (GE signal/finish handlers) with the
// kernel's callback calling convention: a0/a1 are the handler's arguments and
// the return address is the trampoline at 4.  Unlike sceKernelCreateCallback
// records these have no delete-on-nonzero-return semantics, so callback_uid
// stays zero and callback_return only restores the interrupted frame.
void notify_guest_function(Runtime &runtime, AllegrexContext &ctx, std::uint32_t entry,
                           std::uint32_t arg0, std::uint32_t arg1, const char *name,
                           const char *source) {
    CallbackFrame frame{};
    frame.saved = ctx;
    frame.saved.pc = ctx.gpr[31];
    frame.callback_uid = 0;
    frame.notify_arg = arg0;
    frame.entry = entry;
    frame.name = name;
    g_callback_frames[g_current_uid].push_back(frame);

    ctx.gpr[4] = arg0;
    ctx.gpr[5] = arg1;
    ctx.gpr[31] = kCallbackReturnAddress;
    ctx.pc = entry;

    if (g_options.trace_imports) {
        std::ostringstream out;
        out << "entering " << name << " entry=" << hex32(entry) << " a0=" << hex32(arg0)
            << " a1=" << hex32(arg1) << " thread=" << g_current_uid << " source=" << source;
        log_line(category::kCallback, out.str());
    }
}

// Delivers one callback invocation on the current PSP thread.  PSP callbacks
// run on the creating thread; the guest entry point is invoked with
// (notifyCount, notifyArg, commonArgument) and returns into the trampoline at
// address 4.  The caller context is saved with PC = the import return address,
// so when the callback finishes the interrupted guest code resumes as if the
// kernel call that triggered the notification had simply returned.
void notify_callback(Runtime &runtime, AllegrexContext &ctx, std::int32_t callback_uid,
                     std::uint32_t notify_arg, const char *source,
                     std::uint32_t count = 1u, std::uint32_t resume_pc = 0u) {
    const auto found = g_callbacks.find(callback_uid);
    if (found == g_callbacks.end()) {
        error_line("notify_callback: unknown callback uid=" + std::to_string(callback_uid));
        return;
    }
    const CallbackRecord &record = found->second;
    CallbackFrame frame{};
    frame.saved = ctx;
    frame.saved.pc = resume_pc ? resume_pc : ctx.gpr[31];
    frame.callback_uid = callback_uid;
    frame.notify_arg = notify_arg;
    frame.entry = record.entry;
    g_callback_frames[g_current_uid].push_back(frame);

    ctx.gpr[4] = count;          // notifyCount
    ctx.gpr[5] = notify_arg;     // notifyArg
    ctx.gpr[6] = record.arg;     // commonArgument
    ctx.gpr[31] = kCallbackReturnAddress;
    ctx.pc = record.entry;

    if (g_options.trace_imports) {
        std::ostringstream out;
        out << "entering uid=" << callback_uid << " name=\"" << record.name << "\""
            << " entry=" << hex32(record.entry) << " notify_arg=" << hex32(notify_arg)
            << " common_arg=" << hex32(record.arg)
            << " thread=" << g_current_uid << " source=" << source;
        log_line(category::kCallback, out.str());
    }
}

bool service_owned_callback(Runtime &runtime, AllegrexContext &ctx, std::uint32_t resume_pc,bool check) {
    const auto active=g_callback_frames.find(g_current_uid);
    if (check && RR_ENV_FLAG("PSPRECOMP_RIDGERACER_TRACE_CALLBACK_OWNER")) {
        static std::unordered_set<std::int32_t> reported;
        for (const auto &[uid,cb] : g_callbacks) {
            if (cb.owner_uid!=g_current_uid || !cb.notify_count || !reported.insert(uid).second) continue;
            const auto *thread=current_thread();
            log_line("CALLBACK", "check owner="+std::to_string(g_current_uid)+" callback="+std::to_string(uid)+
                " count="+std::to_string(cb.notify_count)+" frames="+
                std::to_string(active==g_callback_frames.end()?0:active->second.size())+" pc="+
                hex32(ctx.pc)+" state="+std::to_string(thread?static_cast<int>(thread->state):-1));
        }
    }
    if (active!=g_callback_frames.end() && !active->second.empty()) return false;
    for (auto &[uid,cb] : g_callbacks) {
        if (cb.owner_uid != g_current_uid || !cb.notify_count) continue;
        const auto count=cb.notify_count,arg=cb.notify_arg;
        cb.notify_count=0;cb.notify_arg=0;
        if (auto *thread=current_thread()) thread->callback_wait=false;
        if (check) ctx.set_gpr(2,1);
        notify_callback(runtime,ctx,uid,arg,"owned-callback",count,resume_pc);
        return true;
    }
    return false;
}

// Keeps the output window responsive even when the guest renders rarely.
void maybe_pump_window() {
    if (!g_hle_options.window) return;
    using clock = std::chrono::steady_clock;
    static clock::time_point last{};
    const auto now = clock::now();
    if (now - last < std::chrono::milliseconds(8)) return;
    last = now;
    present_pump();
}

// Progress monitor: prints a status line every few seconds and an explicit
// warning when the guest makes no progress, so a hang shows where it is stuck.
std::atomic<bool> g_watchdog_stop{false};
std::thread g_watchdog;
void start_watchdog() {
    g_watchdog_stop = false;
    g_watchdog = std::thread([] {
        std::uint64_t last_frames = ~0ull, last_switches = ~0ull, last_time = ~0ull;
        std::uint32_t stalled_seconds = 0, seconds = 0, frame_stall_seconds = 0;
        std::uint64_t last_dump_frames = ~0ull;
        std::uint64_t rate_vt = 0u, rate_frames = 0u, rate_vb = 0u;
        auto rate_t0 = std::chrono::steady_clock::now();
        while (!g_watchdog_stop.load()) {
            for (int i = 0; i < 10 && !g_watchdog_stop.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (g_watchdog_stop.load()) break;
            ++seconds;
            const std::uint64_t frames = g_frames_presented, switches = g_switch_count, vt = g_virtual_time_us;
            const bool progress = frames != last_frames || switches != last_switches || vt != last_time;
            last_frames = frames; last_switches = switches; last_time = vt;
            stalled_seconds = progress ? 0u : stalled_seconds + 1u;
            if (frames == last_dump_frames) {
                if (++frame_stall_seconds == 5u || frame_stall_seconds % 60u == 0u) g_dump_request = true;
                if (frame_stall_seconds == 15u && frames > 0u && g_hle_options.window) rr::report::write_freeze_report(15u);   // (for bug reports)
            }
            else { frame_stall_seconds = 0u; last_dump_frames = frames; }
            if ((seconds % 10u) == 0u && g_hle_options.fps_mult != 1.0) {
                // Frame-rate experiment: how fast the game's own loop runs per second of game time.
                const auto now_t = std::chrono::steady_clock::now();
                const double wall = std::chrono::duration<double>(now_t - rate_t0).count();
                const double virt = static_cast<double>(vt - rate_vt) / 1e6;
                if (virt > 0.5 && wall > 0.5) {
                    char rb[260];
                    std::snprintf(rb, sizeof rb, "RATE fps_mult=%.2f game_time/real_time=%.3f per_game_second: vblank_waits=%.1f presented_frames=%.1f (60 = normal)",
                                  g_hle_options.fps_mult, virt / wall, static_cast<double>(g_vblank_waits - rate_vb) / virt,
                                  static_cast<double>(frames - rate_frames) / virt);
                    log_line("HLE", rb);
                }
                rate_t0 = now_t; rate_vt = vt; rate_frames = frames; rate_vb = g_vblank_waits;
            }
            if (stalled_seconds >= 3u || (seconds % 10u) == 0u) {
                std::ostringstream out;
                out << (stalled_seconds >= 3u ? "NO PROGRESS for " + std::to_string(stalled_seconds) + "s: " : std::string("status: "))
                    << "frames=" << frames << " virtual_time_ms=" << vt / 1000u << " switches=" << switches
                    << " thread=" << g_current_uid << " dispatch_pc=" << hex32(psprecomp::runtime_dispatch_pc());
                log_line("WATCHDOG", out.str());
            }
        }
    });
}

// Converts the current display frame buffer and shows / saves it.

// ---- Deterministic input replay (parsed from "PAD buttons A -> B (read #N, frame F)" log lines) ----
struct ReplayEvent { std::uint64_t read; std::uint32_t buttons; };
double g_present_ms = 0.0;
std::vector<ReplayEvent> g_replay;
std::size_t g_replay_pos = 0;
double g_ctrl_game60 = 0.0;   // controller reads counted in 59.94 Hz frames of game time
extern std::uint64_t g_vblank_period_us, g_base_vblank_us;
bool g_replay_active = false;
// Test-rig input ("RIG anchor kind a b buttons" lines, made by a script from a play log): button changes timed against anchors
// so a recorded play-through still lines up when the race runs differently.  Anchors in order: 0 boot (a = read number),
// each race start (first driver input record call after an init; a = record call count, b = reads after that count was
// reached, for presses while paused) and each race end (race state 3; a = frames after it).
struct RigEvent { std::uint32_t anchor, kind; std::uint64_t a, b; std::uint32_t buttons; };
std::vector<RigEvent> g_rig;
std::size_t g_rig_pos = 0;
bool g_rig_active = false;
std::uint32_t g_rig_anchor = 0;          // anchors reached so far (0 = boot)
std::uint64_t g_rig_ticks = 0, g_rig_tick_read = 0, g_rig_end_frame = 0;
bool g_rig_wait_race = false;            // after a driver input init, until its first record call

void load_replay(const std::filesystem::path &path) {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("RIG ", 0) == 0) {
            RigEvent e{};
            unsigned long long a = 0, b = 0;
            if (std::sscanf(line.c_str() + 4, "%u %u %llu %llu %x", &e.anchor, &e.kind, &a, &b, &e.buttons) == 5) { e.a = a; e.b = b; g_rig.push_back(e); }
            continue;
        }
        const auto at = line.find("PAD buttons 0x");
        if (at == std::string::npos) continue;
        const auto arrow = line.find("-> 0x", at);
        const auto rd = line.find("(read #", arrow == std::string::npos ? at : arrow);
        if (arrow == std::string::npos || rd == std::string::npos) continue;
        const std::uint32_t buttons = static_cast<std::uint32_t>(std::strtoul(line.c_str() + arrow + 3, nullptr, 16));
        const std::uint64_t read = std::strtoull(line.c_str() + rd + 7, nullptr, 10);
        g_replay.push_back({read, buttons});
    }
    g_replay_active = true;
    if (!g_rig.empty()) { g_rig_active = true; log_line(category::kHle, "RIG input: " + std::to_string(g_rig.size()) + " anchored button changes"); }
    log_line(category::kHle, "input replay: " + std::to_string(g_replay.size()) + " button changes from " + path.string());
}

void rig_note_drvin_init(std::uint32_t mode) { if (g_rig_active && mode == 0u) g_rig_wait_race = true; }
void rig_note_drvin_call(std::uint32_t mode) {
    if (!g_rig_active || mode != 0u) return;
    if (g_rig_wait_race) {
        g_rig_wait_race = false; ++g_rig_anchor; g_rig_ticks = 0;
        log_line(category::kHle, "RIG anchor " + std::to_string(g_rig_anchor) + " race start at read " + std::to_string(g_ctrl_reads));
    }
    ++g_rig_ticks; g_rig_tick_read = g_ctrl_reads;
}
void rig_note_race_end() {
    if (!g_rig_active) return;
    ++g_rig_anchor; g_rig_end_frame = g_frames_presented;
    log_line(category::kHle, "RIG anchor " + std::to_string(g_rig_anchor) + " race end at frame " + std::to_string(g_frames_presented));
}
bool rig_due(const RigEvent &e) {
    if (g_rig_anchor > e.anchor) return true;
    if (g_rig_anchor < e.anchor) return false;
    if (e.kind == 0u) return g_ctrl_reads >= e.a;
    if (e.kind == 1u) return g_rig_ticks > e.a || (g_rig_ticks == e.a && g_ctrl_reads - g_rig_tick_read >= e.b);
    return g_frames_presented - g_rig_end_frame >= e.a;
}

// Every pool constant the fps patch changed: {address, original bits, patched bits}.
std::vector<std::array<std::uint32_t, 3>> g_fps_patches;
namespace gates { bool autopilot_buttons(std::uint32_t &out); extern std::uint64_t scene_frame; }
std::uint32_t g_opp_state = 0xffffffffu;   // race state machine state (OPPSTATE), for the test autopilot
using gates::autopilot_buttons;
std::uint32_t effective_pad_buttons() {
    if (g_rig_active) {
        static std::uint32_t current = 0u;
        while (g_rig_pos < g_rig.size() && rig_due(g_rig[g_rig_pos])) { current = g_rig[g_rig_pos].buttons; ++g_rig_pos; }
        std::uint32_t ap = 0u;
        if ((g_rig_anchor & 1u) == 1u && !g_rig_wait_race && autopilot_buttons(ap)) return ap;
        return current;
    }
    if (!g_replay_active) return present_pad_buttons();
    {   // RR_AUTOPILOT with a replay (netplay tests): the autopilot drives while the race scene runs
        static const bool ap_replay = std::getenv("RR_AUTOPILOT") != nullptr && std::getenv("RR_AP_REPLAY") != nullptr;
        std::uint32_t ap = 0u;
        if (ap_replay && g_frames_presented > 30u && g_frames_presented - gates::scene_frame < 5u && autopilot_buttons(ap)) {   // (race scene on screen)
            // RR_AP_BRAKE="every_s,hold_s" (netplay tests): full brake for hold_s seconds every every_s seconds of race (wall clock)
            static const std::pair<double, double> brk = [] { double a = 0, b = 0; if (const char *e = std::getenv("RR_AP_BRAKE")) std::sscanf(e, "%lf,%lf", &a, &b); return std::pair<double, double>{a, b}; }();
            if (brk.first > 0.0) {
                static const auto t0 = std::chrono::steady_clock::now();
                const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                if (std::fmod(t, brk.first) > brk.first - brk.second) return 0x8000u;
            }
            return ap;
        }
    }
    static std::uint32_t current = 0u;
    // RR_REPLAY_GAMETIME=1: the log's read numbers count 59.94 Hz frames of game time (a 60 fps log replayed at any rate)
    static const bool by_time = std::getenv("RR_REPLAY_GAMETIME") != nullptr;
    const std::uint64_t now_read = by_time ? static_cast<std::uint64_t>(g_ctrl_game60 + 1e-6) : g_ctrl_reads;
    while (g_replay_pos < g_replay.size() && g_replay[g_replay_pos].read <= now_read) {
        current = g_replay[g_replay_pos].buttons;
        ++g_replay_pos;
    }
    return current;
}

bool g_net_race_sync = false;   // a race with other players: catch-up always on (see present_display_frame)
double g_ft_sum = 0.0, g_ft_max = 0.0;
std::uint64_t g_ft_frames = 0, g_ft_over20 = 0, g_ft_over33 = 0;
std::uint64_t g_spike_prev[4] = {0, 0, 0, 0}, g_spikes_logged = 0;

// ---- Car trace (Delete key, needs --trace-tools): which game function writes which field of the player's car ----------
// The player's car body is found by its timestep fields (+0x26d0 and +0x26d8 both 1/60, written by the car constructor).
// For kCarTraceFrames frames every store into [body - kCarTraceBefore, body + kCarTraceAfter) is recorded; at the end of
// each frame the fields whose value changed are written out with the function that last wrote them (dispatch pc + ra),
// together with the pad state of that frame.  Reads of the pad buffer are logged too (they lead to the input code).
void hle_watch_sink(bool is_write, std::uint32_t address, std::size_t length, std::uint64_t old_value, std::uint64_t new_value);
void arm_race_probe(Runtime &rt);

// ---- Function gates (needs code generated with psp_recomp --function-gates) --------------------------------------------
// gates::skip: game functions that run only on every other logic step while the raised frame rate is active, so
// frame-counting subsystems (countdown, HUD timers, ...) keep their 60 Hz pace.  gates::profile: while a trace runs,
// every function entry is counted and remembered as "the function now running" for write attribution.
// RR_REPLAYLOG=1 (with --trace-tools): the player's controller object (0x08845E48 a0: +5 source, +8 buttons, +0x18.. axes) is
// write-watched; every 600 steps the writers (pc / ra / offset, with counts) are logged as CTRLW, so the code that feeds the
// controller during a replay (and how often per step) shows up.
std::uint32_t g_ctrl_lo = 0u, g_ctrl_hi = 0u;
std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>, std::uint32_t> g_ctrl_writers;
void hle_watch_sink(bool is_write, std::uint32_t address, std::size_t length, std::uint64_t old_value, std::uint64_t new_value);
[[nodiscard]] std::uint32_t arena_allocate(std::uint32_t size, std::uint32_t alignment);

// ---------------------------------------------------------------------------
// Wireless multiplayer over the network (RR_NET=1, see rr_net.hpp): the game's calls into the PSP's ad-hoc libraries
// are answered by a net::Node, and the node's events (a group was scanned, someone said hello ...) are handed to the
// handlers the game registered, once per frame from sceKernelCheckCallback on the game's main thread.
// ---------------------------------------------------------------------------
struct NetGlue {
    std::uint32_t ctl_entry{0}, ctl_arg{0}, match_cb{0}, scratch{0};
};
NetGlue g_net;
const bool g_net_logger_installed = (net::set_logger([](const std::string &text) { log_line("NET", text); }), true);

net::Mac net_read_mac(Runtime &rt, std::uint32_t address) {
    net::Mac mac{};
    for (std::uint32_t i = 0; i < 6u; ++i) mac[i] = rt.memory().load8(address + i);
    return mac;
}
void net_write_mac(Runtime &rt, std::uint32_t address, const net::Mac &mac) {
    for (std::uint32_t i = 0; i < 6u; ++i) rt.memory().store8(address + i, mac[i]);
}
std::vector<std::uint8_t> net_read_bytes(Runtime &rt, std::uint32_t address, std::uint32_t length) {
    std::vector<std::uint8_t> out;
    if (address == 0u) return out;
    length = std::min<std::uint32_t>(length, 0x1000u);
    out.resize(length);
    for (std::uint32_t i = 0; i < length; ++i) out[i] = rt.memory().load8(address + i);
    return out;
}

// Logs the game's lobby option bytes when they change (RR_NET_TRACE=1): room info the host advertises.
void net_log_opt(const char *what, const std::vector<std::uint8_t> &opt) {
    static const bool on = std::getenv("RR_NET_TRACE") != nullptr;
    static std::vector<std::uint8_t> last;
    if (!on || opt == last) return;
    last = opt;
    std::string hex;
    char b[4];
    for (auto c : opt) { std::snprintf(b, sizeof b, "%02x", c); hex += b; }
    log_line("NET", std::string(what) + " (" + std::to_string(opt.size()) + " bytes): " + hex);
}

// ---- server browser (internet mode): see rr_lobby_ui.hpp ----
// The game starts its network session from one of two places in the Wireless Battle menu and stores which one at
// 0x089F2168 (0 = Host Race, 1 = Search Race) just before; 0x089F2088 is 1 while a session is being set up, and
// 0x089F2124 holds the 28-byte room description the host will advertise.  The group scan both paths start with returns
// from 0x0888C688 (checked, so another version of the game simply keeps the plain behaviour).
constexpr std::uint32_t kNetPathAddr = 0x089F2168u, kNetSetupAddr = 0x089F2088u, kRoomOptAddr = 0x089F2124u, kScanReturn = 0x0888C688u;
struct LobbyGlue {
    bool active{false};
    int list_gen{-1}, lookup_gen{-1}, probe_gen{-1};
    std::uint64_t ping_sig{0};
    std::chrono::steady_clock::time_point ping_at{};
    std::map<net::Mac, net::RoomInfo> known;   // rooms by id, from lists and code lookups
    std::uint64_t shot_key{0};
    int shot_n{0};
    std::uint32_t frames_open{0};
    bool browser{false};   // the open page is the server browser (else the room setup)
} g_lobby;

bool lobby_enabled() {
    static const bool off = std::getenv("RR_NET_NO_BROWSER") != nullptr;   // (tests: the plain search that joins every room)
    return !off;
}

void lobby_load_prefs() {
    static bool loaded = false;
    if (loaded) return;
    loaded = true;
    std::error_code ec;
    const auto ini = settings_file_path();
    if (!std::filesystem::is_regular_file(ini, ec)) return;
    const PlayerSettings s = load_settings(ini);
    lobby_host_prefs() = LobbyHostPrefs{s.room_visibility, s.room_password};
}
void lobby_save_prefs() {
    std::error_code ec;
    const auto ini = settings_file_path();
    if (!std::filesystem::is_regular_file(ini, ec)) return;
    PlayerSettings s = load_settings(ini);
    s.room_visibility = lobby_host_prefs().vis;
    s.room_password = lobby_host_prefs().password;
    save_settings(ini, s);
}

LobbyRoom lobby_room_of(const net::RoomInfo &ri, const net::BrowseState &b) {
    LobbyRoom r;
    std::copy(ri.mac.begin(), ri.mac.end(), r.id.begin());
    lobby_decode_info(ri.info, r);
    r.players = ri.players; r.max = ri.max; r.vis = ri.vis; r.est_ms = ri.est_ms; r.racing = ri.racing;
    if (const auto it = b.ping_ms.find(ri.mac); it != b.ping_ms.end()) r.ping_ms = it->second;
    return r;
}

// At the game's group scan: opens the browser (Search Race) or the room setup page (Host Race) and holds the scan open.
bool lobby_try_open(Runtime &rt, AllegrexContext &ctx, net::Node &node) {
    if (!node.online() || !lobby_enabled()) return false;
    auto &mem = rt.memory();
    if (static_cast<std::uint32_t>(ctx.gpr[31]) != kScanReturn || mem.load8(kNetSetupAddr) != 1u) return false;
    const std::uint32_t path = mem.load32(kNetPathAddr);
    if (path > 1u) return false;
    lobby_load_prefs();
    node.ctl_scan_hold();
    { std::vector<char16_t> stale; present_take_typed(stale); PresentMouse m; present_take_mouse(m); }
    g_lobby.active = true;
    g_lobby.frames_open = 0;
    g_lobby.list_gen = g_lobby.lookup_gen = g_lobby.probe_gen = -1;
    g_lobby.ping_sig = 0;
    g_lobby.browser = path == 1u;
    if (path == 1u) {
        log_line("NET", "Search Race: server browser opened");
        lobby_ui_begin_browser();
    } else {
        std::vector<std::uint8_t> opt(28);
        for (std::uint32_t i = 0; i < 28u; ++i) opt[i] = mem.load8(kRoomOptAddr + i);
        LobbyRoom room;
        lobby_decode_info(opt, room);
        log_line("NET", "Host Race: room setup opened (room code " + node.room_code() + ")");
        lobby_ui_begin_host(room, node.room_code());
    }
    return true;
}

// Once a frame (from the present path): drives the open page and carries out what it asks for.
void lobby_frame() {
    if (!g_lobby.active) return;
    net::Node *node = net::shared_node();
    if (node == nullptr) { lobby_ui_end(); g_lobby.active = false; return; }
    node->poll();
    const net::BrowseState &b = node->browse();
    // results that arrived
    {
        std::uint64_t sig = b.ping_ms.size();
        for (const auto &[m, ms] : b.ping_ms) sig = sig * 1000003ull + static_cast<std::uint64_t>(ms) + m[5];
        if (b.list_gen != g_lobby.list_gen || sig != g_lobby.ping_sig) {
            const bool new_list = b.list_gen != g_lobby.list_gen;
            g_lobby.list_gen = b.list_gen;
            g_lobby.ping_sig = sig;
            std::vector<LobbyRoom> rooms;
            if (b.list_status == 200) for (const auto &ri : b.rooms) { g_lobby.known[ri.mac] = ri; rooms.push_back(lobby_room_of(ri, b)); }
            lobby_ui_set_rooms(rooms, b.list_gen < 0 || b.list_status == 0 ? 0 : b.list_status, new_list);
            if (new_list && b.list_status == 200) {
                for (const auto &ri : b.rooms) node->browse_ping(ri.mac);
                g_lobby.ping_at = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            }
        }
        if (b.lookup_gen != g_lobby.lookup_gen && g_lobby.lookup_gen != -1) {
            if (b.lookup_found) g_lobby.known[b.lookup.mac] = b.lookup;
            lobby_ui_lookup_result(b.lookup_found, lobby_room_of(b.lookup, b), b.lookup_status, b.lookup_other_version);
        }
        g_lobby.lookup_gen = b.lookup_gen;
        if (b.probe_gen != g_lobby.probe_gen && g_lobby.probe_gen != -1) lobby_ui_probe_result(b.probe_result == "version" ? "version " + b.probe_ep : b.probe_result);
        lobby_ui_set_update(b.update_to);   // (the room service says this game version is out of date)
        g_lobby.probe_gen = b.probe_gen;
        if (std::chrono::steady_clock::now() >= g_lobby.ping_at && !b.rooms.empty()) {   // keep the pings fresh
            g_lobby.ping_at = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            for (const auto &ri : b.rooms) node->browse_ping(ri.mac);
        }
    }
    // input
    LobbyInput in;
    present_take_typed(in.typed);
    in.pad = effective_pad_buttons() | (g_replay_active ? present_pad_buttons() : 0u);   // (tests: a live keyboard works during a replay)
    in.gamepad = g_replay_active ? effective_pad_buttons() | present_gamepad_buttons() : present_gamepad_buttons();
    {
        PresentMouse m;
        present_take_mouse(m);
        in.mouse_inside = m.inside; in.mouse_x = m.x; in.mouse_y = m.y; in.clicks = m.clicks; in.wheel = m.wheel;
    }
    {   // RR_LOBBY_SCRIPT="frame:text;frame:@select;..." (tests): typed text ("\\n" = Enter, "\\e" = Esc) or a button
        // (@select / @start / @cross / @circle / @down / @up / @left / @right) at that many frames after the page opened
        static const char *script = std::getenv("RR_LOBBY_SCRIPT");
        ++g_lobby.frames_open;
        if (script != nullptr) {
            std::string sc(script);
            std::size_t pos = 0;
            while (pos < sc.size()) {
                std::size_t end = sc.find(';', pos);
                if (end == std::string::npos) end = sc.size();
                const std::string item = sc.substr(pos, end - pos);
                pos = end + 1;
                const std::size_t colon = item.find(':');
                // optional page letter: "B120:..." only on the server browser, "H120:..." only on the room setup page
                const char page = (!item.empty() && (item[0] == 'B' || item[0] == 'H')) ? item[0] : '\0';
                if (page != '\0' && (page == 'B') != g_lobby.browser) continue;
                if (colon == std::string::npos || std::strtoul(item.c_str() + (page != '\0' ? 1 : 0), nullptr, 10) != g_lobby.frames_open) continue;
                const std::string what = item.substr(colon + 1);
                static const std::pair<const char *, std::uint32_t> buttons[] = {{"@select", 0x1u}, {"@start", 0x8u}, {"@cross", 0x4000u}, {"@circle", 0x2000u},
                                                                                 {"@up", 0x10u}, {"@right", 0x20u}, {"@down", 0x40u}, {"@left", 0x80u},
                                                                                 {"@square", 0x8000u}, {"@triangle", 0x1000u}};
                bool was_button = false;
                for (const auto &[nm, bit] : buttons) if (what == nm) { in.pad |= bit; in.gamepad |= bit; was_button = true; }
                if (was_button) continue;
                for (std::size_t i = 0; i < what.size(); ++i) {
                    if (what[i] == '\\' && i + 1 < what.size()) { in.typed.push_back(what[i + 1] == 'n' ? 13 : what[i + 1] == 'e' ? 27 : what[i + 1]); ++i; }
                    else in.typed.push_back(static_cast<unsigned char>(what[i]));
                }
            }
        }
    }
    const LobbyRequest rq = lobby_ui_update(in);
    if (const std::uint32_t ev = lobby_ui_take_events(); ev != 0u) {
        if (ev & (kOskAccept | kOskInsert)) menu_sound_play(MenuSound::Confirm);
        else if (ev & (kOskCancel | kOskDelete)) menu_sound_play(MenuSound::Back);
        else menu_sound_play(MenuSound::Move);
    }
    const auto id_of = [](const std::array<std::uint8_t, 6> &a) { net::Mac m{}; std::copy(a.begin(), a.end(), m.begin()); return m; };
    const auto close = [&]() { lobby_ui_end(); g_lobby.active = false; };
    switch (rq.kind) {
    case LobbyRequestKind::None: break;
    case LobbyRequestKind::Refresh: node->browse_refresh(); if (g_lobby.list_gen < 0) g_lobby.list_gen = b.list_gen; break;
    case LobbyRequestKind::Lookup: if (g_lobby.lookup_gen == -1) g_lobby.lookup_gen = b.lookup_gen; node->browse_lookup(rq.text); break;
    case LobbyRequestKind::Probe: if (g_lobby.probe_gen == -1) g_lobby.probe_gen = b.probe_gen; node->browse_probe(id_of(rq.id), rq.text); break;
    case LobbyRequestKind::Joined: {
        net::RoomInfo ri = g_lobby.known[id_of(rq.id)];
        ri.mac = id_of(rq.id);
        if (!b.probe_ep.empty()) { ri.ep = b.probe_ep; ri.lep = b.probe_lep; }
        node->set_join_target(ri, rq.text);
        net::ScanEntry e;
        std::memcpy(e.name, ri.name, 8);
        e.bssid = ri.mac;
        node->ctl_scan_finish({e});
        log_line("NET", "server browser: joining room " + lobby_room_code(rq.id));
        close();
        break;
    }
    case LobbyRequestKind::Cancelled:
        node->clear_join_target();
        node->ctl_scan_finish({});
        log_line("NET", "server browser: closed without joining");
        close();
        break;
    case LobbyRequestKind::OpenRoom:
        node->set_host_options(rq.vis, rq.text);
        node->ctl_scan_finish({});
        lobby_save_prefs();
        log_line("NET", std::string("room setup: ") + (rq.vis == 0 ? "anyone can join" : rq.vis == 1 ? "password" : "room code only") + ", code " + node->room_code());
        close();
        break;
    }
    // RR_LOBBY_SHOTS=<folder> (tests): a picture of the page whenever it changes
    static const char *shots = std::getenv("RR_LOBBY_SHOTS");
    if (shots != nullptr && g_lobby.active) {
        const std::uint64_t key = lobby_ui_state_key();
        static std::uint64_t last_shot_frame = 0;   // (at most one picture every 30 frames: the spinner changes every 3)
        if (key != g_lobby.shot_key && g_frames_presented - last_shot_frame >= 30u) {
            last_shot_frame = g_frames_presented;
            g_lobby.shot_key = key;
            std::vector<std::uint8_t> px(960u * 544u * 4u, 255);
            lobby_ui_render(px.data(), 960, 544);
            std::error_code ec; std::filesystem::create_directories(shots, ec);
            char nm[96]; std::snprintf(nm, sizeof nm, "lobby_%03d_frame%07llu.png", ++g_lobby.shot_n, static_cast<unsigned long long>(g_frames_presented));
            write_png((std::filesystem::path(shots) / nm).string().c_str(), px.data(), 960u, 544u);
        }
    }
}

// Handles one of the network imports for the real network.  Returns the PSP result code.
std::uint32_t net_call(Runtime &rt, AllegrexContext &ctx, const std::string &fn, net::Node &node) {
    auto &mem = rt.memory();
    const auto arg = [&](int i) { return static_cast<std::uint32_t>(ctx.gpr[i]); };   // a0..a3 = 4..7, t0..t3 = 8..11
    node.poll();
    if (fn == "sceNetGetLocalEtherAddr") { net_write_mac(rt, arg(4), node.mac()); return 0u; }
    if (fn == "sceNetAdhocctlAddHandler") { g_net.ctl_entry = arg(4); g_net.ctl_arg = arg(5); return 1u; }
    if (fn == "sceNetAdhocctlDelHandler") { g_net.ctl_entry = 0u; return 0u; }
    if (fn == "sceNetAdhocctlScan") { if (!lobby_try_open(rt, ctx, node)) node.ctl_scan(); return 0u; }
    if (fn == "sceNetAdhocctlConnect") {
        char name[8] = {};
        for (std::uint32_t i = 0; i < 8u; ++i) name[i] = static_cast<char>(mem.load8(arg(4) + i));
        node.ctl_connect(name);
        return 0u;
    }
    if (fn == "sceNetAdhocctlDisconnect") {
        node.ctl_disconnect();
        // the game spins on its own state until the "disconnected" event arrives without ever yielding, so the handler
        // has to run right now (as on the real PSP firmware) instead of at the next sceKernelCheckCallback
        int ev = 0;
        while (node.next_ctl_event(ev)) {}   // stale events are moot once we've left the group
        if (g_net.ctl_entry != 0u) {
            ctx.set_gpr(2, 0u);
            notify_guest_function(rt, ctx, g_net.ctl_entry, 2u, 0u, "adhocctl_handler", "net");
            ctx.gpr[6] = g_net.ctl_arg;
        }
        return 0u;
    }
    if (fn == "sceNetAdhocctlGetState") { if (arg(4) != 0u) mem.store32(arg(4), node.ctl_connected() ? 1u : 0u); return 0u; }
    if (fn == "sceNetAdhocctlGetScanInfo") {
        // the buffer holds linked 28-byte entries: next, channel, name[8], bssid[6], pad[2], mode
        const auto found = node.ctl_scan_results();
        const std::uint32_t size_ptr = arg(4), buf = arg(5);
        if (size_ptr == 0u) return 0u;
        const std::uint32_t capacity = mem.load32(size_ptr);
        const std::uint32_t need = static_cast<std::uint32_t>(found.size()) * 28u;
        if (buf == 0u) { mem.store32(size_ptr, need); return 0u; }
        std::uint32_t written = 0u;
        for (std::size_t i = 0; i < found.size() && written + 28u <= capacity; ++i, written += 28u) {
            const std::uint32_t e = buf + written;
            for (std::uint32_t b = 0; b < 28u; ++b) mem.store8(e + b, 0u);
            mem.store32(e, (i + 1 < found.size() && written + 56u <= capacity) ? e + 28u : 0u);
            mem.store32(e + 4u, found[i].channel);
            for (std::uint32_t b = 0; b < 8u; ++b) mem.store8(e + 8u + b, static_cast<std::uint8_t>(found[i].name[b]));
            net_write_mac(rt, e + 16u, found[i].bssid);
        }
        mem.store32(size_ptr, written);
        return 0u;
    }
    if (fn == "sceNetAdhocPdpCreate") return static_cast<std::uint32_t>(node.pdp_create(arg(5)));
    if (fn == "sceNetAdhocPdpDelete") { node.pdp_delete(static_cast<int>(arg(4))); return 0u; }
    if (fn == "sceNetAdhocPdpSend") {
        const auto data = net_read_bytes(rt, arg(7), arg(8));
        return node.pdp_send(static_cast<int>(arg(4)), net_read_mac(rt, arg(5)), arg(6), data.data(), data.size()) == 0 ? 0u : 0x80410701u;
    }
    if (fn == "sceNetAdhocPdpRecv") {
        net::Mac src{};
        unsigned src_port = 0;
        std::uint8_t buf[1500];
        std::size_t len = 0;
        const std::uint32_t len_ptr = arg(8);
        const std::size_t capacity = std::min<std::size_t>(len_ptr != 0u ? mem.load32(len_ptr) : 0u, sizeof buf);
        if (node.pdp_recv(static_cast<int>(arg(4)), src, src_port, buf, capacity, len) != 0) return 0x80410709u;   // no data
        net_write_mac(rt, arg(5), src);
        if (arg(6) != 0u) { mem.store8(arg(6), static_cast<std::uint8_t>(src_port & 0xff)); mem.store8(arg(6) + 1u, static_cast<std::uint8_t>(src_port >> 8)); }
        for (std::size_t i = 0; i < len; ++i) mem.store8(arg(7) + static_cast<std::uint32_t>(i), buf[i]);
        if (len_ptr != 0u) mem.store32(len_ptr, static_cast<std::uint32_t>(len));
        return 0u;
    }
    if (fn == "sceNetAdhocMatchingCreate") {
        g_net.match_cb = mem.load32(arg(29));   // the ninth argument is on the stack
        return static_cast<std::uint32_t>(node.match_create(static_cast<int>(arg(4)), static_cast<int>(arg(5)), arg(6), arg(8), arg(9)));
    }
    if (fn == "sceNetAdhocMatchingStart") {
        const auto opt = net_read_bytes(rt, arg(10), arg(9));
        net_log_opt("start opt", opt);
        return node.match_start(static_cast<int>(arg(4)), opt.data(), opt.size()) == 0 ? 0u : 0x80410701u;
    }
    if (fn == "sceNetAdhocMatchingSetHelloOpt") {
        const auto opt = net_read_bytes(rt, arg(6), arg(5));
        net_log_opt("hello opt", opt);
        node.match_set_hello_opt(static_cast<int>(arg(4)), opt.data(), opt.size());
        return 0u;
    }
    if (fn == "sceNetAdhocMatchingSelectTarget" || fn == "sceNetAdhocMatchingCancelTarget") {
        const auto opt = net_read_bytes(rt, arg(7), arg(6));
        if (fn == "sceNetAdhocMatchingSelectTarget") node.match_select_target(static_cast<int>(arg(4)), net_read_mac(rt, arg(5)), opt.data(), opt.size());
        else node.match_cancel_target(static_cast<int>(arg(4)), net_read_mac(rt, arg(5)), opt.data(), opt.size());
        return 0u;
    }
    if (fn == "sceNetAdhocMatchingGetMembers") {
        // linked 12-byte entries: next, mac[6], pad[2]; the local player is first
        const auto members = node.match_members(static_cast<int>(arg(4)));
        const std::uint32_t size_ptr = arg(5), buf = arg(6);
        if (size_ptr == 0u) return 0u;
        const std::uint32_t capacity = mem.load32(size_ptr);
        const std::uint32_t need = static_cast<std::uint32_t>(members.size()) * 12u;
        if (buf == 0u) { mem.store32(size_ptr, need); return 0u; }
        std::uint32_t written = 0u;
        for (std::size_t i = 0; i < members.size() && written + 12u <= capacity; ++i, written += 12u) {
            const std::uint32_t e = buf + written;
            mem.store32(e, (i + 1 < members.size() && written + 24u <= capacity) ? e + 12u : 0u);
            net_write_mac(rt, e + 4u, members[i].mac);
            mem.store8(e + 10u, 0u); mem.store8(e + 11u, 0u);
        }
        mem.store32(size_ptr, written);
        return 0u;
    }
    if (fn == "sceNetAdhocMatchingStop") { node.match_stop(static_cast<int>(arg(4))); return 0u; }
    if (fn == "sceNetAdhocMatchingDelete") { node.match_delete(static_cast<int>(arg(4))); return 0u; }
    if (fn == "sceNetAdhocMatchingGetPoolMaxAlloc") return 0x4000u;
    return 0u;   // Init / Term and the like
}

// Hands one pending network event to the game's handler (called from sceKernelCheckCallback, so on the main thread
// and never while another callback is running).  Returns true when a handler was started.
bool net_deliver(Runtime &rt, AllegrexContext &ctx) {
    net::Node *node = net::shared_node();
    if (node == nullptr) return false;
    node->set_frame_ms(static_cast<double>(g_vblank_period_us) / 1000.0);   // (prediction: the game's display delay is counted in frames)
    node->poll();
    const auto active = g_callback_frames.find(g_current_uid);
    if (active != g_callback_frames.end() && !active->second.empty()) return false;
    int event = 0;
    if (g_net.ctl_entry != 0u && node->next_ctl_event(event)) {
        ctx.set_gpr(2, 0u);
        notify_guest_function(rt, ctx, g_net.ctl_entry, static_cast<std::uint32_t>(event), 0u, "adhocctl_handler", "net");
        ctx.gpr[6] = g_net.ctl_arg;   // handler(event, error, arg)
        return true;
    }
    net::MatchEvent me;
    if (g_net.match_cb != 0u && node->next_match_event(me)) {
        if (g_net.scratch == 0u) g_net.scratch = arena_allocate(0x400u, 16u);
        if (g_net.scratch == 0u) return false;
        net_write_mac(rt, g_net.scratch, me.mac);
        const std::uint32_t opt_ptr = g_net.scratch + 16u;
        for (std::size_t i = 0; i < me.opt.size() && i < 0x300u; ++i) rt.memory().store8(opt_ptr + static_cast<std::uint32_t>(i), me.opt[i]);
        ctx.set_gpr(2, 0u);
        notify_guest_function(rt, ctx, g_net.match_cb, static_cast<std::uint32_t>(me.id), static_cast<std::uint32_t>(me.event), "matching_callback", "net");
        ctx.gpr[6] = g_net.scratch;                                            // mac
        ctx.gpr[7] = static_cast<std::uint32_t>(std::min<std::size_t>(me.opt.size(), 0x300u));   // option length
        ctx.gpr[8] = opt_ptr;                                                  // option data
        return true;
    }
    return false;
}
namespace gates {
std::vector<std::uint8_t> table;                 // one byte per guest word of kFunctionGateSpan
std::vector<std::uint32_t> skip;                 // addresses gated at the raised rate
bool profile = false;
std::uint32_t current = 0u;                      // last function entered (profiling only)
std::unordered_map<std::uint32_t, std::uint32_t> frame_calls;     // this frame's calls per function (profiling)
std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> call_log;   // function -> calls per traced frame
std::unordered_map<std::uint64_t, std::uint32_t> edges;                    // (function << 32 | return address) -> calls
int profile_owners = 0;                                                    // traces currently profiling
int profile_frame = 0;
Runtime *rt = nullptr;

// RR_AUTOPILOT=<path file> (test rig only): drives the player's car during races instead of the recorded presses.  The path
// file has "d x z" lines (track distance +0x860 -> position), e.g. averaged from a played race's RPOS lines; the car steers
// (right 0x20 raises atan2(x, z), left 0x80 lowers it) toward the path point a little ahead of its own track distance and
// holds accelerate (0x4000); stuck against a wall it brakes / reverses (0x8000) steering the other way for a while.
bool autopilot_buttons(std::uint32_t &out) {
    static const char *file = std::getenv("RR_AUTOPILOT");
    if (file == nullptr || rt == nullptr) return false;
    static std::vector<std::array<double, 3>> path;
    static double lap = 0.0;
    if (path.empty()) {
        std::ifstream in(file);
        double d, x, z;
        while (in >> d >> x >> z) path.push_back({d, x, z});
        if (path.empty()) { file = nullptr; return false; }
        lap = path.back()[0] + (path.back()[0] - path[path.size() - 2][0]);
        log_line(category::kHle, "AUTOPILOT path " + std::to_string(path.size()) + " points, lap " + std::to_string(lap));
    }
    // one scripted pause per race (RR_AP_PAUSE_TICK, default 3000 record calls in): Start, ~2 s later Cross on "continue"
    static std::uint32_t pause_phase = 0, pause_steps = 0, pause_anchor = ~0u;
    static const std::uint32_t pause_tick = [] { const char *e = std::getenv("RR_AP_PAUSE_TICK"); return e ? static_cast<std::uint32_t>(std::atoi(e)) : 3000u; }();
    if (pause_anchor != g_rig_anchor) { pause_anchor = g_rig_anchor; pause_phase = 0; }
    if (pause_phase == 0u && pause_tick != 0u && g_rig_ticks >= pause_tick) { pause_phase = 1; pause_steps = 0; log_line(category::kHle, "AUTOPILOT pause at tick " + std::to_string(g_rig_ticks)); }
    if (pause_phase == 1u) {
        ++pause_steps;
        if (pause_steps <= 10u) { out = 0x0008u; return true; }          // Start
        if (pause_steps <= 300u) { out = 0u; return true; }
        if (pause_steps <= 310u) { out = 0x4000u; return true; }         // Cross: continue
        if (pause_steps <= 330u) { out = 0u; return true; }
        pause_phase = 2; log_line(category::kHle, "AUTOPILOT resumed at tick " + std::to_string(g_rig_ticks));
    }
    // RR_AP_REV_HOLD=1: full throttle held all the way through the countdown (rev-limiter comparisons between frame rates)
    if (std::getenv("RR_AP_REV_HOLD") != nullptr && (g_opp_state == 0u || g_opp_state == 0xffffffffu)) { out = 0x4000u; return true; }
    // RR_AP_NO_REV=1: no throttle until GO (race state >= 1), for launch comparisons without the countdown revving
    if (std::getenv("RR_AP_NO_REV") != nullptr && (g_opp_state == 0u || g_opp_state == 0xffffffffu)) { out = 0u; return true; }
    // RR_AP_SCRIPT="t0-t1:hex;t0-t1:hex;..." : maneuver test -- nothing until GO, then the listed buttons between t0 and t1 seconds
    // after GO, timed in 60 Hz race ticks (driver input record calls) so the test is the same at every frame rate.
    if (const char *sc = std::getenv("RR_AP_SCRIPT")) {
        static std::uint64_t go_tick = ~0ull; static std::uint32_t go_anchor = ~0u;
        if (g_opp_state == 0u || g_opp_state == 0xffffffffu) { out = 0u; return true; }
        if (go_anchor != g_rig_anchor || go_tick == ~0ull) { go_anchor = g_rig_anchor; go_tick = g_rig_ticks; log_line(category::kHle, "APSCRIPT GO at tick " + std::to_string(g_rig_ticks) + " frame " + std::to_string(g_frames_presented)); }
        const double t = static_cast<double>(g_rig_ticks - go_tick) / 60.0;
        std::uint32_t b = 0u;
        std::string spec(sc);
        for (std::size_t q = 0; q < spec.size();) {
            const std::size_t c = spec.find(';', q);
            const std::string part = spec.substr(q, c == std::string::npos ? std::string::npos : c - q);
            double t0 = 0, t1 = 0; unsigned bt = 0;
            if (std::sscanf(part.c_str(), "%lf-%lf:%x", &t0, &t1, &bt) == 3 && t >= t0 && t < t1) b |= bt;
            if (c == std::string::npos) break;
            q = c + 1;
        }
        out = b;
        return true;
    }
    // RR_AP_LAUNCH=1: launch test -- nothing until GO, then plain full throttle (no steering, no stuck handling)
    if (std::getenv("RR_AP_LAUNCH") != nullptr) { out = (g_opp_state == 0u || g_opp_state == 0xffffffffu) ? 0u : 0x4000u; return true; }
    auto &m = rt->memory();
    const std::uint32_t hud = m.load32(0x08a448f0u);
    const std::uint32_t car = (hud >= 0x08800000u && hud < 0x0a000000u) ? m.load32(hud + 0xc4u) : 0u;
    if (car < 0x08800000u || car >= 0x0a000000u) return false;
    const auto f = [&](std::uint32_t o) { return static_cast<double>(std::bit_cast<float>(m.load32(car + o))); };
    const double x = f(0x16a0u), z = f(0x16a8u), d = f(0x860u);
    static std::array<std::array<double, 2>, 8> hist{};
    static std::uint32_t hn = 0, stuck = 0, reverse = 0;
    static double heading = 0.0;
    const auto old = hist[hn % 8u];
    hist[hn % 8u] = {x, z};
    ++hn;
    const double vx = x - old[0], vz = z - old[1], sp = std::hypot(vx, vz) / 8.0;   // per step
    heading = std::atan2(-f(0x260u), f(0x268u));   // forward axis of the car matrix at +0x240
    // target: path point 30 units ahead
    double td = std::fmod(d + 18.0 + 50.0 * sp, lap); if (td < 0) td += lap;
    std::size_t i = 0;
    while (i + 1 < path.size() && path[i + 1][0] <= td) ++i;
    const auto &p0 = path[i];
    const auto &p1 = path[(i + 1) % path.size()];
    double span = p1[0] - p0[0]; if (span <= 0) span += lap;
    double t = (td - p0[0]) / span; t = std::clamp(t, 0.0, 1.0);
    const double tx = p0[1] + (p1[1] - p0[1]) * t, tz = p0[2] + (p1[2] - p0[2]) * t;
    double err = std::atan2(tx - x, tz - z) - heading;
    while (err > 3.14159265) err -= 6.2831853;
    while (err < -3.14159265) err += 6.2831853;
    std::uint32_t b = 0x4000u;
    static bool moving = false;
    static double best_d = 0.0;
    static std::uint32_t since_progress = 0;
    if (sp > 0.15) moving = true;
    // progress along the track (mod lap); no progress for 2 s -> stuck
    double gain = d - best_d; if (gain < -lap / 2) gain += lap; if (gain > lap / 2) gain -= lap;
    if (!moving || gain > 3.0) { best_d = d; since_progress = 0; } else ++since_progress;
    if (std::getenv("RR_APLOG") != nullptr && hn % 72u == 0u) {
        char t[200]; std::snprintf(t, sizeof t, "AP d=%.1f pos=%.1f,%.1f sp=%.3f head=%.2f tgt=%.1f,%.1f err=%.2f rev=%u", d, x, z, sp, heading, tx, tz, err, reverse);
        log_line(category::kHle, t);
    }
    if (reverse == 0u && since_progress > 288u) {
        reverse = 260u; since_progress = 0;
        log_line(category::kHle, "AUTOPILOT stuck at d=" + std::to_string(d) + ", backing off");
    }
    if (reverse > 0u) {
        --reverse;
        if (reverse == 0u) since_progress = 0;
        out = 0x8000u | (err > 0 ? 0x80u : 0x20u);   // backing up: steer the other way to swing the nose toward the target
        return true;
    }
    if (err > 0.05) b |= 0x20u; else if (err < -0.05) b |= 0x80u;
    if (std::fabs(err) > 0.35 && sp > 0.22) b &= ~0x4000u;
    if (std::fabs(err) > 0.8 && sp > 0.15) b |= 0x8000u;
    out = b;
    return true;
}
bool generated_with_gates = false;
std::uint64_t skipped = 0;

bool fast_rate_active();
// The race scene's state machine (0x08842224) runs every step from the moment a race loads until it is left; its state
// (object +0x38) picks the handler: 0 0x0883d588, 1 0x0883e06c, 2 0x0883dde0 (the race itself), 3.. others.  The hook
// watches it to decide when the raised rate applies (track intro included) and logs each state change.
constexpr std::uint32_t kRaceScene = 0x08842224u;
// The intro and race handlers themselves also count as "race running": races with opponents reach them through a
// different state machine than Time Attack.
constexpr std::uint32_t kIntroHandler = 0x0883e06cu, kRaceHandler = 0x0883dde0u;
std::uint32_t last_handler = 0u;
// The race update calls three methods of the game-mode object (+0x28): vtable +0x8, +0xc, +0x14 (Time Attack:
// 088f3790 / 0883ca5c / 088f3c08; races with opponents: 08842d4c / 0883ca5c / 088431ac).  They run the mode's own
// timers (countdown, lap timer), so they are gated with the race systems; found from the object when the race runs.
std::vector<std::uint32_t> mode_fns;
// Nitro (per car body; bodies at [manager+0x14], stride 0x27a0, count [manager+0x10], manager = a0 of the car update
// 0x0883e70c).  The gauge is one accumulator, +0x2740 (0..300, three tanks of 100; 0x0890180c adds +0x1e78 * +0x1e84 or
// a similar product every step while drifting); the tanks at +0x72c.. are display copies made from it each step.  A boost
// (state +0x273c: 0 idle, 1 ramp up, 2 boosting, 3 ramp down; 0x088c61b8) runs a timer +0x2748 down from 100 by +0x274c per
// step and ramps the boost factor +0x2734 by +0x275c (up) / +0x2750 (down) per step.  At the raised rate: the gauge's
// per-step gain is scaled by 1/mult (a hook on the car update sees each step's change), and the three per-step rate fields
// are divided by mult in place (re-applied whenever the game rewrites one).
constexpr std::uint32_t kCarUpdate = 0x0883e70cu;
constexpr std::uint32_t kPresim = 0x088c82acu, kPresimEnd = 0x088c72f0u;   // race-start settle and its final call
constexpr std::uint32_t kTorqueRamp = 0x088c0240u;   // drive engagement (torque scale ramp)
constexpr std::uint32_t kGearbox = 0x088bf840u;   // automatic gearbox (shift countdown +0xc)
constexpr std::uint32_t kTrackShift = 0x088ab8f0u, kCarTrackSpeed = 0x088bae44u, kCarTrackEvents = 0x088bc9e8u;
constexpr std::uint32_t kCarIntegrate = 0x088bb3b4u;   // car motion integrator (a0 = car), reads the road-height history +0x85c/+0x8d4
// Race positions and lap timing 0x088214b8 runs every step (a lap is noticed when a car's previous and current positions
// straddle the line, which a skipped step would miss).  Its timers are integer milliseconds that grow by ~17 per step
// (per-car blocks of 0x6e0 bytes from 0x08a4d000); at the raised rate each such step's growth is scaled by 1/mult.
constexpr std::uint32_t kRaceInfoUpdate = 0x088214b8u;
// Line-crossing test (called from the HUD / lap timer code on the 60 Hz ticks): compares a car's distance along the course
// now (car + 0x860) with the previous one (car + 0x8d0).  The car update writes the previous one every step, so at 120 Hz
// the test only saw the last half tick and missed a crossing half the time (the lap timer never started).
constexpr std::uint32_t kCrossTest = 0x088acd5cu;
// Per-car update of the computer cars 0..7 (class with vtable +0xd0 = 0x088c545c; f12 = step factor loaded from a global table):
// it advances the car's course distance (+0x860) by f12 * (timescale * f13 + f14) every step, with no dependence on the car's
// own dt, so at the raised rate those opponents drove mult times too fast.  f12 is scaled by 1/mult for cars of the computer
// array.  (The other computer cars are updated by another class, 0x088b29f8 / 0x088b3478, whose step is already scaled.)
constexpr std::uint32_t kCpuCarUpdate = 0x088c545cu;
constexpr std::uint32_t kRaceInfoLo = 0x08a4d000u, kRaceInfoHi = 0x08a53000u;
std::vector<std::uint32_t> race_info_prev;
std::vector<double> race_info_frac;
// Buttons at the raised rate: a press is noticed by comparing this step's buttons with the last step's, and some of the
// code that acts on a new press runs only on the 60 Hz ticks (nitro on RB).  A press that arrives on a skipped step would
// be gone by the next tick, so button changes are only handed to the game on tick steps (at most one step later, ~8 ms
// at 120 fps).  pad_step / pad_frame: the step counter at the last pad read; pad_delta: how far the counter seen by the
// gated functions in the same frame is ahead of it (measured, normally 0 or 1).
std::uint32_t pad_step = 0, pad_delta = 0;
std::uint64_t pad_frame = ~0ull;
bool pad_delta_known = false;
// The 60 Hz ticks are counted from tick_origin, set where a race (and its replay) starts (race state 0), so a replay sees its
// gated systems -- the recorded-input player 0x0883BF48 among them -- on exactly the same steps of the race as the recording
// did.  A replay re-simulates the race from the recorded inputs; with the tick pattern shifted by a step or two between the
// race and the replay, the car got its inputs on other steps and drove off the recorded line.  RR_TICKSYNC_OFF=1: old behaviour.
std::uint32_t tick_origin = 0;
std::uint32_t game_step();
bool tick_at(std::uint32_t s) {
    const double m = g_hle_options.fps_mult;
    const double x = static_cast<double>(static_cast<std::int32_t>(s - tick_origin));
    return std::floor((x + 1.0) / m) != std::floor(x / m);
}
// Race systems tick on the simulation count, not on the game step: sim_count counts the steps on which the car simulation ran
// (the car update 0x0883E70C, once per simulated step), so paused steps do not move the 60 Hz pattern and a replay (which has no
// pause) gets every gated system -- and the recorded-input player -- on exactly the same simulated steps as the race.
// sim_origin is set at the first record / playback call after each driver input init.
std::uint32_t sim_count = 0, last_car_step = 0xffffffffu, sim_origin = 0;
bool tsync_pending = false;
std::uint32_t tsync_comp_until = 0;
bool sim_origin_set = false;
std::uint32_t sim_index() { return last_car_step == game_step() ? sim_count : sim_count + 1u; }   // index of this step's simulation
bool race_tick(bool strict = true) { (void)strict; return tick_at(game_step()); }
// Pad / controller handover and the menu repeat fixes: the race pattern (tick_at) stands still while the race is paused (the
// TICKSYNC pause compensation moves its origin every paused step), which froze them -- on a non-tick step the pad never handed a
// button over again and the pause screen could not be left.  Whenever the car simulation did not run on the previous step
// (paused, menus, loading) they use the plain step pattern instead.
bool menu_tick(std::uint32_t s) {
    const bool car_ran = last_car_step != 0xffffffffu && last_car_step + 1u >= game_step();
    if (car_ran || std::getenv("RR_MENU_TICK_OFF") != nullptr) return tick_at(s);
    const double m = g_hle_options.fps_mult;
    const double x = static_cast<double>(s);
    return std::floor((x + 1.0) / m) != std::floor(x / m);
}
constexpr std::uint32_t kGaugeField = 0x2740u;
constexpr std::uint32_t kRateFieldOffsets[] = {0x274cu, 0x275cu, 0x2750u};
struct CarNitro { float gauge{}; bool have{}; std::array<std::uint32_t, 3> written{}; };
std::unordered_map<std::uint32_t, CarNitro> nitro_cars;
// Races with opponents: the mode's per-step method (vtable +0x8 = 0x08842d4c) runs the race state machine and must see every step (gated
// to the 60 Hz ticks, cars on a slope slow down: Union Hill).  It also counts time (state counter at mode+0x10, HUD race clock
// +0x48 and lap timers +0x74 / +0x84, +50 per call), so those words are held back to 1/mult of each step's increase.
// RR_OPP_MODE8_GATE=1 gates the method again (old behaviour).
constexpr std::uint32_t kMusicSelectUpdate = 0x0890daf8u;
std::uint32_t audio_pool_obj = 0;   // SAS voice pool object (32 slots of 76 bytes, [slot] = state byte; key-on mask +3044, key-off mask +3048)
constexpr std::uint32_t kDriverInputInit = 0x0883bd6cu; // driver input record/playback init (a1 = 0 record, 1 play)
constexpr std::uint32_t kDriverInputUpdate = 0x0883bf48u; // per-tick record / playback // driver input record/playback init (a1 = 0 record, 1 play)
constexpr std::uint32_t kInfoWinUpdate = 0x0890922cu; // Fj::C2DInfoWin update (gap window, scrolling 8-bit racers)
constexpr std::uint32_t kCtrlUpdate = 0x08845e48u;   // controller object update (own auto-repeat countdowns at +0x48)
constexpr std::uint32_t kHeliUpdate = 0x088d7ff0u;  // CHeliWork update (helicopter path time += rate each call)
constexpr std::uint32_t kSePlay = 0x08913d4cu;      // sound library play(bank a0, sound a1, ..., handle t0, mode t1)
constexpr std::uint32_t kPadUpdate = 0x0889d3fcu;   // per-frame pad object update (called once per step from the main loop 0x08805188)
constexpr std::uint32_t kOppModeStep = 0x08842d4cu, kTaModeStep = 0x088f3790u;   // opponent races / Time Attack: same class layout (state +0x58, counter +0x10)
std::uint32_t opp_mode_obj = 0, opp_mode_fn = kOppModeStep;
struct ScaledWord { std::uint32_t last{}; double acc{}; bool valid{}; };
std::unordered_map<std::uint32_t, ScaledWord> scaled_words;
void scale_word(psprecomp::GuestMemory &mem, std::uint32_t addr, std::uint32_t max_delta) {
    const std::uint32_t v = mem.load32(addr);
    ScaledWord &w = scaled_words[addr];
    if (w.valid && v > w.last && v - w.last <= max_delta) {
        const double before = w.acc;
        w.acc += static_cast<double>(v - w.last) / g_hle_options.fps_mult;
        const std::uint32_t fixed = w.last + static_cast<std::uint32_t>(std::floor(w.acc) - std::floor(before));
        mem.store32(addr, fixed);
        w.last = fixed;
    } else {
        w.last = v; w.acc = 0.0;
    }
    w.valid = true;
}
// Same for a word that counts down (each step's decrease scaled by 1/mult).
void scale_word_down(psprecomp::GuestMemory &mem, std::uint32_t addr, std::uint32_t max_delta) {
    const std::uint32_t v = mem.load32(addr);
    ScaledWord &w = scaled_words[addr ^ 0x80000000u];
    if (w.valid && v < w.last && w.last - v <= max_delta) {
        const double before = w.acc;
        w.acc += static_cast<double>(w.last - v) / g_hle_options.fps_mult;
        const std::uint32_t fixed = w.last - static_cast<std::uint32_t>(std::floor(w.acc) - std::floor(before));
        mem.store32(addr, fixed);
        w.last = fixed;
    } else {
        w.last = v; w.acc = 0.0;
    }
    w.valid = true;
}
// The state machine must see every step while the race runs (cars on slopes); the countdown and
// the other states keep their 60 Hz pace by being gated like before.  RR_OPP_ALWAYS=1 runs it every step in all states.
// States seen in a race: 0 (track intro), 1 (countdown), 2 (race).  RR_OPP_GATED_STATES=0,1 (decimal list, the default) gates the method in
// those states only; RR_OPP_GATED_STATES=9 gates none.
// Every change of the state (mode+0x58) is logged as "OPPSTATE".
bool opp_mode_gate_update(psprecomp::GuestMemory &mem) {
    if (table.empty() || opp_mode_obj == 0u) return false;
    static const std::string gated_states = [] { const char *e = std::getenv("RR_OPP_GATED_STATES"); return std::string(",") + (e ? e : "0,1") + ","; }();
    static const bool always = std::getenv("RR_OPP_ALWAYS") != nullptr;
    const std::uint32_t state = mem.load32(opp_mode_obj + 0x58u);
    static std::uint32_t last_state = 0xffffffffu;
    g_opp_state = state;
    if (state != last_state) {
        last_state = state;
        if (state == 0u && std::getenv("RR_TICKSYNC_OFF") == nullptr) { race_info_prev.clear(); scaled_words.clear(); }
        if (state == 3u) rig_note_race_end();
        log_line(category::kHle, "OPPSTATE state=" + std::to_string(state) + " frame=" + std::to_string(g_frames_presented) + " counter=" + std::to_string(mem.load32(opp_mode_obj + 0x10u)) + " obj=" + hex32(opp_mode_obj));
    }
    char key[24]; std::snprintf(key, sizeof key, ",%u,", state);
    const bool gate = !always && gated_states.find(key) != std::string::npos;
    table[(opp_mode_fn - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    const auto it = std::find(skip.begin(), skip.end(), opp_mode_fn);
    if (gate && it == skip.end()) skip.push_back(opp_mode_fn);
    else if (!gate && it != skip.end()) skip.erase(it);
    return gate;
}
void opp_mode_scale(Runtime &rt) {
    if (opp_mode_obj < 0x08800000u || opp_mode_obj >= 0x0a000000u || g_hle_options.fps_mult <= 1.0) return;
    auto &mem = rt.memory();
    // In the gated states (pre-race, countdown) the method already runs at the 60 Hz pace, so its counters must not be scaled
    // again: scaled twice the state counter stood still at 120 fps and the 120-step wait before the race music never ended.
    const bool gated = opp_mode_gate_update(mem);
    if (gated && std::getenv("RR_OPP_DOUBLE_SCALE") == nullptr) {
        scaled_words.clear();
        return;
    }
    scale_word(mem, opp_mode_obj + 0x10u, 4u);
    // Race clocks in the object at [0x08A447FC] (written by the mode's race update, 50 per 60 Hz tick): +0x30 counts up (the radar /
    // info window bottom left times its zoom-in, display and zoom-out from it: 181 ticks shown) and +0x20 counts down.  The mode
    // runs every step in the race state, so both ran 2.4x fast at 144 fps (the radar's racers and slide went 2.4x too fast).
    if (std::getenv("RR_RACECLOCK_FIX_OFF") == nullptr) {
        const std::uint32_t g = mem.load32(0x08a447fcu);
        if (g >= 0x08800000u && g < 0x0a000000u) { scale_word(mem, g + 0x30u, 200u); scale_word_down(mem, g + 0x20u, 200u); }
    }
    const std::uint32_t hud = mem.load32(0x08a448f0u);
    if (hud >= 0x08800000u && hud < 0x0a000000u)
        for (const std::uint32_t off : {0x48u, 0x74u, 0x84u}) scale_word(mem, hud + off, 200u);
}
std::uint32_t car_manager = 0;                   // a0 of the last car update (car bodies: [mgr+0x14] + i*0x27a0, count [mgr+0x10])
bool mode_fns_changed = false;
// Track intro camera 0x0888273c: advances its clock (object +0xc7c) by 5 per step and places the camera from it.  At the
// raised rate the clock is held back so it advances 5/mult per step on average (RR_INTRO=gate runs the whole camera at
// 60 Hz instead, RR_INTRO=off leaves it alone).
constexpr std::uint32_t kIntroCamera = 0x0888273cu;
std::uint32_t intro_obj = 0, intro_last = 0;
bool intro_valid = false;
double intro_acc = 0.0;
// Each intro shot is a camera object (pointers at camera +0xbf4, index = low half of +0xc44) whose vtable +0x14 moves the
// camera one step (called from the intro camera, return address 0x08882890).  Shot types with a known clock field get the
// same treatment as the camera clock (advance 5/mult per step); the others run their step on the 60 Hz ticks only, so
// they keep the right speed (at 60 Hz camera motion) until their own clocks are found.
constexpr std::uint32_t kShotStepReturn = 0x08882890u;
struct ShotClock { std::uint32_t step; std::uint32_t offset; };
constexpr ShotClock kShotClocks[] = {{0x088f8c7cu, 0x1bcu}};   // 088f8c7c: +0x1bc += 5 per step (start 088f8ac4 zeroes it)
std::vector<std::uint32_t> shot_steps;           // discovered vtable +0x14 functions
struct ClockTrack { std::uint32_t last{}; double acc{}; bool valid{}; };
std::unordered_map<std::uint32_t, ClockTrack> shot_clocks;   // per shot object
std::uint32_t last_shot_step = 0;
void discover_shots(std::uint32_t camera);
int intro_mode() {   // 0 scale clock, 1 gate, 2 off
    static const int m = [] { const char *e = std::getenv("RR_INTRO"); if (e == nullptr) return 0; const std::string v(e); return v == "gate" ? 1 : v == "off" ? 2 : 0; }();
    return m;
}
std::uint64_t scene_frame = 0;                   // last presented frame during which the race scene ran
std::uint32_t scene_state = 0xFFFFFFFFu;
// When a race scene starts, the gated functions run on its first two steps whatever the tick phase: if the first call is
// skipped (v0 = 0) the scene start can dereference a null result (null + 0x85d0 in the model draw 0x088da280) and crash.
bool warm_pending = false, warm_have = false, cam_started = false, idle_seen = false, pause_muted = false;
std::uint32_t ailog_step0 = 0, ailog_last = ~0u;
std::uint32_t warm_first = 0;
std::uint32_t game_step() {
    const std::uint32_t obj = rt->memory().load32(0x089dc8a0u);
    return (obj >= 0x08800000u && obj < 0x0a000000u) ? rt->memory().load32(obj + 4u) : 0u;
}
// Car sounds 0x08891484 (race update candidate 5): each call copies the sound-request flags (82 words, 1 = play that sound;
// buffer [[obj+0x30]+8]) and starts the requested sounds.  RR_SNDLOG=1 logs, for every call that finds a request, the step
// counter and the ids, to compare a 60 Hz run with a raised-rate run.
constexpr std::uint32_t kSoundManager = 0x08891484u, kSoundStart = 0x08891890u;
constexpr std::uint32_t kSpriteSlotSetX = 0x08918854u;
constexpr std::uint32_t kEffectStep = 0x08918048u;
constexpr std::uint32_t kSlotUpdate = 0x08918300u;   // a0 = sprite slot struct; reads the slot's object pointer at [a0 + 0x17C]
constexpr std::uint32_t kAnimStep = 0x088d9124u;   // animation player update (time += rate), see hook()
constexpr std::uint32_t kMailTake = 0x08818A28u;   // takes the pending game event (RR_MAILLOG)
bool sndlog_enabled() { static const bool on = std::getenv("RR_SNDLOG") != nullptr; return on; }
// RR_CALLLOG=0x08914f58,0x08898768 logs the first calls of those guest functions (game step, presented frame, return address,
// a0..a3), to find out which game code issues a request (e.g. the race music start) and when, at 60 versus a raised rate.
const std::vector<std::uint32_t> &calllog_addrs() {
    static const std::vector<std::uint32_t> v = [] {
        std::vector<std::uint32_t> r;
        const char *e = std::getenv("RR_CALLLOG");
        if (e == nullptr) return r;
        const std::string s(e);
        for (std::size_t p = 0; p < s.size();) {
            const std::size_t c = s.find(',', p);
            const std::string part = s.substr(p, c == std::string::npos ? std::string::npos : c - p);
            if (!part.empty()) r.push_back(static_cast<std::uint32_t>(std::strtoul(part.c_str(), nullptr, 0)));
            if (c == std::string::npos) break;
            p = c + 1;
        }
        return r;
    }();
    return v;
}
void sound_requests(std::uint32_t obj) {
    static std::uint32_t lines = 0;
    auto &m = rt->memory();
    static bool once = false;
    if (!once) { once = true; log_line(category::kHle, "SNDREQ first call: obj=" + hex32(obj) + " [obj+0x30]=" + hex32(m.load32(obj + 0x30u)) + " buf=" + hex32(m.load32(m.load32(obj + 0x30u) + 8u))); }
    if (lines > 20000u || obj < 0x08800000u || obj >= 0x0a000000u) return;
    const std::uint32_t inner = m.load32(obj + 0x30u);
    if (inner < 0x08800000u || inner >= 0x0a000000u) return;
    const std::uint32_t buf = m.load32(inner + 8u);
    if (buf < 0x08800000u || buf >= 0x0a000000u) return;
    std::string ids;
    for (std::uint32_t i = 0; i < 0x52u; ++i)
        if (m.load32(buf + 4u * i) != 0u) ids += " " + std::to_string(i) + "=" + std::to_string(m.load32(buf + 4u * i));
    if (ids.empty()) return;
    ++lines;
    log_line(category::kHle, "SNDREQ step=" + std::to_string(game_step()) + " frame=" + std::to_string(g_frames_presented) + " ids:" + ids);
}
// Scene setup 0x088ee8b4 (candidate 11) runs on the 60 Hz ticks until the player's car moves (the race-start billboards and lights
// flip their pictures once per call), then every step (rear-view mirror, plane and helicopter follow the camera).  Animation rates and
// the world clock are scaled down while it runs every step.  RR_MIRROR_OLD=1: always gated; RR_MIRROR_ALWAYS=1: always every step.
bool clock_pending = false;
std::uint32_t clock_c0 = 0u, clock_f0 = 0u;
void flush_clock() {
    if (!clock_pending) return;
    clock_pending = false;
    auto &m = rt->memory();
    if (m.load32(0x08a43aecu) != clock_c0) { m.store32(0x08a43aecu, clock_c0); m.store32(0x08a43af0u, clock_f0); }
}
bool scene_every_step() {
    if (std::getenv("RR_MIRROR_OLD") != nullptr) return false;
    if (std::getenv("RR_MIRROR_ALWAYS") != nullptr) return std::find(skip.begin(), skip.end(), 0x088ee8b4u) == skip.end();
    return cam_started;
}
// RR_AUDIOLOG: handle watch.  After each sound start the handle's voice (+0) and play id (+8) are remembered; every hooked function entry
// checks them, and a change made by anything other than the library's own play / stop is logged as HCHANGE with the function being
// entered, the previous hooked function and the stack chain -- to find the code that overwrites the drift handle's play id (its
// stop then misses the voice, which loops for ever).
namespace hwatch {
struct W { std::uint32_t h, voice, id; };
std::vector<W> list;
std::uint32_t cur_play_handle = 0, prev_addr = 0, prev_ra = 0, prev_step = 0;
// Every sound handle the game plays (always recorded).  Many live inside the race-info range 0x08A4D000-0x08A53000 whose
// ~17-per-step millisecond timers are scaled at the raised rate; their play-id word (+8) must not be treated as a timer.
std::vector<std::uint32_t> sound_handles;
const std::uint32_t kAddrs[] = {0x088cc0a8u, 0x088cc27cu, 0x088cc360u, 0x088cc704u, 0x088ccbf4u, 0x088cd310u, 0x088cdbd8u, 0x088cde6cu,
                                0x088ce77cu, 0x088ca1fcu, 0x0889f070u, 0x0889f394u, 0x089106fcu, 0x0891086cu, 0x08911160u, 0x08918048u,
                                0x089180f4u, 0x08917c60u, 0x08910360u, 0x0891009cu, 0x08910158u, 0x08918854u, 0x08912d90u, 0x08912f34u,
                                0x089132d4u, 0x08913990u, 0x08912b28u, 0x0891441cu, 0x089144ecu, 0x08914820u, 0x08914d4cu};
}  // namespace hwatch

// Race HUD: the manager update 0x088F9AC8 (once per step, returns 0) calls every Fj::C2D widget's update (vtable +0x1C: gap window
// InfoWin, RaceInfo, Flag, Lap, Position, times, nitro, course map, meters, ...); drawing is a separate pass.
constexpr std::uint32_t kHudUpdates[] = {0x088f9ac8u};   // the HUD manager update: loops over all widgets calling vtable +0x1C (return value ignored)
bool is_hud_update(std::uint32_t a) { for (const std::uint32_t h : kHudUpdates) if (h == a) return true; return false; }
// Draw distance.  The scene list builder 0x088D95EC (a0 = scene, a1 = camera cell, a2 = view) reads a per-cell visibility
// string (precomputed by the game's tools, "PVS"): for every object group, byte pairs (skip, count) of the objects that may be
// seen from that cell, 0xFF ends a group.  Objects in it then get a frustum test.  Pop-in is objects that are left out of the
// current cell's string.  RR_DRAW_CELLS=N (or --draw-distance N) draws what is visible from the N cells before and after
// the current one too: the strings are merged (union) into a buffer of our own and the table entry points there for the call.
constexpr std::uint32_t kPvsBuild = 0x088d95ecu;
int cull_mode() { static const int n = std::getenv("RR_NOCULL") ? std::atoi(std::getenv("RR_NOCULL")) : 0; return n; }
int draw_cells() {
    static const int n = [] {
        if (const char *e = std::getenv("RR_DRAW_CELLS")) return std::max(0, std::atoi(e));
        return std::max(0, g_hle_options.draw_cells);
    }();
    return n;
}
void pvs_hook(psprecomp::AllegrexContext &ctx) {
    auto &m = rt->memory();
    static std::uint32_t g_ptr = 0u;          // address of the global that holds the course object (lui/lw at 0x088D95F4/0x088D9634)
    static std::uint32_t patched_at = 0u, patched_old = 0u, buf = 0u;
    static int logged = 0;
    if (patched_at != 0u) { if (m.load32(patched_at) == buf) m.store32(patched_at, patched_old); patched_at = 0u; }   // undo the previous call's redirect
    const int cells = draw_cells();
    if (g_ptr == 0u) {
        const std::uint32_t lui = m.load32(0x088d95f4u), lw = m.load32(0x088d9634u);
        if ((lui >> 26) != 0x0fu || (lw >> 26) != 0x23u) return;
        g_ptr = ((lui & 0xffffu) << 16) + static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(lw & 0xffffu)));
    }
    const auto ok = [](std::uint32_t a) { return a >= 0x08800000u && a < 0x0a000000u; };
    const std::uint32_t course = m.load32(g_ptr);
    if (!ok(course)) return;
    const std::uint32_t t1 = m.load32(course + 0x28u), t3 = m.load32(course + 0x24u);
    const std::uint32_t head = m.load32(course);
    if (!ok(t3) || !ok(head)) return;
    const std::uint32_t entry = t3 + t1 * 32u;
    const std::uint32_t table = m.load32(entry + 0x1cu);
    const std::int32_t cell = static_cast<std::int32_t>(ctx.gpr[5]);
    const std::int32_t groups = static_cast<std::int32_t>(m.load32(head + 0x30u));
    if (!ok(table) || cell < 0 || groups <= 0 || groups > 4096) return;
    // parse one cell's string into per-group visible flags; returns false if it looks wrong
    const auto parse = [&](std::uint32_t str, std::vector<std::vector<std::uint8_t>> &vis) {
        if (!ok(str)) return false;
        std::uint32_t p = str;
        for (std::int32_t gidx = 0; gidx < groups; ++gidx) {
            std::uint32_t idx = 0u;
            for (int guard = 0;; ++guard) {
                if (guard > 4096 || p - str > 65536u) return false;
                const std::uint32_t skip = m.load8(p);
                if (skip == 0xffu) { ++p; break; }
                const std::uint32_t cnt = m.load8(p + 1u);
                if (cnt == 0xffu) return false;
                p += 2u;
                idx += skip;
                auto &v = vis[static_cast<std::size_t>(gidx)];
                if (v.size() < idx + cnt) v.resize(idx + cnt, 0u);
                for (std::uint32_t k = 0; k < cnt; ++k) v[idx + k] = 1u;
                idx += cnt;
            }
        }
        return true;
    };
    if (logged < 40 && std::getenv("RR_PVS_LOG") != nullptr) {
        ++logged;
        {   // the view object's frustum plane matrices (+0x90..+0x10F) and near/far (+0x1E0..+0x1EF)
            const std::uint32_t v = ctx.gpr[6];
            std::string t = "PVSVIEW frame=" + std::to_string(g_frames_presented) + " view=" + hex32(v);
            if (ok(v)) {
                for (std::uint32_t o = 0x90u; o < 0x110u; o += 4u) { char b[24]; std::snprintf(b, sizeof b, "%s%.4g", (o - 0x90u) % 16u ? "," : " | ", static_cast<double>(std::bit_cast<float>(m.load32(v + o)))); t += b; }
                t += " ||";
                for (std::uint32_t o = 0x1e0u; o < 0x1f0u; o += 4u) { char b[24]; std::snprintf(b, sizeof b, " %.5g", static_cast<double>(std::bit_cast<float>(m.load32(v + o)))); t += b; }
                t += " pos";
                for (std::uint32_t o = 0u; o < 0x10u; o += 4u) { char b[24]; std::snprintf(b, sizeof b, " %.5g", static_cast<double>(std::bit_cast<float>(m.load32(v + o)))); t += b; }
            }
            log_line(category::kHle, t);
        }
        std::vector<std::vector<std::uint8_t>> v(static_cast<std::size_t>(groups));
        const bool good = parse(m.load32(table + static_cast<std::uint32_t>(cell) * 4u), v);
        std::size_t n = 0; for (auto &g : v) for (auto b : g) n += b;
        log_line(category::kHle, "PVS frame=" + std::to_string(g_frames_presented) + " cell=" + std::to_string(cell) + " groups=" + std::to_string(groups) +
                                     " visible=" + std::to_string(n) + (good ? "" : " (parse failed)") + " table=" + hex32(table));
    }
    if (std::getenv("RR_PVS_CELLS") != nullptr) {   // every change of camera cell (per table)
        static std::map<std::uint32_t, std::int32_t> last;
        auto it = last.find(table);
        if (it == last.end() || it->second != cell) {
            last[table] = cell;
            log_line(category::kHle, "PVSCELL frame=" + std::to_string(g_frames_presented) + " table=" + hex32(table) + " cell=" + std::to_string(cell));
        }
    }
    if (cells <= 0) return;
    std::vector<std::vector<std::uint8_t>> vis(static_cast<std::size_t>(groups));
    if (!parse(m.load32(table + static_cast<std::uint32_t>(cell) * 4u), vis)) return;
    for (std::int32_t c = cell - cells; c <= cell + cells; ++c) {
        if (c < 0 || c == cell) continue;
        std::vector<std::vector<std::uint8_t>> extra(static_cast<std::size_t>(groups));
        if (!parse(m.load32(table + static_cast<std::uint32_t>(c) * 4u), extra)) continue;   // past the last cell: not a string
        for (std::size_t gi = 0; gi < extra.size(); ++gi) {
            if (vis[gi].size() < extra[gi].size()) vis[gi].resize(extra[gi].size(), 0u);
            for (std::size_t k = 0; k < extra[gi].size(); ++k) vis[gi][k] |= extra[gi][k];
        }
    }
    std::vector<std::uint8_t> out;
    for (const auto &v : vis) {
        std::size_t i = 0;
        while (i < v.size()) {
            std::size_t skip = 0;
            while (i < v.size() && v[i] == 0u) { ++i; ++skip; }
            if (i >= v.size()) break;
            std::size_t cnt = 0;
            while (i + cnt < v.size() && v[i + cnt] != 0u) ++cnt;
            while (skip > 254u) { out.push_back(254u); out.push_back(0u); skip -= 254u; }
            while (cnt > 254u) { out.push_back(static_cast<std::uint8_t>(skip)); out.push_back(254u); skip = 0; cnt -= 254u; i += 254u; }
            out.push_back(static_cast<std::uint8_t>(skip)); out.push_back(static_cast<std::uint8_t>(cnt));
            i += cnt;
        }
        out.push_back(0xffu);
    }
    constexpr std::uint32_t kBufSize = 0x10000u;
    if (out.size() > kBufSize) return;
    if (buf == 0u) {
        buf = arena_allocate(kBufSize, 64u);
        log_line(category::kHle, "DRAWDIST cells=" + std::to_string(cells) + " buffer=" + hex32(buf));
        if (buf == 0u) return;
    }
    for (std::size_t i = 0; i < out.size(); ++i) m.store8(buf + static_cast<std::uint32_t>(i), out[i]);
    patched_at = table + static_cast<std::uint32_t>(cell) * 4u;
    patched_old = m.load32(patched_at);
    m.store32(patched_at, buf);
}

// ---------------------------------------------------------------------------------------------------------------------
// Native high-rate menus (--menu-native).  The menus are a scene-graph UI engine: 0x0884F290 calls the scene update
// 0x0895FD20(scene, dt) with dt = the pool float at 0x089A6B34 (1.0 = one frame); the update keeps a fixed-step accumulator
// and runs whole ticks (traversal 0x089622E0, then 0x08960200), and 0x0884F2B4 -> 0x0895FEE8 draws the scene.  At the
// raised rate dt becomes 1/mult, so the engine still ticks 60 times a second (nothing speeds up), and between ticks the
// float values the last tick changed (positions, sizes, angles, colours of every UI node) are drawn blended from their
// value before the tick to their value after it, by the share of the tick period that has passed.  The true values are
// put back before the next update, so the engine never sees the blended ones.
namespace ui {
constexpr std::uint32_t kUpdateWrap = 0x0884f290u, kSceneUpdate = 0x0895fd20u, kTickTraverse = 0x089622e0u, kTickPost = 0x08960200u,
                        kSceneDraw = 0x0895fee8u, kDtConst = 0x089a6b34u, kAfterDraw = 0x08825750u;   // (0x0887061C draws, calls 0x08825750, then updates)
constexpr std::uint32_t kHeapLo = 0x09200000u, kHeapHi = 0x09400000u;   // where the UI nodes live (write traces at several menus)
std::uint64_t last_frame = 0;            // last presented frame the UI update ran (raises the rate while the menus run)
bool active() { return g_hle_options.menu_native && g_hle_options.fps_mult > 1.0; }
std::vector<std::uint32_t> before;       // heap before the first tick of the current update
bool have_before = false, ticked = false;
struct Word { std::uint32_t addr, v0, v1; };
std::vector<Word> words;                 // floats the last tick(s) changed
struct Saved { std::uint32_t addr, orig, blended; };
std::vector<Saved> saved;   // values replaced by blended ones (put back before the next update / at the frame's end)
int steps_since = 0;
std::uint64_t ticks = 0, blends = 0, rebuilt = 0;
bool plausible(std::uint32_t v) {
    if (v == 0u) return true;
    const float f = std::bit_cast<float>(v);
    const float a = std::fabs(f);
    return std::isfinite(f) && a > 1e-5f && a < 1e6f;
}
const std::uint32_t *heap(Runtime &rt) {
    return reinterpret_cast<const std::uint32_t *>(static_cast<const psprecomp::GuestMemory &>(rt.memory()).raw_pointer(kHeapLo, kHeapHi - kHeapLo));
}
void restore(Runtime &rt) {
    if (saved.empty()) return;
    for (const Saved &e : saved) if (rt.memory().load32(e.addr) == e.blended) rt.memory().store32(e.addr, e.orig);   // (unless reused since)
    saved.clear();
}
void on_entry(Runtime &rt, std::uint32_t address, psprecomp::AllegrexContext &ctx) {
    if (address == kAfterDraw) return;   // (the GE output of the scene is built after this call: keep the blended values until the update)
    if (address == kUpdateWrap) {
        last_frame = g_frames_presented;
        restore(rt);
        have_before = false;
        // dt: 1/mult while the rate is raised, the game's own 1.0 otherwise
        const float want = gates::fast_rate_active() ? static_cast<float>(1.0 / g_hle_options.fps_mult) : 1.0f;
        if (rt.memory().load32(kDtConst) != std::bit_cast<std::uint32_t>(want)) rt.memory().store32(kDtConst, std::bit_cast<std::uint32_t>(want));
        return;
    }
    if (address == kTickTraverse && ctx.gpr[31] == 0x0895fe4cu) {
        if (!have_before) {
            restore(rt);
            if (const std::uint32_t *h = heap(rt)) { before.assign(h, h + (kHeapHi - kHeapLo) / 4u); have_before = true; }
        }
        return;
    }
    if (address == kTickPost && ctx.gpr[31] == 0x0895fe54u) { ticked = true; ++ticks; return; }
    if (address == kSceneDraw) {
        restore(rt);
        if (!gates::fast_rate_active() || std::getenv("RR_MENU_BLEND_OFF") != nullptr) { words.clear(); ticked = false; return; }
        if (ticked && have_before) {
            ticked = false;
            words.clear();
            if (const std::uint32_t *h = heap(rt)) {
                static const float jump = [] { const char *e = std::getenv("RR_MENU_BLEND_JUMP"); return e ? static_cast<float>(std::atof(e)) : 160.0f; }();
                static const std::pair<std::uint32_t, std::uint32_t> rng = [] {
                    unsigned a = kHeapLo, b = kHeapHi;
                    if (const char *e = std::getenv("RR_MENU_BLEND_RANGE")) std::sscanf(e, "%x,%x", &a, &b);
                    return std::pair<std::uint32_t, std::uint32_t>{a, b};
                }();
                // Only words that changed in one of the last few ticks too (a running animation, not a node being set up),
                // and no tick in which pointers that had been still for a while changed (nodes made / freed: the screen is
                // being rebuilt; pointers that keep changing are display buffers flipping).
                static std::vector<std::uint32_t> last_change;
                static std::uint32_t tick_no = 0;
                ++tick_no;
                last_change.resize(before.size(), 0u);
                std::size_t pointer_moves = 0;
                auto is_ptr = [](std::uint32_t v) { return v >= 0x08800000u && v < 0x0a000000u; };
                for (std::size_t i = 0; i < before.size(); ++i) {
                    const std::uint32_t at = kHeapLo + static_cast<std::uint32_t>(i * 4u);
                    const std::uint32_t v0 = before[i], v1 = h[i];
                    if (v0 == v1) continue;
                    const bool recent = last_change[i] != 0u && tick_no - last_change[i] <= 4u;
                    last_change[i] = tick_no;
                    if (is_ptr(v0) || is_ptr(v1)) {
                        if (!recent) {
                            ++pointer_moves;
                            static int plog = std::getenv("RR_MENU_PTR_LOG") ? std::atoi(std::getenv("RR_MENU_PTR_LOG")) : 0;
                            if (plog > 0) { --plog; char b[96]; std::snprintf(b, sizeof b, "MENUPTR frame %llu %08X %08X>%08X", static_cast<unsigned long long>(g_frames_presented), at, v0, v1); log_line(category::kHle, b); }
                        }
                        continue;
                    }
                    if (!recent) continue;
                    if (at < rng.first || at >= rng.second) continue;
                    if (!plausible(v0) || !plausible(v1)) continue;
                    const float d = std::fabs(std::bit_cast<float>(v1) - std::bit_cast<float>(v0));
                    if (!(d <= jump)) continue;
                    words.push_back({kHeapLo + static_cast<std::uint32_t>(i * 4u), v0, v1});
                }
                {   // A screen being built or torn down (memory freed and reused) is not an animation: values jump to / from 0.
                    std::size_t zeroish = 0;
                    for (const Word &w : words) if (w.v0 == 0u || w.v1 == 0u) ++zeroish;
                    if (zeroish * 4u > words.size() || pointer_moves > 2u) { ++rebuilt; words.clear(); }
                }
                static int blog = std::getenv("RR_MENU_BLEND_LOG") ? std::atoi(std::getenv("RR_MENU_BLEND_LOG")) : 0;
                if (blog > 0) {
                    --blog;
                    std::string l = "MENUBLEND frame " + std::to_string(g_frames_presented) + " words=" + std::to_string(words.size()) + " ptr=" + std::to_string(pointer_moves) + ":";
                    for (const Word &w : words) { char b[64]; std::snprintf(b, sizeof b, " %08X:%g>%g", w.addr, static_cast<double>(std::bit_cast<float>(w.v0)), static_cast<double>(std::bit_cast<float>(w.v1))); l += b; }
                    log_line(category::kHle, l);
                }
            }
            steps_since = 0;
        } else ++steps_since;
        const int m = std::max(1, static_cast<int>(std::lround(g_hle_options.fps_mult)));
        if (words.empty() || steps_since + 1 >= m) return;   // (t = 1: the true values)
        const float t = static_cast<float>(steps_since + 1) / static_cast<float>(m);   // 1/m at the first draw after a tick, 1 just before the next
        for (const Word &w : words) {
            const std::uint32_t cur = rt.memory().load32(w.addr);
            if (cur != w.v1) continue;   // changed since by something else: leave it alone
            const float a = std::bit_cast<float>(w.v0), b = std::bit_cast<float>(w.v1);
            const std::uint32_t bl = std::bit_cast<std::uint32_t>(a + (b - a) * t);
            saved.push_back({w.addr, cur, bl});
            rt.memory().store32(w.addr, bl);
        }
        ++blends;
    }
}
// Machine select: the car turntable angle ([[a0+12]+0x210]) gets += 1.0 every call of 0x088742CC, which runs once per
// presented frame.  At the raised rate the step that call made is cut to 1/mult (seen at the next entry).
constexpr std::uint32_t kTurntable = 0x088742ccu;
void turntable(Runtime &rt, psprecomp::AllegrexContext &ctx) {
    static std::uint32_t at = 0, prev = 0;
    auto ok = [](std::uint32_t a) { return a >= 0x08800000u && a < 0x09ffff00u && (a & 3u) == 0u; };
    const std::uint32_t a0 = ctx.gpr[4];
    if (!ok(a0 + 12u)) return;
    const std::uint32_t obj = rt.memory().load32(a0 + 12u);
    if (!ok(obj)) return;
    const std::uint32_t a = obj + 0x210u;
    const std::uint32_t cur = rt.memory().load32(a);
    if (a == at && gates::fast_rate_active() && std::getenv("RR_TURNTABLE_OFF") == nullptr) {
        const float p = std::bit_cast<float>(prev), c = std::bit_cast<float>(cur);
        if (c == p + 1.0f) {
            const float n = p + static_cast<float>(1.0 / g_hle_options.fps_mult);
            rt.memory().store32(a, std::bit_cast<std::uint32_t>(n));
            prev = std::bit_cast<std::uint32_t>(n);
            return;
        }
    }
    at = a; prev = cur;
}
// Values the menus step once per presented frame (not by the UI engine's dt): at the raised rate the step made since the
// last look is cut to 1/mult.  Floats get the fraction directly, +1 int counters through an accumulator.
struct Stepped { std::uint32_t addr = 0, left = 0; double acc = 0.0; bool valid = false; };
void scale_step(Runtime &rt, Stepped &t, std::uint32_t addr, bool flt, float maxd) {
    const std::uint32_t cur = rt.memory().load32(addr);
    if (t.valid && t.addr == addr && gates::fast_rate_active()) {
        const double m = g_hle_options.fps_mult;
        if (flt) {
            const float l = std::bit_cast<float>(t.left), c = std::bit_cast<float>(cur), d = c - l;
            if (d != 0.0f && std::fabs(d) <= maxd && plausible(t.left) && plausible(cur)) {
                const std::uint32_t n = std::bit_cast<std::uint32_t>(static_cast<float>(l + d / m));
                rt.memory().store32(addr, n); t.left = n; return;
            }
        } else {
            const std::int32_t d = static_cast<std::int32_t>(cur - t.left);
            if (d == 1 || d == -1) {
                t.acc += d / m;
                const std::int32_t k = static_cast<std::int32_t>(t.acc);
                t.acc -= k;
                const std::uint32_t n = t.left + static_cast<std::uint32_t>(k);
                rt.memory().store32(addr, n); t.left = n; return;
            }
        }
    }
    t.addr = addr; t.left = cur; t.acc = 0.0; t.valid = true;
}
// 0x088728C8 (menu manager, once per presented frame): the bottom ticker waits [obj+0x1C0] frames (to 121), then scrolls
// [obj+0x1BC] by 1 (2 with a button held) a frame; [obj+0x1EC] is the machine-select car yaw (+0.5 degree a frame).
constexpr std::uint32_t kMenuMgr = 0x088728c8u;
void menu_mgr(Runtime &rt, psprecomp::AllegrexContext &ctx) {
    if (std::getenv("RR_MENU_STEPS_OFF") != nullptr) return;
    static Stepped hold, scroll, yaw;
    const std::uint32_t a0 = ctx.gpr[4];
    if (a0 < 0x08800000u || a0 >= 0x09ffff00u || (a0 & 3u) != 0u) return;
    const std::uint32_t obj = rt.memory().load32(a0 + 12u);
    if (obj < 0x08800000u || obj >= 0x09ffe000u || (obj & 3u) != 0u) return;
    scale_step(rt, hold, obj + 0x1c0u, false, 0.0f);
    scale_step(rt, scroll, obj + 0x1bcu, true, 4.0f);
    scale_step(rt, yaw, obj + 0x1ecu, true, 0.05f);
}
} // namespace ui

// Netplay prediction.  The other player's car is a physics car that the game steers toward a target position kept in its
// controller (a0 of 0x08888AD0 / 0x08888CF4: target x, y, z floats at +0x30/+0x34/+0x38).  That target trails the newest
// received position by about 8 frames, which is most of the delay players see.  Before the steering runs (0x08888AD0) the
// target is moved ahead by its own per-frame motion times RR_NET_LEAD frames (default 8); the true value is put back at the
// start of the next frame's controller update (0x08888BD8), so the game's own bookkeeping never sees the moved target.
namespace netpred {
constexpr std::uint32_t kStart = 0x08888bd8u, kSteer = 0x08888ad0u;
struct Ctl { float prev[3]{}; bool have_prev{false}; float saved[3]{}; bool moved{false}; float off[3]{}; bool have_off{false}; };
std::unordered_map<std::uint32_t, Ctl> ctls;
// The true target of one controller goes back at the start of that same car's next update (0x08888BD8 gets an object 0x540
// below the steering controller).  Putting every car's back at the first start of the frame was wrong with two or more other
// cars: the game writes each controller back after all cars have run, so the restore of every car but the last was
// overwritten by the moved value and the lead piled up frame after frame (the other car flew off the track; 3-player races).
constexpr std::uint32_t kStartToSteer = 0x540u;
void restore_one(Runtime &rt, std::uint32_t a0, Ctl &c) {
    if (!c.moved) return;
    for (int i = 0; i < 3; ++i) rt.memory().store32(a0 + 0x30u + 4u * i, std::bit_cast<std::uint32_t>(c.saved[i]));
    c.moved = false;
}
void restore(Runtime &rt, std::uint32_t start_a0) {
    if (auto it = ctls.find(start_a0 + kStartToSteer); it != ctls.end()) { restore_one(rt, it->first, it->second); return; }
    if (ctls.size() == 1u) restore_one(rt, ctls.begin()->first, ctls.begin()->second);   // (unknown layout, one car: as before)
}
void on_entry(Runtime &rt, std::uint32_t address, psprecomp::AllegrexContext &ctx) {
    {   // RR_NETPRED_TRACE=frame: the calls (address, a0, a1, target) of 20 frames from that frame on (diagnostic)
        static const std::uint64_t tf = [] { const char *e = std::getenv("RR_NETPRED_TRACE"); return e ? std::strtoull(e, nullptr, 10) : 0ull; }();
        if (tf != 0u && g_frames_presented >= tf && g_frames_presented < tf + 20u) {
            char b[200];
            std::snprintf(b, sizeof b, "NETPREDTRACE f=%llu %s a0=%08X a1=%08X t=%.1f,%.1f,%.1f", static_cast<unsigned long long>(g_frames_presented), address == kStart ? "start" : "steer",
                          ctx.gpr[4], ctx.gpr[5], static_cast<double>(std::bit_cast<float>(rt.memory().load32(ctx.gpr[4] + 0x30u))),
                          static_cast<double>(std::bit_cast<float>(rt.memory().load32(ctx.gpr[4] + 0x34u))), static_cast<double>(std::bit_cast<float>(rt.memory().load32(ctx.gpr[4] + 0x38u))));
            log_line(category::kHle, b);
        }
    }
    if (address == kStart) { restore(rt, ctx.gpr[4]); return; }
    const std::uint32_t a0 = ctx.gpr[4];
    if (a0 < 0x08800000u || a0 >= 0x09ffff00u) return;
    Ctl &c = ctls[a0];
    float t[3];
    for (int i = 0; i < 3; ++i) t[i] = std::bit_cast<float>(rt.memory().load32(a0 + 0x30u + 4u * i));
    if (c.moved) {
        // Not put back since the last move.  If the target still holds exactly the moved value, it is ours: start from the
        // true one, so the lead can never pile up.  (If the game wrote a new target meanwhile, that one is used as it is.)
        bool ours = true;
        for (int i = 0; i < 3; ++i) if (std::bit_cast<std::uint32_t>(t[i]) != std::bit_cast<std::uint32_t>(c.saved[i] + c.off[i])) ours = false;
        if (ours) for (int i = 0; i < 3; ++i) t[i] = c.saved[i];
        c.moved = false;
    }
    float v[3] = {0, 0, 0};
    bool ok = c.have_prev;
    for (int i = 0; i < 3; ++i) { v[i] = t[i] - c.prev[i]; if (!std::isfinite(v[i]) || std::fabs(v[i]) > 2000.0f) ok = false; }
    for (int i = 0; i < 3; ++i) c.prev[i] = t[i];
    c.have_prev = true;
    {   // RR_NETPRED_LOG=1: once a second, the measured per-frame motion of the target next to the reported velocity (+0x40)
        static const bool lg = std::getenv("RR_NETPRED_LOG") != nullptr;
        static std::uint64_t last = 0;
        if (lg && g_frames_presented - last >= 60u) {
            last = g_frames_presented;
            char b[200];
            std::snprintf(b, sizeof b, "NETPRED frame %llu dt=%.3f,%.3f,%.3f vel=%.3f,%.3f,%.3f", static_cast<unsigned long long>(g_frames_presented), v[0], v[1], v[2],
                          static_cast<double>(std::bit_cast<float>(rt.memory().load32(a0 + 0x40u))), static_cast<double>(std::bit_cast<float>(rt.memory().load32(a0 + 0x44u))),
                          static_cast<double>(std::bit_cast<float>(rt.memory().load32(a0 + 0x48u))));
            log_line(category::kHle, b);
        }
    }
    if (!ok) return;
    static const double frames_at60 = [] { const char *e = std::getenv("RR_NET_LEAD"); return e ? std::atof(e) : 8.0; }();
    // The trail is counted in frames of the game's own rate: ~8 at 60 fps, ~8 at 144 too (measured 55 ms there), so lead by
    // the same number of (current) frames.
    // velocity: the reported one (+0x40.., per 60 Hz frame) scaled to the current frame, unless RR_NET_LEAD_MEASURED=1
    static const bool measured = std::getenv("RR_NET_LEAD_MEASURED") != nullptr;
    if (!measured) {
        const double m = std::max(0.25, g_hle_options.fps_mult);
        for (int i = 0; i < 3; ++i) {
            const float f = std::bit_cast<float>(rt.memory().load32(a0 + 0x40u + 4u * i));
            if (std::isfinite(f) && std::fabs(f) < 5000.0f) v[i] = static_cast<float>(f / m);
        }
    }
    // lead: the game's own trail (frames_at60 frames of the current rate) + the age of the data (network), in frames
    static const bool no_age = std::getenv("RR_NET_LEAD_NOAGE") != nullptr;
    double age_ms = 0.0;
    if (!no_age) if (net::Node *node = net::shared_node()) age_ms = node->data_age_ms();
    const double frame_ms = static_cast<double>(g_vblank_period_us) / 1000.0;
    const float k = static_cast<float>(frames_at60 + std::min(age_ms, 300.0) / frame_ms);
    // the forward offset is smoothed over time (RR_NET_LEAD_SMOOTH_MS, default 150): the lead multiplies every small change
    // of the reported velocity, and unsmoothed that shows as jitter in the other car's motion
    static const double tau = [] { const char *e = std::getenv("RR_NET_LEAD_SMOOTH_MS"); return e ? std::max(1.0, std::atof(e)) : 150.0; }();
    const float a = static_cast<float>(1.0 - std::exp(-frame_ms / tau));
    for (int i = 0; i < 3; ++i) {
        const float want = v[i] * k;
        c.off[i] = c.have_off ? c.off[i] + (want - c.off[i]) * a : want;
    }
    c.have_off = true;
    for (int i = 0; i < 3; ++i) { c.saved[i] = t[i]; rt.memory().store32(a0 + 0x30u + 4u * i, std::bit_cast<std::uint32_t>(t[i] + c.off[i])); }
    c.moved = true;
    {
        static const std::uint64_t tf = [] { const char *e = std::getenv("RR_NETPRED_TRACE"); return e ? std::strtoull(e, nullptr, 10) : 0ull; }();
        if (tf != 0u && g_frames_presented >= tf && g_frames_presented < tf + 20u) {
            char b[220];
            std::snprintf(b, sizeof b, "NETPREDTRACE   a0=%08X v=%.1f,%.1f,%.1f k=%.1f off=%.1f,%.1f,%.1f", a0, v[0], v[1], v[2], static_cast<double>(k), c.off[0], c.off[1], c.off[2]);
            log_line(category::kHle, b);
        }
    }
}
bool active() {
    static const bool on = [] { const char *e = std::getenv("RR_NET_LEAD"); return !(e != nullptr && std::atof(e) <= 0.0); }();
    return on && net::networking_enabled();
}
} // namespace netpred

bool hook(std::uint32_t address, psprecomp::AllegrexContext &ctx) {
    if (ui::active() && (address == ui::kUpdateWrap || address == ui::kTickTraverse || address == ui::kTickPost || address == ui::kSceneDraw || address == ui::kAfterDraw)) ui::on_entry(*rt, address, ctx);
    if (address == ui::kTurntable && ui::active()) ui::turntable(*rt, ctx);
    if ((address == netpred::kStart || address == netpred::kSteer) && netpred::active()) netpred::on_entry(*rt, address, ctx);
    if (address == ui::kMenuMgr && ui::active()) ui::menu_mgr(*rt, ctx);
    if (address == kPvsBuild) { pvs_hook(ctx); return false; }
    if ((address == 0x088d08c8u || address == 0x088d0940u || address == 0x088d09b8u) && cull_mode() != 0) {
        // RR_NOCULL=n (experiment): bit 1 = 0x088D08C8, 2 = 0x088D0940, 4 = 0x088D09B8 always report "visible"
        const int bit = address == 0x088d08c8u ? 1 : address == 0x088d0940u ? 2 : 4;
        if ((cull_mode() & bit) != 0) { ctx.gpr[2] = 1u; return true; }
    }
    if (address == kSePlay) {
        const std::uint32_t h = ctx.gpr[8];
        if (h >= kRaceInfoLo && h < kRaceInfoHi && std::find(hwatch::sound_handles.begin(), hwatch::sound_handles.end(), h) == hwatch::sound_handles.end() &&
            hwatch::sound_handles.size() < 4096u)
            hwatch::sound_handles.push_back(h);
    }
    if (hwatch::list.size() != 0u) {
        static int hlines = 0;
        auto &m = rt->memory();
        for (auto &w : hwatch::list) {
            const std::uint32_t v = m.load32(w.h), id = m.load32(w.h + 8u);
            if (v == w.voice && id == w.id) continue;
            if (w.h != hwatch::cur_play_handle && hlines < 20000) {
                ++hlines;
                std::string chain;
                const std::uint32_t sp = ctx.gpr[29];
                int found = 0;
                for (std::uint32_t off = 0; off < 0x180u && found < 8 && sp >= 0x08800000u && sp + off + 4u < 0x0a000000u; off += 4u) {
                    const std::uint32_t x = m.load32(sp + off);
                    if (x >= 0x08804048u && x < 0x08972800u && (x & 3u) == 0u && (m.load32(x - 8u) >> 26) == 3u) { chain += ' ' + hex32(x); ++found; }
                }
                char b[260];
                std::snprintf(b, sizeof b, "HCHANGE step=%u handle=%08X voice %d->%d id 0x%X->0x%X at=%08X ra=%08X prev=%08X prev_ra=%08X prev_step=%u chain=", game_step(), w.h,
                              static_cast<int>(w.voice), static_cast<int>(v), w.id, id, address, ctx.gpr[31], hwatch::prev_addr, hwatch::prev_ra, hwatch::prev_step);
                log_line(category::kHle, std::string(b) + chain);
            }
            w.voice = v; w.id = id;
        }
    }
    hwatch::prev_addr = address; hwatch::prev_ra = ctx.gpr[31]; hwatch::prev_step = game_step();
    if (profile) { current = address; ++frame_calls[address]; ++edges[(static_cast<std::uint64_t>(address) << 32u) | ctx.gpr[31]]; }
    if (address == kSpriteSlotSetX && std::getenv("RR_SLOT_GUARD_OFF") == nullptr) {
        // Sprite-slot setter 0x08918854 (a0 = sprite table, a1 = slot, a2 = value) writes a halfword through the slot's
        // object pointer [a0 + a1*0x50 + 0x180].  An empty slot (null) made a HUD update fault at +0x1C during the pause /
        // countdown scenes; skip the call when the pointer is not a valid object.
        const std::uint32_t t = ctx.gpr[4], slot = ctx.gpr[5];
        std::uint32_t obj = 0u;
        if (t >= 0x08800000u && t < 0x0a000000u && slot < 0x1000u) obj = rt->memory().load32(t + slot * 0x50u + 0x180u);
        if (obj < 0x08800000u || obj >= 0x0a000000u) {
            static int n = 0;
            if (n++ < 5) log_line(category::kHle, "slot guard: 0x08918854 skipped (table " + hex32(t) + " slot " + std::to_string(slot) + " null object) ra=" + hex32(ctx.gpr[31]));
            ctx.gpr[2] = 0u;
            return true;
        }
    }
    if (address == kAnimStep && std::getenv("RR_ANIMLOG") != nullptr) {
        // Diagnostic: once per wall-clock second, for every animation player that was stepped: how many calls it got and how much
        // its time word (+0x470) advanced (the rate word +0x474 is shown too).  Compare 60 / 120 / 144 fps runs of the same scene.
        struct P { std::uint32_t calls{}; float t_first{}, t_last{}; bool have{}; std::uint32_t rate{}; };
        static std::unordered_map<std::uint32_t, P> players;
        static auto last = std::chrono::steady_clock::now();
        static int lines = 0;
        const std::uint32_t o = ctx.gpr[4];
        if (o >= 0x08800000u && o < 0x0a000000u) {
            auto &m = rt->memory();
            P &p = players[o];
            const float t = std::bit_cast<float>(m.load32(o + 0x470u));
            if (!p.have) { p.t_first = t; p.have = true; }
            p.t_last = t; ++p.calls; p.rate = m.load32(o + 0x474u);
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last >= std::chrono::seconds(1) && lines < 900) {
            last = now;
            std::string l = "ANIMLOG frame=" + std::to_string(g_frames_presented) + " step=" + std::to_string(game_step()) + " every_step=" + (scene_every_step() ? "1" : "0") + " players:";
            for (auto &[addr, p] : players) {
                if (p.calls == 0u) continue;
                const float d = p.t_last - p.t_first;
                if (d != 0.0f) {
                    char b[120]; std::snprintf(b, sizeof b, " [%08X calls=%u dt=%.3f rate=%.4f]", addr, p.calls, static_cast<double>(d), static_cast<double>(std::bit_cast<float>(p.rate)));
                    l += b;
                }
                p = P{};
            }
            log_line(category::kHle, l);
            ++lines;
        }
    }
    if (address == kHeliUpdate && g_hle_options.fps_mult > 1.0 && fast_rate_active() && std::getenv("RR_HELI_SCALE_OFF") == nullptr) {
        // Helicopter (Map::Event::CHeliWork, vtable 0x089B1E00; update 0x088D7FF0 called every step): it advances its own path
        // time, [+0x470] += [+0x474], once per call (not through the animation player 0x088D9124), so at the raised rate it flew
        // its path mult times too fast.  Divide the rate by the multiplier; remembered per object so a rate the game writes
        // again is scaled again.
        static std::unordered_map<std::uint32_t, std::uint32_t> scaled;
        const std::uint32_t o = ctx.gpr[4];
        if (o >= 0x08800000u && o < 0x0a000000u) {
            auto &m = rt->memory();
            const std::uint32_t cur = m.load32(o + 0x474u);
            auto it = scaled.find(o);
            if (it == scaled.end() || it->second != cur) {
                const std::uint32_t nb = std::bit_cast<std::uint32_t>(std::bit_cast<float>(cur) * static_cast<float>(1.0 / g_hle_options.fps_mult));
                m.store32(o + 0x474u, nb);
                scaled[o] = nb;
                static int lines = 0;
                if (lines++ < 20) log_line(category::kHle, "HELI rate obj=" + hex32(o) + " " + std::to_string(std::bit_cast<float>(cur)) + " -> " + std::to_string(std::bit_cast<float>(nb)));
            }
        }
    }
    if (address == kAnimStep && g_hle_options.fps_mult > 1.0 && fast_rate_active() && std::getenv("RR_ANIM_SCALE_OFF") == nullptr &&
        scene_every_step()) {
        // Animation player (a0): [+0x470] time += [+0x474] rate, once per call (the plane, the helicopter, ...).  With the scene
        // setup 0x088ee8b4 running every step (which keeps the rear-view mirror steady) it is called every step instead of
        // every 60 Hz tick, so the rate is divided by the multiplier.  The scaled value is remembered per player: if the game
        // writes a new rate, that one is scaled again.
        static std::unordered_map<std::uint32_t, std::uint32_t> scaled;
        const std::uint32_t o = ctx.gpr[4];
        if (o >= 0x08800000u && o < 0x0a000000u) {
            auto &m = rt->memory();
            const std::uint32_t cur = m.load32(o + 0x474u);
            auto it = scaled.find(o);
            if (it == scaled.end() || it->second != cur) {
                const float f = std::bit_cast<float>(cur) * static_cast<float>(1.0 / g_hle_options.fps_mult);
                const std::uint32_t nb = std::bit_cast<std::uint32_t>(f);
                m.store32(o + 0x474u, nb);
                scaled[o] = nb;
            }
        }
    }
    if ((address == kSePlay || address == 0x089105a4u || address == 0x08913608u) && std::getenv("RR_AUDIOLOG") != nullptr) {
        // Who plays and who stops each sound (to find the code that loses the stuck drift voice):
        // SEPLAY  = library play 0x08913D4C (bank index a0, sound a1, handle t0, mode t1) with the handle's previous voice / play id,
        // SESTOP  = Stop(handle a0) 0x089105A4 (finds the voice by the handle's play id),
        // SEVSTOP = stop wrapper 0x08913608 (voice a0; ra 0x0891060C = Stop, 0x0891497C = group stop, 0x0891686C/74 = other).
        // chain = return addresses found on the stack (each preceded by a jal), innermost first.
        static int lines = 0;
        if (lines < 40000) {
            ++lines;
            auto &m = rt->memory();
            std::string chain;
            const std::uint32_t sp = ctx.gpr[29];
            int found = 0;
            for (std::uint32_t off = 0; off < 0x180u && found < 6 && sp >= 0x08800000u && sp + off + 4u < 0x0a000000u; off += 4u) {
                const std::uint32_t w = m.load32(sp + off);
                if (w >= 0x08804048u && w < 0x08972800u && (w & 3u) == 0u && (m.load32(w - 8u) >> 26) == 3u) {
                    chain += ' ' + hex32(w);
                    ++found;
                }
            }
            const std::uint32_t pool = m.load32(0x08a8316cu);
            const bool pool_ok = pool >= 0x08800000u && pool < 0x0a000000u;
            char b[260];
            if (address == 0x08913608u) {
                const std::uint32_t v = ctx.gpr[4];
                std::snprintf(b, sizeof b, "SEVSTOP step=%u voice=%u id=0x%X ra=%08X chain=", game_step(), v,
                              pool_ok && v < 32u ? m.load32(pool + 76u * v + 0x20u) : 0u, ctx.gpr[31]);
            } else {
                const std::uint32_t h = address == kSePlay ? ctx.gpr[8] : ctx.gpr[4];
                const bool h_ok = h >= 0x08800000u && h < 0x09fffff0u;
                const std::uint32_t hv = h_ok ? m.load32(h) : 0xffffffffu, hid = h_ok ? m.load32(h + 8u) : 0u;
                const std::uint32_t hst = pool_ok && hv < 32u ? m.load8(pool + 76u * hv) : 9u;
                const std::uint32_t hsid = pool_ok && hv < 32u ? m.load32(pool + 76u * hv + 0x20u) : 0u;
                if (address == kSePlay) hwatch::cur_play_handle = h_ok ? h : 0u;
                if (address == kSePlay)
                    std::snprintf(b, sizeof b, "SEPLAY step=%u bank=%u snd=%u handle=%08X mode=%u old_voice=%d old_id=0x%X slot_state=%u slot_id=0x%X ra=%08X chain=", game_step(),
                                  ctx.gpr[4] & 0xffffu, ctx.gpr[5] & 0xffffu, h, ctx.gpr[9], static_cast<int>(hv), hid, hst, hsid, ctx.gpr[31]);
                else
                    std::snprintf(b, sizeof b, "SESTOP step=%u handle=%08X voice=%d id=0x%X slot_state=%u slot_id=0x%X ra=%08X chain=", game_step(), h,
                                  static_cast<int>(hv), hid, hst, hsid, ctx.gpr[31]);
            }
            log_line(category::kHle, std::string(b) + chain);
        }
    }
    if ((address == 0x08911274u || address == 0x089112e8u) && std::getenv("RR_AUDIOLOG") != nullptr) {
        // Game-side requests to the voice pool: 0x08911274 = start voice a0 (sets its bit in the key-on mask), 0x089112E8 = stop voice a0
        // (sets its bit in the key-off mask).  Logs the step, the voice, the caller and the slot state, to see who forgets to stop a voice.
        static int lines = 0;
        if (lines < 30000) {
            ++lines;
            const std::uint32_t v = ctx.gpr[4];
            const std::uint32_t pool = audio_pool_obj;
            const std::uint32_t st = (pool != 0u && v < 32u) ? rt->memory().load8(pool + 76u * v) : 99u;
            if (address == 0x08911274u && hwatch::cur_play_handle != 0u && hwatch::cur_play_handle < 0x09000000u && pool != 0u && v < 32u) {
                const std::uint32_t hh = hwatch::cur_play_handle, nid = rt->memory().load32(pool + 76u * v + 0x20u);
                bool found = false;
                for (auto &w : hwatch::list) if (w.h == hh) { w.voice = rt->memory().load32(hh); w.id = rt->memory().load32(hh + 8u); found = true; }
                if (!found && hwatch::list.size() < 256u) hwatch::list.push_back({hh, rt->memory().load32(hh), rt->memory().load32(hh + 8u)});
                if (rt->memory().load32(hh + 8u) != nid)
                    log_line(category::kHle, "HCHANGE start-mismatch step=" + std::to_string(game_step()) + " handle=" + hex32(hh) + " handle_id=" + hex32(rt->memory().load32(hh + 8u)) + " slot_id=" + hex32(nid));
            }
            log_line(category::kHle, std::string("POOLREQ ") + (address == 0x08911274u ? "start" : "stop") + " step=" + std::to_string(game_step()) +
                                         " voice=" + std::to_string(v) + " state=" + std::to_string(st) + " id=" + hex32(pool != 0u && v < 32u ? rt->memory().load32(pool + 76u * v + 0x20u) : 0u) + " caller=" + hex32(ctx.gpr[31]));
        }
    }
    if ((address == 0x08910fecu || address == 0x08910f70u) && std::getenv("RR_AUDIOLOG") != nullptr) {
        // Voice pool of the game's sound library: 0x08910F70 turns the key-on mask (+3044) into key-on calls, 0x08910FEC walks the key-off
        // mask (+3048): a slot whose state byte is 2 gets its key-off, one in state 1 stays pending, any other state is silently dropped
        // (RELDROP) -- a dropped request leaves the voice playing for ever.  Logs every dropped request with the slot states.
        const std::uint32_t pool = ctx.gpr[4];
        if (pool >= 0x08800000u && pool < 0x0a000000u) {
            audio_pool_obj = pool;
            if (address == 0x08910fecu) {
                auto &m = rt->memory();
                static int lines = 0;
                const std::uint32_t mask = m.load32(pool + 3048u);
                for (std::uint32_t i = 0; i < 32u && lines < 3000; ++i) {
                    if ((mask >> i) & 1u) {
                        const std::uint32_t st = m.load8(pool + 76u * i);
                        if (st != 1u && st != 2u) {
                            ++lines;
                            log_line(category::kHle, "RELDROP step=" + std::to_string(game_step()) + " voice=" + std::to_string(i) + " state=" + std::to_string(st) +
                                                         " mask=" + hex32(mask) + " keyonmask=" + hex32(m.load32(pool + 3044u)));
                        }
                    }
                }
            }
        }
    }
    if ((address == 0x0891eeb0u || address == 0x0891eeecu || address == 0x0891ef28u) && std::getenv("RR_AUDIOLOG") != nullptr) {
        // Who calls the game's SAS wrappers (key-on 0x0891EEB0, key-off 0x0891EEEC, volume 0x0891EF28; a0 = voice): the guest return
        // address of the caller, to find the code that should key off a stuck drift / tire voice.  Volume is logged once per caller.
        static int lines = 0;
        static std::unordered_map<std::uint64_t, int> seen;
        const std::uint32_t voice = ctx.gpr[4], ra = ctx.gpr[31];
        const bool vol = address == 0x0891ef28u;
        if (lines < 20000 && (!vol || seen[(std::uint64_t(ra) << 8) | (voice & 0xffu)]++ == 0)) {
            ++lines;
            log_line(category::kHle, std::string("SASCALL ") + (vol ? "volume" : address == 0x0891eeb0u ? "keyon" : "keyoff") + " step=" + std::to_string(game_step()) +
                                         " voice=" + std::to_string(voice) + " caller=" + hex32(ra));
        }
    }
    if (address == 0x08845e48u && std::getenv("RR_REPLAYLOG") != nullptr) {
        auto &m = rt->memory();
        const std::uint32_t o = ctx.gpr[4];
        static std::map<std::uint32_t, std::uint32_t> last;
        if (o >= 0x08800000u && o < 0x0a000000u) {
            const std::uint32_t key = m.load8(o + 4u) | (m.load8(o + 5u) << 8u);
            auto it = last.find(o);
            if (it == last.end() || it->second != key) {
                last[o] = key;
                log_line(category::kHle, "CTRL obj=" + hex32(o) + " src=" + std::to_string(key & 0xffu) + " mode=" + std::to_string(key >> 8u) + " step=" + std::to_string(game_step()) +
                                             " frame=" + std::to_string(g_frames_presented) + " ra=" + hex32(ctx.gpr[31]));
            }
            if (g_ctrl_hi == 0u && rt->memory().write_watch_active()) {
                g_ctrl_lo = o; g_ctrl_hi = o + 0x68u;
                psprecomp::set_watch_sink(&hle_watch_sink);
                psprecomp::set_write_watch_range(o, 0x68u);
                log_line(category::kHle, "CTRLW watching " + hex32(o) + "..+0x68");
            }
        }
        static std::uint32_t last_dump = 0;
        if (g_ctrl_hi != 0u && game_step() >= last_dump + 600u) {
            last_dump = game_step();
            std::string l = "CTRLW step=" + std::to_string(game_step()) + " frame=" + std::to_string(g_frames_presented) + ":";
            for (const auto &[k, n] : g_ctrl_writers) {
                char b[64];
                std::snprintf(b, sizeof b, " [pc=%08X ra=%08X +%X x%u]", std::get<0>(k), std::get<1>(k), std::get<2>(k), n);
                l += b;
            }
            log_line(category::kHle, l);
            g_ctrl_writers.clear();
        }
    }
    static const bool id_fix = std::getenv("RR_SNDID_FIX_OFF") == nullptr;
    if (id_fix && (address == kSePlay || address == 0x08911274u || address == 0x089105a4u)) {
        // Sound handles lose their play id: a handle (+0 voice, +8 play id) is played, the voice gets the id in pool slot +0x20, and a
        // moment later the handle's +8 holds the id of some other sound (seen 3 times: drift handles 08A4D844 / 08A51B34 got the id of an
        // engine-layer voice).  Stop(handle) then stops that other voice and the drift loops for ever, filling the 29 effect voices.
        // Remember the id each handle got when its voice started; when Stop is called on a handle that still names that voice, the
        // voice still plays that id, but the handle's id was changed, put the right id back so Stop finds its own voice.
        struct Rec { std::uint32_t voice, id; };
        static std::unordered_map<std::uint32_t, Rec> rec;
        static std::uint32_t playing_handle = 0;
        static int fix_lines = 0;
        auto &m = rt->memory();
        const std::uint32_t pool = m.load32(0x08a8316cu);
        if (address == kSePlay) {
            const std::uint32_t h = ctx.gpr[8];
            playing_handle = (h >= 0x08800000u && h < 0x09000000u) ? h : 0u;   // stack handles (0x09Fxxxxx) are temporary
        } else if (address == 0x08911274u) {
            const std::uint32_t v = ctx.gpr[4];
            if (playing_handle != 0u && v < 32u && pool >= 0x08800000u && pool < 0x0a000000u) {
                if (rec.size() > 4096u) rec.clear();
                rec[playing_handle] = {v, m.load32(pool + 76u * v + 0x20u)};
            }
            playing_handle = 0u;
        } else {
            const std::uint32_t h = ctx.gpr[4];
            const auto it = rec.find(h);
            if (it != rec.end() && pool >= 0x08800000u && pool < 0x0a000000u) {
                const std::uint32_t hv = m.load32(h), hid = m.load32(h + 8u);
                if (hv == it->second.voice && hv < 32u && hid != it->second.id) {
                    const std::uint32_t slot = pool + 76u * hv, st = m.load8(slot);
                    if ((st == 1u || st == 2u) && m.load32(slot + 0x20u) == it->second.id) {
                        m.store32(h + 8u, it->second.id);
                        if (fix_lines < 2000) {
                            ++fix_lines;
                            char b[200];
                            std::snprintf(b, sizeof b, "SNDIDFIX step=%u handle=%08X voice=%u id 0x%X restored to 0x%X ra=%08X", game_step(), h, hv, hid, it->second.id, ctx.gpr[31]);
                            log_line(category::kHle, b);
                        }
                    }
                }
            }
        }
    }
    static const bool orphan_fix = std::getenv("RR_ORPHAN_FIX_OFF") == nullptr;
    if ((address == kSePlay || address == kPadUpdate) && orphan_fix) {
        // Orphaned sound voices (looping tire screech / car sounds that never stop, then missing effects once the 29 voices run out).
        // The sound library's play function 0x08913D4C takes a sound handle in t0: handle +0 = voice, +4 = sound id, +6 = bank,
        // +8 = unique play id, which the voice pool also keeps in slot +0x20 (slot = [0x08A8316C] + 76 * voice, byte 0 = state,
        // 1 starting / 2 playing).  Stop(handle) finds the voice by that id.  When a handle is played again while its previous voice
        // is still playing, the handle gets the new id and the old voice can never be stopped (seen in a RAM dump: drift voice 12
        // id 0x16E with no handle left, its handle now on voice 18 id 0x175, 7 steps later).  Remember such voices here and, if they
        // are still playing the same id a moment later (one-shot effects stop themselves well before that), key them off through the
        // pool's own key-off mask (+3048), exactly as Stop does.
        struct Orphan { std::uint32_t voice, id; std::chrono::steady_clock::time_point t; };
        static std::vector<Orphan> orphans;
        static int orphan_lines = 0;
        auto &m = rt->memory();
        const std::uint32_t pool = m.load32(0x08a8316cu);
        if (pool >= 0x08800000u && pool < 0x0a000000u) {
            const auto now = std::chrono::steady_clock::now();
            if (address == kSePlay) {
                const std::uint32_t h = ctx.gpr[8];
                if (ctx.gpr[9] != 1u && h >= 0x08800000u && h < 0x09fffff0u) {   // mode 1 keeps the handle's id (not a replacement)
                    const std::uint32_t v = m.load32(h), id = m.load32(h + 8u);
                    if (v < 29u && id != 0u) {
                        const std::uint32_t s = pool + 76u * v, st = m.load8(s);
                        if ((st == 1u || st == 2u) && m.load32(s + 0x20u) == id) {
                            bool known = false;
                            for (const Orphan &o : orphans) known |= o.voice == v && o.id == id;
                            if (!known) orphans.push_back({v, id, now});
                            if (orphan_lines < 2000) {
                                ++orphan_lines;
                                char b[220];
                                std::snprintf(b, sizeof b, "ORPHAN replay step=%u voice=%u id=0x%X state=%u handle=%08X old=%u/%u new=%u/%u ra=%08X", game_step(), v, id, st, h,
                                              m.load16(h + 6u), m.load16(h + 4u), ctx.gpr[4] & 0xffffu, ctx.gpr[5] & 0xffffu, ctx.gpr[31]);
                                log_line(category::kHle, b);
                            }
                        }
                    }
                }
            } else {
                for (std::size_t i = 0; i < orphans.size();) {
                    const Orphan o = orphans[i];
                    const std::uint32_t s = pool + 76u * o.voice, st = m.load8(s);
                    const bool alive = (st == 1u || st == 2u) && m.load32(s + 0x20u) == o.id;
                    const bool looping = psprecomp::sas_hle_state().voices[o.voice].loop;
                    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - o.t).count();
                    if (alive && looping && age < 750) { ++i; continue; }
                    if (alive && looping) m.store32(pool + 3048u, m.load32(pool + 3048u) | (1u << o.voice));
                    if (orphan_lines < 2000) {
                        ++orphan_lines;
                        log_line(category::kHle, std::string("ORPHAN ") + (alive && looping ? "stop" : alive ? "oneshot" : "ended") + " step=" + std::to_string(game_step()) +
                                                     " voice=" + std::to_string(o.voice) + " id=0x" + [&] { char x[12]; std::snprintf(x, sizeof x, "%X", o.id); return std::string(x); }() +
                                                     " age_ms=" + std::to_string(age));
                    }
                    orphans.erase(orphans.begin() + static_cast<std::ptrdiff_t>(i));
                }
            }
        }
    }
    if ((address == 0x088d8eb8u || address == 0x088d8d94u || address == 0x088d8af4u || address == 0x088d89bcu || address == 0x088d8738u ||
         address == 0x088d83d8u) && g_hle_options.fps_mult > 1.0 && fast_rate_active() && std::getenv("RR_EVENTWORK_GATE_OFF") == nullptr &&
        !race_tick(false)) {
        // Trackside event objects (Map::Event::C...Work, update = vtable slot 0, called every step): CUshiWork 0x088D8EB8 (cows:
        // head angle +0x14 moves 0x400 per call), CHatoWork 0x088D8D94 (pigeons), CLeafWork 0x088D8AF4, CHanabiWork 0x088D89BC
        // (fireworks), CFlashWork 0x088D8738, CCycleWork 0x088D83D8.  They step fixed amounts per call, so at 144 fps they ran 2.4x
        // too fast; run them on the 60 Hz ticks only.  (The helicopter, CHeliWork, is scaled instead; CMotWork is the animation
        // player 0x088D9124, scaled above.)
        return true;
    }
    if (g_hle_options.fps_mult > 1.0 && is_hud_update(address) && fast_rate_active() && std::getenv("RR_HUD_GATE_OFF") == nullptr && !menu_tick(game_step())) {
        // Race HUD widgets (Fj::C2D..., update = vtable slot 7, called every step, return 0): their animations (the gap window's
        // racers passing each other, flags, lap / position pop-ups, meters) step fixed amounts per call, 2.4x too fast at 144 fps.
        // Run them on the 60 Hz ticks like the PSP; they are drawn every frame by their own draw methods.
        static bool logged = false;
        if (!logged) { logged = true; log_line(category::kHle, "HUDGATE race HUD updates run on the 60 Hz ticks (first: " + hex32(address) + ")"); }
        ctx.gpr[2] = 0u;
        return true;
    }
    if (address == kInfoWinUpdate && std::getenv("RR_INFOWIN_LOG") != nullptr) {
        // RR_INFOWIN_LOG=1: radar / info window state per update call (state +0x94, last clock/50 +0x98, shown time +0x9c, +0xb4)
        static int lines = 0;
        auto &m = rt->memory();
        const std::uint32_t o = ctx.gpr[4];
        if (lines == 0) {
            const std::uint32_t lui = m.load32(0x08909254u), lw = m.load32(0x08909258u);
            const std::uint32_t gp = ((lui & 0xffffu) << 16) + static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(lw & 0xffffu)));
            const std::uint32_t g = m.load32(gp);
            log_line(category::kHle, "INFOWINLOG lui=" + hex32(lui) + " lw=" + hex32(lw) + " gptr=" + hex32(gp) + " g=" + hex32(g) + " clock=" + std::to_string(m.load32(g + 0x30u)));
        }
        if (lines < 3000 && m.load32(o + 0x94u) != 0u) {
            ++lines;
            log_line(category::kHle, "INFOWINLOG step=" + std::to_string(game_step()) + " frame=" + std::to_string(g_frames_presented) + " st94=" + std::to_string(m.load32(o + 0x94u)) +
                                         " c98=" + std::to_string(static_cast<std::int32_t>(m.load32(o + 0x98u))) + " t9c=" + std::to_string(static_cast<std::int32_t>(m.load32(o + 0x9cu))) +
                                         " b4=" + std::to_string(m.load32(o + 0xb4u)) + " a8=" + std::to_string(static_cast<std::int32_t>(m.load32(o + 0xa8u))) + " ac=" + std::to_string(static_cast<std::int32_t>(m.load32(o + 0xacu))));
        }
    }
    if (address == kInfoWinUpdate && std::getenv("RR_WIDGETDIFF") != nullptr) {
        // RR_WIDGETDIFF=1: per-step change counts of the gap window object's words (and of the objects its words point to), to
        // find state that still advances per frame instead of per 60 Hz tick.  Dumped every 7200 steps.
        static std::uint32_t obj = 0, steps = 0, last_step = ~0u;
        static std::vector<std::uint32_t> prev, cnt;
        static std::vector<std::uint32_t> bases;
        auto &m = rt->memory();
        const std::uint32_t st = game_step();
        if (st != last_step) {
            last_step = st;
            if (ctx.gpr[4] != obj) {
                obj = ctx.gpr[4]; steps = 0; bases.clear(); bases.push_back(obj);
                for (std::uint32_t o = 0; o < 0x200u; o += 4u) {
                    const std::uint32_t v = m.load32(obj + o);
                    if (v >= 0x09000000u && v < 0x0a000000u && (v & 3u) == 0u && v != obj && bases.size() < 24u &&
                        std::find(bases.begin(), bases.end(), v) == bases.end()) bases.push_back(v);
                }
                prev.assign(bases.size() * 128u, 0u); cnt.assign(bases.size() * 128u, 0u);
                for (std::size_t b = 0; b < bases.size(); ++b) for (std::uint32_t w = 0; w < 128u; ++w) prev[b * 128u + w] = m.load32(bases[b] + 4u * w);
            } else {
                ++steps;
                for (std::size_t b = 0; b < bases.size(); ++b) for (std::uint32_t w = 0; w < 128u; ++w) {
                    const std::uint32_t v = m.load32(bases[b] + 4u * w);
                    if (v != prev[b * 128u + w]) {
                        ++cnt[b * 128u + w];
                        if (std::getenv("RR_WIDGETDIFF_VAL") != nullptr && cnt[b * 128u + w] <= 6u)
                            log_line(category::kHle, "WIDGETVAL " + std::to_string(b) + "+" + hex32(4u * w).substr(6) + " step=" + std::to_string(st) + " frame=" + std::to_string(g_frames_presented) +
                                                         " " + hex32(prev[b * 128u + w]) + "->" + hex32(v) + " f=" + std::to_string(std::bit_cast<float>(v)));
                        prev[b * 128u + w] = v;
                    }
                }
                static const std::uint32_t every = [] { const char *e = std::getenv("RR_WIDGETDIFF_N"); return e ? static_cast<std::uint32_t>(std::atoi(e)) : 7200u; }();
                if (steps % every == 0u) {
                    std::string l = "WIDGETDIFF obj=" + hex32(obj) + " steps=" + std::to_string(steps) + " fps_mult=" + std::to_string(g_hle_options.fps_mult) + ":";
                    for (std::size_t b = 0; b < bases.size(); ++b) for (std::uint32_t w = 0; w < 128u; ++w)
                        if (cnt[b * 128u + w] != 0u) l += " " + std::to_string(b) + "+" + hex32(4u * w).substr(6) + "=" + std::to_string(cnt[b * 128u + w]);
                    log_line(category::kHle, l);
                    std::string bl = "WIDGETDIFF bases:";
                    for (std::size_t b = 0; b < bases.size(); ++b) bl += " " + std::to_string(b) + "=" + hex32(bases[b]) + "/vt" + hex32(m.load32(bases[b]));
                    log_line(category::kHle, bl);
                }
            }
        }
    }
    if (address == kInfoWinUpdate && g_hle_options.fps_mult > 1.0 && fast_rate_active() && std::getenv("RR_INFOWIN_FIX_OFF") == nullptr &&
        !menu_tick(game_step())) {
        // Gap window bottom left (Fj::C2DInfoWin, update 0x0890922C, returns 0): its passing animation (8-bit racers moving and
        // scrolling, state machine +0x94, sprite positions +0xD0.., scroll offset +0x90) advances a fixed amount per call, 2.4x too
        // fast at 144 fps.  Run the update on the 60 Hz ticks only (it is drawn every frame by a separate method).
        static std::uint32_t skipped = 0;
        if ((++skipped % 2000u) == 1u) log_line(category::kHle, "INFOWIN update 0x0890922C skipped " + std::to_string(skipped) + " times (obj " + hex32(ctx.gpr[4]) + ")");
        ctx.gpr[2] = 0u;
        return true;
    }
    if (address == kCtrlUpdate && fast_rate_active() && std::getenv("RR_PAD_REPEAT_FIX_OFF") == nullptr) {
        // Controller object update 0x08845E48 (a0 = controller, e.g. the player's at 0x089DC910; vtable +0xC, called once per step
        // from the main loop right after the pad update).  It keeps its own auto-repeat: one countdown byte per input bit at
        // +0x48 + i, decremented each call while the input is held; at zero it raises the bit in the repeat word +0x14 for that
        // call and reloads from +0x61 (re-set to the first delay +0x60 while released).  Menus that read +0x14 (the music
        // selector's up / down) repeated 2.4x too fast at 144 fps.  Same cure as the pad object: on the steps between 60 Hz
        // ticks give every running countdown back the one the update is about to take.
        const std::uint32_t o = ctx.gpr[4];
        if (o >= 0x08800000u && o < 0x0a000000u && !menu_tick(game_step() + pad_delta)) {
            auto &m = rt->memory();
            for (std::uint32_t i = 0; i < 24u; ++i) {
                const std::uint32_t c = m.load8(o + 0x48u + i);
                if (c != 0u && c < 0xffu) m.store8(o + 0x48u + i, static_cast<std::uint8_t>(c + 1u));
            }
        }
    }
    if (address == kPadUpdate && fast_rate_active() && std::getenv("RR_PAD_REPEAT_FIX_OFF") == nullptr) {
        // Pad object update (0x0889d3fc, void, once per step; a0 = pad object): word 0 = held buttons, +0x18 + 4 * i = hold counter of
        // button i (a held button counts down one per call; at zero it raises a repeat pulse for one call and reloads from +0x14).
        // The pulse words (+8 / +0xC) must stay one call wide because the race and menu code reads them every step (running the whole
        // update only on ticks made Start pause and un-pause at once), but the counters ran 2.4x too fast at 144 fps, so a normal tap
        // repeated.  On the steps between 60 Hz ticks give every held button's counter back the one the update is about to take.
        const std::uint32_t o = ctx.gpr[4];
        if (o >= 0x08800000u && o < 0x0a000000u && !menu_tick(game_step() + pad_delta)) {
            auto &m = rt->memory();
            const std::uint32_t held = m.load32(o);
            static const std::uint32_t masks[14] = {0x10u, 0x40u, 0x80u, 0x20u, 0x2000u, 0x4000u, 0x1000u, 0x8000u, 0x100u, 0x200u, 0x1u, 0x8u, 0x10000u, 0x20000u};
            for (std::uint32_t i = 0; i < 14u; ++i)
                if ((held & masks[i]) != 0u) m.store32(o + 0x18u + 4u * i, m.load32(o + 0x18u + 4u * i) + 1u);
        }
    }
    if (address == kMusicSelectUpdate && std::getenv("RR_SELLOG") != nullptr) {
        // Diagnostic: every call of the music selector update (a0 = selector object): game step, presented frame, whether the
        // 60 Hz gate lets it run, and the object words that changed since the previous call (offset=value).
        static std::unordered_map<std::uint32_t, std::array<std::uint32_t, 64>> prev;
        static int lines = 0;
        const std::uint32_t o = ctx.gpr[4];
        if (o >= 0x08800000u && o < 0x0a000000u && lines < 3000) {
            auto &m = rt->memory();
            auto &pv = prev[o];
            std::string chg;
            for (std::uint32_t k = 0; k < 64u; ++k) {
                const std::uint32_t v = m.load32(o + 4u * k);
                if (v != pv[k]) { char b[40]; std::snprintf(b, sizeof b, " +%x=%x", 4u * k, v); chg += b; pv[k] = v; }
            }
            const std::uint32_t st = game_step();
            const bool gated = std::find(skip.begin(), skip.end(), address) != skip.end();
            static std::uint32_t last_pad = 0xffffffffu;
            const std::uint32_t padw = m.load32(0x08A49198u);
            const bool pad_changed = padw != last_pad;
            last_pad = padw;
            if (!chg.empty() || pad_changed || (st % 30u) == 0u) {
                log_line(category::kHle, "SELLOG pad=" + hex32(padw) + " step=" + std::to_string(st) + " frame=" + std::to_string(g_frames_presented) + " obj=" + hex32(o) +
                                             " runs=" + ((!gated || tick_at(st)) ? "1" : "0") + " changed:" + chg);
                ++lines;
            }
        }
    }
    if (address == kSlotUpdate && std::getenv("RR_SLOT_GUARD_OFF") == nullptr) {
        // Slot update 0x08918300 (a0 = HUD sprite table).  It loops over the 32 slots (stride 0x50) and, for every slot whose
        // active word [slot + 0x140] is non-zero, reads [[slot + 0x17C] + 0x10].  A slot that is flagged active but whose object
        // pointer is gone (seen on slots 8 and 10 after a long race) faulted there (guest_pc 0x089184C8 / 0x08918788, read at
        // 0x10).  Deactivate such slots before the loop runs.
        const std::uint32_t tbl = ctx.gpr[4];
        if (tbl >= 0x08800000u && tbl < 0x09ffe000u) {
            auto &m = rt->memory();
            for (std::uint32_t i = 0; i < 32u; ++i) {
                const std::uint32_t s0 = tbl + i * 0x50u;
                if (m.load32(s0 + 0x140u) == 0u) continue;
                const std::uint32_t obj = m.load32(s0 + 0x17Cu);
                if (obj >= 0x08800000u && obj < 0x0a000000u) continue;
                m.store32(s0 + 0x140u, 0u);
                static int n = 0;
                if (n++ < 8) log_line(category::kHle, "slot guard: slot " + std::to_string(i) + " active but no object, deactivated (table " + hex32(tbl) + ")");
            }
        }
    }
    if (address == kEffectStep && std::getenv("RR_SLOT_GUARD_OFF") == nullptr) {
        // Effect step 0x08918048 (t1 = effect, [t1] = sprite slot).  When the slot's object is gone, the setter guard above skips
        // only its own call and the next call of the step still dereferences the null object (fault at +0x10).  Skip the whole step.
        auto &m = rt->memory();
        const std::uint32_t eff = ctx.gpr[9];
        if (eff >= 0x08800000u && eff < 0x0a000000u) {
            const std::uint32_t slot = m.load32(eff);
            const std::uint32_t t = m.load32(0x088CDC00u);
            if (slot < 0x20u && t >= 0x08800000u && t < 0x0a000000u) {
                const std::uint32_t obj = m.load32(t + slot * 0x50u + 0x180u);
                if (obj < 0x08800000u || obj >= 0x0a000000u) {
                    static int n = 0;
                    if (n++ < 5) log_line(category::kHle, "slot guard: effect step skipped (slot " + std::to_string(slot) + " null object) ra=" + hex32(ctx.gpr[31]));
                    ctx.gpr[2] = 0u;
                    return true;
                }
            }
        }
    }
    {
        // World clock: [0x08a43aec] += 1 and the float [0x08a43af0] += 1.0 once per call of the scene setup 0x088ee8b4 (billboards flip
        // their pictures off it).  With that function running every step (mirror fix) the clock would run mult times too fast, so on the
        // steps between the 60 Hz ticks the increment is taken back as soon as the next hooked function runs (after the prologue).
        auto &m = rt->memory();
        flush_clock();
        if (address == 0x088ee8b4u && g_hle_options.fps_mult > 1.0 && fast_rate_active() && std::getenv("RR_ANIM_SCALE_OFF") == nullptr &&
            scene_every_step() && !tick_at(game_step())) {
            clock_pending = true; clock_c0 = m.load32(0x08a43aecu); clock_f0 = m.load32(0x08a43af0u);
        }
    }
    if (address == kSoundManager && sndlog_enabled()) sound_requests(ctx.gpr[4]);
    if (address == kMailTake && std::getenv("RR_MAILLOG") != nullptr) {
        // 0x08818a28 returns and clears the pending event of the game's event queue: [[[lui/lw slot] + 0x28] + 0x28].
        auto &mm = rt->memory();
        const std::uint32_t lui = mm.load32(kMailTake), lw = mm.load32(kMailTake + 4u);
        const std::uint32_t slot = ((lui & 0xFFFFu) << 16u) + static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(lw & 0xFFFFu)));
        const std::uint32_t box = mm.load32(slot);
        const std::uint32_t q = (box >= 0x08800000u && box < 0x0a000000u) ? mm.load32(box + 0x28u) : 0u;
        const std::uint32_t v = (q >= 0x08800000u && q < 0x0a000000u) ? mm.load32(q + 0x28u) : 0u;
        if (q >= 0x08800000u && q < 0x0a000000u && ((q + 0x28u) & 0x1FFFFFFFu) != g_mail_watch_addr && mm.write_watch_active()) {
            g_mail_watch_addr = (q + 0x28u) & 0x1FFFFFFFu;
            psprecomp::set_watch_sink(&hle_watch_sink);
            psprecomp::set_write_watch_range(q + 0x28u, 4u);
            log_line(category::kHle, "MAILWATCH armed on " + hex32(q + 0x28u) + " step=" + std::to_string(game_step()));
        }
        if (q >= 0x08800000u && q < 0x0a000000u && game_step() % 10u == 0u) {
            static std::uint32_t qlines = 0;
            if (qlines++ < 3000u)
                log_line(category::kHle, "MAILQ step=" + std::to_string(game_step()) + " frame=" + std::to_string(g_frames_presented) +
                                             " q=" + hex32(q) + " +10=" + std::to_string(mm.load32(q + 0x10u)) + " +18=" + std::to_string(mm.load32(q + 0x18u)) +
                                             " +24=" + std::to_string(mm.load32(q + 0x24u)) + " +2c=" + std::to_string(mm.load32(q + 0x2cu)) +
                                             " +4c=" + std::to_string(mm.load32(q + 0x4cu)) + " +58=" + std::to_string(mm.load32(q + 0x58u)) +
                                             " +64=" + std::to_string(mm.load32(q + 0x64u)) + " +68=" + std::to_string(mm.load32(q + 0x68u)));
        }
        static std::uint32_t lines = 0;
        if (v != 0u && lines++ < 2000u)
            log_line(category::kHle, "MAILTAKE step=" + std::to_string(game_step()) + " frame=" + std::to_string(g_frames_presented) +
                                         " ra=" + hex32(ctx.gpr[31]) + " event=" + std::to_string(v));
    }
    if (!calllog_addrs().empty() && std::find(calllog_addrs().begin(), calllog_addrs().end(), address) != calllog_addrs().end()) {
        static std::unordered_map<std::uint32_t, std::uint32_t> counts;
        if (counts[address]++ < 400u)
            log_line(category::kHle, "CALL " + hex32(address) + " step=" + std::to_string(game_step()) + " frame=" + std::to_string(g_frames_presented) +
                                         " ra=" + hex32(ctx.gpr[31]) + " a0=" + hex32(ctx.gpr[4]) + " a1=" + hex32(ctx.gpr[5]) +
                                         " a2=" + hex32(ctx.gpr[6]) + " a3=" + hex32(ctx.gpr[7]) + " t0=" + hex32(ctx.gpr[8]) + " t1=" + hex32(ctx.gpr[9]) +
                                         [&] {   // the player's car height at entry (car = [[0x08a448f0]+0xc4])
                                             auto &m = rt->memory();
                                             const std::uint32_t hud = m.load32(0x08a448f0u);
                                             std::uint32_t car = (hud >= 0x08800000u && hud < 0x0a000000u) ? m.load32(hud + 0xc4u) : 0u;
                                             if (const char *e = std::getenv("RR_CALLLOG_CAR")) car = static_cast<std::uint32_t>(std::strtoul(e, nullptr, 16));
                                             if (car < 0x08800000u || car >= 0x0a000000u) return std::string();
                                             char b[80]; std::snprintf(b, sizeof b, " y=%.4f prev=%.4f", static_cast<double>(std::bit_cast<float>(m.load32(car + 0x16a4u))), static_cast<double>(std::bit_cast<float>(m.load32(car + 0x16c4u))));
                                             return std::string(b);
                                         }());
    }
    if (address == kSoundStart && sndlog_enabled()) {
        static std::uint32_t started = 0;
        if (started++ < 20000u)
            log_line(category::kHle, "SNDSTART step=" + std::to_string(game_step()) + " frame=" + std::to_string(g_frames_presented) + " id=" + std::to_string(ctx.gpr[5]));
    }
    if (address == kRaceHandler && last_car_step != 0xffffffffu && tsync_comp_until != 0u && std::getenv("RR_TICKSYNC_OFF") == nullptr) {
        // Pause: steps without a car update must not advance the 60 Hz pattern (the replay has no pause).  The race handler starts the
        // race step, before every gated system and the recorder: move the origin by each step since the last car update that is not
        // accounted for yet -- one per step if the handler also runs on the pause screen, or the whole gap when it resumes.
        const std::uint32_t st = game_step();
        const std::uint32_t target = st - 1u;
        const std::uint32_t from = std::max(last_car_step, tsync_comp_until);
        if (target > from && target - from < 100000u) {
            tick_origin += target - from;
            static int lines = 0;
            if (lines++ < 200) log_line(category::kHle, "TICKSYNC " + std::to_string(target - from) + " step(s) without car simulation before step " + std::to_string(st) + ": origin -> " + std::to_string(tick_origin));
        }
        tsync_comp_until = std::max(tsync_comp_until, target);
    }
    if ((address == kTrackShift || address == kCarIntegrate || address == kCarTrackSpeed || address == kCarTrackEvents) &&
        ((g_hle_options.fps_mult > 1.0 && !g_fps_patches.empty()) || std::getenv("RR_ROADH_TEST") != nullptr) && std::getenv("RR_ROADH_FIX_OFF") == nullptr) {
        // Previous-step track values car+0x8d0 (course distance, from +0x860) and car+0x8d4 (road height, from +0x85c): the track
        // tracker update 0x088ab8f0 (a0 = car + 0x850) copies +0x860/+0x85c into them once per step, and the car code then uses the
        // per-step changes: 0x088bb3b4 feeds the change of the road-height delta (+0x8d4 - +0x85c) into the vertical velocity,
        // 0x088bae44 makes the along-track speed (+0x860 - +0x8d0) / dt, 0x088bc9e8 checks the track sections crossed.  The race
        // mode's 60 Hz update (0x08843b40) rewrites both words with the values it saw on ITS previous run: at 60 fps that is the
        // previous step (no change), at 144 fps two or three steps back, so the deltas jumped between one-step and multi-step
        // sizes every other step: the body was kicked up and down (lifted ~8 cm, wheels bottoming out every other step, ~30 % harder
        // launch) and the track speed jittered.  Keep the values the shift wrote this step.
        struct H { std::uint32_t step; std::uint32_t d, h; };
        static std::unordered_map<std::uint32_t, H> shifted;
        static std::uint32_t fixes = 0;
        auto &m = rt->memory();
        const std::uint32_t st = game_step();
        if (address == kTrackShift) {
            const std::uint32_t car = ctx.gpr[4] - 0x850u;
            if (car >= 0x08800000u && car < 0x0a000000u) shifted[car] = H{st, m.load32(car + 0x860u), m.load32(car + 0x85cu)};
        } else {
            const std::uint32_t car = ctx.gpr[4];
            auto it = shifted.find(car);
            if (it != shifted.end() && st - it->second.step <= 1u) {
                const std::uint32_t d = m.load32(car + 0x8d0u), h = m.load32(car + 0x8d4u);
                if (d != it->second.d || h != it->second.h) {
                    m.store32(car + 0x8d0u, it->second.d);
                    m.store32(car + 0x8d4u, it->second.h);
                    if (fixes < 40u && std::getenv("RR_ROADH_LOG") != nullptr) {
                        char b[200]; std::snprintf(b, sizeof b, "ROADH fn=%08X car=%08X step=%u 8d0 %.3f -> %.3f  8d4 %.3f -> %.3f", address, car, st,
                            std::bit_cast<float>(d), std::bit_cast<float>(it->second.d), std::bit_cast<float>(h), std::bit_cast<float>(it->second.h));
                        log_line(category::kHle, b);
                    }
                    ++fixes;
                }
            }
        }
    }
    if (address == kGearbox && g_hle_options.fps_mult > 1.0 && fast_rate_active() && !g_fps_patches.empty() &&
        std::getenv("RR_SHIFT_FIX_OFF") == nullptr && !race_tick(false)) {
        // Gearbox 0x088bf840 (a0 = car + 0x1e88, once per step): a gear change sets the shift state +8 (1 up / 2 down / 3) and a
        // countdown +0xc = 12; each call takes one off and the drive comes back (+0x14 = 1.0) when it runs out.  The PSP's shifts
        // last 12 frames = 0.2 s; at 144 fps 12 steps = 0.08 s, so the car lost much less time per shift and accelerated about
        // 30 % harder in a straight line.  On the steps between 60 Hz ticks give the countdown back the one this call takes
        // (only on the path that decrements it: state +8 non-zero and +0x84 != 2).
        auto &m = rt->memory();
        const std::uint32_t o = ctx.gpr[4];
        if (o >= 0x08800000u && o < 0x0a000000u && m.load32(o + 8u) != 0u && m.load32(o + 0x84u) != 2u) {
            const std::int32_t c = static_cast<std::int32_t>(m.load32(o + 0xcu));
            if (c > 0 && c < 64) m.store32(o + 0xcu, static_cast<std::uint32_t>(c + 1));
        }
    }
    if (address == kTorqueRamp && g_hle_options.fps_mult > 1.0 && fast_rate_active() && !g_fps_patches.empty() && std::getenv("RR_DRIVE_FILTER_FIX") != nullptr) {
        // EXPERIMENT (RR_DRIVE_FILTER_FIX=1): the per-step spring filter on +0x90/+0x94 (x += v, v = 0.7 * (v + 0.24 * (target - x)))
        // only advances on the 60 Hz ticks: a non-tick step's update is undone at the next call.
        struct F { std::uint32_t step; bool undo; std::uint32_t x, v; };
        static std::unordered_map<std::uint32_t, F> f;
        auto &m = rt->memory();
        const std::uint32_t o = ctx.gpr[4], st = game_step();
        if (o >= 0x08800000u && o < 0x0a000000u) {
            auto it = f.find(o);
            if (it != f.end() && it->second.undo && it->second.step + 1u == st) { m.store32(o + 0x90u, it->second.x); m.store32(o + 0x94u, it->second.v); }
            f[o] = F{st, !race_tick(false), m.load32(o + 0x90u), m.load32(o + 0x94u)};
        }
    }
    if (address == kTorqueRamp && g_hle_options.fps_mult > 1.0 && !g_fps_patches.empty() && std::getenv("RR_TORQUE_RAMP_FIX_OFF") == nullptr) {
        // Drive engagement 0x088c0240 (a0 = car + 0x1e88, a1 = gear-ish count): while engaged ([a0+8] == 0, a1 > 0) the word +0x9c
        // (the engine torque scale read by the torque curve 0x088bfc1c as car + 0x1f24) grows by 1.0 per STEP up to 7.0, so the drive
        // torque came on in 7 frames = 0.12 s on the PSP but in 0.05 s at 144 fps (with the pedal ramp, the harder launch and body
        // lift at the start).  The 1.0 is the pool constant 1.0 shared with other math, so instead the word is pre-set so that the
        // function's +1.0 lands on old + 1/mult.
        auto &m = rt->memory();
        const std::uint32_t o = ctx.gpr[4];
        if (o >= 0x08800000u && o < 0x0a000000u && m.load32(o + 8u) == 0u && static_cast<std::int32_t>(ctx.gpr[5]) > 0) {
            const float cur = std::bit_cast<float>(m.load32(o + 0x9cu));
            if (cur < 7.0f) {
                const float target = std::min(7.0f, cur + static_cast<float>(1.0 / g_hle_options.fps_mult));
                m.store32(o + 0x9cu, std::bit_cast<std::uint32_t>(target - 1.0f));
            }
        }
    }
    if ((address == kPresim || address == kPresimEnd) && !g_fps_patches.empty() && std::getenv("RR_PRESIM_FIX_OFF") == nullptr) {
        // Race-start settle 0x088C82AC (called once while the race loads, with the car lists a3 [a2 cars] and t1 [t0 cars], stride
        // 0x27a0): 20 passes of the per-car simulation 0x088BDD28 drop every car onto the road before the track intro, and the intro
        // then shows them frozen where the last pass left them.  With the fps patch those 20 passes covered 1/mult of the time (20
        // steps of 1/144 s instead of 1/60 s), so the cars were still sinking / bouncing: at 144 fps the player's car sat 2.3 cm too
        // low during the intro and popped up and settled again when the race started (Greenpeak start on a slope).  Run the
        // settle with the original 60 Hz constants and car time steps, then put the patched values back (at its final call
        // 0x088C72F0, which runs right after the passes).
        static bool active = false;
        static std::vector<std::pair<std::uint32_t, std::array<std::uint32_t, 3>>> saved;
        auto &m = rt->memory();
        if (address == kPresim && !active) {
            active = true; saved.clear();
            for (const auto &p : g_fps_patches) m.store32(p[0], p[1]);
            const double mult = g_hle_options.fps_mult;
            const auto fix_list = [&](std::uint32_t base, std::uint32_t count) {
                for (std::uint32_t i = 0; i < count && i < 64u; ++i) {
                    const std::uint32_t c = base + i * 0x27a0u;
                    if (c < 0x08800000u || c + 0x27a0u >= 0x0a000000u) break;
                    saved.push_back({c, {m.load32(c + 0x26d0u), m.load32(c + 0x26d4u), m.load32(c + 0x26d8u)}});
                    const auto ff = [&](std::uint32_t o) { return std::bit_cast<float>(m.load32(c + o)); };
                    m.store32(c + 0x26d0u, std::bit_cast<std::uint32_t>(static_cast<float>(ff(0x26d0u) * mult)));
                    m.store32(c + 0x26d4u, std::bit_cast<std::uint32_t>(static_cast<float>(ff(0x26d4u) / mult)));
                    m.store32(c + 0x26d8u, std::bit_cast<std::uint32_t>(static_cast<float>(ff(0x26d8u) * mult)));
                }
            };
            if (static_cast<std::int32_t>(ctx.gpr[6]) > 0) fix_list(ctx.gpr[7], ctx.gpr[6]);
            if (static_cast<std::int32_t>(ctx.gpr[8]) > 0) fix_list(ctx.gpr[9], ctx.gpr[8]);
            log_line(category::kHle, "PRESIM race-start settle with 60 Hz steps (" + std::to_string(saved.size()) + " cars) at step " + std::to_string(game_step()));
        } else if (address == kPresimEnd && active) {
            active = false;
            for (const auto &p : g_fps_patches) m.store32(p[0], p[2]);
            for (const auto &sv : saved) { m.store32(sv.first + 0x26d0u, sv.second[0]); m.store32(sv.first + 0x26d4u, sv.second[1]); m.store32(sv.first + 0x26d8u, sv.second[2]); }
            saved.clear();
        }
    }
    if ((address == kIntroHandler || address == kRaceHandler) && std::getenv("RR_YLOG") != nullptr) {
        // RR_YLOG=1: the player's car position and the words around it at every intro / race handler call (first 3000 changes)
        static int lines = 0; static std::uint32_t last = 0;
        auto &m = rt->memory();
        const std::uint32_t hud = m.load32(0x08a448f0u);
        const std::uint32_t car = (hud >= 0x08800000u && hud < 0x0a000000u) ? m.load32(hud + 0xc4u) : 0u;
        if (car >= 0x08800000u && car < 0x0a000000u && lines < 3000) {
            const std::uint32_t y = m.load32(car + 0x16a4u);
            if (y != last) {
                last = y; ++lines;
                const auto f = [&](std::uint32_t o) { return static_cast<double>(std::bit_cast<float>(m.load32(car + o))); };
                char b[240];
                std::snprintf(b, sizeof b, "YLOG %s step=%u frame=%llu car=%08X y=%.4f prev=%.4f x=%.4f z=%.4f", address == kIntroHandler ? "intro" : "race", game_step(),
                              static_cast<unsigned long long>(g_frames_presented), car, f(0x16a4u), f(0x16c4u), f(0x16a0u), f(0x16a8u));
                log_line(category::kHle, b);
            }
        }
    }
    if (address == kIntroHandler || address == kRaceHandler) {
        scene_frame = g_frames_presented;
        if (address == kRaceHandler) {
            auto &mem = rt->memory();
            const std::uint32_t mode = mem.load32(ctx.gpr[4] + 0x28u);
            const std::uint32_t vt = (mode >= 0x08800000u && mode < 0x0a000000u) ? mem.load32(mode) : 0u;
            if (vt >= 0x08800000u && vt < 0x0a000000u) {
                // RR_MODE_UNGATE=c,14 (hex vtable offsets) leaves those mode methods running every step (to bisect which one
                // breaks something; with RR_ALL_EXCLUDE for the same functions' fixed candidates 9 / 13 / 14).
                static const std::string ungate = [] { const char *e = std::getenv("RR_MODE_UNGATE"); return std::string(",") + (e ? e : "") + ","; }();
                static const bool oppgate = std::getenv("RR_OPP_MODE8_GATE") != nullptr;
                const std::uint32_t m8 = mem.load32(vt + 0x8u);
                const bool ta_ok = std::getenv("RR_TA_GATE") == nullptr;   // RR_TA_GATE=1: Time Attack's method stays gated on the ticks (old behaviour)
                opp_mode_obj = ((m8 == kOppModeStep || (m8 == kTaModeStep && ta_ok)) && !oppgate) ? mode : 0u;
                if (opp_mode_obj != 0u) opp_mode_fn = m8;
                opp_mode_gate_update(mem);
                for (const std::uint32_t off : {0x8u, 0xcu, 0x14u}) {
                    char offs[16]; std::snprintf(offs, sizeof offs, ",%x,", off);
                    if (ungate.find(offs) != std::string::npos) continue;
                    const std::uint32_t fn = mem.load32(vt + off);
                    if (off == 0x8u && opp_mode_obj != 0u && fn == opp_mode_fn && !oppgate) continue;   // runs every step, its timers are scaled instead
                    if (fn < 0x08804000u || fn >= 0x08980000u || std::find(mode_fns.begin(), mode_fns.end(), fn) != mode_fns.end()) continue;
                    mode_fns.push_back(fn); mode_fns_changed = true;
                    log_line(category::kHle, "RACEMODE handler found: vtable " + hex32(vt) + " +" + hex32(off) + " = " + hex32(fn));
                }
            }
        }
        if (address != last_handler) {
            if (address == kIntroHandler) { warm_pending = true; warm_have = false; cam_started = false; idle_seen = false; pause_muted = false; }   // opponent races have no scene machine hook: start the warm-up here too
            if (address == kRaceHandler) { ailog_step0 = game_step(); ailog_last = ~0u; }
            log_line(category::kHle, std::string("RACESCENE ") + (address == kIntroHandler ? "intro" : "race") + " handler running at frame " + std::to_string(g_frames_presented));
            last_handler = address;
        }
        // RR_AILOG=1: once per game second of the race , every car's speed (+0xc0) and course distance (+0x860), to
        // compare the opponents between a 60 fps run and a raised-rate run (same race, same elapsed game time).
        if (address == kRaceHandler && car_manager != 0u && std::getenv("RR_AILOG") != nullptr) {
            const double per_s = 60.0 * (fast_rate_active() ? g_hle_options.fps_mult : 1.0);
            const std::uint32_t sec = static_cast<std::uint32_t>(static_cast<double>(game_step() - ailog_step0) / per_s);
            if (sec != ailog_last && sec < 400u) {
                ailog_last = sec;
                auto &mem = rt->memory();
                const std::uint32_t base = mem.load32(car_manager + 0x18u), count = mem.load32(car_manager + 0x10u);
                if (base >= 0x08800000u && base < 0x0a000000u && count <= 16u) {
                    std::string l = "AILOG t=" + std::to_string(sec);
                    for (std::uint32_t i = 0; i < count; ++i) {
                        const std::uint32_t c = base + i * 0x27a0u;
                        char b[96];
                        std::snprintf(b, sizeof b, " | %u v=%.1f d=%.0f", i, static_cast<double>(std::bit_cast<float>(mem.load32(c + 0xc0u))),
                                      static_cast<double>(std::bit_cast<float>(mem.load32(c + 0x860u))));
                        l += b;
                    }
                    log_line(category::kHle, l);
                }
            }
        }
        return false;
    }
    if (address == kRaceScene) {
        scene_frame = g_frames_presented;
        const std::uint32_t st = rt->memory().load32(ctx.gpr[4] + 0x38u);
        if (st != scene_state) {
            log_line(category::kHle, "RACESCENE state " + (scene_state == 0xFFFFFFFFu ? std::string("-") : std::to_string(scene_state)) + " -> " +
                                         std::to_string(st) + " at frame " + std::to_string(g_frames_presented));
            scene_state = st;
            if (st == 1u) { warm_pending = true; warm_have = false; cam_started = false; }
        }
        return false;
    }
    if (address == kDriverInputUpdate && ctx.gpr[4] >= 0x08800000u && ctx.gpr[4] < 0x0a000000u) rig_note_drvin_call(rt->memory().load32(ctx.gpr[4]));
    if ((address == kDriverInputUpdate || address == kDriverInputInit) && std::getenv("RR_REPLAYLOG") != nullptr) {
        // RR_REPLAYLOG: driver input record / playback (0x0883BF48, a0 = input object, [+0] mode 0 record / 1 play).  One line per
        // call: call number since the last init, game step, mode and the output words +0xC24..+0xC48 the car reads, to compare the
        // input stream of a race with its replay (value by value, and on which steps each value arrives).
        static std::map<std::uint32_t, std::uint32_t> calls;
        static int lines = 0;
        const std::uint32_t o = ctx.gpr[4];
        if (address == kDriverInputInit) calls[o] = 0;
        else if (o >= 0x08800000u && o < 0x0a000000u && lines < 60000) {
            ++lines;
            auto &m = rt->memory();
            std::string l = "DRVIN n=" + std::to_string(calls[o]++) + " step=" + std::to_string(game_step()) + " obj=" + hex32(o) + " mode=" + std::to_string(m.load32(o)) + " out=";
            for (std::uint32_t k = 0; k < 10u; ++k) { char b[16]; std::snprintf(b, sizeof b, "%s%X", k ? "," : "", m.load32(o + 0xc24u + 4u * k)); l += b; }
            const std::uint32_t hud = m.load32(0x08a448f0u);
            const std::uint32_t car = (hud >= 0x08800000u && hud < 0x0a000000u) ? m.load32(hud + 0xc4u) : 0u;   // the player's car
            if (car >= 0x08800000u && car < 0x0a000000u) {
                const auto f = [&](std::uint32_t off) { return static_cast<double>(std::bit_cast<float>(m.load32(car + off))); };
                char b[120];
                std::snprintf(b, sizeof b, " pos=%.4f,%.4f,%.4f d=%.4f", f(0x16a0u), f(0x16a4u), f(0x16a8u), f(0x860u));
                l += b;
            }
            log_line(category::kHle, l);
        }
    }
    if (address == kCarUpdate) {
        const std::uint32_t st = game_step();
        if (last_car_step != st) { ++sim_count; last_car_step = st; }
    }
    if (address == kDriverInputInit && std::getenv("RR_REPLAY_CAM_FIX_OFF") == nullptr) {
        // A replay (mode 1) starts in the same race scene, so the intro handler / scene start that clear the camera-follow latch do
        // not run again: the latch from the race stayed set and the pan over the car at the start of the replay ran every step,
        // 2.4x too fast at 144 fps.  Clear it at every driver-input init (race start and replay start).
        if (cam_started) log_line(category::kHle, "CAMFOLLOW cleared at driver input init (mode " + std::to_string(ctx.gpr[5]) + ")");
        cam_started = false; idle_seen = false;
    }
    if ((address == kDriverInputInit || address == kDriverInputUpdate) && std::getenv("RR_TICKSYNC_OFF") == nullptr) {
        // Replays re-simulate the race from the recorded inputs; the recorder / player 0x0883BF48 runs on the 60 Hz ticks and the car
        // physics every step, so every recorded input must reach the car on the same simulated step as in the race.  After each init
        // (0x0883BD6C: race start mode 0, replay start mode 1) the first record / playback call becomes the tick origin (the replay
        // then matched the race exactly until the race's pause; pauses are handled at the race handler, see there).
        const std::uint32_t st = game_step();
        if (address == kDriverInputInit) {
            rig_note_drvin_init(ctx.gpr[5]);
            tsync_pending = true;
            log_line(category::kHle, "TICKSYNC driver input init obj=" + hex32(ctx.gpr[4]) + " mode=" + std::to_string(ctx.gpr[5]) + " step=" + std::to_string(st));
        } else if (tsync_pending) {
            tsync_pending = false;
            tick_origin = st - 2u;   // x = 2 is a tick, so this call keeps its slot and the pattern runs from here
            tsync_comp_until = st;
            log_line(category::kHle, "TICKSYNC origin=" + std::to_string(tick_origin) + " at the first input call (step " + std::to_string(st) + ")");
        }
    }
    if (address == kCarUpdate) car_manager = ctx.gpr[4];
    if (address == kCarUpdate && std::getenv("RR_REPLAYLOG") != nullptr) {
        // RR_REPLAYLOG: player car state every 36 steps counted from the tick origin (start signal), to compare the race with its
        // replay line by line (RPOS k=... must match between the two if the replay re-simulates the race exactly).
        static std::uint32_t last = ~0u;
        const std::uint32_t st = game_step(), k = st - tick_origin;
        auto &m = rt->memory();
        const std::uint32_t mgr = ctx.gpr[4];
        static const std::pair<std::uint32_t, std::uint32_t> every = [] {   // RR_CARDUMP_STEPS=lo,hi : every step in that range
            unsigned lo = 0, hi = 0; if (const char *e = std::getenv("RR_CARDUMP_STEPS")) std::sscanf(e, "%u,%u", &lo, &hi); return std::make_pair(lo, hi); }();
        const bool every_step = st >= every.first && st <= every.second;
        if (st != last && ((k % 12u) == 0u || every_step) && k < 200000u && mgr >= 0x08800000u && mgr < 0x0a000000u) {
            last = st;
            const std::uint32_t hud = m.load32(0x08a448f0u);
            const std::uint32_t car = (hud >= 0x08800000u && hud < 0x0a000000u) ? m.load32(hud + 0xc4u) : 0u;   // the player's car
            if (car >= 0x08800000u && car < 0x0a000000u) {
                const auto f = [&](std::uint32_t o) { return static_cast<double>(std::bit_cast<float>(m.load32(car + o))); };
                char b[200];
                std::snprintf(b, sizeof b, "RPOS k=%u step=%u pos=%.4f,%.4f,%.4f d=%.4f gauge=%.3f", k, st, f(0x16a0u), f(0x16a4u), f(0x16a8u), f(0x860u), f(0x2740u));
                log_line(category::kHle, b);
                if (std::getenv("RR_CARDUMP") != nullptr) {
                    std::string l = "CARDUMP k=" + std::to_string(k);
                    for (std::uint32_t o = 0x0u; o < 0x2800u; o += 4u) { char t[32]; std::snprintf(t, sizeof t, " %X:%.5g", o, f(o)); l += t; }
                    log_line(category::kHle, l);
                }
            }
        }
    }
    if (address == kCarUpdate && std::getenv("RR_FRLOG") != nullptr) {
        // RR_FRLOG=1 (rig): every 12th step from the start signal: game time in seconds (steps / (60 * mult)), the player's course distance,
        // speed (+0xc0), engine rpm, nitro gauge, the race clocks (HUD +0x48, radar clock) and the course distance of computer cars 0..7,
        // to compare a race at one frame rate with the same scenario at another (RR_AP_SCRIPT drives the car by 60 Hz race ticks).
        static std::uint32_t last = ~0u;
        const std::uint32_t st = game_step(), k = st - tick_origin;
        auto &m = rt->memory();
        const std::uint32_t mgr = ctx.gpr[4];
        if (st != last && (k % 12u) == 0u && k < 400000u && mgr >= 0x08800000u && mgr < 0x0a000000u) {
            last = st;
            const std::uint32_t hud = m.load32(0x08a448f0u);
            const std::uint32_t car = (hud >= 0x08800000u && hud < 0x0a000000u) ? m.load32(hud + 0xc4u) : 0u;
            const std::uint32_t cb = m.load32(mgr + 0x18u);
            if (car >= 0x08800000u && car < 0x0a000000u) {
                const auto f = [&](std::uint32_t base, std::uint32_t o) { return static_cast<double>(std::bit_cast<float>(m.load32(base + o))); };
                const std::uint32_t radar = m.load32(0x08A447FCu);
                char b[420];
                int n = std::snprintf(b, sizeof b, "FRL t=%.3f k=%u d=%.3f spd=%.3f rpm=%.0f gauge=%.2f hud48=%u radar30=%u opp=", static_cast<double>(k) / (60.0 * (fast_rate_active() ? g_hle_options.fps_mult : 1.0)), k, f(car, 0x860u),
                                      f(car, 0xc0u), f(car, 0x1e98u + 0x1cu), f(car, 0x2740u), m.load32(hud + 0x48u), (radar >= 0x08800000u && radar < 0x0a000000u) ? m.load32(radar + 0x30u) : 0u);
                for (std::uint32_t i = 0; i < 8u && n > 0 && n < 380; ++i) {
                    const std::uint32_t c = cb + i * 0x27a0u;
                    if (c < 0x08800000u || c >= 0x0a000000u) break;
                    n += std::snprintf(b + n, sizeof b - static_cast<std::size_t>(n), "%s%.1f", i ? "," : "", f(c, 0x860u));
                }
                log_line(category::kHle, b);
                // RR_FRL_DUMP=i:T1,T2,...  dump all words of computer car i (hex and float) when the game time first reaches T seconds.
                static const char *dump_env = std::getenv("RR_FRL_DUMP");
                if (dump_env) {
                    static std::vector<double> times; static int ci = -1; static std::size_t next = 0;
                    if (ci < 0) { ci = std::atoi(dump_env); const char *p = std::strchr(dump_env, ':'); while (p && *p) { times.push_back(std::atof(p + 1)); p = std::strchr(p + 1, ','); } }
                    const double gt = static_cast<double>(k) / (60.0 * (fast_rate_active() ? g_hle_options.fps_mult : 1.0));
                    if (next < times.size() && gt >= times[next]) {
                        ++next;
                        const std::uint32_t c = cb + static_cast<std::uint32_t>(ci) * 0x27a0u;
                        std::string l = "CARW c=" + hex32(c) + " T=" + std::to_string(gt);
                        for (std::uint32_t o = 0; o < 0x27a0u; o += 4u) { char t[24]; std::snprintf(t, sizeof t, " %X:%08X", o, m.load32(c + o)); l += t; }
                        log_line(category::kHle, l);
                    }
                }
            }
        }
    }
    if (address == kCarUpdate) {
        // RR_JITLOG=1: every step, words +0x16a0 / +0x16c0 / +0x25e0 (three floats each) of the player and of computer cars
        // 0, 4 and 8, to see whether a car jitters in its own motion (logic) or only on screen (drawing).
        if (std::getenv("RR_JITLOG") != nullptr) {
            static std::uint32_t jit_lines = 0, jit_last_step = ~0u;
            auto &m = rt->memory();
            const std::uint32_t mgr = ctx.gpr[4], st = game_step();
            if (mgr >= 0x08800000u && mgr < 0x0a000000u && st != jit_last_step && jit_lines < 60000u) {
                jit_last_step = st;
                const std::uint32_t hud = m.load32(0x08a448f0u);
                const std::uint32_t pc = (hud >= 0x08800000u && hud < 0x0a000000u) ? m.load32(hud + 0xc4u) : 0u;
                const std::uint32_t cb = m.load32(mgr + 0x18u);
                const std::uint32_t list[4] = {pc, cb, cb + 4u * 0x27a0u, cb + 8u * 0x27a0u};
                if (jit_lines == 0u) log_line(category::kHle, "JITCAR player=" + hex32(pc) + " cpu0=" + hex32(cb) + " mgr=" + hex32(mgr));
                for (int k = 0; k < 4; ++k) {
                    const std::uint32_t c = list[k];
                    if (c < 0x08800000u || c >= 0x0a000000u) continue;
                    const auto f = [&](std::uint32_t o) { return static_cast<double>(std::bit_cast<float>(m.load32(c + o))); };
                    char b[300];
                    std::snprintf(b, sizeof b, "JIT s=%u f=%llu c=%d a=%.3f,%.3f,%.3f b=%.3f,%.3f,%.3f e=%.3f,%.3f,%.3f d=%.2f", st, static_cast<unsigned long long>(g_frames_presented), k,
                                  f(0x16a0u), f(0x16a4u), f(0x16a8u), f(0x16c0u), f(0x16c4u), f(0x16c8u), f(0x25e0u), f(0x25e4u), f(0x25e8u), f(0x860u));
                    std::string extra;
                    if (const char *xs = std::getenv("RR_JITX")) {   // RR_JITX=1764,26a8,... : more float words (hex offsets)
                        std::string t(xs);
                        for (std::size_t q = 0; q < t.size();) {
                            const std::size_t cpos = t.find(',', q);
                            const std::string part = t.substr(q, cpos == std::string::npos ? std::string::npos : cpos - q);
                            if (!part.empty()) { char xb[48]; const std::uint32_t o = static_cast<std::uint32_t>(std::strtoul(part.c_str(), nullptr, 16)); std::snprintf(xb, sizeof xb, " %X=%.5g", o, f(o)); extra += xb; }
                            if (cpos == std::string::npos) break;
                            q = cpos + 1;
                        }
                    }
                    log_line(category::kHle, std::string(b) + extra);
                    ++jit_lines;
                }
            }
        }
    }
    if (address == kIntroCamera && std::getenv("RR_PANLOG") != nullptr) {
        // Diagnostic: one line per intro camera call (game step, frame, camera clock, pre-race timeline, current shot and its clock).
        static std::uint32_t lines = 0, first_step = 0; static bool have_first = false;
        auto &mem = rt->memory();
        const std::uint32_t obj = ctx.gpr[4], st = game_step();
        if (!have_first || st < first_step) { first_step = st; have_first = true; }
        if (lines < 4000u && obj >= 0x08800000u && obj < 0x0a000000u) {
            const std::uint32_t idx = mem.load32(obj + 0xc44u) & 0xffffu, shot = mem.load32(obj + 0xbf4u + 4u * idx);
            const bool ok = shot >= 0x08800000u && shot < 0x0a000000u;
            const std::uint32_t vt = ok ? mem.load32(shot) : 0u;
            char b[260];
            std::snprintf(b, sizeof b, "PAN st=%u rel=%u f=%llu clk=%u tl=%.3f idx=%u shot=%08X step=%08X c1bc=%u",
                          st, st - first_step, static_cast<unsigned long long>(g_frames_presented), mem.load32(obj + 0xc7cu),
                          static_cast<double>(std::bit_cast<float>(mem.load32(0x08A3FE80u + 0x224u))), idx, shot,
                          (vt >= 0x08800000u && vt < 0x0a000000u) ? mem.load32(vt + 0x14u) : 0u, ok ? mem.load32(shot + 0x1bcu) : 0u);
            log_line(category::kHle, b);
            ++lines;
        }
    }
    if (!fast_rate_active()) { intro_valid = false; return false; }   // (nitro_cars is kept: its "written" values stop a rate field being divided twice)
    if (address == kRaceInfoUpdate && std::find(skip.begin(), skip.end(), address) == skip.end()) {
        auto &mem = rt->memory();
        const std::size_t n = (kRaceInfoHi - kRaceInfoLo) / 4u;
        const bool have = race_info_prev.size() == n;
        if (!have) { race_info_prev.assign(n, 0u); race_info_frac.assign(n, 0.0); }
        const double inv = 1.0 / g_hle_options.fps_mult;
        // Pre-race timeline: object +0x224 is a float the race info update counts down by 0.01 per call (while the game
        // phase is 3, before the countdown) and the start camera shot follows; keep 1/mult of each step's decrease.
        if (std::getenv("RR_TIMELINE_OFF") == nullptr) {
            static float tl_last = 0.0f; static bool tl_have = false; static std::uint32_t tl_obj = 0;
            const std::uint32_t obj = ctx.gpr[4];
            const float f = std::bit_cast<float>(mem.load32(obj + 0x224u));
            if (tl_have && obj == tl_obj && f < tl_last && tl_last - f > 0.0099f && tl_last - f < 0.0101f) {
                const float fixed = tl_last - static_cast<float>(0.01 * inv);
                mem.store32(obj + 0x224u, std::bit_cast<std::uint32_t>(fixed));
                tl_last = fixed;
            } else tl_last = f;
            tl_have = true; tl_obj = obj;
        }
        // Sound handles in this range (+0 voice, +4 sound/bank, +8 play id): the play id is a global counter that can also grow
        // by 16-17 between two steps; "scaling" it gave the handle another sound's id, so Stop cut that sound and the drift /
        // tire loop kept playing for ever (and filled the voices).  Never touch those words.
        static std::vector<bool> handle_word;
        static std::size_t handles_marked = 0;
        if (handle_word.size() != n) { handle_word.assign(n, false); handles_marked = 0; }
        static const bool guard_off = std::getenv("RR_RACEINFO_SND_GUARD_OFF") != nullptr;
        if (!guard_off)
            for (; handles_marked < hwatch::sound_handles.size(); ++handles_marked) {
                const std::size_t k0 = (hwatch::sound_handles[handles_marked] - kRaceInfoLo) / 4u;
                for (std::size_t j = 0; j < 3u && k0 + j < n; ++j) handle_word[k0 + j] = true;
            }
        for (std::size_t k = 0; k < n; ++k) {
            const std::uint32_t a = kRaceInfoLo + static_cast<std::uint32_t>(k) * 4u;
            std::uint32_t v = mem.load32(a);
            const std::uint32_t d = v - race_info_prev[k];
            if (have && (d == 16u || d == 17u) && !handle_word[k]) {
                race_info_frac[k] += static_cast<double>(d) * inv;
                const std::uint32_t whole = static_cast<std::uint32_t>(race_info_frac[k]);
                race_info_frac[k] -= whole;
                v = race_info_prev[k] + whole;
                mem.store32(a, v);
            }
            race_info_prev[k] = v;
        }
        return false;
    }
    if (address == kCpuCarUpdate) {
        static const bool off = std::getenv("RR_AI_OFF") != nullptr;
        if (off || car_manager == 0u) return false;
        auto &mem = rt->memory();
        const std::uint32_t base = mem.load32(car_manager + 0x18u), count = mem.load32(car_manager + 0x10u), car = ctx.gpr[4];
        if (base >= 0x08800000u && base < 0x0a000000u && count <= 16u && car >= base && car < base + count * 0x27a0u && (car - base) % 0x27a0u == 0u)
            ctx.fpr[12] *= static_cast<float>(1.0 / g_hle_options.fps_mult);
        return false;
    }
    if (address == kCrossTest) {
        static const bool off = std::getenv("RR_CROSS_OFF") != nullptr;
        if (off) return false;
        auto &mem = rt->memory();
        const std::uint32_t obj = ctx.gpr[4];
        if (obj < 0x08800000u || obj >= 0x0a000000u) return false;
        struct Cross { std::uint32_t step = 0, cur_d = 0, cur_l = 0, prev_d = 0, prev_l = 0; bool have = false; };
        static std::unordered_map<std::uint32_t, Cross> cross;
        Cross &c = cross[obj];
        const std::uint32_t st = game_step();
        const std::uint32_t cur_d = mem.load32(obj + 0x10u), cur_l = mem.load32(obj + 0x0cu);
        if (!c.have || st - c.step > 6u) {   // first call, or a new race: use the game's own previous values
            c.have = true; c.prev_d = mem.load32(obj + 0x80u); c.prev_l = mem.load32(obj + 0x84u);
            c.cur_d = cur_d; c.cur_l = cur_l; c.step = st;
        } else if (c.step != st) {           // a new tick: the previous sample is the one seen at the last tick
            c.prev_d = c.cur_d; c.prev_l = c.cur_l;
            c.cur_d = cur_d; c.cur_l = cur_l; c.step = st;
        }
        mem.store32(obj + 0x80u, c.prev_d); mem.store32(obj + 0x84u, c.prev_l);
        return false;
    }
    if (address == kCarUpdate) {
        car_manager = ctx.gpr[4];
        opp_mode_scale(*rt);
        // RR_CAR_GATE=1 (experimental, use without the physics dt patch): every car steps on the 60 Hz ticks only.
        if (std::getenv("RR_CAR_GATE") != nullptr && std::find(skip.begin(), skip.end(), address) != skip.end()) {
            if (!tick_at(game_step())) { ++skipped; ctx.gpr[2] = 0u; ctx.gpr[3] = 0u; return true; }
            return false;
        }
        static const bool disabled = std::getenv("RR_NITRO_OFF") != nullptr;
        if (disabled) return false;
        auto &mem = rt->memory();
        const std::uint32_t mgr = ctx.gpr[4];
        if (mgr < 0x08800000u || mgr >= 0x0a000000u) return false;
        const float inv = static_cast<float>(1.0 / g_hle_options.fps_mult);
        // Two car arrays, stride 0x27a0: players at [mgr+0x14] (count [mgr+0xc]), computer cars at [mgr+0x18] (count [mgr+0x10]).
        std::uint32_t cars[32]; std::uint32_t ncars = 0;
        for (const auto &[bo, co] : {std::pair<std::uint32_t, std::uint32_t>{0x14u, 0xcu}, {0x18u, 0x10u}}) {
            const std::uint32_t base = mem.load32(mgr + bo), count = mem.load32(mgr + co);
            if (base < 0x08800000u || base >= 0x0a000000u || count > 16u) continue;
            for (std::uint32_t i = 0; i < count && ncars < 32u; ++i) cars[ncars++] = base + i * 0x27a0u;
        }
        for (std::uint32_t ci = 0; ci < ncars; ++ci) {
            const std::uint32_t car = cars[ci];
            CarNitro &n = nitro_cars[car];
            // Gauge: keep 1/mult of each step's gain (spending a tank or other jumps pass unchanged).
            float g = std::bit_cast<float>(mem.load32(car + kGaugeField));
            if (n.have) {
                const float d = g - n.gauge;
                if (d > 0.0f && d < 5.0f) { g = n.gauge + d * inv; mem.store32(car + kGaugeField, std::bit_cast<std::uint32_t>(g)); }
            }
            n.gauge = g; n.have = true;
            // Per-step rates: divide by mult whenever the game has (re)written one.
            for (std::size_t k = 0; k < std::size(kRateFieldOffsets); ++k) {
                const std::uint32_t a = car + kRateFieldOffsets[k];
                const std::uint32_t v = mem.load32(a);
                if (v == n.written[k]) continue;
                n.written[k] = std::bit_cast<std::uint32_t>(std::bit_cast<float>(v) * inv);
                mem.store32(a, n.written[k]);
            }
        }
        return false;
    }
    if (intro_mode() == 0 && ctx.gpr[31] == kShotStepReturn &&
        std::find(shot_steps.begin(), shot_steps.end(), address) != shot_steps.end()) {
        if (address != last_shot_step) { log_line(category::kHle, "INTRO shot step " + hex32(address) + " at frame " + std::to_string(g_frames_presented)); last_shot_step = address; }
        for (const ShotClock &sc : kShotClocks) {
            if (sc.step != address) continue;
            const std::uint32_t field = ctx.gpr[4] + sc.offset;
            const std::uint32_t c = rt->memory().load32(field);
            ClockTrack &t = shot_clocks[ctx.gpr[4]];
            if (t.valid && c == t.last + 5u) {
                const double before = t.acc;
                t.acc += 5.0 / g_hle_options.fps_mult;
                const std::uint32_t fixed = t.last + static_cast<std::uint32_t>(std::floor(t.acc) - std::floor(before));
                rt->memory().store32(field, fixed);
                t.last = fixed;
            } else {
                t.last = c; t.acc = 0.0;
            }
            t.valid = true;
            return false;
        }
        // Unknown shot type: step only on the 60 Hz ticks.
        const double m = g_hle_options.fps_mult;
        const std::uint32_t st = game_step();
        return std::floor((static_cast<double>(st) + 1.0) / m) == std::floor(static_cast<double>(st) / m);
    }
    if (address == kIntroCamera && intro_mode() == 0) {
        const std::uint32_t obj = ctx.gpr[4];
        discover_shots(obj);
        const std::uint32_t c = rt->memory().load32(obj + 0xc7cu);
        if (intro_valid && obj == intro_obj && c == intro_last + 5u) {
            // The camera added its 5 last step: keep only 5/mult of it (whole units, carried in intro_acc).
            const double step = 5.0 / g_hle_options.fps_mult;
            const double before = intro_acc;
            intro_acc += step;
            const std::uint32_t inc = static_cast<std::uint32_t>(std::floor(intro_acc) - std::floor(before));
            const std::uint32_t fixed = intro_last + inc;
            rt->memory().store32(obj + 0xc7cu, fixed);
            intro_last = fixed;
        } else {
            intro_last = c; intro_acc = 0.0;
        }
        intro_obj = obj; intro_valid = true;
        return false;
    }
    if (std::find(skip.begin(), skip.end(), address) == skip.end() && !(address == kIntroCamera && intro_mode() == 1)) return false;
    const std::uint32_t s = game_step();
    if (pad_frame == g_frames_presented && !pad_delta_known) {
        pad_delta = s - pad_step; pad_delta_known = true;
        log_line(category::kHle, "GATES pad read is " + std::to_string(pad_delta) + " step(s) before the gated systems");
    }
    // Run on the steps where a 60 Hz clock would tick: floor((s + 1) / m) != floor(s / m).
    if (warm_pending) { warm_pending = false; warm_have = true; warm_first = s; }
    const bool warm = warm_have && static_cast<std::uint32_t>(s - warm_first) <= 1u && !std::getenv("RR_WARM_OFF");
    // The race camera (candidate 12, 0x08882a64) only needs the 60 Hz ticks for the pan over the car before the start: once the
    // player's car moves it must follow every step, or the cars it passes jitter forward and back against a camera that moves
    // in 60 Hz jumps.  RR_CAM_GATE_ALWAYS=1 keeps the camera gated for the whole race.
    bool cam_follow = false;
    if (address == 0x08882a64u && std::getenv("RR_CAM_GATE_ALWAYS") == nullptr) {
        auto &m = rt->memory();
        const std::uint32_t hud = m.load32(0x08a448f0u);
        const std::uint32_t pc = (hud >= 0x08800000u && hud < 0x0a000000u) ? m.load32(hud + 0xc4u) : 0u;
        // (Before the start the player's speed word idles at 1..3; it jumps past 20 right after GO.  Latched until the next race.)
        // The speed word can hold a stale value > 10 during the track intro (it did at 144+ fps and latched this too early, so the
        // pan over the car ran ungated at the wrong speed): only count a start after the idle value was seen in the race handler.
        if (pc >= 0x08800000u && pc < 0x0a000000u) {
            const float spd = std::fabs(std::bit_cast<float>(m.load32(pc + 0xc0u)));
            if (spd <= 10.0f && last_handler == kRaceHandler) idle_seen = true;
            else if (spd > 10.0f && idle_seen && !cam_started) {
                cam_started = true;
                log_line(category::kHle, "CAMFOLLOW latched at frame " + std::to_string(g_frames_presented) + " step " + std::to_string(game_step()) + " speed " + std::to_string(spd));
            }
        }
        cam_follow = cam_started;
    }
    // (The music selector also runs on the pause screen, where the simulation count stands still: it keeps the game-step pattern.)
    const bool run = (address == kMusicSelectUpdate ? menu_tick(s) : race_tick()) || warm || cam_follow || (address == 0x088ee8b4u && scene_every_step());
    if (!run) {
        ++skipped;
        // A skipped function "returns" 0: some callers act on the result (the race update ends the race when the
        // per-mode handler 0x088f3790 returns non-zero), and a stale v0 would look like a real answer.
        ctx.gpr[2] = 0u; ctx.gpr[3] = 0u;
    }
    return !run;
}
// Candidate subsystems for the live gate test (F2 / F3 / F7): the per-step callees of the race update 0x0883dde0 except
// the car (0x0883e70c), the other calls on the race path and the top-level step calls.
struct Candidate { std::uint32_t address; const char *note; };
const Candidate kCandidates[] = {
    {0x0883d608u, "race 1"},   {0x08842354u, "race 2"},  {0x0889a048u, "race 3"},  {0x088214b8u, "race 4"},
    {0x08891484u, "race 5"},   {0x0883f518u, "race 6 (returns a pointer: not in ALL)"},  {0x088efa3cu, "race 7 draws"}, {0x088f01ccu, "race 8 objects"},
    {0x0883ca5cu, "race 9"},   {0x088980e8u, "race 10 director"}, {0x088ee8b4u, "race 11"}, {0x08882a64u, "race 12 effects"},
    {0x088f3790u, "race 13"},  {0x088f3c08u, "race 14"}, {0x0883d59cu, "race path a"}, {0x08818a28u, "race path b"},
    {0x08809910u, "step a"},   {0x08820648u, "step b sound?"}, {0x0889d23cu, "step c"}, {0x08845e48u, "step d"},
    {0x0889d3fcu, "step e"},
};
constexpr int kCandidateCount = static_cast<int>(sizeof kCandidates / sizeof kCandidates[0]);
// Sprite/effect list update (0x08910a04, once per step, from 0x08911154): drives the race countdown graphics.  RR_SPRITE_OFF=1 leaves it alone.
constexpr std::uint32_t kSpriteListStep = 0x08910a04u;
constexpr int kAllRaceCount = 14;   // "all" gates the race update's own callees only (gating every candidate ends each race at once)
std::vector<std::uint32_t> base_skip;            // gates from RR_GATES / --fps-gate
int applied_sel = -1;
unsigned long long applied_kept = 0ull;

void discover_shots(std::uint32_t camera) {
    auto &m = rt->memory();
    for (std::uint32_t i = 0; i < 32u; ++i) {
        const std::uint32_t shot = m.load32(camera + 0xbf4u + 4u * i);
        if (shot < 0x08800000u || shot >= 0x0a000000u || (shot & 3u) != 0u) continue;
        const std::uint32_t vt = m.load32(shot);
        if (vt < 0x08800000u || vt >= 0x0a000000u || (vt & 3u) != 0u) continue;
        const std::uint32_t step = m.load32(vt + 0x14u);
        if (step < 0x08804000u || step >= 0x08980000u || (step & 3u) != 0u) continue;
        if (std::find(shot_steps.begin(), shot_steps.end(), step) != shot_steps.end()) continue;
        shot_steps.push_back(step);
        table[(step - psprecomp::kFunctionGateBase) >> 2u] = 1u;
        log_line(category::kHle, "INTRO shot type found: object " + hex32(shot) + " vtable " + hex32(vt) + " step " + hex32(step));
    }
}
void refresh_table() {
    if (table.empty()) return;
    std::fill(table.begin(), table.end(), profile ? 1u : 0u);
    table[(kRaceScene - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    table[(kIntroHandler - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    table[(kRaceHandler - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    table[(kCarUpdate - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    table[(kPresim - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    table[(kTorqueRamp - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    table[(kGearbox - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    table[(kCarIntegrate - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    table[(kTrackShift - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    table[(kCarTrackSpeed - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    table[(kCarTrackEvents - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    table[(kPresimEnd - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    table[(kRaceInfoUpdate - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    table[(kCrossTest - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    table[(kCpuCarUpdate - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    if (sndlog_enabled()) table[(kSoundManager - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    if (sndlog_enabled()) table[(kSoundStart - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    if (std::getenv("RR_MAILLOG") != nullptr) table[(kMailTake - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    for (const std::uint32_t a : calllog_addrs())
        if (a >= psprecomp::kFunctionGateBase && a - psprecomp::kFunctionGateBase < psprecomp::kFunctionGateSpan)
            table[(a - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    table[(kSpriteSlotSetX - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    for (const std::uint32_t a : {0x08888bd8u, 0x08888ad0u}) table[(a - psprecomp::kFunctionGateBase) >> 2u] = 1u;   // (netplay prediction; cheap when off)
    if (g_hle_options.menu_native) for (const std::uint32_t a : {0x0884f290u, 0x089622e0u, 0x08960200u, 0x0895fee8u, 0x08825750u, 0x088742ccu, 0x088728c8u}) table[(a - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    table[(kAnimStep - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    table[(kPadUpdate - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    table[(kSePlay - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    table[(kHeliUpdate - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    table[(kCtrlUpdate - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    table[(kInfoWinUpdate - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    table[(kPvsBuild - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    if (std::getenv("RR_NOCULL") != nullptr) for (const std::uint32_t a : {0x088d08c8u, 0x088d0940u, 0x088d09b8u}) table[(a - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    table[(kDriverInputInit - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    for (const std::uint32_t a : kHudUpdates) table[(a - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    table[(kDriverInputUpdate - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    for (const std::uint32_t a : {0x088d8eb8u, 0x088d8d94u, 0x088d8af4u, 0x088d89bcu, 0x088d8738u, 0x088d83d8u}) table[(a - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    if (std::getenv("RR_REPLAYLOG") != nullptr) table[(0x08845e48u - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    table[(0x08911274u - psprecomp::kFunctionGateBase) >> 2u] = 1u;   // sound id repair (start)
    table[(0x089105a4u - psprecomp::kFunctionGateBase) >> 2u] = 1u;   // sound id repair (Stop)
    if (std::getenv("RR_AUDIOLOG") != nullptr)
        for (const std::uint32_t a : {0x0891eeb0u, 0x0891eeecu, 0x0891ef28u, 0x08910fecu, 0x08910f70u, 0x08911274u, 0x089112e8u, 0x089105a4u, 0x08913608u}) table[(a - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    if (std::getenv("RR_AUDIOLOG") != nullptr)
        for (const std::uint32_t a : hwatch::kAddrs) table[(a - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    if (std::getenv("RR_SELLOG") != nullptr) table[(kMusicSelectUpdate - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    table[(0x088ee8b4u - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    table[(kEffectStep - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    table[(kSlotUpdate - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    if (intro_mode() != 2) table[(kIntroCamera - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    for (const std::uint32_t a : shot_steps) table[(a - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    for (const std::uint32_t a : mode_fns) table[(a - psprecomp::kFunctionGateBase) >> 2u] = 1u;
    for (const std::uint32_t a : skip)
        if (a >= psprecomp::kFunctionGateBase && a - psprecomp::kFunctionGateBase < psprecomp::kFunctionGateSpan)
            table[(a - psprecomp::kFunctionGateBase) >> 2u] = 1u;
}
void install(Runtime &runtime) {
    rt = &runtime;
    if (const char *e = std::getenv("RR_GATES")) {
        std::string s(e);
        for (std::size_t p = 0; p < s.size();) {
            const std::size_t c = s.find(',', p);
            const std::string part = s.substr(p, c == std::string::npos ? std::string::npos : c - p);
            if (!part.empty()) skip.push_back(static_cast<std::uint32_t>(std::strtoul(part.c_str(), nullptr, 0)));
            if (c == std::string::npos) break;
            p = c + 1;
        }
    }
    for (const std::uint32_t a : g_hle_options.fps_gates) skip.push_back(a);
    base_skip = skip;
    g_gate_count = kCandidateCount;
    // With a raised frame rate the race systems run at 60 Hz by default (RR_GATES_OFF=1 to start with none; F2/F3 change it).
    if (g_hle_options.fps_mult != 1.0 && std::getenv("RR_GATES_OFF") == nullptr) g_gate_sel = kCandidateCount + 1;
    table.assign(psprecomp::kFunctionGateSpan / 4u, 0u);
    refresh_table();
    psprecomp::g_function_gate_table = table.data();
    psprecomp::g_function_gate_hook = &hook;
    std::string list;
    for (const std::uint32_t a : skip) list += " " + hex32(a);
    log_line(category::kHle, std::string("ANIMFIX build=a53 mirror_old=") + (std::getenv("RR_MIRROR_OLD") ? "1" : "0") + " anim_scale_off=" + (std::getenv("RR_ANIM_SCALE_OFF") ? "1" : "0") +
                                 " all_exclude=" + (std::getenv("RR_ALL_EXCLUDE") ? std::getenv("RR_ALL_EXCLUDE") : "-"));
    log_line(category::kHle, "GATES installed, half-rate functions:" + (list.empty() ? std::string(" none") : list));
}
// Once per frame: picks up F2 / F3 / F7 changes and rebuilds the gate list.
// RR_ALL_EXCLUDE=7,8 leaves those candidates (1-based, as F2 numbers them) out of "ALL" (to find which gate breaks what);
// RR_ALL_NOMODE=1 leaves the game-mode methods out.
bool all_included(int number) {
    const char *e = std::getenv("RR_ALL_INCLUDE");
    if (e == nullptr) return std::getenv("RR_GATES_ALL") != nullptr && number == 4;   // RR_GATES_ALL=1 includes the race info (4)
    const std::string t = std::string(",") + e + ",";
    return t.find("," + std::to_string(number) + ",") != std::string::npos;
}
bool all_excluded(int number) {
    static const std::vector<int> ex = [] {
        std::vector<int> v;
        // Default: the game-mode methods (countdown, timers), 11 (trackside effects, the plane), 12 (race camera) and the
        // effect step below; everything else (1, 2, 3, 7, 8, 10 and the race info 4) runs every step.  The race info must see
        // every step; its timers are scaled instead and the line-crossing test is fed 60 Hz samples (kCrossTest).
        // RR_GATES_ALL=1 gates everything again; "lean" (game-mode methods only) is RR_ALL_EXCLUDE=1,2,3,7,8,10,11,12.
        if (std::getenv("RR_ALL_EXCLUDE") == nullptr && std::getenv("RR_GATES_ALL") == nullptr) return std::getenv("RR_MIRROR_ALWAYS") != nullptr ? std::vector<int>{1, 2, 3, 7, 8, 10, 11, 13} : std::vector<int>{1, 2, 3, 7, 8, 10, 13};
        if (const char *e = std::getenv("RR_ALL_EXCLUDE")) {
            std::string t(e);
            for (std::size_t p = 0; p < t.size();) {
                const std::size_t c = t.find(',', p);
                const std::string part = t.substr(p, c == std::string::npos ? std::string::npos : c - p);
                if (!part.empty()) v.push_back(std::atoi(part.c_str()));
                if (c == std::string::npos) break;
                p = c + 1;
            }
        }
        return v;
    }();
    return std::find(ex.begin(), ex.end(), number) != ex.end();
}
void apply_live() {
    if (table.empty()) return;
    const int sel = g_gate_sel.load();
    const unsigned long long kept = g_gate_kept.load();
    if (sel == applied_sel && kept == applied_kept && !mode_fns_changed) return;
    applied_sel = sel; applied_kept = kept; mode_fns_changed = false;
    skip = base_skip;
    std::string list;
    for (int i = 0; i < kCandidateCount; ++i) {
        const std::uint32_t a = kCandidates[i].address;
        // Not in ALL: 0883f518 (returns a pointer the car code uses), 088214b8 (race positions / laps: it must see every
        // step to catch the finish line; its timers are scaled instead, see kRaceInfoUpdate) and 08891484 (car sounds:
        // it must see every collision).
        bool in_all = a != 0x0883f518u && a != 0x088214b8u && a != 0x08891484u;
        if (in_all && all_excluded(i + 1)) in_all = false;
        if (a == 0x088214b8u && all_included(4)) in_all = true;   // RR_ALL_INCLUDE=4: race info (laps) gated with the rest
        if ((sel == kCandidateCount + 1 && i < kAllRaceCount && in_all) || sel == i + 1 || ((kept >> i) & 1ull) != 0ull) {
            skip.push_back(kCandidates[i].address);
            list += " " + std::to_string(i + 1) + ":" + hex32(kCandidates[i].address);
        }
    }
    // Effect step 0x08918048 (called per active trackside effect from 0x088cc4a4, e.g. the plane): one animation tick per
    // call, so at the raised rate it runs on the 60 Hz ticks only.  Its result is ignored.  RR_FX_OFF=1 leaves it alone.
    if (sel == kCandidateCount + 1 && std::getenv("RR_FX_OFF") == nullptr) { skip.push_back(kEffectStep); list += " fx:" + hex32(kEffectStep); }
    // RR_CAR_GATE=1: the per-car update 0x0883e70c (the one candidate list leaves out) also runs on the 60 Hz ticks only.  Use
    // it WITHOUT --fps-patch (car dt stays 1/60): the car physics then steps exactly like the 60 fps game, so the hill crest
    // behaves the same, while the rest of the game keeps the raised rate.  Experimental.
    if (sel == kCandidateCount + 1 && std::getenv("RR_CAR_GATE") != nullptr) { skip.push_back(0x0883e70cu); list += " car:0883e70c"; }
    // Tire smoke: the scene setup 0x088ee8b4 (candidate 11) runs every step once the race is under way, and it also drives the
    // smoke particles: 0x08904f78 (per car: the wheels add the car speed to a counter every call and spawn a puff past 3000 / 4000,
    // plus the trail emitter), 0x08835c04 (particle frame begin) and 0x08835c40 (particle motion / ageing).  Every step they spawned
    // 4x the puffs at 240 fps that also aged 4x faster: a thick short cloud around drifting cars.  Kept on the 60 Hz ticks; the
    // drawing (0x08835cbc) still runs every step.  RR_SMOKE_GATE_OFF=1 restores the old behaviour.
    if (sel == kCandidateCount + 1 && std::getenv("RR_SMOKE_GATE_OFF") == nullptr) {
        for (const std::uint32_t f : {0x08904f78u, 0x08835c04u, 0x08835c40u}) { skip.push_back(f); list += " smoke:" + hex32(f); }
    }
    // RR_GATE_EXTRA=addr,addr,...: more functions kept on the 60 Hz ticks (diagnostic: finds what moves the plane inside candidate 11).
    if (sel == kCandidateCount + 1) {
        if (const char *e = std::getenv("RR_GATE_EXTRA")) {
            std::string t(e);
            for (std::size_t q = 0; q < t.size();) {
                const std::size_t c = t.find(',', q);
                const std::string part = t.substr(q, c == std::string::npos ? std::string::npos : c - q);
                if (!part.empty()) { const std::uint32_t x = static_cast<std::uint32_t>(std::strtoul(part.c_str(), nullptr, 16)); skip.push_back(x); list += " extra:" + hex32(x); }
                if (c == std::string::npos) break;
                q = c + 1;
            }
        }
    }
    if (sel == kCandidateCount + 1 && (std::getenv("RR_MIRROR_OLD") != nullptr || std::getenv("RR_SPRITE_GATE") != nullptr) && std::getenv("RR_SPRITE_OFF") == nullptr) { skip.push_back(kSpriteListStep); list += " sprite:" + hex32(kSpriteListStep); }
    // Music selector shown over the track intro (update of a screen object, called from the object list loop at 0x088f9b60): a held
    // direction repeats after 8 calls of this function ([obj+0x28] counts them), so at the raised rate it repeated twice as fast.  Its
    // result is ignored.  RR_SELECT_OFF=1 leaves it alone.
    if (sel == kCandidateCount + 1 && std::getenv("RR_SELECT_OFF") == nullptr) { skip.push_back(kMusicSelectUpdate); list += " select:" + hex32(kMusicSelectUpdate); }
    if (sel == kCandidateCount + 1 && std::getenv("RR_ALL_NOMODE") == nullptr)
        for (const std::uint32_t f : mode_fns) { skip.push_back(f); list += " mode:" + hex32(f); }
    refresh_table();
    log_line(category::kHle, "GATE live sel=" + std::to_string(sel) + " gated:" + (list.empty() ? std::string(" none") : list));
}
// Title text for the live gate test ("" when nothing is selected or kept).
std::string title_text() {
    if (table.empty()) return {};
    const int sel = g_gate_sel.load();
    const unsigned long long kept = g_gate_kept.load();
    if (sel < 0 && kept == 0ull) return {};
    std::string t = " - GATE ";
    if (sel == kCandidateCount + 1) t += "ALL race 1-14";
    else if (sel > 0 && sel <= kCandidateCount) {
        char c[64];
        std::snprintf(c, sizeof c, "%d/%d %08X %s", sel, kCandidateCount, kCandidates[sel - 1].address, kCandidates[sel - 1].note);
        t += c;
    } else t += "off";
    if (kept != 0ull) {
        t += " kept";
        for (int i = 0; i < kCandidateCount; ++i) if (((kept >> i) & 1ull) != 0ull) t += " " + std::to_string(i + 1);
    }
    if (!fast_rate_active()) t += " (inactive)";
    return t;
}
void profile_begin() {
    if (table.empty()) return;
    if (profile_owners++ > 0) return;   // already profiling for the other trace: share it
    profile = true; frame_calls.clear(); call_log.clear(); edges.clear(); profile_frame = 0;
    refresh_table();
}
void profile_frame_end(bool owner) {
    if (!profile || !owner) return;
    for (const auto &[a, n] : frame_calls) {
        auto &v = call_log[a];
        v.resize(static_cast<std::size_t>(profile_frame), 0u);
        v.push_back(n);
    }
    frame_calls.clear();
    ++profile_frame;
}
void profile_end(const std::string &name, const std::vector<std::uint64_t> &frames) {
    if (!profile) return;
    if (--profile_owners > 0) return;   // the other trace is still running and will write the file
    profile = false; refresh_table();
    if (!generated_with_gates && call_log.empty()) {
        log_line(category::kHle, "CALLS: no function entries seen - the code was not generated with --function-gates");
        return;
    }
    // One line per function: address, frames called, total calls, then the per-frame counts (run-length compressed).
    std::string out = "CALLS frames=" + std::to_string(profile_frame) + " first_frame=" + (frames.empty() ? std::string("?") : std::to_string(frames.front())) + "\n";
    std::vector<std::uint32_t> keys;
    for (const auto &[a, v] : call_log) keys.push_back(a);
    std::sort(keys.begin(), keys.end());
    for (const std::uint32_t a : keys) {
        auto v = call_log[a];
        v.resize(static_cast<std::size_t>(profile_frame), 0u);
        std::uint64_t total = 0; int called = 0;
        for (const auto n : v) { total += n; called += n != 0u ? 1 : 0; }
        char b[64];
        std::snprintf(b, sizeof b, "%08X %d %llu", a, called, static_cast<unsigned long long>(total));
        out += b;
        for (std::size_t i = 0; i < v.size();) {
            std::size_t j = i;
            while (j < v.size() && v[j] == v[i]) ++j;
            out += " " + std::to_string(v[i]) + (j - i > 1 ? "x" + std::to_string(j - i) : std::string());
            i = j;
        }
        out += "\n";
    }
    std::error_code ec;
    std::filesystem::create_directories("traces", ec);
    write_file_async(name, std::vector<std::uint8_t>(out.begin(), out.end()));
    log_line(category::kHle, "CALLS written to " + name + " (" + std::to_string(keys.size()) + " functions)");
    call_log.clear();
    // Call edges: function, return address (inside the caller), count.
    std::string eout;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> ev(edges.begin(), edges.end());
    std::sort(ev.begin(), ev.end());
    for (const auto &[k, n] : ev) {
        char b[48];
        std::snprintf(b, sizeof b, "%08X %08X %u\n", static_cast<unsigned>(k >> 32u), static_cast<unsigned>(k & 0xFFFFFFFFu), n);
        eout += b;
    }
    std::string ename = name;
    if (const auto dot = ename.rfind(".txt"); dot != std::string::npos) ename.insert(dot, "_edges");
    write_file_async(ename, std::vector<std::uint8_t>(eout.begin(), eout.end()));
    edges.clear();
}
} // namespace gates

namespace cartrace {
constexpr int kCarTraceFramesDefault = 300;
int trace_frames() { const char *e = std::getenv("RR_TRACE_FRAMES"); const int n = e ? std::atoi(e) : 0; return n > 0 ? std::min(n, 3000) : kCarTraceFramesDefault; }
constexpr std::uint32_t kCarTraceBefore = 0x8000u, kCarTraceAfter = 0x30000u;
struct Field { std::uint32_t value; std::uint32_t pc; std::uint32_t ra; std::uint32_t count; };
bool active = false;
int frames_left = 0, number = 0;
std::uint32_t body = 0, lo = 0, hi = 0;
std::unordered_map<std::uint32_t, Field> frame_fields;
std::unordered_map<std::uint32_t, std::uint32_t> last_value;
std::string out;
// RR_TRACE_SUMMARY=1: instead of listing every change, keep per-word statistics over the whole trace and write only the
// words that rise or fall steadily (a gauge, a timer), with the function that wrote them.
bool summary = false;
struct Stat { std::uint32_t first{}, last{}, changes{}, fup{}, fdown{}, iup{}, idown{}, pc{}, ra{}, first_frame{}, last_frame{}; };
std::unordered_map<std::uint32_t, Stat> stats;
int trace_frame = 0;
std::uint32_t pad_buttons = 0, pad_lx = 128, pad_ly = 128, pad_buffer = 0, pad_count = 0;
bool in_hle_pad_write = false;
std::size_t lines = 0;

void sink(bool is_write, std::uint32_t address, std::size_t length, std::uint64_t, std::uint64_t new_value) {
    if (!active) return;
    const std::uint32_t a = address & 0x1FFFFFFFu;
    if (is_write) {
        if (a < lo || a >= hi || length != 4u) return;
        Field &f = frame_fields[a];
        f.value = static_cast<std::uint32_t>(new_value);
        f.pc = gates::profile ? gates::current : psprecomp::runtime_dispatch_pc();
        f.ra = psprecomp::runtime_watch_register(31);
        ++f.count;
    } else if (!in_hle_pad_write && lines < 4000000u) {
        char b[96];
        std::snprintf(b, sizeof b, "R %08X pc=%08X ra=%08X len=%u\n", a, psprecomp::runtime_dispatch_pc(), psprecomp::runtime_watch_register(31),
                      static_cast<unsigned>(length));
        out += b; ++lines;
    }
}

std::uint32_t find_body(Runtime &rt) {
    const std::uint32_t base = 0x09000000u, len = 0x01000000u;
    const std::uint8_t *p = static_cast<const psprecomp::GuestMemory &>(rt.memory()).raw_pointer(base, len);
    if (p == nullptr) return 0u;
    std::uint32_t found = 0u;
    for (std::uint32_t o = 0x26d0u; o + 12u <= len; o += 4u) {
        std::uint32_t a, c;
        std::memcpy(&a, p + o, 4); if (a != 0x3C888889u) continue;
        std::memcpy(&c, p + o + 8u, 4); if (c != 0x3C888889u) continue;
        const std::uint32_t b = base + o - 0x26d0u;
        log_line(category::kHle, "CARTRACE body candidate " + hex32(b));
        if (found == 0u) found = b;
    }
    return found;
}

void start(Runtime &rt) {
    if (active) return;
    if (!rt.memory().write_watch_active()) { log_line(category::kHle, "CARTRACE needs --trace-tools on the command line"); return; }
    const char *range_env = std::getenv("RR_TRACE_RANGE");
    if (range_env != nullptr && std::string(range_env).rfind("car", 0) == 0 && std::string(range_env).size() > 3) {
        // "carN": car N of the race's car array (0 = first; the player's index is logged as CARLOG player=).
        const int n = std::atoi(range_env + 3);
        const std::uint32_t mgr = gates::car_manager;
        const std::uint32_t base = mgr ? rt.memory().load32(mgr + 0x14u) : 0u;
        if (base < 0x08800000u || base >= 0x0a000000u) { log_line(category::kHle, "CARTRACE: no car array (start a race first)"); return; }
        body = base + static_cast<std::uint32_t>(n) * 0x27a0u;
        lo = body; hi = body + 0x27a0u;
    } else if (range_env != nullptr && std::string(range_env) == "ai") {
        const std::uint32_t mgr = gates::car_manager;
        const std::uint32_t base = mgr ? rt.memory().load32(mgr + 0x18u) : 0u, count = mgr ? rt.memory().load32(mgr + 0x10u) : 0u;
        if (base < 0x08800000u || base >= 0x0a000000u || count == 0u || count > 16u) { log_line(category::kHle, "CARTRACE: no computer cars"); return; }
        body = base; lo = base; hi = base + count * 0x27a0u;
    } else if (range_env != nullptr && std::string(range_env) == "mode") {
        // The race mode object of a race with opponents (state counter +0x10, thresholds): who counts the countdown.
        const std::uint32_t mo = gates::opp_mode_obj;
        if (mo < 0x08800000u || mo >= 0x0a000000u) { log_line(category::kHle, "CARTRACE: no opponent-race mode object"); return; }
        body = mo; lo = mo; hi = mo + 0x100u;
    } else if (range_env != nullptr && std::string(range_env) == "hud") {
        // The HUD object's timer words (+0x40..+0xA0): who writes the race clock and the lap time displays.
        const std::uint32_t hud = rt.memory().load32(0x08a448f0u);
        if (hud < 0x08800000u || hud >= 0x0a000000u) { log_line(category::kHle, "CARTRACE: no HUD object (start a race first)"); return; }
        body = hud; lo = hud + 0x40u; hi = hud + 0xA0u;
    } else if (range_env != nullptr && std::string(range_env) == "car") {
        // The player's car as the HUD sees it: [[0x08a448f0] + 0xc4], one body (0x27a0 bytes).
        const std::uint32_t hud = rt.memory().load32(0x08a448f0u);
        body = (hud >= 0x08800000u && hud < 0x0a000000u) ? rt.memory().load32(hud + 0xc4u) : 0u;
        if (body < 0x08800000u || body >= 0x0a000000u) { log_line(category::kHle, "CARTRACE: no player car (start a race first)"); return; }
        lo = body; hi = body + 0x27a0u;
    } else if (const char *e = range_env) {
        // "lo,hi": trace this address range instead of the player's car (offsets are then from lo)
        const std::string s(e);
        const std::size_t c = s.find(',');
        lo = static_cast<std::uint32_t>(std::strtoul(s.c_str(), nullptr, 0));
        hi = c == std::string::npos ? lo + 4u : static_cast<std::uint32_t>(std::strtoul(s.c_str() + c + 1, nullptr, 0));
        body = lo;
    } else {
        body = find_body(rt);
        if (body == 0u) { log_line(category::kHle, "CARTRACE: no car found (start a race first)"); return; }
        lo = body - kCarTraceBefore; hi = body + kCarTraceAfter;
    }
    gates::profile_begin();
    psprecomp::set_watch_sink(&hle_watch_sink);
    psprecomp::set_write_watch_range(lo, hi - lo);
    if (std::getenv("RR_TRACE_READS") != nullptr) rt.memory().set_read_watch_range(lo, hi - lo);   // also log who reads the range
    else if (pad_buffer != 0u) rt.memory().set_read_watch_range(pad_buffer, 16u * std::max<std::uint32_t>(pad_count, 1u));
    frame_fields.clear(); last_value.clear(); out.clear(); lines = 0;
    summary = std::getenv("RR_TRACE_SUMMARY") != nullptr; stats.clear(); trace_frame = 0;
    out += "CARTRACE body=" + hex32(body) + " range=" + hex32(lo) + "-" + hex32(hi) + " (offsets below are from body)\n";
    active = true; frames_left = trace_frames(); ++number;
    log_line(category::kHle, "CARTRACE started, body " + hex32(body));
}

void frame_end(Runtime &rt, std::uint64_t frame) {
    if (!active) return;
    char b[160];
    std::snprintf(b, sizeof b, "F %llu buttons=%08X lx=%u ly=%u\n", static_cast<unsigned long long>(frame), pad_buttons, pad_lx, pad_ly);
    out += b; ++lines;
    if (summary) {
        for (const auto &[a, f] : frame_fields) {
            auto it = last_value.find(a);
            if (it == last_value.end()) {   // first write seen: the starting value
                last_value[a] = f.value;
                Stat &st = stats[a]; st.first = st.last = f.value; st.pc = f.pc; st.ra = f.ra; st.first_frame = st.last_frame = static_cast<std::uint32_t>(trace_frame);
                continue;
            }
            if (it->second == f.value) continue;
            Stat &st = stats[a];
            const float o = std::bit_cast<float>(it->second), n = std::bit_cast<float>(f.value);
            if (n > o) ++st.fup; else if (n < o) ++st.fdown;
            if (static_cast<std::int32_t>(f.value) > static_cast<std::int32_t>(it->second)) ++st.iup; else ++st.idown;
            ++st.changes; st.last = f.value; st.pc = f.pc; st.ra = f.ra; st.last_frame = static_cast<std::uint32_t>(trace_frame);
            it->second = f.value;
        }
        frame_fields.clear();
        ++trace_frame;
        gates::profile_frame_end(true);
        if (--frames_left > 0) return;
        out += "SUMMARY: addr changes float_up float_down int_up int_down first -> last (as float) frames writer\n";
        std::vector<std::uint32_t> keys;
        for (const auto &[a, st] : stats) {
            if (st.changes < 8u) continue;
            const bool steady = (st.fup >= 8u && st.fdown * 8u <= st.fup) || (st.fdown >= 8u && st.fup * 8u <= st.fdown) ||
                                (st.iup >= 8u && st.idown * 8u <= st.iup) || (st.idown >= 8u && st.iup * 8u <= st.idown);
            if (steady) keys.push_back(a);
        }
        std::sort(keys.begin(), keys.end());
        for (const std::uint32_t a : keys) {
            const Stat &st = stats[a];
            char l[200];
            std::snprintf(l, sizeof l, "%08X n=%u fu=%u fd=%u iu=%u id=%u %08X -> %08X (%.6g -> %.6g) fr %u-%u pc=%08X ra=%08X\n", a, st.changes, st.fup, st.fdown, st.iup, st.idown,
                          st.first, st.last, static_cast<double>(std::bit_cast<float>(st.first)), static_cast<double>(std::bit_cast<float>(st.last)), st.first_frame, st.last_frame, st.pc, st.ra);
            out += l; ++lines;
        }
        stats.clear(); last_value.clear();
        frames_left = 1;   // fall through to the common end-of-trace code below
    }
    std::vector<std::pair<std::uint32_t, Field>> sorted(frame_fields.begin(), frame_fields.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto &x, const auto &y) { return x.first < y.first; });
    for (const auto &[a, f] : sorted) {
        auto it = last_value.find(a);
        if (it != last_value.end() && it->second == f.value) continue;
        last_value[a] = f.value;
        if (lines >= 4000000u) break;
        const std::int64_t rel = static_cast<std::int64_t>(a) - static_cast<std::int64_t>(body);
        std::snprintf(b, sizeof b, "W %c%05X %08X %.6g pc=%08X ra=%08X n=%u\n", rel < 0 ? '-' : '+', static_cast<unsigned>(rel < 0 ? -rel : rel), f.value,
                      static_cast<double>(std::bit_cast<float>(f.value)), f.pc, f.ra, f.count);
        out += b; ++lines;
    }
    frame_fields.clear();
    gates::profile_frame_end(true);
    if (--frames_left > 0) return;
    active = false;
    gates::profile_end("traces/cartrace_" + std::to_string(number) + "_calls.txt", {});
    psprecomp::set_write_watch_range(0u, 0u);
    rt.memory().set_read_watch_range(0u, 0u);
    arm_race_probe(rt);
    std::error_code ec;
    std::filesystem::create_directories("traces", ec);
    const std::string name = "traces/cartrace_" + std::to_string(number) + ".txt";
    write_file_async(name, std::vector<std::uint8_t>(out.begin(), out.end()));
    log_line(category::kHle, "CARTRACE written to " + name + " (" + std::to_string(lines) + " lines)");
    out.clear(); out.shrink_to_fit();
}
} // namespace cartrace

// ---- RAM diff trace (Home key): every word of user RAM that changes, frame by frame, for kDiffFrames frames -------------
// Binary file traces/difftrace_N.bin: "RRDT" magic, then per frame: u32 frame, u32 buttons, u32 lx|ly<<8, u32 count, then
// count x (u32 address, u32 new value).  Used to find frame counters (countdown, gauges, menu timers) and what they drive.
namespace difftrace {
constexpr int kDiffFrames = 300;
std::uint64_t first_frame = 0;
constexpr std::uint32_t kBase = 0x08800000u, kLen = 0x01800000u;
bool active = false;
int frames_left = 0, number = 0;
std::vector<std::uint32_t> prev;
std::vector<std::uint8_t> out;
void put32(std::uint32_t v) { const std::size_t n = out.size(); out.resize(n + 4u); std::memcpy(out.data() + n, &v, 4); }
void start(Runtime &rt) {
    if (active) return;
    const std::uint8_t *p = static_cast<const psprecomp::GuestMemory &>(rt.memory()).raw_pointer(kBase, kLen);
    if (p == nullptr) return;
    prev.resize(kLen / 4u);
    std::memcpy(prev.data(), p, kLen);
    out.clear(); out.reserve(64u << 20);
    out.insert(out.end(), {'R', 'R', 'D', 'T'});
    {
        // RR_DIFF_SECONDS=S records S seconds of game time (S * 60 * fps multiplier frames); default 300 frames.
        const char *sec = std::getenv("RR_DIFF_SECONDS");
        frames_left = sec ? std::max(1, static_cast<int>(std::atof(sec) * 60.0 * g_hle_options.fps_mult)) : kDiffFrames;
    }
    active = true; ++number;
    first_frame = 0;
    gates::profile_begin();
    log_line(category::kHle, "DIFFTRACE started");
}
void frame_end(Runtime &rt, std::uint64_t frame) {
    if (!active) return;
    const std::uint8_t *p = static_cast<const psprecomp::GuestMemory &>(rt.memory()).raw_pointer(kBase, kLen);
    if (p == nullptr) { active = false; return; }
    put32(static_cast<std::uint32_t>(frame)); put32(cartrace::pad_buttons); put32(cartrace::pad_lx | (cartrace::pad_ly << 8u));
    const std::size_t count_at = out.size(); put32(0u);
    std::uint32_t count = 0u;
    const std::uint32_t *cur = reinterpret_cast<const std::uint32_t *>(p);
    for (std::size_t i = 0; i < prev.size(); ++i) {
        std::uint32_t v; std::memcpy(&v, p + i * 4u, 4);
        if (v == prev[i]) continue;
        prev[i] = v;
        if (out.size() > (400u << 20)) continue;   // cap at ~400 MB
        put32(kBase + static_cast<std::uint32_t>(i * 4u)); put32(v); ++count;
    }
    (void)cur;
    std::memcpy(out.data() + count_at, &count, 4);
    if (first_frame == 0u) first_frame = frame;
    gates::profile_frame_end(!cartrace::active);
    if (--frames_left > 0) return;
    gates::profile_end("traces/difftrace_" + std::to_string(number) + "_calls.txt", {first_frame});
    active = false;
    std::error_code ec;
    std::filesystem::create_directories("traces", ec);
    const std::string name = "traces/difftrace_" + std::to_string(number) + ".bin";
    log_line(category::kHle, "DIFFTRACE written to " + name + " (" + std::to_string(out.size() >> 20) + " MB)");
    write_file_async(name, std::move(out));
    out = {};
    prev.clear(); prev.shrink_to_fit();
}
} // namespace difftrace

// ---- Call profile (Scroll Lock): which functions run each frame, with their callers, for RR_CALLS_FRAMES frames ------------
// (default 10 seconds of game time).  Writes traces/calls_N.txt and calls_N_edges.txt.  Slow while it runs: every function
// entry goes through the hook.
namespace callprof {
bool active = false;
int frames_left = 0, number = 0;
std::uint64_t first_frame = 0;
void start() {
    if (active || cartrace::active || difftrace::active) return;
    gates::profile_begin();
    if (!gates::profile) { log_line(category::kHle, "CALLS: code was not generated with --function-gates"); return; }
    const char *e = std::getenv("RR_CALLS_FRAMES");
    const int n = e ? std::atoi(e) : 0;
    frames_left = n > 0 ? std::min(n, 6000) : static_cast<int>(600.0 * std::max(1.0, g_hle_options.fps_mult));
    active = true; ++number; first_frame = 0;
    log_line(category::kHle, "CALLS profile started for " + std::to_string(frames_left) + " frames");
}
void frame_end(std::uint64_t frame) {
    if (!active) return;
    if (first_frame == 0u) first_frame = frame;
    gates::profile_frame_end(true);
    if (--frames_left > 0) return;
    active = false;
    gates::profile_end("traces/calls_x" + std::to_string(static_cast<int>(g_hle_options.fps_mult * 100.0)) + "_f" + std::to_string(first_frame) + ".txt", {first_frame});
}
} // namespace callprof

// ---- Raised frame rate during races only (--fps-mult with --fps-patch car) ----------------------------------------------
// Menus, the loading-screen mini game and other screens count frames, so they stay at the PSP's 59.94 Hz.  The game is "in
// a race" while its car steering code runs: that code reads one constant (0bd234's 1/60 at 1acc44) every frame, so a read
// watch on that word tells us.  The vblank rate switches up while it is being read and back down 20 frames after it stops.
constexpr std::uint32_t kRaceProbe = 0x08804000u + 0x1acc44u;
std::uint64_t g_race_probe_frame = 0;
bool g_fast_rate = false;
std::uint64_t g_go_frame = 0;   // first presented frame of the current race with the player car moving (0 = not yet)
std::uint64_t g_base_vblank_us = 16683u, g_fast_vblank_us = 16683u;
bool g_rate_for_race = true;   // the raised rate is for a race (false: --menu-native menus)

void hle_watch_sink(bool is_write, std::uint32_t address, std::size_t length, std::uint64_t old_value, std::uint64_t new_value) {
    if (is_write && g_ctrl_hi != 0u && (address & 0x1FFFFFFFu) >= (g_ctrl_lo & 0x1FFFFFFFu) && (address & 0x1FFFFFFFu) < (g_ctrl_hi & 0x1FFFFFFFu)) {
        if (g_ctrl_writers.size() < 4000u)
            ++g_ctrl_writers[{psprecomp::runtime_dispatch_pc(), psprecomp::runtime_watch_register(31), (address & 0x1FFFFFFFu) - (g_ctrl_lo & 0x1FFFFFFFu)}];
    }
    {   // RR_WATCH_W=addr,len (hex): log who writes that range (first 80 writes)
        static std::uint32_t lo = 0u, hi = 0u; static bool init = false; static int lines = 0;
        if (!init) { init = true; if (const char *e = std::getenv("RR_WATCH_W")) { unsigned a = 0, l = 4; if (std::sscanf(e, "%x,%x", &a, &l) >= 1) { lo = a & 0x1FFFFFFFu; hi = lo + l; } } }
        static const std::uint32_t from = [] { const char *e = std::getenv("RR_WATCH_FROM"); return e ? static_cast<std::uint32_t>(std::atoi(e)) : 0u; }();
        if (is_write && hi != 0u && (address & 0x1FFFFFFFu) >= lo && (address & 0x1FFFFFFFu) < hi && lines < 80 && gates::game_step() >= from) {
            ++lines;
            log_line(category::kHle, "WWRITE addr=" + hex32(address) + " old=" + hex32(static_cast<std::uint32_t>(old_value)) + " new=" + hex32(static_cast<std::uint32_t>(new_value)) +
                                         " pc=" + hex32(psprecomp::runtime_dispatch_pc()) + " ra=" + hex32(psprecomp::runtime_watch_register(31)) + " step=" + std::to_string(gates::game_step()) + " val=" + std::to_string(std::bit_cast<float>(static_cast<std::uint32_t>(new_value))));
        }
    }
    {   // RR_WATCH_STATS=addr,len (hex) with RR_WATCH_FROM / RR_WATCH_TO steps: per (ra, offset) count writes and how many changed the
        // value; written to the log at step RR_WATCH_TO.  Comparing 60 and 144 fps finds writers that only change things at 144.
        static std::uint32_t lo = 0u, hi = 0u, from = 0u, to = 0u; static bool init = false, dumped = false;
        struct C { std::uint32_t n = 0, changed = 0, tick_changed = 0; };
        static std::map<std::pair<std::uint32_t, std::uint32_t>, C> stats;
        if (!init) {
            init = true;
            if (const char *e = std::getenv("RR_WATCH_STATS")) { unsigned a = 0, l = 4; if (std::sscanf(e, "%x,%x", &a, &l) >= 1) { lo = a & 0x1FFFFFFFu; hi = lo + l; } }
            if (const char *e = std::getenv("RR_WATCH_FROM")) from = static_cast<std::uint32_t>(std::atoi(e));
            if (const char *e = std::getenv("RR_WATCH_TO")) to = static_cast<std::uint32_t>(std::atoi(e));
        }
        const std::uint32_t a = address & 0x1FFFFFFFu, st = gates::game_step();
        if (is_write && hi != 0u && a >= lo && a < hi && st >= from && st < to) {
            C &c = stats[{psprecomp::runtime_watch_register(31), (a - lo) & ~3u}];
            ++c.n;
            if (old_value != new_value) { ++c.changed; if (gates::race_tick(false)) ++c.tick_changed; }
        }
        if (hi != 0u && !dumped && st >= to && to != 0u) {
            dumped = true;
            for (const auto &kv : stats) {
                char b[160]; std::snprintf(b, sizeof b, "WSTAT ra=%08X off=%04X n=%u changed=%u tick_changed=%u", kv.first.first, kv.first.second, kv.second.n, kv.second.changed, kv.second.tick_changed);
                log_line(category::kHle, b);
            }
        }
    }
    if (!is_write && std::getenv("RR_WATCH_R") != nullptr) {
        static std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint64_t> seen;
        const std::uint32_t pc = psprecomp::runtime_dispatch_pc(), ra = psprecomp::runtime_watch_register(31);
        const std::uint64_t n = ++seen[{pc, ra}];
        if (n == 1u || n == 10u || n == 100u || n == 1000u || n == 10000u || n == 100000u || n == 1000000u)
            log_line(category::kHle, "WREAD n=" + std::to_string(n) + " pc=" + hex32(pc) + " ra=" + hex32(ra) + " addr=" + hex32(address) + " len=" + std::to_string(length) +
                                         " val=" + hex32(static_cast<std::uint32_t>(old_value)) + " step=" + std::to_string(gates::game_step()) + " frame=" + std::to_string(g_frames_presented));
        return;
    }
    if (!is_write && (address & 0x1FFFFFFFu) == (kRaceProbe & 0x1FFFFFFFu)) { g_race_probe_frame = g_frames_presented; return; }
    if (is_write && new_value != 0u && g_mail_watch_addr != 0u && (address & 0x1FFFFFFFu) == g_mail_watch_addr) {
        static std::uint32_t lines = 0;
        if (lines++ < 400u)
            log_line(category::kHle, "MAILPOST step=" + std::to_string(gates::game_step()) + " frame=" + std::to_string(g_frames_presented) +
                                         " pc=" + hex32(psprecomp::runtime_dispatch_pc()) + " ra=" + hex32(psprecomp::runtime_watch_register(31)) +
                                         " old=" + std::to_string(old_value) + " new=" + std::to_string(new_value));
        if (lines <= 3u && g_dump_rt != nullptr) {
            auto &mm = g_dump_rt->memory();
            std::string r = "MAILPOST regs";
            for (std::uint32_t n : {4u, 5u, 6u, 7u, 16u, 17u, 18u, 29u}) r += " r" + std::to_string(n) + "=" + hex32(psprecomp::runtime_watch_register(n));
            const std::uint32_t s0 = psprecomp::runtime_watch_register(16);
            if (s0 >= 0x08800000u && s0 < 0x0a000000u) {
                const std::uint32_t idx = mm.load32(s0 + 0x58u);
                const std::uint32_t hi = (mm.load32(0x08842D9Cu) & 0xFFFFu) << 16u;
                const std::uint32_t lo = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(mm.load32(0x08842DA0u) & 0xFFFFu)));
                const std::uint32_t tbl = hi + lo + idx * 8u;
                r += " s0idx=" + std::to_string(idx) + " tbl=" + hex32(tbl);
                if (idx < 4096u) {
                    const std::uint32_t w0 = mm.load32(tbl), w1 = mm.load32(tbl + 4u);
                    r += " w0=" + hex32(w0) + " w1=" + hex32(w1);
                    if ((w1 & 1u) == 0u) r += " fn=" + hex32(w0);
                    else { const std::uint32_t vp = mm.load32(s0 + (static_cast<std::int32_t>(w1) >> 1)); r += " vptr=" + hex32(vp) + " fn=" + hex32(mm.load32(vp + w0)); }
                }
            }
            log_line(category::kHle, r);
        }
    }
    cartrace::sink(is_write, address, length, old_value, new_value);
}

// Race-scene states that get the raised rate: all of them unless RR_FAST_STATES lists some ("1,2"), "none" = steering only.
bool scene_state_fast(std::uint32_t st) {
    static const std::vector<std::uint32_t> states = [] {
        std::vector<std::uint32_t> v;
        const char *e = std::getenv("RR_FAST_STATES");
        if (e == nullptr) return v;
        std::string s(e);
        if (s == "none") { v.push_back(0xFFFFFFFEu); return v; }
        for (std::size_t p = 0; p < s.size();) {
            const std::size_t c = s.find(',', p);
            const std::string part = s.substr(p, c == std::string::npos ? std::string::npos : c - p);
            if (!part.empty()) v.push_back(static_cast<std::uint32_t>(std::strtoul(part.c_str(), nullptr, 0)));
            if (c == std::string::npos) break;
            p = c + 1;
        }
        return v;
    }();
    return states.empty() || std::find(states.begin(), states.end(), st) != states.end();
}
bool race_only_rate() { return g_hle_options.fps_mult != 1.0 && g_hle_options.fps_race_only; }
bool gates::fast_rate_active() { return g_hle_options.fps_mult != 1.0 && (!race_only_rate() || g_fast_rate); }

void arm_race_probe(Runtime &rt) {
    if (const char *e = std::getenv("RR_WATCH_W")) {
        unsigned wa = 0, wl = 4;
        if (std::sscanf(e, "%x,%x", &wa, &wl) >= 1) { psprecomp::set_watch_sink(&hle_watch_sink); psprecomp::set_write_watch_range(wa, wl); }
    }
    if (const char *e = std::getenv("RR_WATCH_R")) {
        // RR_WATCH_R=addr,len (hex): log which code reads that range (replaces the race probe watch; race rate still follows the scene)
        unsigned wa = 0, wl = 4;
        if (std::sscanf(e, "%x,%x", &wa, &wl) >= 1) { psprecomp::set_watch_sink(&hle_watch_sink); rt.memory().set_read_watch_range(wa, wl); return; }
    }
    if (!race_only_rate()) return;
    psprecomp::set_watch_sink(&hle_watch_sink);
    rt.memory().set_read_watch_range(kRaceProbe, 4u);
}

void update_race_rate() {
    if (!race_only_rate() || cartrace::active) return;   // (the car trace borrows the read watch)
    // In a race while the steering code reads its constant, or while the race scene runs (load, track intro, race).
    const bool in_race = (g_race_probe_frame != 0u && g_frames_presented - g_race_probe_frame < 20u) ||
                         (gates::scene_frame != 0u && g_frames_presented - gates::scene_frame < 20u && scene_state_fast(gates::scene_state));
    // --menu-native: the menus' UI engine ran in the last few frames (see gates::ui)
    const bool menus = g_hle_options.menu_native && gates::ui::last_frame != 0u && g_frames_presented - gates::ui::last_frame < 8u;
    const bool race = in_race || menus;
    g_rate_for_race = in_race;
    if (race == g_fast_rate) return;
    g_fast_rate = race;
    set_vblank_period(race ? g_fast_vblank_us : g_base_vblank_us);
    log_line(category::kHle, std::string("FPSMULT ") + (race ? (in_race ? "race started: raised rate" : "menus: raised rate") : "race over: back to 59.94 Hz") +
                                 " at frame " + std::to_string(g_frames_presented) + " (game time " + std::to_string(static_cast<long long>(g_ctrl_game60)) + " pad reads at 59.94 Hz)");
}

// Performance report (--perf-report FILE; on by default for players via the settings file).  Every presented frame adds its
// timings to small histograms, separately for "race" frames and everything else (menus, loading); the report file is
// rewritten about every half minute and at exit, so a crash or a killed window still leaves a report.
namespace perfrep {
std::filesystem::path path;   // empty = off
struct Hist {
    static constexpr int kBins = 800;   // 0.25 ms per bin, up to 200 ms
    std::uint64_t bin[kBins + 1]{};
    std::uint64_t n = 0;
    double sum = 0.0, max = 0.0;
    void add(double ms) {
        if (ms < 0.0) ms = 0.0;
        ++n; sum += ms;
        if (ms > max) max = ms;
        const int b = static_cast<int>(ms * 4.0);
        ++bin[b >= kBins ? kBins : b];
    }
    double avg() const { return n != 0u ? sum / static_cast<double>(n) : 0.0; }
    double pct(double q) const {
        if (n == 0u) return 0.0;
        const std::uint64_t want = static_cast<std::uint64_t>(std::ceil(q * static_cast<double>(n)));
        std::uint64_t acc = 0;
        for (int i = 0; i <= kBins; ++i) { acc += bin[i]; if (acc >= want) return i == kBins ? max : (i + 0.5) * 0.25; }
        return max;
    }
};
struct Group {
    Hist interval, work, present, draw;
    std::uint64_t slow = 0, over33 = 0;   // frames slower than 1.25 x the target, and slower than 33.5 ms
    double target_ms = 16.683;
    // Game speed: the game advances one target period of game time per presented frame, so over a second of real time the game ran at
    // (sum of periods) / (real time) of full speed.  Seconds are counted in blocks of real time >= 1000 ms.
    double win_wall = 0.0, win_game = 0.0, total_wall = 0.0, total_game = 0.0, speed_min = 1e9, speed_max = 0.0, speed_min_at = 0.0, speed_max_at = 0.0;
    std::uint64_t secs = 0, secs_slow = 0, secs_fast = 0;
};
Group race, other;
// A frame's cycle: [block point] -> present tail (blit, swap, pacing sleep / vsync wait) -> the game runs the next frame ->
// next block point.  So the time between block points = the previous frame's tail + the next frame's real work.
std::chrono::steady_clock::time_point block_t;
bool have_block = false;
double tail_prev = 0.0;
void note_block(std::chrono::steady_clock::time_point t) { block_t = t; have_block = true; }
void end_tail() { if (have_block) { tail_prev = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - block_t).count(); have_block = false; } }
void record(bool in_race, double interval, double /*unused*/, double /*unused*/, double draw, double target) {
    Group &g = in_race ? race : other;
    g.target_ms = target;
    const double tail = std::min(tail_prev, interval);
    g.interval.add(interval); g.work.add(interval - tail); g.present.add(tail); g.draw.add(draw);
    if (interval > target * 1.25) ++g.slow;
    if (interval > 33.5) ++g.over33;
    g.win_wall += interval; g.win_game += target; g.total_wall += interval; g.total_game += target;
    if (g.win_wall >= 1000.0) {
        const double sp = g.win_game / g.win_wall;
        ++g.secs;
        if (sp < 0.95) ++g.secs_slow;
        if (sp > 1.03) ++g.secs_fast;
        if (sp < g.speed_min) { g.speed_min = sp; g.speed_min_at = g.total_wall / 1000.0; }
        if (sp > g.speed_max) { g.speed_max = sp; g.speed_max_at = g.total_wall / 1000.0; }
        g.win_wall = 0.0; g.win_game = 0.0;
    }
}
void describe(std::ostringstream &o, const char *title, const Group &g) {
    char b[400];
    if (g.interval.n == 0u) { o << title << ": no frames\n\n"; return; }
    const double secs = g.interval.sum / 1000.0;
    std::snprintf(b, sizeof b, "%s: %llu frames over %.0f s, target %.1f fps (%.1f ms per frame)\n", title, static_cast<unsigned long long>(g.interval.n), secs, 1000.0 / g.target_ms, g.target_ms);
    o << b;
    std::snprintf(b, sizeof b, "  achieved:   %.1f fps average;  frames slower than 1.25x target: %llu (%.1f%%);  slower than 33 ms: %llu (%.1f%%)\n",
                  1000.0 / std::max(g.interval.avg(), 0.001), static_cast<unsigned long long>(g.slow), 100.0 * static_cast<double>(g.slow) / static_cast<double>(g.interval.n),
                  static_cast<unsigned long long>(g.over33), 100.0 * static_cast<double>(g.over33) / static_cast<double>(g.interval.n));
    o << b;
    const auto row = [&](const char *name, const Hist &h) {
        std::snprintf(b, sizeof b, "  %-11s avg %6.2f ms   median %6.2f   95%% %6.2f   99%% %6.2f   worst %7.2f\n", name, h.avg(), h.pct(0.5), h.pct(0.95), h.pct(0.99), h.max);
        o << b;
    };
    row("frame time", g.interval);
    row("CPU work", g.work);
    row("present", g.present);
    row("drawing", g.draw);
    if (g_hle_options.throttle && g.secs >= 3u && g.total_wall > 0.0) {
        // Is the game running at its proper speed?  (Below 100 % = slow motion because the PC cannot keep up; above 100 % would be a bug.)
        std::snprintf(b, sizeof b, "  game speed: %.1f%% of full speed on average;  slowest second %.0f%% (at %.0f s);  fastest second %.0f%% (at %.0f s);  seconds below 95%%: %llu of %llu;  above 103%%: %llu\n",
                      100.0 * g.total_game / g.total_wall, 100.0 * g.speed_min, g.speed_min_at, 100.0 * g.speed_max, g.speed_max_at,
                      static_cast<unsigned long long>(g.secs_slow), static_cast<unsigned long long>(g.secs), static_cast<unsigned long long>(g.secs_fast));
        o << b;
    }
    // Plain-language reading: only meaningful when the frame rate is short of the target.
    if (!g_hle_options.throttle) {
        // Uncapped run: nothing waits for the display, so the achieved rate is the most this PC can do with these settings.
        const double fps = 1000.0 / std::max(g.interval.avg(), 0.001), need = 1000.0 / g.target_ms;
        char u[400];
        std::snprintf(u, sizeof u, "  -> Uncapped: %.0f fps at most = %.1fx the %.0f fps needed (%.0f fps at the 5%% slowest frames). ", fps, fps / need, need, 1000.0 / std::max(g.interval.pct(0.95), 0.001));
        o << u;
        if (g.work.avg() >= g.present.avg()) o << "The limit is the CPU (game code and draw preparation): lower settings will not help much, a faster CPU or a faster build would.\n";
        else o << "The limit is the graphics chip (or memory bandwidth): lower render scale / anti-aliasing will raise the rate.\n";
        if (fps < need) o << "  -> This PC cannot reach full speed with these settings.\n";
        else if (fps < need * 1.3) o << "  -> Full speed, but with little headroom: expect stutter in heavier scenes.\n";
    } else if (g.interval.avg() > g.target_ms * 1.08) {
        if (g.work.avg() > g.target_ms * 0.8) o << "  -> Too slow, and the CPU is the limit: running the game and preparing the frame takes " << static_cast<int>(g.work.avg() + 0.5) << " ms of a " << static_cast<int>(g.target_ms + 0.5) << " ms budget.\n";
        else if (g.present.avg() > g.interval.avg() * 0.45) o << "  -> Too slow, and the time goes into presenting the frame (waiting for the graphics chip or the display): lower the render scale / anti-aliasing.\n";
        else o << "  -> Too slow, with the time spread between CPU work and presenting.\n";
    } else if (g.slow * 20u > g.interval.n) {
        o << "  -> Average is fine but more than 5% of frames are slow: this is stutter, not low speed.\n";
    } else {
        o << "  -> Holds the target.\n";
    }
    o << "\n";
}
void write_report() {
    if (path.empty()) return;
    std::ostringstream o;
    o << "Ridge Racer PC port - performance report\n";
    o << "port build: " << __DATE__ << " " << __TIME__ << "\n";
    o << system_description() << "\n";
    if (!g_hle_options.throttle) o << "Mode: UNCAPPED (frame pacing and vsync off; measures the most this PC can do)\n";
    if (!g_gl) {
        o << "Graphics: software renderer (CPU)";
        if (!g_gpu_status.empty()) o << "  <-- OpenGL was requested but NOT used: " << g_gpu_status;
        o << "\n";
        if (!gl_description().empty()) o << "OpenGL reported: " << gl_description() << "\n";
    } else {
        o << "Graphics: " << gl_description() << "\n";
    }
    if (g_hle_options.frame_skip > 1) o << "Frame skip: 1 picture in " << g_hle_options.frame_skip << " is drawn during races\n";
    if (g_hle_options.frame_skip == 0) o << "Frame skip: automatic; " << g_auto_skipped << " of " << g_auto_race_frames << " race pictures were skipped to keep the game at full speed\n";
    char b[300];
    std::snprintf(b, sizeof b, "Settings: %s, internal scale x%d, MSAA %d, aniso %d, frame rate x%.2f, low latency %s, %s\n", g_hle_options.gpu ? "OpenGL" : "software", g_hle_options.internal_scale,
                  g_hle_options.msaa, g_hle_options.aniso, g_hle_options.fps_mult, g_hle_options.low_latency ? "on" : "off", g_hle_options.fullscreen ? "fullscreen" : "windowed");
    o << b << "\n";
    describe(o, "RACE (race scene incl. countdown)", race);
    describe(o, "OTHER (menus, loading, movies)", other);
    o << "frame time = time between two presented frames. CPU work = the game running one frame (game code + drawing prep), without the waiting.\n"
         "present = handing the frame to the display, including the waits for the screen refresh and the GPU (a PC that is ahead of the target spends its spare time here). drawing = part of CPU work spent turning the game's drawing commands into pictures (or GPU work).\n";
    std::ofstream f(path, std::ios::trunc);
    if (f) f << o.str();
}
} // namespace perfrep

std::uint64_t g_interp_pictures = 0;   // menu in-between pictures shown (window title fps)
void present_display_frame(Runtime &rt) {
    if (g_display.frame_top == 0u) return;
    ++g_frames_presented;
    if (!g_hle_options.window) {   // RR_REALTIME=1 (headless netplay tests): keep the game's clock in step with the real one
        static const bool pace = std::getenv("RR_REALTIME") != nullptr;
        // RR_NO_DRAW=N (N > 1, with --screenshots DIR N): draw only the frames around every Nth one, so screenshots still work
        static const long long nd = [] { const char *e = std::getenv("RR_NO_DRAW"); return e ? std::atoll(e) : 0ll; }();
        if (nd > 1) g_no_draw = static_cast<long long>(g_frames_presented % static_cast<std::uint64_t>(nd)) < nd - 2;
        if (pace) {
            static const auto t0 = std::chrono::steady_clock::now();
            static const std::uint64_t v0 = g_virtual_time_us;
            const auto due = t0 + std::chrono::microseconds(g_virtual_time_us - v0);
            if (due > std::chrono::steady_clock::now()) std::this_thread::sleep_until(due);
        }
    }
    gates::ui::restore(rt);   // (blended menu values only live while the frame is drawn)
    { static const bool stretch_once = (audio_set_time_stretch(g_hle_options.frame_skip != 0), true); (void)stretch_once; }   // (auto frame skip: sound never slows)
    {   // Game catch-up steps (more than one game step per presented frame): logged every 1200 frames when any happened.
        static std::uint32_t last_step = 0; static std::uint64_t extra = 0, frames = 0;
        const std::uint32_t st = gates::game_step();
        if (last_step != 0u && st > last_step + 1u && st - last_step < 100u) extra += st - last_step - 1u;
        last_step = st;
        if (++frames >= 1200u) {
            if (extra != 0u) log_line(category::kHle, "CATCHUP game steps without a picture: " + std::to_string(extra) + " in the last 1200 frames (frame " + std::to_string(g_frames_presented) + ")");
            frames = 0; extra = 0;
        }
    }
    update_race_rate();
    // Which button pictures to show (controller kind used last, or the keyboard): fonts, controller screen, our pages.
    rr::pad::frame(present_pad_buttons(), present_gamepad_buttons(), present_pad_stick_active(), static_cast<rr::pad::Family>(present_pad_family()));
    lobby_frame();   // (server browser / room setup page, when open; every frame, also without a window: tests)
    {   // RR_TEST_STALL="frame,ms" (netplay tests): this copy freezes for ms at that frame, like a slow PC loading a course
        static const std::pair<unsigned long long, int> stall = [] { unsigned long long f = 0; int ms = 0; if (const char *e = std::getenv("RR_TEST_STALL")) std::sscanf(e, "%llu,%d", &f, &ms); return std::pair<unsigned long long, int>{f, ms}; }();
        if (stall.second > 0 && g_frames_presented == stall.first) { log_line(category::kHle, "TEST stall " + std::to_string(stall.second) + " ms"); std::this_thread::sleep_for(std::chrono::milliseconds(stall.second)); }
    }
    const bool skipped_frame = g_skip_draws;   // this frame's drawing was skipped: do not show it
    {   // RR_LOAD_MS=n (rig): pretend drawing a picture costs n ms of CPU (a slow PC), to test the automatic frame skip
        static const double load_ms = [] { const char *e = std::getenv("RR_LOAD_MS"); return e ? std::atof(e) : 0.0; }();
        if (load_ms > 0.0 && !skipped_frame && gates::scene_frame != 0u && g_frames_presented - gates::scene_frame < 5u) {
            const auto t_end = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double, std::milli>(load_ms));
            while (std::chrono::steady_clock::now() < t_end) {}
        }
    }
    // A race with other players: every copy runs its own race clock (the game does not resynchronize them after the start), so
    // a copy that falls behind real time (a stall, a busy or hidden browser window) would stay behind for the rest of the race
    // and the others would see its car in the wrong place.  So in a networked race the catch-up below is always on, and it
    // makes up as much as 10 s (single player keeps losing anything beyond 100 ms instead of fast-forwarding).
    {
        static bool was_net = false;
        net::Node *nn = net::shared_node();
        g_net_race_sync = nn != nullptr && nn->in_session() && gates::scene_frame != 0u && g_frames_presented - gates::scene_frame < 5u &&
                          std::getenv("RR_NET_NO_CATCHUP") == nullptr;
        if (g_net_race_sync != was_net) { was_net = g_net_race_sync; if (g_net_race_sync) log_line(category::kHle, "networked race: catch-up on (the race clock stays in step with real time)"); }
    }
    if (g_hle_options.frame_skip == 0 || g_net_race_sync) {
        // Automatic frame skip: the game should run at full speed whatever the PC manages.  An ideal timeline (one virtual vblank period per
        // frame) is kept for the race; when the real clock is more than 2.5 periods behind it, the next pictures are not drawn (the game
        // logic and the drawing-command decoding still run, only pixels are skipped) and no pacing wait is done, so the game catches up.
        // At most 5 pictures in a row are skipped; if still more than 0.6 s behind the timeline restarts (slow motion, as before).
        static std::chrono::steady_clock::time_point ideal;
        static bool have_ideal = false;
        static int run = 0;
        const bool race = gates::scene_frame != 0u && g_frames_presented - gates::scene_frame < 5u;
        if (!race) { have_ideal = false; run = 0; g_skip_draws = false; }
        else {
            ++g_auto_race_frames;
            const auto now = std::chrono::steady_clock::now();
            const auto period = std::chrono::microseconds(static_cast<long long>(g_vblank_period_us));
            if (!have_ideal) { ideal = now; have_ideal = true; }
            else ideal += period;
            const double p_ms = static_cast<double>(g_vblank_period_us) / 1000.0;
            const double lag_ms = std::chrono::duration<double, std::milli>(now - ideal).count();
            // Further behind than this: the lag is not made up (that would be a fast-forward the player sees and hears); the game just
            // lost that moment.  RR_AUTOSKIP_MAX_LAG_MS overrides (the old value was 600).
            static const double max_lag_solo = [] { const char *e = std::getenv("RR_AUTOSKIP_MAX_LAG_MS"); return e ? std::atof(e) : 100.0; }();
            const double max_lag = g_net_race_sync ? 10000.0 : max_lag_solo;
            if (lag_ms > max_lag) { ideal = now; run = 0; g_skip_draws = false; }
            else {
                if (lag_ms < -2.0 * p_ms) ideal = now - period;   // ahead of the timeline (the pacing waits): no credit to spend later
                // Catching up plays the sound faster than real time too (it follows the game), so only a real lag is caught up, not a hiccup:
                // the sound queue absorbs a lag of this size as a little extra latency without any audible speed-up.
                static const double lag_limit = [] { const char *e = std::getenv("RR_AUTOSKIP_LAG_MS"); return e ? std::atof(e) : -1.0; }();
                static const int run_limit = [] { const char *e = std::getenv("RR_AUTOSKIP_RUN"); return e ? std::max(1, std::atoi(e)) : 5; }();
                if (lag_ms > (lag_limit >= 0.0 ? std::max(lag_limit, 1.5 * p_ms) : std::max(2.5 * p_ms, 60.0)) && run < (g_net_race_sync ? 15 : run_limit)) {
                    g_skip_draws = true; ++run; ++g_auto_skipped;
                    if (g_net_race_sync && lag_ms > 500.0) {   // (diagnostic: a real lag being made up)
                        static std::uint64_t last_log = 0;
                        if (g_frames_presented - last_log > 300u) { last_log = g_frames_presented; log_line(category::kHle, "networked race: catching up " + std::to_string(static_cast<int>(lag_ms)) + " ms"); }
                    }
                }
                else { g_skip_draws = false; run = 0; }
            }
        }
    } else if (g_hle_options.frame_skip > 1) {
        static const bool always = std::getenv("RR_FRAMESKIP_ALWAYS") != nullptr;   // test: skip in menus too
        const bool race = always || (gates::scene_frame != 0u && g_frames_presented - gates::scene_frame < 5u);
        g_skip_draws = race && (g_frames_presented % static_cast<std::uint64_t>(g_hle_options.frame_skip)) != 0u;   // the frame that starts now
    }
    if (g_gl) {
        g_gl->end_frame();
        for (const std::string &line : g_gl->take_events()) log_line(category::kGe, line);
    }
    if (g_ge) {
        g_ge->set_frame(g_frames_presented);
        static const bool full_skip_off = std::getenv("RR_SKIP_TRANSFORM_OFF") != nullptr;
        g_ge->set_skip_prims(g_skip_draws && !full_skip_off);
    }
    if (g_hle_options.stop_after_frame != 0u && g_frames_presented >= g_hle_options.stop_after_frame)
        rt.stop("stop-after-frame reached");
    // Race-relative test triggers (test harness): GO = first frame the player's car moves (the race camera latch).
    //   RR_GO_STOP=N   quit N frames after GO          RR_GO_DIFF=N   start the RAM diff trace N frames after GO
    //   RR_GO_CALLS=N  start the call profile N frames after GO       RR_GO_SHOTS=start,count  save those frames (needs --shot-dir)
    {
        // GO = the player's speed word (HUD car +0xc0) passes 10 (same test as the race camera latch, but independent of the
        // fps gates so it also works at 60 fps).  Cleared when the HUD car goes away (menus, loading).
        {
            auto &gm = rt.memory();
            const std::uint32_t hud = gm.load32(0x08a448f0u);
            const std::uint32_t pcar = (hud >= 0x08800000u && hud < 0x0a000000u) ? gm.load32(hud + 0xc4u) : 0u;
            const bool have_car = pcar >= 0x08800000u && pcar < 0x0a000000u;
            const bool moving = have_car && gates::idle_seen && std::fabs(std::bit_cast<float>(gm.load32(pcar + 0xc0u))) > 10.0f;
            // The speed word can read > 10 for a frame or two while a scene loads (a black frame passed for GO at 144+ fps), so GO needs
            // 30 consecutive moving frames; it is reported at the first of them.
            static std::uint32_t move_run = 0u, move_first = 0u;
            if (!have_car) g_go_frame = 0u;
            if (!moving) move_run = 0u;
            else {
                if (move_run++ == 0u) move_first = g_frames_presented;
                if (move_run >= 30u && g_go_frame == 0u) { g_go_frame = move_first; log_line(category::kHle, "GO at frame " + std::to_string(g_go_frame) + " step " + std::to_string(gates::game_step()) + " (game time " + std::to_string(static_cast<long long>(g_ctrl_game60)) + " pad reads at 59.94 Hz)"); }
            }
        }
        static const long long go_stop = [] { const char *e = std::getenv("RR_GO_STOP"); return e ? std::atoll(e) : -1ll; }();
        static const long long go_diff = [] { const char *e = std::getenv("RR_GO_DIFF"); return e ? std::atoll(e) : -1ll; }();
        static const long long go_calls = [] { const char *e = std::getenv("RR_GO_CALLS"); return e ? std::atoll(e) : -1ll; }();
        if (g_go_frame != 0u) {
            const long long since = static_cast<long long>(g_frames_presented - g_go_frame);
            if (go_stop >= 0 && since >= go_stop) rt.stop("RR_GO_STOP reached");
            if (go_diff >= 0 && since == go_diff && !difftrace::active) difftrace::start(rt);
            if (go_calls >= 0 && since == go_calls) callprof::start();
        }
    }
    {   // RR_CALLS_AT_FRAME=N (rig): start the call profile at presented frame N (absolute), independent of GO.
        static const long long at_frame = [] { const char *e = std::getenv("RR_CALLS_AT_FRAME"); return e ? std::atoll(e) : -1ll; }();
        if (at_frame >= 0 && static_cast<long long>(g_frames_presented) == at_frame) callprof::start();
    }
    g_import_snapshot = g_import_calls;
    // Purple detector: save a screenshot whenever a presented frame is noticeably purple.
    static std::uint64_t purple_last_shot = 0u;
    static std::uint32_t purple_shots = 0u;
    bool purple_shot = false;
    std::uint64_t purple_px = g_ge ? g_ge->take_frame_purple() : 0u;
    if (purple_px >= 2500u) {
        log_line(category::kGe, "PURPLEFRAME frame=" + std::to_string(g_frames_presented) + " pixels=" + std::to_string(purple_px));
        if (!g_hle_options.screenshot_dir.empty() && purple_shots < 12u && g_frames_presented >= purple_last_shot + 120u) {
            purple_shot = true; purple_last_shot = g_frames_presented; ++purple_shots;
        }
    }
    // F12: save the current frame and trace the next frame's GE commands (for diagnosing rendering glitches).
    // RR_TRACE_AT=N: start the car trace N presented frames after the race handler of a race with opponents begins (the countdown).
    {
        static std::uint32_t auto_obj = 0; static std::uint64_t auto_frame = 0; static bool auto_armed = false;
        static const char *at = std::getenv("RR_TRACE_AT");
        if (at != nullptr) {
            if (gates::opp_mode_obj != 0u && gates::opp_mode_obj != auto_obj) { auto_obj = gates::opp_mode_obj; auto_frame = g_frames_presented + static_cast<std::uint64_t>(std::max(0, std::atoi(at))); auto_armed = true; }
            if (auto_armed && g_frames_presented >= auto_frame && !cartrace::active) { auto_armed = false; cartrace::start(rt); }
        }
    }
    // RR_DIFF_AT=N: the same for the RAM diff trace (Home key).
    {
        static std::uint32_t d_obj = 0; static std::uint64_t d_frame = 0; static bool d_armed = false;
        static const char *at = std::getenv("RR_DIFF_AT");
        if (at != nullptr) {
            if (gates::opp_mode_obj != 0u && gates::opp_mode_obj != d_obj) { d_obj = gates::opp_mode_obj; d_frame = g_frames_presented + static_cast<std::uint64_t>(std::max(0, std::atoi(at))); d_armed = true; }
            if (d_armed && g_frames_presented >= d_frame && !difftrace::active) { d_armed = false; difftrace::start(rt); }
        }
    }
    // RR_TRACE_FRAME=N: start the write trace (needs --trace-tools; RR_TRACE_RANGE, RR_TRACE_FRAMES) at absolute presented frame N.
    {
        static const char *tf = std::getenv("RR_TRACE_FRAME");
        static bool tf_done = false;
        if (tf != nullptr && !tf_done && !cartrace::active && g_frames_presented >= std::strtoull(tf, nullptr, 10)) { tf_done = true; cartrace::start(rt); }
    }
    // RR_DIFF_FRAME=N: start the RAM diff trace (and call profile) at absolute presented frame N (headless rig).
    {
        static const char *df = std::getenv("RR_DIFF_FRAME");
        static bool df_done = false;
        if (df != nullptr && !df_done && !difftrace::active && g_frames_presented >= std::strtoull(df, nullptr, 10)) { df_done = true; difftrace::start(rt); }
    }
    // RR_TRACE_DIST=N: start the car trace when the player's course distance (float at car +0x860) passes N upward.
    {
        static const char *td = std::getenv("RR_TRACE_DIST");
        static float last_dist = -1.0f; static bool done = false;
        if (td != nullptr && !done && !cartrace::active) {
            auto &m = rt.memory();
            const std::uint32_t hud = m.load32(0x08a448f0u);
            const std::uint32_t car = (hud >= 0x08800000u && hud < 0x0a000000u) ? m.load32(hud + 0xc4u) : 0u;
            if (car >= 0x08800000u && car < 0x0a000000u) {
                const float d = std::bit_cast<float>(m.load32(car + 0x860u)), n = static_cast<float>(std::atof(td));
                if (last_dist >= 0.0f && last_dist < n && d >= n && d - last_dist < 100.0f) { done = true; cartrace::start(rt); }
                last_dist = d;
            }
        }
    }
    // RR_DIFF_DIST=N: start the RAM diff trace when the player's car passes course distance N (car +0x860, a float, upward).
    {
        static const char *dd = std::getenv("RR_DIFF_DIST");
        static float last_dist = -1.0f; static bool dist_done = false;
        if (dd != nullptr && !dist_done && !difftrace::active) {
            auto &m = rt.memory();
            const std::uint32_t hud = m.load32(0x08a448f0u);
            const std::uint32_t car = (hud >= 0x08800000u && hud < 0x0a000000u) ? m.load32(hud + 0xc4u) : 0u;
            if (car >= 0x08800000u && car < 0x0a000000u) {
                const float d = std::bit_cast<float>(m.load32(car + 0x860u)), n = static_cast<float>(std::atof(dd));
                if (last_dist >= 0.0f && last_dist < n && d >= n && d - last_dist < 100.0f) { dist_done = true; difftrace::start(rt); }
                last_dist = d;
            }
        }
    }
    if (g_hle_options.window && present_cartrace_requested()) cartrace::start(rt);
    cartrace::frame_end(rt, g_frames_presented);
    if (g_hle_options.window && present_difftrace_requested()) difftrace::start(rt);
    difftrace::frame_end(rt, g_frames_presented);
    if (g_hle_options.window && present_callprofile_requested()) callprof::start();
    callprof::frame_end(g_frames_presented);
    // Insert: snapshot user RAM (plus a screenshot) for finding game variables by comparing snapshots.
    // RR_RAMDUMP_FRAMES="f1,f2,..." (rig): the same snapshot at those frames.
    static const std::vector<unsigned long long> dump_frames = [] {
        std::vector<unsigned long long> v;
        if (const char *e = std::getenv("RR_RAMDUMP_FRAMES")) for (const char *q = e; *q != '\0';) { char *end = nullptr; v.push_back(std::strtoull(q, &end, 10)); if (end == q) break; q = *end == ',' ? end + 1 : end; }
        return v;
    }();
    const bool ramdump = (g_hle_options.window && present_ramdump_requested()) ||
                         std::find(dump_frames.begin(), dump_frames.end(), static_cast<unsigned long long>(g_frames_presented)) != dump_frames.end();
    if (ramdump) {
        static int dump_n = 0;
        const std::uint32_t base = 0x08800000u, len = 0x01800000u;
        if (const std::uint8_t *p = static_cast<const psprecomp::GuestMemory &>(rt.memory()).raw_pointer(base, len)) {
            std::error_code ec;
            std::filesystem::create_directories("ramdumps", ec);
            char name[96];
            std::snprintf(name, sizeof name, "ramdumps/ram_%02d_frame%07llu.bin", ++dump_n, static_cast<unsigned long long>(g_frames_presented));
            write_file_async(name, std::vector<std::uint8_t>(p, p + len));
            log_line(category::kHle, std::string("RAMDUMP ") + name + " (base 08800000, 24 MB)");
        }
    }
    // Ctrl+F12: bug report (with a screenshot of this frame), made in the background while the game goes on.
    {   // RR_TEST_CRASH=frame (test of the crash report): a bad memory write at that frame
        static const std::uint64_t crash_at = [] { const char *e = std::getenv("RR_TEST_CRASH"); return e ? std::strtoull(e, nullptr, 10) : 0ull; }();
        if (crash_at != 0u && g_frames_presented == crash_at) { volatile int *bad = nullptr; *bad = 1; }
    }
    const bool bug_report = g_hle_options.window && present_bug_report_requested();
    if (bug_report) log_line(category::kHle, "bug report requested (Ctrl+F12)");
    const bool f12 = g_hle_options.window && !bug_report && (present_screenshot_requested() || ramdump);
    if (f12 && g_gl && g_hle_options.shot_mode != 1 && g_hle_options.shot_mode != 3) g_gl->request_capture();   // grab the finished picture while it is presented
    if (f12 && g_ge && std::getenv("RR_F12_TRACE") != nullptr) g_ge->trace_commands(3000u);   // debugging aid only
    // RR_SHOT_RANGE="start,count;start,count": save every frame in those ranges (to the screenshots directory).
    static const std::vector<std::pair<std::uint64_t, std::uint64_t>> shot_ranges = [] {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> out;
        if (const char *e = std::getenv("RR_SHOT_RANGE")) {
            std::string s(e);
            std::size_t pos = 0;
            while (pos < s.size()) {
                const std::size_t end = s.find(';', pos);
                const std::string part = s.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
                const std::size_t comma = part.find(',');
                if (comma != std::string::npos) out.emplace_back(std::strtoull(part.c_str(), nullptr, 0), std::strtoull(part.c_str() + comma + 1, nullptr, 0));
                if (end == std::string::npos) break;
                pos = end + 1;
            }
        }
        return out;
    }();
    bool in_shot_range = false;
    {   // RR_SHOT_GAMETIME=N (rig): a screenshot whenever another N frames of game time (59.94 Hz units, pad-read clock) have passed,
        // named frame_<frame>.png; the log line GAMESHOT maps it to the game time (compares runs at different frame rates).
        static const long long gt_every = [] { const char *e = std::getenv("RR_SHOT_GAMETIME"); return e ? std::atoll(e) : 0ll; }();
        static long long gt_next = 0;
        if (gt_every > 0 && static_cast<long long>(g_ctrl_game60) >= gt_next) {
            log_line(category::kHle, "GAMESHOT game " + std::to_string(gt_next) + " frame " + std::to_string(g_frames_presented));
            gt_next += gt_every;
            in_shot_range = true;
        }
    }
    for (const auto &[start, count] : shot_ranges) if (g_frames_presented >= start && g_frames_presented < start + count) in_shot_range = true;
    {
        static long long gs_a = -1, gs_b = 0, gs_step = 1;   // RR_GO_SHOTS=start,count[,stride]
        static const bool gs_parsed = [] { if (const char *e = std::getenv("RR_GO_SHOTS")) std::sscanf(e, "%lld,%lld,%lld", &gs_a, &gs_b, &gs_step); if (gs_step < 1) gs_step = 1; return true; }();
        (void)gs_parsed;
        if (gs_a >= 0 && g_go_frame != 0u) {
            const long long since = static_cast<long long>(g_frames_presented - g_go_frame);
            if (since >= gs_a && since < gs_a + gs_b && ((since - gs_a) % gs_step) == 0) in_shot_range = true;
        }
    }
    {   // RR_SHOT_BURST=N (with --screenshots DIR ...): F12 also saves the next N frames at PSP resolution (frame-by-frame motion checks)
        static const std::uint32_t burst_n = [] { const char *e = std::getenv("RR_SHOT_BURST"); return e ? static_cast<std::uint32_t>(std::atoi(e)) : 0u; }();
        static std::uint32_t burst_left = 0;
        if (f12 && burst_n != 0u) { burst_left = burst_n; log_line(category::kHle, "SHOTBURST " + std::to_string(burst_n) + " frames from frame " + std::to_string(g_frames_presented) + " step " + std::to_string(gates::game_step())); }
        if (burst_left != 0u) { --burst_left; in_shot_range = true; }
    }
    const bool shot = f12 || bug_report || (in_shot_range && !g_hle_options.screenshot_dir.empty()) || (!g_hle_options.screenshot_dir.empty() && g_hle_options.screenshot_every != 0u &&
                      (g_frames_presented % g_hle_options.screenshot_every) == 0u &&
                      g_screenshots_taken < g_hle_options.screenshot_max);
    if (!g_hle_options.window) {   // RR_WORDLOG_H=addr,... (headless): those guest words once per presented frame from RR_WORDLOG_FROM (400 lines)
        static const std::vector<std::uint32_t> hw = [] { std::vector<std::uint32_t> v; if (const char *e = std::getenv("RR_WORDLOG_H")) { std::string t(e); for (std::size_t q = 0; q < t.size();) { const std::size_t c = t.find(',', q); const std::string part = t.substr(q, c == std::string::npos ? std::string::npos : c - q); if (!part.empty()) v.push_back(static_cast<std::uint32_t>(std::strtoul(part.c_str(), nullptr, 16))); if (c == std::string::npos) break; q = c + 1; } } return v; }();
        static const std::uint64_t hw_from = [] { const char *e = std::getenv("RR_WORDLOG_FROM"); return e ? std::strtoull(e, nullptr, 0) : 0ull; }();
        static int hw_lines = 0;
        if (!hw.empty() && hw_lines < 400 && g_frames_presented >= hw_from && g_dump_rt != nullptr) {
            ++hw_lines;
            std::string t = "WORDLOG f=" + std::to_string(g_frames_presented) + " step=" + std::to_string(gates::game_step());
            for (const std::uint32_t w : hw) t += " " + std::to_string(static_cast<std::int32_t>(g_dump_rt->memory().load32(w)));
            log_line(category::kHle, t);
        }
    }
    static const auto netsync_every = [] { const char *e = std::getenv("RR_NETSYNC_EVERY"); const long v = e ? std::atol(e) : 15; return static_cast<std::uint64_t>(v > 0 ? v : 15); };
    // Diagnostic (RR_NETSYNC=1, netplay tests): every 15 frames of a race, the wall clock (ms, shared by copies on one PC),
    // the player's distance (+0x860), and every car body's distance, to compare what each copy sees of the other.
    if (std::getenv("RR_NETSYNC") != nullptr && g_frames_presented - gates::scene_frame < 5u && (g_frames_presented % netsync_every()) == 0u && gates::car_manager != 0u) {
        auto &mem = rt.memory();
        const std::uint32_t base = mem.load32(gates::car_manager + 0x14u), count = mem.load32(gates::car_manager + 0x10u);
        const std::uint32_t hud = mem.load32(0x08a448f0u);
        const std::uint32_t pc = (hud >= 0x08800000u && hud < 0x0a000000u) ? mem.load32(hud + 0xc4u) : 0u;
        if (base >= 0x08800000u && base < 0x0a000000u && count <= 16u && pc >= 0x08800000u && pc < 0x0a000000u) {
            const long long wall = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            char b[160];
            std::snprintf(b, sizeof b, "NETSYNC wall=%lld frame=%llu me=%u dist=%.2f", wall, static_cast<unsigned long long>(g_frames_presented), (pc - base) / 0x27a0u,
                          static_cast<double>(std::bit_cast<float>(mem.load32(pc + 0x860u))));
            std::string l = b;
            for (std::uint32_t i = 0; i < count; ++i) {
                std::snprintf(b, sizeof b, " c%u=%.2f", i, static_cast<double>(std::bit_cast<float>(mem.load32(base + i * 0x27a0u + 0x860u))));
                l += b;
            }
            const std::uint32_t base2 = mem.load32(gates::car_manager + 0x18u);   // the other car array (CPU / remote cars)
            { static std::uint32_t told = 0; if (base2 != told) { told = base2; log_line(category::kHle, "NETSYNC cars: own " + hex32(pc) + " remote k0 " + hex32(base2) + " mgr " + hex32(gates::car_manager)); } }
            if (base2 >= 0x08800000u && base2 < 0x0a000000u) {   // positions (x, z) of the player and of the first remote car
                const auto fl = [&](std::uint32_t a) { return static_cast<double>(std::bit_cast<float>(mem.load32(a))); };
                std::snprintf(b, sizeof b, " mepos=%.3f,%.3f k0pos=%.3f,%.3f", fl(pc + 0x16a0u), fl(pc + 0x16a8u), fl(base2 + 0x16a0u), fl(base2 + 0x16a8u));
                l += b;
            }
            if (base2 >= 0x08800000u && base2 < 0x0a000000u)
                for (std::uint32_t i = 0; i < count; ++i) {
                    std::snprintf(b, sizeof b, " k%u=%.2f", i, static_cast<double>(std::bit_cast<float>(mem.load32(base2 + i * 0x27a0u + 0x860u))));
                    l += b;
                }
            log_line(category::kHle, l);
            if (std::getenv("RR_PATHLOG") != nullptr)   // every car body's distance and position (builds RR_AUTOPILOT path files)
                for (std::uint32_t i = 0; i < count; ++i) {
                    const std::uint32_t c = mem.load32(gates::car_manager + 0x18u) + i * 0x27a0u;
                    std::snprintf(b, sizeof b, "PATHPT %.2f %.2f %.2f", static_cast<double>(std::bit_cast<float>(mem.load32(c + 0x860u))),
                                  static_cast<double>(std::bit_cast<float>(mem.load32(c + 0x16a0u))), static_cast<double>(std::bit_cast<float>(mem.load32(c + 0x16a8u))));
                    log_line(category::kHle, b);
                }
        }
    }
    if (!g_hle_options.window) gates::apply_live();   // headless runs (the window block below does it otherwise)
    if (!g_hle_options.window && !shot && !purple_shot) return;
    static std::vector<std::uint8_t> rgba(480u * 272u * 4u);
    const auto present_t0 = std::chrono::steady_clock::now();
    struct PresentTimer { std::chrono::steady_clock::time_point t0; ~PresentTimer() { g_present_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); } } present_timer{present_t0};
    // With the GPU renderer the screen usually lives on the GPU: show it directly and only read it back when the
    // frame is needed on the CPU (screenshots, heartbeat) or was not drawn by the GPU (movies, CPU-drawn screens).
    const bool gpu_direct = g_gl && g_hle_options.window &&
                            g_display.frame_width == 512u && g_gl->has_target(g_display.frame_top);
    // Menu frame interpolation (--menu-interp): outside races, with the rate raised for races, the 59.94 Hz menus get in-between
    // pictures so they move at the raised rate too (the game itself still runs at 59.94 Hz, so nothing speeds up).
    int interp_m = 0;
    {
        const bool mode = g_gl && g_hle_options.window && g_hle_options.menu_interp && race_only_rate() && !g_fast_rate;
        const int m = mode ? std::clamp(static_cast<int>(std::lround(g_hle_options.fps_mult)), 1, 8) : 1;
        if (g_gl) g_gl->interp_set_recording(mode && m > 1);
        if (mode && m > 1 && gpu_direct && !skipped_frame && !osk_active()) interp_m = m;
        static int paced_m = 0;
        if (interp_m != paced_m) {
            paced_m = interp_m;
            present_set_frame_period_us(static_cast<int>(interp_m > 1 ? g_vblank_period_us / static_cast<std::uint64_t>(interp_m) : g_vblank_period_us));
        }
    }
    // Motion log (RR_MOTION_LOG=start,count): per presented frame, how much the picture changed since the previous one.
    static const std::uint64_t motion_start = [] { const char *e = std::getenv("RR_MOTION_LOG"); return e ? std::strtoull(e, nullptr, 0) : 0ull; }();
    static const std::uint64_t motion_count = [] { const char *e = std::getenv("RR_MOTION_LOG"); if (!e) return 0ull; const char *c = std::strchr(e, ','); return c ? std::strtoull(c + 1, nullptr, 0) : 120ull; }();
    const bool motion = motion_count != 0u && g_frames_presented >= motion_start && g_frames_presented < motion_start + motion_count;
    gates::flush_clock();
    {   // Insert = AUDMARK (log every sounding voice), Delete = silence all SAS voices (audio diagnostics)
        static int seen_mark = 0, seen_silence = 0;
        const int mk = g_audio_mark.load(), sl = g_audio_silence.load();
        if (mk != seen_mark) {
            seen_mark = mk;
            log_line(category::kHle, "AUDMARK n=" + std::to_string(mk) + " s=" + std::to_string(gates::game_step()) + " f=" + std::to_string(g_frames_presented) + " pool=" + hex32(gates::audio_pool_obj) + " slots=" + [] { std::string r; if (gates::audio_pool_obj != 0u) for (std::uint32_t i = 0; i < 32u; ++i) r += char('0' + (gates::rt->memory().load8(gates::audio_pool_obj + 76u * i) & 7u)); return r; }() + " " + psprecomp::sas_debug_voice_summary());
        }
        if (sl != seen_silence) {
            seen_silence = sl;
            const int n = psprecomp::sas_debug_silence_all();
            log_line(category::kHle, "AUDSILENCE n=" + std::to_string(sl) + " s=" + std::to_string(gates::game_step()) + " f=" + std::to_string(g_frames_presented) + " silenced=" + std::to_string(n));
        }
    }
    {   // RR_WORDLOG=addr,addr,...: after the player's car starts moving, log those guest words once per presented frame (400 lines)
        static const std::vector<std::uint32_t> words = [] { std::vector<std::uint32_t> v; if (const char *e = std::getenv("RR_WORDLOG")) { std::string t(e); for (std::size_t q = 0; q < t.size();) { const std::size_t c = t.find(',', q); const std::string part = t.substr(q, c == std::string::npos ? std::string::npos : c - q); if (!part.empty()) v.push_back(static_cast<std::uint32_t>(std::strtoul(part.c_str(), nullptr, 16))); if (c == std::string::npos) break; q = c + 1; } } return v; }();
        static int wl_lines = 0;
        static const std::uint64_t wl_from = [] { const char *e = std::getenv("RR_WORDLOG_FROM"); return e ? std::strtoull(e, nullptr, 0) : 0ull; }();
        if (!words.empty() && gates::cam_started && wl_lines < 400 && g_frames_presented >= wl_from) {
            ++wl_lines;
            std::string t = "WORDLOG f=" + std::to_string(g_frames_presented) + " step=" + std::to_string(gates::game_step());
            for (const std::uint32_t w : words) t += " " + hex32(w) + "=" + hex32(g_dump_rt ? g_dump_rt->memory().load32(w) : 0u);
            log_line(category::kHle, t);
        }
    }
    // The heartbeat used to read the whole screen back from the GPU every 300 frames just to count lit pixels: a
    // synchronous readback that showed up as a hitch every 2.5 s at 120 fps.  RR_HEARTBEAT_LIT=1 brings it back.
    static const bool heartbeat_lit = std::getenv("RR_HEARTBEAT_LIT") != nullptr;
    const bool need_rgba = (!gpu_direct && !skipped_frame) || shot || purple_shot || motion || (heartbeat_lit && g_frames_presented % 300u == 0u);
    if (need_rgba) {
        bool have = false;
        if (gpu_direct && g_gl->read_native_rgba(g_display.frame_top, rgba.data())) have = true;
        if (!have) {
            if (g_gl) g_gl->vram_read(g_display.frame_top, g_display.frame_width * 272u * (g_display.frame_format == 3u ? 4u : 2u));
            if (!convert_frame_to_rgba(rt.memory(), g_display.frame_top, g_display.frame_width, g_display.frame_format,
                                       480u, 272u, rgba.data()))
                return;
        }
    }
    if (motion && need_rgba) {
        static std::vector<std::uint8_t> previous;
        static auto last_t = std::chrono::steady_clock::now();
        const auto now_t = std::chrono::steady_clock::now();
        std::uint64_t sum = 0, changed = 0;
        if (previous.size() == rgba.size())
            for (std::size_t i = 0; i < rgba.size(); i += 4) {
                const unsigned d = std::abs(int(rgba[i]) - int(previous[i])) + std::abs(int(rgba[i + 1]) - int(previous[i + 1])) + std::abs(int(rgba[i + 2]) - int(previous[i + 2]));
                sum += d; if (d > 12u) ++changed;
            }
        previous = rgba;
        static std::uint64_t last_vt = 0u, last_vb = 0u, last_setfb = 0u, last_lists = 0u, last_sync = 0u;
        const auto import_count = [](const char *name) {
            const std::string suffix = std::string("::") + name; unsigned long long total = 0;
            for (const auto &[k, c] : g_import_calls)
                if (k.size() >= suffix.size() && k.compare(k.size() - suffix.size(), suffix.size(), suffix) == 0) total += c;
            return total; };
        const std::uint64_t cur_setfb = import_count("sceDisplaySetFrameBuf"), cur_lists = import_count("sceGeListEnQueue") + import_count("sceGeListEnQueueHead"),
                            cur_sync = import_count("sceGeDrawSync") + import_count("sceGeListSync");
        char mb[200];
        std::snprintf(mb, sizeof mb, "MOTION frame=%llu dt=%.1fms vt=%lluus vblanks=%llu setfb=%llu lists=%llu syncs=%llu meandiff=%.3f changed=%llu", static_cast<unsigned long long>(g_frames_presented),
                      std::chrono::duration<double, std::milli>(now_t - last_t).count(), static_cast<unsigned long long>(g_virtual_time_us - last_vt),
                      static_cast<unsigned long long>(g_vblank_waits - last_vb), static_cast<unsigned long long>(cur_setfb - last_setfb),
                      static_cast<unsigned long long>(cur_lists - last_lists), static_cast<unsigned long long>(cur_sync - last_sync),
                      static_cast<double>(sum) / (480.0 * 272.0), static_cast<unsigned long long>(changed));
        last_vt = g_virtual_time_us; last_vb = g_vblank_waits; last_setfb = cur_setfb; last_lists = cur_lists; last_sync = cur_sync;
        last_t = now_t;
        log_line(category::kHle, mb);
    }
    if (g_frames_presented % 300u == 0u) {
        // Heartbeat: what the screen holds, what the player is pressing, and
        // which imports ran during the last 300 frames.
        std::uint64_t lit = 0;
        if (need_rgba)
            for (std::size_t i = 0; i < 480u * 272u; ++i)
                lit += (rgba[i * 4] | rgba[i * 4 + 1] | rgba[i * 4 + 2]) > 8u ? 1u : 0u;
        static std::unordered_map<std::string, std::uint64_t> beat_snapshot;
        std::vector<std::pair<std::uint64_t, std::string>> diff;
        for (const auto &[name, count] : g_import_calls) {
            const auto before = beat_snapshot.find(name);
            const std::uint64_t delta = count - (before == beat_snapshot.end() ? 0u : before->second);
            if (delta != 0u) diff.emplace_back(delta, name);
        }
        std::sort(diff.rbegin(), diff.rend());
        beat_snapshot = g_import_calls;
        std::ostringstream out;
        out << "HEARTBEAT frame=" << g_frames_presented << " lit_pixels=" << (need_rgba ? std::to_string(lit * 100u / (480u * 272u)) + "%" : std::string("n/a"))
            << " pad=" << hex32(effective_pad_buttons()) << " imports:";
        std::size_t shown = 0;
        for (const auto &[delta, name] : diff) {
            if (++shown > 26u) break;
            const auto sep = name.find("::");
            out << " " << (sep == std::string::npos ? name : name.substr(sep + 2)) << "=" << delta;
        }
        log_line(category::kHle, out.str());
        if (g_ge) log_line(category::kGe, "HEARTBEAT GE " + g_ge->brief());
        if (g_gl) log_line(category::kGe, "HEARTBEAT GL " + g_gl->brief());
        {
            // Where the time goes: wall clock for the last 300 frames vs time spent drawing and presenting.
            static auto last_beat = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            const double wall_ms = std::chrono::duration<double, std::milli>(now - last_beat).count();
            last_beat = now;
            const double draw_ms = g_ge ? g_ge->take_draw_ms() : 0.0;
            char tbuf[400];
            std::snprintf(tbuf, sizeof tbuf, "HEARTBEAT TIMING frames=300 wall=%.0fms (%.1f fps) ge_draw=%.0fms (%.0f%%) present_extra=%.0fms",
                          wall_ms, 300000.0 / std::max(wall_ms, 1.0), draw_ms, 100.0 * draw_ms / std::max(wall_ms, 1.0), g_present_ms);
            g_present_ms = 0.0;
            log_line(category::kHle, tbuf);
            {
                // Game time vs wall time: step counter delta / (60 * rate) is the game seconds that passed; the ratio says whether
                // the game fell behind real time (slow motion) or caught up with logic-only steps.
                static std::uint32_t gs_last = 0u; static bool gs_have = false;
                const std::uint32_t gs = gates::game_step();
                if (gs_have && gs >= gs_last) {
                    const double rate = gates::fast_rate_active() ? g_hle_options.fps_mult : 1.0;
                    const double game_s = static_cast<double>(gs - gs_last) / (59.94 * rate);
                    char gb[200];
                    std::snprintf(gb, sizeof gb, "HEARTBEAT GAMETIME steps=%u frames=300 wall=%.2fs game=%.2fs ratio=%.3f (steps per frame %.3f)",
                                  gs - gs_last, wall_ms / 1000.0, game_s, game_s / std::max(wall_ms / 1000.0, 0.001), static_cast<double>(gs - gs_last) / 300.0);
                    log_line(category::kHle, gb);
                }
                gs_last = gs; gs_have = true;
            }
            {
                double swap_ms = 0.0, pace_ms = 0.0;
                present_take_timing(swap_ms, pace_ms);
                if (g_hle_options.latency_log) {
                    PresentLatency pl;
                    present_take_latency(pl);
                    std::snprintf(tbuf, sizeof tbuf, "HEARTBEAT LATENCY frame start -> pad read avg=%.2fms max=%.2fms | pad read -> frame handed to display avg=%.2fms max=%.2fms (%d frames) | GPU still busy after that avg=%.2fms max=%.2fms (%d probes)%s",
                                  pl.wake_to_pad_avg, pl.wake_to_pad_max, pl.pad_to_swap_avg, pl.pad_to_swap_max, pl.frames, pl.gpu_backlog_avg, pl.gpu_backlog_max, pl.probes, g_hle_options.low_latency ? " [low-latency mode: GPU waited every frame]" : "");
                    log_line(category::kHle, tbuf);
                }
                std::snprintf(tbuf, sizeof tbuf, "HEARTBEAT PRESENT swapbuffers=%.0fms pacing_sleep=%.0fms gl_flush=%.0fms", swap_ms, pace_ms, g_gl ? g_gl->take_flush_ms() : 0.0);
                log_line(category::kHle, tbuf);
            }
            if (prof::enabled) log_line(category::kHle, prof::take_report());
            std::snprintf(tbuf, sizeof tbuf, "HEARTBEAT FRAMETIME avg=%.1fms max=%.1fms over20ms=%llu over33ms=%llu",
                          g_ft_sum / static_cast<double>(std::max<std::uint64_t>(g_ft_frames, 1u)), g_ft_max,
                          static_cast<unsigned long long>(g_ft_over20), static_cast<unsigned long long>(g_ft_over33));
            g_ft_sum = 0.0; g_ft_max = 0.0; g_ft_frames = g_ft_over20 = g_ft_over33 = 0;
            log_line(category::kHle, tbuf);
            {
                const AudioStats a = audio_take_stats();
                std::snprintf(tbuf, sizeof tbuf, "HEARTBEAT AUDIO pushed=%llu played=%llu underruns=%llu drops=%llu backlog=%llu..%llu frames device_starved=%llu max_mixer_gap=%llums stretched=%llu (%llu starts)",
                              static_cast<unsigned long long>(a.pushed), static_cast<unsigned long long>(a.played), static_cast<unsigned long long>(a.underruns),
                              static_cast<unsigned long long>(a.drops), static_cast<unsigned long long>(a.queue_min), static_cast<unsigned long long>(a.queue_max),
                              static_cast<unsigned long long>(a.device_starved), static_cast<unsigned long long>(a.device_gap_max_ms),
                              static_cast<unsigned long long>(a.stretched), static_cast<unsigned long long>(a.stretch_starts));
                log_line(category::kHle, tbuf);
            }
        }
        // Trace one frame's worth of GE commands whenever the screen is black.
        if (lit == 0u && (g_frames_presented % 900u) == 0u && g_ge) g_ge->trace_commands(2500u);
    }
    if (bug_report) {   // (the frame as the PSP would show it; small and quick to write)
        std::error_code ec;
        std::filesystem::create_directories(rr::report::reports_dir(), ec);
        const std::filesystem::path shot_path = rr::report::reports_dir() / "bug-report-screenshot.png";
        const bool ok = write_png(shot_path.string().c_str(), rgba.data(), 480u, 272u);
        rr::report::make_bug_report_async("Ctrl+F12 in game", ok ? shot_path : std::filesystem::path{});
    }
    if (f12) {
        // F12: one tidy file per press, named by date and time so nothing is ever overwritten.
        std::error_code ec;
        const std::filesystem::path dir = g_hle_options.shot_dir.empty() ? std::filesystem::path("screenshots") : g_hle_options.shot_dir;
        std::filesystem::create_directories(dir, ec);
        const std::time_t now_t = std::time(nullptr);
        std::tm tmv{};
#ifdef _WIN32
        localtime_s(&tmv, &now_t);
#else
        localtime_r(&now_t, &tmv);
#endif
        char stamp[48];
        std::strftime(stamp, sizeof stamp, "RidgeRacer_%Y-%m-%d_%H-%M-%S", &tmv);
        std::filesystem::path base = dir / stamp;
        static std::string last_stamp; static int dup = 1;   // files are written in the background, so count same-second shots ourselves
        if (last_stamp == stamp) ++dup; else { last_stamp = stamp; dup = 1; }
        const auto name_for = [&](int n) { return n > 1 ? dir / (std::string(stamp) + "_" + std::to_string(n)) : dir / stamp; };
        base = name_for(dup);
        while (std::filesystem::exists(base.string() + ".png", ec) || std::filesystem::exists(base.string() + "_native.png", ec)) base = name_for(++dup);
        const int mode = g_hle_options.shot_mode;
        bool saved_big = false;
        if ((mode == 0 || mode == 2) && g_gl) {   // exactly what the window shows: filters, anti-aliasing and sharpening included
            std::vector<std::uint8_t> shown; int sw = 0, sh = 0;
            if (g_gl->take_capture(shown, sw, sh)) {
                const std::string hp = base.string() + ".png";
                saved_big = true; ++g_screenshots_taken;
                log_line(category::kGe, "screenshot " + hp + " (" + std::to_string(sw) + "x" + std::to_string(sh) + ", as shown)");
                write_png_async(hp, std::move(shown), static_cast<std::uint32_t>(sw), static_cast<std::uint32_t>(sh));
            }
        }
        if (!saved_big && mode != 1 && g_gl && g_gl->scale() > 1 && g_display.frame_width == 512u && g_gl->has_target(g_display.frame_top)) {
            std::vector<std::uint8_t> big; int bw = 0, bh = 0;
            if (g_gl->read_scaled_rgba(g_display.frame_top, big, bw, bh)) {
                const std::string hp = base.string() + ".png";
                saved_big = true; ++g_screenshots_taken;
                log_line(category::kGe, "screenshot " + hp + " (" + std::to_string(bw) + "x" + std::to_string(bh) + ")");
                write_png_async(hp, std::move(big), static_cast<std::uint32_t>(bw), static_cast<std::uint32_t>(bh));
            }
        }
        if (mode != 0 || !saved_big) {   // native size: asked for, or the only thing available (movies, software renderer)
            const std::string np = base.string() + ((mode == 2 && saved_big) ? "_native.png" : ".png");
            ++g_screenshots_taken; log_line(category::kGe, "screenshot " + np);
            write_png_async(np, std::vector<std::uint8_t>(rgba.begin(), rgba.begin() + 480 * 272 * 4), 480u, 272u);
        }
    }
    if (shot && !f12 && !bug_report) {
        std::error_code ec;
        std::filesystem::create_directories(g_hle_options.screenshot_dir, ec);
        char name[64];
        std::snprintf(name, sizeof(name), "frame_%07llu.png", static_cast<unsigned long long>(g_frames_presented));
        std::filesystem::path path = g_hle_options.screenshot_dir / name;
        if (write_png(path.string().c_str(), rgba.data(), 480u, 272u)) {
            ++g_screenshots_taken;
            log_line(category::kGe, "screenshot " + path.string());
        }
        if ((in_shot_range && std::getenv("RR_SHOT_HIRES") != nullptr) && g_gl && g_gl->scale() > 1 && g_display.frame_width == 512u && g_gl->has_target(g_display.frame_top)) {
            // F12 (or RR_SHOT_RANGE with RR_SHOT_HIRES set) also saves the frame at internal resolution (what the window shows): "<name>_hires.png"
            std::vector<std::uint8_t> big; int bw = 0, bh = 0;
            if (g_gl->read_scaled_rgba(g_display.frame_top, big, bw, bh)) {
                std::filesystem::path hp = path; hp.replace_extension(); hp += "_hires.png";
                if (write_png(hp.string().c_str(), big.data(), static_cast<std::uint32_t>(bw), static_cast<std::uint32_t>(bh)))
                    log_line(category::kGe, "screenshot " + hp.string());
            }
        }
    }
    {   // RR_REGION="x,y,w,h;start,count" (native 480x272 coords, absolute presented frames): log a hash + mean colour of that
        // screen region, read from the scaled render target (what the window shows), once per frame.  Test harness measurement.
        static int rx = -1, ry = 0, rw = 0, rh = 0; static long long rs = 0, rc = 0;
        static bool parsed = false;
        static bool go_rel = false;
        if (!parsed) {
            parsed = true;
            if (const char *e = std::getenv("RR_REGION")) std::sscanf(e, "%d,%d,%d,%d;%lld,%lld", &rx, &ry, &rw, &rh, &rs, &rc);
            else if (const char *e2 = std::getenv("RR_REGION_GO")) { std::sscanf(e2, "%d,%d,%d,%d;%lld,%lld", &rx, &ry, &rw, &rh, &rs, &rc); go_rel = true; }   // start/count relative to GO
        }
        const long long f = go_rel ? (g_go_frame != 0u ? static_cast<long long>(g_frames_presented - g_go_frame) : -1000000ll) : static_cast<long long>(g_frames_presented);
        if (rx >= 0 && f >= rs && f < rs + rc && g_gl && g_display.frame_width == 512u && g_gl->has_target(g_display.frame_top)) {
            std::vector<std::uint8_t> big; int bw = 0, bh = 0;
            if (g_gl->read_scaled_rgba(g_display.frame_top, big, bw, bh) && bw > 0) {
                const double sx = bw / 480.0, sy = bh / 272.0;
                const int x0 = static_cast<int>(rx * sx), y0 = static_cast<int>(ry * sy), x1 = static_cast<int>((rx + rw) * sx), y1 = static_cast<int>((ry + rh) * sy);
                std::uint32_t h = 2166136261u; double r = 0, g = 0, b = 0; long n = 0;
                for (int y = std::max(0, y0); y < std::min(bh, y1); ++y)
                    for (int x = std::max(0, x0); x < std::min(bw, x1); ++x) {
                        const std::uint8_t *px = &big[(static_cast<std::size_t>(y) * bw + x) * 4u];
                        h = (h ^ (px[0] >> 3)) * 16777619u; h = (h ^ (px[1] >> 3)) * 16777619u; h = (h ^ (px[2] >> 3)) * 16777619u;
                        r += px[0]; g += px[1]; b += px[2]; ++n;
                    }
                if (n > 0) {
                    char line[160];
                    std::snprintf(line, sizeof line, "REGION f=%lld step=%u h=%08x mean=%.1f,%.1f,%.1f", f, gates::game_step(), h, r / n, g / n, b / n);
                    log_line(category::kHle, line);
                }
            }
        }
    }
    if (shot) {
        // RR_DUMP_RT="addr,stride,fmt" (hex addr): also save that VRAM buffer (as the GPU backend sees it) next to the screenshot.
        static unsigned dump_addr = 0u, dump_stride = 0u, dump_fmt = 0u; static bool dump_parsed = false;
        if (!dump_parsed) {
            dump_parsed = true;
            if (const char *e = std::getenv("RR_DUMP_RT")) { if (std::sscanf(e, "%x,%u,%u", &dump_addr, &dump_stride, &dump_fmt) != 3) dump_addr = 0u; }
        }
        if (dump_addr != 0u && dump_stride >= 32u && dump_stride <= 512u) {
            const std::uint32_t bpp = dump_fmt == 3u ? 4u : 2u, rows = 272u, bytes = dump_stride * rows * bpp;
            if (g_gl) g_gl->vram_read(dump_addr, bytes);
            if (rt.memory().contains(dump_addr, bytes)) {
                std::vector<std::uint8_t> img(static_cast<std::size_t>(dump_stride) * rows * 4u);
                for (std::uint32_t y = 0; y < rows; ++y)
                    for (std::uint32_t x = 0; x < dump_stride; ++x) {
                        std::uint32_t r, g, b;
                        if (bpp == 4u) { const std::uint32_t p = rt.memory().load32(dump_addr + (y * dump_stride + x) * 4u); r = p & 255u; g = (p >> 8u) & 255u; b = (p >> 16u) & 255u; }
                        else {
                            const std::uint32_t p = rt.memory().load16(dump_addr + (y * dump_stride + x) * 2u);
                            if (dump_fmt == 0u) { r = (p & 31u) * 255u / 31u; g = ((p >> 5u) & 63u) * 255u / 63u; b = ((p >> 11u) & 31u) * 255u / 31u; }
                            else { r = (p & 31u) * 255u / 31u; g = ((p >> 5u) & 31u) * 255u / 31u; b = ((p >> 10u) & 31u) * 255u / 31u; }
                        }
                        std::uint8_t *o = &img[(static_cast<std::size_t>(y) * dump_stride + x) * 4u];
                        o[0] = static_cast<std::uint8_t>(r); o[1] = static_cast<std::uint8_t>(g); o[2] = static_cast<std::uint8_t>(b); o[3] = 255u;
                    }
                std::error_code ec3;
                std::filesystem::create_directories(g_hle_options.screenshot_dir, ec3);
                char dn[96];
                std::snprintf(dn, sizeof dn, "rt_%08X_frame_%07llu.png", dump_addr, static_cast<unsigned long long>(g_frames_presented));
                write_png((g_hle_options.screenshot_dir / dn).string().c_str(), img.data(), dump_stride, rows);
            }
        }
    }
    if (purple_shot) {
        std::error_code ec2;
        std::filesystem::create_directories(g_hle_options.screenshot_dir, ec2);
        char pname[64];
        std::snprintf(pname, sizeof(pname), "purple_%07llu.png", static_cast<unsigned long long>(g_frames_presented));
        const auto ppath = g_hle_options.screenshot_dir / pname;
        if (write_png(ppath.string().c_str(), rgba.data(), 480u, 272u)) log_line(category::kGe, "screenshot " + ppath.string());
    }
    if (g_hle_options.window) {
        {
            // Frame-time histogram for the heartbeat: how evenly frames are presented matters more than the average.
            static auto last_present = std::chrono::steady_clock::now();
            static auto spike_prev_present_end = last_present;
            static double spike_prev_draw = 0.0, spike_prev_rb = 0.0;
            const auto now_p = std::chrono::steady_clock::now();
            const double ms = std::chrono::duration<double, std::milli>(now_p - last_present).count();
            last_present = now_p;
            struct SpikeReset { std::chrono::steady_clock::time_point &t; std::chrono::steady_clock::time_point v; double &d; double &r; ~SpikeReset() { t = v; d = g_ge ? g_ge->peek_draw_ms() : 0.0; r = g_gl ? g_gl->rb_total_ms() : 0.0; } } spike_reset{spike_prev_present_end, now_p, spike_prev_draw, spike_prev_rb};
            ++g_ft_frames; g_ft_sum += ms;
            if (ms > g_ft_max) g_ft_max = ms;
            if (ms > 20.0) ++g_ft_over20;
            if (ms > 33.5) ++g_ft_over33;
            if (!perfrep::path.empty() && g_frames_presented > 30u) {
                const bool in_race = gates::scene_frame != 0u && g_frames_presented - gates::scene_frame < 5u;
                const double target = gates::fast_rate_active() ? 1000.0 / (59.94 * std::max(g_hle_options.fps_mult, 0.5)) : 1000.0 / 59.94;   // (menus run at the raised rate too with --menu-native)
                perfrep::record(in_race, ms, 0.0, 0.0, (g_ge ? g_ge->peek_draw_ms() : 0.0) - spike_prev_draw, target);
                perfrep::note_block(now_p);
                if ((g_frames_presented % 1800u) == 0u) perfrep::write_report();
            }
            {
                // One line per slow frame (first 400): when it happens and what the GPU backend did during it.
                std::uint64_t cur[4] = {0, 0, 0, 0};
                if (g_gl) for (int i = 0; i < 4; ++i) cur[i] = g_gl->spike_counter(i);
                static const double spike_ms = [] { const char *e = std::getenv("RR_SPIKE_MS"); return e ? std::atof(e) : 24.0; }();
                if (ms > spike_ms && g_spikes_logged < (std::getenv("RR_SPIKE_MS") != nullptr ? 6000u : 400u) && (g_frames_presented > 3000u || std::getenv("RR_SPIKE_MS") == nullptr)) {
                    ++g_spikes_logged;
                    char sb[300];
                    std::snprintf(sb, sizeof sb, "SPIKE frame=%llu %.1fms [guest+ge-to-present %.1f, in-present %.1f, ge_draw %.1f, readback %.1f] readbacks=%llu gl=%llu sw=%llu texup=%llu", static_cast<unsigned long long>(g_frames_presented), ms,
                                  std::chrono::duration<double, std::milli>(present_t0 - spike_prev_present_end).count(),
                                  std::chrono::duration<double, std::milli>(now_p - present_t0).count(),
                                  (g_ge ? g_ge->peek_draw_ms() : 0.0) - spike_prev_draw, (g_gl ? g_gl->rb_total_ms() : 0.0) - spike_prev_rb,
                                  static_cast<unsigned long long>(cur[0] - g_spike_prev[0]), static_cast<unsigned long long>(cur[1] - g_spike_prev[1]),
                                  static_cast<unsigned long long>(cur[2] - g_spike_prev[2]), static_cast<unsigned long long>(cur[3] - g_spike_prev[3]));
                    log_line(category::kHle, sb);
                }
                for (int i = 0; i < 4; ++i) g_spike_prev[i] = cur[i];
            }
        }
        {
            gates::apply_live();
            // Diagnostic (RR_LAPDIAG=1): twice a second during a race, the race info object's state and every word of the
            // player's per-car block that moves like a timer, to see whether the lap timer runs.
            if (std::getenv("RR_LAPDIAG") != nullptr && g_frames_presented - gates::scene_frame < 5u && (g_frames_presented % 30u) == 0u) {
                auto &mem = rt.memory();
                const std::uint32_t info = 0x08A3FE80u, blk = 0x08A4D330u;
                static std::vector<std::uint32_t> prev;
                if (prev.size() != 0x770u / 4u) prev.assign(0x770u / 4u, 0u);
                const std::uint32_t game = mem.load32(info + 0x88u);
                char b[256];
                std::snprintf(b, sizeof b, "LAPDIAG frame=%llu step=%u cars=%u cnt54=%u f224=%.3f running298=%u cfg10=%u blk8=%08X a4=%u c4=%u",
                              static_cast<unsigned long long>(g_frames_presented), gates::game_step(), mem.load32(info + 4u), mem.load32(info + 0x54u),
                              static_cast<double>(std::bit_cast<float>(mem.load32(info + 0x224u))),
                              (game >= 0x08800000u && game < 0x0a000000u) ? mem.load32(game + 0x298u) : 0xFFFFFFFFu,
                              mem.load32(mem.load32(0x08A447FCu) + 0x10u),
                              mem.load32(blk + 8u), mem.load32(blk + 0xa4u) & 0xffffu, mem.load32(blk + 0xc4u) & 0xffffu);
                std::string l = b + std::string(" timers:");
                for (std::uint32_t k = 0; k < 0x770u / 4u; ++k) {
                    const std::uint32_t v = mem.load32(blk + 4u * k);
                    const std::int32_t d = static_cast<std::int32_t>(v - prev[k]);
                    if (d >= 100 && d <= 2000 && v < 5000000u) l += " +" + std::to_string(4u * k) + "=" + std::to_string(v);
                    prev[k] = v;
                }
                log_line(category::kHle, l);
                {   // the HUD object's own moving words (what the lap timer display reads)
                    const std::uint32_t hud = mem.load32(0x08a448f0u);
                    static std::vector<std::uint32_t> hprev;
                    if (hud >= 0x08800000u && hud < 0x0a000000u) {
                        if (hprev.size() != 0x1000u / 4u) hprev.assign(0x1000u / 4u, 0u);
                        std::string h = "LAPHUD " + hex32(hud) + " step=" + std::to_string(gates::game_step()) + " cnt54=" + std::to_string(mem.load32(info + 0x54u)) + " moving:";
                        for (std::uint32_t k = 0; k < 0x1000u / 4u; ++k) {
                            const std::uint32_t v = mem.load32(hud + 4u * k);
                            const std::int32_t d = static_cast<std::int32_t>(v - hprev[k]);
                            if (d >= 50 && d <= 3000 && v < 50000000u) h += " +" + std::to_string(4u * k) + "=" + std::to_string(v);
                            hprev[k] = v;
                        }
                        log_line(category::kHle, h);
                    }
                }
            }
            // Diagnostic (RR_LAPEV=1): every presented frame while the race info counter (+0x54) is within the first 3000
            // frames of a scene, log each word of the race info object, the game object, the player's per-car block and the
            // HUD object that changes, at most 4 times per 300 frames per word (rare changes are the one-shot events: GO,
            // line crossing, lap start, lap done).
            if (std::getenv("RR_LAPEV") != nullptr) {
                auto &mem = rt.memory();
                const std::uint32_t info = 0x08A3FE80u, blk = 0x08A4D330u;
                const std::uint32_t game = mem.load32(info + 0x88u);
                const std::uint32_t hud = mem.load32(0x08a448f0u);
                const std::uint32_t cnt = mem.load32(info + 0x54u);
                static std::uint32_t last_cnt = 0xFFFFFFFFu;
                static std::vector<std::uint32_t> pv[4];
                static std::vector<std::uint16_t> nch[4];
                static std::vector<std::uint32_t> wstart[4];
                static unsigned lines = 0;
                const auto ok = [](std::uint32_t a) { return a >= 0x08800000u && a < 0x0a000000u; };
                const std::uint32_t base[4] = {info, ok(game) ? game : 0u, blk, ok(hud) ? hud : 0u};
                const std::uint32_t words[4] = {0x400u / 4u, 0x400u / 4u, 0x770u / 4u, 0x400u / 4u};
                const char *tag[4] = {"info", "game", "blk", "hud"};
                if (cnt < last_cnt) {  // a new scene: forget the history
                    for (int r = 0; r < 4; ++r) { pv[r].assign(words[r], 0u); nch[r].assign(words[r], 0u); wstart[r].assign(words[r], 0u); }
                    lines = 0;
                }
                if (cnt != 0u && cnt <= 3000u && lines < 30000u) {
                    for (int r = 0; r < 4; ++r) {
                        if (base[r] == 0u || pv[r].size() != words[r]) continue;
                        for (std::uint32_t k = 0; k < words[r]; ++k) {
                            const std::uint32_t v = mem.load32(base[r] + 4u * k);
                            if (v != pv[r][k]) {
                                if (cnt - wstart[r][k] > 300u) { wstart[r][k] = cnt; nch[r][k] = 0; }
                                if (nch[r][k] < 4u && !(r == 0 && k == 0x54u / 4u)) {
                                    ++nch[r][k];
                                    char b[160];
                                    std::snprintf(b, sizeof b, "LAPEV frame=%llu step=%u cnt=%u %s+%X %08X->%08X", static_cast<unsigned long long>(g_frames_presented),
                                                  gates::game_step(), cnt, tag[r], 4u * k, pv[r][k], v);
                                    log_line(category::kHle, b);
                                    ++lines;
                                }
                                pv[r][k] = v;
                            }
                        }
                    }
                }
                last_cnt = cnt;
            }
            // Diagnostic (RR_CARLOG=1): once a second during a race, every car's speed (+0xc0), distance (+0x860) and
            // position (+0x25e0), to compare how far each car really moves with the speed it reports.
            if (std::getenv("RR_CARLOG") != nullptr && g_frames_presented - gates::scene_frame < 5u && (g_frames_presented % 30u) == 0u && gates::car_manager != 0u) {
                auto &mem = rt.memory();
                const std::uint32_t base = mem.load32(gates::car_manager + 0x18u), count = mem.load32(gates::car_manager + 0x10u);
                if (base >= 0x08800000u && base < 0x0a000000u && count <= 16u) {
                    std::string l = "CARLOG frame " + std::to_string(g_frames_presented);
                    {
                        const std::uint32_t hud = mem.load32(0x08a448f0u);
                        const std::uint32_t pc = (hud >= 0x08800000u && hud < 0x0a000000u) ? mem.load32(hud + 0xc4u) : 0u;
                        if (pc >= 0x08800000u && pc < 0x0a000000u) {
                            char b[80];
                            std::snprintf(b, sizeof b, " player speed=%.1f gear=%d", static_cast<double>(std::bit_cast<float>(mem.load32(pc + 0xc0u))), static_cast<int>(mem.load32(pc + 0x718u)));
                            l += b;
                        }
                    }
                    l += " cpu cars";
                    for (std::uint32_t i = 0; i < count; ++i) {
                        const std::uint32_t c = base + i * 0x27a0u;
                        char b[200];
                        const auto f = [&](std::uint32_t o) { return static_cast<double>(std::bit_cast<float>(mem.load32(c + o))); };
                        std::snprintf(b, sizeof b, " | %u act=%u fl=%x near=%u ts=%.3g dt=%.5g dts=%.5g g=%.4g", i, mem.load32(c + 0x4cu), mem.load32(c + 0x50u), mem.load32(c + 0x760u),
                                      f(0x26c8u), f(0x26d0u), f(0x26d8u), f(0x2740u));
                        l += b;
                    }
                    log_line(category::kHle, l);
                }
            }
            // Diagnostic (RR_HUDLOG=1): every 15 frames during a race, log the player-car fields the HUD copies
            // (0x0883ca80: car = [[0x08a448f0] + 0xc4]) so the nitro gauge can be told apart from speed and the rest.
            static const std::uint32_t hudlog_every = [] { const char *e = std::getenv("RR_HUDLOG_EVERY"); const int n = e ? std::atoi(e) : 15; return static_cast<std::uint32_t>(n > 0 ? n : 15); }();
            if (std::getenv("RR_HUDLOG") != nullptr && g_fast_rate && (g_frames_presented % hudlog_every) == 0u) {
                auto &mem = rt.memory();
                const std::uint32_t hud = mem.load32(0x08a448f0u);
                const std::uint32_t car = (hud >= 0x08800000u && hud < 0x0a000000u) ? mem.load32(hud + 0xc4u) : 0u;
                if (car >= 0x08800000u && car < 0x0a000000u) {
                    std::string l = "HUDLOG frame " + std::to_string(g_frames_presented) + " pad " + hex32(cartrace::pad_buttons) + " car " + hex32(car);
                    for (const std::uint32_t off : {0xc0u, 0x860u, 0x864u, 0x710u, 0x714u, 0x71cu, 0x728u, 0x72cu, 0x730u, 0x734u, 0x25e0u, 0x25e4u, 0x25e8u, 0x26a0u, 0x26a4u, 0x26a8u}) {
                        char b[40];
                        std::snprintf(b, sizeof b, " %x=%.4g", off, static_cast<double>(std::bit_cast<float>(mem.load32(car + off))));
                        l += b;
                    }
                    for (const std::uint32_t off : {0x718u, 0x724u, 0x26b8u}) l += " " + [&] { char b[32]; std::snprintf(b, sizeof b, "%x=%d", off, static_cast<int>(mem.load32(car + off))); return std::string(b); }();
                    log_line(category::kHle, l);
                }
            }
            // Diagnostic (RR_HUDLOG_DUMP=1): every frame while the player's course distance is within 1795..1830 (Union Hill
            // crest) dump the car words +0x25e0..+0x2780 (floats; non-float-looking words as hex).
            if (std::getenv("RR_HUDLOG_DUMP") != nullptr && g_fast_rate) {
                auto &mem = rt.memory();
                const std::uint32_t hud = mem.load32(0x08a448f0u);
                const std::uint32_t car = (hud >= 0x08800000u && hud < 0x0a000000u) ? mem.load32(hud + 0xc4u) : 0u;
                if (car >= 0x08800000u && car < 0x0a000000u) {
                    const float d = std::bit_cast<float>(mem.load32(car + 0x860u));
                    if (d >= 1795.0f && d <= 1830.0f) {
                        std::string l = "CARDUMP frame " + std::to_string(g_frames_presented) + " d " + std::to_string(d);
                        for (std::uint32_t off = 0x25e0u; off < 0x2780u; off += 4u) {
                            const std::uint32_t w = mem.load32(car + off);
                            const float f = std::bit_cast<float>(w);
                            char b[40];
                            if (w == 0u) continue;
                            if (std::isfinite(f) && std::fabs(f) > 1e-6f && std::fabs(f) < 1e7f) std::snprintf(b, sizeof b, " %x=%.5g", off, static_cast<double>(f));
                            else std::snprintf(b, sizeof b, " %x=#%x", off, w);
                            l += b;
                        }
                        log_line(category::kHle, l);
                        // Second line: the wheel structs, the road-height block and the physics globals (RR_HUDLOG_DUMP=1).
                        std::string w = "WDUMP frame " + std::to_string(g_frames_presented) + " d " + std::to_string(d);
                        const auto dump = [&](std::uint32_t base, std::uint32_t lo, std::uint32_t hi, const char *tag) {
                            for (std::uint32_t off = lo; off < hi; off += 4u) {
                                const std::uint32_t v = mem.load32(base + off);
                                if (v == 0u) continue;
                                const float f = std::bit_cast<float>(v);
                                char b[48];
                                if (std::isfinite(f) && std::fabs(f) > 1e-6f && std::fabs(f) < 1e9f) std::snprintf(b, sizeof b, " %s%x=%.6g", tag, off, static_cast<double>(f));
                                else std::snprintf(b, sizeof b, " %s%x=#%x", tag, off, v);
                                w += b;
                            }
                        };
                        dump(car, 0x80u, 0x100u, "");
                        dump(car, 0x1f80u, 0x2570u, "");
                        dump(car, 0x16a0u, 0x16e0u, "");
                        dump(car, 0x1a90u, 0x1aa0u, "");
                        const std::uint32_t gp = mem.load32(0x08890c00u);
                        if (gp >= 0x08800000u && gp < 0x0a000000u) dump(gp, 0x230u, 0x300u, "G");
                        log_line(category::kHle, w);
                    }
                }
            }
            // Live fps readout in the window title, refreshed twice a second.
            static auto fps_t0 = std::chrono::steady_clock::now();
            static std::uint64_t fps_frames = 0, fps_extra0 = 0;
            ++fps_frames;
            const auto now = std::chrono::steady_clock::now();
            const double secs = std::chrono::duration<double>(now - fps_t0).count();
            if (secs >= 0.5) {
                char title[360];
                // Pictures shown per second: game frames plus the menu in-between pictures (the game itself stays at 59.94 Hz there).
                const std::uint64_t extra = g_interp_pictures - fps_extra0;
                fps_extra0 = g_interp_pictures;
                if (g_gl) std::snprintf(title, sizeof title, RR_TITLE_PREFIX " - %.1f fps%s - GPU %dx%s", static_cast<double>(fps_frames + extra) / secs,
                                        extra != 0u ? " (menu, smoothed)" : "", g_gl->scale(), (std::getenv("RR_GPU_VERIFY") != nullptr || std::getenv("RR_GPU_VERIFY_SMALL") != nullptr) ? " [SELF-CHECK ON - SLOW]" : "");
                else std::snprintf(title, sizeof title, RR_TITLE_PREFIX " - %.1f fps - software", static_cast<double>(fps_frames) / secs);
                {
                    char vol[128];
                    if (audio_muted()) std::snprintf(vol, sizeof vol, " - MUTED");
                    else std::snprintf(vol, sizeof vol, " - vol %d%%", static_cast<int>(audio_master_volume() * 100.0f + 0.5f));
                    if (g_hide_class.load() > 0) { char c[24]; std::snprintf(c, sizeof c, " - HIDE %d", g_hide_class.load()); std::strncat(vol, c, sizeof vol - std::strlen(vol) - 1); }
                    {
                        static const char *const kAaNames[] = {"none", "FXAA", "TAA", "SSAA"};
                        const int am = std::clamp(g_aa_mode.load(), 0, 3);
                        char c[48];
                        if (g_msaa_samples.load() > 1) std::snprintf(c, sizeof c, " - AA %s+MSAA%dx", kAaNames[am], g_msaa_samples.load());
                        else std::snprintf(c, sizeof c, " - AA %s", kAaNames[am]);
                        if (am == 2) { char t[24]; std::snprintf(t, sizeof t, " (new %d%%)", g_taa_blend.load()); std::strncat(c, t, sizeof c - std::strlen(c) - 1); }
                        std::strncat(vol, c, sizeof vol - std::strlen(vol) - 1);
                    }
                    { char c[16]; std::snprintf(c, sizeof c, " - AF %dx", g_aniso.load()); std::strncat(vol, c, sizeof vol - std::strlen(vol) - 1); }
                    if (g_alpha_cutoff.load() > 0) { char c[24]; std::snprintf(c, sizeof c, " - cut %d", g_alpha_cutoff.load()); std::strncat(vol, c, sizeof vol - std::strlen(vol) - 1); }
                    std::strncat(title, vol, sizeof title - std::strlen(title) - 1);
                    const std::string gt = gates::title_text();
                    std::strncat(title, gt.c_str(), sizeof title - std::strlen(title) - 1);
                }
                // RR_TITLE_DIST=1: the player's course distance and speed (car +0x860 / +0xc0, as in RR_HUDLOG) lead the window title.
                static const bool title_dist = std::getenv("RR_TITLE_DIST") != nullptr;
                if (title_dist) {
                    auto &m = rt.memory();
                    const std::uint32_t hud = m.load32(0x08a448f0u);
                    const std::uint32_t car = (hud >= 0x08800000u && hud < 0x0a000000u) ? m.load32(hud + 0xc4u) : 0u;
                    if (car >= 0x08800000u && car < 0x0a000000u) {
                        char pre[96];
                        std::snprintf(pre, sizeof pre, "DIST %.0f  SPD %.0f  |  ", static_cast<double>(std::bit_cast<float>(m.load32(car + 0x860u))), static_cast<double>(std::bit_cast<float>(m.load32(car + 0xc0u))));
                        const std::string full = std::string(pre) + title;
                        present_set_title(full.c_str());
                        fps_t0 = now; fps_frames = 0;
                        goto title_done;
                    }
                }
                present_set_title(title);
                fps_t0 = now; fps_frames = 0;
                title_done:;
            }
        }
        if (skipped_frame && !shot && !purple_shot) {
            if (g_hle_options.frame_skip == 0 || g_net_race_sync) present_skip_frame_fast(); else present_skip_frame();
        } else if (g_gl) {
            int win_w = 0, win_h = 0;
            present_gl_size(win_w, win_h);
            bool shown = false;
            if (osk_active() && gpu_direct) {
                // The keyboard page is opaque, so the game frame is not read back at all: the page is drawn once at about the
                // window's resolution and only redrawn when the text, the cursor cell or the caret blink changes.
                static std::vector<std::uint8_t> big;
                static std::uint64_t big_key = 0u;
                const int os = std::clamp((win_w + 479) / 480, 2, 5);
                const int bw = 480 * os, bh = 272 * os;
                const std::uint64_t key = ui_page_state_key() ^ (static_cast<std::uint64_t>(os) << 56);
                if (big.size() != static_cast<std::size_t>(bw) * bh * 4u || key != big_key) {
                    big.assign(static_cast<std::size_t>(bw) * bh * 4u, 255);
                    ui_page_render(big.data(), bw, bh);
                    big_key = key;
                }
                g_gl->present_rgba(big.data(), bw, bh, win_w, win_h);
                shown = true;
            } else if (osk_active()) {
                ui_page_render(rgba.data(), 480, 272);
            }
            if (!shown && interp_m > 1) {
                // Menu frame interpolation: interp_m - 1 in-between pictures (previous game frame -> this one), each paced at
                // 1 / interp_m of a vblank, then the frame itself.  A frame that cannot be rebuilt is shown again instead.
                g_gl->interp_frame_done(g_display.frame_top);
                static const char *ishots = std::getenv("RR_INTERP_SHOTS");   // test: save the in-between pictures of RR_SHOT_RANGE frames
                for (int k = 1; k < interp_m; ++k) {
                    const bool ok = g_gl->interp_render(static_cast<float>(k) / static_cast<float>(interp_m)) && g_gl->present_interp(win_w, win_h);
                    if (!ok) g_gl->present_target(g_display.frame_top, win_w, win_h);
                    if (ishots != nullptr && in_shot_range) {
                        std::vector<std::uint8_t> px(480u * 272u * 4u);
                        if (ok && g_gl->read_interp_native(px.data())) {
                            std::error_code ec; std::filesystem::create_directories(ishots, ec);
                            char nm[64]; std::snprintf(nm, sizeof nm, "frame_%07llu_%d.png", static_cast<unsigned long long>(g_frames_presented), k);
                            write_png((std::filesystem::path(ishots) / nm).string().c_str(), px.data(), 480u, 272u);
                        }
                    }
                    present_gl_swap();
                    ++g_interp_pictures;
                }
                {   // a summary every ~10 s of menus
                    static std::uint64_t last_log = 0;
                    const auto &is = g_gl->interp_stats();
                    if (is.frames - last_log >= 600u) {
                        last_log = is.frames;
                        char b[300];
                        std::snprintf(b, sizeof b, "MENUINTERP x%d frames=%llu usable=%llu (cleared %llu) draws=%llu matched=%llu (same source %llu) moved=%llu 3d_snapped=%llu bad: sw=%llu self=%llu cpu=%llu base=%llu big=%llu", interp_m,
                                      (unsigned long long)is.frames, (unsigned long long)is.usable, (unsigned long long)is.cleared, (unsigned long long)is.draws, (unsigned long long)is.matched, (unsigned long long)is.exact, (unsigned long long)is.lerped,
                                      (unsigned long long)is.snapped_3d, (unsigned long long)is.bad_sw, (unsigned long long)is.bad_self, (unsigned long long)is.bad_cpu, (unsigned long long)is.bad_base, (unsigned long long)is.bad_big);
                        log_line(category::kGe, b);
                    }
                }
            }
            if (!shown && !(gpu_direct && g_gl->present_target(g_display.frame_top, win_w, win_h))) g_gl->present_rgba(rgba.data(), 480, 272, win_w, win_h);
            present_gl_swap();
        } else {
            if (osk_active()) ui_page_render(rgba.data(), 480, 272);
            present_frame(rgba.data(), 480, 272);
        }
        perfrep::end_tail();
        if (present_quit_requested()) rt.stop("window closed by user");
    }
}

// Delivers one pending GE interrupt (signal or finish callback) on the current
// thread.  The guest handler is void f(int id, void *arg); it runs through the
// regular callback-frame machinery, so it returns into the interrupted code.
bool ge_deliver(Runtime &runtime, AllegrexContext &ctx, std::uint32_t resume_pc) {
    while (!g_ge_pending.empty()) {
        const auto active = g_callback_frames.find(g_current_uid);
        if (active != g_callback_frames.end() && !active->second.empty()) return false;
        const GePending pending = g_ge_pending.front();
        g_ge_pending.erase(g_ge_pending.begin());
        const auto slot = g_ge_callbacks.find(pending.callback_id);
        if (slot == g_ge_callbacks.end()) continue;
        const std::uint32_t func = pending.finish ? slot->second.finish_func : slot->second.signal_func;
        const std::uint32_t arg = pending.finish ? slot->second.finish_arg : slot->second.signal_arg;
        if (func == 0u) continue;
        CallbackRecord record{};
        record.name = pending.finish ? "ge-finish" : "ge-signal";
        record.entry = func;
        record.arg = arg;
        record.owner_uid = -1;
        g_callbacks[-1] = std::move(record);
        notify_callback(runtime, ctx, -1, arg, "ge", pending.token, resume_pc);
        return true;
    }
    return false;
}

void queue_owned_callback(std::int32_t uid,std::uint32_t arg) {
    auto &cb=g_callbacks.at(uid);++cb.notify_count;cb.notify_arg=arg;
    const auto it=g_threads.find(cb.owner_uid);
    if (it==g_threads.end() || !it->second.callback_wait) return;
    auto &thread=it->second;
    if (thread.state == ThreadState::Delayed && thread.callback_wait_kind == CallbackWaitKind::Vblank) {
        thread.suspended.pc = thread.callback_wait_pc;
        make_ready(thread, 0u);
        return;
    }
    if (thread.state != ThreadState::Waiting) return;
    // Interrupt a callback-aware wait without consuming its semaphore. Reenter
    // the wait import on the owning PSP stack, run the callback, then retry it.
    for (auto &[sema_uid,sema] : g_semas) {
        (void)sema_uid;
        std::erase_if(sema.waiters,[&](const SemaWaiter &w){return w.uid==cb.owner_uid;});
    }
    if (it->second.callback_wait_kind==CallbackWaitKind::EventFlag) {
        for (auto &[flag_uid,flag] : g_event_flags) {
            (void)flag_uid;
            std::erase_if(flag.waiters,[&](const FlagWaiter &w){return w.uid==cb.owner_uid;});
        }
    }
    if (thread.callback_wait_kind==CallbackWaitKind::ThreadEnd) {
        const auto joins=g_joiners.find(thread.callback_wait_object);
        if (joins!=g_joiners.end())
            std::erase_if(joins->second,[&](const Joiner &w){return w.uid==cb.owner_uid;});
    }
    thread.suspended.pc=thread.callback_wait_pc;
    make_ready(thread,0);
}

// Trampoline target for a returning guest callback (registered at address 4).
void callback_return(Runtime &runtime, AllegrexContext &ctx) {
    const std::int32_t uid = g_current_uid;
    auto found = g_callback_frames.find(uid);
    if (found == g_callback_frames.end() || found->second.empty()) {
        error_line("callback_return: no pending callback frame on thread " + std::to_string(uid));
        runtime.stop("PSP callback returned without a pending frame");
        return;
    }
    const CallbackFrame frame = found->second.back();
    found->second.pop_back();
    const std::uint32_t result = ctx.gpr[2];

    if (g_options.trace_imports) {
        std::ostringstream out;
        out << "returned uid=" << frame.callback_uid << " name=\""
            << (g_callbacks.contains(frame.callback_uid) ? g_callbacks.at(frame.callback_uid).name
                                                         : std::string(frame.name))
            << "\" result=" << hex32(result) << " notify_arg=" << hex32(frame.notify_arg)
            << " thread=" << uid;
        log_line(category::kCallback, out.str());
    }

    // The kernel deletes a callback whose function returns non-zero.
    if (result != 0u && g_callbacks.erase(frame.callback_uid) == 1u) {
        if (g_umd_callback_uid == frame.callback_uid) g_umd_callback_uid = 0;
        log_line(category::kCallback, "uid=" + std::to_string(frame.callback_uid) +
                                          " returned non-zero -> deleted");
    }

    ctx = frame.saved;
}

// ---------------------------------------------------------------------------
// UMD virtual drive
// ---------------------------------------------------------------------------

void umd_wake_waiters(const char *reason) {
    std::vector<std::int32_t> woken;
    for (const UmdWaiter &waiter : g_umd_waiters) {
        if ((waiter.stat & g_umd_state) == 0u) continue;
        const auto thread = g_threads.find(waiter.uid);
        if (thread != g_threads.end()) wake_thread(thread->second, 0u, reason);
        woken.push_back(waiter.uid);
    }
    for (const std::int32_t uid : woken) {
        g_umd_waiters.erase(
            std::remove_if(g_umd_waiters.begin(), g_umd_waiters.end(),
                           [uid](const UmdWaiter &waiter) { return waiter.uid == uid; }),
            g_umd_waiters.end());
    }
}

void umd_set_state(Runtime &runtime, AllegrexContext &ctx, std::uint32_t new_state,
                   const char *reason) {
    if (new_state == g_umd_state) return;
    const std::uint32_t old_state = g_umd_state;
    g_umd_state = new_state;
    ++g_umd_transitions;
    std::ostringstream out;
    out << "state " << hex32(old_state) << " -> " << hex32(new_state) << " (" << reason << ")";
    log_line(category::kUmd, out.str());
    umd_wake_waiters("umd-state-change");
    if (g_umd_callback_uid != 0)
        notify_callback(runtime, ctx, g_umd_callback_uid, new_state, reason);
}

// ---------------------------------------------------------------------------
// Execution-driven time
// ---------------------------------------------------------------------------

std::uint64_t g_tick_microseconds = 64u;

void starvation_tick(Runtime &runtime, AllegrexContext &ctx) {
    // Execution-driven time: so many guest dispatches = so much PSP time.  At a raised frame rate each game step must fit into a
    // shorter vblank, but it does the same work as at 59.94 Hz; with the unscaled clock a busy step looked late, and the game ran
    // its own catch-up steps (logic only, up to 3 per frame): bursts of fast game time = rubber banding, and the extra sound was
    // cut by the audio queue (heard as speed-ups).  The clock runs slower by the same factor as the vblank (a PSP that is that
    // much faster), so a step costs the same fraction of its frame as on the real console.  RR_TICK_SCALE_OFF=1: old clock.
    static const bool scale_off = std::getenv("RR_TICK_SCALE_OFF") != nullptr;
    if (!scale_off && g_vblank_period_us < g_base_vblank_us) {
        static std::uint64_t acc = 0;   // in units of 1/g_base_vblank_us microseconds
        acc += g_tick_microseconds * g_vblank_period_us;
        g_virtual_time_us += acc / g_base_vblank_us;
        acc %= g_base_vblank_us;
    } else g_virtual_time_us += g_tick_microseconds;
    promote_expired_delays();

    ThreadRecord *thread = current_thread();
    if (thread == nullptr || thread->state != ThreadState::Running) return;
    if (!g_dispatch_enabled || !g_interrupts_enabled) return;
    const std::int32_t best = best_ready_thread();
    if (best == -1 || thread_priority(best) >= thread->priority) return;

    // Timer preemption resumes exactly at ctx.pc, not at $ra: the thread is not
    // inside a call.  set_runtime_thread_identity() makes the runtime
    // invalidate every nested native chain and re-dispatch from the new
    // context.
    if (RR_ENV_FLAG("PSPRECOMP_RIDGERACER_TRACE_PREEMPT")) {
        std::ostringstream out;
        out << "[PREEMPT] uid=" << thread->uid << " name=" << thread->name
            << " pc=" << hex32(ctx.pc)
            << " v0=" << hex32(ctx.gpr[2]) << " v1=" << hex32(ctx.gpr[3])
            << " s1=" << hex32(ctx.gpr[17])
            << " sp=" << hex32(ctx.gpr[29]) << " ra=" << hex32(ctx.gpr[31]);
        log_line(category::kDispatch, out.str());
    }
    thread->suspended = ctx;
    thread->state = ThreadState::Ready;
    thread->ready_sequence = g_ready_sequence++;
    g_ready.push_back(thread->uid);
    ++g_preempt_count;
    (void)activate_next(runtime, ctx, "timer-preempt");
}

// ---------------------------------------------------------------------------
// Guest memory helpers
// ---------------------------------------------------------------------------

[[nodiscard]] std::uint32_t arena_allocate(std::uint32_t size, std::uint32_t alignment) {
    return arena_take(g_arena, size, alignment, kUserRamTop - kModuleThreadStackSize);
}

// Gives a block back, zeroed as fresh arena memory is.
void arena_release(Runtime &runtime, std::uint32_t address, std::uint32_t size) {
    if (size == 0u) size = 16u;
    if (address == 0u || address >= g_arena.next) return;
    size = std::min(size, g_arena.next - address);
    runtime.memory().zero(address, size);
    arena_give_back(g_arena, address, size);
}
// Frees a recorded block (any allocation API); false when the id is unknown.
// Variable-size pools: first-fit sub-allocation inside the pool's block (pool uid -> start -> size).
std::map<std::int32_t, std::map<std::uint32_t, std::uint32_t>> g_vpl_chunks;

bool free_block(Runtime &runtime, std::int32_t uid) {
    const auto found = g_blocks.find(uid);
    if (found == g_blocks.end()) return false;
    g_vpl_chunks.erase(uid);
    arena_release(runtime, found->second.address, found->second.size);
    g_blocks.erase(found);
    return true;
}

[[nodiscard]] bool allocate_thread_stack(std::uint32_t size, std::uint32_t &bottom, std::uint32_t &top) {
    if (size == 0u) return false;
    const std::uint32_t aligned = (size + 0xFFu) & ~0xFFu;
    top = g_stack_next_top & ~0xFFu;
    if (top < aligned) return false;
    bottom = top - aligned;
    if (bottom < g_arena.next) return false;
    g_stack_next_top = bottom;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Filesystem
// ---------------------------------------------------------------------------
namespace {

struct FileHandle {
    std::fstream stream;
    std::string psp_path;
    std::uint64_t position{};
    bool directory{};
    bool synthetic{};
    std::vector<std::string> entries;
    std::size_t entry_index{};
    // Async I/O state (IoFileMgrForUser async family).  Operations complete
    // inline in this cooperative runtime, so a result is always ready by the
    // time the guest polls/waits; the flags keep the PSP-visible protocol.
    std::int64_t async_result{};
    bool has_async_result{};
    bool close_pending{};
    bool open_failed{};
    std::uint32_t async_callback{};
    std::uint32_t async_callback_arg{};
};

std::unordered_map<std::int32_t, FileHandle> g_files;
std::int32_t g_next_fd = 3;
bool g_first_file_logged = false;
std::unordered_set<std::string> g_reported_missing;
std::uint64_t g_open_count = 0;
std::uint64_t g_open_missing = 0;
std::uint64_t g_read_count = 0;
std::uint64_t g_read_bytes = 0;
std::uint64_t g_write_count = 0;
std::uint64_t g_write_bytes = 0;
std::uint64_t g_seek_count = 0;
std::uint64_t g_largest_read = 0;
std::uint32_t g_seek_logs = 0;
std::uint32_t g_seek_offset_logs = 0;
std::uint32_t g_read_logs = 0;
std::set<std::int32_t> g_refer_flag_logs;
std::set<std::uint64_t> g_poll_flag_logs;

// First-time path access tracking (Phase 3 milestone instrumentation).
constexpr std::size_t kTrackedPathCapacity = 64u;
std::vector<std::string> g_tracked_paths;
std::unordered_set<std::string> g_tracked_path_set;
std::uint64_t g_path_access_total = 0;

[[nodiscard]] bool track_path_first_access(const std::string &path) {
    ++g_path_access_total;
    if (!g_tracked_path_set.insert(path).second) return false;
    if (g_tracked_paths.size() < kTrackedPathCapacity) g_tracked_paths.push_back(path);
    return true;
}

void file_line(const std::string &message) { log_line(category::kFile, message); }

[[nodiscard]] std::string guest_string(Runtime &runtime, std::uint32_t address, std::size_t max = 1024u) {
    if (address == 0u || !runtime.memory().contains(address, 1u)) return std::string();
    try {
        return runtime.memory().read_c_string(address, max);
    } catch (const std::exception &) {
        return std::string("<unreadable>");
    }
}

void log_file_request(Runtime &runtime, const char *operation, const std::string &psp_path,
                      const std::string &native_path, const char *result) {
    std::ostringstream out;
    out << operation << " \"" << psp_path << "\" -> \"" << native_path << "\" [" << result << "]"
        << " thread=" << psprecomp::runtime_thread_uid();
    log_line(category::kFilesystem, out.str());
    // A movie the game is about to play: let the MPEG layer decode it for real.
    if (std::string_view(operation) == "sceIoOpen" && std::string_view(result) == "ok" && native_path.size() > 4u) {
        std::string ext = native_path.substr(native_path.size() - 4u);
        for (char &ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (ext == ".pmf") mpeg_note_movie_file(native_path);
    }
}

void fill_stat(GuestMemory &memory, std::uint32_t address, std::uint32_t mode, std::uint64_t size) {
    if (address == 0u || !memory.contains(address, 64u)) return;
    memory.zero(address, 64u);
    memory.store32(address + 0u, mode);        // st_mode
    memory.store32(address + 4u, 0x00000020u); // st_attr (FIO_S_IFREG)
    memory.store32(address + 8u, static_cast<std::uint32_t>(size));
    memory.store32(address + 12u, static_cast<std::uint32_t>(size >> 32u));
    // atime/ctime/mtime left as zero (epoch) -- only size/mode are consumed in
    // practice before the first blocker.
}

} // namespace

// ---------------------------------------------------------------------------
// HLE installation
// ---------------------------------------------------------------------------

void register_thread_return_target(Runtime &runtime) {
    runtime.register_function(0x00000000u,
        [](Runtime &rt, AllegrexContext &ctx) {
            // Two conventions land on address 0.  A thread entry point returns
            // with $ra = 0, which the kernel treats as the thread exiting.
            // Games also call address 0 as a no-op trap (`jal 0`) with a live
            // $ra -- the kernel's page 0 holds a return stub -- and that must
            // simply return to the caller.  MotorStorm's main loop uses
            // `jal 0` as its iteration terminator, so completing the thread
            // there ends the game instead of restarting the boot stage.
            if (ctx.gpr[31] != 0u) {
                static int trap_logs = 0;
                if (trap_logs < 16) {
                    ++trap_logs;
                    log_line(category::kDispatch, "address-0 no-op trap return ra=" +
                                                      hex32(ctx.gpr[31]) + " sp=" +
                                                      hex32(ctx.gpr[29]) + " a0=" +
                                                      hex32(ctx.gpr[4]));
                }
                ctx.pc = ctx.gpr[31];
                return;
            }
            const std::int32_t uid = g_current_uid;
            const std::uint32_t status = ctx.gpr[2];
            // One-shot exit trace: the saved return addresses on the stack
            // identify the outermost function that returned with $ra = 0.
            std::ostringstream trace;
            trace << "thread entry returned uid=" << uid << " status=" << hex32(status)
                  << " sp=" << hex32(ctx.gpr[29]);
            for (std::int32_t offset = 0; offset <= 0x300; offset += 4) {
                const std::uint32_t address =
                    static_cast<std::uint32_t>(static_cast<std::int32_t>(ctx.gpr[29]) + offset);
                if (!rt.memory().contains(address, 4u)) break;
                const std::uint32_t value = rt.memory().load32(address);
                if (value < 0x08804000u || value >= 0x08AC0000u || (value & 3u) != 0u) continue;
                trace << " [" << hex32(address) << "]=" << hex32(value);
            }
            log_line(category::kDispatch, trace.str());
            complete_thread(rt, ctx, uid, status, false);
        },
        "psp_thread_return");
}

// ---------------------------------------------------------------------------

// Frame-rate experiment: the game's code loads these float constants (1/60, 60, 30, 1/30) from its constant pools (found by
// scanning BOOT.BIN for lwc1 loads of those values).  With --fps-patch they are rescaled to the raised vblank rate so code
// that converts per-second values to per-frame steps uses the new frame length.  Addresses are module offsets (load base
// 0x08804000).  RR_FPS_PATCH_SKIP / RR_FPS_PATCH_ONLY ("0x1a2700,0x1b043c") narrow the set for bisecting.
void apply_fps_patch(Runtime &runtime) {
    const int level = g_hle_options.fps_patch;
    const double m = g_hle_options.fps_mult;
    if ((level <= 0 || m == 1.0) && std::getenv("RR_CONST_FACTOR") == nullptr) return;
    const auto parse = [](const char *e) {
        std::vector<std::uint32_t> out;
        if (e == nullptr) return out;
        std::string s(e);
        for (std::size_t p = 0; p < s.size();) {
            const std::size_t c = s.find(',', p);
            const std::string part = s.substr(p, c == std::string::npos ? std::string::npos : c - p);
            if (!part.empty()) out.push_back(static_cast<std::uint32_t>(std::strtoul(part.c_str(), nullptr, 0)));
            if (c == std::string::npos) break;
            p = c + 1;
        }
        return out;
    };
    const auto skip = parse(std::getenv("RR_FPS_PATCH_SKIP")), only = parse(std::getenv("RR_FPS_PATCH_ONLY"));
    struct Group { float expect; float scaled; int min_level; std::vector<std::uint32_t> addrs; };
    const float fm = static_cast<float>(m);
    const Group groups[] = {
        {1.0f / 60.0f, 1.0f / (60.0f * fm), 1, {0x1a2700,0x1a7578,0x1a75b0,0x1ac970,0x1aca08,0x1acb3c,0x1acb64,0x1acbbc,0x1acc1c,0x1acc44,0x1acc54,0x1accd8,0x1acdec,0x1ace1c,0x1acfb0,0x1ad1a4,0x1b02c4,0x1b02e4,0x1b0364,0x1b0390,0x1b03b0,0x1b03c4,0x1b03d4,0x1b0418,0x1b0430,0x1b043c,0x1b0534}},
        {60.0f, 60.0f * fm, 1, {0x1a51f8,0x1a829c,0x1ac600,0x1acb58,0x1acb7c,0x1acbc4,0x1acc08,0x1ace10,0x1acfbc,0x1ad41c,0x1ad5d8,0x1adcac,0x1adcec,0x1add14,0x1add1c,0x1add50,0x1af3e8,0x1b03b8,0x1b0f18,0x1b17ac,0x1b17c8,0x1b2fac}},
        {30.0f, 30.0f * fm, 2, {0x1a5208,0x1a7548,0x1ae414,0x1b03d0,0x1b04f4,0x1b055c,0x1b17a8}},
        {1.0f / 30.0f, 1.0f / (30.0f * fm), 2, {0x1b17c0}},
    };
    // Level 3 ("physics"): only the car physics timestep.  Every car keeps its own step in its body struct (+0x26d0 dt,
    // +0x26d4 1/dt, +0x26d8 dt/scale), written by the car constructor (0b8d24: constants 1/60 at 1aca08, 60 at 1aca0c)
    // and by three time-scale setters (0c01e8 / 0c10e8 / 0c23d8: 1/60 at 1accd8 / 1acdec / 1acfb0).  Each of those pool
    // entries is loaded by that function only, so rescaling them changes nothing else.
    const Group physics[] = {
        {1.0f / 60.0f, 1.0f / (60.0f * fm), 3, {0x1aca08, 0x1accd8, 0x1acdec, 0x1acfb0}},
        {60.0f, 60.0f * fm, 3, {0x1aca0c}},
    };
    // Level 4 ("car"): physics plus every car/driver function that uses 1/60 as a time step (each pool entry below is
    // loaded by exactly one function, checked by disassembly):
    //   steering: 0bd234 ramp rate (1acc44), 0fd6f8 / 0fd80c smoothing (1b0430 / 1b043c), 0fd184 (1b03d4)
    //   derivatives (x - x_prev) / dt: 0b6e44 (1ac970), 0bbcb0 (1acbbc), 0bd408 (1acc54), 0c1860 (1ace1c), 0fb680 (1b02e4)
    //   rates / timers (+= or -= dt): 0b73b4 (1ac9ac), 0bb480 (1acb3c), 0bb840 (1acb64), 0bc954 (1acc1c), 0c2c44 (1ad1a4),
    //     0fb3f0 (1b02c4), 0fc2e8 (1b0364), 0fc6e8 (1b0390), 0fcd88 (1b03b0), 0fcfd8 (1b03c4), 0fd184 (1b0418),
    //     0fe674 (1b0534), 03a70c (1a2700), 082a74 (1a7578), 082bfc (1a75b0)
    //   speed from the per-frame position change (|dpos| * 60): 0c145c (1ace10), 0c24e8 (1acfbc)
    // Left alone on purpose: every 60 used with 2*pi (rad/s -> rpm: patching those halved the rev limit, the 70 mph cap),
    // 60 / 30 used as thresholds, 30 used as a gain.
    // Plus one per-frame decay with no time step in it: 0fdee0 multiplies the steering target by 0.2 each frame with the
    // stick centred (1b04a0) -> 0.2^(1/mult) keeps the same decay per second.
    const Group car[] = {
        {1.0f / 60.0f, 1.0f / (60.0f * fm), 4, {0x1aca08, 0x1accd8, 0x1acdec, 0x1acfb0, 0x1acc44, 0x1b0430, 0x1b043c, 0x1b03d4,
                                                0x1ac970, 0x1acbbc, 0x1acc54, 0x1ace1c, 0x1b02e4, 0x1ac9ac, 0x1acb3c, 0x1acb64,
                                                0x1acc1c, 0x1ad1a4, 0x1b02c4, 0x1b0364, 0x1b0390, 0x1b03b0, 0x1b03c4, 0x1b0418,
                                                0x1b0534, 0x1a2700, 0x1a7578, 0x1a75b0}},
        {60.0f, 60.0f * fm, 4, {0x1aca0c, 0x1ace10, 0x1acfbc}},
        {0.2f, static_cast<float>(std::pow(0.2, 1.0 / m)), 4, {0x1b04a0}},
        // Per-step body damping in 0bc310 (the car-motion integrator, loaded by that one function only): the three words at
        // car-struct +0x100 / +0x108 / +0x390 (the pitch/roll rates) are multiplied by 0.98 / 0.95 / 0.98 every step while the
        // body is within its angle limits.  Twice the steps per second at 120 fps damped them twice as hard, so the car could
        // not pitch over the Union Hill crest and left the ground.  x^(1/mult) keeps the decay per second.
        {0.98f, static_cast<float>(std::pow(0.98, 1.0 / m)), 4, {0x1ac9d8}},
        {0.95f, static_cast<float>(std::pow(0.95, 1.0 / m)), 4, {0x1ac9dc}},
        // Landing clamp at the end of 0c1cb0 (the "wheels bottomed out" response): after the wheel impulses the car's vertical
        // velocity (car+0x1764, per second) is capped at 0.06 * car+0x26a8 * car+0x26cc, where +0x26a8 is the road-height
        // delta per STEP.  At 120 fps +0x26a8 halves but the velocity does not, so the cap cut the descent speed in half on the
        // first step the wheels touched down over Union Hill (28 -> 13) and the car lifted off the road.  This constant is
        // loaded by that one instruction only; scaling it by the step multiplier keeps the cap per second.
        {0.06f, 0.06f * fm, 4, {0x1acc6c}},
        // Throttle pedal ramp in 0x089021b8 (car + 0x1e88: +0x24 pedal travel, +0x1c = travel^2 fed to the engine): the travel moves
        // 1/12 per STEP toward the pressed amount (full throttle in 12 frames = 0.2 s on the PSP), so at 144 fps the pedal went
        // down in 0.08 s and the car launched about 2x harder off the line, compressing the rear suspension and lifting the body
        // (the "drop" at the race start).  1/12 is loaded only by that function (both ramps there).
        {1.0f / 12.0f, 1.0f / (12.0f * fm), 4, {0x1b04b8}},
        // Fixed-point state in the car integrator 0x088bc310: every step the body angles (car+0x1710..) and the position (car+0x16a0..)
        // are rounded toward zero to 1/1000 (x * 1000, trunc, * 0.001).  The per-step change is 2.4x smaller at 144 fps, so the
        // rounding ate a much bigger share of it: the car turned ~7 % less for the same yaw rate (21.5 -> 19.7 degrees in a 0.5 s
        // steer) and lost a little distance and ride height.  1000 / 0.001 are loaded only by those two roundings; scaling the grid by
        // the step multiplier keeps the rounding loss per second as on the PSP.
        {1000.0f, 1000.0f * fm, 4, {0x1ac9c8}},
        {0.001f, 0.001f / fm, 4, {0x1ac9cc}},
        // Brake pedal ramp in 0x089022a8 (same shape: +0x7c moves 1/12 per STEP toward the pressed amount), loaded only there.
        {1.0f / 12.0f, 1.0f / (12.0f * fm), 4, {0x1b04c4}},
    };
    int patched = 0, mismatched = 0;
    const auto *gbegin = level == 4 ? std::begin(car) : level == 3 ? std::begin(physics) : std::begin(groups);
    const auto *gend = level == 4 ? std::end(car) : level == 3 ? std::end(physics) : std::end(groups);
    for (const Group *gp = gbegin; gp != gend; ++gp) {
        const Group &g = *gp;
        if (level < g.min_level) continue;
        for (const std::uint32_t off : g.addrs) {
            if (std::find(skip.begin(), skip.end(), off) != skip.end()) continue;
            if (!only.empty() && std::find(only.begin(), only.end(), off) == only.end()) continue;
            const std::uint32_t addr = 0x08804000u + off;
            const std::uint32_t bits = runtime.memory().load32(addr);
            // The pool holds 59.999996 instead of 60 (1aca0c) and 0.20000002 instead of 0.2 (1b04a0): compare as floats, not bits,
            // otherwise those two stayed unpatched (body 1/dt stayed 60 at 120 fps).  RR_FPS_PATCH_EXACT=1 restores the bit compare.
            static const bool exact = std::getenv("RR_FPS_PATCH_EXACT") != nullptr;
            const float cur = std::bit_cast<float>(bits);
            const bool same = exact ? bits == std::bit_cast<std::uint32_t>(g.expect)
                                    : std::fabs(cur - g.expect) <= std::fabs(g.expect) * 1e-5f;
            if (!same) { ++mismatched; continue; }   // not the expected game build: leave it
            runtime.memory().store32(addr, std::bit_cast<std::uint32_t>(g.scaled));
            g_fps_patches.push_back({addr, bits, std::bit_cast<std::uint32_t>(g.scaled)});
            ++patched;
        }
    }
    // Experiments: RR_CONST_POW / RR_CONST_MUL / RR_CONST_DIV = "0x1ac980,0x1ac9b0" rescale those pool words by ^(1/mult), *mult, /mult.
    for (const auto &[env, mode] : {std::pair<const char *, int>{"RR_CONST_POW", 0}, {"RR_CONST_MUL", 1}, {"RR_CONST_DIV", 2}}) {
        for (const std::uint32_t off : parse(std::getenv(env))) {
            const std::uint32_t addr = 0x08804000u + off, bits = runtime.memory().load32(addr);
            const double c = std::bit_cast<float>(bits);
            static const double cf = std::getenv("RR_CONST_FACTOR") ? std::atof(std::getenv("RR_CONST_FACTOR")) : 0.0;   // overrides the fps multiplier
            const double mm = cf > 0.0 ? cf : m;
            const double v = mode == 0 ? std::pow(c, 1.0 / mm) : mode == 1 ? c * mm : c / mm;
            runtime.memory().store32(addr, std::bit_cast<std::uint32_t>(static_cast<float>(v)));
            g_fps_patches.push_back({addr, bits, std::bit_cast<std::uint32_t>(static_cast<float>(v))});
            char t[120]; std::snprintf(t, sizeof t, "CONST %s %x %g -> %g", env, off, c, v); log_line(category::kHle, t);
        }
    }
    char b[160];
    std::snprintf(b, sizeof b, "FPSPATCH level=%d mult=%.2f patched=%d constants (%d did not match and were left alone)", level, m, patched, mismatched);
    log_line(category::kHle, b);
}

void install_hle(Runtime &runtime, std::uint32_t user_arena_start, const HleOptions &options) {
    g_hle_options = options;
    if (options.trace_tools) psprecomp::set_write_watch_range(0u, 0u);   // armed later by the Delete key
    if (options.trace_tools) {
        if (const char *e = std::getenv("RR_WATCH_W")) {
            unsigned wa = 0, wl = 4;
            if (std::sscanf(e, "%x,%x", &wa, &wl) >= 1) { psprecomp::set_watch_sink(&hle_watch_sink); psprecomp::set_write_watch_range(wa, wl); }
        }
    }
    apply_fps_patch(runtime);
    arm_race_probe(runtime);
    gates::install(runtime);
    if (!options.replay_input.empty()) load_replay(options.replay_input);
    g_dump_rt = &runtime;
    g_ge_pending.clear();
    g_ge_callbacks.clear();
    g_ge_callback_id = 0;
    g_frames_presented = 0;
    g_screenshots_taken = 0;
    g_backend = std::make_unique<SoftwareBackend>(runtime.memory());
    g_gl.reset();
    // (Was inside the window branch: without a window the raised rate never took effect, so headless "240 fps" runs ran at 59.94 Hz.)
    g_fast_vblank_us = static_cast<std::uint64_t>(std::max(2000.0, std::min(33366.0, 16683.0 / std::max(0.25, std::min(8.0, g_hle_options.fps_mult)))));
    g_vblank_period_us = race_only_rate() ? g_base_vblank_us : g_fast_vblank_us;
    if (g_hle_options.window) {
        PresentOptions present{};
        present.window = true;
        present.scale = g_hle_options.window_scale;
        present.throttle = g_hle_options.throttle;
        present.frame_period_us = static_cast<int>(g_vblank_period_us); present.low_latency = g_hle_options.low_latency; present.latency_log = g_hle_options.latency_log;
        present.fullscreen = g_hle_options.fullscreen;
        if (!present_open(present)) {
            log_line(category::kBoot, "no output window available; running headless");
            g_hle_options.window = false;
        }
    }
    GpuBackend *gpu_backend = g_backend.get();
    if (g_hle_options.gpu) {
        if (!g_hle_options.window) {
            log_line(category::kBoot, "GPU renderer needs a window; using the software renderer");
            g_gpu_status = "no window";
        } else if (const GlProc proc = present_gl_open()) {
            #ifdef __EMSCRIPTEN__
            constexpr bool kGles = true;   // WebGL2 = OpenGL ES 3.0 dialect
#else
            constexpr bool kGles = false;
#endif
            auto gl = std::make_unique<GlBackend>(runtime.memory(), proc, kGles, g_hle_options.internal_scale);
            const std::string &gdesc = gl_description();
            const bool sw_gl = gdesc.find("GDI Generic") != std::string::npos || gdesc.find("Microsoft") != std::string::npos ||
                               gdesc.find("llvmpipe") != std::string::npos || gdesc.find("softpipe") != std::string::npos || gdesc.find("SwiftShader") != std::string::npos;
            if (!gdesc.empty()) log_line(category::kBoot, "OpenGL: " + gdesc + (sw_gl ? "  (SOFTWARE OpenGL - slow)" : ""));
            if (sw_gl && gl->ok()) g_gpu_status = "OpenGL is running in software (" + gdesc + "); installing or updating the graphics driver should fix it";
            if (gl->ok()) {
                log_line(category::kBoot, "GPU renderer active, internal scale " + std::to_string(gl->scale()) + "x");
                GlBackend::Enhance enhance;
                g_aa_mode = g_hle_options.aa_mode; enhance.msaa = g_hle_options.msaa; enhance.sharpen = g_hle_options.sharpen;
                enhance.mipmaps = g_hle_options.mipmaps; enhance.aniso = g_hle_options.aniso; enhance.ui_crisp = g_hle_options.ui_crisp; enhance.smooth_cutouts = g_hle_options.smooth_cutouts;
                gl->set_enhance(enhance);
                g_alpha_cutoff = g_hle_options.alpha_cutoff;
                for (const std::string &line : gl->take_events()) log_line(category::kBoot, line);
                g_gl = std::move(gl);
                gpu_backend = g_gl.get();
            } else {
                log_line(category::kBoot, "GPU renderer failed (" + gl->error() + "); using the software renderer");
                g_gpu_status = "the graphics driver could not start OpenGL 3.3 (" + gl->error() + (gdesc.empty() ? std::string() : "; driver says " + gdesc) + ")";
            }
        } else {
            log_line(category::kBoot, "no OpenGL context available; using the software renderer");
            g_gpu_status = "the graphics driver did not provide an OpenGL 3.3 context";
        }
    }
    if (g_hle_options.gpu && !g_gpu_status.empty() && g_hle_options.window && !g_rr_dev)
        show_error_box("The game could not use your graphics card through OpenGL, so it is running on the slow software renderer.\n\nReason: " + g_gpu_status +
                       "\n\nUpdating the graphics driver (from AMD, Intel or NVIDIA, not only Windows Update) usually fixes this. The game still starts.");
    static const bool no_draw = [] { const char *e = std::getenv("RR_NO_DRAW"); return e != nullptr && std::string(e) != "0"; }();   // test rig: decode the GE lists but draw nothing (fast logic tests)
    if (no_draw) g_no_draw = true;
    if (g_hle_options.frame_skip != 1 || no_draw) {
        g_skip_backend = std::make_unique<SkipBackend>(*gpu_backend);
        gpu_backend = g_skip_backend.get();
        if (g_hle_options.frame_skip > 1) present_set_cpu_pacing(true);
    }
    g_ge = std::make_unique<GeProcessor>(runtime.memory(), *gpu_backend);
#ifndef __EMSCRIPTEN__   // the browser build creates no extra threads (the page watches for stalls instead)
    if (!g_watchdog.joinable()) start_watchdog();
    // For crash and bug reports: what the game was doing (plain reads of a few numbers, safe from any thread).
    rr::report::set_state_text_provider([]() -> std::string {
        std::string t = "Frames shown: " + std::to_string(g_frames_presented);
        t += gates::scene_frame != 0u && g_frames_presented - gates::scene_frame < 5u ? " (in a race)" : " (menus / loading)";
        t += "\nFrame rate setting: " + std::to_string(g_hle_options.fps_mult * 59.94) + " fps, frame skip " + std::to_string(g_hle_options.frame_skip);
        t += ", renderer " + std::string(g_gl ? "OpenGL" : "software");
        t += "\nOnline race: " + std::string(g_net_race_sync ? "yes" : "no") + "\n";
        return t;
    });
#endif

    g_options = options;
    g_installed = true;
    g_arena.next = (user_arena_start + 0xFFu) & ~0xFFu;
    g_stack_next_top = kUserRamTop - kModuleThreadStackSize;
    psprecomp::reset_sas_hle_state();
    psprecomp::reset_audio_output2_state();

    // Re-installation (tests) starts from a clean scheduler/IO state.
    g_threads.clear();
    g_ready.clear();
    g_joiners.clear();
    g_semas.clear();
    g_event_flags.clear();
    g_mutexes.clear();
    g_callbacks.clear();
    g_callback_frames.clear();
    g_blocks.clear();
    g_arena.free.clear();
    g_files.clear();
    g_import_calls.clear();
    g_reported_missing.clear();
    g_tracked_paths.clear();
    g_tracked_path_set.clear();
    g_utility_modules.clear();
    g_umd_waiters.clear();
    g_current_uid = 0;
    g_next_thread_uid = 1;
    g_next_sema_uid = 0x100;
    g_next_event_flag_uid = 0x200;
    g_next_mutex_uid = 0x500;
    g_dispatch_enabled = true;
    g_interrupts_enabled = true;
    g_next_callback_uid = 0x300;
    g_next_block_uid = 0x400;
    g_next_fd = 3;
    g_ready_sequence = 1;
    g_virtual_time_us = 0;
    g_vb_base_vt = 0; g_vb_base_count = 0;
    g_music_next_trace = 0;
    g_ctrl_requested_cycle = g_ctrl_requested_mode = 0u;
    g_delay_sequence = 1;
    g_switch_count = 0;
    g_delay_count = 0;
    g_preempt_count = 0;
    g_deadlock_count = 0;
    g_thread_wake_logs = 0;
    g_open_count = 0;
    g_open_missing = 0;
    g_read_count = 0;
    g_read_bytes = 0;
    g_write_count = 0;
    g_write_bytes = 0;
    g_seek_count = 0;
    g_seek_offset_logs = 0;
    g_largest_read = 0;
    g_seek_logs = 0;
    g_path_access_total = 0;
    g_umd_state = kUmdPresent | kUmdReady;
    g_umd_activated = false;
    g_umd_callback_uid = 0;
    g_umd_transitions = 0;
    g_first_file_logged = false;
    g_read_logs = 0;
    g_refer_flag_logs.clear();
    g_poll_flag_logs.clear();

    // Module thread (uid 0): mirrors the PSP kernel thread that runs
    // module_start.  The bootstrap sets the actual registers before run().
    ThreadRecord module_thread{};
    module_thread.uid = 0;
    module_thread.name = "module_start";
    module_thread.priority = 32u;
    module_thread.stack_size = kModuleThreadStackSize;
    module_thread.stack_top = kUserRamTop;
    module_thread.stack_bottom = kUserRamTop - kModuleThreadStackSize;
    module_thread.kernel_context = module_thread.stack_top - 0x100u;
    module_thread.state = ThreadState::Running;
    g_threads.emplace(0, std::move(module_thread));

    runtime.memory().zero(kUserRamTop - kModuleThreadStackSize, kModuleThreadStackSize);
    register_thread_return_target(runtime);
    runtime.register_function(kCallbackReturnAddress, &callback_return, "psp_callback_return");

    const std::uint64_t tick_interval = []() {
        const char *text = std::getenv("PSPRECOMP_TIME_TICK_DISPATCHES");
        if (text == nullptr || *text == '\0') return std::uint64_t{256};
        char *end = nullptr;
        const unsigned long long value = std::strtoull(text, &end, 0);
        return end != text && *end == '\0' ? static_cast<std::uint64_t>(value) : std::uint64_t{256};
    }();
    if (tick_interval != 0u) {
    g_tick_microseconds = std::max<std::uint64_t>(1u, tick_interval / 4u);
        psprecomp::set_runtime_starvation_hook(&starvation_tick, tick_interval);
        hle_line("execution clock: interval=" + std::to_string(tick_interval) +
                 " dispatches tick_us=" + std::to_string(g_tick_microseconds));
    }


    // -----------------------------------------------------------------------
    // Kernel_Library
    // -----------------------------------------------------------------------
    register_import(runtime, "Kernel_Library", 0x092968F4u, "sceKernelCpuSuspendIntr",
        [](Runtime &, AllegrexContext &ctx) {
            const auto previous=g_interrupts_enabled?1u:0u;
            g_interrupts_enabled=false;ctx.set_gpr(2,previous);
        });
    register_import(runtime, "Kernel_Library", 0x5F10D406u, "sceKernelCpuResumeIntr",
        [](Runtime &, AllegrexContext &ctx) {
            g_interrupts_enabled=ctx.gpr[4]!=0;ctx.set_gpr(2,0u);
        });

    // -----------------------------------------------------------------------
    // SysMemUserForUser
    // -----------------------------------------------------------------------
    register_import(runtime, "SysMemUserForUser", 0x7591C7DBu, "sceKernelSetCompiledSdkVersion",
        [](Runtime &, AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    register_import(runtime, "SysMemUserForUser", 0x91DE343Cu, "sceKernelSetCompiledSdkVersion500_550",
        [](Runtime &, AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    register_import(runtime, "SysMemUserForUser", 0xF77D77CBu, "sceKernelSetCompilerVersion",
        [](Runtime &, AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    register_import(runtime, "SysMemUserForUser", 0x237DBD4Fu, "sceKernelAllocPartitionMemory",
        [](Runtime &rt, AllegrexContext &ctx) {
            const std::uint32_t name_address = ctx.gpr[5];
            const std::uint32_t size = ctx.gpr[7];
            const std::string name = name_address != 0u ? guest_string(rt, name_address, 64u) : "unnamed";
            const std::uint32_t address = arena_allocate(size, 64u);
            if (address == 0u) {
                error_line("sceKernelAllocPartitionMemory: arena exhausted for size=" + hex32(size));
                ctx.set_gpr(2, 0x800200D9u); // SCE_KERNEL_ERROR_MEMBLOCK_ALLOC_FAILED
                return;
            }
            const std::int32_t uid = g_next_block_uid++;
            g_blocks.emplace(uid, BlockRecord{name, address, size, 0u});
            std::ostringstream out;
            out << "sceKernelAllocPartitionMemory \"" << name << "\" uid=" << uid
                << " size=" << hex32(size) << " -> " << hex32(address);
            hle_line(out.str());
            ctx.set_gpr(2, static_cast<std::uint32_t>(uid));
        });
    register_import(runtime, "SysMemUserForUser", 0xB6D61D02u, "sceKernelFreePartitionMemory",
        [](Runtime &rt, AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
            ctx.set_gpr(2, free_block(rt, uid) ? 0u : 0x800200CBu);
        });
    register_import(runtime, "SysMemUserForUser", 0x9D9A5BA1u, "sceKernelGetBlockHeadAddr",
        [](Runtime &, AllegrexContext &ctx) {
            const auto found = g_blocks.find(static_cast<std::int32_t>(ctx.gpr[4]));
            ctx.set_gpr(2, found != g_blocks.end() ? found->second.address : 0u);
        });
    register_import(runtime, "SysMemUserForUser", 0xA291F107u, "sceKernelMaxFreeMemSize",
        [](Runtime &, AllegrexContext &ctx) {
            ctx.set_gpr(2, arena_free_largest(g_arena, kUserRamTop - kModuleThreadStackSize));
        });
    register_import(runtime, "SysMemUserForUser", 0xF919F628u, "sceKernelTotalFreeMemSize",
        [](Runtime &, AllegrexContext &ctx) {
            ctx.set_gpr(2, arena_free_total(g_arena, kUserRamTop - kModuleThreadStackSize));
        });
    register_import(runtime, "SysMemUserForUser", 0x13A5ABEFu, "sceKernelPrintf",
        [](Runtime &rt, AllegrexContext &ctx) {
            const std::string format = guest_string(rt, ctx.gpr[4], 512u);
            hle_line("sceKernelPrintf \"" + format + "\"");
        });
    // sceKernelAllocMemoryBlock / GetMemoryBlockAddr / FreeMemoryBlock: the
    // alternative block allocator next to the partition API.  MotorStorm asks
    // for its multi-megabyte main heap through this path.
    register_import(runtime, "SysMemUserForUser", 0xFE707FDFu, "sceKernelAllocMemoryBlock",
        [](Runtime &rt, AllegrexContext &ctx) {
            const std::uint32_t name_address = ctx.gpr[4];
            const std::uint32_t size = ctx.gpr[6];
            const std::string name = name_address != 0u ? guest_string(rt, name_address, 64u) : "unnamed";
            const std::uint32_t address = arena_allocate(size, 64u);
            if (address == 0u) {
                error_line("sceKernelAllocMemoryBlock: arena exhausted for size=" + hex32(size));
                ctx.set_gpr(2, 0x800200D9u);
                return;
            }
            const std::int32_t uid = g_next_block_uid++;
            g_blocks.emplace(uid, BlockRecord{name, address, size, 0u});
            std::ostringstream out;
            out << "sceKernelAllocMemoryBlock \"" << name << "\" uid=" << uid
                << " size=" << hex32(size) << " -> " << hex32(address);
            hle_line(out.str());
            ctx.set_gpr(2, static_cast<std::uint32_t>(uid));
        });
    register_import(runtime, "SysMemUserForUser", 0xDB83A952u, "sceKernelGetMemoryBlockAddr",
        [](Runtime &rt, AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
            const std::uint32_t address_out = ctx.gpr[5];
            const auto found = g_blocks.find(uid);
            const std::uint32_t address = found != g_blocks.end() ? found->second.address : 0u;
            if (address_out != 0u && rt.memory().contains(address_out, 4u))
                rt.memory().store32(address_out, address);
            if (found == g_blocks.end()) {
                error_line("sceKernelGetMemoryBlockAddr unknown uid=" + std::to_string(uid));
                ctx.set_gpr(2, static_cast<std::uint32_t>(-1));
                return;
            }
            std::ostringstream out;
            out << "sceKernelGetMemoryBlockAddr uid=" << uid << " -> " << hex32(address);
            hle_line(out.str());
            ctx.set_gpr(2, 0u);
        });
    register_import(runtime, "SysMemUserForUser", 0x50F61D8Au, "sceKernelFreeMemoryBlock",
        [](Runtime &rt, AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
            ctx.set_gpr(2, free_block(rt, uid) ? 0u : static_cast<std::uint32_t>(-1));
        });

    // -----------------------------------------------------------------------
    // LoadExecForUser
    // -----------------------------------------------------------------------
    register_import(runtime, "LoadExecForUser", 0x4AC57943u, "sceKernelRegisterExitCallback",
        [](Runtime &, AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    register_import(runtime, "LoadExecForUser", 0x05572A5Fu, "sceKernelExitGame",
        [](Runtime &rt, AllegrexContext &ctx) {
            log_line(category::kHle, "sceKernelExitGame called by thread " + std::to_string(g_current_uid) +
                                         " ra=" + hex32(ctx.gpr[31]) + " sp=" + hex32(ctx.gpr[29]));
            if (g_dump_rt != nullptr) {
                auto &mem = *&g_dump_rt->memory();
                std::ostringstream stack;
                stack << "GUESTSTACK exit sp=" << hex32(ctx.gpr[29]) << ":";
                for (std::uint32_t a = ctx.gpr[29]; a < ctx.gpr[29] + 0x200u && mem.contains(a, 4u); a += 4u)
                    stack << ' ' << hex32(mem.load32(a));
                log_line(category::kDispatch, stack.str());
            }
            rt.stop("Guest requested sceKernelExitGame");
        });

    // -----------------------------------------------------------------------
    // ThreadManForUser: thread life cycle
    // -----------------------------------------------------------------------
    register_import(runtime, "ThreadManForUser", 0x446D8DE6u, "sceKernelCreateThread",
        [](Runtime &rt, AllegrexContext &ctx) {
            const std::string name = ctx.gpr[4] != 0u ? guest_string(rt, ctx.gpr[4], 64u) : "unnamed";
            const std::uint32_t requested_stack = ctx.gpr[7];
            if (requested_stack < 0x200u) {
                ctx.set_gpr(2, 0x80020194u);
                return;
            }
            std::uint32_t stack_bottom = 0;
            std::uint32_t stack_top = 0;
            if (!allocate_thread_stack(requested_stack, stack_bottom, stack_top)) {
                error_line("sceKernelCreateThread: no stack space for " + hex32(requested_stack));
                ctx.set_gpr(2, 0x80020190u);
                return;
            }
            ThreadRecord record{};
            record.uid = g_next_thread_uid++;
            record.name = name;
            record.entry = ctx.gpr[5];
            record.priority = ctx.gpr[6];
            record.stack_size = (requested_stack + 0xFFu) & ~0xFFu;
            record.stack_top = stack_top;
            record.stack_bottom = stack_bottom;
            record.kernel_context = stack_top - 0x100u;
            record.state = ThreadState::Created;
            rt.memory().zero(stack_bottom, record.stack_size);
            rt.memory().store32(stack_bottom, static_cast<std::uint32_t>(record.uid));
            rt.memory().store32(record.kernel_context + 0xC0u, static_cast<std::uint32_t>(record.uid));
            rt.memory().store32(record.kernel_context + 0xC8u, stack_bottom);
            rt.memory().store32(record.kernel_context + 0xF8u, 0xFFFFFFFFu);
            rt.memory().store32(record.kernel_context + 0xFCu, 0xFFFFFFFFu);
            const std::int32_t uid = record.uid;
            std::ostringstream out;
            out << "sceKernelCreateThread uid=" << uid << " name=\"" << name << "\""
                << " entry=" << hex32(record.entry) << " priority=" << record.priority
                << " stack=" << hex32(record.stack_size) << " range="
                << hex32(stack_bottom) << "-" << hex32(stack_top);
            hle_line(out.str());
            g_threads.emplace(uid, std::move(record));
            ctx.set_gpr(2, static_cast<std::uint32_t>(uid));
        });
    register_import(runtime, "ThreadManForUser", 0xF475845Du, "sceKernelStartThread",
        [](Runtime &rt, AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
            const auto found = g_threads.find(uid);
            if (found == g_threads.end()) {
                ctx.set_gpr(2, 0x80020198u);
                return;
            }
            ThreadRecord &thread = found->second;
            // A thread that exited becomes dormant on the PSP and may be started
            // again; MotorStorm restarts its StreamThread for each new music
            // stream after the profile load.  Only a thread that has never run
            // or has completed can be started.
            if (thread.state != ThreadState::Created && thread.state != ThreadState::Completed) {
                ctx.set_gpr(2, 0x800201A4u);
                return;
            }
            if (thread.state == ThreadState::Completed) {
                thread.exit_status = 0;
                thread.suspended = AllegrexContext{};
                thread.callback_wait = false;
                thread.callback_wait_kind = CallbackWaitKind::None;
                thread.wait_has_deadline = false;
            }
            const std::uint32_t arg_size = ctx.gpr[5];
            const std::uint32_t arg_ptr = ctx.gpr[6];
            std::uint32_t sp = thread.kernel_context;
            AllegrexContext next{};
            if (arg_ptr != 0u && arg_size != 0u) {
                const std::uint32_t aligned = (arg_size + 0xFu) & ~0xFu;
                if (sp < thread.stack_bottom + aligned + 64u ||
                    !rt.memory().contains(arg_ptr, arg_size)) {
                    ctx.set_gpr(2, 0x800200D3u);
                    return;
                }
                sp -= aligned;
                std::vector<std::uint8_t> arguments(arg_size);
                rt.memory().copy_out(arg_ptr, arguments);
                rt.memory().copy_in(sp, arguments);
                next.set_gpr(4, arg_size);
                next.set_gpr(5, sp);
            } else {
                next.set_gpr(4, 0u);
                next.set_gpr(5, 0u);
            }
            sp -= 64u;
            next.set_gpr(26, thread.kernel_context);
            next.set_gpr(28, ctx.gpr[28]);
            next.set_gpr(29, sp);
            next.set_gpr(30, sp);
            next.set_gpr(31, 0u);
            next.pc = thread.entry;
            make_ready(thread, 0u);
            thread.suspended = next;
            std::ostringstream out;
            out << "sceKernelStartThread uid=" << uid << " name=\"" << thread.name << "\""
                << " entry=" << hex32(thread.entry) << " priority=" << thread.priority
                << " caller=" << g_current_uid;
            hle_line(out.str());
            if (g_dispatch_enabled && g_interrupts_enabled &&
                thread.priority < thread_priority(g_current_uid)) {
                ThreadRecord *caller = current_thread();
                // The caller resumes after this import returns, so its result
                // register must already hold the success code (the audio
                // middleware returns StartThread's result to its caller).
                ctx.set_gpr(2, 0u);
                if (caller != nullptr) {
                    caller->suspended = ctx;
                    caller->suspended.pc = ctx.gpr[31];
                    caller->state = ThreadState::Ready;
                    caller->ready_sequence = g_ready_sequence++;
                    g_ready.push_back(caller->uid);
                }
                (void)activate_next(rt, ctx, "thread-control");
            } else {
                ctx.set_gpr(2, 0u);
            }
        });
    register_import(runtime, "ThreadManForUser", 0xAA73C935u, "sceKernelExitThread",
        [](Runtime &rt, AllegrexContext &ctx) {
            const std::uint32_t status = ctx.gpr[4];
            complete_thread(rt, ctx, g_current_uid, status, false);
        });
    register_import(runtime, "ThreadManForUser", 0x809CE29Bu, "sceKernelExitDeleteThread",
        [](Runtime &rt, AllegrexContext &ctx) {
            const std::uint32_t status = ctx.gpr[4];
            complete_thread(rt, ctx, g_current_uid, status, true);
        });
    register_import(runtime, "ThreadManForUser", 0x616403BAu, "sceKernelTerminateThread",
        [](Runtime &rt, AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
            const auto found = g_threads.find(uid);
            if (found == g_threads.end()) {
                ctx.set_gpr(2, 0x80020198u);
                return;
            }
            if (uid == g_current_uid) {
                complete_thread(rt, ctx, uid, 0u, false);
                return;
            }
            complete_thread(rt, ctx, uid, 0u, false);
            ctx.set_gpr(2, 0u);
        });
    register_import(runtime, "ThreadManForUser", 0x383F7BCCu, "sceKernelTerminateDeleteThread",
        [](Runtime &rt, AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
            if (g_threads.find(uid) == g_threads.end()) {
                ctx.set_gpr(2, 0x80020198u);
                return;
            }
            if (uid == g_current_uid) {
                complete_thread(rt, ctx, uid, 0u, true);
                return;
            }
            complete_thread(rt, ctx, uid, 0u, true);
            ctx.set_gpr(2, 0u);
        });
    register_import(runtime, "ThreadManForUser", 0x9FA03CD3u, "sceKernelDeleteThread",
        [](Runtime &rt, AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
            const auto found = g_threads.find(uid);
            if (found == g_threads.end()) {
                ctx.set_gpr(2, 0x80020198u);
                return;
            }
            if (uid == g_current_uid) {
                complete_thread(rt, ctx, uid, 0u, true);
                return;
            }
            g_threads.erase(found);
            ctx.set_gpr(2, 0u);
        });
    register_import(runtime, "ThreadManForUser", 0x278C0DF5u, "sceKernelWaitThreadEnd",
        [](Runtime &rt, AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
            const std::uint32_t status_ptr = ctx.gpr[5];
            const std::uint32_t timeout_ptr = ctx.gpr[6];
            const auto found = g_threads.find(uid);
            if (found == g_threads.end()) {
                ctx.set_gpr(2, 0x80020198u);
                return;
            }
            if (found->second.state == ThreadState::Completed ||
                found->second.state == ThreadState::Created) {
                // A never-started (dormant) thread ends the wait immediately
                // with its unset exit status, exactly like an ended one.
                if (status_ptr != 0u && rt.memory().contains(status_ptr, 4u))
                    rt.memory().store32(status_ptr, found->second.exit_status);
                ctx.set_gpr(2, 0u);
                return;
            }
            // Diagnostic: log each distinct waiter -> target join so a stuck
            // shutdown can be traced to the exact thread it is waiting for.
            {
                static std::set<std::string> logged_joins;
                const std::string key = std::to_string(g_current_uid) + "->" + std::to_string(uid);
                if (logged_joins.insert(key).second)
                    hle_line("sceKernelWaitThreadEnd waiter=" + std::to_string(g_current_uid) +
                             " target=" + std::to_string(uid) + " name=\"" + found->second.name +
                             "\" state=" + std::to_string(static_cast<int>(found->second.state)));
            }
            g_joiners[uid].push_back(Joiner{g_current_uid, status_ptr});
            ThreadRecord *thread = current_thread();
            if (thread != nullptr && timeout_ptr != 0u && rt.memory().contains(timeout_ptr, 4u)) {
                const std::uint32_t timeout = rt.memory().load32(timeout_ptr);
                if (timeout != 0u) {
                    thread->wait_has_deadline = true;
                    thread->wait_deadline_us = g_virtual_time_us + timeout;
                }
            }
            (void)block_current(rt, ctx, ThreadState::Waiting, "wait-thread-end");
        });
    register_import(runtime, "ThreadManForUser", 0x293B45B8u, "sceKernelGetThreadId",
        [](Runtime &, AllegrexContext &ctx) { ctx.set_gpr(2, static_cast<std::uint32_t>(g_current_uid)); });
    register_import(runtime, "ThreadManForUser", 0x71BC9871u, "sceKernelChangeThreadPriority",
        [](Runtime &, AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
            const auto found = g_threads.find(uid);
            if (found == g_threads.end()) {
                ctx.set_gpr(2, 0x80020198u);
                return;
            }
            found->second.priority = ctx.gpr[5];
            ctx.set_gpr(2, 0u);
        });
    register_import(runtime, "ThreadManForUser", 0x94AA61EEu, "sceKernelGetThreadCurrentPriority",
        [](Runtime &, AllegrexContext &ctx) {
            const auto found = g_threads.find(g_current_uid);
            ctx.set_gpr(2, found != g_threads.end() ? found->second.priority : 32u);
        });
    register_import(runtime, "ThreadManForUser", 0xEA748E31u, "sceKernelChangeCurrentThreadAttr",
        [](Runtime &, AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    register_import(runtime, "ThreadManForUser", 0x17C1684Eu, "sceKernelReferThreadStatus",
        [](Runtime &rt, AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
            const std::uint32_t address = ctx.gpr[5];
            // SceKernelThreadInfo is exactly 0x68 bytes:
            // size@0x00, name[32]@0x04, attr@0x24, status@0x28, entry@0x2C,
            // stack@0x30, stackSize@0x34, gpReg@0x38, initPriority@0x3C,
            // currentPriority@0x40, waitType@0x44, waitId@0x48,
            // wakeupCount@0x4C, exitStatus@0x50, runClocks@0x54,
            // intrPreemptCount@0x5C, threadPreemptCount@0x60, releaseCount@0x64.
            // Over-long or shifted writes corrupt the caller's stack frame.
            constexpr std::uint32_t kThreadInfoSize = 0x68u;
            const auto found = g_threads.find(uid);
            if (found == g_threads.end() || address == 0u ||
                !rt.memory().contains(address, kThreadInfoSize)) {
                ctx.set_gpr(2, 0x80020198u);
                return;
            }
            rt.memory().zero(address, kThreadInfoSize);
            rt.memory().store32(address + 0u, kThreadInfoSize);
            const std::size_t name_length = std::min<std::size_t>(found->second.name.size(), 31u);
            for (std::size_t index = 0; index < name_length; ++index)
                rt.memory().store8(address + 4u + static_cast<std::uint32_t>(index),
                                   static_cast<std::uint8_t>(found->second.name[index]));
            const std::uint32_t status =
                found->second.state == ThreadState::Completed ? 4u
                : found->second.state == ThreadState::Running ? 1u : 2u;
            rt.memory().store32(address + 0x28u, status);
            rt.memory().store32(address + 0x2Cu, found->second.entry);
            rt.memory().store32(address + 0x30u, found->second.stack_top);
            rt.memory().store32(address + 0x34u, found->second.stack_size);
            rt.memory().store32(address + 0x3Cu, found->second.priority);
            rt.memory().store32(address + 0x40u, found->second.priority);
            rt.memory().store32(address + 0x4Cu, found->second.wakeup_count);
            rt.memory().store32(address + 0x50u, found->second.exit_status);
            ctx.set_gpr(2, 0u);
        });
    register_import(runtime, "ThreadManForUser", 0x3AD58B8Cu, "sceKernelSuspendDispatchThread",
        [](Runtime &, AllegrexContext &ctx) {
            const std::uint32_t previous = g_dispatch_enabled ? 1u : 0u;
            g_dispatch_enabled = false;
            ctx.set_gpr(2, previous);
        });
    register_import(runtime, "ThreadManForUser", 0x27E22EC2u, "sceKernelResumeDispatchThread",
        [](Runtime &, AllegrexContext &ctx) {
            g_dispatch_enabled = ctx.gpr[4] != 0u;
            ctx.set_gpr(2, 0u);
        });
    register_import(runtime, "ThreadManForUser", 0xCEADEB47u, "sceKernelDelayThread",
        [](Runtime &rt, AllegrexContext &ctx) {
            const std::uint32_t microseconds = ctx.gpr[4];
            ++g_delay_count;
            ThreadRecord *thread = current_thread();
            if (thread == nullptr) {
                rt.stop("sceKernelDelayThread with no current thread");
                return;
            }
            save_continuation(*thread, ctx);
            thread->suspended.set_gpr(2, 0u);
            thread->state = ThreadState::Delayed;
            thread->delay_until_us = g_virtual_time_us + microseconds;
            if (!activate_next(rt, ctx, "delay")) {
                ++g_deadlock_count;
                rt.stop("PSP scheduler deadlock while delaying thread");
            }
        });
    register_import(runtime, "ThreadManForUser", 0x68DA9E36u, "sceKernelDelayThreadCB",
        [](Runtime &rt, AllegrexContext &ctx) {
            const std::uint32_t microseconds = ctx.gpr[4];
            ++g_delay_count;
            ThreadRecord *thread = current_thread();
            if (thread == nullptr) {
                rt.stop("sceKernelDelayThreadCB with no current thread");
                return;
            }
            save_continuation(*thread, ctx);
            thread->suspended.set_gpr(2, 0u);
            thread->state = ThreadState::Delayed;
            thread->delay_until_us = g_virtual_time_us + microseconds;
            if (!activate_next(rt, ctx, "delay-cb")) {
                ++g_deadlock_count;
                rt.stop("PSP scheduler deadlock while delaying thread");
            }
        });
    register_import(runtime, "ThreadManForUser", 0x9ACE131Eu, "sceKernelSleepThread",
        [](Runtime &rt, AllegrexContext &ctx) {
            ThreadRecord *thread = current_thread();
            if (thread == nullptr) {
                rt.stop("sceKernelSleepThread with no current thread");
                return;
            }
            if (thread->wakeup_count != 0u) {
                --thread->wakeup_count;
                ctx.set_gpr(2, 0u);
                return;
            }
            (void)block_current(rt, ctx, ThreadState::Waiting, "sleep");
        });
    register_import(runtime, "ThreadManForUser", 0x82826F70u, "sceKernelSleepThreadCB",
        [](Runtime &rt, AllegrexContext &ctx) {
            ThreadRecord *thread = current_thread();
            if (thread == nullptr) {
                rt.stop("sceKernelSleepThreadCB with no current thread");
                return;
            }
            if (thread->wakeup_count != 0u) {
                --thread->wakeup_count;
                ctx.set_gpr(2, 0u);
                return;
            }
            (void)block_current(rt, ctx, ThreadState::Waiting, "sleep-cb");
        });
    register_import(runtime, "ThreadManForUser", 0xD59EAD2Fu, "sceKernelWakeupThread",
        [](Runtime &, AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
            const auto found = g_threads.find(uid);
            if (found == g_threads.end()) {
                ctx.set_gpr(2, static_cast<std::uint32_t>(-1));
                return;
            }
            if (found->second.state == ThreadState::Waiting) {
                wake_thread(found->second, 0u, "thread-wakeup");
                ctx.set_gpr(2, 0u);
            } else {
                ++found->second.wakeup_count;
                ctx.set_gpr(2, 0u);
            }
        });
    register_import(runtime, "ThreadManForUser", 0xFCCFAD26u, "sceKernelCancelWakeupThread",
        [](Runtime &, AllegrexContext &ctx) {
            const auto found = g_threads.find(static_cast<std::int32_t>(ctx.gpr[4]));
            if (found == g_threads.end()) {
                ctx.set_gpr(2, 0x80020198u);
                return;
            }
            const std::uint32_t previous = found->second.wakeup_count;
            found->second.wakeup_count = 0u;
            ctx.set_gpr(2, previous);
        });
    // NOTE: several ThreadManForUser NIDs in the import table (0xF8170FBE,
    // 0x3B183E26, 0x0DDCD2C9, 0x840E8133, ...) do not have a verified semantic
    // yet.  They are deliberately left unregistered so the runtime prints a
    // precise [HLE MISSING] diagnostic with module/NID/PC/RA when the guest
    // genuinely calls them.

    // -----------------------------------------------------------------------
    // ThreadManForUser: time
    // -----------------------------------------------------------------------
    register_import(runtime, "ThreadManForUser", 0xDB738F35u, "sceKernelGetSystemTime",
        [](Runtime &rt, AllegrexContext &ctx) {
            const std::uint32_t address = ctx.gpr[4];
            if (address != 0u && rt.memory().contains(address, 8u)) {
                rt.memory().store32(address, static_cast<std::uint32_t>(g_virtual_time_us));
                rt.memory().store32(address + 4u, static_cast<std::uint32_t>(g_virtual_time_us >> 32u));
            }
            ctx.set_gpr(2, 0u);
        });
    register_import(runtime, "ThreadManForUser", 0x82BC5777u, "sceKernelGetSystemTimeWide",
        [](Runtime &, AllegrexContext &ctx) {
            ctx.set_gpr(2, static_cast<std::uint32_t>(g_virtual_time_us));
            ctx.set_gpr(3, static_cast<std::uint32_t>(g_virtual_time_us >> 32u));
        });
    register_import(runtime, "ThreadManForUser", 0x369ED59Du, "sceKernelGetSystemTimeLow",
        [](Runtime &, AllegrexContext &ctx) {
            maybe_pump_window();
            ctx.set_gpr(2, static_cast<std::uint32_t>(g_virtual_time_us));
        });
    register_import(runtime, "ThreadManForUser", 0x110DEC9Au, "sceKernelUSec2SysClock",
        [](Runtime &rt, AllegrexContext &ctx) {
            const std::uint32_t address = ctx.gpr[5];
            if (address != 0u && rt.memory().contains(address, 8u)) {
                rt.memory().store32(address, ctx.gpr[4]);
                rt.memory().store32(address + 4u, 0u);
            }
            ctx.set_gpr(2, 0u);
        });
    register_import(runtime, "ThreadManForUser", 0xC8CD158Cu, "sceKernelUSec2SysClockWide",
        [](Runtime &, AllegrexContext &ctx) {
            ctx.set_gpr(2, ctx.gpr[4]);
            ctx.set_gpr(3, 0u);
        });
    register_import(runtime, "ThreadManForUser", 0xBA6B92E2u, "sceKernelSysClock2USec",
        [](Runtime &rt, AllegrexContext &ctx) {
            const std::uint32_t clock_address = ctx.gpr[4];
            const std::uint32_t seconds_address = ctx.gpr[5];
            const std::uint32_t usec_address = ctx.gpr[6];
            std::uint64_t clock = 0u;
            if (clock_address != 0u && rt.memory().contains(clock_address, 8u))
                clock = rt.memory().load32(clock_address) |
                        (static_cast<std::uint64_t>(rt.memory().load32(clock_address + 4u)) << 32u);
            if (seconds_address != 0u && rt.memory().contains(seconds_address, 4u))
                rt.memory().store32(seconds_address, static_cast<std::uint32_t>(clock / 1'000'000ull));
            if (usec_address != 0u && rt.memory().contains(usec_address, 4u))
                rt.memory().store32(usec_address, static_cast<std::uint32_t>(clock % 1'000'000ull));
            ctx.set_gpr(2, 0u);
        });
    register_import(runtime, "ThreadManForUser", 0xE1619D7Cu, "sceKernelSysClock2USecWide",
        [](Runtime &, AllegrexContext &ctx) {
            ctx.set_gpr(2, static_cast<std::uint32_t>(ctx.gpr[4] / 1'000'000u));
            ctx.set_gpr(3, static_cast<std::uint32_t>(ctx.gpr[4] % 1'000'000u));
        });

    // -----------------------------------------------------------------------
    // ThreadManForUser: callbacks
    // -----------------------------------------------------------------------
    register_import(runtime, "ThreadManForUser", 0xE81CAF8Fu, "sceKernelCreateCallback",
        [](Runtime &rt, AllegrexContext &ctx) {
            CallbackRecord record{};
            record.name = ctx.gpr[4] != 0u ? guest_string(rt, ctx.gpr[4], 64u) : "callback";
            record.entry = ctx.gpr[5];
            record.arg = ctx.gpr[6];
            record.owner_uid = g_current_uid;
            const std::int32_t uid = g_next_callback_uid++;
            std::ostringstream out;
            out << "sceKernelCreateCallback uid=" << uid << " name=\"" << record.name
                << "\" entry=" << hex32(record.entry) << " arg=" << hex32(record.arg);
            hle_line(out.str());
            g_callbacks.emplace(uid, std::move(record));
            ctx.set_gpr(2, static_cast<std::uint32_t>(uid));
        });
    register_import(runtime, "ThreadManForUser", 0xEDBA5844u, "sceKernelDeleteCallback",
        [](Runtime &rt, AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
            const auto removed = g_callbacks.erase(uid);
            if (removed == 1u) {
                g_callback_frames.erase(uid);
                if (g_umd_callback_uid == uid) {
                    g_umd_callback_uid = 0;
                    log_line(category::kUmd, "callback uid=" + std::to_string(uid) +
                                                 " deleted; UMD callback unregistered");
                }
                hle_line("sceKernelDeleteCallback uid=" + std::to_string(uid));
                ctx.set_gpr(2, 0u);
            } else {
                ctx.set_gpr(2, 0x800201A6u);
            }
        });
    register_import(runtime, "ThreadManForUser", 0x349D6D6Cu, "sceKernelCheckCallback",
        [](Runtime &rt, AllegrexContext &ctx) {
            maybe_pump_window();
            if (std::getenv("RR_NETSTUB") != nullptr) {   // study aid: show the game's network-module state now and then
                static std::uint32_t last0 = 0xffffffffu, last8 = 0xffffffffu, lastc = 0xffffffffu, ticks = 0u;
                auto &mm = rt.memory();
                const std::uint32_t o = 0x08A45E98u, st = mm.load32(o), fl = mm.load32(o + 8u), er = mm.load32(o + 0xcu);
                if ((st != last0 || fl != last8 || er != lastc) && (++ticks, true)) {
                    last0 = st; last8 = fl; lastc = er;
                    char text[160];
                    std::snprintf(text, sizeof text, "net module object: state=%u flags=0x%X error=0x%08X (channel %u, busy %u)", st, fl, er,
                                  mm.load32(o + 0x14u), static_cast<unsigned>(mm.load8(o + 0x19u)));
                    log_line("NET", text);
                }
            }
            if (service_owned_callback(rt,ctx,0,true)) return;
            ctx.set_gpr(2, 0u);
            if (ge_deliver(rt, ctx, 0u)) return;
            if (net_deliver(rt, ctx)) return;
            // Watch self-test (PSPRECOMP_RIDGERACER_PROBE=1): write a canary to
            // the scene-object pointer so a write watch on it can be shown to
            // work before trusting a "no writes" result.
            if (RR_ENV_FLAG("PSPRECOMP_RIDGERACER_PROBE")) {
                static bool probed = false;
                if (!probed) {
                    probed = true;
                    rt.memory().store32(0x08AAE48Cu, 0xC0FFEE00u);
                    log_line(category::kGe, "probe wrote 0xC0FFEE00 to 0x08AAE48C");
                }
            }
            ctx.set_gpr(2, 0u);
        });

    // -----------------------------------------------------------------------
    // ThreadManForUser: semaphores
    // -----------------------------------------------------------------------
    register_import(runtime, "ThreadManForUser", 0xD6DA4BA1u, "sceKernelCreateSema",
        [](Runtime &rt, AllegrexContext &ctx) {
            SemaRecord record{};
            record.name = ctx.gpr[4] != 0u ? guest_string(rt, ctx.gpr[4], 64u) : "sema";
            record.count = static_cast<std::int32_t>(ctx.gpr[6]);
            record.initial_count = record.count;
            record.maximum = static_cast<std::int32_t>(ctx.gpr[7]);
            const std::int32_t uid = g_next_sema_uid++;
            std::ostringstream out;
            out << "sceKernelCreateSema uid=" << uid << " name=\"" << record.name
                << "\" initial=" << record.count << " max=" << record.maximum;
            hle_line(out.str());
            g_semas.emplace(uid, std::move(record));
            ctx.set_gpr(2, static_cast<std::uint32_t>(uid));
        });
    register_import(runtime, "ThreadManForUser", 0x28B6489Cu, "sceKernelDeleteSema",
        [](Runtime &, AllegrexContext &ctx) {
            const auto found = g_semas.find(static_cast<std::int32_t>(ctx.gpr[4]));
            if (found == g_semas.end()) {
                ctx.set_gpr(2, 0x80020199u);
                return;
            }
            for (const SemaWaiter &waiter : found->second.waiters) {
                const auto thread = g_threads.find(waiter.uid);
                if (thread != g_threads.end()) wake_thread(thread->second, 0x800201A7u, "sema-deleted");
            }
            g_semas.erase(found);
            ctx.set_gpr(2, 0u);
        });
    register_import(runtime, "ThreadManForUser", 0x3F53E640u, "sceKernelSignalSema",
        [](Runtime &, AllegrexContext &ctx) {
            const auto found = g_semas.find(static_cast<std::int32_t>(ctx.gpr[4]));
            const auto amount = static_cast<std::int32_t>(ctx.gpr[5]);
            if (found == g_semas.end() || amount <= 0 ||
                static_cast<std::int64_t>(found->second.count) + amount > found->second.maximum) {
                ctx.set_gpr(2, 0x80020199u);
                return;
            }
            SemaRecord &sema = found->second;
            sema.count += amount;
            if (sema.name == "pthread mutex" && sema.maximum == 1) sema.mutex_owner = -1;
            // Wake waiters by PSP priority, then arrival order.
            std::sort(sema.waiters.begin(), sema.waiters.end(),
                [](const SemaWaiter &left, const SemaWaiter &right) {
                    const std::uint32_t left_priority = thread_priority(left.uid);
                    const std::uint32_t right_priority = thread_priority(right.uid);
                    if (left_priority != right_priority) return left_priority < right_priority;
                    return left.uid < right.uid;
                });
            auto waiter = sema.waiters.begin();
            while (waiter != sema.waiters.end() && sema.count >= waiter->amount) {
                sema.count -= waiter->amount;
                if (sema.name == "pthread mutex" && sema.maximum == 1)
                    sema.mutex_owner = waiter->uid;
                const auto thread = g_threads.find(waiter->uid);
                if (thread != g_threads.end()) wake_thread(thread->second, 0u, "sema-signal");
                waiter = sema.waiters.erase(waiter);
            }
            ctx.set_gpr(2, 0u);
        });
    auto semaphore_wait = [](Runtime &rt, AllegrexContext &ctx, bool callbacks) {
        if (callbacks && service_owned_callback(rt,ctx,ctx.pc)) return;
        if (auto *thread=current_thread()) {
            thread->callback_wait=callbacks;thread->callback_wait_pc=ctx.pc;
            thread->callback_wait_kind=callbacks?CallbackWaitKind::Semaphore:CallbackWaitKind::None;
            thread->callback_wait_object=static_cast<std::int32_t>(ctx.gpr[4]);
        }
        const auto found = g_semas.find(static_cast<std::int32_t>(ctx.gpr[4]));
        const auto amount = static_cast<std::int32_t>(ctx.gpr[5]);
        if (found == g_semas.end() || amount <= 0 || amount > found->second.maximum) {
            ctx.set_gpr(2, 0x80020199u);
            return;
        }
        if (found->second.count >= amount) {
            if (auto *thread=current_thread()) {thread->callback_wait=false;thread->callback_wait_kind=CallbackWaitKind::None;}
            found->second.count -= amount;
            if (found->second.name == "pthread mutex" && found->second.maximum == 1)
                found->second.mutex_owner = g_current_uid;
            ctx.set_gpr(2, 0u);
            return;
        }
        if (found->second.name=="pthread mutex" && found->second.mutex_owner==g_current_uid) {
            std::ostringstream out;
            out << "self-wait sema=" << found->first << " thread=" << g_current_uid
                << " ra=" << hex32(ctx.gpr[31]) << " guest_mutex=" << hex32(ctx.gpr[18])
                << " self=" << hex32(ctx.gpr[20]) << " sp=" << hex32(ctx.gpr[29]);
            if (rt.memory().contains(ctx.gpr[18],24))
                for (std::uint32_t i=0;i<6;++i)
                    out << " word" << i << '=' << hex32(rt.memory().load32(ctx.gpr[18]+i*4));
            if (rt.memory().contains(ctx.gpr[29]+0x6C,4))
                out << " caller=" << hex32(rt.memory().load32(ctx.gpr[29]+0x6C));
            log_line(category::kDispatch,out.str());
        }
        found->second.waiters.push_back(SemaWaiter{g_current_uid, amount});
        (void)block_current(rt, ctx, ThreadState::Waiting,
                            ("wait-sema-" + std::to_string(found->first)).c_str());
    };
    register_import(runtime, "ThreadManForUser", 0x4E3A1105u, "sceKernelWaitSema",
        [semaphore_wait](Runtime &rt,AllegrexContext &ctx){semaphore_wait(rt,ctx,false);});
    register_import(runtime, "ThreadManForUser", 0x6D212BACu, "sceKernelWaitSemaCB",
        [semaphore_wait](Runtime &rt,AllegrexContext &ctx){semaphore_wait(rt,ctx,true);});
    register_import(runtime, "ThreadManForUser", 0x58B1F937u, "sceKernelPollSema",
        [](Runtime &, AllegrexContext &ctx) {
            const auto found = g_semas.find(static_cast<std::int32_t>(ctx.gpr[4]));
            const auto amount = static_cast<std::int32_t>(ctx.gpr[5]);
            if (found == g_semas.end() || amount <= 0 || amount > found->second.maximum) {
                ctx.set_gpr(2, 0x80020199u);
                return;
            }
            if (found->second.count < amount) {
                ctx.set_gpr(2, 0x800201AEu);
                return;
            }
            found->second.count -= amount;
            if (found->second.name == "pthread mutex" && found->second.maximum == 1)
                found->second.mutex_owner = g_current_uid;
            ctx.set_gpr(2, 0u);
        });

    // -----------------------------------------------------------------------
    // ThreadManForUser: event flags
    // -----------------------------------------------------------------------
    register_import(runtime, "ThreadManForUser", 0x55C20A00u, "sceKernelCreateEventFlag",
        [](Runtime &rt, AllegrexContext &ctx) {
            EventFlagRecord record{};
            record.name = ctx.gpr[4] != 0u ? guest_string(rt, ctx.gpr[4], 64u) : "event_flag";
            record.current_pattern = ctx.gpr[6];
            record.initial_pattern = ctx.gpr[6];
            const std::int32_t uid = g_next_event_flag_uid++;
            std::ostringstream out;
            out << "sceKernelCreateEventFlag uid=" << uid << " name=\"" << record.name
                << "\" initial=" << hex32(record.current_pattern);
            hle_line(out.str());
            g_event_flags.emplace(uid, std::move(record));
            ctx.set_gpr(2, static_cast<std::uint32_t>(uid));
        });
    register_import(runtime, "ThreadManForUser", 0xEF9E4C70u, "sceKernelDeleteEventFlag",
        [](Runtime &, AllegrexContext &ctx) {
            const auto found = g_event_flags.find(static_cast<std::int32_t>(ctx.gpr[4]));
            if (found == g_event_flags.end()) {
                ctx.set_gpr(2, 0x8002019Au);
                return;
            }
            for (const FlagWaiter &waiter : found->second.waiters) {
                const auto thread = g_threads.find(waiter.uid);
                if (thread != g_threads.end()) wake_thread(thread->second, 0x800201A7u, "event-flag-deleted");
            }
            g_event_flags.erase(found);
            ctx.set_gpr(2, 0u);
        });
    register_import(runtime, "ThreadManForUser", 0x1FB15A32u, "sceKernelSetEventFlag",
        [](Runtime &rt, AllegrexContext &ctx) {
            const auto found = g_event_flags.find(static_cast<std::int32_t>(ctx.gpr[4]));
            if (found == g_event_flags.end()) {
                ctx.set_gpr(2, 0x8002019Au);
                return;
            }
            EventFlagRecord &flag = found->second;
            signal_event_flag_bits(rt, flag, ctx.gpr[5]);
            ctx.set_gpr(2, 0u);
        });
    register_import(runtime, "ThreadManForUser", 0x812346E4u, "sceKernelClearEventFlag",
        [](Runtime &, AllegrexContext &ctx) {
            const auto found = g_event_flags.find(static_cast<std::int32_t>(ctx.gpr[4]));
            if (found == g_event_flags.end()) {
                ctx.set_gpr(2, 0x8002019Au);
                return;
            }
            // Unlike WAITCLEAR, ClearEventFlag ANDs the supplied retain mask.
            // Callers pass ~bits to remove those bits, or zero to clear all.
            static const bool trace_flags = std::getenv("PSPRECOMP_RIDGERACER_TRACE_FLAGS") != nullptr;
            if (trace_flags) {
                static std::uint64_t trace_count = 0;
                if (trace_count++ < 20000u)
                    log_line("FLAGS", "clear uid=" + std::to_string(found->first) + " name=\"" +
                                          found->second.name + "\" mask=" + hex32(ctx.gpr[5]) +
                                          " pattern=" + hex32(found->second.current_pattern) +
                                          " thread=" + std::to_string(g_current_uid));
            }
            found->second.current_pattern &= ctx.gpr[5];
            ctx.set_gpr(2, 0u);
        });
    auto event_flag_wait = [](Runtime &rt, AllegrexContext &ctx, bool callbacks) {
        const auto found = g_event_flags.find(static_cast<std::int32_t>(ctx.gpr[4]));
        if (found == g_event_flags.end()) {
            ctx.set_gpr(2, 0x8002019Au);
            return;
        }
        EventFlagRecord &flag = found->second;
        if (callbacks && service_owned_callback(rt,ctx,ctx.pc)) return;
        const std::uint32_t requested = ctx.gpr[5];
        const std::uint32_t mode = ctx.gpr[6];
        const std::uint32_t result_ptr = ctx.gpr[7];
        const std::uint32_t timeout_ptr = ctx.gpr[8];
        {
            // Rate-limit the wait trace: a correctly parked thread logs a few
            // entries, while a misspinning consumer cannot flood the log.
            static std::unordered_map<std::string, std::uint64_t> wait_counts;
            static const bool trace_flags = std::getenv("PSPRECOMP_RIDGERACER_TRACE_FLAGS") != nullptr;
            const std::string key = std::to_string(found->first) + ":" + flag.name;
            const std::uint64_t count = ++wait_counts[key];
            if (trace_flags || count <= 4u || count % 4096u == 0u) {
                std::ostringstream out;
                out << "sceKernelWaitEventFlag uid=" << found->first << " name=\"" << flag.name
                    << "\" bits=" << hex32(requested) << " mode=" << hex32(mode)
                    << " current=" << hex32(flag.current_pattern)
                    << " attempt=" << count
                    << " ra=" << hex32(ctx.gpr[31]);
                hle_line(out.str());
            }
        }
        const bool any = (mode & 1u) != 0u;
        const bool matches = any ? (flag.current_pattern & requested) != 0u
                                 : (flag.current_pattern & requested) == requested;
        if (matches) {
            if (ThreadRecord *thread=current_thread()) {thread->callback_wait=false;thread->callback_wait_kind=CallbackWaitKind::None;}
            if (result_ptr != 0u && rt.memory().contains(result_ptr, 4u))
                rt.memory().store32(result_ptr, flag.current_pattern);
            if ((mode & 0x20u) != 0u) flag.current_pattern &= ~requested;
            if ((mode & 0x10u) != 0u) flag.current_pattern = 0u;
            ctx.set_gpr(2, 0u);
            return;
        }
        if (ThreadRecord *thread=current_thread()) {
            thread->callback_wait=callbacks;thread->callback_wait_pc=ctx.pc;
            thread->callback_wait_kind=callbacks?CallbackWaitKind::EventFlag:CallbackWaitKind::None;
            thread->callback_wait_object=static_cast<std::int32_t>(ctx.gpr[4]);
        }
        flag.waiters.push_back(FlagWaiter{g_current_uid, requested, mode, result_ptr});
        ThreadRecord *thread = current_thread();
        if (thread != nullptr && timeout_ptr != 0u && rt.memory().contains(timeout_ptr, 4u)) {
            const std::uint32_t timeout = rt.memory().load32(timeout_ptr);
            if (timeout != 0u) {
                thread->wait_has_deadline = true;
                thread->wait_deadline_us = g_virtual_time_us + timeout;
            }
        }
        (void)block_current(rt, ctx, ThreadState::Waiting, "wait-event-flag");
    };
    register_import(runtime, "ThreadManForUser", 0x402FCF22u, "sceKernelWaitEventFlag",
        [event_flag_wait](Runtime &rt,AllegrexContext &ctx){event_flag_wait(rt,ctx,false);});
    register_import(runtime, "ThreadManForUser", 0x328C546Au, "sceKernelWaitEventFlagCB",
        [event_flag_wait](Runtime &rt,AllegrexContext &ctx){event_flag_wait(rt,ctx,true);});
    register_import(runtime, "ThreadManForUser", 0x30FD48F0u, "sceKernelPollEventFlag",
        [](Runtime &rt, AllegrexContext &ctx) {
            const auto found = g_event_flags.find(static_cast<std::int32_t>(ctx.gpr[4]));
            if (found == g_event_flags.end()) {
                ctx.set_gpr(2, 0x8002019Au);
                return;
            }
            EventFlagRecord &flag = found->second;
            const std::uint32_t requested = ctx.gpr[5];
            const std::uint32_t mode = ctx.gpr[6];
            const std::uint32_t result_ptr = ctx.gpr[7];
            const std::uint64_t key = (static_cast<std::uint64_t>(ctx.gpr[4]) << 32u) |
                (static_cast<std::uint64_t>(requested) << 8u) | (mode & 0xFFu);
            if (g_poll_flag_logs.size() < 16u && g_poll_flag_logs.insert(key).second) {
                std::ostringstream out;
                out << "sceKernelPollEventFlag uid=" << ctx.gpr[4] << " name=\"" << flag.name
                    << "\" bits=" << hex32(requested) << " mode=" << hex32(mode)
                    << " current=" << hex32(flag.current_pattern)
                    << " ra=" << hex32(ctx.gpr[31]);
                log_line(category::kDispatch, out.str());
            }
            const bool any = (mode & 1u) != 0u;
            const bool matches = any ? (flag.current_pattern & requested) != 0u
                                     : (flag.current_pattern & requested) == requested;
            if (!matches) {
                ctx.set_gpr(2, 0x800201A8u);
                return;
            }
            if (result_ptr != 0u && rt.memory().contains(result_ptr, 4u))
                rt.memory().store32(result_ptr, flag.current_pattern);
            if ((mode & 0x20u) != 0u) flag.current_pattern &= ~requested;
            if ((mode & 0x10u) != 0u) flag.current_pattern = 0u;
            ctx.set_gpr(2, 0u);
        });

    // -----------------------------------------------------------------------
    // ThreadManForUser: mutexes (NID mapping verified against PPSSPP)
    // -----------------------------------------------------------------------
    register_import(runtime, "ThreadManForUser", 0xB7D098C6u, "sceKernelCreateMutex",
        [](Runtime &rt, AllegrexContext &ctx) {
            MutexRecord record{};
            record.name = ctx.gpr[4] != 0u ? guest_string(rt, ctx.gpr[4], 64u) : "mutex";
            const auto initial_count = static_cast<std::int32_t>(ctx.gpr[6]);
            record.lock_count = initial_count;
            record.owner_uid = initial_count > 0 ? g_current_uid : -1;
            const std::int32_t uid = g_next_mutex_uid++;
            std::ostringstream out;
            out << "sceKernelCreateMutex uid=" << uid << " name=\"" << record.name
                << "\" attr=" << hex32(ctx.gpr[5]) << " initial=" << initial_count;
            hle_line(out.str());
            g_mutexes.emplace(uid, std::move(record));
            ctx.set_gpr(2, static_cast<std::uint32_t>(uid));
        });
    auto lock_mutex_fn = [](Runtime &rt, AllegrexContext &ctx) {
        const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
        const auto count = static_cast<std::int32_t>(ctx.gpr[5]);
        const auto found = g_mutexes.find(uid);
        if (found == g_mutexes.end() || count <= 0) {
            ctx.set_gpr(2, 0x800201C3u); // SCE_KERNEL_ERROR_MUTEX_NOT_FOUND
            return;
        }
        MutexRecord &mutex = found->second;
        if (mutex.owner_uid == g_current_uid || mutex.lock_count == 0) {
            mutex.lock_count += count;
            mutex.owner_uid = g_current_uid;
            ctx.set_gpr(2, 0u);
            return;
        }
        mutex.waiters.push_back(MutexWaiter{g_current_uid, count});
        (void)block_current(rt, ctx, ThreadState::Waiting, "mutex-lock");
    };
    register_import(runtime, "ThreadManForUser", 0xB011B11Fu, "sceKernelLockMutex", lock_mutex_fn);
    register_import(runtime, "ThreadManForUser", 0x5BF4DD27u, "sceKernelLockMutexCB", lock_mutex_fn);
    register_import(runtime, "ThreadManForUser", 0x0DDCD2C9u, "sceKernelTryLockMutex",
        [](Runtime &, AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
            const auto count = static_cast<std::int32_t>(ctx.gpr[5]);
            const auto found = g_mutexes.find(uid);
            if (found == g_mutexes.end() || count <= 0) {
                ctx.set_gpr(2, 0x800201C3u);
                return;
            }
            MutexRecord &mutex = found->second;
            if (mutex.owner_uid == g_current_uid || mutex.lock_count == 0) {
                mutex.lock_count += count;
                mutex.owner_uid = g_current_uid;
                ctx.set_gpr(2, 0u);
            } else {
                ctx.set_gpr(2, 0x800201C4u); // SCE_KERNEL_ERROR_MUTEX_LOCKED
            }
        });
    register_import(runtime, "ThreadManForUser", 0x6B30100Fu, "sceKernelUnlockMutex",
        [](Runtime &, AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
            const auto count = static_cast<std::int32_t>(ctx.gpr[5]);
            const auto found = g_mutexes.find(uid);
            if (found == g_mutexes.end() || count <= 0) {
                ctx.set_gpr(2, 0x800201C3u);
                return;
            }
            MutexRecord &mutex = found->second;
            if (mutex.lock_count == 0 || mutex.owner_uid != g_current_uid) {
                ctx.set_gpr(2, 0x800201C5u); // SCE_KERNEL_ERROR_MUTEX_UNLOCKED
                return;
            }
            mutex.lock_count -= count;
            if (mutex.lock_count <= 0) {
                mutex.lock_count = 0;
                mutex.owner_uid = -1;
                if (!mutex.waiters.empty()) {
                    const MutexWaiter next = mutex.waiters.front();
                    mutex.waiters.erase(mutex.waiters.begin());
                    mutex.owner_uid = next.uid;
                    mutex.lock_count = next.count;
                    const auto thread = g_threads.find(next.uid);
                    if (thread != g_threads.end()) wake_thread(thread->second, 0u, "mutex-handoff");
                }
            }
            ctx.set_gpr(2, 0u);
        });
    register_import(runtime, "ThreadManForUser", 0xF8170FBEu, "sceKernelDeleteMutex",
        [](Runtime &, AllegrexContext &ctx) {
            const auto found = g_mutexes.find(static_cast<std::int32_t>(ctx.gpr[4]));
            if (found == g_mutexes.end()) {
                ctx.set_gpr(2, 0x800201C3u);
                return;
            }
            for (const MutexWaiter &waiter : found->second.waiters) {
                const auto thread = g_threads.find(waiter.uid);
                if (thread != g_threads.end()) wake_thread(thread->second, 0x800201A7u, "mutex-deleted");
            }
            g_mutexes.erase(found);
            ctx.set_gpr(2, 0u);
        });

    // -----------------------------------------------------------------------
    // ThreadManForUser: remaining imported queries / control
    // -----------------------------------------------------------------------
    register_import(runtime, "ThreadManForUser", 0x840E8133u, "sceKernelWaitThreadEndCB",
        [](Runtime &rt, AllegrexContext &ctx) {
            // Same semantics as sceKernelWaitThreadEnd; the CB variant only allows
            // callbacks while waiting.
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
            const std::uint32_t status_ptr = ctx.gpr[5];
            if (service_owned_callback(rt,ctx,ctx.pc)) return;
            const auto found = g_threads.find(uid);
            if (found == g_threads.end()) {
                ctx.set_gpr(2, 0x80020198u);
                return;
            }
            // A never-started (dormant) thread resumes the waiter immediately,
            // same as an ended one (see sceKernelWaitThreadEnd).
            if (found->second.state == ThreadState::Completed ||
                found->second.state == ThreadState::Created) {
                if (status_ptr != 0u && rt.memory().contains(status_ptr, 4u))
                    rt.memory().store32(status_ptr, found->second.exit_status);
                if (auto *thread=current_thread()) {
                    thread->callback_wait=false;
                    thread->callback_wait_kind=CallbackWaitKind::None;
                }
                ctx.set_gpr(2, 0u);
                return;
            }
            // Diagnostic: log each distinct waiter -> target join so a stuck
            // shutdown can be traced to the exact thread it is waiting for.
            {
                static std::set<std::string> logged_joins;
                const std::string key = std::to_string(g_current_uid) + "->" + std::to_string(uid);
                if (logged_joins.insert(key).second)
                    hle_line("sceKernelWaitThreadEndCB waiter=" + std::to_string(g_current_uid) +
                             " target=" + std::to_string(uid) + " name=\"" + found->second.name +
                             "\" state=" + std::to_string(static_cast<int>(found->second.state)));
            }
            if (auto *thread=current_thread()) {
                thread->callback_wait=true;
                thread->callback_wait_pc=ctx.pc;
                thread->callback_wait_kind=CallbackWaitKind::ThreadEnd;
                thread->callback_wait_object=uid;
                const auto timeout_ptr=ctx.gpr[6];
                if (timeout_ptr!=0 && rt.memory().contains(timeout_ptr,4)) {
                    const auto timeout=rt.memory().load32(timeout_ptr);
                    if (timeout!=0) {
                        thread->wait_has_deadline=true;
                        thread->wait_deadline_us=g_virtual_time_us+timeout;
                    }
                }
            }
            g_joiners[uid].push_back(Joiner{g_current_uid, status_ptr});
            (void)block_current(rt, ctx, ThreadState::Waiting, "wait-thread-end-cb");
        });
    register_import(runtime, "ThreadManForUser", 0x3B183E26u, "sceKernelGetThreadExitStatus",
        [](Runtime &rt, AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
            const std::uint32_t status_ptr = ctx.gpr[5];
            const auto found = g_threads.find(uid);
            if (found == g_threads.end()) {
                ctx.set_gpr(2, 0x80020198u);
                return;
            }
            if (found->second.state != ThreadState::Completed) {
                ctx.set_gpr(2, 0x800201A4u); // not dormant
                return;
            }
            if (status_ptr != 0u && rt.memory().contains(status_ptr, 4u))
                rt.memory().store32(status_ptr, found->second.exit_status);
            ctx.set_gpr(2, 0u);
        });
    register_import(runtime, "ThreadManForUser", 0xA66B0120u, "sceKernelReferEventFlagStatus",
        [](Runtime &rt, AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
            const std::uint32_t address = ctx.gpr[5];
            // SceKernelEventFlagInfo is exactly 0x34 bytes:
            // size@0x00, name[32]@0x04, attr@0x24, initPattern@0x28,
            // currentPattern@0x2C, numWaitThreads@0x30.  Writing more than
            // that (or a shifted layout) corrupts the caller's stack frame:
            // the movie library passes its own 52-byte frame as the info
            // struct, so an over-long write zeroed the saved $ra and the
            // thread died with pc=0.
            constexpr std::uint32_t kEventFlagInfoSize = 0x34u;
            const auto found = g_event_flags.find(uid);
            if (found != g_event_flags.end() && g_refer_flag_logs.size() < 32u &&
                g_refer_flag_logs.insert(uid).second) {
                std::ostringstream out;
                out << "sceKernelReferEventFlagStatus uid=" << uid << " name=\""
                    << found->second.name << "\" pattern=" << hex32(found->second.current_pattern)
                    << " ra=" << hex32(ctx.gpr[31]);
                log_line(category::kDispatch, out.str());
            }
            if (found == g_event_flags.end() || address == 0u ||
                !rt.memory().contains(address, kEventFlagInfoSize)) {
                ctx.set_gpr(2, 0x8002019Au);
                return;
            }
            rt.memory().zero(address, kEventFlagInfoSize);
            rt.memory().store32(address + 0u, kEventFlagInfoSize);
            const std::size_t length = std::min<std::size_t>(found->second.name.size(), 31u);
            for (std::size_t index = 0; index < length; ++index)
                rt.memory().store8(address + 4u + static_cast<std::uint32_t>(index),
                                   static_cast<std::uint8_t>(found->second.name[index]));
            rt.memory().store32(address + 0x24u, 0u); // attr
            rt.memory().store32(address + 0x28u, found->second.initial_pattern);
            rt.memory().store32(address + 0x2Cu, found->second.current_pattern);
            rt.memory().store32(address + 0x30u,
                                static_cast<std::uint32_t>(found->second.waiters.size()));
            ctx.set_gpr(2, 0u);
        });
    register_import(runtime, "ThreadManForUser", 0xC11BA8C4u, "sceKernelNotifyCallback",
        [](Runtime &rt, AllegrexContext &ctx) {
            const auto uid = static_cast<std::int32_t>(ctx.gpr[4]);
            if (!g_callbacks.contains(uid)) {
                error_line("sceKernelNotifyCallback: unknown callback uid=" + std::to_string(uid));
                ctx.set_gpr(2, 0x800201A6u);
                return;
            }
            const std::uint32_t notify_arg = ctx.gpr[5];
            // Return value observed by the interrupted guest code once the
            // callback returns into the trampoline.
            ctx.set_gpr(2, 0u);
            queue_owned_callback(uid,notify_arg);
        });
    register_import(runtime, "ThreadManForUser", 0x8FFDF9A2u, "sceKernelCancelSema",
        [](Runtime &rt, AllegrexContext &ctx) {
            const auto found = g_semas.find(static_cast<std::int32_t>(ctx.gpr[4]));
            if (found == g_semas.end()) {
                ctx.set_gpr(2, 0x80020199u);
                return;
            }
            const auto new_count=static_cast<std::int32_t>(ctx.gpr[5]);
            if (new_count>found->second.maximum) {
                ctx.set_gpr(2,0x800201BDu);
                return;
            }
            const std::uint32_t count_ptr = ctx.gpr[6];
            const std::uint32_t previous_waiters =
                static_cast<std::uint32_t>(found->second.waiters.size());
            for (const SemaWaiter &waiter : found->second.waiters) {
                const auto thread = g_threads.find(waiter.uid);
                if (thread != g_threads.end()) wake_thread(thread->second, 0x800201A9u, "sema-cancelled");
            }
            found->second.waiters.clear();
            found->second.count = new_count<0 ? found->second.initial_count : new_count;
            if (count_ptr != 0u && rt.memory().contains(count_ptr, 4u))
                rt.memory().store32(count_ptr, previous_waiters);
            ctx.set_gpr(2, 0u);
        });
    register_import(runtime, "ThreadManForUser", 0x94416130u, "sceKernelGetThreadmanIdList",
        [](Runtime &rt, AllegrexContext &ctx) {
            const std::uint32_t type = ctx.gpr[4];
            const std::uint32_t buffer = ctx.gpr[5];
            const std::uint32_t buffer_size = ctx.gpr[6];
            const std::uint32_t count_ptr = ctx.gpr[7];
            std::vector<std::int32_t> ids;
            switch (type) {
            case 1u: for (const auto &[uid, t] : g_threads) { (void)t; ids.push_back(uid); } break;
            case 2u: for (const auto &[uid, s] : g_semas) { (void)s; ids.push_back(uid); } break;
            case 3u: for (const auto &[uid, f] : g_event_flags) { (void)f; ids.push_back(uid); } break;
            case 6u: for (const auto &[uid, b] : g_blocks) { (void)b; ids.push_back(uid); } break;
            case 8u: for (const auto &[uid, c] : g_callbacks) { (void)c; ids.push_back(uid); } break;
            default: break;
            }
            std::sort(ids.begin(), ids.end());
            const std::size_t written = std::min<std::size_t>(ids.size(), buffer_size);
            for (std::size_t index = 0; index < written; ++index) {
                if (buffer != 0u && rt.memory().contains(buffer + static_cast<std::uint32_t>(index) * 4u, 4u))
                    rt.memory().store32(buffer + static_cast<std::uint32_t>(index) * 4u,
                                        static_cast<std::uint32_t>(ids[index]));
            }
            if (count_ptr != 0u && rt.memory().contains(count_ptr, 4u))
                rt.memory().store32(count_ptr, static_cast<std::uint32_t>(ids.size()));
            ctx.set_gpr(2, static_cast<std::uint32_t>(written));
        });

    // -----------------------------------------------------------------------
    // ThreadManForUser: fixed/variable pools
    // -----------------------------------------------------------------------
    register_import(runtime, "ThreadManForUser", 0xC07BB470u, "sceKernelCreateFpl",
        [](Runtime &rt, AllegrexContext &ctx) {
            BlockRecord record{};
            record.name = ctx.gpr[4] != 0u ? guest_string(rt, ctx.gpr[4], 64u) : "fpl";
            const std::uint32_t size = ctx.gpr[6];
            record.address = arena_allocate(size, 64u);
            record.size = size;
            if (record.address == 0u) {
                ctx.set_gpr(2, 0x800200D9u);
                return;
            }
            const std::int32_t uid = g_next_block_uid++;
            g_blocks.emplace(uid, record);
            ctx.set_gpr(2, static_cast<std::uint32_t>(uid));
        });
    register_import(runtime, "ThreadManForUser", 0xD979E9BFu, "sceKernelAllocateFpl",
        [](Runtime &rt, AllegrexContext &ctx) {
            const auto found = g_blocks.find(static_cast<std::int32_t>(ctx.gpr[4]));
            const std::uint32_t pointer = ctx.gpr[5];
            if (found == g_blocks.end() || pointer == 0u || !rt.memory().contains(pointer, 4u)) {
                ctx.set_gpr(2, 0x80020198u);
                return;
            }
            // Single-shot pool: hand out the whole block once.
            rt.memory().store32(pointer, found->second.address);
            ctx.set_gpr(2, 0u);
        });
    register_import(runtime, "ThreadManForUser", 0x623AE665u, "sceKernelTryAllocateFpl",
        [](Runtime &rt, AllegrexContext &ctx) {
            const auto found = g_blocks.find(static_cast<std::int32_t>(ctx.gpr[4]));
            const std::uint32_t pointer = ctx.gpr[5];
            if (found == g_blocks.end() || pointer == 0u || !rt.memory().contains(pointer, 4u)) {
                ctx.set_gpr(2, 0x80020198u);
                return;
            }
            rt.memory().store32(pointer, found->second.address);
            ctx.set_gpr(2, 0u);
        });
    register_import(runtime, "ThreadManForUser", 0xF6414A71u, "sceKernelFreeFpl",
        [](Runtime &, AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    register_import(runtime, "ThreadManForUser", 0xED1410E0u, "sceKernelDeleteFpl",
        [](Runtime &rt, AllegrexContext &ctx) {
            ctx.set_gpr(2, free_block(rt, static_cast<std::int32_t>(ctx.gpr[4])) ? 0u : 0x80020198u);
        });
    register_import(runtime, "ThreadManForUser", 0x56C039B5u, "sceKernelCreateVpl",
        [](Runtime &rt, AllegrexContext &ctx) {
            BlockRecord record{};
            record.name = ctx.gpr[4] != 0u ? guest_string(rt, ctx.gpr[4], 64u) : "vpl";
            const std::uint32_t size = ctx.gpr[7];   // sceKernelCreateVpl(name, partition, attr, size, opt)
            record.address = arena_allocate(size, 64u);
            record.size = size;
            if (record.address == 0u) {
                ctx.set_gpr(2, 0x800200D9u);
                return;
            }
            const std::int32_t uid = g_next_block_uid++;
            g_blocks.emplace(uid, record);
            ctx.set_gpr(2, static_cast<std::uint32_t>(uid));
        });
    // VPL pool family.  NID mapping verified against the PPSSPP ThreadMan table:
    // 0xBED27435 = AllocateVpl, 0xB736E9FF = FreeVpl, 0x89B3D48C = DeleteVpl.
    register_import(runtime, "ThreadManForUser", 0xBED27435u, "sceKernelAllocateVpl",
        [](Runtime &rt, AllegrexContext &ctx) {
            const auto found = g_blocks.find(static_cast<std::int32_t>(ctx.gpr[4]));
            const std::uint32_t pointer = ctx.gpr[6];
            if (found == g_blocks.end() || pointer == 0u || !rt.memory().contains(pointer, 4u)) {
                ctx.set_gpr(2, 0x80020198u);
                return;
            }
            const std::uint32_t want = (std::max(ctx.gpr[5], 1u) + 15u) & ~15u;
            auto &chunks = g_vpl_chunks[static_cast<std::int32_t>(ctx.gpr[4])];
            std::uint32_t at = found->second.address;
            const std::uint32_t end = found->second.address + found->second.size;
            bool placed = false;
            for (const auto &[start, len] : chunks) {
                if (start - at >= want) { placed = true; break; }
                at = std::max(at, start + len);
            }
            if (!placed && end - at >= want && end >= at) placed = true;
            if (!placed) {
                log_line(category::kHle, "sceKernelAllocateVpl: pool " + std::to_string(ctx.gpr[4]) + " is full for " + std::to_string(want) + " bytes");
                ctx.set_gpr(2, 0x80020190u);   // SCE_KERNEL_ERROR_NO_MEMORY
                return;
            }
            chunks[at] = want;
            rt.memory().store32(pointer, at);
            ctx.set_gpr(2, 0u);
        });
    register_import(runtime, "ThreadManForUser", 0x89B3D48Cu, "sceKernelDeleteVpl",
        [](Runtime &rt, AllegrexContext &ctx) {
            ctx.set_gpr(2, free_block(rt, static_cast<std::int32_t>(ctx.gpr[4])) ? 0u : 0x80020198u);
        });
    register_import(runtime, "ThreadManForUser", 0xB736E9FFu, "sceKernelFreeVpl",
        [](Runtime &, AllegrexContext &ctx) {
            const auto pool = g_vpl_chunks.find(static_cast<std::int32_t>(ctx.gpr[4]));
            if (pool != g_vpl_chunks.end()) pool->second.erase(ctx.gpr[5]);
            ctx.set_gpr(2, 0u);
        });

    // -----------------------------------------------------------------------
    // IoFileMgrForUser file primitives, shared by the synchronous and the
    // asynchronous entry points.  The async family completes inline in this
    // cooperative runtime, so a result is always ready before the guest can
    // observe it; the protocol flags keep PSP-visible behavior.
    constexpr std::uint32_t kIoErrorNoent = 0x80010002u;
    constexpr std::uint32_t kIoErrorBadFd = 0x80010009u;
    constexpr std::uint32_t kIoErrorInval = 0x80010016u;
    constexpr std::uint32_t kIoErrorNoAsync = 0x8002032Au;
    constexpr std::uint32_t kIoErrorAsyncBusy = 0x80020329u;

    auto io_open_file = [](Runtime &rt, const std::string &psp_path, std::uint32_t flags,
                           std::uint32_t caller_ra) -> std::uint32_t {
        const auto native = rt.translate_path(psp_path);
        ++g_open_count;
        const bool first_access = track_path_first_access(psp_path);
        std::ios::openmode mode = std::ios::binary;
        if ((flags & 0x0003u) == 0u || (flags & 0x0001u) != 0u) mode |= std::ios::in;
        if ((flags & 0x0002u) != 0u) mode |= std::ios::out;
        std::fstream stream(native, mode);
        if (!stream) {
            ++g_open_missing;
            if (g_reported_missing.insert(psp_path).second || first_access) {
                log_file_request(rt, "sceIoOpen", psp_path, native.string(), "MISSING");
                file_line("open \"" + psp_path + "\" [MISSING] flags=" + hex32(flags) +
                          " native=\"" + native.string() + "\"");
            }
            return kIoErrorNoent;
        }
        const std::int32_t fd = g_next_fd++;
        FileHandle handle;
        handle.stream = std::move(stream);
        handle.psp_path = psp_path;
        g_files.emplace(fd, std::move(handle));
        if (g_options.trace_filesystem && first_access) {
            log_file_request(rt, "sceIoOpen", psp_path, native.string(), "ok");
            file_line("open \"" + psp_path + "\" fd=" + std::to_string(fd) +
                      " flags=" + hex32(flags) + " [ok]");
        }
        // Phase 5 milestone record: the first real disc0 file the game opens.
        if (!g_first_file_logged) {
            g_first_file_logged = true;
            std::error_code size_ec;
            const auto size = std::filesystem::file_size(native, size_ec);
            file_line("[FILE FIRST] path=\"" + psp_path + "\" size=" +
                      (size_ec ? std::string("?") : std::to_string(size)) +
                      " caller_ra=" + hex32(caller_ra));
        }
        return static_cast<std::uint32_t>(fd);
    };

    auto io_read_file = [](Runtime &rt, std::int32_t fd, std::uint32_t buffer,
                           std::uint32_t size) -> std::uint32_t {
        const auto found = g_files.find(fd);
        if (found == g_files.end() || found->second.open_failed || found->second.directory)
            return kIoErrorBadFd;
        if (buffer == 0u || !rt.memory().contains(buffer, size == 0u ? 1u : size))
            return kIoErrorBadFd;
        // A zero-byte request leaves std::istream::gcount() unchanged. The
        // soundtrack streamer issues one at the asset boundary; reusing the
        // previous count would copy from an empty vector's null data pointer.
        if (size == 0u) return 0u;
        std::vector<std::uint8_t> data(size);
        found->second.stream.clear();
        found->second.stream.seekg(static_cast<std::streamoff>(found->second.position));
        found->second.stream.read(reinterpret_cast<char *>(data.data()),
                                  static_cast<std::streamsize>(size));
        const std::streamsize got = found->second.stream.gcount();
        const std::size_t read_size = static_cast<std::size_t>(std::max<std::streamsize>(0, got));
        if (read_size != 0u)
            rt.memory().copy_in(buffer, std::span<const std::uint8_t>(data.data(), read_size));
        found->second.position += read_size;
        ++g_read_count;
        g_read_bytes += read_size;
        if (read_size > g_largest_read) g_largest_read = read_size;
        // Log the first few large reads (asset/archive traffic) with the file's
        // path so the phase record can identify payload streams.
        if (read_size >= 65536u && g_read_logs < 128u) {
            ++g_read_logs;
            file_line("read fd=" + std::to_string(fd) + " size=" + std::to_string(read_size) +
                      " at=" + std::to_string(found->second.position - read_size) +
                      " file=\"" + found->second.psp_path + "\"");
        }
        return static_cast<std::uint32_t>(read_size);
    };

    auto io_write_file = [](Runtime &rt, std::int32_t fd, std::uint32_t buffer,
                            std::uint32_t size) -> std::uint32_t {
        const auto found = g_files.find(fd);
        if (found == g_files.end() || found->second.open_failed || found->second.directory)
            return kIoErrorBadFd;
        if (!rt.memory().contains(buffer, size == 0u ? 1u : size)) return kIoErrorBadFd;
        std::vector<std::uint8_t> data(size);
        if (size != 0u) rt.memory().copy_out(buffer, data);
        found->second.stream.clear();
        found->second.stream.seekp(static_cast<std::streamoff>(found->second.position));
        if (size != 0u)
            found->second.stream.write(reinterpret_cast<const char *>(data.data()),
                                       static_cast<std::streamsize>(size));
        found->second.position += size;
        ++g_write_count;
        g_write_bytes += size;
        return size;
    };

    auto io_seek_file = [](std::int32_t fd, std::int64_t offset, std::int32_t whence,
                           std::int64_t &position_out) -> std::uint32_t {
        const auto found = g_files.find(fd);
        if (found == g_files.end() || found->second.open_failed) return kIoErrorBadFd;
        std::int64_t base = 0;
        if (whence == 1) base = static_cast<std::int64_t>(found->second.position);
        else if (whence == 2) {
            found->second.stream.clear();
            found->second.stream.seekg(0, std::ios::end);
            base = static_cast<std::int64_t>(found->second.stream.tellg());
        } else if (whence != 0) {
            return kIoErrorInval;
        }
        const std::int64_t position = base + offset;
        if (position < 0) return kIoErrorInval;
        found->second.position = static_cast<std::uint64_t>(position);
        ++g_seek_count;
        if (g_seek_logs < 32u) {
            ++g_seek_logs;
            file_line("lseek fd=" + std::to_string(fd) + " offset=" + std::to_string(offset) +
                      " whence=" + std::to_string(whence) + " -> pos=" +
                      std::to_string(position) + " file=\"" + found->second.psp_path + "\"");
        }
        position_out = position;
        return 0u;
    };

    auto io_close_file = [](std::int32_t fd) -> std::uint32_t {
        return g_files.erase(fd) == 1u ? 0u : kIoErrorBadFd;
    };

    // Publishes a completed async result to the guest and releases a
    // close-pending descriptor.
    auto io_complete_async = [](Runtime &rt, std::int32_t fd, std::uint32_t address) -> bool {
        const auto found = g_files.find(fd);
        if (found == g_files.end() || !found->second.has_async_result) return false;
        const std::int64_t result = found->second.async_result;
        const bool close_pending = found->second.close_pending;
        if (address != 0u && rt.memory().contains(address, 8u)) {
            rt.memory().store32(address, static_cast<std::uint32_t>(result));
            rt.memory().store32(address + 4u,
                                static_cast<std::uint32_t>(static_cast<std::uint64_t>(result) >> 32u));
        }
        if (close_pending)
            g_files.erase(found);
        else
            found->second.has_async_result = false;
        return true;
    };

    register_import(runtime, "IoFileMgrForUser", 0x109F50BCu, "sceIoOpen",
        [io_open_file](Runtime &rt, AllegrexContext &ctx) {
            const std::string psp_path = guest_string(rt, ctx.gpr[4]);
            ctx.set_gpr(2, io_open_file(rt, psp_path, ctx.gpr[5], ctx.gpr[31]));
        });
    register_import(runtime, "IoFileMgrForUser", 0x810C4BC3u, "sceIoClose",
        [io_close_file](Runtime &, AllegrexContext &ctx) {
            ctx.set_gpr(2, io_close_file(static_cast<std::int32_t>(ctx.gpr[4])));
        });
    register_import(runtime, "IoFileMgrForUser", 0x6A638D83u, "sceIoRead",
        [io_read_file](Runtime &rt, AllegrexContext &ctx) {
            ctx.set_gpr(2, io_read_file(rt, static_cast<std::int32_t>(ctx.gpr[4]), ctx.gpr[5],
                                        ctx.gpr[6]));
        });
    register_import(runtime, "IoFileMgrForUser", 0x42EC03ACu, "sceIoWrite",
        [io_write_file](Runtime &rt, AllegrexContext &ctx) {
            ctx.set_gpr(2, io_write_file(rt, static_cast<std::int32_t>(ctx.gpr[4]), ctx.gpr[5],
                                         ctx.gpr[6]));
        });
    register_import(runtime, "IoFileMgrForUser", 0x27EB27B8u, "sceIoLseek",
        [io_seek_file](Runtime &, AllegrexContext &ctx) {
            const auto fd = static_cast<std::int32_t>(ctx.gpr[4]);
            const std::uint64_t raw_offset = static_cast<std::uint64_t>(ctx.gpr[6]) |
                (static_cast<std::uint64_t>(ctx.gpr[7]) << 32u);
            if ((raw_offset >> 32u) != 0u && g_seek_offset_logs < 32u) {
                ++g_seek_offset_logs;
                std::ostringstream out;
                out << "sceIoLseek 64-bit offset high word nonzero fd=" << fd
                    << " low=" << hex32(ctx.gpr[6]) << " high=" << hex32(ctx.gpr[7])
                    << " a1=" << hex32(ctx.gpr[5]) << " a3=" << hex32(ctx.gpr[8])
                    << " ra=" << hex32(ctx.gpr[31]);
                log_line(category::kFile, out.str());
            }
            std::int64_t position = 0;
            const std::uint32_t error = io_seek_file(
                fd, static_cast<std::int64_t>(raw_offset), static_cast<std::int32_t>(ctx.gpr[8]),
                position);
            if (error != 0u) {
                ctx.set_gpr(2, error);
                return;
            }
            ctx.set_gpr(2, static_cast<std::uint32_t>(position));
            ctx.set_gpr(3, static_cast<std::uint32_t>(static_cast<std::uint64_t>(position) >> 32u));
        });

    // --- asynchronous family -------------------------------------------------
    // The game streams its assets through the async API; each operation
    // completes inline and its result stays queued until the guest polls or
    // waits, exactly like a finished PSP async request.
    register_import(runtime, "IoFileMgrForUser", 0x89AA9906u, "sceIoOpenAsync",
        [io_open_file](Runtime &rt, AllegrexContext &ctx) {
            const std::string psp_path = guest_string(rt, ctx.gpr[4]);
            const std::uint32_t result = io_open_file(rt, psp_path, ctx.gpr[5], ctx.gpr[31]);
            if (static_cast<std::int32_t>(result) >= 0) {
                g_files[static_cast<std::int32_t>(result)].has_async_result = true;
                g_files[static_cast<std::int32_t>(result)].async_result =
                    static_cast<std::int64_t>(static_cast<std::int32_t>(result));
                ctx.set_gpr(2, result);
                return;
            }
            // PSP still returns a descriptor for a failed async open; the error
            // surfaces through WaitAsync/PollAsync and releases the descriptor.
            const std::int32_t fd = g_next_fd++;
            FileHandle handle;
            handle.open_failed = true;
            handle.psp_path = psp_path;
            handle.async_result = static_cast<std::int64_t>(static_cast<std::int32_t>(result));
            handle.has_async_result = true;
            handle.close_pending = true;
            g_files.emplace(fd, std::move(handle));
            ctx.set_gpr(2, static_cast<std::uint32_t>(fd));
        });
    register_import(runtime, "IoFileMgrForUser", 0xA0B5A7C2u, "sceIoReadAsync",
        [io_read_file](Runtime &rt, AllegrexContext &ctx) {
            const std::int32_t fd = static_cast<std::int32_t>(ctx.gpr[4]);
            auto found = g_files.find(fd);
            if (found == g_files.end()) { ctx.set_gpr(2, kIoErrorBadFd); return; }
            if (found->second.has_async_result) { ctx.set_gpr(2, kIoErrorAsyncBusy); return; }
            found->second.async_result = static_cast<std::int64_t>(static_cast<std::int32_t>(
                io_read_file(rt, fd, ctx.gpr[5], ctx.gpr[6])));
            found->second.has_async_result = true;
            ctx.set_gpr(2, 0u);
        });
    register_import(runtime, "IoFileMgrForUser", 0x0FACAB19u, "sceIoWriteAsync",
        [io_write_file](Runtime &rt, AllegrexContext &ctx) {
            const std::int32_t fd = static_cast<std::int32_t>(ctx.gpr[4]);
            auto found = g_files.find(fd);
            if (found == g_files.end()) { ctx.set_gpr(2, kIoErrorBadFd); return; }
            if (found->second.has_async_result) { ctx.set_gpr(2, kIoErrorAsyncBusy); return; }
            found->second.async_result = static_cast<std::int64_t>(static_cast<std::int32_t>(
                io_write_file(rt, fd, ctx.gpr[5], ctx.gpr[6])));
            found->second.has_async_result = true;
            ctx.set_gpr(2, 0u);
        });
    register_import(runtime, "IoFileMgrForUser", 0x71B19E77u, "sceIoLseekAsync",
        [io_seek_file](Runtime &, AllegrexContext &ctx) {
            const std::int32_t fd = static_cast<std::int32_t>(ctx.gpr[4]);
            // Same o32 64-bit-argument ABI as sceIoLseek: a0 = fd, a1 is
            // padding, the SceOff lands in the even-aligned pair a2/a3 and the
            // whence in a4.  Decoding a1/a2 instead produced enormous bogus
            // offsets (0x02EC0000_00000000-style) that read zero bytes at EOF
            // and stalled the movie streamer.
            const std::uint64_t raw_offset = static_cast<std::uint64_t>(ctx.gpr[6]) |
                (static_cast<std::uint64_t>(ctx.gpr[7]) << 32u);
            if ((raw_offset >> 32u) != 0u && g_seek_offset_logs < 32u) {
                ++g_seek_offset_logs;
                std::ostringstream out;
                out << "sceIoLseekAsync 64-bit offset high word nonzero fd=" << fd
                    << " low=" << hex32(ctx.gpr[6]) << " high=" << hex32(ctx.gpr[7])
                    << " a1=" << hex32(ctx.gpr[5]) << " ra=" << hex32(ctx.gpr[31]);
                log_line(category::kFile, out.str());
            }
            const auto found = g_files.find(fd);
            if (found == g_files.end()) { ctx.set_gpr(2, kIoErrorBadFd); return; }
            if (found->second.has_async_result) { ctx.set_gpr(2, kIoErrorAsyncBusy); return; }
            std::int64_t position = 0;
            const std::uint32_t error = io_seek_file(
                fd, static_cast<std::int64_t>(raw_offset), static_cast<std::int32_t>(ctx.gpr[8]),
                position);
            found->second.async_result =
                error != 0u ? static_cast<std::int64_t>(static_cast<std::int32_t>(error)) : position;
            found->second.has_async_result = true;
            ctx.set_gpr(2, 0u);
        });
    register_import(runtime, "IoFileMgrForUser", 0xFF5940B6u, "sceIoCloseAsync",
        [](Runtime &, AllegrexContext &ctx) {
            const std::int32_t fd = static_cast<std::int32_t>(ctx.gpr[4]);
            auto found = g_files.find(fd);
            if (found == g_files.end()) { ctx.set_gpr(2, kIoErrorBadFd); return; }
            if (found->second.has_async_result) { ctx.set_gpr(2, kIoErrorAsyncBusy); return; }
            found->second.async_result = 0;
            found->second.has_async_result = true;
            found->second.close_pending = true;
            ctx.set_gpr(2, 0u);
        });
    auto io_async_wait = [io_complete_async](Runtime &rt, AllegrexContext &ctx) {
        const std::int32_t fd = static_cast<std::int32_t>(ctx.gpr[4]);
        const auto found = g_files.find(fd);
        if (found == g_files.end()) { ctx.set_gpr(2, kIoErrorBadFd); return; }
        if (!found->second.has_async_result) { ctx.set_gpr(2, kIoErrorNoAsync); return; }
        (void)io_complete_async(rt, fd, ctx.gpr[5]);
        ctx.set_gpr(2, 0u);
    };
    register_import(runtime, "IoFileMgrForUser", 0xE23EEC33u, "sceIoWaitAsync", io_async_wait);
    register_import(runtime, "IoFileMgrForUser", 0x35DBD746u, "sceIoWaitAsyncCB", io_async_wait);
    auto io_async_poll = [io_complete_async](Runtime &rt, AllegrexContext &ctx) {
        const std::int32_t fd = static_cast<std::int32_t>(ctx.gpr[4]);
        const auto found = g_files.find(fd);
        if (found == g_files.end()) { ctx.set_gpr(2, kIoErrorBadFd); return; }
        // Operations complete inline, so a queued result means "done" and an
        // empty queue means the guest never issued one.
        if (!found->second.has_async_result) { ctx.set_gpr(2, kIoErrorNoAsync); return; }
        (void)io_complete_async(rt, fd, ctx.gpr[5]);
        ctx.set_gpr(2, 0u);
    };
    register_import(runtime, "IoFileMgrForUser", 0x3251EA56u, "sceIoPollAsync", io_async_poll);
    register_import(runtime, "IoFileMgrForUser", 0xCB05F8D6u, "sceIoGetAsyncStat",
        [io_complete_async](Runtime &rt, AllegrexContext &ctx) {
            const std::int32_t fd = static_cast<std::int32_t>(ctx.gpr[4]);
            const auto found = g_files.find(fd);
            std::uint32_t ret = 0u;
            std::string path = "?";
            std::int64_t res = 0;
            if (found == g_files.end()) ret = kIoErrorBadFd;
            else {
                path = found->second.psp_path;
                res = found->second.async_result;
                if (!found->second.has_async_result) ret = kIoErrorNoAsync;
                else (void)io_complete_async(rt, fd, ctx.gpr[6]);
            }
            // Diagnostic: log each distinct (fd, return, result) combination once
            // (capped), so an endless poll loop shows exactly what it is polling.
            static std::set<std::tuple<std::int32_t, std::uint32_t, std::int64_t>> seen;
            static std::uint64_t calls = 0;
            ++calls;
            if (seen.size() < 64u && seen.emplace(fd, ret, res).second) {
                // Guest code around the caller, for offline disassembly.
                if (ret != 0u) {
                    const std::uint32_t ra = ctx.gpr[31];
                    const std::uint32_t from = (ra > 0x180u ? ra - 0x180u : 0u) & ~3u;
                    std::ostringstream code;
                    code << "GUESTCODE " << hex32(from) << ":";
                    for (std::uint32_t a = from; a < ra + 0x80u; a += 4u) {
                        if (rt.memory().contains(a, 4u)) code << ' ' << hex32(rt.memory().load32(a));
                    }
                    log_line(category::kFile, code.str());
                    std::ostringstream regs;
                    regs << "GUESTREGS";
                    for (int r = 0; r < 32; ++r) regs << " r" << r << "=" << hex32(ctx.gpr[r]);
                    log_line(category::kFile, regs.str());
                }
                std::ostringstream out;
                out << "sceIoGetAsyncStat fd=" << fd << " poll=" << ctx.gpr[5] << " ret=" << hex32(ret)
                    << " result=" << res << " file=\"" << path << "\" call#" << calls
                    << " ra=" << hex32(ctx.gpr[31]);
                log_line(category::kFile, out.str());
            }
            ctx.set_gpr(2, ret);
        });
    register_import(runtime, "IoFileMgrForUser", 0xB293727Fu, "sceIoChangeAsyncPriority",
        [](Runtime &, AllegrexContext &ctx) {
            const std::int32_t fd = static_cast<std::int32_t>(ctx.gpr[4]);
            ctx.set_gpr(2, g_files.find(fd) != g_files.end() ? 0u : kIoErrorBadFd);
        });
    register_import(runtime, "IoFileMgrForUser", 0xA12A0514u, "sceIoSetAsyncCallback",
        [](Runtime &, AllegrexContext &ctx) {
            const std::int32_t fd = static_cast<std::int32_t>(ctx.gpr[4]);
            auto found = g_files.find(fd);
            if (found == g_files.end()) { ctx.set_gpr(2, kIoErrorBadFd); return; }
            found->second.async_callback = ctx.gpr[5];
            found->second.async_callback_arg = ctx.gpr[6];
            ctx.set_gpr(2, 0u);
        });
    register_import(runtime, "IoFileMgrForUser", 0xACE946E8u, "sceIoGetstat",
        [](Runtime &rt, AllegrexContext &ctx) {
            const std::string psp_path = guest_string(rt, ctx.gpr[4]);
            const std::uint32_t stat_address = ctx.gpr[5];
            const auto native = rt.translate_path(psp_path);
            const bool first_access = track_path_first_access(psp_path);
            std::error_code ec;
            const auto size = std::filesystem::file_size(native, ec);
            if (ec) {
                if (g_reported_missing.insert(psp_path).second || first_access) {
                    log_file_request(rt, "sceIoGetstat", psp_path, native.string(), "MISSING");
                    file_line("getstat \"" + psp_path + "\" [MISSING]");
                }
                ctx.set_gpr(2, 0x80010002u);
                return;
            }
            fill_stat(rt.memory(), stat_address, 0x00001FFFu, size);
            if (first_access)
                file_line("getstat \"" + psp_path + "\" size=" + std::to_string(size) + " [ok]");
            ctx.set_gpr(2, 0u);
        });
    register_import(runtime, "IoFileMgrForUser", 0xB29DDF9Cu, "sceIoDopen",
        [](Runtime &rt, AllegrexContext &ctx) {
            const std::string psp_path = guest_string(rt, ctx.gpr[4]);
            const auto native = rt.translate_path(psp_path);
            const bool first_access = track_path_first_access(psp_path);
            std::error_code ec;
            if (!std::filesystem::is_directory(native, ec)) {
                if (g_reported_missing.insert(psp_path).second || first_access) {
                    log_file_request(rt, "sceIoDopen", psp_path, native.string(), "MISSING");
                    file_line("dopen \"" + psp_path + "\" [MISSING]");
                }
                ctx.set_gpr(2, 0x80010002u);
                return;
            }
            FileHandle handle;
            handle.directory = true;
            handle.psp_path = psp_path;
            for (const auto &entry : std::filesystem::directory_iterator(native, ec)) {
                handle.entries.push_back(entry.path().filename().string());
            }
            if (first_access)
                file_line("dopen \"" + psp_path + "\" entries=" +
                          std::to_string(handle.entries.size()) + " [ok]");
            if (g_options.trace_filesystem && first_access)
                log_file_request(rt, "sceIoDopen", psp_path, native.string(),
                                 ("ok entries=" + std::to_string(handle.entries.size())).c_str());
            const std::int32_t fd = g_next_fd++;
            g_files.emplace(fd, std::move(handle));
            ctx.set_gpr(2, static_cast<std::uint32_t>(fd));
        });
    register_import(runtime, "IoFileMgrForUser", 0xE3EB004Cu, "sceIoDread",
        [](Runtime &rt, AllegrexContext &ctx) {
            const auto fd = static_cast<std::int32_t>(ctx.gpr[4]);
            const std::uint32_t dirent = ctx.gpr[5];
            const auto found = g_files.find(fd);
            if (found == g_files.end() || !found->second.directory) {
                ctx.set_gpr(2, 0x80010009u);
                return;
            }
            if (found->second.entry_index >= found->second.entries.size()) {
                ctx.set_gpr(2, 0u);
                return;
            }
            constexpr std::size_t kDirentSize = 0x148u;
            if (dirent != 0u && rt.memory().contains(dirent, kDirentSize)) {
                rt.memory().zero(dirent, kDirentSize);
                const std::string &name = found->second.entries[found->second.entry_index];
                const std::size_t length = std::min<std::size_t>(name.size(), 255u);
                rt.memory().store32(dirent + 0u, 0x00001FFFu); // st_mode
                rt.memory().store32(dirent + 4u, 0x00000020u); // st_attr (FIO_S_IFREG)
                for (std::size_t index = 0; index < length; ++index)
                    rt.memory().store8(dirent + 64u + static_cast<std::uint32_t>(index),
                                       static_cast<std::uint8_t>(name[index]));
            }
            ++found->second.entry_index;
            ctx.set_gpr(2, 1u);
        });
    register_import(runtime, "IoFileMgrForUser", 0xEB092469u, "sceIoDclose",
        [](Runtime &, AllegrexContext &ctx) {
            const auto removed = g_files.erase(static_cast<std::int32_t>(ctx.gpr[4]));
            ctx.set_gpr(2, removed == 1u ? 0u : 0x80010009u);
        });
    register_import(runtime, "IoFileMgrForUser", 0x54F5FB11u, "sceIoDevctl",
        [](Runtime &rt, AllegrexContext &ctx) {
            const std::string psp_path = guest_string(rt, ctx.gpr[4]);
            const std::uint32_t command = ctx.gpr[5];
            const std::uint32_t out_data = ctx.gpr[8];
            const std::uint32_t out_length = ctx.gpr[9];
            if (g_options.trace_filesystem) {
                std::ostringstream out;
                out << "sceIoDevctl \"" << psp_path << "\" cmd=" << hex32(command)
                    << " out_len=" << out_length << " (returning success)";
                log_line(category::kFilesystem, out.str());
            }
            if (out_data != 0u && out_length != 0u && rt.memory().contains(out_data, out_length))
                rt.memory().zero(out_data, out_length);
            // Memory Stick commands: report an inserted, writable stick with
            // plenty of free space so the game takes its normal save path.
            const std::uint32_t in_data = ctx.gpr[6];
            if (command == 0x02025806u && out_data != 0u && out_length >= 4u && rt.memory().contains(out_data, 4u)) {
                rt.memory().store32(out_data, 1u);                      // stick inserted
            } else if (command == 0x02425818u && in_data != 0u && rt.memory().contains(in_data, 4u)) {
                const std::uint32_t info = rt.memory().load32(in_data); // -> MemoryStickInfo
                if (info != 0u && rt.memory().contains(info, 20u)) {
                    constexpr std::uint32_t kSectorSize = 0x200u, kSectorsPerCluster = 0x40u;
                    constexpr std::uint32_t kClusters = 0x8000u;        // 1 GiB of 32 KiB clusters
                    rt.memory().store32(info + 0, kClusters);           // maxClusters
                    rt.memory().store32(info + 4, kClusters);           // freeClusters
                    rt.memory().store32(info + 8, kClusters);           // maxSectors
                    rt.memory().store32(info + 12, kSectorSize);
                    rt.memory().store32(info + 16, kSectorsPerCluster);
                }
            }
            ctx.set_gpr(2, 0u);
        });
    register_import(runtime, "IoFileMgrForUser", 0x06A70004u, "sceIoRemove",
        [](Runtime &rt, AllegrexContext &ctx) {
            const std::string psp_path = guest_string(rt, ctx.gpr[4]);
            const auto native = rt.translate_path(psp_path);
            std::error_code ec;
            if (!std::filesystem::remove(native, ec)) {
                log_file_request(rt, "sceIoRemove", psp_path, native.string(), "MISSING");
                ctx.set_gpr(2, 0x80010002u);
                return;
            }
            ctx.set_gpr(2, 0u);
        });
    register_import(runtime, "IoFileMgrForUser", 0x779103A0u, "sceIoRename",
        [](Runtime &rt, AllegrexContext &ctx) {
            const std::string from = guest_string(rt, ctx.gpr[4]);
            const std::string to = guest_string(rt, ctx.gpr[5]);
            std::error_code ec;
            std::filesystem::rename(rt.translate_path(from), rt.translate_path(to), ec);
            if (ec) {
                log_line(category::kFilesystem, "sceIoRename failed \"" + from + "\" -> \"" + to + "\"");
                ctx.set_gpr(2, 0x80010002u);
                return;
            }
            ctx.set_gpr(2, 0u);
        });

    // -----------------------------------------------------------------------
    // StdioForUser (stdout/stderr are informational)
    // -----------------------------------------------------------------------
    auto stdio_handle = [](Runtime &rt, AllegrexContext &ctx, const char *stream) {
        const std::string format = guest_string(rt, ctx.gpr[4], 512u);
        if (!format.empty()) hle_line(std::string("stdio ") + stream + ": " + format);
        ctx.set_gpr(2, 0u);
    };
    register_import(runtime, "StdioForUser", 0x172D316Eu, "sceKernelStdin",
        [](Runtime &, AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    register_import(runtime, "StdioForUser", 0xA6BAB2E9u, "sceKernelStdout",
        [stdio_handle](Runtime &rt, AllegrexContext &ctx) { stdio_handle(rt, ctx, "stdout"); });
    register_import(runtime, "StdioForUser", 0xF78BA90Au, "sceKernelStderr",
        [stdio_handle](Runtime &rt, AllegrexContext &ctx) { stdio_handle(rt, ctx, "stderr"); });

    // -----------------------------------------------------------------------
    // ModuleMgrForUser
    // -----------------------------------------------------------------------
    auto module_load = [](const char *name) {
        return [name](Runtime &rt, AllegrexContext &ctx) {
            const std::string psp_path = guest_string(rt, ctx.gpr[4]);
            const auto native = rt.translate_path(psp_path);
            std::error_code ec;
            const bool exists = std::filesystem::is_regular_file(native, ec);
            // Diagnostic experiment only: claim dynamic modules load when the
            // caller tolerates a failure differently.  Never enabled by default.
            const bool fake_success = std::getenv("PSPRECOMP_RIDGERACER_FAKE_MODULE_UID") != nullptr;
            if (exists && fake_success) {
                static std::int32_t next_fake_uid = 0x300;
                log_line(category::kModule, std::string(name) + " \"" + psp_path +
                                               "\" -> fake uid=" + std::to_string(next_fake_uid) +
                                               " (PSPRECOMP_RIDGERACER_FAKE_MODULE_UID diagnostic)");
                ctx.set_gpr(2, static_cast<std::uint32_t>(next_fake_uid++));
                return;
            }
            // Dynamic (game-supplied) PRX modules are encrypted with the
            // kernel's `~SCE` container.  This runtime cannot decrypt or
            // recompile them, so the load fails with the real kernel error a
            // corrupt/unsupported module would produce.  Callers that treat the
            // module as optional (MotorStorm's movie module wrapper does)
            // continue without it; we never fake a successful load.
            static thread_local std::uint64_t logged = 0;
            if (logged < 16u) {
                ++logged;
                std::ostringstream out;
                out << name << " \"" << psp_path << "\" native=\"" << native.string()
                    << "\" exists=" << (exists ? 1 : 0)
                    << " caller_pc=" << hex32(psprecomp::runtime_dispatch_pc())
                    << " ra=" << hex32(ctx.gpr[31])
                    << (exists ? " -> 0x80020148 (encrypted dynamic PRX, unsupported)"
                               : " -> 0x80010002 (not found)");
                log_line(category::kModule, out.str());
            }
            ctx.set_gpr(2, exists ? 0x80020148u : 0x80010002u);
        };
    };
    register_import(runtime, "ModuleMgrForUser", 0x977DE386u, "sceKernelLoadModule",
                    module_load("sceKernelLoadModule"));
    register_import(runtime, "ModuleMgrForUser", 0xB7F46618u, "sceKernelLoadModuleByID",
                    module_load("sceKernelLoadModuleByID"));
    register_import(runtime, "ModuleMgrForUser", 0x50F0C1ECu, "sceKernelStartModule",
        [](Runtime &, AllegrexContext &ctx) {
            log_line(category::kModule, "sceKernelStartModule uid=" + std::to_string(static_cast<std::int32_t>(ctx.gpr[4])) + " -> 0x8002012E");
            ctx.set_gpr(2, 0x8002012Eu);
        });
    register_import(runtime, "ModuleMgrForUser", 0xD1FF982Au, "sceKernelStopModule",
        [](Runtime &, AllegrexContext &ctx) { ctx.set_gpr(2, 0x8002012Eu); });
    register_import(runtime, "ModuleMgrForUser", 0xD675EBB8u, "sceKernelSelfStopUnloadModule",
        [](Runtime &, AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    register_import(runtime, "ModuleMgrForUser", 0x2E0911AAu, "sceKernelUnloadModule",
        [](Runtime &, AllegrexContext &ctx) { ctx.set_gpr(2, 0x80020148u); });
    register_import(runtime, "ModuleMgrForUser", 0x8F2DF740u, "sceKernelUnloadModule",
        [](Runtime &rt, AllegrexContext &ctx) {
            std::ostringstream out;
            out << "sceKernelUnloadModule uid=" << static_cast<std::int32_t>(ctx.gpr[4]);
            log_line(category::kModule, out.str());
            ctx.set_gpr(2, 0x80020148u);
        });
    register_import(runtime, "ModuleMgrForUser", 0xF0A26395u, "sceKernelGetModuleId",
        [](Runtime &, AllegrexContext &ctx) { ctx.set_gpr(2, 1u); });
    register_import(runtime, "ModuleMgrForUser", 0xD8B73127u, "sceKernelGetModuleIdByAddress",
        [](Runtime &, AllegrexContext &ctx) { ctx.set_gpr(2, 1u); });

    // -----------------------------------------------------------------------
    // InterruptManager
    // -----------------------------------------------------------------------
    register_import(runtime, "InterruptManager", 0xCA04A2B9u, "sceKernelRegisterSubIntrHandler",
        [](Runtime &, AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    register_import(runtime, "InterruptManager", 0xD61E6961u, "sceKernelReleaseSubIntrHandler",
        [](Runtime &, AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    register_import(runtime, "InterruptManager", 0xFB8E22ECu, "sceKernelEnableSubIntr",
        [](Runtime &, AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });

    // -----------------------------------------------------------------------
    // sceDisplay (headless): mode/frame-buffer bookkeeping and vblank pacing.
    // -----------------------------------------------------------------------
    register_import(runtime, "sceDisplay", 0x0E20F177u, "sceDisplaySetMode",
        [](Runtime &, AllegrexContext &ctx) {
            g_display.mode = ctx.gpr[4];
            g_display.width = ctx.gpr[5];
            g_display.height = ctx.gpr[6];
            ctx.set_gpr(2, 0u);
        });
    register_import(runtime, "sceDisplay", 0x289D82FEu, "sceDisplaySetFrameBuf",
        [](Runtime &rt, AllegrexContext &ctx) {
            g_display.frame_top = ctx.gpr[4];
            g_display.frame_width = ctx.gpr[5];
            g_display.frame_format = ctx.gpr[6];
            ++g_display.frame_sets;
            ctx.set_gpr(2, 0u);
            present_display_frame(rt);
        });
    register_import(runtime, "sceDisplay", 0xEEDA2E54u, "sceDisplayGetFrameBuf",
        [](Runtime &rt, AllegrexContext &ctx) {
            if (ctx.gpr[4]) rt.memory().store32(ctx.gpr[4], g_display.frame_top);
            if (ctx.gpr[5]) rt.memory().store32(ctx.gpr[5], g_display.frame_width);
            if (ctx.gpr[6]) rt.memory().store32(ctx.gpr[6], g_display.frame_format);
            ctx.set_gpr(2, 0u);
        });
    register_import(runtime, "sceDisplay", 0x773DD3A3u, "sceDisplayGetCurrentHcount",
        [](Runtime &, AllegrexContext &ctx) {
            ctx.set_gpr(2, static_cast<std::uint32_t>((g_virtual_time_us - g_vb_base_vt) * 286ull / g_vblank_period_us % 286u));
        });
    register_import(runtime, "sceDisplay", 0x210EAB3Au, "sceDisplayGetAccumulatedHcount",
        [](Runtime &, AllegrexContext &ctx) {
            ctx.set_gpr(2, static_cast<std::uint32_t>(g_vb_base_count * 286ull + (g_virtual_time_us - g_vb_base_vt) * 286ull / g_vblank_period_us));
        });
    register_import(runtime, "sceDisplay", 0x9C6EAAD7u, "sceDisplayGetVcount",
        [](Runtime &, AllegrexContext &ctx) {
            ctx.set_gpr(2, static_cast<std::uint32_t>(vblank_count_at(g_virtual_time_us)));
        });
    const auto vblank_wait = [](const char *label, bool callbacks) {
        return [label, callbacks](Runtime &rt, AllegrexContext &ctx) {
            ++g_vblank_waits;
            if (callbacks && service_owned_callback(rt, ctx, ctx.pc)) return;
            ThreadRecord *thread = current_thread();
            if (thread == nullptr) {
                rt.stop(std::string(label) + " with no current thread");
                return;
            }
            const std::uint64_t target = next_vblank_after(g_virtual_time_us);
            thread->callback_wait = callbacks;
            thread->callback_wait_pc = ctx.pc;
            thread->callback_wait_kind = callbacks ? CallbackWaitKind::Vblank : CallbackWaitKind::None;
            save_continuation(*thread, ctx);
            thread->suspended.set_gpr(2, 0u);
            thread->state = ThreadState::Delayed;
            thread->delay_until_us = target;
            if (!activate_next(rt, ctx, label)) {
                ++g_deadlock_count;
                rt.stop("PSP scheduler deadlock while waiting for vblank");
            }
        };
    };
    register_import(runtime, "sceDisplay", 0x984C27E7u, "sceDisplayWaitVblankStart",
                    vblank_wait("vblank", false));
    register_import(runtime, "sceDisplay", 0x46F186C3u, "sceDisplayWaitVblankStartCB",
                    vblank_wait("vblank-cb", true));

    // -----------------------------------------------------------------------
    // sceCtrl (headless): no buttons pressed, analog stick centred.
    // -----------------------------------------------------------------------
    register_import(runtime, "sceCtrl", 0x6A2774F3u, "sceCtrlSetSamplingCycle",
        [](Runtime &, AllegrexContext &ctx) {
            g_ctrl_requested_cycle = ctx.gpr[4];
            ctx.set_gpr(2, 0u);
        });
    register_import(runtime, "sceCtrl", 0x1F4011E6u, "sceCtrlSetSamplingMode",
        [](Runtime &, AllegrexContext &ctx) {
            const std::uint32_t previous = g_ctrl_requested_mode;
            g_ctrl_requested_mode = ctx.gpr[4];
            ctx.set_gpr(2, previous);
        });
    const auto ctrl_read = [](Runtime &rt, AllegrexContext &ctx) {
        ++g_ctrl_reads;
        g_ctrl_game60 += static_cast<double>(g_vblank_period_us) / static_cast<double>(g_base_vblank_us);
        const std::uint32_t buffer = ctx.gpr[4];
        const std::uint32_t count = std::min<std::uint32_t>(std::max<std::uint32_t>(ctx.gpr[5], 1u), 64u);
        present_pump();
        present_note_pad_read();
        std::uint32_t buttons = osk_active() ? 0u : effective_pad_buttons();
        if (gates::fast_rate_active() && !gates::table.empty()) {
            // Hand button changes over on 60 Hz tick steps only (see gates::pad_step).
            static std::uint32_t held = 0u;
            const std::uint32_t st = gates::game_step();
            gates::pad_step = st; gates::pad_frame = g_frames_presented;
            if (gates::menu_tick(st + gates::pad_delta)) held = buttons;
            else buttons = held;
        }
        {
            static std::uint32_t last_buttons = 0u;
            if (buttons != last_buttons) {
                log_line(category::kHle, "PAD buttons " + hex32(last_buttons) + " -> " + hex32(buttons) +
                                             " (read #" + std::to_string(g_ctrl_reads) + ", frame " +
                                             std::to_string(g_frames_presented) + ")");
                menu_sound_note_press(buttons & ~last_buttons, g_frames_presented);
                // Start during a race = pause: mute the voices the game's key-off burst leaves behind (the burst was only 5 voices once,
                // below the heuristic's threshold, and 8 engine / ambient voices played through the whole pause).  RR_PAUSE_MUTE_OFF=1 disables.
                // The next Start press (resume) restores every muted voice: the pause mute used to fire again on the resume press and
                // left the car without engine / effect sounds for the rest of the race.
                if (((buttons & ~last_buttons) & 0x8u) != 0u && std::getenv("RR_PAUSE_MUTE_OFF") == nullptr) {
                    if (gates::pause_muted) { gates::pause_muted = false; psprecomp::sas_unmute_all(); }
                    else if (gates::last_handler == gates::kRaceHandler) { gates::pause_muted = true; psprecomp::sas_request_stop_all(); }
                }
                last_buttons = buttons;
            }
        }
        std::uint8_t lx = 128, ly = 128;
        if (!osk_active()) present_pad_analog(lx, ly);
        cartrace::pad_buttons = buttons; cartrace::pad_lx = lx; cartrace::pad_ly = ly;
        if (buffer != 0u && cartrace::pad_buffer != buffer) { cartrace::pad_buffer = buffer; cartrace::pad_count = count; }
        cartrace::in_hle_pad_write = true;
        struct PadWriteDone { ~PadWriteDone() { cartrace::in_hle_pad_write = false; } } pad_write_done;
        if (buffer == 0u) {
            ctx.set_gpr(2, 0x80000023u);
            return;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t at = buffer + i * 16u;
            rt.memory().store32(at, static_cast<std::uint32_t>(g_virtual_time_us));
            rt.memory().store32(at + 4u, buttons);            // buttons
            rt.memory().store32(at + 8u, static_cast<std::uint32_t>(lx) | (static_cast<std::uint32_t>(ly) << 8u));
            rt.memory().store32(at + 12u, 0u);                // reserved
        }
        ctx.set_gpr(2, count);
    };
    register_import(runtime, "sceCtrl", 0x1F803938u, "sceCtrlReadBufferPositive", ctrl_read);
    register_import(runtime, "sceCtrl", 0x3A622550u, "sceCtrlPeekBufferPositive", ctrl_read);

    // -----------------------------------------------------------------------
    // sceGe_user: lists are executed synchronously by GeProcessor and drawn by
    // the software backend; its SIGNAL/FINISH interrupts feed the game's callbacks.
    // -----------------------------------------------------------------------
    register_import(runtime, "sceGe_user", 0xE47E40E4u, "sceGeEdramGetAddr",
        [](Runtime &, AllegrexContext &ctx) { ctx.set_gpr(2, 0x04000000u); });
    register_import(runtime, "sceGe_user", 0x1F6752ADu, "sceGeEdramGetSize",
        [](Runtime &, AllegrexContext &ctx) { ctx.set_gpr(2, 0x00200000u); });
    register_import(runtime, "sceGe_user", 0xA4FC06A4u, "sceGeSetCallback",
        [](Runtime &rt, AllegrexContext &ctx) {
            GeCallbackSlot slot{};
            if (ctx.gpr[4] != 0u && rt.memory().contains(ctx.gpr[4], 16u)) {
                slot.signal_func = rt.memory().load32(ctx.gpr[4]);
                slot.signal_arg = rt.memory().load32(ctx.gpr[4] + 4u);
                slot.finish_func = rt.memory().load32(ctx.gpr[4] + 8u);
                slot.finish_arg = rt.memory().load32(ctx.gpr[4] + 12u);
            }
            slot.used = true;
            const std::int32_t id = g_ge_callback_id++;
            g_ge_callbacks[id] = slot;
            std::ostringstream out;
            out << "sceGeSetCallback id=" << id << " signal=" << hex32(slot.signal_func)
                << "/" << hex32(slot.signal_arg) << " finish=" << hex32(slot.finish_func)
                << "/" << hex32(slot.finish_arg);
            log_line(category::kGe, out.str());
            ctx.set_gpr(2, static_cast<std::uint32_t>(id));
        });
    register_import(runtime, "sceGe_user", 0x05DB22CEu, "sceGeUnsetCallback",
        [](Runtime &, AllegrexContext &ctx) {
            g_ge_callbacks.erase(static_cast<std::int32_t>(ctx.gpr[4]));
            ctx.set_gpr(2, 0u);
        });
    const auto ge_collect = [] {
        for (const GeInterrupt &irq : g_ge->take_interrupts())
            g_ge_pending.push_back(GePending{irq.token, irq.finish, irq.callback_id});
    };
    const auto ge_enqueue = [ge_collect](bool head) {
        return [ge_collect, head](Runtime &, AllegrexContext &ctx) {
            const std::int32_t id = g_ge->enqueue(ctx.gpr[4], ctx.gpr[5], static_cast<std::int32_t>(ctx.gpr[6]), head);
            ge_collect();
            ctx.set_gpr(2, static_cast<std::uint32_t>(id));
        };
    };
    register_import(runtime, "sceGe_user", 0xAB49E76Au, "sceGeListEnQueue", ge_enqueue(false));
    register_import(runtime, "sceGe_user", 0x1C0D95A6u, "sceGeListEnQueueHead", ge_enqueue(true));
    register_import(runtime, "sceGe_user", 0xE0D68148u, "sceGeListUpdateStallAddr",
        [ge_collect](Runtime &, AllegrexContext &ctx) {
            g_ge->update_stall(static_cast<std::int32_t>(ctx.gpr[4]), ctx.gpr[5]);
            ge_collect();
            ctx.set_gpr(2, 0u);
        });
    register_import(runtime, "sceGe_user", 0x03444EB4u, "sceGeListSync",
        [](Runtime &rt, AllegrexContext &ctx) {
            const auto status = g_ge->status(static_cast<std::int32_t>(ctx.gpr[4]));
            ctx.set_gpr(2, ctx.gpr[5] == 0u ? 0u : static_cast<std::uint32_t>(status));
            (void)ge_deliver(rt, ctx, 0u);
        });
    register_import(runtime, "sceGe_user", 0xB287BD61u, "sceGeDrawSync",
        [](Runtime &rt, AllegrexContext &ctx) {
            ctx.set_gpr(2, ctx.gpr[4] == 0u || g_ge->idle() ? 0u : 2u);
            (void)ge_deliver(rt, ctx, 0u);
        });
    register_import(runtime, "sceGe_user", 0xB448EC0Du, "sceGeBreak",
        [](Runtime &, AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    register_import(runtime, "sceGe_user", 0x4C06E472u, "sceGeContinue",
        [ge_collect](Runtime &, AllegrexContext &ctx) {
            g_ge->resume();
            ge_collect();
            ctx.set_gpr(2, 0u);
        });

    // -----------------------------------------------------------------------
    // sceAudio (old channel API): reserve/output with blocking paced by the
    // channel's sample count; audio is not yet played back.
    // -----------------------------------------------------------------------
    {
        struct AudioChannel { bool reserved{}; std::uint32_t samples{1024}; std::uint32_t format{}; };
        static AudioChannel channels[8];
        // Sends `samples` frames at `buffer` to the mixer (lazily opening the device; only with a window and not RR_NO_AUDIO).
        const auto audio_play = [](Runtime &rt, std::uint32_t channel, std::uint32_t buffer, std::uint32_t vol_l, std::uint32_t vol_r) {
            static const bool enabled = g_hle_options.window && std::getenv("RR_NO_AUDIO") == nullptr;
            if (!enabled || channel >= 8u || buffer == 0u) return;
            static const bool started = (audio_set_master_volume(g_hle_options.volume), audio_set_menu_gain(g_hle_options.menu_volume),
                                         audio_set_time_stretch(g_hle_options.frame_skip != 0), audio_start());
            if (!started) return;
            const bool stereo = (channels[channel].format & 0x10u) == 0u;
            const std::uint32_t frames = std::min<std::uint32_t>(channels[channel].samples, 65536u);
            const std::size_t bytes = static_cast<std::size_t>(frames) * (stereo ? 4u : 2u);
            if (frames == 0u || !rt.memory().contains(buffer, bytes)) return;
            static thread_local std::vector<std::int16_t> pcm;
            pcm.resize(bytes / 2u);
            const bool movie = stereo && mpeg_take_audio(buffer, frames, pcm.data());
            if (!movie)
                rt.memory().copy_out(buffer, std::span<std::uint8_t>(reinterpret_cast<std::uint8_t *>(pcm.data()), bytes));
            {
                // The output device now buffers less than it used to; the movie's picture was timed against the old
                // latency, so at the start of each movie its audio gets the difference back as leading silence.
                static std::uint64_t last_movie_us[8] = {};
                audio_channel_set_trim(static_cast<int>(channel), !movie);
                if (movie) {
                    if (last_movie_us[channel] == 0u || g_virtual_time_us - last_movie_us[channel] > 1000000ull) {
                        audio_channel_clear(static_cast<int>(channel));   // whatever the previous user of the channel left behind
                        static thread_local std::vector<std::int16_t> pad;
                        pad.assign(static_cast<std::size_t>(audio_latency_trim_frames()) * 2u, 0);
                        if (!pad.empty()) audio_channel_push(static_cast<int>(channel), pad.data(), audio_latency_trim_frames(), true, 0x8000, 0x8000);
                    }
                    last_movie_us[channel] = g_virtual_time_us;
                }
                // Diagnostics: which channel/volume combinations the game uses (first 60 distinct ones).
                static std::uint32_t seen[64][5];
                static std::size_t seen_count = 0;
                const std::uint32_t key[5] = {channel, vol_l, vol_r, frames, stereo ? 1u : 0u};
                bool known = false;
                for (std::size_t i = 0; i < seen_count && !known; ++i) known = std::equal(key, key + 5, seen[i]);
                if (!known && seen_count < 60u) {
                    std::copy(key, key + 5, seen[seen_count++]);
                    log_line(category::kHle, "AUDIO channel " + std::to_string(channel) + " vol " + hex32(vol_l) + "/" + hex32(vol_r) +
                                                 " frames " + std::to_string(frames) + (stereo ? " stereo" : " mono") + (movie ? " movie" : ""));
                }
            }
            {
                // RR_AUDIO_LEVELS=1: level of what the game pushes (before the mixer), every ~0.5 s of audio per channel.
                static const bool levels = std::getenv("RR_AUDIO_LEVELS") != nullptr;
                if (levels) {
                    struct Meter { double sum{}; std::uint64_t n{}; int peak{}; std::uint64_t clipped{}; std::uint64_t frames{}; };
                    static Meter meters[8];
                    Meter &m = meters[channel];
                    for (std::size_t i = 0; i < pcm.size(); ++i) {
                        const int v = pcm[i];
                        m.sum += static_cast<double>(v) * v; ++m.n;
                        m.peak = std::max(m.peak, std::abs(v));
                        if (v >= 32767 || v <= -32768) ++m.clipped;
                    }
                    m.frames += frames;
                    if (m.frames >= 22050u) {
                        const double rms = m.n ? std::sqrt(m.sum / static_cast<double>(m.n)) : 0.0;
                        char b[200];
                        std::snprintf(b, sizeof b, "AUDIOLEVEL frame=%llu ch=%u vol=%04X/%04X rms=%.1fdBFS peak=%.1fdBFS clipped=%llu",
                                      static_cast<unsigned long long>(g_frames_presented), channel, vol_l, vol_r,
                                      20.0 * std::log10(std::max(rms, 1.0) / 32768.0), 20.0 * std::log10(std::max(m.peak, 1) / 32768.0),
                                      static_cast<unsigned long long>(m.clipped));
                        log_line(category::kHle, b);
                        if (channel == 1u) log_line(category::kHle, "SASVOICES step=" + std::to_string(gates::game_step()) + " " + psprecomp::sas_debug_voice_summary());
                        m = Meter{};
                    }
                }
            }
            audio_channel_push(static_cast<int>(channel), pcm.data(), frames, stereo, static_cast<int>(std::min<std::uint32_t>(vol_l, 0x8000u)),
                               static_cast<int>(std::min<std::uint32_t>(vol_r, 0x8000u)));
        };
        const auto audio_block = [](Runtime &rt, AllegrexContext &ctx, std::uint32_t channel) {
            const std::uint32_t samples = channel < 8u ? channels[channel].samples : 1024u;
            const std::uint64_t microseconds = static_cast<std::uint64_t>(samples) * 1000000ull / 44100ull;
            ++g_delay_count;
            ThreadRecord *thread = current_thread();
            if (thread == nullptr) { ctx.set_gpr(2, 0u); return; }
            save_continuation(*thread, ctx);
            thread->suspended.set_gpr(2, 0u);
            thread->state = ThreadState::Delayed;
            thread->delay_until_us = g_virtual_time_us + microseconds;
            if (!activate_next(rt, ctx, "audio-old")) {
                ++g_deadlock_count;
                rt.stop("PSP scheduler deadlock while pacing sceAudio output");
            }
        };
        register_import(runtime, "sceAudio", 0x5EC81C55u, "sceAudioChReserve",
            [](Runtime &, AllegrexContext &ctx) {
                std::int32_t channel = static_cast<std::int32_t>(ctx.gpr[4]);
                if (channel < 0) {
                    for (std::int32_t i = 0; i < 8; ++i) if (!channels[i].reserved) { channel = i; break; }
                }
                if (channel < 0 || channel >= 8) { ctx.set_gpr(2, 0x80260004u); return; }
                channels[channel] = AudioChannel{true, ctx.gpr[5], ctx.gpr[6]};
                ctx.set_gpr(2, static_cast<std::uint32_t>(channel));
            });
        register_import(runtime, "sceAudio", 0x6FC46853u, "sceAudioChRelease",
            [](Runtime &, AllegrexContext &ctx) {
                if (ctx.gpr[4] < 8u) { channels[ctx.gpr[4]].reserved = false; audio_channel_clear(static_cast<int>(ctx.gpr[4])); }
                ctx.set_gpr(2, 0u);
            });
        register_import(runtime, "sceAudio", 0xCB2E439Eu, "sceAudioSetChannelDataLen",
            [](Runtime &, AllegrexContext &ctx) {
                if (ctx.gpr[4] < 8u) channels[ctx.gpr[4]].samples = ctx.gpr[5];
                ctx.set_gpr(2, 0u);
            });
        register_import(runtime, "sceAudio", 0x95FD0C2Du, "sceAudioChangeChannelConfig",
            [](Runtime &, AllegrexContext &ctx) {
                if (ctx.gpr[4] < 8u) channels[ctx.gpr[4]].format = ctx.gpr[5];
                ctx.set_gpr(2, 0u);
            });
        register_import(runtime, "sceAudio", 0xB7E1D8E7u, "sceAudioChangeChannelVolume",
            [](Runtime &, AllegrexContext &ctx) {
                audio_channel_volume(static_cast<int>(ctx.gpr[4]), static_cast<int>(std::min<std::uint32_t>(ctx.gpr[5], 0x8000u)),
                                     static_cast<int>(std::min<std::uint32_t>(ctx.gpr[6], 0x8000u)));
                ctx.set_gpr(2, 0u);
            });
        register_import(runtime, "sceAudio", 0xE9D97901u, "sceAudioGetChannelRestLen",
            [](Runtime &, AllegrexContext &ctx) { ctx.set_gpr(2, audio_channel_queued(static_cast<int>(ctx.gpr[4]))); });
        // sceAudioOutput(ch, vol, buf) / sceAudioOutputPanned(ch, left, right, buf): queue and return.
        register_import(runtime, "sceAudio", 0x8C1009B2u, "sceAudioOutput",
            [audio_play](Runtime &rt, AllegrexContext &ctx) { audio_play(rt, ctx.gpr[4], ctx.gpr[6], ctx.gpr[5], ctx.gpr[5]); ctx.set_gpr(2, 0u); });
        register_import(runtime, "sceAudio", 0xE2D56B2Du, "sceAudioOutputPanned",
            [audio_play](Runtime &rt, AllegrexContext &ctx) { audio_play(rt, ctx.gpr[4], ctx.gpr[7], ctx.gpr[5], ctx.gpr[6]); ctx.set_gpr(2, 0u); });
        // The blocking variants play the buffer, then pace the calling thread by the channel's sample count.
        register_import(runtime, "sceAudio", 0x136CAF51u, "sceAudioOutputBlocking",
            [audio_block, audio_play](Runtime &rt, AllegrexContext &ctx) {
                audio_play(rt, ctx.gpr[4], ctx.gpr[6], ctx.gpr[5], ctx.gpr[5]);
                audio_block(rt, ctx, ctx.gpr[4]);
            });
        register_import(runtime, "sceAudio", 0x13F592BCu, "sceAudioOutputPannedBlocking",
            [audio_block, audio_play](Runtime &rt, AllegrexContext &ctx) {
                audio_play(rt, ctx.gpr[4], ctx.gpr[7], ctx.gpr[5], ctx.gpr[6]);
                audio_block(rt, ctx, ctx.gpr[4]);
            });
    }

    // -----------------------------------------------------------------------
    // Network libraries: report "no wireless / not available" so the game
    // disables its multiplayer features.
    // -----------------------------------------------------------------------
    {
        struct NetImport { const char *library; std::uint32_t nid; const char *name; };
        static const NetImport kNet[] = {
            {"sceNet", 0x39AF39A6u, "sceNetInit"},
            {"sceNet", 0x281928A9u, "sceNetTerm"},
            {"sceNet", 0x0BF0A3AEu, "sceNetGetLocalEtherAddr"},
            {"sceNetAdhoc", 0xE1D621D7u, "sceNetAdhocInit"},
            {"sceNetAdhoc", 0xA62C6F57u, "sceNetAdhocTerm"},
            {"sceNetAdhoc", 0x6F92741Bu, "sceNetAdhocPdpCreate"},
            {"sceNetAdhoc", 0xABED3790u, "sceNetAdhocPdpSend"},
            {"sceNetAdhoc", 0xDFE53E03u, "sceNetAdhocPdpRecv"},
            {"sceNetAdhoc", 0x7F27BB5Eu, "sceNetAdhocPdpDelete"},
            {"sceNetAdhocctl", 0xE26F226Eu, "sceNetAdhocctlInit"},
            {"sceNetAdhocctl", 0x9D689E13u, "sceNetAdhocctlTerm"},
            {"sceNetAdhocctl", 0x0AD043EDu, "sceNetAdhocctlConnect"},
            {"sceNetAdhocctl", 0x08FFF7A0u, "sceNetAdhocctlScan"},
            {"sceNetAdhocctl", 0x34401D65u, "sceNetAdhocctlDisconnect"},
            {"sceNetAdhocctl", 0x20B317A0u, "sceNetAdhocctlAddHandler"},
            {"sceNetAdhocctl", 0x6402490Bu, "sceNetAdhocctlDelHandler"},
            {"sceNetAdhocctl", 0x75ECD386u, "sceNetAdhocctlGetState"},
            {"sceNetAdhocctl", 0x81AEE1BEu, "sceNetAdhocctlGetScanInfo"},
            {"sceNetAdhocMatching", 0x2A2A1E07u, "sceNetAdhocMatchingInit"},
            {"sceNetAdhocMatching", 0x7945ECDAu, "sceNetAdhocMatchingTerm"},
            {"sceNetAdhocMatching", 0xCA5EDA6Fu, "sceNetAdhocMatchingCreate"},
            {"sceNetAdhocMatching", 0x93EF3843u, "sceNetAdhocMatchingStart"},
            {"sceNetAdhocMatching", 0x32B156B3u, "sceNetAdhocMatchingStop"},
            {"sceNetAdhocMatching", 0xF16EAF4Fu, "sceNetAdhocMatchingDelete"},
            {"sceNetAdhocMatching", 0x5E3D4B79u, "sceNetAdhocMatchingSelectTarget"},
            {"sceNetAdhocMatching", 0xEA3C6108u, "sceNetAdhocMatchingCancelTarget"},
            {"sceNetAdhocMatching", 0xB58E61B7u, "sceNetAdhocMatchingSetHelloOpt"},
            {"sceNetAdhocMatching", 0xC58BCD9Eu, "sceNetAdhocMatchingGetMembers"},
            {"sceNetAdhocMatching", 0x40F8F435u, "sceNetAdhocMatchingGetPoolMaxAlloc"},
        };
        for (const NetImport &entry : kNet) {
            // Still reports "unavailable", but logs every call (first 40 per function) so the game's multiplayer use can be
            // studied: the log shows which functions the menus call and with which arguments.
            const char *name = entry.name;
            register_import(runtime, entry.library, entry.nid, name,
                [name](Runtime &rt, AllegrexContext &ctx) {
                    static std::atomic<int> total{0};
                    static std::map<std::string, int> per;
                    static std::mutex mu;
                    int n;
                    { std::lock_guard<std::mutex> lk(mu); n = ++per[name]; }
                    if (n <= 40 && total.fetch_add(1) < 600)
                    {
                        char text[256];
                        std::snprintf(text, sizeof text, "%s(0x%08X, 0x%08X, 0x%08X, 0x%08X, 0x%08X, 0x%08X) -> %s", name,
                                      static_cast<unsigned>(ctx.gpr[4]), static_cast<unsigned>(ctx.gpr[5]), static_cast<unsigned>(ctx.gpr[6]),
                                      static_cast<unsigned>(ctx.gpr[7]), static_cast<unsigned>(ctx.gpr[8]), static_cast<unsigned>(ctx.gpr[9]),
                                      std::getenv("RR_NETSTUB") != nullptr ? "pretend-ok (RR_NETSTUB)" : "unavailable");
                        log_line("NET", text);
                    }
                    if (net::Node *node = net::shared_node()) {   // RR_NET=1: the real thing
                        ctx.set_gpr(2, net_call(rt, ctx, name, *node));
                        return;
                    }
                    // RR_NETSTUB=1 (study aid): pretend the network works but is empty, so the game gets past its first
                    // initialisation and the log shows what it does next.  No packets are sent anywhere.
                    static const bool pretend = std::getenv("RR_NETSTUB") != nullptr;
                    if (!pretend) { ctx.set_gpr(2, 0x80410001u); return; }
                    const std::string fn = name;
                    auto &mem = rt.memory();
                    std::uint32_t ret = 0u;
                    // State of the pretend network: the adhoc-control handler the game registered and whether it is "connected".
                    static std::uint32_t ctl_entry = 0u, ctl_arg = 0u, ctl_state = 0u, match_cb = 0u;
                    auto fire_ctl = [&](std::uint32_t event) {
                        {
                            char text[128];
                            std::snprintf(text, sizeof text, "firing adhocctl event %u at handler 0x%08X (arg 0x%08X)", event, ctl_entry, ctl_arg);
                            log_line("NET", text);
                        }
                        if (ctl_entry == 0u) return;
                        notify_guest_function(rt, ctx, ctl_entry, event, 0u, "adhocctl_handler", "netstub");
                        ctx.gpr[6] = ctl_arg;   // handler(event, error, arg)
                    };
                    if (fn == "sceNetAdhocctlAddHandler") {
                        ctl_entry = static_cast<std::uint32_t>(ctx.gpr[4]);
                        ctl_arg = static_cast<std::uint32_t>(ctx.gpr[5]);
                        ctx.set_gpr(2, 1u);
                        return;
                    }
                    if (fn == "sceNetAdhocctlDisconnect") {
                        ctl_state = 0u;
                        ctx.set_gpr(2, 0u);
                        fire_ctl(2u);   // ADHOCCTL_EVENT_DISCONNECTED
                        return;
                    }
                    if (fn == "sceNetAdhocctlConnect") {
                        char group[9] = {};
                        for (int i = 0; i < 8; ++i) group[i] = static_cast<char>(mem.load8(static_cast<std::uint32_t>(ctx.gpr[4]) + static_cast<std::uint32_t>(i)));
                        char text[96];
                        std::snprintf(text, sizeof text, "adhocctl group name \"%s\"", group);
                        log_line("NET", text);
                        ctl_state = 1u;
                        ctx.set_gpr(2, 0u);
                        fire_ctl(1u);   // ADHOCCTL_EVENT_CONNECTED
                        return;
                    }
                    if (fn == "sceNetAdhocctlScan") {
                        ctx.set_gpr(2, 0u);
                        fire_ctl(3u);   // ADHOCCTL_EVENT_SCAN
                        return;
                    }
                    if (fn == "sceNetAdhocMatchingCreate") {
                        match_cb = mem.load32(static_cast<std::uint32_t>(ctx.gpr[29]));
                        char text[96];
                        std::snprintf(text, sizeof text, "matching callback is 0x%08X", match_cb);
                        log_line("NET", text);
                    }
                    if (fn == "sceNetAdhocPdpRecv") {
                        ctx.set_gpr(2, 0x80410709u);   // SCE_NET_ADHOC_NO_DATA_AVAILABLE: nothing has arrived
                        return;
                    }
                    if (fn == "sceNetAdhocctlGetState") {
                        if (ctx.gpr[4] != 0u) mem.store32(static_cast<std::uint32_t>(ctx.gpr[4]), ctl_state);
                        ctx.set_gpr(2, 0u);
                        return;
                    }
                    if (fn == "sceNetGetLocalEtherAddr") {
                        const std::uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
                        for (int i = 0; i < 6; ++i) mem.store8(static_cast<std::uint32_t>(ctx.gpr[4]) + static_cast<std::uint32_t>(i), mac[i]);
                    } else if (fn == "sceNetAdhocctlGetScanInfo") {
                        if (ctx.gpr[4] != 0u) mem.store32(static_cast<std::uint32_t>(ctx.gpr[4]), 0u);   // nothing found
                    } else if (fn == "sceNetAdhocMatchingCreate" || fn == "sceNetAdhocPdpCreate" || fn == "sceNetAdhocctlAddHandler") {
                        ret = 1u;   // an id / handle
                    }
                    ctx.set_gpr(2, ret);
                });
        }
    }

    // -----------------------------------------------------------------------
    // IoFileMgr assign/unassign: the virtual disc has fixed device names.
    // -----------------------------------------------------------------------
    register_import(runtime, "IoFileMgrForUser", 0xB2A628C1u, "sceIoAssign",
        [](Runtime &, AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    register_import(runtime, "IoFileMgrForUser", 0x6D08A871u, "sceIoUnassign",
        [](Runtime &, AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });

    // -----------------------------------------------------------------------
    // UtilsForUser: cache maintenance is a no-op on the recompiler.
    // -----------------------------------------------------------------------
    register_import(runtime, "UtilsForUser", 0x79D1C3FAu, "sceKernelDcacheWritebackAll",
        [](Runtime &, AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    register_import(runtime, "UtilsForUser", 0x3EE30821u, "sceKernelDcacheWritebackRange",
        [](Runtime &, AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    register_import(runtime, "UtilsForUser", 0x34B9FA9Eu, "sceKernelDcacheWritebackInvalidateRange",
        [](Runtime &, AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    register_import(runtime, "UtilsForUser", 0x71EC4271u, "sceKernelCacheOp(0x71EC4271)",
        [](Runtime &, AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    register_import(runtime, "UtilsForUser", 0x91E4F6A7u, "sceKernelCacheOp(0x91E4F6A7)",
        [](Runtime &, AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    register_import(runtime, "UtilsForUser", 0x27CC57F0u, "sceKernelCacheOp(0x27CC57F0)",
        [](Runtime &, AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });

    // -----------------------------------------------------------------------
    // sceSuspendForUser
    // -----------------------------------------------------------------------
    register_import(runtime, "sceSuspendForUser", 0xEADB1BD7u, "sceKernelPowerLock",
        [](Runtime &, AllegrexContext &ctx) { ctx.set_gpr(2, ctx.gpr[4] == 0u ? 0u : 0x80000107u); });
    register_import(runtime, "sceSuspendForUser", 0x3AEE7261u, "sceKernelPowerUnlock",
        [](Runtime &, AllegrexContext &ctx) { ctx.set_gpr(2, ctx.gpr[4] == 0u ? 0u : 0x80000107u); });
    register_import(runtime, "sceSuspendForUser", 0x090CCB3Fu, "sceKernelPowerTick",
        [](Runtime &, AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    // Volatile RAM: 4 MiB at 0x08400000, the extra memory PSP-2000+ exposes as
    // scratch.  Games lock it during boot to grow their own heaps -- MotorStorm
    // stores the returned base pointer as its arena and *spins* in its
    // allocator until it is non-zero, so failing this call hangs the game right
    // after the boot sequence.  The call takes output pointers, not values.
    register_import(runtime, "sceSuspendForUser", 0x3E0271D3u, "sceKernelVolatileMemLock",
        [](Runtime &rt, AllegrexContext &ctx) {
            const std::uint32_t type = ctx.gpr[4];
            const std::uint32_t address_out = ctx.gpr[5];
            const std::uint32_t size_out = ctx.gpr[6];
            if (type != 0u) {
                ctx.set_gpr(2, 0x80000107u); // invalid mode
                return;
            }
            if (g_volatile_mem_locked) {
                ctx.set_gpr(2, 0x800201B1u); // SCE_KERNEL_ERROR_POWER_VMEM_IN_USE
                return;
            }
            if (address_out != 0u && rt.memory().contains(address_out, 4u))
                rt.memory().store32(address_out, kVolatileMemAddress);
            if (size_out != 0u && rt.memory().contains(size_out, 4u))
                rt.memory().store32(size_out, kVolatileMemSize);
            g_volatile_mem_locked = true;
            hle_line("sceKernelVolatileMemLock -> address=" + hex32(kVolatileMemAddress) +
                     " size=" + hex32(kVolatileMemSize));
            ctx.set_gpr(2, 0u);
        });
    register_import(runtime, "sceSuspendForUser", 0xA569E425u, "sceKernelVolatileMemUnlock",
        [](Runtime &, AllegrexContext &ctx) {
            g_volatile_mem_locked = false;
            ctx.set_gpr(2, 0u);
        });

    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // sceUmdUser: game-neutral virtual drive state machine.
    //
    // The host supplies a disc, so the drive powers up inserted and ready
    // (kUmdPresent|kUmdReady).  sceUmdActivate() spins it up to readable
    // (kUmdReadable) and delivers the registered UMD callback through the
    // normal guest callback path; every transition wakes matching waiters.
    // -----------------------------------------------------------------------
    register_import(runtime, "sceUmdUser", 0x46EBB729u, "sceUmdCheckMedium",
        [](Runtime &, AllegrexContext &ctx) {
            hle_line("sceUmdCheckMedium -> 1 (disc present)");
            ctx.set_gpr(2, 1u);
        });
    register_import(runtime, "sceUmdUser", 0x6B4A146Cu, "sceUmdGetDriveStat",
        [](Runtime &rt, AllegrexContext &ctx) {
            // Diagnostic boot-skip (PSPRECOMP_RIDGERACER_SKIP_BOOT=N): once the
            // loading screen has been up for N frames, walk the Boot scene's
            // object to the "finished" state the scene-change handler looks for
            // (obj+0x40 == -2).  This runs in the present phase, i.e. after the
            // update forced -1 and before the scene-change handler runs, which
            // is exactly when the game's own loading logic would set it.  It is
            // a bring-up probe for the missing producer of that marker.
            static const std::uint64_t skip_after = [] {
                const char *text = std::getenv("PSPRECOMP_RIDGERACER_SKIP_BOOT");
                return text != nullptr ? std::strtoull(text, nullptr, 0) : std::uint64_t{0};
            }();
            if (skip_after != 0u) {
                static std::uint64_t polls = 0u;
                static bool fired = false;
                ++polls;
                if (!fired && polls >= skip_after) {
                    fired = true;
                    if (rt.memory().contains(0x08A9E48Cu, 4u)) {
                        const std::uint32_t object = rt.memory().load32(0x08A9E48Cu);
                        if (object != 0u && rt.memory().contains(object + 0x40u, 4u)) {
                            rt.memory().store32(object + 0x40u, 0xFFFFFFFEu);
                            log_line(category::kGe, "skip-boot: scene object " + hex32(object) +
                                " marked complete after " + std::to_string(polls) + " polls");
                        }
                    }
                }
            }
            // Diagnostic movie skip (PSPRECOMP_RIDGERACER_SKIP_MOVIE=1): the PMF
            // player cannot run without MPEG decoding, so once the game raises
            // its "movie playing" flag (0x08AB58E6) the profile calls the game's
            // own completion callback (0x0893BC5C) with status 0 -- exactly the
            // call the player makes when a movie ends.  The callback clears the
            // flag and updates the movie state machine, which a bare flag clear
            // does not (the game then exits instead of continuing).
            if (std::getenv("PSPRECOMP_RIDGERACER_SKIP_MOVIE") != nullptr) {
                static std::uint64_t movie_completions = 0u;
                static bool in_callback = false;
                if (!in_callback && rt.memory().contains(0x08AB58E6u, 1u) &&
                    rt.memory().load8(0x08AB58E6u) != 0u) {
                    if (movie_completions < 8u) {
                        ++movie_completions;
                        in_callback = true;
                        log_line(category::kGe, "skip-movie: invoking movie completion (#" +
                            std::to_string(movie_completions) + ")");
                        notify_guest_function(rt, ctx, 0x0893BC5Cu, 0u, 0u, "movie-complete",
                                              "skip-movie");
                        in_callback = false;
                        ctx.set_gpr(2, g_umd_state);
                        return;
                    }
                }
            }
            // Diagnostic movie-scene skip
            // (PSPRECOMP_RIDGERACER_SKIP_MOVIE_SCENE=1): the host cannot decode
            // the PMF/ATRAC stream, so the movie's player threads keep polling
            // for data that never arrives and starve the main thread.  Once the
            // movie pipeline has come up, mark the current scene complete (the
            // same obj+0x40 == -2 marker the scene-change handler looks for)
            // and reap the middleware player threads, letting the scene runner
            // advance to the next scene.  Bring-up probe in the same spirit as
            // SKIP_BOOT.
            if (std::getenv("PSPRECOMP_RIDGERACER_SKIP_MOVIE_SCENE") != nullptr) {
                static bool fired_movie_scene = false;
                if (!fired_movie_scene && rt.memory().contains(0x08AB58E4u, 1u) &&
                    rt.memory().load8(0x08AB58E4u) != 0u &&
                    rt.memory().contains(0x08AB58E6u, 1u) &&
                    rt.memory().load8(0x08AB58E6u) == 0u) {
                    fired_movie_scene = true;
                    if (rt.memory().contains(0x08A9E48Cu, 4u)) {
                        const std::uint32_t object = rt.memory().load32(0x08A9E48Cu);
                        if (object != 0u && rt.memory().contains(object + 0x40u, 4u)) {
                            rt.memory().store32(object + 0x40u, 0xFFFFFFFEu);
                            log_line(category::kGe, "skip-movie-scene: scene object " +
                                hex32(object) + " marked complete");
                        }
                    }
                    std::vector<std::int32_t> workers;
                    for (const auto &[uid, thread] : g_threads) {
                        if (uid != g_current_uid && thread.entry == 0x08A40DF8u &&
                            thread.state != ThreadState::Completed)
                            workers.push_back(uid);
                    }
                    for (const std::int32_t uid : workers) {
                        log_line(category::kGe, "skip-movie-scene: terminating movie worker uid=" +
                            std::to_string(uid));
                        complete_thread(rt, ctx, uid, 0u, false);
                        // This diagnostic cancellation bypasses the worker's
                        // pthread cleanup. Release only mutex semaphores we
                        // observed it acquiring, so its parent can join and
                        // destroy the movie player. Normal thread termination
                        // retains the kernel's semaphore semantics.
                        std::vector<std::int32_t> held_mutexes;
                        for (const auto &[sema_uid, sema] : g_semas) {
                            if (sema.mutex_owner == uid && sema.count == 0)
                                held_mutexes.push_back(sema_uid);
                        }
                        for (const std::int32_t sema_uid : held_mutexes) {
                            AllegrexContext release{};
                            release.gpr[4] = static_cast<std::uint32_t>(sema_uid);
                            release.gpr[5] = 1u;
                            rt.invoke_import("ThreadManForUser", 0x3F53E640u, release);
                            log_line(category::kGe, "skip-movie-scene: released worker " +
                                std::to_string(uid) + " mutex uid=" + std::to_string(sema_uid));
                        }
                    }
                    // The movie shutdown then drains the streamed audio: it
                    // waits for the sound driver to free its output buffers
                    // (SoundEvent bits).  The stream that would feed those
                    // buffers cannot run on the host, so release the drain
                    // waiters once, exactly like a completed audio stream.
                    for (auto &[flag_uid, flag] : g_event_flags) {
                        if (flag.name == "SoundEvent") {
                            signal_event_flag_bits(rt, flag, 0x09u);
                            log_line(category::kGe, "skip-movie-scene: released SoundEvent uid=" +
                                std::to_string(flag_uid) + " pattern=" +
                                hex32(flag.current_pattern));
                        }
                    }
                }
            }
            ctx.set_gpr(2, g_umd_state);
        });
    auto umd_wait = [](Runtime &rt, AllegrexContext &ctx, bool has_timeout) {
        const std::uint32_t stat = ctx.gpr[4];
        // WithTimeout takes a microsecond duration in a1, not a pointer.
        const std::uint32_t timeout = has_timeout ? ctx.gpr[5] : 0u;
        if ((stat & g_umd_state) != 0u) {
            ctx.set_gpr(2, 0u);
            return;
        }
        // Park until the drive reaches the requested bits or the timeout expires.
        ThreadRecord *thread = current_thread();
        if (thread != nullptr && timeout != 0u) {
            thread->wait_has_deadline = true;
            thread->wait_deadline_us = g_virtual_time_us + timeout;
            thread->wait_timeout_result = 0x800201C9u; // SCE_KERNEL_ERROR_WAIT_TIMEOUT
        }
        {
            std::ostringstream out;
            out << "wait stat=" << hex32(stat) << " state=" << hex32(g_umd_state)
                << " timeout_us=" << timeout << " ra=" << hex32(ctx.gpr[31]);
            log_line(category::kUmd, out.str());
        }
        g_umd_waiters.push_back(UmdWaiter{g_current_uid, stat});
        (void)block_current(rt, ctx, ThreadState::Waiting, "umd-wait-drive-stat");
    };
    register_import(runtime, "sceUmdUser", 0x8EF08FCEu, "sceUmdWaitDriveStat",
        [umd_wait](Runtime &rt, AllegrexContext &ctx) { umd_wait(rt, ctx, false); });
    register_import(runtime, "sceUmdUser", 0x4A9E5E29u, "sceUmdWaitDriveStatCB",
        [umd_wait](Runtime &rt, AllegrexContext &ctx) { umd_wait(rt, ctx, false); });
    register_import(runtime, "sceUmdUser", 0x6AF9B50Au, "sceUmdWaitDriveStatWithTimeout",
        [umd_wait](Runtime &rt, AllegrexContext &ctx) { umd_wait(rt, ctx, true); });
    register_import(runtime, "sceUmdUser", 0xAEE7404Du, "sceUmdRegisterUMDCallBack",
        [](Runtime &rt, AllegrexContext &ctx) {
            const auto callback_uid = static_cast<std::int32_t>(ctx.gpr[4]);
            const auto found = g_callbacks.find(callback_uid);
            if (found == g_callbacks.end()) {
                error_line("sceUmdRegisterUMDCallBack: unknown callback uid=" +
                           std::to_string(callback_uid));
                ctx.set_gpr(2, 0x80000103u); // invalid argument
                return;
            }
            g_umd_callback_uid = callback_uid;
            std::ostringstream out;
            out << "callback uid=" << callback_uid << " name=\"" << found->second.name
                << "\" entry=" << hex32(found->second.entry) << " ra=" << hex32(ctx.gpr[31])
                << " registered (drive stat=" << hex32(g_umd_state) << ")";
            log_line(category::kUmd, out.str());
            // If the drive is already readable, deliver the current state so a
            // late registration still observes "media ready".
            if (g_umd_activated)
                notify_callback(rt, ctx, callback_uid, g_umd_state, "register-already-ready");
            ctx.set_gpr(2, 0u);
        });
    register_import(runtime, "sceUmdUser", 0xBD2BDE07u, "sceUmdUnRegisterUMDCallBack",
        [](Runtime &, AllegrexContext &ctx) {
            const auto callback_uid = static_cast<std::int32_t>(ctx.gpr[4]);
            if (callback_uid != g_umd_callback_uid) {
                log_line(category::kUmd, "unregister callback uid=" + std::to_string(callback_uid) +
                                             " does not match the registered UMD callback");
                ctx.set_gpr(2, 0x80000103u);
                return;
            }
            g_umd_callback_uid = 0;
            log_line(category::kUmd, "callback uid=" + std::to_string(callback_uid) + " unregistered");
            ctx.set_gpr(2, 0u);
        });
    register_import(runtime, "sceUmdUser", 0xC6183D47u, "sceUmdActivate",
        [](Runtime &rt, AllegrexContext &ctx) {
            const std::uint32_t mode = ctx.gpr[4];
            const std::string name = guest_string(rt, ctx.gpr[5], 32u);
            if (mode < 1u || mode > 2u || name.rfind("disc0:", 0u) != 0u) {
                std::ostringstream out;
                out << "activate ignored mode=" << mode << " name=\"" << name
                    << "\" (expects mode 1-2 and \"disc0:\")";
                log_line(category::kUmd, out.str());
                ctx.set_gpr(2, 0x80000103u);
                return;
            }
            g_umd_activated = true;
            umd_set_state(rt, ctx, kUmdPresent | kUmdReady | kUmdReadable, "activate");
            ctx.set_gpr(2, 0u);
        });

    // -----------------------------------------------------------------------
    // sceRtc
    // -----------------------------------------------------------------------
    // Wall-clock time here made guest execution depend on host speed: the same
    // guest code took different branches depending on how fast the emulator
    // was.  sceRtcGetCurrentTick must report the same execution-driven clock
    // the scheduler uses, so guest timing is deterministic.
    register_import(runtime, "sceRtc", 0x3F7AD767u, "sceRtcGetCurrentTick",
        [](Runtime &rt, AllegrexContext &ctx) {
            const std::uint32_t address = ctx.gpr[4];
            const std::uint64_t microseconds = g_virtual_time_us;
            if (address != 0u && rt.memory().contains(address, 8u)) {
                rt.memory().store32(address, static_cast<std::uint32_t>(microseconds));
                rt.memory().store32(address + 4u, static_cast<std::uint32_t>(microseconds >> 32u));
            }
            ctx.set_gpr(2, 0u);
        });
    register_import(runtime, "sceRtc", 0xE7C27D1Bu, "sceRtcGetCurrentClockLocalTime",
        [](Runtime &rt, AllegrexContext &ctx) {
            const std::uint32_t address = ctx.gpr[4];
            if (address == 0u || !rt.memory().contains(address, 16u)) {
                ctx.set_gpr(2, 0x80000103u);
                return;
            }
            // Derived from the virtual clock so the date is deterministic too.
            const std::time_t now = static_cast<std::time_t>(g_virtual_time_us / 1'000'000u);
            std::tm local{};
#if defined(_WIN32)
            gmtime_s(&local, &now);
#else
            gmtime_r(&now, &local);
#endif
            rt.memory().zero(address, 16u);
            rt.memory().store16(address + 0u, static_cast<std::uint16_t>(local.tm_year + 1900));
            rt.memory().store16(address + 2u, static_cast<std::uint16_t>(local.tm_mon + 1));
            rt.memory().store16(address + 4u, static_cast<std::uint16_t>(local.tm_mday));
            rt.memory().store16(address + 6u, static_cast<std::uint16_t>(local.tm_hour));
            rt.memory().store16(address + 8u, static_cast<std::uint16_t>(local.tm_min));
            rt.memory().store16(address + 10u, static_cast<std::uint16_t>(local.tm_sec));
            rt.memory().store32(address + 12u, 0u);
            ctx.set_gpr(2, 0u);
        });

    // -----------------------------------------------------------------------
    // scePower
    // -----------------------------------------------------------------------
    register_import(runtime, "scePower", 0x04B7766Eu, "scePowerIsBatteryExist",
        [](Runtime &, AllegrexContext &ctx) { ctx.set_gpr(2, 1u); });
    register_import(runtime, "scePower", 0x87440F5Eu, "scePowerIsPowerOnline",
        [](Runtime &, AllegrexContext &ctx) { ctx.set_gpr(2, 1u); });
    register_import(runtime, "scePower", 0xDFA8BAF8u, "scePowerIsBatteryCharging",
        [](Runtime &, AllegrexContext &ctx) { ctx.set_gpr(2, 1u); });
    register_import(runtime, "scePower", 0xEBD177D6u, "scePowerGetBatteryLifePercent",
        [](Runtime &, AllegrexContext &ctx) { ctx.set_gpr(2, 100u); });

    // -----------------------------------------------------------------------
    // sceImpose / sceDmac
    // -----------------------------------------------------------------------
    register_import(runtime, "sceImpose", 0x36AA6E91u, "sceImposeSetLanguageMode",
        [](Runtime &, AllegrexContext &ctx) { ctx.set_gpr(2, 0u); });
    register_import(runtime, "sceDmac", 0x617F3FE6u, "sceDmacMemcpy",
        [](Runtime &rt, AllegrexContext &ctx) {
            const std::uint32_t destination = ctx.gpr[4];
            const std::uint32_t source = ctx.gpr[5];
            const std::uint32_t size = ctx.gpr[6];
            if (!rt.memory().contains(destination, size == 0u ? 1u : size) ||
                !rt.memory().contains(source, size == 0u ? 1u : size)) {
                ctx.set_gpr(2, 0x80000102u);
                return;
            }
            if (size != 0u) {
                std::vector<std::uint8_t> data(size);
                rt.memory().copy_out(source, data);
                rt.memory().copy_in(destination, data);
            }
            ctx.set_gpr(2, 0u);
        });

    // -----------------------------------------------------------------------
    // sceUtility: system parameters are answered with sane defaults; firmware
    // module loading is the gate to the AV codec and networking subsystems.
    // LoadModule is understood, but successfully loading a firmware PRX
    // without implementing the services it exports would be a fake success, so
    // it reports the exact module and stops.
    // -----------------------------------------------------------------------
    register_import(runtime, "sceUtility", 0xA5DA2406u, "sceUtilityGetSystemParamInt",
        [](Runtime &rt, AllegrexContext &ctx) {
            const std::uint32_t param = ctx.gpr[4];
            const std::uint32_t address = ctx.gpr[5];
            std::uint32_t value = 0u;
            const char *name = "unknown";
            switch (param) {
            case 1u: name = "nickname"; value = 0u; break;
            case 2u: name = "adhoc_channel"; value = 0u; break;
            case 3u: name = "wlan_powersave"; value = 0u; break;
            case 4u: name = "date_format"; value = 0u; break;       // YYYYMMDD
            case 5u: name = "time_format"; value = 0u; break;       // 24 hour
            case 6u: name = "timezone"; value = 0u; break;          // UTC
            case 7u: name = "daylight_savings"; value = 0u; break;
            case 8u: {
                name = "language";
                value = 1u; // English (PSP_SYSTEMPARAM_LANGUAGE_ENGLISH)
                if (const char *env_lang = std::getenv("PSPRECOMP_RIDGERACER_LANGUAGE")) {
                    value = static_cast<std::uint32_t>(std::strtoul(env_lang, nullptr, 0));
                }
                break;
            }
            case 9u: name = "button_preference"; value = 1u; break; // cross = confirm
            case 10u: name = "parental_level"; value = 0u; break;   // unrestricted
            default: break;
            }
            if (address != 0u && rt.memory().contains(address, 4u))
                rt.memory().store32(address, value);
            if (param == 0u || param > 10u) {
                hle_line("sceUtilityGetSystemParamInt unknown id=" + std::to_string(param) +
                         " -> 0");
            }
            ctx.set_gpr(2, 0u);
        });
    register_import(runtime, "sceUtility", 0x34B78343u, "sceUtilityGetSystemParamString",
        [](Runtime &rt, AllegrexContext &ctx) {
            const std::uint32_t param = ctx.gpr[4];
            const std::uint32_t buffer = ctx.gpr[5];
            const std::uint32_t size = ctx.gpr[6];
            const std::string value = param == 1u ? std::string("MSPlayer") : std::string();
            if (buffer != 0u && size != 0u && rt.memory().contains(buffer, size)) {
                const std::size_t length = std::min<std::size_t>(value.size(), size - 1u);
                rt.memory().zero(buffer, size);
                for (std::size_t index = 0; index < length; ++index)
                    rt.memory().store8(buffer + static_cast<std::uint32_t>(index),
                                       static_cast<std::uint8_t>(value[index]));
            }
            ctx.set_gpr(2, 0u);
        });
    // -----------------------------------------------------------------------
    // sceUtility module manager and NP DRM entry points.
    //
    // Module loader policy: known firmware modules are reported present (0),
    // matching a real PSP whose flash0 carries them, because the guest's boot
    // path has no bounded failure handling (its retry helper always returns 1,
    // so a negative result spins forever).  This claims loader bookkeeping
    // only: none of the module SERVICES are implemented, and the DRM/AV/NET
    // imports remain unregistered, stopping loudly with [HLE MISSING] if the
    // guest ever calls one.  PSPRECOMP_RIDGERACER_MODULE_FAILURE=1 restores the
    // unusual "module unavailable" result (0x80111103) for diagnostics; with
    // MotorStorm that result makes ensure_module_loaded() retry without bound.
    // -----------------------------------------------------------------------
    auto utility_module_name = [](std::uint32_t module_id) -> const char * {
        switch (module_id) {
        case 0x0100u: return "PSP_MODULE_NET_COMMON";
        case 0x0101u: return "PSP_MODULE_NET_ADHOC";
        case 0x0102u: return "PSP_MODULE_NET_INET";
        case 0x0103u: return "PSP_MODULE_NET_PARSEURI";
        case 0x0104u: return "PSP_MODULE_NET_PARSEHTTP";
        case 0x0105u: return "PSP_MODULE_NET_HTTP";
        case 0x0106u: return "PSP_MODULE_NET_SSL";
        case 0x0300u: return "PSP_MODULE_AV_AVCODEC";
        case 0x0301u: return "PSP_MODULE_AV_SASCORE";
        case 0x0302u: return "PSP_MODULE_AV_ATRAC3PLUS";
        case 0x0303u: return "PSP_MODULE_AV_MPEGBASE";
        case 0x0304u: return "PSP_MODULE_AV_MP3";
        case 0x0305u: return "PSP_MODULE_AV_VAUDIO";
        case 0x0306u: return "PSP_MODULE_AV_AAC";
        case 0x0307u: return "PSP_MODULE_AV_G729";
        case 0x0400u: return "PSP_MODULE_NP_COMMON";
        case 0x0401u: return "PSP_MODULE_NP_SERVICE";
        case 0x0500u: return "PSP_MODULE_NP_DRM";
        default: return nullptr;
        }
    };
    register_import(runtime, "sceUtility", 0x2A2B3DE0u, "sceUtilityLoadModule",
        [utility_module_name](Runtime &, AllegrexContext &ctx) {
            const std::uint32_t module_id = ctx.gpr[4];
            const char *module_name = utility_module_name(module_id);
            std::ostringstream out;
            out << "sceUtilityLoadModule id=" << hex32(module_id) << " ("
                << (module_name != nullptr ? module_name : "unknown")
                << ") ra=" << hex32(ctx.gpr[31]);
            log_line(category::kModule, out.str());
            if (module_name == nullptr) {
                log_line(category::kModule,
                         "unknown module id -> 0x80111101 (SCE_ERROR_MODULE_BAD_ID)");
                ctx.set_gpr(2, 0x80111101u);
                return;
            }
            if (std::getenv("PSPRECOMP_RIDGERACER_MODULE_FAILURE") != nullptr) {
                log_line(category::kModule,
                         "diagnostic mode: -> 0x80111103 (SCE_ERROR_MODULE_NOT_LOADED); "
                         "no successful load claimed");
                ctx.set_gpr(2, 0x80111103u);
                return;
            }
            const bool already_loaded = g_utility_modules.contains(module_id);
            g_utility_modules[module_id] = true;
            log_line(category::kModule,
                     std::string("-> 0 (loader bookkeeping only") +
                         (already_loaded ? ", repeat load is idempotent" : "") +
                         "; module services are not implemented and any call stops with "
                         "[HLE MISSING])");
            ctx.set_gpr(2, 0u);
        });
    register_import(runtime, "sceUtility", 0xE49BFE92u, "sceUtilityUnloadModule",
        [utility_module_name](Runtime &, AllegrexContext &ctx) {
            const std::uint32_t module_id = ctx.gpr[4];
            const char *module_name = utility_module_name(module_id);
            if (module_name == nullptr) {
                log_line(category::kModule, "sceUtilityUnloadModule unknown id=" +
                                                hex32(module_id) + " -> 0x80111101");
                ctx.set_gpr(2, 0x80111101u);
                return;
            }
            if (g_utility_modules.erase(module_id) == 0u) {
                log_line(category::kModule,
                         std::string("sceUtilityUnloadModule id=") + hex32(module_id) + " (" +
                             module_name + ") -> 0x80111103 (SCE_ERROR_MODULE_NOT_LOADED)");
                ctx.set_gpr(2, 0x80111103u);
                return;
            }
            log_line(category::kModule, std::string("sceUtilityUnloadModule id=") +
                                            hex32(module_id) + " (" + module_name + ") -> 0");
            ctx.set_gpr(2, 0u);
        });
    // -----------------------------------------------------------------------
    // scePspNpDrm_user (same NID table as sceNpDrm).  These are the only NP DRM
    // services MotorStorm imports.  For plain (non-EDAT) UMD files both are
    // pure bookkeeping; encrypted EDAT files need PGD decryption and report an
    // honest "no licensee key set" failure instead of faking content.
    // -----------------------------------------------------------------------
    auto file_size_of = [](FileHandle &handle) -> std::uint64_t {
        handle.stream.clear();
        handle.stream.seekg(0, std::ios::end);
        const std::streamoff end = handle.stream.tellg();
        return end > 0 ? static_cast<std::uint64_t>(end) : 0u;
    };
    auto edat_magic_present = [](FileHandle &handle) -> bool {
        std::uint8_t header[8]{};
        handle.stream.clear();
        handle.stream.seekg(static_cast<std::streamoff>(handle.position));
        handle.stream.read(reinterpret_cast<char *>(header), sizeof(header));
        const bool read_ok = handle.stream.gcount() == static_cast<std::streamsize>(sizeof(header));
        handle.stream.clear();
        return read_ok && header[0] == 0x50 && header[1] == 0x53 && header[2] == 0x50 &&
               header[3] == 0x00 && header[4] == 0x45 && header[5] == 0x44 &&
               header[6] == 0x41 && header[7] == 0x54; // "PSP\0EDAT"
    };
    register_import(runtime, "scePspNpDrm_user", 0x08D98894u, "sceNpDrmEdataSetupKey",
        [edat_magic_present](Runtime &rt, AllegrexContext &ctx) {
            const auto fd = static_cast<std::int32_t>(ctx.gpr[4]);
            const auto found = g_files.find(fd);
            if (found == g_files.end()) {
                error_line("sceNpDrmEdataSetupKey: bad fd=" + std::to_string(fd));
                ctx.set_gpr(2, 0x80010009u);
                return;
            }
            if (!edat_magic_present(found->second)) {
                log_line(category::kFile, "sceNpDrmEdataSetupKey fd=" + std::to_string(fd) +
                                              " plain file -> 0 (no DRM)");
                ctx.set_gpr(2, 0u);
                return;
            }
            error_line("sceNpDrmEdataSetupKey: EDAT (encrypted) file requires PGD decryption, "
                       "not implemented");
            ctx.set_gpr(2, 0x80550901u); // SCE_NPDRM_ERROR_NO_K_LICENSEE_SET
        });
    register_import(runtime, "scePspNpDrm_user", 0x219EF5CCu, "sceNpDrmEdataGetDataSize",
        [file_size_of](Runtime &, AllegrexContext &ctx) {
            const auto fd = static_cast<std::int32_t>(ctx.gpr[4]);
            const auto found = g_files.find(fd);
            if (found == g_files.end()) {
                error_line("sceNpDrmEdataGetDataSize: bad fd=" + std::to_string(fd));
                ctx.set_gpr(2, static_cast<std::uint32_t>(-1));
                return;
            }
            const std::uint64_t size = found->second.directory ? 0u : file_size_of(found->second);
            log_line(category::kFile, "sceNpDrmEdataGetDataSize fd=" + std::to_string(fd) +
                                          " -> " + std::to_string(size));
            ctx.set_gpr(2, static_cast<std::uint32_t>(size));
        });
    register_import(runtime, "scePspNpDrm_user", 0xA1336091u, "sceNpDrmSetLicenseeKey",
        [](Runtime &, AllegrexContext &ctx) {
            log_line(category::kModule, "sceNpDrmSetLicenseeKey ra=" + hex32(ctx.gpr[31]) +
                                            " -> 0 (key stored by reference only; no DRM content "
                                            "is decrypted by this profile)");
            ctx.set_gpr(2, 0u);
        });

    // Shared game-neutral sceSasCore HLE (same implementation used by the VCS
    // profile): software VAG/ADSR mixer with honest PSP state.  No custom audio
    // backend is added here; audio output is a deterministic host-side mix.
    psprecomp::install_sce_sas_core_hle(runtime);

    // sceAudioOutput2 output path: blocking output parks the calling thread for the buffer's
    // duration.  Returning immediately starved lower-priority threads (the
    // SoundThread spun at priority 16 and user_main never ran again).
    psprecomp::AudioOutput2Config audio_config{};
    audio_config.clock=[](void *) {return g_virtual_time_us;};
    audio_config.delay = [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx,
                            std::uint32_t microseconds, void *) {
        ++g_delay_count;
        ThreadRecord *thread = current_thread();
        if (thread == nullptr) {
            rt.stop("sceAudioOutput2OutputBlocking with no current thread");
            return;
        }
        save_continuation(*thread, ctx);
        thread->suspended.set_gpr(2, 0u);
        thread->state = ThreadState::Delayed;
        thread->delay_until_us = g_virtual_time_us + microseconds;
        if (!activate_next(rt, ctx, "audio-output2")) {
            ++g_deadlock_count;
            rt.stop("PSP scheduler deadlock while pacing audio output");
        }
    };
    // Stage 2: audio is mixed by the shared HLE but not yet played back.
    audio_config.sink = [](void *, const psprecomp::Runtime &, std::uint32_t, std::uint32_t) {};
    psprecomp::install_sce_audio_output2_hle(runtime, audio_config);
    {
        // Every SAS voice the game starts.  The menu-sound module learns which samples the game's own cursor / confirm / back
        // presses start (see rr_menusound.hpp); RR_SE_LOG=1 also logs them (diagnostic).
        static psprecomp::Runtime *se_runtime = nullptr;
        se_runtime = &runtime;
        static const bool se_log = std::getenv("RR_SE_LOG") != nullptr;
        psprecomp::set_sas_keyon_observer([](int voice, const psprecomp::SasVoiceState &v) {
            if (se_runtime != nullptr) menu_sound_on_keyon(g_frames_presented, se_runtime->memory(), v);
            static const bool aud_log = std::getenv("RR_AUDIOLOG") != nullptr;
            if (aud_log) {
                // One line per sound the game starts.  Compare runs with tools/audiocmp.py.
                char a[400];
                std::snprintf(a, sizeof a, "AUDON s=%u f=%llu v=%d type=%d data=%08X size=%d loop=%d pitch=%X vol=%d/%d fx=%d/%d adsr=%d r=%X,%X,%X,%X sl=%X ra=%08X",
                              gates::game_step(), static_cast<unsigned long long>(g_frames_presented), voice, static_cast<int>(v.type), v.data_address, v.data_size,
                              v.loop ? 1 : 0, static_cast<unsigned>(v.pitch), v.left_volume, v.right_volume, v.effect_left_volume, v.effect_right_volume,
                              v.adsr_configured ? 1 : 0, static_cast<unsigned>(v.adsr_rates[0]), static_cast<unsigned>(v.adsr_rates[1]),
                              static_cast<unsigned>(v.adsr_rates[2]), static_cast<unsigned>(v.adsr_rates[3]), static_cast<unsigned>(v.sustain_level),
                              psprecomp::sas_last_call_ra());
                log_line(category::kHle, a);
            }
            if (!se_log) return;
            char b[200];
            std::snprintf(b, sizeof b, "SEKEYON frame=%llu voice=%d type=%d data=0x%08X size=%d pitch=%d vol=%d/%d loop=%d",
                          static_cast<unsigned long long>(g_frames_presented), voice, static_cast<int>(v.type), v.data_address, v.data_size,
                          v.pitch, v.left_volume, v.right_volume, v.loop ? 1 : 0);
            log_line(category::kHle, b);
        });
        // RR_SASLOG=1: key-off / pause calls, and volume calls that change a voice between silent and audible, with the
        // game step (engine sound still audible while paused).
        if (std::getenv("RR_SASLOG") != nullptr || std::getenv("RR_AUDIOLOG") != nullptr)
            psprecomp::set_sas_event_observer([](const char *name, int voice, int value) {
                // RR_AUDIOLOG=1: compact event stream (key-off, natural end, rejected calls, big volume changes, pause).
                static const bool aud_log = std::getenv("RR_AUDIOLOG") != nullptr;
                static const bool sas_log = std::getenv("RR_SASLOG") != nullptr;
                if (aud_log) {
                    const std::string_view n(name);
                    bool emit = true;
                    if (n == "volume") {
                        // only when a voice's volume jumps by more than 25 % (or goes silent / audible)
                        static std::unordered_map<int, int> last;
                        const int cur = value;
                        auto it = last.find(voice);
                        emit = it == last.end() || (cur == 0) != (it->second == 0) ||
                               std::abs(cur - it->second) * 4 > std::max(std::abs(it->second), 64);
                        if (emit) last[voice] = cur;
                    } else if (n == "keyon" || n.rfind("vagflags", 0) == 0) {
                        emit = false;   // keyon has its own AUDON line; vagflags is a one-off dump (RR_SASLOG)
                    }
                    if (emit) {
                        char a[200];
                        std::snprintf(a, sizeof a, "AUDEV s=%u f=%llu %s v=%d val=%d ra=%08X", gates::game_step(),
                                      static_cast<unsigned long long>(g_frames_presented), name, voice, value, psprecomp::sas_last_call_ra());
                        log_line(category::kHle, a);
                    }
                    // Every 2 s of game time: which voices are sounding right now.
                    static unsigned last_dump = 0;
                    const unsigned st = gates::game_step();
                    if (st >= last_dump + 120u || st < last_dump) {
                        last_dump = st;
                        log_line(category::kHle, "AUDVOICES s=" + std::to_string(st) + " " + psprecomp::sas_debug_voice_summary());
                    }
                }
                if (!sas_log) return;
                // The game's sound code reads its SAS core pointer from 0x08A866D0; a null there faults the countdown.
                {
                    static std::uint32_t last_core = 0xFFFFFFFFu;
                    if (g_dump_rt != nullptr) {
                        const std::uint32_t p = g_dump_rt->memory().load32(0x08A866D0u);
                        if (p != last_core) {
                            char c[160];
                            std::snprintf(c, sizeof c, "SASCOREPTR 0x%08X -> 0x%08X (at %s voice=%d) step=%u frame=%llu", last_core, p, name, voice,
                                          gates::game_step(), static_cast<unsigned long long>(g_frames_presented));
                            log_line(category::kHle, c);
                            last_core = p;
                        }
                    }
                }
                static std::unordered_map<int, bool> silent;
                static std::uint32_t lines = 0;
                const bool vol = std::string_view(name) == "volume";
                if (vol) {
                    const bool s = value == 0;
                    auto it = silent.find(voice);
                    if (it != silent.end() && it->second == s) return;
                    silent[voice] = s;
                }
                if (lines++ > 30000u) return;
                char b[400];
                std::snprintf(b, sizeof b, "SASEV %s voice=%d value=%d step=%u frame=%llu", name, voice, value, gates::game_step(),
                              static_cast<unsigned long long>(g_frames_presented));
                log_line(category::kHle, b);
            });
    }
    install_atrac_hle(runtime);
    install_mpeg_hle(runtime);
    install_utility_hle(runtime);
    utility_set_interactive(g_hle_options.window && !g_replay_active);

    // Save data folder: PSPRECOMP_RIDGERACER_SAVEDATA if set; otherwise a "savedata" folder next to the executable.
    // With --dev the old location (SAVEDATA two folders above the game files) is still used when it exists, so
    // developer setups keep their existing saves.
    const auto save_root_env = std::getenv("PSPRECOMP_RIDGERACER_SAVEDATA");
    std::filesystem::path save_root_path;
    if (save_root_env && *save_root_env) {
        save_root_path = save_root_env;
    } else {
        const auto legacy = runtime.game_root().parent_path().parent_path() / "SAVEDATA";
        std::error_code legacy_ec;
        if (rr::g_rr_dev && std::filesystem::is_directory(legacy, legacy_ec))
            save_root_path = legacy;
        else
            save_root_path = rr::executable_dir() / "savedata";
    }
    {
        std::error_code mkdir_ec;
        std::filesystem::create_directories(save_root_path, mkdir_ec);
    }
    hle_line("savedata folder: " + save_root_path.string());
    psprecomp::install_savedata_hle(runtime,
        save_root_path,
        [&runtime](std::uint32_t parameter, std::uint32_t result) {
            const auto &memory = runtime.memory();
            const auto mode = memory.load32(parameter + 0x30);
            std::ostringstream line;
            line << "mode=" << mode
                << " game=" << memory.read_c_string(parameter + 0x3C, 13)
                << " slot=" << memory.read_c_string(parameter + 0x4C, 20)
                << " file=" << memory.read_c_string(parameter + 0x64, 13)
                << " bytes=" << memory.load32(parameter + 0x7C) << " result=" << hex32(result);
            // Delete-family modes carry their targets in saveNameList (offset 0x60);
            // dump it plus the surrounding parameter words so the HLE behavior for
            // these modes can be verified against the real request.
            if (mode == 6 || mode == 7 || mode == 10 || mode == 21) {
                const auto list = memory.load32(parameter + 0x60);
                line << " list=" << hex32(list);
                if (list && memory.contains(list, 20)) {
                    for (int i = 0; i < 16; ++i) {
                        if (!memory.contains(list + static_cast<std::uint32_t>(i) * 20u, 20)) break;
                        const auto name = memory.read_c_string(list + static_cast<std::uint32_t>(i) * 20u, 20);
                        if (name.empty()) break;
                        line << " [" << i << "]=\"" << name << "\"";
                    }
                }
                line << " raw=";
                for (std::uint32_t offset = 0x40; offset < 0x90; offset += 4)
                    line << hex32(memory.load32(parameter + offset)) << ' ';
            }
            log_line("SAVEDATA", line.str());
        });
    hle_line("HLE installation complete: scheduler, filesystem, savedata, SAS, AudioOutput2");
}

std::uint64_t guest_time_us() { return g_virtual_time_us; }

void dump_scheduler(const char *why) {
    log_line(category::kDispatch, std::string("scheduler dump (") + why + ")");
    for (const auto &[tid, label] : g_last_import)
        log_line(category::kDispatch, "last import of thread " + std::to_string(tid) + ": " + (label ? *label : std::string("?")));
    {
        std::vector<std::pair<std::uint64_t, std::string>> diff;
        for (const auto &[name, count] : g_import_calls) {
            const auto before = g_import_snapshot.find(name);
            const std::uint64_t delta = count - (before == g_import_snapshot.end() ? 0u : before->second);
            if (delta != 0u) diff.emplace_back(delta, name);
        }
        std::sort(diff.rbegin(), diff.rend());
        std::ostringstream out;
        out << "imports since the last presented frame:";
        std::size_t shown = 0;
        for (const auto &[delta, name] : diff) { if (++shown > 12u) break; out << " " << name << "=" << delta; }
        log_line(category::kDispatch, out.str());
    }
        std::ostringstream out;
        out << "switches=" << g_switch_count << " delays=" << g_delay_count
            << " preempts=" << g_preempt_count << " deadlocks=" << g_deadlock_count
            << " virtual_time_us=" << g_virtual_time_us
            << " threads_created=" << (g_next_thread_uid - 1);
        log_line(category::kDispatch, out.str());
        for (const auto &[uid, thread] : g_threads) {
            std::ostringstream thread_out;
            thread_out << "uid=" << uid << " name=\"" << thread.name << "\""
                       << " entry=" << hex32(thread.entry) << " priority=" << thread.priority
                       << " state=" << static_cast<int>(thread.state)
                       << " pc=" << hex32(thread.suspended.pc)
                       << " ra=" << hex32(thread.suspended.gpr[31])
                       << " sp=" << hex32(thread.suspended.gpr[29]);
            log_line(category::kDispatch, thread_out.str());
        }
        // Guest-memory snapshot of the threads' stacks and the code they
        // return into, for offline disassembly ("GUESTSTACK"/"GUESTCODE").
        if (g_dump_rt != nullptr) {
            auto &mem = g_dump_rt->memory();
            auto is_code = [](std::uint32_t v) { return v >= 0x08804000u && v < 0x08A00000u && (v & 3u) == 0u; };
            std::set<std::uint32_t> dumped;
            auto dump_code = [&](std::uint32_t center, const char *tag) {
                const std::uint32_t from = (center - 0x90u) & ~3u;
                if (!dumped.insert(from & ~0x3Fu).second) return;
                std::ostringstream code;
                code << "GUESTCODE " << tag << " " << hex32(from) << ":";
                for (std::uint32_t a = from; a < center + 0x30u; a += 4u)
                    if (mem.contains(a, 4u)) code << ' ' << hex32(mem.load32(a));
                log_line(category::kDispatch, code.str());
            };
            // Audio slot table (0x80 bytes per slot), words 0..31 of slots 0-3.
            for (std::uint32_t slot = 0; slot < 4u; ++slot) {
                std::ostringstream row;
                row << "GUESTSLOT " << slot << ":";
                for (std::uint32_t w = 0; w < 32u; ++w) {
                    const std::uint32_t a = 0x08A95A54u + slot * 0x80u + w * 4u;
                    if (mem.contains(a, 4u)) row << ' ' << hex32(mem.load32(a));
                }
                log_line(category::kDispatch, row.str());
            }
            // Fixed ranges: the audio-stream middleware that user_main is waiting on.
            for (std::uint32_t a = 0x08916C80u; a < 0x08916C80u; a += 0x100u) {
                std::ostringstream range;
                range << "GUESTRANGE " << hex32(a) << ":";
                for (std::uint32_t w = a; w < a + 0x100u; w += 4u)
                    if (mem.contains(w, 4u)) range << ' ' << hex32(mem.load32(w));
                log_line(category::kDispatch, range.str());
            }
            for (const auto &[uid, thread] : g_threads) {
                if (uid != 1) continue;   // user_main
                const std::uint32_t sp = thread.suspended.gpr[29];
                dump_code(thread.suspended.pc, "pc");
                std::ostringstream stack;
                stack << "GUESTSTACK uid=" << uid << " sp=" << hex32(sp) << ":";
                std::vector<std::uint32_t> candidates;
                for (std::uint32_t a = sp; a < sp + 0x300u && mem.contains(a, 4u); a += 4u) {
                    const std::uint32_t v = mem.load32(a);
                    stack << ' ' << hex32(v);
                    if (is_code(v) && candidates.size() < 14u) candidates.push_back(v);
                }
                log_line(category::kDispatch, stack.str());
                for (const std::uint32_t c : candidates) dump_code(c, "stackret");
            }
        }
        for (const auto &[uid,cb] : g_callbacks)
            if (cb.notify_count)
                log_line("CALLBACK", "pending uid=" + std::to_string(uid) + " owner=" +
                    std::to_string(cb.owner_uid) + " count=" + std::to_string(cb.notify_count));
        for (const auto &[uid, flag] : g_event_flags) {
            std::ostringstream flag_out;
            flag_out << "eventflag uid=" << uid << " name=\"" << flag.name << "\""
                     << " pattern=" << hex32(flag.current_pattern) << " waiters=";
            if (flag.waiters.empty()) {
                flag_out << "none";
            } else {
                for (std::size_t index = 0; index < flag.waiters.size(); ++index) {
                    if (index != 0u) flag_out << ",";
                    flag_out << flag.waiters[index].uid << "/"
                             << hex32(flag.waiters[index].requested) << "/"
                             << hex32(flag.waiters[index].mode);
                }
            }
            log_line(category::kDispatch, flag_out.str());
        }
        for (const auto &[uid, sema] : g_semas) {
            if (sema.waiters.empty() && sema.mutex_owner < 0) continue;
            std::ostringstream sema_out;
            sema_out << "sema uid=" << uid << " name=\"" << sema.name << "\" count=" << sema.count
                     << " owner=" << sema.mutex_owner << " waiters=";
            for (const auto &waiter : sema.waiters) sema_out << waiter.uid << '/' << waiter.amount << ',';
            log_line(category::kDispatch, sema_out.str());
        }
    for (const auto &[uid, thread] : g_threads) {
        if (thread.state != ThreadState::Waiting) continue;
        std::ostringstream wait_out;
        wait_out << "thread " << uid << " \"" << thread.name << "\" is WAITING:";
        for (const auto &[sid, sema] : g_semas)
            for (const auto &w : sema.waiters)
                if (w.uid == uid) wait_out << " semaphore " << sid << " \"" << sema.name << "\" need=" << w.amount << " count=" << sema.count;
        for (const auto &[fid, flag] : g_event_flags)
            for (const auto &w : flag.waiters)
                if (w.uid == uid) wait_out << " eventflag " << fid << " \"" << flag.name << "\" want=" << hex32(w.requested) << " have=" << hex32(flag.current_pattern);
        for (const auto &[tid, list] : g_joiners)
            for (const auto &w : list)
                if (w.uid == uid) wait_out << " thread-end " << tid;
        log_line(category::kDispatch, wait_out.str());
    }
}

void set_perf_report(const std::filesystem::path &file) { perfrep::path = file; }

void report_summary() {
    perfrep::write_report();
    g_watchdog_stop = true;
    if (g_watchdog.joinable()) g_watchdog.join();
    audio_stop();
    present_close();
    if (g_ge) g_ge->report();
    if (g_backend) log_line(category::kGe, "software backend: pixels_written=" + std::to_string(g_backend->pixels_written()) + " unreadable_texture_draws=" + std::to_string(g_backend->unsupported_texture_draws()) + " frames_presented=" + std::to_string(g_frames_presented));
    {
        std::vector<std::pair<std::string, std::uint64_t>> imports(g_import_calls.begin(),
                                                                   g_import_calls.end());
        std::sort(imports.begin(), imports.end(), [](const auto &left, const auto &right) {
            return left.second > right.second;
        });
        std::uint64_t total_calls = 0;
        for (const auto &[name, count] : imports) total_calls += count;
        std::ostringstream out;
        out << "imports_with_calls=" << imports.size() << " total_calls=" << total_calls;
        log_line(category::kHle, out.str());
        std::size_t shown = 0;
        for (const auto &[name, count] : imports) {
            if (shown++ >= 25u) break;
            log_line(category::kImport, name + " calls=" + std::to_string(count));
        }
    }
    {
        std::ostringstream out;
        out << "opens=" << g_open_count << " missing=" << g_open_missing
            << " reads=" << g_read_count << " read_bytes=" << g_read_bytes
            << " largest_read=" << g_largest_read
            << " seeks=" << g_seek_count << " writes=" << g_write_count
            << " written_bytes=" << g_write_bytes;
        log_line(category::kFilesystem, out.str());
    }
    {
        std::ostringstream out;
        out << "unique_paths=" << g_tracked_path_set.size() << " accesses=" << g_path_access_total
            << " (first " << std::min<std::size_t>(g_tracked_paths.size(), 50u) << " listed)";
        log_line(category::kFile, out.str());
        const std::size_t count = std::min<std::size_t>(g_tracked_paths.size(), 50u);
        for (std::size_t index = 0; index < count; ++index)
            log_line(category::kFile, "path[" + std::to_string(index) + "]=" + g_tracked_paths[index]);
    }
    {
        log_line(category::kUmd, "transitions=" + std::to_string(g_umd_transitions) +
                                     " final_state=" + hex32(g_umd_state) +
                                     " activated=" + (g_umd_activated ? "1" : "0") +
                                     " callback_uid=" + std::to_string(g_umd_callback_uid) +
                                     " pending_waiters=" + std::to_string(g_umd_waiters.size()));
    }
    dump_scheduler("exit");
}


} // namespace rr
