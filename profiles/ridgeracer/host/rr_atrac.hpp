#pragma once

namespace psprecomp { class Runtime; }

namespace rr {
// sceAtrac3plus: streams are tracked exactly like the real library (buffer
// refill protocol, frame counts, loop counts) but decode to silence for now.
void install_atrac_hle(psprecomp::Runtime &runtime);
}
