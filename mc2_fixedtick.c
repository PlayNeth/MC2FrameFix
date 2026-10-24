/*
 * mc2_fixedtick.asi - fixed-timestep simulation for Midnight Club II (PC)
 *
 * Load with Ultimate ASI Loader (drop its dll in the game folder named
 * dinput8.dll or version.dll; put this .asi + .ini next to mc2.exe).
 *
 * What it does
 * ------------
 * The in-race main loop calls   Game::Update()  (0x401CB0)  once per rendered
 * frame. Update() calls datTimeManager::Update() (0x613FC0) which measures the
 * real frame time and stores it in the global "Seconds" (0x6797CC) and
 * "InvSeconds" (0x6797D4). All car physics integrate with that value, so the
 * physics step size equals the frame time and handling/top speed/damage change
 * with framerate.
 *
 * This plugin replaces the loop's call to Update() with an accumulator:
 * every frame real time is added to a bucket and Update() is run N times with
 * Seconds forced to exactly 1/SimHz. Physics therefore always integrates with
 * the same step regardless of framerate.
 *
 * Addresses are for the exe with SHA1 7dc19475132af0724139cb1916a44a42d5368bd8.
 * The plugin verifies the original bytes and does nothing on mismatch.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

/* ---- game addresses ---------------------------------------------------- */
#define ADDR_CALL_UPDATE_RACE  0x004021AB  /* call Game::Update in race loop      */
#define ADDR_CALL_TIMER        0x00401CE1  /* call datTimeManager::Update in Update */
#define FN_GAME_PRE_UPDATE     0x00401960  /* thiscall, paired with POST          */
#define FN_GAME_UPDATE         0x00401CB0  /* thiscall                            */
#define FN_GAME_POST_UPDATE    0x004019B0  /* thiscall (ignores this)             */
#define FN_TIMER_UPDATE        0x00613FC0  /* cdecl, no args                      */

#define G_SECONDS      (*(volatile float*)0x006797CC)
#define G_INVSECONDS   (*(volatile float*)0x006797D4)
#define G_TIMESCALE    (*(volatile float*)0x006797F0)  /* slow-mo multiplier      */
#define G_MAXSECONDS   (*(volatile float*)0x006797E0)
#define G_MINSECONDS   (*(volatile float*)0x006797E4)
#define G_TIMER_RESET  (*(volatile uint8_t*)0x006797EC) /* one-shot after load    */
#define G_TIMER_FIXED  (*(volatile int32_t*)0x008602F4) /* 1 = fixed-frame mode   */
#define G_TIMER_FREEZE (*(volatile uint8_t*)0x008602F1)
#define G_GAMESTATE_PTR (*(uint8_t* volatile*)0x006C3890) /* +4 state, +9 pause flag */

typedef void (__thiscall *ThisFn)(void *self);
typedef void (__cdecl *VoidFn)(void);

/* ---- config -------------------------------------------------------------- */
static int    cfg_enabled   = 1;
static double cfg_simhz     = 60.0;
static int    cfg_maxsteps  = 8;
static double cfg_fpslimit  = 0.0;
static int    cfg_log       = 1;

/* ---- state ---------------------------------------------------------------- */
static LARGE_INTEGER qpf, last_frame, limiter_next;
static int    have_last = 0;
static double accum = 0.0;
static int    forcing = 0;        /* set while we run a fixed step */
static float  forced_dt = 1.0f / 60.0f;
static FILE  *logf_ = NULL;

static void logmsg(const char *fmt, ...)
{
    if (!cfg_log) return;
    if (!logf_) logf_ = fopen("mc2_fixedtick.log", "w");
    if (!logf_) return;
    va_list ap; va_start(ap, fmt); vfprintf(logf_, fmt, ap); va_end(ap);
    fputc('\n', logf_); fflush(logf_);
}

