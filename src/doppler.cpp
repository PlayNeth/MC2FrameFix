#include "common.h"
#include "doppler.h"
#include "game.h"
#include "config.h"
#include "log.h"
#include "patch.h"
#include "timing.h"
#include "streams.h"

// ---------------------------------------------------------------------------
// Camera-switch Doppler (dopplerHoldOnSwitch)
//
// Every update the player's camera is handed to the sound manager as the
// listener (0x469B60 -> 0x5D6050(matrix, ear position, noDoppler)); listener
// slots at 0x6CE320 (0x7C each): matrix +0, ear +0x30, noDoppler +0x3C, and the
// previous frame's copy at +0x40 / +0x70 (0x5D6170, end of frame). Each sound's
// Doppler factor (0x5D5CD0, flag 8) is c / (c + d(distance)/dt) from how far
// the ear moved against the sound since the last frame, using InvSeconds.
// A camera switch flies the camera from one view to the other in ~0.3 s,
// passing through the car, and its path has uneven steps: the ear moves tens
// of m/s against the car's own engine, back and forth, so every engine layer's
// pitch warbles by several percent each frame - the scratching. The game
// already turns Doppler off for one camera type (word +0x10 == 4); the same
// flag is now set for dopplerHoldOnSwitch seconds after the camera (mode
// [[obj+0x4C]+0xC], its camera [mode+0x2C] or its type) changes. While it is
// set each sound keeps its last Doppler factor; afterwards it continues from
// the current positions, so there is no jump.
// ---------------------------------------------------------------------------
// --- traceCameraAudio: which voices start / stop around a camera switch -----
// The game's voice class (vtable 0x654F38, "sndVoice") is wrapped at Play
// (slot 2, 0x5D6CB0), Stop (slot 5, 0x5D6DA0: queues AIL stop + release) and
// Free (slot 6, 0x5D6CA0). Each call is kept in a ring with its caller and the
// next return addresses found on the stack; at stats time the calls from
// 100 ms before to 800 ms after each camera switch are written to the log.
struct SndEv { double ms; DWORD slot, self, ret, arg0, stk[4]; };
static const int kSndEv = 4096;
static SndEv g_sev[kSndEv]; static volatile LONG g_sevN = 0;
struct SndSwitch { double ms; DWORD tick; WORD type; bool obj; };
static SndSwitch g_sw[16]; static int g_nSw = 0, g_swDumped = 0;
bool g_sndTrace = false;
static double NowMs() { return (double)QpcNow() * 1000.0 / (double)g_qpf.QuadPart; }
bool LooksLikeRet(DWORD v) {
    if (v < 0x401005 || v >= 0x62C000) return false;
    const BYTE* p = (const BYTE*)v;
    return p[-5] == 0xE8 || (p[-6] == 0xFF && p[-5] == 0x15) || (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) ||
           (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10);
}
extern "C" __attribute__((used)) void __cdecl MC2FF_SndTrace(DWORD slot, DWORD* regs) {
    // regs: pushad image (edi esi ebp esp ebx edx ecx eax), then the return address and args
    LONG i = InterlockedIncrement(&g_sevN) - 1;
    SndEv& e = g_sev[i & (kSndEv - 1)];
    e.ms = NowMs(); e.slot = slot; e.self = regs[6]; e.ret = regs[8]; e.arg0 = regs[9];
    int k = 0;
    if (!IsBadReadPtr(regs, 96 * 4))
        for (int j = 10; j < 96 && k < 4; j++) if (LooksLikeRet(regs[j])) e.stk[k++] = regs[j];
    for (; k < 4; k++) e.stk[k] = 0;
}
static void SndTraceSwitch(WORD type, bool obj) {
    if (!g_sndTrace || g_nSw >= 16) return;
    SndSwitch& w = g_sw[g_nSw++]; w.ms = NowMs(); w.tick = GetTickCount(); w.type = type; w.obj = obj;
}
void SndTraceDump() {
    if (!g_sndTrace) return;
    double now = NowMs();
    static const char* names[8] = { "?", "?", "play", "?", "?", "stop", "free", "?" };
    while (g_swDumped < g_nSw && g_sw[g_swDumped].ms + 800.0 < now) {
        SndSwitch& w = g_sw[g_swDumped++];
        Logf("  camera switch #%d (GetTickCount %lu): type %u%s\n", g_swDumped, w.tick,
             (unsigned)w.type, w.obj ? "" : " (type only)");
        LONG n = g_sevN; int shown = 0;
        for (LONG j = n > kSndEv ? n - kSndEv : 0; j < n && shown < 80; j++) {
            SndEv& e = g_sev[j & (kSndEv - 1)];
            double d = e.ms - w.ms;
            if (d < -100.0 || d > 800.0) continue;
            Logf("    %+7.1f ms %-4s voice %08lX arg %08lX from %06lX  stack %06lX %06lX %06lX %06lX\n", d,
                 names[e.slot & 7], e.self, e.arg0, e.ret, e.stk[0], e.stk[1], e.stk[2], e.stk[3]);
            shown++;
        }
        if (!shown) Logf("    (no voice started or stopped)\n");
    }
}
void InstallSndTrace() {
    static const DWORD vt = 0x654F38;
    static const struct { DWORD slot, fn; } hooks[] = { { 2, 0x5D6CB0 }, { 5, 0x5D6DA0 }, { 6, 0x5D6CA0 } };
    BYTE* mem = (BYTE*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) return;
    for (auto& h : hooks) if (G<DWORD>(vt + h.slot * 4) != h.fn) { Logf("camera audio trace: vtable differs, OFF\n"); return; }
    int i = 0;
    for (auto& h : hooks) {
        BYTE* p = mem + 32 * i++;
        p[0] = 0x60;                                   // pushad
        p[1] = 0x89; p[2] = 0xE0;                      // mov eax, esp
        p[3] = 0x50;                                   // push eax
        p[4] = 0x6A; p[5] = (BYTE)h.slot;              // push slot
        p[6] = 0xE8; *(int32_t*)(p + 7) = (int32_t)((DWORD)&MC2FF_SndTrace - (DWORD)(p + 11));
        p[11] = 0x83; p[12] = 0xC4; p[13] = 0x08;      // add esp, 8
        p[14] = 0x61;                                  // popad
        p[15] = 0xE9; *(int32_t*)(p + 16) = (int32_t)(h.fn - (DWORD)(p + 20));
        DWORD old; VirtualProtect((void*)(vt + h.slot * 4), 4, PAGE_READWRITE, &old);
        G<DWORD>(vt + h.slot * 4) = (DWORD)p;
        VirtualProtect((void*)(vt + h.slot * 4), 4, old, &old);
    }
    g_sndTrace = true;
    Logf("camera audio trace: installed\n");
}

