#pragma once

// On-screen keyboard UI for the sceUtilityOsk HLE: a text field fed by typed characters (physical keyboard) and by a
// letter grid driven with the gamepad.  Pure logic + a CPU overlay renderer, no platform dependencies.

#include <cstdint>
#include <string>
#include <vector>

namespace rr {

enum class OskState { Active, Accepted, Cancelled };

// Starts a session.  `limit` = maximum number of characters (0 = default 15).
void osk_ui_begin(const std::u16string &description, const std::u16string &initial, std::uint32_t limit);
// Call once per frame.  `typed` holds characters typed since the last call (8 = backspace, 13 = Enter, 27 = Esc);
// `pad` holds PSP button bits from the gamepad only.
OskState osk_ui_update(const std::vector<char16_t> &typed, std::uint32_t pad);
// What happened during the last updates, for sound feedback; reading clears it.
constexpr std::uint32_t kOskMove = 1u, kOskInsert = 2u, kOskDelete = 4u, kOskAccept = 8u, kOskCancel = 16u;
[[nodiscard]] std::uint32_t osk_ui_take_events();
[[nodiscard]] bool osk_ui_active();
[[nodiscard]] const std::u16string &osk_ui_text();
void osk_ui_end();
// Draws the keyboard page (opaque, covers the whole image) into a w x h RGBA image (w should be a multiple of 480; the
// layout is designed for 480x272 and scales smoothly).
void osk_ui_render(std::uint8_t *rgba, int w, int h);
// Changes whenever what osk_ui_render would draw changes (text, cursor cell, caret blink): lets the caller redraw only then.
[[nodiscard]] std::uint64_t osk_ui_state_key();

} // namespace rr
