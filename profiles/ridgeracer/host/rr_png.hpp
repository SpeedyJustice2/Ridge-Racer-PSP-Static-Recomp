#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace rr {
// Minimal PNG writer (per-row filters + LZ77 + dynamic Huffman deflate, no zlib needed).  rgba is width*height*4 bytes; alpha is dropped.
// Writes the file on a background thread (a large screenshot would otherwise stall the game for a moment); takes the pixels over.
// Pending writes are finished before the program exits.
void write_png_async(std::string path, std::vector<std::uint8_t> rgba, std::uint32_t width, std::uint32_t height);
// Writes raw bytes to a file on the same background thread pool.
void write_file_async(std::string path, std::vector<std::uint8_t> bytes);
bool write_png(const char *path, const std::uint8_t *rgba, std::uint32_t width, std::uint32_t height);
}
