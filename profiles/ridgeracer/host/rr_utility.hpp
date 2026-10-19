#pragma once

namespace psprecomp { class Runtime; }

namespace rr {
// sceUtility dialogs other than savedata: the on-screen keyboard (name entry)
// and the message dialog.  They complete automatically for now (the keyboard
// accepts the suggested text or a default name) unless interactive mode is on, in
// which case the on-screen keyboard UI (rr_osk_ui) is shown and driven.
void install_utility_hle(psprecomp::Runtime &runtime);
void utility_set_interactive(bool on);
// True while the on-screen keyboard is open (the game must not see button presses meanwhile).
[[nodiscard]] bool osk_active();
}
