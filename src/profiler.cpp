#include "common.h"
#include "profiler.h"
#include "log.h"
#include "patch.h"
#include "timing.h"

// ---------------------------------------------------------------------------
// Audio profiler (profileAudio=1): finds what stalls the main thread.
//
// The game plays music and city ambience through DirectMusic (dxSong,
// "AUDIO\DirectMusic\PC\"): the loader [0x8567C8] and performance [0x8567CC]
// are driven from the main thread by audio commands, while dxSong::Load loads
// segments on its own thread. Sound effects and streams go through Miles
// (mss32.dll). Every Miles import and every method of the DirectMusic
// Performance8 / Loader8 / Segment8 interfaces is timed.
//
// Timing is signature-agnostic: each hooked function gets a 10-byte stub
// (push id; jmp ProfEntry). ProfEntry swaps the caller's return address for
// ProfExitStub and remembers it on a per-thread shadow stack; the exit stub
// takes the time, restores the real return address and returns there. eax,
// edx and the x87 stack are left untouched on the way out (the exit handler
// uses integer code only), so any calling convention and return type works.
// ---------------------------------------------------------------------------
int   g_profOn = 0;
DWORD g_mainTid = 0;
static const int kProfMax = 512;
struct ProfFn {
    const char* name; char nameBuf[48]; DWORD target;
    // plain 64-bit fields: volatile ones can be moved with fild/fistp (x87), and
    // the exit handler must not touch the x87 stack (float return values)
    volatile LONG calls, slow; LONGLONG total, max;           // main thread
    volatile LONG callsOther; LONGLONG totalOther, maxOther;  // other threads
};
static ProfFn  g_prof[kProfMax];
static volatile LONG g_profCount = 0;
static BYTE*   g_profStubs = nullptr;
static DWORD   g_profTls = TLS_OUT_OF_INDEXES;
struct ProfFrame { int id; DWORD realRet; LONGLONG t0; DWORD* args; };
struct ProfStack { int depth; ProfFrame f[64]; };

// per-frame accumulation (main thread, top-level calls only)
static LONGLONG g_pfAudio = 0, g_pfMax = 0; static int g_pfMaxId = -1;
// slow-call reports (any thread), written by the exit handler, printed later
struct SlowCall { int id; LONGLONG dt; DWORD tid; };
static SlowCall g_slow[64]; static volatile LONG g_nSlow = 0;
static LONGLONG g_slowTicks = 0;      // >= 8 ms

extern "C" void ProfExitStub();
static void ProfPatchVtable(DWORD vt, int first, int last, const char* iface, const char* const* names);

static const char* const kPerfNames[] = { "Init","PlaySegment","Stop","GetSegmentState","SetPrepareTime",
 "GetPrepareTime","SetBumperLength","GetBumperLength","SendPMsg","MusicToReferenceTime","ReferenceToMusicTime",
 "IsPlaying","GetTime","AllocPMsg","FreePMsg","GetGraph","SetGraph","SetNotificationHandle","GetNotificationPMsg",
 "AddNotificationType","RemoveNotificationType","AddPort","RemovePort","AssignPChannelBlock","AssignPChannel",
 "PChannelInfo","DownloadInstrument","Invalidate","GetParam","SetParam","GetGlobalParam","SetGlobalParam",
 "GetLatencyTime","GetQueueTime","AdjustTime","CloseDown","GetResolvedTime","MIDIToMusic","MusicToMIDI",
 "TimeToRhythm","RhythmToTime","InitAudio","PlaySegmentEx","StopEx","ClonePMsg","CreateAudioPath",
 "CreateStandardAudioPath","SetDefaultAudioPath","GetDefaultAudioPath","GetParamEx" };          // slots 3..52
static const char* const kLoaderNames[] = { "GetObject","SetObject","SetSearchDirectory","ScanDirectory",
 "CacheObject","ReleaseObject","ClearCache","EnableCache","EnumObject","CollectGarbage",
 "ReleaseObjectByUnknown","LoadObjectFromFile" };                                                 // slots 3..14
