#pragma once

// Rewrites the game's PSP button symbols (fonts, Options > Controller picture) for the controller in use.  Called by
// the GE for every textured CLUT draw, before the renderer reads the texture.

#include "rr_gpu.hpp"

namespace psprecomp { class GuestMemory; }

namespace rr {
void glyph_patch_draw(psprecomp::GuestMemory &memory, const GpuDraw &d);
}
