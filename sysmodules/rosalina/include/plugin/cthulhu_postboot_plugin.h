#pragma once
#include <3ds.h>
Result CthulhuPostBoot_LoadPlugin(Handle process, u32 pid, u32 *threadIdOut);
const char *CthulhuPostBoot_LastStage(void);
