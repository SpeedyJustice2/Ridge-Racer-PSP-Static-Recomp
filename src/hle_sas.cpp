#include <array>
#include <cmath>
#include <cstdio>
#include "psprecomp/hle_sas.hpp"

#include "psprecomp/common.hpp"
#include "psprecomp/guest_memory.hpp"
#include "psprecomp/runtime.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace psprecomp {
namespace {

void set_success(AllegrexContext &ctx) { ctx.set_gpr(2, 0u); }

// ---------------------------------------------------------------------------
// Statistics for the [SAS SUMMARY] block.
// ---------------------------------------------------------------------------
std::uint64_t g_sas_contexts{};
std::uint64_t g_sas_core_calls{};
std::uint64_t g_sas_core_with_mix_calls{};
std::uint64_t g_sas_key_on_calls{};
psprecomp::SasKeyOnObserver g_sas_keyon_observer = nullptr;
psprecomp::SasEventObserver g_sas_event_observer = nullptr;
std::uint64_t g_sas_key_off_calls{};
std::uint64_t g_sas_buffers_generated{};
std::uint64_t g_sas_samples_generated{};
std::uint32_t g_sas_voices_used{};

bool sas_trace_enabled() {
    static const bool enabled = std::getenv("PSPRECOMP_AUDIO_TRACE") != nullptr;
    return enabled;
}

// Per-symbol cap keeps a runaway guest mix loop from growing the log without
// bound while still showing the first calls of every API.
constexpr std::uint64_t kSasTraceLimit = 64u;

bool sas_trace_suppressed(const char *symbol) {
    static std::unordered_map<std::string, std::uint64_t> counts;
    auto &count = counts[symbol];
    if (++count <= kSasTraceLimit) return false;
    if (count == kSasTraceLimit + 1u) {
        std::cerr << "[AUDIO] " << symbol << " trace suppressed after " << kSasTraceLimit
                  << " entries\n";
    }
    return true;
}

std::uint32_t sas_nid_for_symbol(const char *symbol) {
    const std::string_view name(symbol);
    if (name == "sceSasInit") return 0x42778A9Fu;
    if (name == "sceSasSetVoice") return 0x99944089u;
    if (name == "sceSasSetVoicePCM") return 0xE1CD9561u;
    if (name == "sceSasSetPitch") return 0xAD84D37Fu;
    if (name == "sceSasSetVolume") return 0x440CA7D8u;
    if (name == "sceSasSetKeyOn") return 0x76F01ACAu;
    if (name == "sceSasSetKeyOff") return 0xA0CF2FA4u;
    if (name == "sceSasCore") return 0xA3589D81u;
    if (name == "sceSasCoreWithMix") return 0x50A14DFCu;
    if (name == "sceSasGetOutputmode") return 0xE175EF66u;
    if (name == "sceSasSetOutputmode") return 0xE855BF76u;
    if (name == "sceSasSetGrain") return 0xD1E0A01Eu;
    return 0u;
}

void sas_trace(const char *symbol, const AllegrexContext &ctx, std::uint32_t result,
               const std::string &detail = {}) {
    if (!sas_trace_enabled() || sas_trace_suppressed(symbol)) return;
    std::ostringstream out;
    out << "[AUDIO] thread=" << runtime_thread_uid()
        << " name=" << runtime_thread_name()
        << " lib=sceSasCore nid=" << hex32(sas_nid_for_symbol(symbol))
        << " symbol=" << symbol
        << " ra=" << hex32(ctx.gpr[31])
        << " a0=" << hex32(ctx.gpr[4]) << " a1=" << hex32(ctx.gpr[5])
        << " a2=" << hex32(ctx.gpr[6]) << " a3=" << hex32(ctx.gpr[7]);
    if (!detail.empty()) out << " " << detail;
    out << " result=" << hex32(result);
    std::cerr << out.str() << "\n";
}

std::string sas_voice_detail(std::int32_t voice) {
    return "voice=" + std::to_string(voice);
}

void sas_activate_voice(std::int32_t voice);
void sas_touch_voice(std::int32_t voice);   // defined once the SAS state exists

void sas_note_voice(std::int32_t voice) {
    if (voice >= 0 && voice < 32) g_sas_voices_used |= 1u << static_cast<unsigned>(voice);
    sas_touch_voice(voice);
}

// ---------------------------------------------------------------------------
// State and helpers (moved from the VCS profile; game-neutral).
// ---------------------------------------------------------------------------
SasState sas_state{};

// ---- "Stop everything" heuristic --------------------------------------------------------------------------------
// When the game pauses or leaves a race it key-offs its voices in one burst, but a few engine voices it started
// recently are missing from that list and would keep playing at their last volume (they stay audible on the pause
// screen).  A burst of key-offs therefore marks every voice that is still on and has not been touched since as
// suppressed (still decoded, not mixed); any later call that configures or starts the voice un-suppresses it, so
// persistent voices come back as soon as the game uses them again.  RR_SAS_STOPALL_OFF=1 disables this.
std::deque<std::pair<std::uint64_t, bool>> g_keyoff_times;   // (time, voice had a big ring-buffer sample = the music)
bool g_stopall_pending = false;
std::uint64_t g_stopall_trigger = 0, g_stopall_apply_at = 0;

void sas_touch_voice(std::int32_t voice) {
    if (voice < 0 || static_cast<std::size_t>(voice) >= sas_state.voices.size()) return;
    auto &v = sas_state.voices[static_cast<std::size_t>(voice)];
    v.touch_time = g_sas_samples_generated;   // any call (pitch updates included); diagnostics only
}

// A voice that stopped on its own (not by the game's key-off): code 1 = envelope reached zero in release,
// 2 = sample data ran out, 3 = last sample consumed.  Diagnostics only (RR_AUDIOLOG).
void sas_note_ended(const SasVoiceState &voice, int code) {
    if (g_sas_event_observer == nullptr) return;
    const auto index = static_cast<int>(&voice - sas_state.voices.data());
    g_sas_event_observer("ended", index, code);
}

// Key-on or a changed volume: the game really wants this voice to sound, so it counts as activity.
void sas_activate_voice(std::int32_t voice) {
    if (voice < 0 || static_cast<std::size_t>(voice) >= sas_state.voices.size()) return;
    auto &v = sas_state.voices[static_cast<std::size_t>(voice)];
    v.activity_time = g_sas_samples_generated;
    v.suppressed = false;
}

void sas_note_key_off_call(bool big_voice) {
    static const bool disabled = std::getenv("RR_SAS_STOPALL_OFF") != nullptr;
    if (disabled) return;
    constexpr std::uint64_t kWindow = 4410u;   // 100 ms
    constexpr std::size_t kBurst = 8u;
    constexpr std::size_t kHugeBurst = 16u;
    const std::uint64_t now = g_sas_samples_generated;
    g_keyoff_times.emplace_back(now, big_voice);
    while (!g_keyoff_times.empty() && g_keyoff_times.front().first + kWindow < now) g_keyoff_times.pop_front();
    // A mid-race pile-up of 6-9 effect key-offs is not a "stop everything": the pause / race exit burst always includes the
    // music (a 49152-byte voice) or is huge.  Without that test the burst silenced distant opponents' engines and ambient loops
    // that the game never touches again.  RR_SAS_STOPALL_ANY=1 restores the old test (any burst of 8).
    static const bool any_burst = std::getenv("RR_SAS_STOPALL_ANY") != nullptr;
    const bool has_big = std::any_of(g_keyoff_times.begin(), g_keyoff_times.end(), [](const auto &e) { return e.second; });
    if (g_keyoff_times.size() >= kBurst && (any_burst || has_big || g_keyoff_times.size() >= kHugeBurst) && !g_stopall_pending) {
        g_stopall_pending = true;
        g_stopall_trigger = now;
        g_stopall_apply_at = now + kWindow;
        g_keyoff_times.clear();
        if (g_sas_event_observer != nullptr) g_sas_event_observer("stopall_trigger", -1, 0);
    }
}

}   // namespace
void sas_request_stop_all() {
    // The host knows the game is being paused (Start pressed during a race) even when the game's key-off burst was too small
    // for the heuristic (5 key-offs left 8 engine / ambient voices playing on the pause screen).
    static const bool disabled = std::getenv("RR_SAS_STOPALL_OFF") != nullptr;
    if (disabled || g_stopall_pending) return;
    g_stopall_pending = true;
    g_stopall_trigger = g_sas_samples_generated;
    g_stopall_apply_at = g_sas_samples_generated + 4410u;
    g_keyoff_times.clear();
    if (g_sas_event_observer != nullptr) g_sas_event_observer("stopall_trigger", -2, 0);
}
void sas_unmute_all() {
    // The game resumes after a pause: everything the pause mute (or a key-off burst) suppressed plays again.  A voice the game never
    // retouches (an ambient loop, an opponent's engine at constant volume) would otherwise stay silent for the rest of the race.
    g_stopall_pending = false;
    for (auto &v : sas_state.voices) v.suppressed = false;
    if (g_sas_event_observer != nullptr) g_sas_event_observer("unmute_all", -2, 0);
}
namespace {
void sas_apply_stop_all() {
    if (!g_stopall_pending || g_sas_samples_generated < g_stopall_apply_at) return;
    g_stopall_pending = false;
    for (std::size_t i = 0; i < sas_state.voices.size(); ++i) {
        auto &v = sas_state.voices[i];
        // The race music is a pair of big looping ring-buffer voices (49152 bytes each) that the game never retouches after
        // the key-on; a burst of key-offs in the middle of a race (several effects ending together) used to silence them.
        // Voices with that much data are never suppressed.  RR_SAS_STOPALL_EXEMPT=<bytes> changes the limit (0 = no exemption).
        static const std::int32_t exempt = [] {
            const char *e = std::getenv("RR_SAS_STOPALL_EXEMPT");
            return e ? static_cast<std::int32_t>(std::strtol(e, nullptr, 0)) : 49152;
        }();
        if (exempt > 0 && v.on && v.playing && v.data_size >= exempt) {
            if (g_sas_event_observer != nullptr) g_sas_event_observer("stopall_exempt", static_cast<int>(i), v.data_size);
        } else if (v.on && v.playing && v.activity_time < g_stopall_trigger) {
            v.suppressed = true;
            if (g_sas_event_observer != nullptr) g_sas_event_observer("stopall_suppress", static_cast<int>(i), 0);
        } else if (v.on && v.playing && g_sas_event_observer != nullptr) {
            // touched after the trigger: report how long after (in samples) so the heuristic can be tuned
            g_sas_event_observer("stopall_keep_touched", static_cast<int>(i),
                static_cast<int>(v.activity_time - g_stopall_trigger));
        }
    }
}
std::uint64_t sas_core_mix_calls{};
std::uint64_t sas_core_with_mix_calls{};

bool sas_audio_diagnostics_enabled() {
    static const bool enabled = std::getenv("PSPRECOMP_AUDIO_DIAG") != nullptr ||
        std::getenv("PSPRECOMP_SAS_DIAG") != nullptr;
    return enabled;
}

std::size_t sas_playing_voice_count() {
    return static_cast<std::size_t>(std::count_if(
        sas_state.voices.begin(), sas_state.voices.end(),
        [](const SasVoiceState &voice) { return voice.playing && !voice.paused; }));
}

void sas_log_mix_checkpoint(const char *kind, std::uint64_t count) {
    if (!sas_audio_diagnostics_enabled()) return;
    if (count <= 8u || (count % 256u) == 0u) {
        std::cerr << "[sas] " << kind << " call=" << count
                  << " voices=" << sas_playing_voice_count()
                  << " dry=" << sas_state.reverb.dry
                  << " wet=" << sas_state.reverb.wet
                  << " effect_type=" << sas_state.reverb.type
                  << " grain=" << sas_state.grain_size << "\n";
    }
}

bool sas_valid_core(std::uint32_t core) noexcept {
    return sas_state.initialized && core == sas_state.core_address;
}

std::uint32_t g_sas_last_ra = 0u;   // return address of the guest code that made the latest per-voice SAS call (diagnostics)

SasVoiceState *sas_voice(std::uint32_t core, std::int32_t voice, psprecomp::AllegrexContext &ctx) {
    g_sas_last_ra = ctx.gpr[31];
    if (!sas_valid_core(core)) {
        ctx.set_gpr(2, kSasErrorNotInitialized);
        return nullptr;
    }
    if (voice < 0 || voice >= 32) {
        ctx.set_gpr(2, kSasErrorInvalidVoice);
        return nullptr;
    }
    return &sas_state.voices[static_cast<std::size_t>(voice)];
}

void sas_reset_voice_duration(SasVoiceState &voice) noexcept {
    if (voice.type == SasVoiceType::Vag && voice.data_size > 0) {
        // PSP VAG/ADPCM uses 16-byte blocks producing 28 PCM samples.  This is
        // an upper bound for streams with an earlier end marker, but it remains
        // useful for diagnostics and never drives end-of-voice by itself.
        voice.total_samples = static_cast<std::uint64_t>(voice.data_size / 16) * 28u;
        voice.remaining_samples = voice.total_samples;
    } else if (voice.type == SasVoiceType::Pcm && voice.data_size > 0) {
        // For PCM voices the size parameter is already a sample count.
        voice.total_samples = static_cast<std::uint64_t>(voice.data_size);
        voice.remaining_samples = voice.total_samples;
    } else {
        voice.total_samples = 0u;
        voice.remaining_samples = 0u;
    }
}

void sas_reset_decoder(SasVoiceState &voice) noexcept {
    voice.decode_offset = 0u;
    voice.history1 = 0;
    voice.history2 = 0;
    voice.block_position = 28u;
    voice.loop_start_offset = 0u;
    voice.loop_start_history1 = 0;
    voice.loop_start_history2 = 0;
    voice.loop_start_valid = false;
    voice.finished = false;
    voice.current_sample = 0;
    voice.next_sample = 0;
    voice.current_sample_valid = false;
    voice.next_sample_valid = false;
    voice.pitch_accumulator = 0u;
    voice.noise_lfsr = 0x13579BDFu;
    voice.noise_phase = 0u;
    voice.noise_sample = 0;
    voice.pcm_cursor = 0u;
    sas_reset_voice_duration(voice);
}

// When a title never configures ADSR, use a short de-click ramp rather than
// leaving the envelope at zero forever.  Once VCS supplies real SAS envelope
// parameters we follow those curves instead.
constexpr std::uint32_t kSasFallbackAttackSamples = 8u;
constexpr std::uint32_t kSasFallbackReleaseSamples = 32u;
constexpr std::uint32_t kSasFallbackAttackStep =
    kSasEnvelopeMaximum / kSasFallbackAttackSamples;
constexpr std::uint32_t kSasFallbackReleaseStep =
    kSasEnvelopeMaximum / kSasFallbackReleaseSamples;

std::int64_t sas_walk_envelope_curve(std::int64_t height, std::int32_t mode,
                                     std::int32_t rate) noexcept {
    const std::int64_t r = std::max<std::int64_t>(0, rate);
    switch (mode) {
    case 0: // linear increase
        return height + r;
    case 1: // linear decrease
        return height - r;
    case 2: // linear bent increase
        return height + (height <= static_cast<std::int64_t>(kSasEnvelopeMaximum) * 3 / 4
            ? r : r / 4);
    case 3: { // exponential decrease
        std::int64_t delta = height - static_cast<std::int64_t>(kSasEnvelopeMaximum);
        delta += ((-delta) * r) >> 32;
        return delta + kSasEnvelopeMaximum - (r + 3) / 4;
    }
    case 4: { // exponential increase
        std::int64_t delta = height - static_cast<std::int64_t>(kSasEnvelopeMaximum);
        delta += ((-delta) * r) >> 32;
        return delta + kSasEnvelopeMaximum + 0x4000;
    }
    case 5: // direct
        return r;
    default:
        return height;
    }
}

std::uint32_t sas_step_envelope_impl(SasVoiceState &voice) noexcept {
    if (!voice.playing) return 0u;

    if (!voice.adsr_configured) {
        if (voice.on) {
            if (voice.envelope_height < kSasEnvelopeMaximum) {
                voice.envelope_height = std::min<std::uint32_t>(
                    kSasEnvelopeMaximum, voice.envelope_height + kSasFallbackAttackStep);
            }
        } else if (voice.envelope_height <= kSasFallbackReleaseStep) {
            voice.envelope_height = 0u;
            voice.envelope_phase = SasEnvelopePhase::Off;
            voice.playing = false;
            sas_note_ended(voice, 1);
        } else {
            voice.envelope_height -= kSasFallbackReleaseStep;
        }
        return voice.envelope_height;
    }

    // Real PSP SAS holds the envelope at zero briefly after KeyOn.  Keeping the
    // delay in samples (rather than grains) avoids a sharp transient and makes
    // the same voice deterministic at every grain size.
    if (voice.key_on_delay_samples != 0u) {
        --voice.key_on_delay_samples;
        voice.envelope_height = 0u;
        return 0u;
    }

    if (!voice.on && voice.envelope_phase != SasEnvelopePhase::Off)
        voice.envelope_phase = SasEnvelopePhase::Release;

    std::int64_t height = voice.envelope_height;
    switch (voice.envelope_phase) {
    case SasEnvelopePhase::Attack:
        height = sas_walk_envelope_curve(height, voice.adsr_modes[0], voice.adsr_rates[0]);
        if (height >= static_cast<std::int64_t>(kSasEnvelopeMaximum) || height < 0) {
            height = kSasEnvelopeMaximum;
            voice.envelope_phase = SasEnvelopePhase::Decay;
        }
        break;
    case SasEnvelopePhase::Decay:
        height = sas_walk_envelope_curve(height, voice.adsr_modes[1], voice.adsr_rates[1]);
        if (height <= voice.sustain_level) {
            height = std::max<std::int64_t>(0, voice.sustain_level);
            voice.envelope_phase = SasEnvelopePhase::Sustain;
        }
        break;
    case SasEnvelopePhase::Sustain:
        height = sas_walk_envelope_curve(height, voice.adsr_modes[2], voice.adsr_rates[2]);
        if (height <= 0) {
            height = 0;
            voice.envelope_phase = SasEnvelopePhase::Release;
        } else if (height > static_cast<std::int64_t>(kSasEnvelopeMaximum)) {
            height = kSasEnvelopeMaximum;
        }
        break;
    case SasEnvelopePhase::Release:
        height = sas_walk_envelope_curve(height, voice.adsr_modes[3], voice.adsr_rates[3]);
        if (height <= 0) {
            height = 0;
            voice.envelope_phase = SasEnvelopePhase::Off;
            voice.playing = false;
            sas_note_ended(voice, 1);
        }
        break;
    case SasEnvelopePhase::Off:
        height = 0;
        voice.playing = false;
        break;
    }

    height = std::clamp<std::int64_t>(height, 0, kSasEnvelopeMaximum);
    voice.envelope_height = static_cast<std::uint32_t>(height);
    return voice.envelope_height;
}

std::int32_t sas_simple_rate(std::uint32_t value) noexcept {
    value &= 0x7Fu;
    if (value == 0x7Fu) return 0;
    const std::uint64_t base = static_cast<std::uint64_t>(7u - (value & 3u)) << 26u;
    const std::uint64_t rate = base >> (value >> 2u);
    return static_cast<std::int32_t>(std::max<std::uint64_t>(1u, rate));
}

std::int32_t sas_exponent_rate(std::uint32_t value) noexcept {
    value &= 0x7Fu;
    if (value == 0x7Fu) return 0;
    const std::uint64_t base = static_cast<std::uint64_t>(7u - (value & 3u)) << 24u;
    const std::uint64_t rate = base >> (value >> 2u);
    return static_cast<std::int32_t>(std::max<std::uint64_t>(1u, rate));
}

void sas_decode_simple_adsr(SasVoiceState &voice) noexcept {
    const std::uint32_t a1 = voice.simple_adsr1;
    const std::uint32_t a2 = voice.simple_adsr2;
    voice.adsr_rates[0] = sas_simple_rate(a1 >> 8u);
    voice.adsr_modes[0] = (a1 & 0x8000u) == 0u ? 0 : 2;

    const std::uint32_t decay = (a1 >> 4u) & 0x0Fu;
    voice.adsr_rates[1] = decay == 0u ? 0x7FFFFFFF :
        static_cast<std::int32_t>(0x80000000u >> decay);
    voice.adsr_modes[1] = 3;

    voice.adsr_modes[2] = static_cast<std::int32_t>((a2 >> 14u) & 3u);
    voice.adsr_rates[2] = voice.adsr_modes[2] == 3
        ? sas_exponent_rate(a2 >> 6u) : sas_simple_rate(a2 >> 6u);

    const std::uint32_t release = a2 & 0x1Fu;
    voice.adsr_modes[3] = (a2 & 0x20u) == 0u ? 1 : 3;
    if (release == 31u) {
        voice.adsr_rates[3] = 0;
    } else if (voice.adsr_modes[3] == 1) {
        if (release == 30u) voice.adsr_rates[3] = 0x40000000;
        else if (release == 29u) voice.adsr_rates[3] = 1;
        else voice.adsr_rates[3] = static_cast<std::int32_t>(0x10000000u >> release);
    } else {
        voice.adsr_rates[3] = release == 0u ? 0x7FFFFFFF :
            static_cast<std::int32_t>(0x80000000u >> release);
    }

    voice.sustain_level = static_cast<std::int32_t>(((a1 & 0x0Fu) + 1u) << 26u);
    voice.adsr_configured = true;
}

// PSP VAG is the PS1 ADPCM format: 16-byte blocks holding 28 four-bit samples
// plus a shift/filter byte and a flags byte. The predictor coefficients are the
// standard table, scaled by 64.
constexpr std::int32_t kVagFilter0[16] = {
    0, 60, 115, 98, 122, 0, 0, 52, 55, 60, 0, 0, 0, 2, 125, 0
};
constexpr std::int32_t kVagFilter1[16] = {
    0, 0, -52, -55, -60, 0, 0, 0, -2, -125, 0, -91, 0, -216, -6, -151
};

// Decodes the next 16-byte block into voice.block_samples.  Loop markers follow
// the PSP SAS convention used by VCS: 6 marks the loop start, 3 the loop end,
// and 7 the terminal block.  Marker 1 is also accepted as a terminal block for
// ordinary PSX-style VAG assets.
bool sas_decode_next_block(const psprecomp::GuestMemory &memory, SasVoiceState &voice) {
    if (voice.data_address == 0u || voice.data_size <= 0 || voice.finished) return false;

    const auto rewind_loop = [&]() {
        // PSP SAS keeps the ADPCM predictor history across a loop jump.
        // Resetting it at the marker makes otherwise seamless ambient loops
        // click every time they wrap.
        voice.decode_offset = voice.loop_start_valid ? voice.loop_start_offset : 0u;
        voice.remaining_samples = voice.total_samples;
    };

    if (voice.decode_offset + 16u > static_cast<std::uint32_t>(voice.data_size)) {
        // Reaching the end of the data without a loop-end block (flag 3) ends the voice, loop mode or not; the game
        // starts many one-shot sounds with the loop argument set and never stops them (PPSSPP ends them too).
        // RR_SAS_WRAP_END=1 restores the old behaviour of wrapping to the start.
        static const bool legacy_wrap = std::getenv("RR_SAS_WRAP_END") != nullptr;
        if (g_sas_event_observer != nullptr && voice.loop) {
            static std::unordered_set<std::uint32_t> seen;
            if (seen.insert(voice.data_address).second)
                g_sas_event_observer(legacy_wrap ? "end_of_data_WRAPPED" : "end_of_data_ENDED", -1, static_cast<int>(voice.data_address));
        }
        if (!voice.loop || !legacy_wrap) { voice.finished = true; return false; }
        rewind_loop();
    }
    const std::uint32_t relative_offset = voice.decode_offset;
    const std::uint32_t base = voice.data_address + relative_offset;
    if (!memory.contains(base, 16u)) return false;

    const std::uint32_t header = memory.aot_load8(base);
    const std::uint32_t flags = memory.aot_load8(base + 1u);
    const std::int32_t history_before_1 = voice.history1;
    const std::int32_t history_before_2 = voice.history2;
    std::int32_t shift = static_cast<std::int32_t>(header & 0x0Fu);
    std::int32_t filter = static_cast<std::int32_t>((header >> 4u) & 0x0Fu);
    // All four predictor bits and all four shift bits are meaningful on the
    // PSP SAS decoder.
    filter &= 0x0F;
    shift &= 0x0F;

    // Flag 7 is a terminal marker block; it does not contribute 28 samples.
    if (flags == 7u) {
        voice.finished = true;
        return false;
    }

    for (std::uint32_t index = 0u; index < 28u; ++index) {
        const std::uint32_t byte = memory.aot_load8(base + 2u + index / 2u);
        const std::uint32_t nibble = (index & 1u) != 0u ? (byte >> 4u) : (byte & 0x0Fu);
        std::int32_t sample = static_cast<std::int32_t>(nibble << 12u);
        if (sample & 0x8000) sample = static_cast<std::int32_t>(sample | 0xFFFF0000u);
        sample >>= shift;
        sample += (voice.history1 * kVagFilter0[filter] +
                   voice.history2 * kVagFilter1[filter] + 32) / 64;
        sample = std::clamp(sample, -32768, 32767);
        voice.block_samples[index] = static_cast<std::int16_t>(sample);
        voice.history2 = voice.history1;
        voice.history1 = sample;
    }

    if (flags == 6u) {
        // Re-entering the loop must restore the predictor state from immediately
        // before the loop-start block, otherwise each pass drifts/clicks.
        voice.loop_start_offset = relative_offset;
        voice.loop_start_history1 = history_before_1;
        voice.loop_start_history2 = history_before_2;
        voice.loop_start_valid = true;
    }

    voice.decode_offset += 16u;
    voice.block_position = 0u;

    if (flags == 3u) {
        if (voice.loop) rewind_loop();
        else voice.finished = true;
    } else if (flags == 1u) {
        voice.finished = true;
    }
    return true;
}

bool sas_fetch_vag_sample(const psprecomp::GuestMemory &memory, SasVoiceState &voice,
                          std::int16_t &sample) {
    if (voice.block_position >= 28u) {
        if (voice.finished || !sas_decode_next_block(memory, voice)) return false;
    }
    sample = voice.block_samples[voice.block_position++];
    if (voice.remaining_samples != 0u) --voice.remaining_samples;
    return true;
}

// Raw mono 16-bit signed little-endian PCM (sceSasSetVoicePCM).  `data_size`
// is the sample count; `pcm_loop_position` the sample index to wrap to.
bool sas_fetch_pcm_sample(const psprecomp::GuestMemory &memory, SasVoiceState &voice,
                          std::int16_t &sample) {
    const std::uint32_t sample_count = static_cast<std::uint32_t>(voice.data_size);
    if (voice.data_address == 0u || sample_count == 0u || voice.finished) return false;
    if (voice.pcm_cursor >= sample_count) {
        if (!voice.loop) {
            voice.finished = true;
            return false;
        }
        voice.pcm_cursor = voice.pcm_loop_position < sample_count ? voice.pcm_loop_position : 0u;
    }
    const std::uint32_t address = voice.data_address + voice.pcm_cursor * 2u;
    if (!memory.contains(address, 2u)) return false;
    sample = static_cast<std::int16_t>(memory.aot_load16(address));
    ++voice.pcm_cursor;
    if (voice.remaining_samples != 0u) --voice.remaining_samples;
    return true;
}

bool sas_fetch_voice_sample(const psprecomp::GuestMemory &memory, SasVoiceState &voice,
                            std::int16_t &sample) {
    if (voice.type == SasVoiceType::Pcm) return sas_fetch_pcm_sample(memory, voice, sample);
    return sas_fetch_vag_sample(memory, voice, sample);
}

bool sas_prepare_sample_pair(const psprecomp::GuestMemory &memory, SasVoiceState &voice) {
    if (!voice.current_sample_valid) {
        if (!sas_fetch_voice_sample(memory, voice, voice.current_sample)) return false;
        voice.current_sample_valid = true;
    }
    if (!voice.next_sample_valid) {
        std::int16_t next{};
        if (sas_fetch_voice_sample(memory, voice, next)) {
            voice.next_sample = next;
            voice.next_sample_valid = true;
        }
    }
    return true;
}

std::int32_t sas_render_stream_sample(const psprecomp::GuestMemory &memory, SasVoiceState &voice) {
    if (!sas_prepare_sample_pair(memory, voice)) {
        voice.playing = false;
        voice.on = false;
        sas_note_ended(voice, 2);
        voice.envelope_height = 0u;
        return 0;
    }

    const std::int32_t current = voice.current_sample;
    const std::int32_t next = voice.next_sample_valid ? voice.next_sample : current;
    const std::int32_t sample = current +
        ((next - current) * static_cast<std::int32_t>(voice.pitch_accumulator)) / 0x1000;

    const std::uint32_t pitch = voice.pitch < 0 ? 0u : static_cast<std::uint32_t>(voice.pitch);
    voice.pitch_accumulator += pitch;
    while (voice.pitch_accumulator >= 0x1000u && voice.playing) {
        voice.pitch_accumulator -= 0x1000u;
        if (!voice.next_sample_valid) {
            // The current sample was the last one.  It has just been rendered;
            // retire the voice without creating a discontinuous extra zero.
            voice.playing = false;
            voice.on = false;
            voice.envelope_height = 0u;
            sas_note_ended(voice, 3);
            break;
        }
        voice.current_sample = voice.next_sample;
        voice.current_sample_valid = true;
        std::int16_t following{};
        if (sas_fetch_voice_sample(memory, voice, following)) {
            voice.next_sample = following;
            voice.next_sample_valid = true;
        } else {
            voice.next_sample_valid = false;
        }
    }
    return sample;
}

std::int32_t sas_render_noise_sample(SasVoiceState &voice) noexcept {
    // Map 0..63 to progressively faster LFSR updates.  Exact spectral shaping is
    // hardware-specific, but implementing the path is crucial: VCS uses SAS
    // noise voices for effects that were previously completely silent.
    voice.noise_phase += static_cast<std::uint32_t>(voice.noise_frequency + 1);
    while (voice.noise_phase >= 64u) {
        voice.noise_phase -= 64u;
        const std::uint32_t feedback =
            ((voice.noise_lfsr >> 0u) ^ (voice.noise_lfsr >> 1u) ^
             (voice.noise_lfsr >> 21u) ^ (voice.noise_lfsr >> 31u)) & 1u;
        voice.noise_lfsr = (voice.noise_lfsr >> 1u) | (feedback << 31u);
        voice.noise_sample = (voice.noise_lfsr & 1u) != 0u ? 12288 : -12288;
    }
    return voice.noise_sample;
}

// Renders one voice into separate dry and effect-send accumulators.  PSP SAS
// has two independent volume pairs per voice.  The old HLE threw the effect
// pair away entirely, so any sound routed only to the wet bus was silent.
void sas_render_voice(const psprecomp::GuestMemory &memory, SasVoiceState &voice,
                      std::vector<std::int32_t> &dry_mix,
                      std::vector<std::int32_t> &effect_send,
                      std::uint32_t frames) {
    if (!voice.playing || voice.paused || voice.type == SasVoiceType::Off) return;

    for (std::uint32_t frame = 0u; frame < frames && voice.playing; ++frame) {
        const std::uint32_t envelope = sas_step_envelope_impl(voice);
        if (!voice.playing || envelope == 0u) continue;

        std::int32_t sample = 0;
        if (voice.type == SasVoiceType::Vag || voice.type == SasVoiceType::Pcm)
            sample = sas_render_stream_sample(memory, voice);
        else if (voice.type == SasVoiceType::Noise)
            sample = sas_render_noise_sample(voice);

        const auto accumulate = [&](std::vector<std::int32_t> &target,
                                    std::int32_t left_volume,
                                    std::int32_t right_volume) {
            const std::int64_t left_gain =
                (static_cast<std::int64_t>(left_volume) * envelope) >> 30;
            const std::int64_t right_gain =
                (static_cast<std::int64_t>(right_volume) * envelope) >> 30;
            target[frame * 2u] += static_cast<std::int32_t>((sample * left_gain) >> 12);
            target[frame * 2u + 1u] += static_cast<std::int32_t>((sample * right_gain) >> 12);
        };
        accumulate(dry_mix, voice.left_volume, voice.right_volume);
        accumulate(effect_send, voice.effect_left_volume, voice.effect_right_volume);
    }
}

void sas_render_buses(const psprecomp::GuestMemory &memory, std::uint32_t frames,
                      std::vector<std::int32_t> &dry_mix,
                      std::vector<std::int32_t> &effect_send) {
    dry_mix.assign(static_cast<std::size_t>(frames) * 2u, 0);
    effect_send.assign(static_cast<std::size_t>(frames) * 2u, 0);
    sas_apply_stop_all();
    // RR_SAS_MUTE=<hex mask>: voices in the mask still advance but are discarded (finding which voice a sound comes from).
    static const std::uint32_t mute_mask = [] {
        const char *e = std::getenv("RR_SAS_MUTE");
        return e ? static_cast<std::uint32_t>(std::strtoul(e, nullptr, 16)) : 0u;
    }();
    std::vector<std::int32_t> scratch_dry, scratch_fx;
    for (std::size_t i = 0; i < sas_state.voices.size(); ++i) {
        if ((i < 32u && ((mute_mask >> i) & 1u) != 0u) || sas_state.voices[i].suppressed) {
            scratch_dry.assign(dry_mix.size(), 0); scratch_fx.assign(effect_send.size(), 0);
            sas_render_voice(memory, sas_state.voices[i], scratch_dry, scratch_fx, frames);
        } else {
            sas_render_voice(memory, sas_state.voices[i], dry_mix, effect_send, frames);
        }
    }
}

// ---- Reverb network -------------------------------------------------------------------------------------------
constexpr float kWetGain = 0.62f;   // reverb output energy relative to the effect send (calibrated against PPSSPP)

struct ReverbBank {
    struct Comb {
        std::vector<float> buf; std::size_t idx{}; float feedback{}; float store{};
        float process(float in) {
            const float out = buf[idx];
            store = out * 0.75f + store * 0.25f;          // damping low-pass inside the loop
            if (store < 1e-9f && store > -1e-9f) store = 0.0f;   // a decaying tail must not reach denormal floats (about 75x slower)
            buf[idx] = in + store * feedback;
            if (++idx >= buf.size()) idx = 0u;
            return out;
        }
    };
    struct Allpass {
        std::vector<float> buf; std::size_t idx{};
        float process(float in) {
            const float b = buf[idx];
            const float out = b - in;
            float next = in + b * 0.5f;
            if (next < 1e-9f && next > -1e-9f) next = 0.0f;
            buf[idx] = next;
            if (++idx >= buf.size()) idx = 0u;
            return out;
        }
    };
    std::array<Comb, 8> combs;
    std::array<Allpass, 4> allpasses;
    void build(float rt60, float size, std::size_t spread) {
        static constexpr std::size_t kComb[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
        static constexpr std::size_t kAll[4] = {556, 441, 341, 225};
        for (int i = 0; i < 8; ++i) {
            const std::size_t len = std::max<std::size_t>(8u, static_cast<std::size_t>(static_cast<float>(kComb[i] + spread) * size));
            combs[i].buf.assign(len, 0.0f);
            combs[i].idx = 0u; combs[i].store = 0.0f;
            combs[i].feedback = std::pow(10.0f, -3.0f * static_cast<float>(len) / (rt60 * 44100.0f));
        }
        for (int i = 0; i < 4; ++i) {
            const std::size_t len = std::max<std::size_t>(4u, static_cast<std::size_t>(static_cast<float>(kAll[i] + spread) * size));
            allpasses[i].buf.assign(len, 0.0f);
            allpasses[i].idx = 0u;
        }
    }
    float process(float in) {
        float sum = 0.0f;
        for (auto &c : combs) sum += c.process(in);
        for (auto &a : allpasses) sum = a.process(sum);
        return sum;
    }
};

struct ReverbDsp {
    static constexpr std::size_t kEchoMax = 44100u * 2u;
    std::int32_t type{-99};
    ReverbBank left, right;
    float gain{1.0f};
    std::vector<float> echo_l, echo_r;
    std::size_t echo_pos{};
    void configure(std::int32_t t) {
        type = t;
        if (echo_l.size() != kEchoMax) { echo_l.assign(kEchoMax, 0.0f); echo_r.assign(kEchoMax, 0.0f); }
        std::fill(echo_l.begin(), echo_l.end(), 0.0f);
        std::fill(echo_r.begin(), echo_r.end(), 0.0f);
        echo_pos = 0u;
        // ROOM, STUDIO A/B/C, HALL, SPACE, (ECHO, DELAY use the echo line), PIPE
        static constexpr float kRt60[9] = {0.45f, 0.6f, 0.8f, 1.0f, 1.8f, 2.6f, 1.0f, 1.0f, 0.6f};
        static constexpr float kSize[9] = {0.35f, 0.5f, 0.65f, 0.8f, 1.1f, 1.4f, 1.0f, 1.0f, 0.25f};
        left.build(kRt60[t], kSize[t], 0u);
        right.build(kRt60[t], kSize[t], 23u);
        // Normalise: run an impulse through a fresh copy; the energy of its response is the network's gain for noise.
        ReverbBank probe;
        probe.build(kRt60[t], kSize[t], 0u);
        double energy = 0.0;
        const std::size_t frames = static_cast<std::size_t>(kRt60[t] * 3.0f * 44100.0f);
        for (std::size_t i = 0; i < frames; ++i) {
            const float y = probe.process(i == 0u ? 1.0f : 0.0f);
            energy += static_cast<double>(y) * y;
        }
        gain = energy > 1e-12 ? static_cast<float>(kWetGain / std::sqrt(energy)) : 0.0f;
    }
};

// ---- SPU-style reverb (the network the PSP's SAS shares with the PlayStation SPU) ---------------------------------
// Runs at 22.05 kHz over a circular work area of 16-bit words.  Register values are in 8-byte units (4 words); the
// preset tables are the published hardware constants.  Per tick: two same-side and two cross-side reflections with
// IIR damping, four comb taps per channel, then two all-pass stages.
struct PsxReverbPreset {
    std::uint32_t size;
    std::uint16_t dAPF1, dAPF2, vIIR, vCOMB1, vCOMB2, vCOMB3, vCOMB4, vWALL, vAPF1, vAPF2,
        mLSAME, mRSAME, mLCOMB1, mRCOMB1, mLCOMB2, mRCOMB2, dLSAME, dRSAME, mLDIFF, mRDIFF,
        mLCOMB3, mRCOMB3, mLCOMB4, mRCOMB4, dLDIFF, dRDIFF, mLAPF1, mRAPF1, mLAPF2, mRAPF2;
};
constexpr PsxReverbPreset kPsxPresets[9] = {
    {0x26C0,0x007D,0x005B,0x6D80,0x54B8,0xBED0,0x0000,0x0000,0xBA80,0x5800,0x5300,0x04D6,0x0333,0x03F0,0x0227,0x0374,0x01EF,0x0334,0x01B5,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x01B4,0x0136,0x00B8,0x005C},
    {0x1F40,0x0033,0x0025,0x70F0,0x4FA8,0xBCE0,0x4410,0xC0F0,0x9C00,0x5280,0x4EC0,0x03E4,0x031B,0x03A4,0x02AF,0x0372,0x0266,0x031C,0x025D,0x025C,0x018E,0x022F,0x0135,0x01D2,0x00B7,0x018F,0x00B5,0x00B4,0x0080,0x004C,0x0026},
    {0x4840,0x00B1,0x007F,0x70F0,0x4FA8,0xBCE0,0x4510,0xBEF0,0xB4C0,0x5280,0x4EC0,0x0904,0x076B,0x0824,0x065F,0x07A2,0x0616,0x076C,0x05ED,0x05EC,0x042E,0x050F,0x0305,0x0462,0x02B7,0x042F,0x0265,0x0264,0x01B2,0x0100,0x0080},
    {0x6FE0,0x00E3,0x00A9,0x6F60,0x4FA8,0xBCE0,0x4510,0xBEF0,0xA680,0x5680,0x52C0,0x0DFB,0x0B58,0x0D09,0x0A3C,0x0BD9,0x0973,0x0B59,0x08DA,0x08D9,0x05E9,0x07EC,0x04B0,0x06EF,0x03D2,0x05EA,0x031D,0x031C,0x0238,0x0154,0x00AA},
    {0xADE0,0x01A5,0x0139,0x6000,0x5000,0x4C00,0xB800,0xBC00,0xC000,0x6000,0x5C00,0x15BA,0x11BB,0x14C2,0x10BD,0x11BC,0x0DC1,0x11C0,0x0DC3,0x0DC0,0x09C1,0x0BC4,0x07C1,0x0A00,0x06CD,0x09C2,0x05C1,0x05C0,0x041A,0x0274,0x013A},
    {0xF6C0,0x033D,0x0231,0x7E00,0x5000,0xB400,0xB000,0x4C00,0xB000,0x6000,0x5400,0x1ED6,0x1A31,0x1D14,0x183B,0x1BC2,0x16B2,0x1A32,0x15EF,0x15EE,0x1055,0x1334,0x0F2D,0x11F6,0x0C5D,0x1056,0x0AE1,0x0AE0,0x07A2,0x0464,0x0232},
    {0x18040,0x0001,0x0001,0x7FFF,0x7FFF,0x0000,0x0000,0x0000,0xC080,0x0000,0x0000,0x1FFF,0x0FFF,0x1005,0x0005,0x0000,0x0000,0x1005,0x0005,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x1004,0x1002,0x0004,0x0002},
    {0x18040,0x0001,0x0001,0x7FFF,0x7FFF,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x1FFF,0x0FFF,0x1005,0x0005,0x0000,0x0000,0x1005,0x0005,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x1004,0x1002,0x0004,0x0002},
    {0x3C00,0x0017,0x0013,0x70F0,0x4FA8,0xBCE0,0x4510,0xBEF0,0x8500,0x5F80,0x54C0,0x0371,0x02AF,0x02E5,0x01DF,0x02B0,0x01D7,0x0358,0x026A,0x01D6,0x011E,0x012D,0x00B1,0x011F,0x0059,0x01A0,0x00E3,0x0058,0x0040,0x0028,0x0014},
};
struct PsxReverb {
    std::int32_t type{-99};
    std::vector<std::int16_t> ram;
    std::size_t pos{}, words{};
    PsxReverbPreset p{};
    int phase{};
    std::int32_t in_l{}, in_r{}, cur_l{}, cur_r{};
    void configure(std::int32_t t) {
        type = t;
        p = kPsxPresets[std::clamp(t, 0, 8)];
        words = std::max<std::size_t>(64u, static_cast<std::size_t>(p.size) / 2u);
        ram.assign(words, 0);
        pos = 0u; phase = 0; in_l = in_r = cur_l = cur_r = 0;
    }
    static std::int32_t sat(std::int32_t v) { return std::clamp(v, -32768, 32767); }
    std::size_t at(std::int32_t reg_units, std::int32_t extra = 0) const {
        const std::int64_t a = static_cast<std::int64_t>(reg_units) * 4 + static_cast<std::int64_t>(pos) + extra;
        const std::int64_t m = static_cast<std::int64_t>(words);
        return static_cast<std::size_t>(((a % m) + m) % m);
    }
    void tick(std::int32_t lin, std::int32_t rin, std::int32_t &out_l, std::int32_t &out_r) {
        auto v = [](std::uint16_t x) { return static_cast<std::int32_t>(static_cast<std::int16_t>(x)); };
        const std::int32_t iir = v(p.vIIR), wall = v(p.vWALL);
        auto reflect = [&](std::uint16_t m, std::uint16_t d, std::int32_t input) {
            const std::int32_t prev = ram[at(m, -1)];
            const std::int32_t delayed = ram[at(d)];
            const std::int32_t val = ((input + ((delayed * wall) >> 15) - prev) * iir >> 15) + prev;
            ram[at(m)] = static_cast<std::int16_t>(sat(val));
        };
        reflect(p.mLSAME, p.dLSAME, lin);
        reflect(p.mRSAME, p.dRSAME, rin);
        reflect(p.mLDIFF, p.dRDIFF, lin);
        reflect(p.mRDIFF, p.dLDIFF, rin);
        auto channel = [&](std::uint16_t c1, std::uint16_t c2, std::uint16_t c3, std::uint16_t c4,
                           std::uint16_t a1, std::uint16_t a2) {
            std::int32_t o = (v(p.vCOMB1) * ram[at(c1)] + v(p.vCOMB2) * ram[at(c2)] +
                              v(p.vCOMB3) * ram[at(c3)] + v(p.vCOMB4) * ram[at(c4)]) >> 15;
            o = sat(o);
            const std::int32_t ap1 = v(p.vAPF1), ap2 = v(p.vAPF2);
            const std::int32_t d1 = ram[at(a1, -static_cast<std::int32_t>(p.dAPF1) * 4)];
            o = sat(o - ((ap1 * d1) >> 15));
            ram[at(a1)] = static_cast<std::int16_t>(o);
            o = sat(((o * ap1) >> 15) + d1);
            const std::int32_t d2 = ram[at(a2, -static_cast<std::int32_t>(p.dAPF2) * 4)];
            o = sat(o - ((ap2 * d2) >> 15));
            ram[at(a2)] = static_cast<std::int16_t>(o);
            return sat(((o * ap2) >> 15) + d2);
        };
        out_l = channel(p.mLCOMB1, p.mLCOMB2, p.mLCOMB3, p.mLCOMB4, p.mLAPF1, p.mLAPF2);
        out_r = channel(p.mRCOMB1, p.mRCOMB2, p.mRCOMB3, p.mRCOMB4, p.mRAPF1, p.mRAPF2);
        pos = (pos + 1u) % words;
    }
};

// A conservative host-side recreation of the SAS effect-send path.  The exact
// PSP presets are hardware-specific, but the important observable semantics are
// preserved here: effectLeft/effectRight feed a persistent wet bus, RevVON can
// select dry/wet independently, RevEVOL scales wet output, and delay/feedback
// persist between grains.  Type OFF is a transparent wet send rather than a
// destructive silent sink; this avoids dropping effect-routed SFX while still
// keeping the default (dry on, wet off) bit-for-bit simple.
void sas_process_effect_send(const std::vector<std::int32_t> &effect_send,
                             std::vector<std::int32_t> &wet_mix,
                             std::uint32_t frames) {
    wet_mix.assign(static_cast<std::size_t>(frames) * 2u, 0);
    if (!sas_state.reverb.wet) return;

    const std::int64_t global_left = sas_state.reverb.left_volume;
    const std::int64_t global_right = sas_state.reverb.right_volume;
    if (sas_state.reverb.type < 0) {
        for (std::uint32_t frame = 0u; frame < frames; ++frame) {
            wet_mix[frame * 2u] = static_cast<std::int32_t>(
                (static_cast<std::int64_t>(effect_send[frame * 2u]) * global_left) >> 12);
            wet_mix[frame * 2u + 1u] = static_cast<std::int32_t>(
                (static_cast<std::int64_t>(effect_send[frame * 2u + 1u]) * global_right) >> 12);
        }
        return;
    }

    // The PSP's reverb is a diffuse decorrelated tail built from the effect send; it never passes the send
    // through at full level.  (The first version here added the send straight onto the output and fed a delayed
    // copy back, which came out several dB too loud and made Ridge Racer's menus and tunnels clip.)  This is a
    // Freeverb-style comb/all-pass network per channel, with the decay time chosen by the effect type and the
    // output normalised so that its energy is a fixed fraction of the send (kWetGain), whatever the decay time.
    auto &reverb = sas_state.reverb;
    static ReverbDsp dsp;
    const std::int32_t type = std::clamp(reverb.type, 0, 8);
    if (dsp.type != type) dsp.configure(type);
    const bool echo = type == 6 || type == 7;
    const float echo_feedback = static_cast<float>(std::clamp<std::int32_t>(reverb.feedback, 0, 127)) / 128.0f;
    const std::size_t echo_frames = std::clamp<std::size_t>(
        1764u + static_cast<std::size_t>(reverb.delay) * 172u, 1u, ReverbDsp::kEchoMax - 1u);
    static const bool old_reverb = std::getenv("RR_REVERB_OLD") != nullptr;
    static const bool no_reverb = std::getenv("RR_REVERB_OFF") != nullptr;
    static bool announced = false;
    if (!announced) {
        announced = true;
        std::fprintf(stderr, "[SAS] reverb path: %s (type %d, evol %d/%d)\n",
                     no_reverb ? "OFF (RR_REVERB_OFF)" : echo ? "echo line" : old_reverb ? "old freeverb" : "SPU-style",
                     static_cast<int>(type), static_cast<int>(global_left), static_cast<int>(global_right));
    }
    if (no_reverb) return;
    if (!echo && !old_reverb) {
        static PsxReverb psx;
        if (psx.type != type) psx.configure(type);
        for (std::uint32_t frame = 0u; frame < frames; ++frame) {
            const std::int32_t l = PsxReverb::sat(effect_send[frame * 2u] >> 1);
            const std::int32_t r = PsxReverb::sat(effect_send[frame * 2u + 1u] >> 1);
            std::int32_t out_l, out_r;
            if (psx.phase == 0) {
                psx.in_l = l; psx.in_r = r; psx.phase = 1;
                out_l = psx.cur_l; out_r = psx.cur_r;
            } else {
                std::int32_t nl, nr;
                psx.tick((psx.in_l + l) >> 1, (psx.in_r + r) >> 1, nl, nr);
                out_l = (psx.cur_l + nl) >> 1; out_r = (psx.cur_r + nr) >> 1;
                psx.cur_l = nl; psx.cur_r = nr; psx.phase = 0;
            }
            wet_mix[frame * 2u] = static_cast<std::int32_t>((static_cast<std::int64_t>(out_l) * global_left) >> 12);
            wet_mix[frame * 2u + 1u] = static_cast<std::int32_t>((static_cast<std::int64_t>(out_r) * global_right) >> 12);
        }
        return;
    }
    for (std::uint32_t frame = 0u; frame < frames; ++frame) {
        float out_l, out_r;
        const float in_l = static_cast<float>(effect_send[frame * 2u]);
        const float in_r = static_cast<float>(effect_send[frame * 2u + 1u]);
        if (echo) {
            const std::size_t read = (dsp.echo_pos + ReverbDsp::kEchoMax - echo_frames) % ReverbDsp::kEchoMax;
            out_l = dsp.echo_l[read];
            out_r = dsp.echo_r[read];
            dsp.echo_l[dsp.echo_pos] = in_l + out_l * echo_feedback;
            dsp.echo_r[dsp.echo_pos] = in_r + out_r * echo_feedback;
            dsp.echo_pos = (dsp.echo_pos + 1u) % ReverbDsp::kEchoMax;
            out_l *= kWetGain;
            out_r *= kWetGain;
        } else {
            out_l = dsp.left.process(in_l) * dsp.gain;
            out_r = dsp.right.process(in_r) * dsp.gain;
        }
        wet_mix[frame * 2u] = static_cast<std::int32_t>((static_cast<std::int64_t>(out_l) * global_left) >> 12);
        wet_mix[frame * 2u + 1u] = static_cast<std::int32_t>((static_cast<std::int64_t>(out_r) * global_right) >> 12);
    }
}

// Mixed output mode: CoreWithMix starts from caller PCM, then the selected SAS
// dry/wet buses are added.  Plain Core uses the same path without caller input.
struct SasMeter { double dry{}, wet{}; std::uint64_t n{}; };
SasMeter g_sas_meter;

void sas_mix_into(psprecomp::Runtime &rt, std::uint32_t output, std::uint32_t frames,
                  bool include_input = false,
                  std::uint32_t input_left = 0x1000u,
                  std::uint32_t input_right = 0x1000u) {
    static thread_local std::vector<std::int32_t> dry_mix;
    static thread_local std::vector<std::int32_t> effect_send;
    static thread_local std::vector<std::int32_t> wet_mix;
    sas_render_buses(rt.memory(), frames, dry_mix, effect_send);
    sas_process_effect_send(effect_send, wet_mix, frames);

    for (std::uint32_t frame = 0u; frame < frames; ++frame) {
        std::int64_t l = 0;
        std::int64_t r = 0;
        if (include_input) {
            const auto input_l = static_cast<std::int16_t>(
                rt.memory().aot_load16(output + frame * 4u));
            const auto input_r = static_cast<std::int16_t>(
                rt.memory().aot_load16(output + frame * 4u + 2u));
            l += (static_cast<std::int64_t>(input_l) * input_left) >> 12;
            r += (static_cast<std::int64_t>(input_r) * input_right) >> 12;
        }
        if (sas_state.reverb.dry) {
            l += dry_mix[frame * 2u];
            r += dry_mix[frame * 2u + 1u];
        }
        if (sas_state.reverb.wet) {
            l += wet_mix[frame * 2u];
            r += wet_mix[frame * 2u + 1u];
        }
        g_sas_meter.dry += static_cast<double>(dry_mix[frame * 2u]) * dry_mix[frame * 2u] + static_cast<double>(dry_mix[frame * 2u + 1u]) * dry_mix[frame * 2u + 1u];
        g_sas_meter.wet += static_cast<double>(wet_mix[frame * 2u]) * wet_mix[frame * 2u] + static_cast<double>(wet_mix[frame * 2u + 1u]) * wet_mix[frame * 2u + 1u];
        g_sas_meter.n += 2u;
        rt.memory().store16(output + frame * 4u, static_cast<std::uint16_t>(
            static_cast<std::int16_t>(std::clamp<std::int64_t>(l, -32768, 32767))));
        rt.memory().store16(output + frame * 4u + 2u, static_cast<std::uint16_t>(
            static_cast<std::int16_t>(std::clamp<std::int64_t>(r, -32768, 32767))));
    }
}

// Raw output mode exposes four non-interleaved planes: dry L, dry R, effect L,
// effect R.  The previous HLE incorrectly treated mode 1 as mono and allocated
// only two bytes per frame, which could both lose send-routed sounds and write
// the wrong guest buffer layout.
void sas_mix_raw(psprecomp::Runtime &rt, std::uint32_t output, std::uint32_t frames) {
    static thread_local std::vector<std::int32_t> dry_mix;
    static thread_local std::vector<std::int32_t> effect_send;
    sas_render_buses(rt.memory(), frames, dry_mix, effect_send);
    const std::uint32_t left_base = output;
    const std::uint32_t right_base = output + frames * 2u;
    const std::uint32_t send_left_base = output + frames * 4u;
    const std::uint32_t send_right_base = output + frames * 6u;
    for (std::uint32_t frame = 0u; frame < frames; ++frame) {
        const auto store = [&](std::uint32_t base, std::int32_t value) {
            rt.memory().store16(base + frame * 2u, static_cast<std::uint16_t>(
                static_cast<std::int16_t>(std::clamp(value, -32768, 32767))));
        };
        store(left_base, dry_mix[frame * 2u]);
        store(right_base, dry_mix[frame * 2u + 1u]);
        store(send_left_base, effect_send[frame * 2u]);
        store(send_right_base, effect_send[frame * 2u + 1u]);
    }
}

} // namespace

void install_sce_sas_core_hle(Runtime &runtime) {
    runtime.register_hle("sceSasCore", 0x42778A9Fu, // sceSasInit
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            const std::uint32_t core = ctx.gpr[4];
            const std::uint32_t grain = ctx.gpr[5];
            const std::uint32_t max_voices = ctx.gpr[6];
            const std::uint32_t output_mode = ctx.gpr[7];
            const std::uint32_t sample_rate = ctx.gpr[8];
            const auto log_init = [&](std::uint32_t result) {
                std::cerr << "[SAS] init core=" << hex32(core) << " grain=" << grain
                          << " voices=" << max_voices << " output_mode=" << output_mode
                          << " sample_rate=" << sample_rate
                          << " result=" << hex32(result) << "\n";
            };
            if ((core & 0x3Fu) != 0u || !rt.memory().contains(core, 64u)) {
                ctx.set_gpr(2, kSasErrorBadAddress); log_init(kSasErrorBadAddress); return;
            }
            if (max_voices == 0u || max_voices > 32u) {
                ctx.set_gpr(2, kSasErrorInvalidMaxVoices); log_init(kSasErrorInvalidMaxVoices); return;
            }
            if (grain < 0x40u || grain > 0x800u || (grain & 0x1Fu) != 0u) {
                ctx.set_gpr(2, kSasErrorInvalidGrain); log_init(kSasErrorInvalidGrain); return;
            }
            if (output_mode > 1u) {
                ctx.set_gpr(2, kSasErrorInvalidOutputMode); log_init(kSasErrorInvalidOutputMode); return;
            }
            if (sample_rate != 44100u) {
                ctx.set_gpr(2, kSasErrorInvalidSampleRate); log_init(kSasErrorInvalidSampleRate); return;
            }
            sas_state = SasState{};
            sas_core_mix_calls = 0u;
            sas_core_with_mix_calls = 0u;
            sas_state.initialized = true;
            sas_state.core_address = core;
            sas_state.grain_size = grain;
            // Hardware exposes all 32 voices even when maxVoices is smaller.
            sas_state.max_voices = 32u;
            sas_state.output_mode = output_mode;
            sas_state.sample_rate = sample_rate;
            for (auto &voice : sas_state.voices) voice.pitch = 0x1000;
            rt.memory().zero(core, 64u);
            ++g_sas_contexts;
            log_init(0u);
            sas_trace("sceSasInit", ctx, 0u);
            set_success(ctx);
        });

