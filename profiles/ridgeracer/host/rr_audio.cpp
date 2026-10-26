#include "rr_audio.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#ifdef RR_AUDIO_SDL
#include <SDL.h>
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <thread>
#include "rr_log.hpp"
#endif

namespace rr {
namespace {

constexpr std::size_t kRingFrames = 65536;      // per channel (~1.5 s; the movie keeps ~0.5 s queued on purpose)
constexpr std::size_t kPrimeBase = 3072;        // a channel starts sounding once this much is queued (absorbs scheduling jitter)
constexpr std::size_t kPrimeMax = 8192;         // ... and the amount grows after each underrun, then shrinks back slowly
constexpr std::size_t kIdleNewStream = 13230;   // silence longer than ~300 ms makes the next push a new stream (see the hold in audio_channel_push)

struct Channel {
    std::vector<std::int16_t> ring = std::vector<std::int16_t>(kRingFrames * 2u);   // stereo frames
    std::size_t head = 0, count = 0;   // frames
    std::vector<std::uint16_t> vols = std::vector<std::uint16_t>(kRingFrames * 2u, 0x8000u);   // volume each frame was queued with
    bool primed = false;
    bool trim = true;                  // keep the backlog (latency) bounded; off for the movie, whose lip-sync depends on it
    bool splice = false;               // frames were just skipped: blend from the last sample played
    int vol_left = 0x8000, vol_right = 0x8000;
    int last_l = 0, last_r = 0;        // last (volume scaled) sample this channel contributed
    int off_l = 0, off_r = 0, off_n = 0;
    int fade_in = 0;                   // frames of fade-in left after (re)starting
    int tail_l = 0, tail_r = 0, tail_n = 0;   // decaying continuation of the last sample after the channel ran dry
    bool holding = false;              // a freshly started stream waits for its second chunk (see audio_channel_push)
    std::size_t hold_start = 0, hold_len = 0, hold_age = 0;
    std::uint16_t hold_vl = 0, hold_vr = 0;
    std::size_t prime = kPrimeBase, quiet = 0, idle = 1u << 30;
    double frac = 0.0;                 // read position between two queued frames (drift correction, see audio_mix)
    double avg_count = 0.0;            // smoothed backlog the drift correction steers
    // Time stretch (see stretch_render): the game makes sound slower than real time while the frame rate is below target.
    bool stretching = false;
    double st_pos = 0.0;               // nominal read position (frames after head) of the next segment
    std::size_t st_nat = 0;            // where the previous segment naturally continues (frames after head)
    std::vector<float> st_tail = std::vector<float>(1024u, 0.0f);   // second half of the previous segment (stereo, kStretchHop frames)
    std::vector<float> st_out;         // stretched frames made but not played yet (stereo)
    std::size_t st_out_at = 0;
};
constexpr int kEdgeFrames = 128;      // fade-in length when a channel (re)starts
constexpr int kTailFrames = 661;      // when a channel runs dry its last sample decays to zero over ~15 ms instead of stepping
constexpr std::size_t kHoldMaxFrames = 4410;   // a started stream waits at most ~100 ms for its second chunk
constexpr std::size_t kTrimHigh = 8192;   // backlog above this (~186 ms) is cut back to kTrimTarget
constexpr std::size_t kTrimTarget = 3072;
constexpr int kSpliceFrames = 192;
// Time stretch: segments of 2 x kStretchHop frames overlap by half; each new segment starts within +-kStretchSearch frames of
// its nominal place, at the offset where it best matches how the previous one would have continued (WSOLA), so the sound
// slows down without changing pitch and without gaps.
constexpr std::size_t kStretchHop = 384;
constexpr std::size_t kStretchSearch = 192;
constexpr double kStretchMin = 0.80;      // slowest playback (the game itself is then running at 80 % speed)

std::mutex g_mutex;
Channel g_channels[kAudioChannels];
bool g_started = false;
AudioStats g_stats;
std::FILE *g_dump = nullptr;           // RR_AUDIO_DUMP=<file.wav>: everything the mixer produced (first ~120 s)
std::uint64_t g_dump_frames = 0;
std::FILE *g_chdump[kAudioChannels] = {};   // raw stereo int16 of what each channel was handed (before volume)
std::uint64_t g_chdump_frames[kAudioChannels] = {};
constexpr std::uint64_t kDumpMax = 44100ull * 120ull;

void dump_open() {
    const char *path = std::getenv("RR_AUDIO_DUMP");
    if (path == nullptr || *path == 0) return;
    g_dump = std::fopen(path, "wb");
    if (g_dump != nullptr) { unsigned char hdr[44] = {}; std::fwrite(hdr, 1, sizeof hdr, g_dump); }
    g_dump_frames = 0;
    for (int i = 0; i < kAudioChannels; ++i) {
        g_chdump_frames[i] = 0;
        g_chdump[i] = std::fopen((std::string(path) + ".ch" + std::to_string(i) + ".raw").c_str(), "wb");
    }
}
void dump_close() {
    for (int i = 0; i < kAudioChannels; ++i)
        if (g_chdump[i] != nullptr) { std::fclose(g_chdump[i]); g_chdump[i] = nullptr; }
    if (g_dump == nullptr) return;
    const std::uint32_t data = static_cast<std::uint32_t>(g_dump_frames * 4u), riff = data + 36u, rate = 44100u, brate = 44100u * 4u, fmt = 16u;
    const std::uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
    std::fseek(g_dump, 0, SEEK_SET);
    std::fwrite("RIFF", 1, 4, g_dump); std::fwrite(&riff, 4, 1, g_dump); std::fwrite("WAVEfmt ", 1, 8, g_dump);
    std::fwrite(&fmt, 4, 1, g_dump); std::fwrite(&pcm, 2, 1, g_dump); std::fwrite(&ch, 2, 1, g_dump);
    std::fwrite(&rate, 4, 1, g_dump); std::fwrite(&brate, 4, 1, g_dump); std::fwrite(&align, 2, 1, g_dump);
    std::fwrite(&bits, 2, 1, g_dump); std::fwrite("data", 1, 4, g_dump); std::fwrite(&data, 4, 1, g_dump);
    std::fclose(g_dump);
    g_dump = nullptr;
}
bool g_stats_fresh = true;

} // namespace

void audio_channel_push(int channel, const std::int16_t *pcm, std::uint32_t frames, bool stereo, int vol_left, int vol_right) {
    if (channel < 0 || channel >= kAudioChannels || pcm == nullptr || frames == 0u) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    Channel &c = g_channels[channel];
    c.vol_left = vol_left; c.vol_right = vol_right;
    const std::uint16_t vl = static_cast<std::uint16_t>(std::clamp(vol_left, 0, 0xFFFF)), vr = static_cast<std::uint16_t>(std::clamp(vol_right, 0, 0xFFFF));
    g_stats.pushed += frames;
    if (g_chdump[channel] != nullptr && g_chdump_frames[channel] < kDumpMax) {
        for (std::uint32_t i = 0; i < frames; ++i) {
            const std::int16_t lr[2] = {stereo ? pcm[i * 2u] : pcm[i], stereo ? pcm[i * 2u + 1u] : pcm[i]};
            std::fwrite(lr, 2, 2, g_chdump[channel]);
        }
        g_chdump_frames[channel] += frames;
    }
    // A stream that starts from silence often gets its volume wrong on the very first chunk (the game sets the real level
    // a moment later).  Hold the start until the second chunk arrives and give the first chunk the lower of the two
    // volumes, so a start never blasts.
    const bool fresh = c.trim && !c.primed && c.count == 0 && c.idle >= kIdleNewStream;
    if (c.holding && !fresh) {
        const std::uint16_t ml = std::min(c.hold_vl, vl), mr = std::min(c.hold_vr, vr);
        for (std::size_t k = 0; k < c.hold_len; ++k) {
            const std::size_t at = (c.hold_start + k) % kRingFrames;
            c.vols[at * 2u] = std::min(c.vols[at * 2u], ml);
            c.vols[at * 2u + 1u] = std::min(c.vols[at * 2u + 1u], mr);
        }
        c.holding = false;
    }
    if (fresh) { c.holding = true; c.hold_start = (c.head + c.count) % kRingFrames; c.hold_len = frames; c.hold_age = 0; c.hold_vl = vl; c.hold_vr = vr; }
    for (std::uint32_t i = 0; i < frames; ++i) {
        if (c.count == kRingFrames) { c.head = (c.head + 1) % kRingFrames; --c.count; ++g_stats.drops; }   // drop the oldest
        const std::size_t at = (c.head + c.count) % kRingFrames;
        const std::int16_t l = stereo ? pcm[i * 2u] : pcm[i];
        const std::int16_t r = stereo ? pcm[i * 2u + 1u] : pcm[i];
        c.ring[at * 2u] = l; c.ring[at * 2u + 1u] = r;
        c.vols[at * 2u] = vl; c.vols[at * 2u + 1u] = vr;
        ++c.count;
    }
    // The trim limits must stay above the adaptive prime level: with the prime raised to 8192 by a few underruns the old
    // fixed limits kept cutting the queue back to 3072 before it could ever reach the prime, so the channel never
    // started sounding again (permanent silence).
    const std::size_t trim_high = std::max(kTrimHigh, c.prime + 3072u), trim_target = std::max(kTrimTarget, c.prime + 512u);
    if (c.trim && c.count > trim_high) {
        // The game ran ahead of the sound card (a burst after a load, say) and the audio would stay late for good:
        // skip the oldest frames and blend over the join.
        const std::size_t drop = c.count - trim_target;
        c.head = (c.head + drop) % kRingFrames;
        c.count -= drop;
        g_stats.drops += drop;
        c.splice = true;
    }
}

std::atomic<bool> g_stretch_on{true};
void audio_set_time_stretch(bool on) { g_stretch_on = on; }

void audio_channel_set_trim(int channel, bool on) {
    if (channel < 0 || channel >= kAudioChannels) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_channels[channel].trim = on;
}

void audio_channel_volume(int channel, int vol_left, int vol_right) {
    if (channel < 0 || channel >= kAudioChannels) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_channels[channel].vol_left = vol_left;
    g_channels[channel].vol_right = vol_right;
}

std::uint32_t audio_channel_queued(int channel) {
    if (channel < 0 || channel >= kAudioChannels) return 0u;
    std::lock_guard<std::mutex> lock(g_mutex);
    return static_cast<std::uint32_t>(g_channels[channel].count);
}

void audio_channel_clear(int channel) {
    if (channel < 0 || channel >= kAudioChannels) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_channels[channel].head = g_channels[channel].count = 0;
    g_channels[channel].primed = false;
    g_channels[channel].frac = 0.0;
    g_channels[channel].splice = false;
    g_channels[channel].off_n = 0;
    g_channels[channel].holding = false;
    g_channels[channel].tail_n = 0;
    g_channels[channel].stretching = false;
    g_channels[channel].st_out.clear();
    g_channels[channel].st_out_at = 0;
}

AudioStats audio_take_stats() {
    std::lock_guard<std::mutex> lock(g_mutex);
    AudioStats out = g_stats;
    g_stats = AudioStats{};
    g_stats_fresh = true;
    return out;
}

namespace {
std::atomic<float> g_master{1.0f}, g_menu_gain{1.0f};
std::atomic<bool> g_muted{false};
float g_applied_gain = 1.0f;   // gain at the end of the previous buffer (mixer thread only)
constexpr int kMenuMixChannel = 7;
}
void audio_set_master_volume(float v) { g_master = std::clamp(v, 0.0f, 1.5f); }
float audio_master_volume() { return g_master; }
void audio_set_muted(bool m) { g_muted = m; }
bool audio_muted() { return g_muted; }
void audio_set_menu_gain(float v) { g_menu_gain = std::clamp(v, 0.0f, 2.0f); }
float audio_menu_gain() { return g_menu_gain; }
void audio_volume_step(int steps) {
    const float v = std::round(g_master * 20.0f + static_cast<float>(steps)) / 20.0f;
    g_master = std::clamp(v, 0.0f, 1.5f);
    g_muted = false;
}
void audio_mute_toggle() { g_muted = !g_muted; }

namespace {
// Volume-scaled sample of frame `i` after the channel's head (k = 0 left, 1 right).
inline float ring_sample(const Channel &c, std::size_t i, int k) {
    const std::size_t at = ((c.head + i) % kRingFrames) * 2u + static_cast<std::size_t>(k);
    return static_cast<float>((c.ring[at] * static_cast<int>(c.vols[at])) >> 15);
}

// Starts stretching at the channel's current read position: the first hop is played as it is.
void stretch_begin(Channel &c) {
    c.stretching = true;
    c.st_out.clear(); c.st_out_at = 0;
    for (std::size_t i = 0; i < kStretchHop; ++i)
        for (int k = 0; k < 2; ++k) {
            c.st_out.push_back(ring_sample(c, i, k));
            c.st_tail[i * 2u + static_cast<std::size_t>(k)] = ring_sample(c, kStretchHop + i, k);
        }
    c.st_nat = kStretchHop;
    c.st_pos = static_cast<double>(kStretchHop);
}

// Makes one more hop of stretched output (consuming `rate` x kStretchHop input frames on average).  False when the queue
// does not hold enough audio for the search and the next segment.
bool stretch_hop(Channel &c, double rate) {
    const std::size_t need = static_cast<std::size_t>(c.st_pos) + kStretchSearch + 2u * kStretchHop + 1u;
    if (c.count < need || c.st_nat + 2u * kStretchHop > c.count) return false;
    // Best start near the nominal position: highest correlation (mono, every 2nd frame) with the natural continuation.
    const long nominal = static_cast<long>(c.st_pos);
    const long lo = std::max(0L, nominal - static_cast<long>(kStretchSearch)), hi = nominal + static_cast<long>(kStretchSearch);
    static thread_local std::vector<float> ref, cand;
    ref.resize(kStretchHop / 2u);
    for (std::size_t i = 0; i < kStretchHop / 2u; ++i) ref[i] = ring_sample(c, c.st_nat + i * 2u, 0) + ring_sample(c, c.st_nat + i * 2u, 1);
    const std::size_t span = static_cast<std::size_t>(hi - lo) + kStretchHop;
    cand.resize(span);
    for (std::size_t i = 0; i < span; ++i) cand[i] = ring_sample(c, static_cast<std::size_t>(lo) + i, 0) + ring_sample(c, static_cast<std::size_t>(lo) + i, 1);
    long best = nominal; double best_score = -1e300;
    for (long s = lo; s <= hi; ++s) {
        const float *x = &cand[static_cast<std::size_t>(s - lo)];
        double xy = 0.0, xx = 1e-9;
        for (std::size_t i = 0; i < kStretchHop / 2u; ++i) { const float v = x[i * 2u]; xy += static_cast<double>(v) * ref[i]; xx += static_cast<double>(v) * v; }
        const double score = xy / std::sqrt(xx);
        if (score > best_score) { best_score = score; best = s; }
    }
    const std::size_t s0 = static_cast<std::size_t>(best);
    // Cross-fade the previous segment's second half into this segment's first half.
    for (std::size_t i = 0; i < kStretchHop; ++i) {
        const float w = 0.5f - 0.5f * std::cos(3.14159265f * (static_cast<float>(i) + 0.5f) / static_cast<float>(kStretchHop));
        for (int k = 0; k < 2; ++k) {
            const std::size_t j = i * 2u + static_cast<std::size_t>(k);
            c.st_out.push_back(c.st_tail[j] * (1.0f - w) + ring_sample(c, s0 + i, k) * w);
            c.st_tail[j] = ring_sample(c, s0 + kStretchHop + i, k);
        }
    }
    c.st_nat = s0 + kStretchHop;
    c.st_pos += static_cast<double>(kStretchHop) * rate;
    // Drop what no later segment can reach.
    const double keep = std::min(c.st_pos - static_cast<double>(kStretchSearch), static_cast<double>(c.st_nat));
    if (keep >= 1.0) {
        const std::size_t a = static_cast<std::size_t>(keep);
        c.head = (c.head + a) % kRingFrames; c.count -= a;
        c.st_pos -= static_cast<double>(a); c.st_nat -= a;
        g_stats.played += a;
    }
    return true;
}

// Stops stretching: the queue continues where the last segment ended (st_nat), so the join is seamless once the
// frames already made have been played.
void stretch_end(Channel &c) {
    c.stretching = false;
    const std::size_t a = std::min(c.st_nat, c.count);
    c.head = (c.head + a) % kRingFrames; c.count -= a;
    g_stats.played += a;
    c.st_nat = 0; c.st_pos = 0.0;
    c.frac = 0.0;
}
} // namespace

void audio_mix(std::int16_t *out, std::size_t frames) {
    std::vector<std::int32_t> acc(frames * 2u, 0);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (Channel &c : g_channels) {
            if (c.holding && (c.hold_age += frames) > kHoldMaxFrames) c.holding = false;
            const auto render_tail = [&](std::size_t from) {
                for (std::size_t i = from; i < frames && c.tail_n > 0; ++i, --c.tail_n) {
                    acc[i * 2u] += c.tail_l * c.tail_n / kTailFrames;
                    acc[i * 2u + 1u] += c.tail_r * c.tail_n / kTailFrames;
                }
            };
            if (!c.primed && !c.holding && c.count >= c.prime) { c.primed = true; c.fade_in = kEdgeFrames; c.tail_n = 0; c.frac = 0.0; c.avg_count = static_cast<double>(c.count); }
            if (!c.primed) { if (c.count == 0) c.idle += frames; render_tail(0); continue; }
            c.idle = 0;
            if ((c.quiet += frames) > 441000u) { c.quiet = 0; c.prime = std::max(kPrimeBase, c.prime - 512u); }
            if (g_stats_fresh) { g_stats.queue_min = g_stats.queue_max = c.count; g_stats_fresh = false; }
            g_stats.queue_min = std::min<std::uint64_t>(g_stats.queue_min, c.count);
            g_stats.queue_max = std::max<std::uint64_t>(g_stats.queue_max, c.count);
            // Drift correction: the game produces sound a hair slower (or faster) than the sound card consumes it, which slowly
            // drains (or fills) the queue until it runs dry and the sound drops out for a moment.  Steer the backlog toward a
            // target by reading the queue a fraction of a percent slower or faster (linear interpolation, inaudible).
            double rate = 1.0;
            if (c.trim) {
                c.avg_count += (static_cast<double>(c.count) - c.avg_count) * 0.02;
                const double target = static_cast<double>(c.prime) + 768.0;
                rate = std::clamp(1.0 + (c.avg_count - target) * 4e-6, 0.996, 1.004);
                if (std::fabs(rate - 1.0) < 0.0002) rate = 1.0;
            } else c.avg_count = static_cast<double>(c.count);
            {
                // Time stretch when the queue keeps running low (the game is producing sound slower than real time because the
                // frame rate dropped): play slower without changing pitch instead of running dry and going silent.
                static const bool stretch_off = std::getenv("RR_AUDIO_STRETCH_OFF") != nullptr;
                const double target = static_cast<double>(c.prime) + 768.0;
                const double srate = std::clamp(1.0 + (c.avg_count - target) * 8e-5, kStretchMin, 1.0);
                if (!stretch_off && c.trim && (g_stretch_on.load(std::memory_order_relaxed) || c.stretching)) {
                    if (!c.stretching && g_stretch_on.load(std::memory_order_relaxed) && srate < 0.99 && c.count >= 2u * kStretchHop + kStretchSearch + 2u) { stretch_begin(c); ++g_stats.stretch_starts; }
                    std::size_t done = 0;
                    if (c.stretching) {
                        const bool leave = srate > 0.997 || !g_stretch_on.load(std::memory_order_relaxed);
                        while (done < frames) {
                            if (c.st_out_at >= c.st_out.size()) {
                                c.st_out.clear(); c.st_out_at = 0;
                                if (leave) break;
                                if (!stretch_hop(c, srate)) break;
                            }
                            const std::size_t take = std::min(frames - done, (c.st_out.size() - c.st_out_at) / 2u);
                            for (std::size_t i = 0; i < take; ++i) {
                                int l = static_cast<int>(c.st_out[c.st_out_at + i * 2u]), r = static_cast<int>(c.st_out[c.st_out_at + i * 2u + 1u]);
                                if (c.fade_in > 0) { l = l * (kEdgeFrames - c.fade_in) / kEdgeFrames; r = r * (kEdgeFrames - c.fade_in) / kEdgeFrames; --c.fade_in; }
                                c.last_l = l; c.last_r = r;
                                if (&c == &g_channels[kMenuMixChannel]) {
                                    const float mg = g_menu_gain.load(std::memory_order_relaxed);
                                    l = static_cast<int>(static_cast<float>(l) * mg); r = static_cast<int>(static_cast<float>(r) * mg);
                                }
                                acc[(done + i) * 2u] += l; acc[(done + i) * 2u + 1u] += r;
                            }
                            c.st_out_at += take * 2u;
                            done += take;
                            g_stats.stretched += take;
                        }
                        if (done < frames) {
                            stretch_end(c);
                            if (!leave) {   // not enough audio queued even for stretching: the old behaviour (fade out, refill)
                                c.primed = false;
                                ++g_stats.underruns;
                                c.prime = std::min(kPrimeMax, c.prime + 1024u);
                                c.quiet = 0;
                                c.tail_l = c.last_l; c.tail_r = c.last_r; c.tail_n = kTailFrames;
                                render_tail(done);
                                continue;
                            }
                            c.splice = false;
                        } else continue;
                    }
                    if (done != 0u) {
                        // Finished stretching part-way through this buffer: play the rest normally from where the stretch ended.
                        const std::size_t n2 = std::min(frames - done, c.count);
                        for (std::size_t i = 0; i < n2; ++i) {
                            const int l = static_cast<int>(ring_sample(c, i, 0)), r = static_cast<int>(ring_sample(c, i, 1));
                            c.last_l = l; c.last_r = r;
                            acc[(done + i) * 2u] += l; acc[(done + i) * 2u + 1u] += r;
                        }
                        c.head = (c.head + n2) % kRingFrames; c.count -= n2; g_stats.played += n2;
                        if (done + n2 < frames) { c.primed = false; ++g_stats.underruns; c.tail_l = c.last_l; c.tail_r = c.last_r; c.tail_n = kTailFrames; render_tail(done + n2); }
                        continue;
                    }
                }
            }
            const bool interp = rate != 1.0 || c.frac > 0.0;
            std::size_t n = std::min(frames, c.count);
            double pos = c.frac;
            if (interp) {
                // output frames available while the next queued frame exists for the blend
                std::size_t m = 0; double q = pos;
                while (m < frames && static_cast<std::size_t>(q) + 1u < c.count) { q += rate; ++m; }
                n = m;
            }
            g_stats.played += n;
            for (std::size_t i = 0; i < n; ++i) {
                const std::size_t i0 = interp ? static_cast<std::size_t>(pos) : i;
                const double fr = interp ? pos - static_cast<double>(i0) : 0.0;
                const std::size_t at = (c.head + i0) % kRingFrames;
                int l = (c.ring[at * 2u] * static_cast<int>(c.vols[at * 2u])) >> 15;
                int r = (c.ring[at * 2u + 1u] * static_cast<int>(c.vols[at * 2u + 1u])) >> 15;
                if (interp) {
                    const std::size_t at2 = (c.head + i0 + 1u) % kRingFrames;
                    const int l2 = (c.ring[at2 * 2u] * static_cast<int>(c.vols[at2 * 2u])) >> 15;
                    const int r2 = (c.ring[at2 * 2u + 1u] * static_cast<int>(c.vols[at2 * 2u + 1u])) >> 15;
                    l += static_cast<int>(static_cast<double>(l2 - l) * fr); r += static_cast<int>(static_cast<double>(r2 - r) * fr);
                    pos += rate;
                }
                if (c.splice) {   // first frame after a skip: start the blend from where the channel left off
                    c.splice = false;
                    c.off_l = c.last_l - l; c.off_r = c.last_r - r; c.off_n = kSpliceFrames;
                }
                if (c.off_n > 0) {
                    l += c.off_l * c.off_n / kSpliceFrames; r += c.off_r * c.off_n / kSpliceFrames;
                    --c.off_n;
                }
                if (c.fade_in > 0) { l = l * (kEdgeFrames - c.fade_in) / kEdgeFrames; r = r * (kEdgeFrames - c.fade_in) / kEdgeFrames; --c.fade_in; }
                c.last_l = l; c.last_r = r;
                if (&c == &g_channels[kMenuMixChannel]) {
                    const float mg = g_menu_gain.load(std::memory_order_relaxed);
                    l = static_cast<int>(static_cast<float>(l) * mg); r = static_cast<int>(static_cast<float>(r) * mg);
                }
                acc[i * 2u] += l;
                acc[i * 2u + 1u] += r;
            }
            if (interp) {
                const std::size_t used = static_cast<std::size_t>(pos);
                c.head = (c.head + used) % kRingFrames;
                c.count -= used;
                c.frac = pos - static_cast<double>(used);
            } else {
                c.head = (c.head + n) % kRingFrames;
                c.count -= n;
            }
            if (n < frames) {
                c.frac = 0.0;   // ran dry: refill before sounding again, and let the last sample die away instead of cutting it
                c.primed = false;
                ++g_stats.underruns;
                c.prime = std::min(kPrimeMax, c.prime + 1024u);   // be more patient next time
                c.quiet = 0;
                c.tail_l = c.last_l; c.tail_r = c.last_r; c.tail_n = kTailFrames;
                render_tail(n);
            }
        }
    }
    const float target = g_muted.load(std::memory_order_relaxed) ? 0.0f : g_master.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < frames; ++i) {
        const float g = g_applied_gain + (target - g_applied_gain) * static_cast<float>(i + 1u) / static_cast<float>(frames);
        for (std::size_t k = 0; k < 2u; ++k) {
            const std::int32_t v = g == 1.0f ? acc[i * 2u + k] : static_cast<std::int32_t>(static_cast<float>(acc[i * 2u + k]) * g);
            out[i * 2u + k] = static_cast<std::int16_t>(std::clamp(v, -32768, 32767));
        }
    }
    g_applied_gain = target;
    if (g_dump != nullptr && g_dump_frames < kDumpMax) {
        std::fwrite(out, 4, frames, g_dump);
        g_dump_frames += frames;
    }
}

