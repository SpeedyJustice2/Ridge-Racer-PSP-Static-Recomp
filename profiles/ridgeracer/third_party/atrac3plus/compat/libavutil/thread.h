#pragma once
/* The Atrac3pDecoder wrapper makes sure the one-time setup runs only once (under a C++ std::call_once). */
typedef int AVOnce;
#define AV_ONCE_INIT 0
static inline int ff_thread_once(AVOnce *o, void (*f)(void)) { if (!*o) { *o = 1; f(); } return 0; }