    runtime.register_hle("sceSasCore", 0xE175EF66u, // sceSasGetOutputmode
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            if (!sas_valid_core(ctx.gpr[4])) { ctx.set_gpr(2, kSasErrorNotInitialized); return; }
            const std::uint32_t mode = sas_state.output_mode;
            sas_trace("sceSasGetOutputmode", ctx, mode);
            ctx.set_gpr(2, mode);
        });

    runtime.register_hle("sceSasCore", 0xE855BF76u, // sceSasSetOutputmode
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            if (!sas_valid_core(ctx.gpr[4])) { ctx.set_gpr(2, kSasErrorNotInitialized); return; }
            const std::uint32_t mode = ctx.gpr[5];
            if (mode > 1u) { ctx.set_gpr(2, kSasErrorInvalidOutputMode); return; }
            sas_state.output_mode = mode;
            sas_trace("sceSasSetOutputmode", ctx, 0u, "mode=" + std::to_string(mode));
            set_success(ctx);
        });

    runtime.register_hle("sceSasCore", 0xBD11B7C2u, // sceSasGetGrain
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            if (!sas_valid_core(ctx.gpr[4])) { ctx.set_gpr(2, kSasErrorNotInitialized); return; }
            ctx.set_gpr(2, sas_state.grain_size);
        });

    runtime.register_hle("sceSasCore", 0xD1E0A01Eu, // sceSasSetGrain
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            if (!sas_valid_core(ctx.gpr[4])) { ctx.set_gpr(2, kSasErrorNotInitialized); return; }
            const std::uint32_t grain = ctx.gpr[5];
            if (grain < 0x40u || grain > 0x800u || (grain & 0x1Fu) != 0u) {
                ctx.set_gpr(2, kSasErrorInvalidGrain); return;
            }
            sas_state.grain_size = grain;
            sas_trace("sceSasSetGrain", ctx, 0u, "grain=" + std::to_string(grain));
            set_success(ctx);
        });

    runtime.register_hle("sceSasCore", 0x99944089u, // sceSasSetVoice
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            auto *voice = sas_voice(ctx.gpr[4], static_cast<std::int32_t>(ctx.gpr[5]), ctx);
            if (!voice) return;
            sas_note_voice(static_cast<std::int32_t>(ctx.gpr[5]));
            const std::uint32_t address = ctx.gpr[6];
            std::int32_t size = static_cast<std::int32_t>(ctx.gpr[7]);
            const std::int32_t loop = static_cast<std::int32_t>(ctx.gpr[8]);
            if (size == 0 || (static_cast<std::uint32_t>(size) & 0xFu) != 0u) {
                ctx.set_gpr(2, kSasErrorInvalidParameter); return;
            }
            if (loop != 0 && loop != 1) {
                ctx.set_gpr(2, kSasErrorInvalidLoop); return;
            }
            if (size < 0) size = 0;
            if (size > 0 && !rt.memory().contains(address, static_cast<std::size_t>(size))) {
                // PSP ignores an invalid VAG pointer, leaving the voice configured.
                set_success(ctx); return;
            }
            if (g_sas_event_observer != nullptr && size > 0) {
                // Once per sample: where its loop / end flags sit (diagnosis of voices that never stop).
                static std::unordered_set<std::uint64_t> scanned;
                if (scanned.insert((static_cast<std::uint64_t>(address) << 32) | static_cast<std::uint32_t>(size)).second) {
                    std::string text = "vagflags addr=" + hex32(address) + " size=" + std::to_string(size) +
                                       " blocks=" + std::to_string(size / 16) + " loopmode=" + std::to_string(loop) + " flags:";
                    int shown = 0;
                    for (std::int32_t b = 0; b < size / 16 && shown < 14; ++b) {
                        const std::uint32_t fl = rt.memory().aot_load8(address + static_cast<std::uint32_t>(b) * 16u + 1u);
                        if (fl != 0u) { text += " " + std::to_string(b) + ":" + std::to_string(fl); ++shown; }
                    }
                    g_sas_event_observer(text.c_str(), -1, size);
                }
            }
            voice->type = SasVoiceType::Vag;
            voice->data_address = address;
            voice->data_size = size;
            voice->loop = loop != 0;
            sas_reset_decoder(*voice);
            if (voice->on) voice->playing = true;
            sas_trace("sceSasSetVoice", ctx, 0u,
                      sas_voice_detail(static_cast<std::int32_t>(ctx.gpr[5])) +
                          " addr=" + hex32(address) + " size=" + std::to_string(size) +
                          " loop=" + std::to_string(loop));
            set_success(ctx);
        });

    runtime.register_hle("sceSasCore", 0xE1CD9561u, // sceSasSetVoicePCM
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            auto *voice = sas_voice(ctx.gpr[4], static_cast<std::int32_t>(ctx.gpr[5]), ctx);
            if (!voice) return;
            sas_note_voice(static_cast<std::int32_t>(ctx.gpr[5]));
            const std::uint32_t address = ctx.gpr[6];
            const std::int32_t samples = static_cast<std::int32_t>(ctx.gpr[7]);
            const std::int32_t loop_position = static_cast<std::int32_t>(ctx.gpr[8]);
            if (samples <= 0 || samples > 0x10000) {
                ctx.set_gpr(2, kSasErrorInvalidPcmSize); return;
            }
            if (loop_position >= samples) {
                ctx.set_gpr(2, kSasErrorInvalidLoop); return;
            }
            if (!rt.memory().contains(address, static_cast<std::size_t>(samples) * 2u)) {
                // PSP ignores an invalid PCM pointer.
                set_success(ctx); return;
            }
            voice->type = SasVoiceType::Pcm;
            voice->data_address = address;
            voice->data_size = samples;
            voice->loop = loop_position >= 0;
            voice->pcm_loop_position = loop_position >= 0 ? static_cast<std::uint32_t>(loop_position) : 0u;
            sas_reset_decoder(*voice);
            // Matches PSP/PPSSPP semantics: a PCM voice starts playing as soon
            // as it is installed (KeyOn then only opens the envelope).
            voice->playing = true;
            sas_trace("sceSasSetVoicePCM", ctx, 0u,
                      sas_voice_detail(static_cast<std::int32_t>(ctx.gpr[5])) +
                          " addr=" + hex32(address) + " samples=" + std::to_string(samples) +
                          " loop=" + std::to_string(loop_position));
            set_success(ctx);
        });

    runtime.register_hle("sceSasCore", 0xB7660A23u,
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            auto *voice = sas_voice(ctx.gpr[4], static_cast<std::int32_t>(ctx.gpr[5]), ctx);
            if (!voice) return;
            const std::int32_t frequency = static_cast<std::int32_t>(ctx.gpr[6]);
            if (frequency < 0 || frequency >= 64) {
                ctx.set_gpr(2, kSasErrorInvalidNoiseFrequency); return;
            }
            voice->type = SasVoiceType::Noise;
            voice->noise_frequency = frequency;
            sas_reset_decoder(*voice);
            if (voice->on) voice->playing = true;
            set_success(ctx);
        });

    runtime.register_hle("sceSasCore", 0xAD84D37Fu, // sceSasSetPitch
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            auto *voice = sas_voice(ctx.gpr[4], static_cast<std::int32_t>(ctx.gpr[5]), ctx);
            if (!voice) return;
            sas_note_voice(static_cast<std::int32_t>(ctx.gpr[5]));
            const std::int32_t pitch = static_cast<std::int32_t>(ctx.gpr[6]);
            if (pitch < 0 || pitch > 0x4000) {
                if (g_sas_event_observer != nullptr) g_sas_event_observer("pitch_REJECTED", static_cast<int>(ctx.gpr[5]), pitch);
                ctx.set_gpr(2, kSasErrorInvalidPitch); return;
            }
            voice->pitch = pitch;
            sas_trace("sceSasSetPitch", ctx, 0u,
                      sas_voice_detail(static_cast<std::int32_t>(ctx.gpr[5])) +
                          " pitch=" + std::to_string(pitch));
            set_success(ctx);
        });

    runtime.register_hle("sceSasCore", 0x440CA7D8u, // sceSasSetVolume
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            auto *voice = sas_voice(ctx.gpr[4], static_cast<std::int32_t>(ctx.gpr[5]), ctx);
            if (!voice) return;
            sas_note_voice(static_cast<std::int32_t>(ctx.gpr[5]));
            const std::array<std::int32_t, 4> volumes{
                static_cast<std::int32_t>(ctx.gpr[6]), static_cast<std::int32_t>(ctx.gpr[7]),
                static_cast<std::int32_t>(ctx.gpr[8]), static_cast<std::int32_t>(ctx.gpr[9])};
            for (const auto volume : volumes) {
                if (static_cast<std::int64_t>(volume) < -0x1000ll || static_cast<std::int64_t>(volume) > 0x1000ll) {
                    if (g_sas_event_observer != nullptr) {
                        // the offending value, and the (unchanged) voice
                        g_sas_event_observer("volume_REJECTED", static_cast<int>(ctx.gpr[5]), volume);
                    }
                    ctx.set_gpr(2, kSasErrorInvalidVolume); return;
                }
            }
            if (g_sas_event_observer != nullptr) g_sas_event_observer("volume", static_cast<int>(ctx.gpr[5]), volumes[0] | volumes[1]);
            if (voice->left_volume != volumes[0] || voice->right_volume != volumes[1] ||
                voice->effect_left_volume != volumes[2] || voice->effect_right_volume != volumes[3])
                sas_activate_voice(static_cast<std::int32_t>(ctx.gpr[5]));
            voice->left_volume = volumes[0]; voice->right_volume = volumes[1];
            voice->effect_left_volume = volumes[2]; voice->effect_right_volume = volumes[3];
            sas_trace("sceSasSetVolume", ctx, 0u,
                      sas_voice_detail(static_cast<std::int32_t>(ctx.gpr[5])) +
                          " left=" + std::to_string(volumes[0]) +
                          " right=" + std::to_string(volumes[1]) +
                          " effect_left=" + std::to_string(volumes[2]) +
                          " effect_right=" + std::to_string(volumes[3]));
            if (sas_audio_diagnostics_enabled() &&
                (volumes[2] != 0 || volumes[3] != 0)) {
                std::cerr << "[sas] volume voice=" << static_cast<std::int32_t>(ctx.gpr[5])
                          << " dry=" << volumes[0] << "," << volumes[1]
                          << " effect=" << volumes[2] << "," << volumes[3] << "\n";
            }
            set_success(ctx);
        });

    runtime.register_hle("sceSasCore", 0x019B25EBu,
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            auto *voice = sas_voice(ctx.gpr[4], static_cast<std::int32_t>(ctx.gpr[5]), ctx);
            if (!voice) return;
            const std::uint32_t flags = ctx.gpr[6] & 0xFu;
            const std::array<std::int32_t, 4> rates{
                static_cast<std::int32_t>(ctx.gpr[7]), static_cast<std::int32_t>(ctx.gpr[8]),
                static_cast<std::int32_t>(ctx.gpr[9]), static_cast<std::int32_t>(ctx.gpr[10])};
            for (std::size_t i = 0; i < rates.size(); ++i) {
                if ((flags & (1u << i)) != 0u && rates[i] < 0) {
                    ctx.set_gpr(2, kSasErrorInvalidAdsrRate); return;
                }
            }
            for (std::size_t i = 0; i < rates.size(); ++i)
                if ((flags & (1u << i)) != 0u) voice->adsr_rates[i] = rates[i];
            if (flags != 0u) voice->adsr_configured = true;
            set_success(ctx);
        });

    runtime.register_hle("sceSasCore", 0x9EC3676Au,
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            auto *voice = sas_voice(ctx.gpr[4], static_cast<std::int32_t>(ctx.gpr[5]), ctx);
            if (!voice) return;
            const std::uint32_t flags = ctx.gpr[6] & 0xFu;
            std::array<std::int32_t, 4> modes{
                static_cast<std::int32_t>(ctx.gpr[7] & 0x7FFFFFFFu), static_cast<std::int32_t>(ctx.gpr[8] & 0x7FFFFFFFu),
                static_cast<std::int32_t>(ctx.gpr[9] & 0x7FFFFFFFu), static_cast<std::int32_t>(ctx.gpr[10] & 0x7FFFFFFFu)};
            const bool invalid_attack = modes[0] > 5 || (modes[0] & 1) != 0;
            const bool invalid_decay = modes[1] > 5 || (modes[1] & 1) != 1;
            const bool invalid_sustain = modes[2] > 5;
            const bool invalid_release = modes[3] > 5 || (modes[3] & 1) != 1;
            const std::array<bool, 4> invalid{invalid_attack, invalid_decay, invalid_sustain, invalid_release};
            for (std::size_t i = 0; i < invalid.size(); ++i) {
                if ((flags & (1u << i)) != 0u && invalid[i]) {
                    ctx.set_gpr(2, kSasErrorInvalidAdsrMode); return;
                }
            }
            for (std::size_t i = 0; i < modes.size(); ++i)
                if ((flags & (1u << i)) != 0u) voice->adsr_modes[i] = modes[i];
            if (flags != 0u) voice->adsr_configured = true;
            set_success(ctx);
        });

    runtime.register_hle("sceSasCore", 0x5F9529F6u,
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            auto *voice = sas_voice(ctx.gpr[4], static_cast<std::int32_t>(ctx.gpr[5]), ctx);
            if (!voice) return;
            voice->sustain_level = static_cast<std::int32_t>(ctx.gpr[6]);
            voice->adsr_configured = true;
            set_success(ctx);
        });

    runtime.register_hle("sceSasCore", 0xCBCD4F79u,
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            auto *voice = sas_voice(ctx.gpr[4], static_cast<std::int32_t>(ctx.gpr[5]), ctx);
            if (!voice) return;
            if (((ctx.gpr[7] >> 13u) & 1u) != 0u) {
                ctx.set_gpr(2, kSasErrorInvalidAdsrMode); return;
            }
            voice->simple_adsr1 = ctx.gpr[6] & 0xFFFFu;
            voice->simple_adsr2 = ctx.gpr[7] & 0xFFFFu;
            sas_decode_simple_adsr(*voice);
            set_success(ctx);
        });

    runtime.register_hle("sceSasCore", 0x76F01ACAu, // sceSasSetKeyOn
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            auto *voice = sas_voice(ctx.gpr[4], static_cast<std::int32_t>(ctx.gpr[5]), ctx);
            if (!voice) return;
            sas_note_voice(static_cast<std::int32_t>(ctx.gpr[5]));
            if (voice->paused || voice->on) {
                if (g_sas_event_observer != nullptr)
                    g_sas_event_observer(voice->paused ? "keyon_REJECTED_paused" : "keyon_REJECTED_already_on", static_cast<int>(ctx.gpr[5]), 0);
                ctx.set_gpr(2, kSasErrorVoicePaused); return;
            }
            sas_activate_voice(static_cast<std::int32_t>(ctx.gpr[5]));
            sas_reset_decoder(*voice);
            voice->on = true;
            voice->playing = voice->type != SasVoiceType::Off;
            voice->envelope_height = 0u;
            voice->envelope_phase = SasEnvelopePhase::Attack;
            voice->key_on_delay_samples = voice->adsr_configured
                ? (voice->type == SasVoiceType::Vag ? 33u : 32u) : 0u;
            ++g_sas_key_on_calls;
            if (g_sas_event_observer != nullptr) g_sas_event_observer("keyon", static_cast<int>(ctx.gpr[5]), voice->loop ? 1 : 0);
            if (g_sas_keyon_observer != nullptr) g_sas_keyon_observer(static_cast<int>(ctx.gpr[5]), *voice);
            sas_trace("sceSasSetKeyOn", ctx, 0u,
                      sas_voice_detail(static_cast<std::int32_t>(ctx.gpr[5])) +
                          " type=" + std::to_string(static_cast<int>(voice->type)) +
                          " pitch=" + std::to_string(voice->pitch) +
                          " loop=" + std::to_string(voice->loop));
            if (sas_audio_diagnostics_enabled())
                std::cerr << "[sas] keyon voice=" << static_cast<std::int32_t>(ctx.gpr[5])
                          << " type=" << static_cast<int>(voice->type)
                          << " pitch=" << voice->pitch
                          << " loop=" << voice->loop << "\n";
            set_success(ctx);
        });

    runtime.register_hle("sceSasCore", 0xA0CF2FA4u, // sceSasSetKeyOff
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            auto *voice = sas_voice(ctx.gpr[4], static_cast<std::int32_t>(ctx.gpr[5]), ctx);
            if (!voice) return;
            sas_note_voice(static_cast<std::int32_t>(ctx.gpr[5]));
            sas_note_key_off_call(voice->data_size >= 49152);
            if (voice->paused) {
                if (g_sas_event_observer != nullptr) g_sas_event_observer("keyoff_REJECTED_paused", static_cast<int>(ctx.gpr[5]), 0);
                ctx.set_gpr(2, kSasErrorVoicePaused); return;
            }
            if (!voice->on) {
                // A voice that has already finished on its own: the game still sends its key-off.  The PSP accepts it
                // (nothing to do); answering with an error here made Ridge Racer's sound code fault at the countdown.
                // RR_SAS_KEYOFF_ERR=1 restores the old error.
                static const bool legacy_err = std::getenv("RR_SAS_KEYOFF_ERR") != nullptr;
                if (g_sas_event_observer != nullptr) g_sas_event_observer("keyoff_not_on", static_cast<int>(ctx.gpr[5]), 0);
                if (legacy_err) { ctx.set_gpr(2, kSasErrorVoicePaused); return; }
                set_success(ctx);
                return;
            }
            voice->on = false;
            voice->envelope_phase = SasEnvelopePhase::Release;
            ++g_sas_key_off_calls;
            if (g_sas_event_observer != nullptr) g_sas_event_observer("keyoff", static_cast<int>(ctx.gpr[5]), 0);
            // A release rate of zero never walks the envelope down, so a looping
            // voice keyed off here would sound forever.  Log it when diagnostics
            // are enabled: that is the remaining suspect for endless loops.
            sas_trace("sceSasSetKeyOff", ctx, 0u,
                      sas_voice_detail(static_cast<std::int32_t>(ctx.gpr[5])) +
                          " loop=" + std::to_string(voice->loop) +
                          " release_mode=" + std::to_string(voice->adsr_modes[3]) +
                          " release_rate=" + std::to_string(voice->adsr_rates[3]));
            if (sas_audio_diagnostics_enabled())
                std::cerr << "[sas] keyoff voice=" << static_cast<std::int32_t>(ctx.gpr[5])
                          << " loop=" << voice->loop
                          << " release_mode=" << voice->adsr_modes[3]
                          << " release_rate=" << voice->adsr_rates[3]
                          << " height=" << voice->envelope_height << "\n";
            set_success(ctx);
        });

    runtime.register_hle("sceSasCore", 0x787D04D5u,
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            if (!sas_valid_core(ctx.gpr[4])) { ctx.set_gpr(2, kSasErrorNotInitialized); return; }
            std::uint32_t mask = ctx.gpr[5];
            const bool pause = ctx.gpr[6] != 0u;
            if (g_sas_event_observer != nullptr) g_sas_event_observer("pause", static_cast<int>(mask), pause ? 1 : 0);
            for (std::size_t i = 0; i < sas_state.voices.size(); ++i)
                if ((mask & (1u << i)) != 0u) sas_state.voices[i].paused = pause;
            set_success(ctx);
        });

    runtime.register_hle("sceSasCore", 0x2C8E6AB3u,
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            if (!sas_valid_core(ctx.gpr[4])) { ctx.set_gpr(2, kSasErrorNotInitialized); return; }
            std::uint32_t flags = 0u;
            for (std::size_t i = 0; i < sas_state.voices.size(); ++i)
                if (sas_state.voices[i].paused) flags |= 1u << i;
            ctx.set_gpr(2, flags);
        });

    runtime.register_hle("sceSasCore", 0x68A46B95u,
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            if (!sas_valid_core(ctx.gpr[4])) { ctx.set_gpr(2, kSasErrorNotInitialized); return; }
            std::uint32_t flags = 0u;
            for (std::size_t i = 0; i < sas_state.voices.size(); ++i)
                if (!sas_state.voices[i].playing) flags |= 1u << i;
            {
                static std::uint32_t last_flags = 0xDEADBEEFu;
                if (flags != last_flags) {
                    last_flags = flags;
                    if (g_sas_event_observer != nullptr) g_sas_event_observer("endflags", -1, static_cast<int>(flags));
                }
            }
            ctx.set_gpr(2, flags);
        });

    runtime.register_hle("sceSasCore", 0x74AE582Au,
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            auto *voice = sas_voice(ctx.gpr[4], static_cast<std::int32_t>(ctx.gpr[5]), ctx);
            if (!voice) return;
            ctx.set_gpr(2, voice->envelope_height);
        });

    runtime.register_hle("sceSasCore", 0x07F58C24u, // sceSasGetAllEnvelopeHeights
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            if (!sas_valid_core(ctx.gpr[4])) { ctx.set_gpr(2, kSasErrorNotInitialized); return; }
            const std::uint32_t heights = ctx.gpr[5];
            if (!rt.memory().contains(heights, 32u * 4u)) {
                ctx.set_gpr(2, kSasErrorInvalidParameter); return;
            }
            for (std::size_t index = 0u; index < sas_state.voices.size(); ++index)
                rt.memory().store32(heights + static_cast<std::uint32_t>(index) * 4u,
                                    sas_state.voices[index].envelope_height);
            set_success(ctx);
        });

    runtime.register_hle("sceSasCore", 0x33D4AB37u,
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            if (!sas_valid_core(ctx.gpr[4])) { ctx.set_gpr(2, kSasErrorNotInitialized); return; }
            const std::int32_t type = static_cast<std::int32_t>(ctx.gpr[5]);
            if (type < -1 || type > 8) { ctx.set_gpr(2, kSasErrorReverbType); return; }
            if (sas_state.reverb.type != type) {
                sas_state.reverb.type = type;
                sas_state.reverb.history_left.clear();
                sas_state.reverb.history_right.clear();
                sas_state.reverb.history_cursor = 0u;
            }
            set_success(ctx);
        });

    runtime.register_hle("sceSasCore", 0x267A6DD2u,
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            if (!sas_valid_core(ctx.gpr[4])) { ctx.set_gpr(2, kSasErrorNotInitialized); return; }
            const std::int32_t delay = static_cast<std::int32_t>(ctx.gpr[5]);
            const std::int32_t feedback = static_cast<std::int32_t>(ctx.gpr[6]);
            if (delay < 0 || delay >= 128) { ctx.set_gpr(2, kSasErrorReverbDelay); return; }
            if (feedback < 0 || feedback >= 128) { ctx.set_gpr(2, kSasErrorReverbFeedback); return; }
            sas_state.reverb.delay = delay; sas_state.reverb.feedback = feedback; set_success(ctx);
        });

    runtime.register_hle("sceSasCore", 0xD5A229C9u,
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            if (!sas_valid_core(ctx.gpr[4])) { ctx.set_gpr(2, kSasErrorNotInitialized); return; }
            if (ctx.gpr[5] > 0x1000u || ctx.gpr[6] > 0x1000u) {
                ctx.set_gpr(2, kSasErrorReverbVolume); return;
            }
            sas_state.reverb.left_volume = ctx.gpr[5]; sas_state.reverb.right_volume = ctx.gpr[6]; set_success(ctx);
        });

    runtime.register_hle("sceSasCore", 0xF983B186u,
        [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
            if (!sas_valid_core(ctx.gpr[4])) { ctx.set_gpr(2, kSasErrorNotInitialized); return; }
            sas_state.reverb.dry = ctx.gpr[5] != 0u; sas_state.reverb.wet = ctx.gpr[6] != 0u; set_success(ctx);
        });

    runtime.register_hle("sceSasCore", 0xA3589D81u, // sceSasCore
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            if (!sas_valid_core(ctx.gpr[4])) { ctx.set_gpr(2, kSasErrorNotInitialized); return; }
            const std::uint32_t output = ctx.gpr[5];
            const std::size_t bytes = static_cast<std::size_t>(sas_state.grain_size) *
                (sas_state.output_mode == 0u ? 4u : 8u);
            if (!rt.memory().contains(output, bytes)) { ctx.set_gpr(2, kSasErrorInvalidParameter); return; }
            ++sas_core_mix_calls;
            ++g_sas_core_calls;
            ++g_sas_buffers_generated;
            g_sas_samples_generated += sas_state.grain_size;
            sas_log_mix_checkpoint("core", sas_core_mix_calls);
            if (sas_state.output_mode == 0u)
                sas_mix_into(rt, output, sas_state.grain_size);
            else
                sas_mix_raw(rt, output, sas_state.grain_size);
            sas_trace("sceSasCore", ctx, 0u,
                      "output=" + hex32(output) + " grain=" +
                          std::to_string(sas_state.grain_size) +
                          " mode=" + std::to_string(sas_state.output_mode));
            set_success(ctx);
        });

    runtime.register_hle("sceSasCore", 0x50A14DFCu, // sceSasCoreWithMix
        [](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
            if (!sas_valid_core(ctx.gpr[4])) { ctx.set_gpr(2, kSasErrorNotInitialized); return; }
            if (sas_state.output_mode == 1u) { ctx.set_gpr(2, 0x800001FFu); return; }
            const std::uint32_t inout = ctx.gpr[5];
            const std::size_t bytes = static_cast<std::size_t>(sas_state.grain_size) * 4u;
            if (!rt.memory().contains(inout, bytes)) { ctx.set_gpr(2, kSasErrorInvalidParameter); return; }
            const std::uint32_t input_left = ctx.gpr[6];
            const std::uint32_t input_right = ctx.gpr[7];
            if (input_left > 0x1000u || input_right > 0x1000u) {
                ctx.set_gpr(2, kSasErrorInvalidVolume); return;
            }
            ++sas_core_with_mix_calls;
            ++g_sas_core_with_mix_calls;
            ++g_sas_buffers_generated;
            g_sas_samples_generated += sas_state.grain_size;
            sas_log_mix_checkpoint("core-with-mix", sas_core_with_mix_calls);
            sas_mix_into(rt, inout, sas_state.grain_size, true, input_left, input_right);
            sas_trace("sceSasCoreWithMix", ctx, 0u,
                      "output=" + hex32(inout) + " left=" + std::to_string(input_left) +
                          " right=" + std::to_string(input_right));
            set_success(ctx);
        });
}