#ifdef _WIN32
namespace {
// Device queue: kBuffers * kBufferFrames frames are always in flight, so this is the latency every sound pays on
// top of the mixer rings (the old 8 x 1024 = 186 ms made menu sounds noticeably late).
constexpr int kBuffers = 6;
constexpr std::size_t kBufferFrames = 512;
HWAVEOUT g_wave = nullptr;
HANDLE g_event = nullptr;
WAVEHDR g_headers[kBuffers];
std::int16_t g_buffers[kBuffers][kBufferFrames * 2u];
std::thread g_thread;
std::atomic<bool> g_run{false};

void device_loop() {
    // The mixer must never be starved by the (CPU hungry) emulation threads, or the sound card runs dry.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    LARGE_INTEGER freq, last;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    while (g_run.load()) {
        WaitForSingleObject(g_event, 20);
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const auto gap_ms = static_cast<std::uint64_t>((now.QuadPart - last.QuadPart) * 1000 / freq.QuadPart);
        last = now;
        int pending = 0;
        for (int i = 0; i < kBuffers; ++i)
            if (!(g_headers[i].dwFlags & WHDR_DONE) && (g_headers[i].dwFlags & WHDR_PREPARED)) ++pending;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_stats.device_gap_max_ms = std::max(g_stats.device_gap_max_ms, gap_ms);
            if (pending == 0) ++g_stats.device_starved;
        }
        for (int i = 0; i < kBuffers; ++i) {
            WAVEHDR &h = g_headers[i];
            if (!(h.dwFlags & WHDR_DONE) && (h.dwFlags & WHDR_PREPARED)) continue;
            audio_mix(g_buffers[i], kBufferFrames);
            h.dwFlags &= ~static_cast<DWORD>(WHDR_DONE);
            waveOutWrite(g_wave, &h, sizeof h);
        }
    }
}
} // namespace

