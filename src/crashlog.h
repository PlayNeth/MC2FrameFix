// What the other parts use from crashlog.cpp.
#pragma once
#include "common.h"

extern DWORD g_modLo, g_modHi;
LONG CALLBACK CrashLog(EXCEPTION_POINTERS* x);
