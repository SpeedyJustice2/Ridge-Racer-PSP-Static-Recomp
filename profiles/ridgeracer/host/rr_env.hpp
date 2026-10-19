#pragma once

#include <cstdlib>

// Presence of a diagnostic environment switch, read once per call site.
#define RR_ENV_FLAG(name) \
    ([]() noexcept { static const bool value = std::getenv(name) != nullptr; return value; }())

// Set from the --dev command-line flag.  Without it the developer hot keys are inactive and every RR_* environment
// variable is removed at start-up (see main.cpp).
namespace rr { inline bool g_rr_dev = false; }
