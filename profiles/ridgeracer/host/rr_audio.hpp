#pragma once

// PCM output for the PSP's sceAudio channels.  Each channel owns a small jitter buffer; a device thread mixes the
// channels (with their volumes) and plays the result at 44.1 kHz stereo.  Windows uses waveOut; other platforms have no
// device yet (pushes are accepted and dropped).

#include <cstddef>
#include <cstdint>

namespace rr {

constexpr int kAudioChannels = 8;
constexpr std::uint32_t kAudioRate = 44100u;

// Opens the output device (once).  False when there is none; pushes are then ignored.
bool audio_start();
void audio_stop();
// How many sample frames of device latency were removed compared with the original 8 x 1024 queue; the movie path adds
// this much delay to its audio so lip-sync stays where it was tuned.
[[nodiscard]] std::uint32_t audio_latency_trim_frames();
// Queues `frames` sample frames for a channel: stereo = interleaved L,R int16, otherwise mono.  Volumes are PSP style
// (0..0x8000).  When the channel's buffer is full the oldest audio is dropped.
void audio_channel_push(int channel, const std::int16_t *pcm, std::uint32_t frames, bool stereo, int vol_left, int vol_right);
// Channels keep their backlog bounded by default (excess is skipped with a short blend); the movie channel turns this off.
void audio_channel_set_trim(int channel, bool on);
void audio_channel_volume(int channel, int vol_left, int vol_right);
[[nodiscard]] std::uint32_t audio_channel_queued(int channel);
void audio_channel_clear(int channel);
// Counters since the last call (diagnostics): underruns = a primed channel ran dry, drops = frames discarded because a
// ring was full, pushed/played = sample frames in/out, device_starved = times the sound card had run out of queued buffers (the mixer thread was too late), device_gap_max_ms = longest wait
// between two mixer wake-ups, queue_min/max = largest single-channel backlog seen by the mixer.
struct AudioStats { std::uint64_t underruns{}, drops{}, pushed{}, played{}, queue_min{}, queue_max{}, device_starved{}, device_gap_max_ms{}; };
AudioStats audio_take_stats();
// Mixes `frames` stereo frames of all channels into `out` (interleaved int16).  Called by the device thread; exposed
// for tests.
void audio_mix(std::int16_t *out, std::size_t frames);

// Player volume controls. Master is 0..1.5 (1 = the game's own level; above 1 can clip), the menu-sound gain scales only
// the keyboard/menu click channel. Changes ramp in over one buffer so they never click.
void audio_set_master_volume(float v);
[[nodiscard]] float audio_master_volume();
void audio_set_muted(bool muted);
[[nodiscard]] bool audio_muted();
void audio_set_menu_gain(float v);
[[nodiscard]] float audio_menu_gain();
void audio_volume_step(int steps);   // +/- 5 % per step; unmutes
void audio_mute_toggle();

} // namespace rr
