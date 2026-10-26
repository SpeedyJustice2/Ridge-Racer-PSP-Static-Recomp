// A few plain-text facts about the machine for the performance report: CPU, memory, power state.
#pragma once
#include <string>
#include <vector>

namespace rr {

// Names of the graphics adapters Windows knows about (empty elsewhere / when unknown).
std::vector<std::string> gpu_adapter_names();
// True when the only graphics hardware found looks integrated or software (Intel HD/UHD/Iris, AMD "Radeon Graphics" / 610M-style
// laptop chips, Vega, Microsoft Basic Render): the first-run settings are then lighter.  False when unsure or a discrete card exists.
bool weak_graphics_only();

// Several short lines ("CPU: ...", "Memory: ...", "Power: ...") separated by newlines, no trailing newline.
std::string system_description();

} // namespace rr
