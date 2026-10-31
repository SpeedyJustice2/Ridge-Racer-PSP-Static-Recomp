#pragma once
#include <cstdint>
namespace rr::gen {
// Bump when the table in rr_gen_abi.hpp, the layout of Runtime / AllegrexContext / GuestMemory (include/psprecomp) or the code
// generator's output format changes.  A program update with the same number works with the game library already installed;
// a different number makes the updater run the full Setup (which rebuilds the library).  make-release.cmd writes the number
// into the update package (abi.txt).
inline constexpr std::uint32_t kAbiVersion = 1;
}
