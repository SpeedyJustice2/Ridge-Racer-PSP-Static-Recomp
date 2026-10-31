// Gets the converted game code into the program: either linked in (the original single-file build) or loaded from the game
// library next to the program (the split build; see gen/rr_gen_abi.hpp).
#pragma once
#include <string>

namespace psprecomp { class Runtime; }

namespace rr {

// Returns "" on success or a message for the player.  `exe_dir` is the folder that holds the program.
std::string load_game_code(psprecomp::Runtime &runtime, const std::string &exe_dir);

// The folder that holds the running program.
std::string executable_directory();

// True when the program expects a separate game library (split build).
bool game_library_split();

} // namespace rr
