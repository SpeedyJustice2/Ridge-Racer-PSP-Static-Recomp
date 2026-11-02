#pragma once

// HD car textures ("Car textures: HD" in Recomp Options > Graphics).
//
// In races the game draws each car with a small texture (256x256) that is a scaled-down copy of the car's showroom texture
// (machine select: 512x512, same layout).  The showroom textures are kept here when they are uploaded (memory, and a
// cache folder so they survive restarts), and when a race texture is uploaded that matches one of them scaled down, a
// composite at twice the size is uploaded instead: 8x8-texel tiles that match take the showroom pixels, the others keep the
// race texels (enlarged), so parts that differ between the two (lights, the bottom strip) stay correct.
// The texture is then sampled with its original size in texel units, so nothing else changes.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rr::texhd {

// The cache folder (texcache next to the game); empty = memory only.
void set_cache_dir(const std::filesystem::path &dir);
// The game's RRP.DAT: on first use its car model textures (the showroom ones) are read in the background, so every car can be
// drawn in HD without having been shown in machine select first.
void preload_disc(const std::filesystem::path &rrp_dat);
// Changes when the disc textures have been read: a texture that found no match before may match now.
[[nodiscard]] int generation();
// A texture the GPU renderer uploads (RGBA, 0xAABBGGRR per texel).  Big ones are remembered as candidates.
void offer(const std::uint32_t *rgba, std::uint32_t w, std::uint32_t h);
// A texture about to be uploaded: if it is a scaled-down copy of a remembered one, `out` gets the composite (out_w x out_h: 2x or 4x) and
// true is returned.  `why` gets a short line for the log when a match was found.
bool upgrade(const std::uint32_t *rgba, std::uint32_t w, std::uint32_t h, std::vector<std::uint32_t> &out, std::uint32_t &out_w, std::uint32_t &out_h, std::string &why);

} // namespace rr::texhd
