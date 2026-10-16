#ifndef ENGINE_CORE_PROFILE_H
#define ENGINE_CORE_PROFILE_H

// Tracy zones, compiled in only with POCKET_ENABLE_TRACY. Names must be string
// literals. Zones are per thread, so each World's thread shows its own work.
#if defined(POCKET_PROFILE)
#include "tracy/Tracy.hpp"
#define POCKET_ZONE(name) ZoneScopedN(name)
#define POCKET_FRAME_MARK() FrameMark
#else
#define POCKET_ZONE(name) ((void)0)
#define POCKET_FRAME_MARK() ((void)0)
#endif

#endif
