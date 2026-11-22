#include "common.h"
#include "timing.h"
#include "game.h"
#include "config.h"
#include "patch.h"
#include "streams.h"

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------
LARGE_INTEGER g_qpf;
LONGLONG QpcNow() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
LONGLONG g_lastFrame = 0, g_limNext = 0;
double   g_accum = 0.0;
bool     g_forcing = false;
float    g_alpha = 0.0f;       // blend position between prev and cur step
bool     g_frameSimOk = false; // this frame ran the fixed-step path
unsigned long g_step = 0;      // fixed steps completed (snapshot index)
double   g_drawDt = 0.0;       // display time since the previous frame (s)

// --- frame clock ---------------------------------------------------------------
// The accumulator advances by "display time" deltas, not raw QPC readings, so
// the blend position moves evenly (raw readings jitter by Sleep/vsync noise).
//  - limiter on: a frame on time is displayed at its deadline (exactly one
//    period after the previous one); a late frame at its real time, and the
//    grid re-anchors there, so the sum always follows real time.
//  - limiter off: a light filter on frame start times (as HP2FrameFix).
static LONGLONG FrameLimit() {
    LONGLONG period = (LONGLONG)((double)g_qpf.QuadPart / g_cfg.fpsLimit);
    static bool res = false;
    if (!res) { timeBeginPeriod(1); res = true; }
    LONGLONG now = QpcNow();
    if (!g_limNext || now - g_limNext > period * 4) g_limNext = now;     // first frame / long hitch
    for (;;) {
        now = QpcNow();
        LONGLONG left = g_limNext - now;
        if (left <= 0) break;
        double ms = (double)left * 1000.0 / (double)g_qpf.QuadPart;
        if (ms > 2.0) Sleep((DWORD)(ms - 1.5)); else YieldProcessor();
    }
    LONGLONG display = (now - g_limNext < period / 4) ? g_limNext : now; // small overshoot = on time
    g_limNext = display + period;
    return display;
}
static LONGLONG g_ftLast = 0, g_ftInterval = 0;
static LONGLONG SmoothedNow() {
    LONGLONG now = QpcNow();
    if (!g_ftLast) { g_ftLast = now; return now; }
    LONGLONG d = now - g_ftLast;
    if (!g_ftInterval) g_ftInterval = d;
    LONGLONG pred = g_ftLast + g_ftInterval, err = now - pred, ft;
    if (g_ftInterval > 0 && err < g_ftInterval / 2 && err > -g_ftInterval / 2) {
        ft = pred + err / 8;                          // steady: follow gently
        g_ftInterval += (d - g_ftInterval) / 16;
    } else {
        ft = now;                                     // hitch or rate change: snap
        if (d > 0 && d < g_qpf.QuadPart / 10) g_ftInterval = d;
    }
    g_ftLast = ft;
    return ft;
}
LONGLONG FrameTime() { return g_cfg.fpsLimit > 0.0 ? FrameLimit() : SmoothedNow(); }

void __cdecl Hook_Timer() {
    bool special = G<BYTE>(mc2::pTimerReset) || G<int32_t>(mc2::pTimerFixed) || G<BYTE>(mc2::pTimerFreeze);
    ((VoidFn)mc2::fnTimer)();
    if (g_forcing && !special) {
        float dt = (float)(1.0 / g_cfg.simHz) * G<float>(mc2::pTimeScale);
        if (dt < G<float>(mc2::pMinSec)) dt = G<float>(mc2::pMinSec);
        float mx = G<float>(mc2::pMaxSec);
        if (mx > 0.0f && dt > mx) dt = mx;
        G<float>(mc2::pSeconds) = dt;
        G<float>(mc2::pInvSeconds) = 1.0f / dt;
    }
}

BYTE* GameState() { BYTE* gs = nullptr; SafeRead(mc2::pGameState, (DWORD*)&gs); return gs; }
bool RaceState() {
    BYTE* gs = GameState(); int st = 0;
    return gs && SafeRead((DWORD)(gs + 4), &st) && (st == 2 || st == 3);
}
