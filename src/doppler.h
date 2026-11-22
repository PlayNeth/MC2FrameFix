// What the other parts use from doppler.cpp.
#pragma once
#include "common.h"

extern bool g_sndTrace;
bool LooksLikeRet(DWORD v);
void SndTraceDump();
void InstallSndTrace();
extern unsigned long st_dopplerSwitches, st_dopplerHeld;
extern unsigned long st_listenerDelayed;
void InstallDopplerFix();
