#include "rr_utility.hpp"

#include "psprecomp/common.hpp"
#include "psprecomp/runtime.hpp"
#include "rr_log.hpp"
#include "rr_menusound.hpp"
#include "rr_osk_ui.hpp"
#include "rr_present.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace rr {
namespace {

using psprecomp::AllegrexContext;
using psprecomp::GuestMemory;
using psprecomp::Runtime;
using psprecomp::hex32;

// Dialog states as the games see them.
enum Status : std::uint32_t { kNone = 0, kInitialize = 1, kRunning = 2, kFinished = 3 };
constexpr std::uint32_t kUpdatesToFinish = 20u;   // ~1/3 s, so the game's transition can play

struct Dialog {
    Status status{kNone};
    std::uint32_t params{};
    std::uint32_t updates{};
};
Dialog g_osk, g_msg;
bool g_interactive = false;
GuestMemory *g_dump_rt_ptr = nullptr;

void dump_words(GuestMemory &m, const char *tag, std::uint32_t at, std::uint32_t words) {
    if (!m.contains(at, words * 4u)) return;
    std::string line = std::string(tag) + " " + hex32(at) + ":";
    for (std::uint32_t i = 0; i < words; ++i) line += " " + hex32(m.load32(at + i * 4u));
    log_line(category::kHle, line);
}

std::u16string read_utf16(GuestMemory &m, std::uint32_t at, std::uint32_t max_chars) {
    std::u16string out;
    if (at == 0u) return out;
    for (std::uint32_t i = 0; i < max_chars && m.contains(at + i * 2u, 2u); ++i) {
        const std::uint16_t c = m.load16(at + i * 2u);
        if (!c) break;
        out.push_back(static_cast<char16_t>(c));
    }
    return out;
}

// SceUtilityOskParams (pspsdk): common(0x30) datacount@0x30 data*@0x34 state@0x38.
// SceUtilityOskData (pspsdk): desc@28 intext@32 outtextlength@36 outtext@40 result@44 outtextlimit@48.
// The dumped game data confirmed data*@0x34; the field order is detected from which word looks like a RAM pointer.
struct OskField {
    std::uint32_t field{}, out_text{}, result_at{}, limit{};
    bool sdk_order{true}, valid{false};
    std::u16string desc, intext;
};

OskField parse_field(GuestMemory &m, std::uint32_t index) {
    OskField f;
    const std::uint32_t params = g_osk.params;
    if (!m.contains(params, 0x40u)) return f;
    const std::uint32_t fields = m.load32(params + 0x34);
    f.field = fields + index * 52u;
    if (!m.contains(f.field, 52u)) return f;
    const auto looks_like_ram = [](std::uint32_t v) { return v >= 0x08000000u && v < 0x0A000000u; };
    f.sdk_order = looks_like_ram(m.load32(f.field + 40));
    f.out_text = m.load32(f.field + (f.sdk_order ? 40u : 44u));
    f.result_at = f.field + (f.sdk_order ? 44u : 48u);
    f.limit = m.load32(f.field + (f.sdk_order ? 48u : 40u));
    f.desc = read_utf16(m, m.load32(f.field + 28), 64u);
    f.intext = read_utf16(m, m.load32(f.field + 32), 64u);
    f.valid = true;
    return f;
}

void write_field(GuestMemory &m, const OskField &f, const std::u16string &text, bool cancelled) {
    const std::uint32_t limit = f.limit ? f.limit : 15u;
    const std::uint32_t chars = cancelled ? 0u : std::min<std::uint32_t>(static_cast<std::uint32_t>(text.size()), limit);
    if (f.out_text != 0u && m.contains(f.out_text, (limit + 1u) * 2u)) {
        for (std::uint32_t c = 0; c < chars; ++c) m.store16(f.out_text + c * 2u, static_cast<std::uint16_t>(text[c]));
        m.store16(f.out_text + chars * 2u, 0);
    }
    m.store32(f.field + 36, chars);                  // outtextlength
    m.store32(f.result_at, cancelled ? 1u : 2u);     // result: 1 cancelled, 2 changed
}

// `ui_text`/`cancelled` describe the user's answer for the first field; with no UI the suggested text (or a default
// name) is accepted.
void finish_osk(GuestMemory &m, const std::u16string *ui_text, bool cancelled) {
    const std::uint32_t params = g_osk.params;
    if (!m.contains(params, 0x40u)) return;
    const std::uint32_t count = std::min<std::uint32_t>(m.load32(params + 0x30), 8u);
    for (std::uint32_t i = 0; i < std::max(count, 1u); ++i) {
        const OskField f = parse_field(m, i);
        if (!f.valid) break;
        std::u16string text = (i == 0u && ui_text != nullptr) ? *ui_text : f.intext;
        if (text.empty() && !cancelled) text = u"RIDGE";
        write_field(m, f, text, cancelled && i == 0u);
        std::string shown;
        for (char16_t ch : text) shown.push_back(ch < 0x80u ? static_cast<char>(ch) : '?');
        log_line(category::kHle, "sceUtilityOsk: field " + std::to_string(i) + (cancelled && i == 0u ? " cancelled" : " accepted \"" + shown + "\"") +
                                     " (limit " + std::to_string(f.limit) + ", " + (f.sdk_order ? "sdk" : "alt") + " order)");
    }
    m.store32(params + 0x1C, 0u);           // common.result = success
}

void finish_msg(GuestMemory &m) {
    // pspMessageDialog: common(0x30) result@0x30 type@0x34 error@0x38 string@0x3C options@0x23C pressed@0x240
    const std::uint32_t params = g_msg.params;
    if (!m.contains(params, 0x244u)) return;
    std::string text;
    for (std::uint32_t i = 0; i < 511u && m.load8(params + 0x3C + i); ++i)
        text.push_back(static_cast<char>(m.load8(params + 0x3C + i)));
    log_line(category::kHle, "sceUtilityMsgDialog: type=" + std::to_string(m.load32(params + 0x34)) +
                                 " options=" + hex32(m.load32(params + 0x23C)) + " text=\"" + text + "\" -> Yes");
    m.store32(params + 0x240, 1u);   // button pressed: Yes
    m.store32(params + 0x1C, 0u);
}

} // namespace