static const char* const kSegNames[] = { "GetLength","SetLength","GetRepeats","SetRepeats",
 "GetDefaultResolution","SetDefaultResolution","GetTrack","GetTrackGroup","InsertTrack","RemoveTrack",
 "InitPlay","GetGraph","SetGraph","AddNotificationType","RemoveNotificationType","GetParam","SetParam","Clone",
 "SetStartPoint","GetStartPoint","SetLoopPoints","GetLoopPoints","SetPChannelsUsed","SetTrackConfig",
 "GetAudioPathConfig","Compose","Download","Unload" };                                            // slots 3..30
static int g_idLoaderGetObject = -1;
static CRITICAL_SECTION g_profLock;   // stub creation / vtable patching (main + loader thread)

extern "C" DWORD __cdecl ProfEnter(int id, DWORD* retSlot) {
    DWORD target = g_prof[id].target;
    ProfStack* s = (ProfStack*)TlsGetValue(g_profTls);
    if (!s) {
        s = (ProfStack*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ProfStack));
        if (!s) return target;
        TlsSetValue(g_profTls, s);
    }
    if (s->depth >= 64) return target;                 // too deep: run untimed
    ProfFrame& f = s->f[s->depth++];
    f.id = id; f.realRet = *retSlot; f.args = retSlot + 1;
    LARGE_INTEGER t; QueryPerformanceCounter(&t); f.t0 = t.QuadPart;
    *retSlot = (DWORD)&ProfExitStub;
    return target;
}

// integer-only: a float return value may be sitting in st(0)
extern "C" DWORD __cdecl ProfExit() {
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    ProfStack* s = (ProfStack*)TlsGetValue(g_profTls);
    ProfFrame f = s->f[--s->depth];
    LONGLONG dt = t.QuadPart - f.t0;
    ProfFn& p = g_prof[f.id];
    DWORD tid = GetCurrentThreadId();
    if (tid == g_mainTid) {
        InterlockedIncrement(&p.calls); p.total += dt; if (dt > p.max) p.max = dt;
        if (dt > g_slowTicks / 4) InterlockedIncrement(&p.slow);           // > 2 ms
        if (s->depth == 0) { g_pfAudio += dt; if (dt > g_pfMax) { g_pfMax = dt; g_pfMaxId = f.id; } }
    } else {
        InterlockedIncrement(&p.callsOther); p.totalOther += dt; if (dt > p.maxOther) p.maxOther = dt;
    }
    if (dt >= g_slowTicks) {
        LONG n = InterlockedIncrement(&g_nSlow) - 1;
        if (n < 64) { g_slow[n].id = f.id; g_slow[n].dt = dt; g_slow[n].tid = tid; }
    }
    // a segment came out of the loader: time its methods too
    if (f.id == g_idLoaderGetObject) {
        const DWORD* riid = (const DWORD*)f.args[2];
        DWORD** ppv = (DWORD**)f.args[3];
        if (riid && !IsBadReadPtr(riid, 4) && ppv && !IsBadReadPtr(ppv, 4) && *ppv && !IsBadReadPtr(*ppv, 4)) {
            if (*riid == 0xC6784488u)      ProfPatchVtable(**ppv, 3, 30, "Seg", kSegNames);  // IDirectMusicSegment8
            else if (*riid == 0xF96029A2u) ProfPatchVtable(**ppv, 3, 25, "Seg", kSegNames);  // IDirectMusicSegment
        }
    }
    return f.realRet;
}

__asm__(
    ".text\n"
    ".globl _ProfEntryCommon\n"
    "_ProfEntryCommon:\n"                 // stack: [id][ret][args]
    "  pushl %eax\n  pushl %ecx\n  pushl %edx\n"
    "  leal 16(%esp), %eax\n"             // &ret
    "  pushl %eax\n"
    "  pushl 16(%esp)\n"                  // id
    "  call _ProfEnter\n"
    "  addl $8, %esp\n"
    "  movl %eax, 12(%esp)\n"             // id slot := target
    "  popl %edx\n  popl %ecx\n  popl %eax\n"
    "  ret\n"                             // jump to target, [ret] now ProfExitStub
    ".globl _ProfExitStub\n"
    "_ProfExitStub:\n"
    "  subl $4, %esp\n"                   // slot for the real return address
    "  pushl %eax\n  pushl %ecx\n  pushl %edx\n"
    "  call _ProfExit\n"
    "  movl %eax, 12(%esp)\n"
    "  popl %edx\n  popl %ecx\n  popl %eax\n"
    "  ret\n");
