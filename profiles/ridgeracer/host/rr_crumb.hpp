#pragma once
// Breadcrumbs for diagnosing hangs: a few words the engine updates as it works.  Reading them from another thread (the
// browser page does this when the engine stops producing frames) shows where it was last.  [0] = GE list address,
// [1] = GE command word, [2] = GL backend method id (see kCrumbNames in web/rr-web.js), [3] = free for finer markers.
#include <cstdint>
namespace rr {
inline volatile std::uint32_t g_rr_crumb[8] = {};
}
#define RR_CRUMB(i, v) (::rr::g_rr_crumb[(i)] = static_cast<std::uint32_t>(v))
