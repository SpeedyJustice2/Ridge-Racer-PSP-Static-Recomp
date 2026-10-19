#pragma once

// Menu sounds for the on-screen keyboard, taken from the player's own game at run time.
//
// The game plays its cursor / confirm / back sounds through the SAS voices.  While it does, this module notes which sample
// each kind of button press started (it never writes anything to disk and ships no audio): the ADPCM data is decoded from
// the game's own memory into RAM, and the on-screen keyboard replays those clips.  A sound the game has not played yet in
// this session falls back to a short synthesized click.
#include <cstdint>

#include "psprecomp/guest_memory.hpp"
#include "psprecomp/hle_sas.hpp"

namespace rr {

enum class MenuSound { Move, Confirm, Back };

// Call when the game reads the pad: `pressed` = buttons that went down since the last read.
void menu_sound_note_press(std::uint32_t pressed, std::uint64_t frame);
// Call for every SAS key-on the game performs.
void menu_sound_on_keyon(std::uint64_t frame, const psprecomp::GuestMemory &memory, const psprecomp::SasVoiceState &voice);
// Plays one of the sounds (starts the audio device if needed).
void menu_sound_play(MenuSound sound);

} // namespace rr
