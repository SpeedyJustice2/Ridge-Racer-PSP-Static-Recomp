#pragma once

#include "rr_gpu.hpp"

#include <vector>

namespace psprecomp { class GuestMemory; }

namespace rr {

// Native-resolution software backend that draws straight into emulated VRAM.
class SoftwareBackend final : public GpuBackend {
public:
    explicit SoftwareBackend(psprecomp::GuestMemory &memory) : memory_(memory) {}
    void draw(const GpuDraw &state, GpuPrim prim, const GpuVertex *vertices, std::size_t count) override;
    [[nodiscard]] std::uint64_t pixels_written() const { return pixels_written_; }
    [[nodiscard]] std::uint64_t unsupported_texture_draws() const { return unsupported_texture_draws_; }

private:
    psprecomp::GuestMemory &memory_;
    std::uint64_t pixels_written_{};
    std::uint64_t unsupported_texture_draws_{};
};

// Shared pixel helpers (colours are 0xAABBGGRR; formats 0=565 1=5551 2=4444 3=8888).
[[nodiscard]] std::uint32_t pixel_expand(std::uint32_t format, std::uint32_t packed);
[[nodiscard]] std::uint32_t pixel_compress(std::uint32_t format, std::uint32_t colour);
// Decodes the draw's current texture (palettes, swizzle, 16-bit formats) to width*height RGBA8 texels.
bool decode_texture_rgba(const GpuDraw &d, const psprecomp::GuestMemory &memory, std::vector<std::uint32_t> &out);

// Converts one display frame buffer to RGBA8888 (byte order R,G,B,A).
// Returns false when the address range is not readable.
bool convert_frame_to_rgba(const psprecomp::GuestMemory &memory, std::uint32_t address, std::uint32_t stride,
                           std::uint32_t format, std::uint32_t width, std::uint32_t height, std::uint8_t *rgba);

} // namespace rr