SasState &sas_hle_state() noexcept { return sas_state; }

std::uint32_t sas_step_envelope(SasVoiceState &voice) noexcept {
    return sas_step_envelope_impl(voice);
}

void reset_sas_hle_state() noexcept {
    sas_state = SasState{};
    sas_core_mix_calls = 0u;
    sas_core_with_mix_calls = 0u;
    g_sas_contexts = 0u;
    g_sas_core_calls = 0u;
    g_sas_core_with_mix_calls = 0u;
    g_sas_key_on_calls = 0u;
    g_sas_key_off_calls = 0u;
    g_sas_buffers_generated = 0u;
    g_sas_samples_generated = 0u;
    g_sas_voices_used = 0u;
}

void report_sas_hle_summary() {
    std::cerr << "[SAS SUMMARY]"
              << " contexts=" << g_sas_contexts
              << " voices used=" << hex32(g_sas_voices_used)
              << " core calls=" << g_sas_core_calls
              << " key-on calls=" << g_sas_key_on_calls
              << " key-off calls=" << g_sas_key_off_calls
              << " buffers generated=" << g_sas_buffers_generated
              << " samples generated=" << g_sas_samples_generated << "\n";
}

void set_sas_keyon_observer(SasKeyOnObserver observer) { g_sas_keyon_observer = observer; }
void set_sas_event_observer(SasEventObserver observer) { g_sas_event_observer = observer; }

