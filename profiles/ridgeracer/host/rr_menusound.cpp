#include "rr_menusound.hpp"

#include "psprecomp/hle_sas.hpp"
#include "psprecomp/runtime.hpp"
#include "rr_audio.hpp"
#include "rr_log.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

namespace rr {
namespace {

constexpr int kMenuChannel = 7;                    // mixer channel the game does not use
constexpr float kGain = 1.5f;                      // SAS volume 0x1000 = unity; the game's menu voices sit around 0x420
constexpr std::uint32_t kPadUp = 0x10u, kPadRight = 0x20u, kPadDown = 0x40u, kPadLeft = 0x80u, kPadTriangle = 0x1000u,
                        kPadCircle = 0x2000u, kPadCross = 0x4000u;

struct Clip {
    std::uint32_t address{};
    std::int32_t size{};
    std::vector<std::int16_t> stereo;   // 44.1 kHz interleaved, padded with silence so the mixer's start threshold is reached
};

std::mutex g_mutex;
Clip g_clips[3];                  // the clip in use per sound (the most often heard candidate)
struct Candidate { std::uint32_t address; std::int32_t size; int count; Clip clip; };

// Sample sizes of the game's own menu sounds, found with RR_SE_LOG. Racing presses the same buttons (accelerate, steer) and
// those start engine / tyre sounds too, so a candidate with one of these sizes always beats any other.
// Returns 0 for "not a menu sound", otherwise a rank (higher wins regardless of how often each was heard: both cursor
// samples are used in different screens, and which one was heard most depends on how the player got there).
int menu_rank(int slot, std::int32_t size) {
    switch (slot) {
    case 0: return size == 12560 ? 2 : (size == 4320 ? 1 : 0);   // cursor move
    case 1: return size == 10464 ? 1 : 0;                        // confirm
    case 2: return size == 8304 ? 1 : 0;                         // back
    default: return 0;
    }
}
bool is_menu_size(int slot, std::int32_t size) { return menu_rank(slot, size) > 0; }
std::vector<Candidate> g_candidates[3];
std::uint64_t g_press_frame = ~0ull;
std::uint32_t g_press_bits = 0u;
bool g_press_claimed = true;

const std::int32_t kFilter0[16] = {0, 60, 115, 98, 122, 0, 0, 52, 55, 60, 0, 0, 0, 2, 125, 0};
const std::int32_t kFilter1[16] = {0, 0, -52, -55, -60, 0, 0, 0, -2, -125, 0, -91, 0, -216, -6, -151};

// PSX ADPCM (VAG): 16-byte blocks of 28 samples; stops at an end flag.
std::vector<std::int16_t> decode_vag(const psprecomp::GuestMemory &memory, std::uint32_t address, std::int32_t size) {
    std::vector<std::int16_t> pcm;
    std::int32_t h1 = 0, h2 = 0;
    for (std::int32_t off = 0; off + 16 <= size && pcm.size() < 200000u; off += 16) {
        const std::uint32_t base = address + static_cast<std::uint32_t>(off);
        if (!memory.contains(base, 16u)) break;
        const std::uint32_t header = memory.aot_load8(base), flags = memory.aot_load8(base + 1u);
        if (flags == 7u) break;
        const std::int32_t shift = static_cast<std::int32_t>(header & 0x0Fu), filter = static_cast<std::int32_t>((header >> 4u) & 0x0Fu);
        for (std::uint32_t i = 0; i < 28u; ++i) {
            const std::uint32_t byte = memory.aot_load8(base + 2u + i / 2u);
            const std::uint32_t nibble = (i & 1u) != 0u ? (byte >> 4u) : (byte & 0x0Fu);
            std::int32_t s = static_cast<std::int32_t>(nibble << 12u);
            if (s & 0x8000) s = static_cast<std::int32_t>(s | 0xFFFF0000u);
            s >>= shift;
            s += (h1 * kFilter0[filter] + h2 * kFilter1[filter] + 32) / 64;
            s = std::clamp(s, -32768, 32767);
            h2 = h1; h1 = s;
            pcm.push_back(static_cast<std::int16_t>(s));
        }
        if (flags == 1u || flags == 3u) break;   // end of sample (3 = loop end)
    }
    return pcm;
}

void finish_clip(Clip &clip, const std::vector<std::int16_t> &mono, int pitch, int vol_l, int vol_r) {
    const double step = std::max(0.05, static_cast<double>(pitch) / 4096.0);   // source samples per 44.1 kHz output frame
    const std::size_t frames = static_cast<std::size_t>(static_cast<double>(mono.size()) / step);
    const float gl = std::min(1.0f, kGain * static_cast<float>(vol_l) / 4096.0f), gr = std::min(1.0f, kGain * static_cast<float>(vol_r) / 4096.0f);
    const std::size_t padded = std::max<std::size_t>(frames + 256u, 9000u);   // silence tail: the mixer needs a few thousand frames queued before it sounds
    clip.stereo.assign(padded * 2u, 0);
    for (std::size_t i = 0; i < frames; ++i) {
        const double pos = static_cast<double>(i) * step;
        const std::size_t i0 = static_cast<std::size_t>(pos);
        const float f = static_cast<float>(pos - static_cast<double>(i0));
        const float a = mono[i0], b = i0 + 1u < mono.size() ? mono[i0 + 1u] : 0.0f;
        const float v = a + (b - a) * f;
        clip.stereo[i * 2u] = static_cast<std::int16_t>(std::clamp(v * gl, -32768.0f, 32767.0f));
        clip.stereo[i * 2u + 1u] = static_cast<std::int16_t>(std::clamp(v * gr, -32768.0f, 32767.0f));
    }
}

// Own click, used until the game has played the real thing.
void synth_clip(Clip &clip, MenuSound which) {
    const float freq = which == MenuSound::Move ? 1500.0f : (which == MenuSound::Confirm ? 1100.0f : 750.0f);
    const std::size_t n = which == MenuSound::Move ? 1500u : 3200u;
    clip.stereo.assign(9000u * 2u, 0);
    for (std::size_t i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / 44100.0f, env = std::exp(-t * (which == MenuSound::Move ? 90.0f : 45.0f));
        const float v = std::sin(6.2831853f * freq * t) * env * 9000.0f * (i < 40u ? static_cast<float>(i) / 40.0f : 1.0f);
        clip.stereo[i * 2u] = clip.stereo[i * 2u + 1u] = static_cast<std::int16_t>(v);
    }
}

} // namespace

void menu_sound_note_press(std::uint32_t pressed, std::uint64_t frame) {
    if (pressed == 0u) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_press_frame = frame;
    g_press_bits = pressed;
    g_press_claimed = false;
}

void menu_sound_on_keyon(std::uint64_t frame, const psprecomp::GuestMemory &memory, const psprecomp::SasVoiceState &voice) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_press_claimed || frame < g_press_frame || frame - g_press_frame > 5u) return;
    if (voice.data_size <= 0 || voice.data_size > 40000 || voice.left_volume <= 0) return;   // short effects only (not music / ambience)
    int slot = -1;
    if (g_press_bits & kPadCross) slot = static_cast<int>(MenuSound::Confirm);
    else if (g_press_bits & (kPadCircle | kPadTriangle)) slot = static_cast<int>(MenuSound::Back);
    else if (g_press_bits & (kPadUp | kPadDown | kPadLeft | kPadRight)) slot = static_cast<int>(MenuSound::Move);
    if (slot < 0) return;
    g_press_claimed = true;   // the first short effect after a press is the one that press makes
    // The same press can start different samples in different screens; the one heard most often is used.
    auto &cands = g_candidates[slot];
    Candidate *found = nullptr;
    for (Candidate &c : cands) if (c.address == voice.data_address && c.size == voice.data_size) found = &c;
    if (found == nullptr) {
        const std::vector<std::int16_t> mono = decode_vag(memory, voice.data_address, voice.data_size);
        if (mono.size() < 200u) return;
        if (cands.size() >= 6u) {   // full of look-alikes: make room for a real menu sound
            if (!is_menu_size(slot, voice.data_size)) return;
            cands.erase(std::min_element(cands.begin(), cands.end(), [](const Candidate &a, const Candidate &b) { return a.count < b.count; }));
        }
        cands.push_back(Candidate{voice.data_address, voice.data_size, 0, Clip{}});
        finish_clip(cands.back().clip, mono, voice.pitch, voice.left_volume, voice.right_volume);
        found = &cands.back();
        log_line(category::kHle, "menu sound " + std::to_string(slot) + " (0=move 1=confirm 2=back): candidate 0x" + [&] { char b[16]; std::snprintf(b, sizeof b, "%08X", voice.data_address); return std::string(b); }() + ", " + std::to_string(mono.size()) + " samples");
    }
    ++found->count;
    const auto score = [&](const Candidate &c) { return c.count + menu_rank(slot, c.size) * 100000; };
    const Candidate *best = &cands.front();
    for (const Candidate &c : cands) if (score(c) >= score(*best)) best = &c;   // ties go to the latest
    if (g_clips[slot].address != best->address || g_clips[slot].size != best->size) {
        g_clips[slot] = best->clip;
        g_clips[slot].address = best->address;
        g_clips[slot].size = best->size;
    }
}

void menu_sound_play(MenuSound sound) {
    static const bool started = audio_start();
    if (!started) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    Clip &clip = g_clips[static_cast<int>(sound)];
    if (clip.stereo.empty()) synth_clip(clip, sound);
    audio_channel_set_trim(kMenuChannel, false);
    audio_channel_clear(kMenuChannel);
    audio_channel_push(kMenuChannel, clip.stereo.data(), static_cast<std::uint32_t>(clip.stereo.size() / 2u), true, 0x8000, 0x8000);
}

} // namespace rr
