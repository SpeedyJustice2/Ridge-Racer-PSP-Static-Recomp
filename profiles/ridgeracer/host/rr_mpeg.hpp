#pragma once

#include <cstdint>
#include <string>

namespace psprecomp { class Runtime; }

namespace rr {
// sceMpeg: a faithful *protocol* stand-in (ring buffer accounting, stream
// registration, access-unit bookkeeping, end of stream).  The video is decoded
// for real (PMF demux + the platform's H.264 decoder) when the movie file is
// known and a decoder exists, otherwise it falls back to black frames; audio is
// still silence.
void install_mpeg_hle(psprecomp::Runtime &runtime);
// The host path of the most recently opened .pmf file; the next sceMpegCreate decodes that movie for real.
void mpeg_note_movie_file(const std::string &host_path);
// The game recycles its ten ATRAC output buffers faster than it plays them, so by the time sceAudioOutput* reads a buffer
// it may already hold a later frame.  Decoded movie audio is therefore also kept per buffer (in decode order); the audio
// output asks for the PCM that belongs to `address` and plays that instead of the (possibly overwritten) memory.
// Fills `frames` stereo sample frames starting at guest `address`; false when nothing is queued for that address.
bool mpeg_take_audio(std::uint32_t address, std::uint32_t frames, std::int16_t *out);
}