// ---- WASAPI (shared mode, event driven): ~20-30 ms instead of the 150+ ms the legacy waveOut layer costs ----
std::atomic<int> g_wasapi_state{0};          // 0 starting, 1 running, 2 failed
std::atomic<std::uint32_t> g_wasapi_latency{0};   // frames between the mixer and the speakers
bool g_use_wasapi = false;
constexpr std::uint32_t kLegacyLatencyFrames = 8u * 1024u + 1764u;   // the original waveOut setup (8 x 1024 plus ~40 ms of MME overhead)

template <class T> void release(T *&p) { if (p != nullptr) { p->Release(); p = nullptr; } }

void wasapi_loop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (int session = 0; session < 50; ++session) {
    IMMDeviceEnumerator *en = nullptr;
    IMMDevice *dev = nullptr;
    IAudioClient *client = nullptr;
    IAudioRenderClient *render = nullptr;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT32 frames = 0;
    bool started = false;
    HRESULT lost = S_OK;
    const auto fail = [&](const char *what, HRESULT hr) {
        log_line(category::kHle, std::string("audio: WASAPI ") + what + " failed (0x" + [&] { char b[16]; std::snprintf(b, sizeof b, "%08lX", static_cast<unsigned long>(hr)); return std::string(b); }() + "), using waveOut");
        g_wasapi_state = 2;
    };
    do {
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&en));
        if (FAILED(hr)) { fail("enumerator", hr); break; }
        hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
        if (FAILED(hr)) { fail("default device", hr); break; }
        hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&client));
        if (FAILED(hr)) { fail("activate", hr); break; }
        WAVEFORMATEX fmt{};
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = 2;
        fmt.nSamplesPerSec = kAudioRate;
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = 4;
        fmt.nAvgBytesPerSec = kAudioRate * 4u;
        REFERENCE_TIME def = 0, minimum = 0;
        client->GetDevicePeriod(&def, &minimum);
        if (def <= 0) def = 100000;   // 10 ms
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                def * 2, 0, &fmt, nullptr);
        if (FAILED(hr)) { fail("initialize", hr); break; }
        hr = client->SetEventHandle(ev);
        if (FAILED(hr)) { fail("event handle", hr); break; }
        hr = client->GetBufferSize(&frames);
        if (FAILED(hr)) { fail("buffer size", hr); break; }
        hr = client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void **>(&render));
        if (FAILED(hr)) { fail("render client", hr); break; }
        BYTE *data = nullptr;
        if (SUCCEEDED(render->GetBuffer(frames, &data))) render->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT);
        hr = client->Start();
        if (FAILED(hr)) { fail("start", hr); break; }
        started = true;
        const auto period_frames = static_cast<std::uint32_t>(def * kAudioRate / 10000000);
        g_wasapi_latency = frames + period_frames;
        log_line(category::kHle, "audio: WASAPI shared mode, buffer " + std::to_string(frames) + " frames, period " + std::to_string(period_frames) + " frames");
        g_wasapi_state = 1;

        LARGE_INTEGER freq, last;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&last);
        std::uint64_t stall_ms = 0;
        UINT32 last_padding = 0xFFFFFFFFu;
        while (g_run.load()) {
            WaitForSingleObject(ev, 100);
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            const auto gap_ms = static_cast<std::uint64_t>((now.QuadPart - last.QuadPart) * 1000 / freq.QuadPart);
            last = now;
            UINT32 padding = 0;
            if (const HRESULT hrp = client->GetCurrentPadding(&padding); FAILED(hrp)) { lost = hrp; break; }
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_stats.device_gap_max_ms = std::max(g_stats.device_gap_max_ms, gap_ms);
                if (padding == 0) ++g_stats.device_starved;
            }
            const UINT32 avail = frames - padding;
            // Watchdog: a device buffer that stays completely full and never drains means the engine stopped
            // consuming (device switch, driver hiccup). Restart the session instead of staying silent forever.
            if (avail == 0 && padding == last_padding) stall_ms += gap_ms; else stall_ms = 0;
            last_padding = padding;
            if (stall_ms > 400) {
                lost = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                log_line(category::kHle, "audio: WASAPI stalled (device buffer not draining)");
                break;
            }
            if (avail == 0) continue;
            if (const HRESULT hrb = render->GetBuffer(avail, &data); FAILED(hrb) || data == nullptr) { lost = FAILED(hrb) ? hrb : E_POINTER; break; }
            audio_mix(reinterpret_cast<std::int16_t *>(data), avail);
            render->ReleaseBuffer(avail, 0);
        }
    } while (false);
    if (started) client->Stop();
    release(render);
    release(client);
    release(dev);
    release(en);
    if (ev != nullptr) CloseHandle(ev);
    // The device went away mid-run (format change, device switch, driver reset): start over on the current default
    // device instead of staying silent for the rest of the session.
    if (started && g_run.load() && FAILED(lost)) {
        log_line(category::kHle, "audio: WASAPI device lost (0x" + [&] { char b[16]; std::snprintf(b, sizeof b, "%08lX", static_cast<unsigned long>(lost)); return std::string(b); }() + "), restarting");
        Sleep(200);
        continue;
    }
    break;
    }
    if (SUCCEEDED(com)) CoUninitialize();
    if (g_wasapi_state.load() == 0) g_wasapi_state = 2;   // left the loop without ever running
}