int sas_debug_silence_all() {
    int n = 0;
    for (auto &v : sas_state.voices)
        if (v.on && v.playing && !v.suppressed) { v.suppressed = true; ++n; }
        else if (!v.on && v.playing && !v.suppressed) { v.suppressed = true; ++n; }
    return n;
}

std::uint32_t sas_last_call_ra() { return g_sas_last_ra; }

std::string sas_debug_voice_summary() {
    std::string out;
    int playing = 0;
    for (std::size_t i = 0; i < sas_state.voices.size(); ++i) {
        const SasVoiceState &v = sas_state.voices[i];
        if (!v.playing || v.paused || v.type == SasVoiceType::Off) continue;
        ++playing;
        char b[200];
        std::snprintf(b, sizeof b, " [%zu %s pitch=%X vol=%d/%d fx=%d/%d env=%u ph=%d loop=%d data=%08X]", i,
                      v.type == SasVoiceType::Vag ? "vag" : v.type == SasVoiceType::Pcm ? "pcm" : "noise", static_cast<unsigned>(v.pitch),
                      v.left_volume, v.right_volume, v.effect_left_volume, v.effect_right_volume,
                      static_cast<unsigned>(v.envelope_height >> 20), static_cast<int>(v.envelope_phase), v.loop ? 1 : 0, v.data_address);
        out += b;
        char e[200];
        std::snprintf(e, sizeof e, "{adsr=%d on=%d m=%d,%d,%d,%d r=%X,%X,%X,%X sl=%X}", v.adsr_configured ? 1 : 0, v.on ? 1 : 0,
                      static_cast<int>(v.adsr_modes[0]), static_cast<int>(v.adsr_modes[1]), static_cast<int>(v.adsr_modes[2]), static_cast<int>(v.adsr_modes[3]),
                      static_cast<unsigned>(v.adsr_rates[0]), static_cast<unsigned>(v.adsr_rates[1]), static_cast<unsigned>(v.adsr_rates[2]), static_cast<unsigned>(v.adsr_rates[3]),
                      static_cast<unsigned>(v.sustain_level));
        out += e;
    }
    char m[200];
    const double n = g_sas_meter.n ? static_cast<double>(g_sas_meter.n) : 1.0;
    std::snprintf(m, sizeof m, " busdry=%.1fdBFS buswet=%.1fdBFS reverb(type=%d delay=%d fb=%d evol=%u/%u)",
                  20.0 * std::log10(std::max(std::sqrt(g_sas_meter.dry / n), 1.0) / 32768.0),
                  20.0 * std::log10(std::max(std::sqrt(g_sas_meter.wet / n), 1.0) / 32768.0),
                  sas_state.reverb.type, sas_state.reverb.delay, sas_state.reverb.feedback,
                  sas_state.reverb.left_volume, sas_state.reverb.right_volume);
    g_sas_meter = SasMeter{};
    out = std::string(m) + out;
    return "mixed=" + std::to_string(g_sas_samples_generated) + " voices=" + std::to_string(playing) + " rev(dry=" + std::to_string(sas_state.reverb.dry ? 1 : 0) + " wet=" +
           std::to_string(sas_state.reverb.wet ? 1 : 0) + ")" + out;
}

} // namespace psprecomp