void install_utility_hle(Runtime &runtime) {
    g_osk = Dialog{};
    g_msg = Dialog{};
    g_dump_rt_ptr = &runtime.memory();
    const auto bind = [&](std::uint32_t nid, Runtime::HleFunction handler) {
        runtime.register_hle("sceUtility", nid, std::move(handler));
    };
    const auto start = [](Dialog &d, AllegrexContext &c) {
        d = Dialog{kInitialize, c.gpr[4], 0u};
        if (&d == &g_osk && g_dump_rt_ptr) dump_words(*g_dump_rt_ptr, "OSKPARAMS start", c.gpr[4], 24u);
        log_line(category::kHle, std::string("sceUtility ") + (&d == &g_osk ? "Osk" : "MsgDialog") +
                                     "InitStart params=" + hex32(c.gpr[4]));
        if (&d == &g_osk && g_interactive && g_dump_rt_ptr) {
            const OskField f = parse_field(*g_dump_rt_ptr, 0u);
            if (f.valid) {
                std::vector<char16_t> stale;
                present_take_typed(stale);   // drop keys typed before the keyboard opened
                osk_ui_begin(f.desc, f.intext, f.limit);
            }
        }
        c.set_gpr(2, 0u);
    };
    const auto status = [](Dialog &d, AllegrexContext &c) {
        static std::uint32_t last_osk = 99u, last_msg = 99u;
        std::uint32_t &last = &d == &g_osk ? last_osk : last_msg;
        if (last != d.status) {
            last = d.status;
            log_line(category::kHle, std::string("sceUtility ") + (&d == &g_osk ? "Osk" : "MsgDialog") +
                                         "GetStatus -> " + std::to_string(d.status));
        }
        const std::uint32_t current = d.status;
        // A real PSP moves INITIALIZE -> RUNNING by itself; the game only
        // starts calling Update once it sees RUNNING.
        if (d.status == kInitialize) d.status = kRunning;
        c.set_gpr(2, current);
    };
    const auto shutdown = [](Dialog &d, AllegrexContext &c) {
        log_line(category::kHle, std::string("sceUtility ") + (&d == &g_osk ? "Osk" : "MsgDialog") +
                                     "ShutdownStart (status was " + std::to_string(d.status) + ", updates " +
                                     std::to_string(d.updates) + ")");
        d.status = kNone;
        if (&d == &g_osk) osk_ui_end();
        c.set_gpr(2, 0u);
    };
    const auto update = [](Dialog &d, GuestMemory &m, void (*finish)(GuestMemory &), AllegrexContext &c) {
        if (d.status == kInitialize) d.status = kRunning;
        else if (d.status == kRunning && &d == &g_osk && osk_ui_active()) {
            std::vector<char16_t> typed;
            present_take_typed(typed);
            const OskState st = osk_ui_update(typed, present_gamepad_buttons());
            if (const std::uint32_t ev = osk_ui_take_events(); ev != 0u) {
                if (ev & (kOskAccept | kOskInsert)) menu_sound_play(MenuSound::Confirm);
                else if (ev & (kOskCancel | kOskDelete)) menu_sound_play(MenuSound::Back);
                else menu_sound_play(MenuSound::Move);
            }
            if (st != OskState::Active) {
                const std::u16string text = osk_ui_text();
                finish_osk(m, &text, st == OskState::Cancelled);
                osk_ui_end();
                d.status = kFinished;
            }
        }
        else if (d.status == kRunning && ++d.updates >= kUpdatesToFinish) { finish(m); d.status = kFinished; }
        c.set_gpr(2, 0u);
    };

    bind(0xF6269B82u, [=](Runtime &, AllegrexContext &c) { start(g_osk, c); });                 // OskInitStart
    bind(0x3DFAEBA9u, [=](Runtime &, AllegrexContext &c) { shutdown(g_osk, c); });              // OskShutdownStart
    bind(0x4B85C861u, [=](Runtime &r, AllegrexContext &c) { update(g_osk, r.memory(), [](GuestMemory &mm) { finish_osk(mm, nullptr, false); }, c); });
    bind(0xF3F76017u, [=](Runtime &, AllegrexContext &c) { status(g_osk, c); });                // OskGetStatus

    bind(0x2AD8E239u, [=](Runtime &, AllegrexContext &c) { start(g_msg, c); });                 // MsgDialogInitStart
    bind(0x67AF3428u, [=](Runtime &, AllegrexContext &c) { shutdown(g_msg, c); });              // MsgDialogShutdownStart
    bind(0x95FC253Bu, [=](Runtime &r, AllegrexContext &c) { update(g_msg, r.memory(), finish_msg, c); });
    bind(0x9A1C91D7u, [=](Runtime &, AllegrexContext &c) { status(g_msg, c); });                // MsgDialogGetStatus
}

} // namespace rr

namespace rr {
void utility_set_interactive(bool on) { g_interactive = on; }
bool osk_active() { return osk_ui_active(); }
}