bool wasapi_start() {
    g_wasapi_state = 0;
    g_run = true;
    g_thread = std::thread(wasapi_loop);
    for (int i = 0; i < 300 && g_wasapi_state.load() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (g_wasapi_state.load() == 1) return true;
    g_run = false;
    if (g_thread.joinable()) g_thread.join();
    return false;
}

// ---- waveOut fallback ----
bool mme_start() {
    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = kAudioRate;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = kAudioRate * 4u;
    g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (waveOutOpen(&g_wave, WAVE_MAPPER, &fmt, reinterpret_cast<DWORD_PTR>(g_event), 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        g_wave = nullptr;
        return false;
    }
    for (int i = 0; i < kBuffers; ++i) {
        std::memset(&g_headers[i], 0, sizeof g_headers[i]);
        std::memset(g_buffers[i], 0, sizeof g_buffers[i]);
        g_headers[i].lpData = reinterpret_cast<LPSTR>(g_buffers[i]);
        g_headers[i].dwBufferLength = static_cast<DWORD>(sizeof g_buffers[i]);
        waveOutPrepareHeader(g_wave, &g_headers[i], sizeof g_headers[i]);
        g_headers[i].dwFlags |= WHDR_DONE;   // start every buffer through the fill path
    }
    g_run = true;
    g_thread = std::thread(device_loop);
    return true;
}

std::uint32_t audio_latency_trim_frames() {
    // The movie's picture was timed against the original output latency; the movie path delays its audio by whatever
    // this setup saves.  RR_MOVIE_AUDIO_DELAY_MS overrides the amount (for tuning lip-sync).
    if (const char *e = std::getenv("RR_MOVIE_AUDIO_DELAY_MS")) return static_cast<std::uint32_t>(std::max(0, std::atoi(e))) * kAudioRate / 1000u;
    const std::uint32_t now = g_use_wasapi ? g_wasapi_latency.load() : static_cast<std::uint32_t>(kBuffers * kBufferFrames) + 1764u;
    return kLegacyLatencyFrames > now ? kLegacyLatencyFrames - now : 0u;
}

bool audio_start() {
    if (g_started) return g_use_wasapi || g_wave != nullptr;
    g_started = true;
    timeBeginPeriod(1);
    dump_open();
    if (std::getenv("RR_AUDIO_MME") == nullptr && wasapi_start()) { g_use_wasapi = true; return true; }
    g_use_wasapi = false;
    return mme_start();
}

void audio_stop() {
    if (!g_started) return;
    g_run = false;
    if (g_thread.joinable()) g_thread.join();
    if (g_wave != nullptr) {
        waveOutReset(g_wave);
        for (int i = 0; i < kBuffers; ++i) waveOutUnprepareHeader(g_wave, &g_headers[i], sizeof g_headers[i]);
        waveOutClose(g_wave);
        g_wave = nullptr;
    }
    if (g_event != nullptr) { CloseHandle(g_event); g_event = nullptr; }
    g_use_wasapi = false;
    g_started = false;
    dump_close();
    timeEndPeriod(1);
}
#elif defined(RR_AUDIO_SDL)
namespace {
SDL_AudioDeviceID g_sdl_dev = 0;
constexpr Uint16 kSdlBufferFrames = 512;

void SDLCALL sdl_audio_callback(void *, Uint8 *stream, int len) {
    audio_mix(reinterpret_cast<std::int16_t *>(stream), static_cast<std::size_t>(len) / 4u);   // stereo int16 = 4 bytes per frame
}
} // namespace

std::uint32_t audio_latency_trim_frames() { return 0u; }

bool audio_start() {
    if (g_started) return g_sdl_dev != 0;
    g_started = true;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return false;
    SDL_AudioSpec want{}, have{};
    want.freq = static_cast<int>(kAudioRate);
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = kSdlBufferFrames;
    want.callback = sdl_audio_callback;
    g_sdl_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);   // 0 = no format changes: SDL converts for the device
    if (g_sdl_dev == 0) return false;
    SDL_PauseAudioDevice(g_sdl_dev, 0);
    return true;
}

void audio_stop() {
    if (g_sdl_dev != 0) { SDL_CloseAudioDevice(g_sdl_dev); g_sdl_dev = 0; }
    g_started = false;
}
#elif defined(__EMSCRIPTEN__)
// Browser: the engine thread mixes into a shared ring that an AudioWorklet plays (rr_present_web.cpp pumps it once per frame).
std::uint32_t audio_latency_trim_frames() { return 0u; }
bool audio_start() { g_started = true; return true; }
void audio_stop() { g_started = false; }
#else
std::uint32_t audio_latency_trim_frames() { return 0u; }
bool audio_start() { g_started = true; return false; }
void audio_stop() { g_started = false; }
#endif

} // namespace rr
