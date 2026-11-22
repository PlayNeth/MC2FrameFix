#include "common.h"
#include "crashlog.h"
#include "log.h"
#include "doppler.h"

// ---------------------------------------------------------------------------
// Crash logger (log=1): where a fatal fault happened
// ---------------------------------------------------------------------------
DWORD g_modLo, g_modHi;
static bool InGame(DWORD a) { return a >= 0x401000 && a < 0x700000; }
static bool InMod(DWORD a) { return a >= g_modLo && a < g_modHi; }
LONG CALLBACK CrashLog(EXCEPTION_POINTERS* x) {
    static volatile LONG n = 0;
    DWORD code = x->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_PRIV_INSTRUCTION) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = x->ContextRecord;
    if (!InGame(c->Eip) && !InMod(c->Eip)) return EXCEPTION_CONTINUE_SEARCH;   // (IsBadReadPtr probes etc.)
    if (InterlockedIncrement(&n) > 3) return EXCEPTION_CONTINUE_SEARCH;
    Logf("CRASH %08lX at %s+%lX (eip %08lX), access %s %08lX, thread %lu\n", code,
         InMod(c->Eip) ? "MC2FrameFix" : "mc2", InMod(c->Eip) ? c->Eip - g_modLo : c->Eip - 0x400000, c->Eip,
         x->ExceptionRecord->NumberParameters >= 2 && x->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
         x->ExceptionRecord->NumberParameters >= 2 ? (DWORD)x->ExceptionRecord->ExceptionInformation[1] : 0,
         GetCurrentThreadId());
    Logf("  eax %08lX ebx %08lX ecx %08lX edx %08lX esi %08lX edi %08lX ebp %08lX esp %08lX\n",
         c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    const DWORD* sp = (const DWORD*)c->Esp; int k = 0;
    Logf("  stack:");
    for (int i = 0; i < 512 && k < 16; i++) {
        if (IsBadReadPtr(sp + i, 4)) break;
        DWORD v = sp[i];
        if (InMod(v)) { Logf(" mod+%lX", v - g_modLo); k++; }
        else if (InGame(v) && LooksLikeRet(v)) { Logf(" %06lX", v); k++; }
    }
    Logf("\n");
    return EXCEPTION_CONTINUE_SEARCH;
}