extern "C" void ProfEntryCommon();

static void* ProfMakeStub(DWORD target, const char* name) {
    LONG id = g_profCount;
    if (id >= kProfMax || !g_profStubs) return nullptr;
    g_profCount++;
    ProfFn& p = g_prof[id];
    snprintf(p.nameBuf, sizeof p.nameBuf, "%s", name); p.name = p.nameBuf;
    p.target = target;
    BYTE* st = g_profStubs + id * 10;
    st[0] = 0x68; *(DWORD*)(st + 1) = (DWORD)id;                                     // push id
    st[5] = 0xE9; *(int32_t*)(st + 6) = (int32_t)((DWORD)&ProfEntryCommon - (DWORD)(st + 10));   // jmp
    return st;
}

static DWORD g_patchedVt[64]; static int g_nPatchedVt = 0;
static void ProfPatchVtable(DWORD vt, int first, int last, const char* iface, const char* const* names) {
    EnterCriticalSection(&g_profLock);
    bool done = false;
    for (int i = 0; i < g_nPatchedVt; i++) if (g_patchedVt[i] == vt) done = true;
    DWORD old;
    if (done || g_nPatchedVt >= 64 || IsBadReadPtr((void*)vt, (last + 1) * 4) ||
        !VirtualProtect((void*)vt, (last + 1) * 4, PAGE_READWRITE, &old)) { LeaveCriticalSection(&g_profLock); return; }
    g_patchedVt[g_nPatchedVt++] = vt;
    for (int k = first; k <= last; k++) {
        char nm[48]; snprintf(nm, sizeof nm, "DM %s::%s", iface, names[k - first]);
        void* st = ProfMakeStub(((DWORD*)vt)[k], nm);
        if (st) {
            if (!strcmp(iface, "Loader") && k == 3) g_idLoaderGetObject = g_profCount - 1;
            ((DWORD*)vt)[k] = (DWORD)st;
        }
    }
    VirtualProtect((void*)vt, (last + 1) * 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    LeaveCriticalSection(&g_profLock);
}

// Miles: every mss32 import of the exe
static void ProfHookMiles() {
    BYTE* base = (BYTE*)GetModuleHandleA(nullptr);
    auto* dos = (IMAGE_DOS_HEADER*)base;
    auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++) {
        if (_stricmp((char*)(base + imp->Name), "mss32.dll")) continue;
        auto* names = (IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto* iat = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            const char* nm = (const char*)((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name;
            char buf[48]; snprintf(buf, sizeof buf, "Miles %s", nm[0] == '_' ? nm + 1 : nm);
            void* st = ProfMakeStub((DWORD)iat->u1.Function, buf);
            if (!st) continue;
            DWORD old; VirtualProtect(&iat->u1.Function, 4, PAGE_READWRITE, &old);
            iat->u1.Function = (DWORD)st;
            VirtualProtect(&iat->u1.Function, 4, old, &old);
        }
    }
}

static bool g_profPerf = false, g_profLoader = false;
void ProfPollObjects() {       // DirectMusic objects exist once audio is up
    if (!g_profOn) return;
    DWORD obj = 0, vt = 0;
    if (!g_profLoader && SafeRead(0x8567C8, &obj) && obj && SafeRead(obj, &vt) && vt) {
        ProfPatchVtable(vt, 3, 14, "Loader", kLoaderNames); g_profLoader = true;
        Logf("profiler: DirectMusic loader hooked (%08lX)\n", obj);
    }
    if (!g_profPerf && SafeRead(0x8567CC, &obj) && obj && SafeRead(obj, &vt) && vt) {
        ProfPatchVtable(vt, 3, 52, "Perf", kPerfNames); g_profPerf = true;
        Logf("profiler: DirectMusic performance hooked (%08lX)\n", obj);
    }
}

void ProfInit() {
    g_mainTid = GetCurrentThreadId();
    g_slowTicks = g_qpf.QuadPart * 8 / 1000;
    InitializeCriticalSection(&g_profLock);
    g_profTls = TlsAlloc();
    g_profStubs = (BYTE*)VirtualAlloc(nullptr, kProfMax * 10, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (g_profTls == TLS_OUT_OF_INDEXES || !g_profStubs) { Logf("profiler: init failed\n"); return; }
    g_profOn = 1;
    ProfHookMiles();
    Logf("profiler: on (%ld Miles functions)\n", g_profCount);
}

static double TicksMs(LONGLONG t) { return (double)t * 1000.0 / (double)g_qpf.QuadPart; }

// frame phases (main thread, race loop)
LONGLONG g_pfFrameStart = 0, g_pfUpdate = 0, g_pfDraw = 0; int g_pfSteps = 0;
struct SlowFrame { double total, update, draw, other, audio, maxCall; int maxId, steps; };
static SlowFrame g_slowFrames[10]; static int g_nSlowFrames = 0; static unsigned long g_slowFramesAll = 0;
void ProfFrameBoundary(LONGLONG now) {
    if (g_pfFrameStart) {
        LONGLONG total = now - g_pfFrameStart;
        if (TicksMs(total) > 40.0) {
            g_slowFramesAll++;
            if (g_nSlowFrames < 10) {
                SlowFrame& s = g_slowFrames[g_nSlowFrames++];
                s.total = TicksMs(total); s.update = TicksMs(g_pfUpdate); s.draw = TicksMs(g_pfDraw);
                s.other = s.total - s.update - s.draw; s.audio = TicksMs(g_pfAudio);
                s.maxCall = TicksMs(g_pfMax); s.maxId = g_pfMaxId; s.steps = g_pfSteps;
            }
        }
    }
    g_pfFrameStart = now; g_pfUpdate = g_pfDraw = 0; g_pfAudio = g_pfMax = 0; g_pfMaxId = -1; g_pfSteps = 0;
}

void ProfLogPeriod() {
    if (!g_profOn) return;
    LONG ns = g_nSlow; if (ns > 64) ns = 64;
    for (LONG i = 0; i < ns; i++)
        Logf("  SLOW CALL %.1f ms: %s (%s)\n", TicksMs(g_slow[i].dt), g_prof[g_slow[i].id].name,
             g_slow[i].tid == g_mainTid ? "main thread" : "other thread");
    if (g_nSlow > 64) Logf("  (%ld more slow calls)\n", g_nSlow - 64);
    g_nSlow = 0;
    for (int i = 0; i < g_nSlowFrames; i++) {
        SlowFrame& s = g_slowFrames[i];
        Logf("  HITCH frame %.1f ms: update %.1f (%d steps) draw %.1f present/other %.1f | audio calls %.1f, longest %.1f %s\n",
             s.total, s.update, s.steps, s.draw, s.other, s.audio, s.maxCall, s.maxId >= 0 ? g_prof[s.maxId].name : "-");
    }
    if (g_slowFramesAll > (unsigned long)g_nSlowFrames) Logf("  (%lu hitch frames in total)\n", g_slowFramesAll);
    g_nSlowFrames = 0; g_slowFramesAll = 0;
    // top functions by main-thread time
    int idx[kProfMax]; int n = g_profCount;
    for (int i = 0; i < n; i++) idx[i] = i;
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++)
        if (g_prof[idx[j]].total + g_prof[idx[j]].totalOther > g_prof[idx[i]].total + g_prof[idx[i]].totalOther) { int t = idx[i]; idx[i] = idx[j]; idx[j] = t; }
    Logf("  audio time by function (main thread calls/total/max/>2ms | other threads calls/total/max):\n");
    for (int k = 0; k < n && k < 12; k++) {
        ProfFn& p = g_prof[idx[k]];
        if (!p.calls && !p.callsOther) break;
        Logf("    %-36s %6ld %8.1fms %6.1fms %4ld | %5ld %8.1fms %6.1fms\n", p.name, p.calls, TicksMs(p.total),
             TicksMs(p.max), p.slow, p.callsOther, TicksMs(p.totalOther), TicksMs(p.maxOther));
    }
    for (int i = 0; i < n; i++) { g_prof[i].calls = g_prof[i].slow = g_prof[i].callsOther = 0;
        g_prof[i].total = g_prof[i].max = g_prof[i].totalOther = g_prof[i].maxOther = 0; }
}
