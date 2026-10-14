#pragma once
// Tracy instrumentation; compiled out unless configured with -DJOA_TRACY=ON.
#ifdef JOA_TRACY
#include <tracy/Tracy.hpp>
#define JOA_ZONE(name) ZoneScopedN(name)
#define JOA_FRAME_MARK() FrameMark
#else
#define JOA_ZONE(name) ((void)0)
#define JOA_FRAME_MARK() ((void)0)
#endif
