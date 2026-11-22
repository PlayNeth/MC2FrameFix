// What the other parts use from profiler.cpp.
#pragma once
#include "common.h"

extern int g_profOn;
extern DWORD g_mainTid;
void ProfPollObjects();
void ProfInit();
extern LONGLONG g_pfFrameStart, g_pfUpdate, g_pfDraw;
extern int g_pfSteps;
void ProfFrameBoundary(LONGLONG now);
void ProfLogPeriod();
