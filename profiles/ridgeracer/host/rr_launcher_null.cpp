// No settings window on this platform: the saved settings (ridgeracer.ini) are used as they are.
#include "rr_settings.hpp"

namespace rr {

bool run_launcher(PlayerSettings &) { return true; }
bool launcher_shift_held() { return false; }
int monitor_refresh_hz() { return 60; }
void hide_own_console() {}
void show_error_box(const std::string &) {}

} // namespace rr
