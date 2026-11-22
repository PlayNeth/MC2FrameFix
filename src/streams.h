// What the other parts use from streams.cpp.
#pragma once
#include "common.h"

namespace mc2 {
    const DWORD iatStreamStatus = 0x62D290, iatOpenStream = 0x62D294, iatStartStream = 0x62D298,
                iatPauseStream  = 0x62D29C, iatCloseStream = 0x62D2A0, iatStreamVolPan = 0x62D2A4;
}
extern CRITICAL_SECTION g_streamLock;
extern bool g_asyncStreams;
extern unsigned long st_streamOpens, st_streamSync, st_streamFail;
extern double st_streamMaxMs;
struct StreamLogEnt { char name[48]; float prefetchMs, openMs, readyMs; bool ok; };
extern StreamLogEnt g_streamLog[8];
extern int g_nStreamLog;
extern unsigned long st_prefetchMB10;
void StreamPoll();
void InstallAsyncStreams();