unsigned long st_dopplerSwitches = 0, st_dopplerHeld = 0;
// A new camera object (the cut to the bumper camera, which the game always
// makes as an instant cut, type 4) is handed to the listener before it has
// updated: its matrix is still where that camera was last used. For one tick
// the listener sits far from the car, every engine voice drops below the
// audible threshold (0x5D5920) and is stopped and freed, and a tick later the
// voices are started again from the beginning (0x5D5AA0): the audio cut-off.
// The camera-audio trace showed exactly that: all five engine voices stopped
// one tick after a cut and restarted two ticks later. So for fixCameraCutAudio
// ticks after the camera object changes, the listener keeps the previous
// tick's pose (inaudible at 30 Hz) until the new camera has a real one.
struct ListenerState { DWORD obj, mode, cam; WORD type; float m[12], ear[3]; bool have; int delay; float hold; double lastMs; };
static ListenerState g_ls[4];
unsigned long st_listenerDelayed = 0;
extern "C" __attribute__((used)) int __cdecl MC2FF_ListenerHook(DWORD obj, DWORD* args) {
    ListenerState* L = nullptr;
    for (auto& x : g_ls) if (x.obj == obj) { L = &x; break; }
    double now = NowMs();
    if (!L) {
        // a new owner (each race creates its own): take a free slot, else the
        // one updated longest ago, so the fix keeps working after many races
        L = &g_ls[0];
        for (auto& x : g_ls) { if (!x.obj) { L = &x; break; } if (x.lastMs < L->lastMs) L = &x; }
        memset(L, 0, sizeof *L); L->obj = obj; L->type = 0xFFFF; L->lastMs = now;
    }
    if (now - L->lastMs > 250.0) { L->type = 0xFFFF; L->have = false; L->delay = 0; L->hold = 0; }   // new race / was paused
    L->lastMs = now;
    DWORD mgr = 0, mode = 0, cam = 0; WORD type = 0;
    if (!SafeRead(obj + 0x4C, &mgr) || !mgr || !SafeRead(mgr + 0x0C, &mode)) return 0;
    if (mode) { SafeRead(mode + 0x2C, &cam); SafeRead(mode + 0x10, &type); }
    bool objChanged = L->type != 0xFFFF && (mode != L->mode || cam != L->cam);
    if (L->type != 0xFFFF && (objChanged || type != L->type)) {
        L->hold = (float)g_cfg.dopplerHoldOnSwitch; st_dopplerSwitches++;
        SndTraceSwitch(type, objChanged);
        if (objChanged && L->have) L->delay = g_cfg.fixCameraCutAudio;
    }
    L->mode = mode; L->cam = cam; L->type = type;
    const float* m = (const float*)args[0];
    const float* ear = (const float*)args[1];
    if (L->delay > 0) {
        L->delay--;
        args[0] = (DWORD)L->m; args[1] = (DWORD)L->ear;    // last tick's listener
        st_listenerDelayed++;
    } else if (m && !IsBadReadPtr(m, 48)) {
        memcpy(L->m, m, 48);
        if (ear && !IsBadReadPtr(ear, 12)) memcpy(L->ear, ear, 12); else memcpy(L->ear, m + 9, 12);
        L->have = true;
    }
    if (L->hold > 0.0f && g_cfg.dopplerHoldOnSwitch > 0) {
        L->hold -= G<float>(mc2::pSeconds);
        st_dopplerHeld++;
        return 1;                                            // Doppler off
    }
    return 0;
}
// replaces `call 0x5D6050` at 0x469C79; esi = the player camera's owner
// stack on entry: [esp] ret, +4 matrix, +8 ear, +0xC noDoppler (low byte)
__attribute__((naked)) static void Stub_AddListener() {
    asm volatile(
        "leal 4(%esp), %eax\n\t"
        "pushl %eax\n\t"
        "pushl %esi\n\t"
        "call _MC2FF_ListenerHook\n\t"
        "addl $8, %esp\n\t"
        "testl %eax, %eax\n\t"
        "jz 1f\n\t"
        "movb $1, 0xC(%esp)\n"             // Doppler off
        "1:\n\t"
        "movl $0x5D6050, %eax\n\t"
        "jmp *%eax\n");
}
void InstallDopplerFix() {
    if (!CheckCall(0x469C79, 0x5D6050)) { Logf("doppler fix: code differs, OFF\n"); return; }
    WriteRel(0x469C79, 0xE8, (void*)&Stub_AddListener);
    Logf("camera-switch audio fix: installed (Doppler hold %.2f s, listener hold %d ticks after a cut)\n", g_cfg.dopplerHoldOnSwitch, g_cfg.fixCameraCutAudio);
}