static double qpc_seconds(LARGE_INTEGER a, LARGE_INTEGER b)
{
    return (double)(b.QuadPart - a.QuadPart) / (double)qpf.QuadPart;
}

static void load_config(HMODULE self)
{
    char path[MAX_PATH], *p;
    GetModuleFileNameA(self, path, MAX_PATH);
    p = strrchr(path, '.');
    if (p) strcpy(p, ".ini"); else strcat(path, ".ini");
    char buf[64];
    cfg_enabled  = GetPrivateProfileIntA("FixedTick", "Enabled", 1, path);
    GetPrivateProfileStringA("FixedTick", "SimHz", "60", buf, sizeof buf, path);
    cfg_simhz    = atof(buf);
    cfg_maxsteps = GetPrivateProfileIntA("FixedTick", "MaxStepsPerFrame", 8, path);
    GetPrivateProfileStringA("FixedTick", "FPSLimit", "0", buf, sizeof buf, path);
    cfg_fpslimit = atof(buf);
    cfg_log      = GetPrivateProfileIntA("FixedTick", "Log", 1, path);
    if (cfg_simhz < 10.0)  cfg_simhz = 10.0;
    if (cfg_simhz > 1000.0) cfg_simhz = 1000.0;
    if (cfg_maxsteps < 1)  cfg_maxsteps = 1;
    if (cfg_maxsteps > 64) cfg_maxsteps = 64;
    forced_dt = (float)(1.0 / cfg_simhz);
}

/* ---- optional frame limiter ------------------------------------------- */
static void frame_limit(void)
{
    if (cfg_fpslimit <= 0.0) return;
    static int period_set = 0;
    if (!period_set) { timeBeginPeriod(1); period_set = 1; }
    LARGE_INTEGER now;
    LONGLONG period = (LONGLONG)((double)qpf.QuadPart / cfg_fpslimit);
    QueryPerformanceCounter(&now);
    if (limiter_next.QuadPart == 0 || now.QuadPart - limiter_next.QuadPart > period * 4)
        limiter_next.QuadPart = now.QuadPart;           /* (re)sync after hitches */
    for (;;) {
        QueryPerformanceCounter(&now);
        LONGLONG left = limiter_next.QuadPart - now.QuadPart;
        if (left <= 0) break;
        double ms = (double)left * 1000.0 / (double)qpf.QuadPart;
        if (ms > 2.0) Sleep((DWORD)(ms - 1.5)); else YieldProcessor();
    }
    limiter_next.QuadPart += period;
}

/* ---- hook: datTimeManager::Update inside Game::Update ------------------- */
static void __cdecl hook_timer_update(void)
{
    /* Respect special timer paths: post-load reset, fixed-frame mode
       (-framerate arg), and frozen time. */
    int special = G_TIMER_RESET || G_TIMER_FIXED || G_TIMER_FREEZE;

    ((VoidFn)FN_TIMER_UPDATE)();     /* keep all of the game's bookkeeping */

    if (forcing && !special) {
        float dt = forced_dt * G_TIMESCALE;           /* honour slow-mo */
        if (dt < G_MINSECONDS) dt = G_MINSECONDS;
        if (G_MAXSECONDS > 0.0f && dt > G_MAXSECONDS) dt = G_MAXSECONDS;
        G_SECONDS    = dt;
        G_INVSECONDS = 1.0f / dt;
    }
}

static int race_state_ok(void)
{
    uint8_t *gs = G_GAMESTATE_PTR;
    if (!gs) return 0;
    int st = *(int32_t*)(gs + 4);
    return (st == 2 || st == 3);
}

/* ---- hook: Game::Update call in the race loop ---------------------------- */
/* __fastcall gives us ecx = this; edx unused; no stack args -> ABI-compatible
   with the original __thiscall call site. */
