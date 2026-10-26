#pragma once

// Controllers: which kind is in use (for the button pictures shown on screen) and, on Windows and Linux, reading every
// connected controller through SDL2 (Xbox, PlayStation 3/4/5, Switch Pro / Joy-Con, 8BitDo, Logitech and other
// DirectInput/HID pads; SDL's own controller database plus an optional gamecontrollerdb.txt next to the game).
//
// Windows: SDL2.dll is loaded at run time when it sits next to the game (the installer puts it there).  Without it the
// presenter falls back to XInput (Xbox-style controllers only), as before.  Linux: the SDL2 the game is linked with.
// Web: the page reads the Gamepad API itself and reports the controller kind (rr-web.js).

#include <cstdint>
#include <string>

namespace rr::pad {

// The kind of controller (by maker), as far as it can be told.
enum class Family : int { None = 0, Xbox = 1, PlayStation = 2, Nintendo = 3, Generic = 4 };
// Which button pictures to show.
enum class Glyphs : int { PlayStation = 0, Xbox = 1, Nintendo = 2, Keyboard = 3 };
// The player's choice (settings "button_prompts"): automatic follows the device used last.
enum class Prompts : int { Auto = 0, Xbox = 1, PlayStation = 2, Nintendo = 3, Keyboard = 4 };

Family family_from_ids(std::uint16_t vendor, std::uint16_t product, const char *name);
const char *family_name(Family f);
const char *glyphs_name(Glyphs g);

void set_prompts(Prompts p);              // --button-prompts / settings
Prompts prompts();
Prompts parse_prompts(const std::string &s, Prompts fallback);   // auto / xbox / playstation / nintendo / keyboard
void set_triggers(bool on);               // --triggers / --no-triggers: RT acts as CROSS, LT as SQUARE (default on)
bool triggers();

// Once per presented frame: all buttons (keyboard + pads), the pads' share of them, the left stick, and the kind of the
// controller used last.  Decides which device the player is using.
void frame(std::uint32_t all_buttons, std::uint32_t pad_buttons, bool pad_stick_moved, Family pad_family);
Glyphs glyphs();                          // what to show right now
unsigned generation();                    // changes whenever glyphs() changes (redraw cached pictures)

// ---- SDL2 controllers (Windows: SDL2.dll at run time; Linux: the linked SDL2) ----
bool sdl_available();                     // the library is there and its controller part started
std::uint32_t sdl_buttons();              // PSP bits of all connected controllers (OR)
bool sdl_stick(std::uint8_t &lx, std::uint8_t &ly);   // left stick of the controller moved last, outside the dead zone
Family sdl_family();                      // kind of the controller used last (None if none connected)
std::string sdl_describe();               // "2 controllers: Xbox Wireless Controller (Xbox), DualSense (PlayStation)"

} // namespace rr::pad
