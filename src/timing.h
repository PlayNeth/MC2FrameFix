// What the other parts use from timing.cpp.
#pragma once
#include "common.h"

extern LARGE_INTEGER g_qpf;
LONGLONG QpcNow();
extern LONGLONG g_lastFrame, g_limNext;
extern double g_accum;
extern bool g_forcing;
extern float g_alpha;
extern bool g_frameSimOk;
extern unsigned long g_step;
extern double g_drawDt;
LONGLONG FrameTime();
void __cdecl Hook_Timer();
BYTE* GameState();
bool RaceState();