static void __fastcall hook_race_update(void *self, void *edx_unused)
{
    (void)edx_unused;
    LARGE_INTEGER now;

    if (!cfg_enabled) { ((ThisFn)FN_GAME_UPDATE)(self); return; }

    frame_limit();
    QueryPerformanceCounter(&now);

    double step = 1.0 / cfg_simhz;
    double frame = have_last ? qpc_seconds(last_frame, now) : step;
    last_frame = now; have_last = 1;

    /* After loads / alt-tab the frame time is huge: don't try to catch up. */
    if (frame > 0.25 || G_TIMER_RESET) { frame = step; accum = 0.0; }
    if (frame < 0.0) frame = 0.0;

    accum += frame;
    int steps = (int)floor(accum / step);
    if (steps > cfg_maxsteps) { steps = cfg_maxsteps; accum = 0.0; }
    else accum -= steps * step;

    /* Paused / menu overlay: the game skips its own timer in that case;
       just run one normal update so menus stay responsive. */
    uint8_t *gs = G_GAMESTATE_PTR;
    if (gs && gs[9]) { ((ThisFn)FN_GAME_UPDATE)(self); accum = 0.0; return; }

    for (int i = 0; i < steps; i++) {
        if (i > 0) {
            if (!race_state_ok()) break;            /* race ended mid-frame */
            ((ThisFn)FN_GAME_POST_UPDATE)(self);    /* close previous tick  */
            ((ThisFn)FN_GAME_PRE_UPDATE)(self);     /* open next tick       */
        }
        forcing = 1;
        ((ThisFn)FN_GAME_UPDATE)(self);
        forcing = 0;
    }
    /* steps == 0: render rate is above SimHz, nothing to simulate this frame;
       the loop redraws the current state. */
}

/* ---- patching ---------------------------------------------------------- */
static int check_call(uintptr_t at, uintptr_t expect_target)
{
    uint8_t *p = (uint8_t*)at;
    if (p[0] != 0xE8) return 0;
    int32_t rel = *(int32_t*)(p + 1);
    return (uintptr_t)(at + 5 + rel) == expect_target;
}

static void write_call(uintptr_t at, void *target)
{
    DWORD old;
    VirtualProtect((void*)at, 5, PAGE_EXECUTE_READWRITE, &old);
    *(uint8_t*)at = 0xE8;
    *(int32_t*)(at + 1) = (int32_t)((uintptr_t)target - (at + 5));
    VirtualProtect((void*)at, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, 5);
}

static int try_install(void)
{
    if (!check_call(ADDR_CALL_UPDATE_RACE, FN_GAME_UPDATE) ||
        !check_call(ADDR_CALL_TIMER, FN_TIMER_UPDATE))
        return 0;
    write_call(ADDR_CALL_TIMER, (void*)hook_timer_update);
    write_call(ADDR_CALL_UPDATE_RACE, (void*)hook_race_update);
    return 1;
}

static DWORD WINAPI install_thread(LPVOID unused)
{
    (void)unused;
    /* The exe has a loader stub (.grxdd); give it up to 10 s to settle. */
    for (int i = 0; i < 1000; i++) {
        if (try_install()) {
            logmsg("hooks installed: SimHz=%.2f MaxSteps=%d FPSLimit=%.1f",
                   cfg_simhz, cfg_maxsteps, cfg_fpslimit);
            return 0;
        }
        Sleep(10);
    }
    logmsg("ERROR: code at hook sites doesn't match the supported mc2.exe "
           "(SHA1 7dc19475...). Nothing was patched.");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        QueryPerformanceFrequency(&qpf);
        load_config(inst);
        if (!cfg_enabled) { logmsg("disabled in ini"); return TRUE; }
        if (try_install())
            logmsg("hooks installed: SimHz=%.2f MaxSteps=%d FPSLimit=%.1f",
                   cfg_simhz, cfg_maxsteps, cfg_fpslimit);
        else
            CloseHandle(CreateThread(NULL, 0, install_thread, NULL, 0, NULL));
    }
    return TRUE;
}
