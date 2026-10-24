// MC2FrameFix.asi
// Fixed-timestep simulation and frame interpolation for Midnight Club II (PC).
// Version history: CHANGELOG.md.
//
// Analysed exe: mc2.exe SHA-1 7dc19475132af0724139cb1916a44a42d5368bd8
// (image base 0x400000, no relocations used). Every patch site is checked
// against the expected bytes first; on any other exe nothing is patched.
//
// How the game keeps time
// -----------------------
//  - The race loop (state 2/3, 0x40219D) calls Game::Update (0x401CB0) once per
//    rendered frame, then Draw (0x4019C0), then Present.
//  - Update calls datTimeManager::Update (0x613FC0). In races the timer runs in
//    "realtime" mode (set up at 0x402F70): Seconds (0x6797CC) = real frame time,
//    clamped to 0.0001..0.04 s. Everything integrates with Seconds, including
//    the car physics, so the physics step equals the frame time and handling,
//    top speed, burnout boost and damage change with frame rate.
//
// What this ASI does
// ------------------
//  - fixed step: the loop's Update call (0x4021AB) is replaced by an
//    accumulator that runs Update N times per frame with Seconds forced to
//    exactly 1/simHz (the slow-motion scale 0x6797F0 is respected). Physics
//    therefore integrates with the same step at any frame rate.
//  - interpolation: after every step, the world matrices of
//      * every active physics collider (level [0x6C8EB8]: active list +0x8A/+0x8C,
//        objects +0x34 stride 0x280, collider at +8; its world matrix pointer
//        at collider+0x14, and the rigid body's own matrix at ICS+0x4C),
//      * every ambient traffic vehicle (manager [0x69947C], array +0x10C,
//        count +0x108, stride 0x1B4; drawn from vehicle+0x34, physics +0xC4),
//      * the active camera of every camViewCS (camera matrix +0x0C, FOV +0x7C)
//    are snapshotted. Before Draw they are written blended between the last two
//    steps (position lerp, rotation slerp) and the exact step values go back
//    right after Draw, so the simulation never sees blended data.
//  - Car models (vehModel::Draw, vtable 0x6443DC slot 1) and traffic
//    (vtable 0x646EC4 slot 1) also draw parts from matrices cached during the
//    step (wheels, shadow, ...). While one of them draws, its body gets its
//    exact step matrix back and every world matrix it sets (SetWorld34 0x5EDB20,
//    SetWorld44 0x5EDB00) is moved by delta = inverse(step) * blended, so the
//    whole car moves as one.
//  - Cameras are blended relative to the car they follow (the moving physics
//    object they move least against), or in world space if that moves less.
//    A cut (new camera, look back, jump) rides on that car's blend at its step
//    pose until it has two steps; the world stays blended.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define MC2FF_VERSION "0.4.3"

// ---------------------------------------------------------------------------
// Game addresses
// ---------------------------------------------------------------------------
namespace mc2 {
    // loop
    const DWORD callUpdateRace = 0x4021AB;   // call Game::Update
    const DWORD callDrawRace   = 0x402223;   // call Game::Draw
    const DWORD callTimer      = 0x401CE1;   // call datTimeManager::Update (inside Update)
    const DWORD callPreRace    = 0x4021A4;   // call pre-update, before Update
    const DWORD callPostRace   = 0x40222A;   // call post-update, after Draw
    const DWORD callPostRace2  = 0x4021E8;   // call post-update, device-lost path (no Draw)
    const DWORD fnPreUpdate    = 0x401960;   // thiscall
    const DWORD fnUpdate       = 0x401CB0;   // thiscall
    const DWORD fnPostUpdate   = 0x4019B0;   // thiscall
    const DWORD fnDraw         = 0x4019C0;   // thiscall
    const DWORD fnTimer        = 0x613FC0;   // cdecl
    // timer globals
    const DWORD pSeconds = 0x6797CC, pInvSeconds = 0x6797D4, pTimeScale = 0x6797F0,
                pMaxSec = 0x6797E0, pMinSec = 0x6797E4, pTimerReset = 0x6797EC,
                pTimerFixed = 0x8602F4, pTimerFreeze = 0x8602F1;
    const DWORD pGameState = 0x6C3890;       // ptr; +4 state, +9 paused/menu
    // world
    const DWORD pPhysLevel = 0x6C8EB8;
    const DWORD pAmbient   = 0x69947C;
    // render
    const DWORD fnSetWorld34 = 0x5EDB20;     // cdecl(const Matrix34*)
    const DWORD fnSetWorld44 = 0x5EDB00;     // cdecl(const Matrix44*)
    const DWORD fnM34to44    = 0x5DE230;     // cdecl(Matrix44* dst, const Matrix34* src)
    const DWORD fnM44Copy    = 0x609D00;     // thiscall(this=dst, const Matrix44* src), ret 4
    const DWORD fnFlushWorld = 0x5EDAE0;     // SetTransform(WORLD, 0x8584C0)
    const DWORD gWorld44     = 0x8584C0;
    const DWORD vtCarDraw    = 0x6443E0;     // vehModel vtable slot 1 -> 0x4CAC00
    const DWORD fnCarDraw    = 0x4CAC00;
    const DWORD vtAmbDraw    = 0x646EC8;     // ambient renderable slot 1 -> 0x4ECB50
    const DWORD fnAmbDraw    = 0x4ECB50;
    const DWORD vtViewSetCam = 0x651B54;     // camViewCS slot 10 -> 0x57C360
    const DWORD fnViewSetCam = 0x57C360;
    // glows/flares: queued as world-space sprites from a matrix argument
    // (all thiscall on the light manager [0x6C2640]; entry is "sub esp, imm32")
    struct LightFn { DWORD fn; DWORD subImm; int arg; };
    const LightFn lightFns[] = {
        { 0x51CEA0, 0xCC,  2 },   // light list: (a1, const Matrix34* m, count, localPos*, ...)
        { 0x51CB00, 0xC8,  1 },   // (const Matrix34* m, ...)
        { 0x51C6F0, 0xCC,  1 },
        { 0x51C8A0, 0xD8,  1 },   // flare (also via 0x51CE60)
        { 0x51F1F0, 0xE0,  1 },   // (also via 0x51F4B0); calls 0x51CFF0 itself
        { 0x51CFF0, 0xD0,  1 },
        { 0x51EF00, 0x114, 1 },   // (via 0x51F4D0)
    };
    // pedestrians
    const DWORD pPedMgr      = 0x6C38B8;     // 26 peds at +0x18, extra list +0xE4 (count +0x90)
    const DWORD vtPedDraw    = 0x64EA7C;     // ped model (ped+0x110) slot 1 -> 0x5425D0
    const DWORD fnPedDraw    = 0x5425D0;
}

typedef void (__fastcall *ThisFn)(void* self, void* edx);
typedef void (__fastcall *DrawFn)(void* self, void* edx, int pass);
typedef void (__cdecl *VoidFn)();
typedef void (__cdecl *M34to44Fn)(void* dst, const void* src);
typedef void (__fastcall *M44CopyFn)(void* dst, void* edx, const void* src);

template <class T> static inline T& G(DWORD a) { return *reinterpret_cast<T*>(a); }

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
struct Config {
    int    enabled = 1;
    double simHz = 30.0;
    int    maxSteps = 8;
    double fpsLimit = 0.0;
    int    interpolate = 1;
    int    interpCamera = 1;
    int    interpModels = 1;     // delta for car / traffic parts
    int    interpTraffic = 1;
    int    interpPhysics = 1;    // all active colliders (cars, props)
    int    interpPeds = 1;
    int    interpGlows = 1;      // car / traffic light glows follow the blended body
    int    profileAudio = 0;     // diagnostic: time Miles / DirectMusic calls
    int    asyncStreams = 1;     // open Miles streams on a worker thread
    int    fixExitCrash = 1;     // null check in the debug-font shutdown (0x5F9D70)
    double dopplerHoldOnSwitch = 0.5;   // s of frozen Doppler after a camera change (0 = off)
    int    traceCameraAudio = 0;        // diagnostic: log voice play/stop around camera switches
    int    fixCameraCutAudio = 2;       // ticks the listener keeps its pose after a camera change
    double lodDistanceScale = 2.0;      // car / traffic LOD switch distances x this
    double ambientDistance = 300.0;     // m beyond an object's radius it is still drawn (game: 150)
    double spawnDistanceScale = 2.0;    // traffic / ped population area around the player x this
    float  maxStepDistance = 12.0f;  // metres per step
    float  maxStepAngle = 60.0f;     // degrees per step (objects)
    float  maxCamAngle = 45.0f;      // degrees per step (camera direction)
    int    log = 1;
    double statsSeconds = 10.0;
};
static Config g_cfg;
static char g_asiDir[MAX_PATH];
static char g_iniPath[MAX_PATH];

static const char kDefaultIni[] =
"; MC2FrameFix - fixed-timestep physics and interpolation for Midnight Club II\n"
"[MC2FrameFix]\n"
"; 1 = on, 0 = game runs exactly like vanilla\n"
"enabled=1\n"
"\n"
"; Physics/simulation rate. Physics behaves identically at any fps for a given\n"
"; value. 30 = the rate the game was tuned for (console). 60 = original PC feel\n"
"; on a 60 Hz monitor. With interpolation on, frames are smooth either way.\n"
"; Range 25-1000 (the game clamps a step to 0.04 s).\n"
"simHz=30\n"
"\n"
"; Max simulation steps per frame (prevents a slow-motion spiral on a slow PC)\n"
"maxStepsPerFrame=8\n"
"\n"
"; Frame limiter, 0 = off. Use it with vsync OFF, set to your refresh rate.\n"
"; With vsync on, leave it at 0: the limiter and vsync fight and frames stutter.\n"
"fpsLimit=0\n"
"\n"
"; Draw frames between simulation steps (smooth motion above simHz)\n"
"interpolate=1\n"
"; Blend the camera too\n"
"interpolateCamera=1\n"
"; Move car / traffic parts (wheels, shadows) with the blended body\n"
"interpolateModels=1\n"
"; Ambient traffic\n"
"interpolateTraffic=1\n"
"; Every active physics object (cars, opponents, knocked props)\n"
"interpolatePhysics=1\n"
"; Pedestrians\n"
"interpolatePeds=1\n"
"; Light glows (tail/head lights) follow the blended car\n"
"interpolateGlows=1\n"

"\n"
"; A step longer than this (metres) or turning more than this (degrees) is\n"
"; treated as a teleport/reset and drawn unblended\n"
"maxStepDistance=12\n"
"maxStepAngle=60\n"
"; Camera turning more than this per step counts as a cut (look back, switch)\n"
"maxCameraAngle=45\n"
"\n"
"; Open audio streams (city ambience, voices) on a worker thread. The game\n"
"; opens them on the main thread and each cold open stalls a frame ~100 ms.\n"
"asyncStreams=1\n"
"\n"
"; Fix a crash on exit (the game frees its debug font twice)\n"
"fixExitCrash=1\n"
"\n"
"; Seconds to hold the Doppler pitch after a camera switch. The game derives\n"
"; Doppler from the camera's movement, and the switch swoop makes engine sounds\n"
"; warble (scratching). 0 = original behaviour.\n"
"dopplerHoldOnSwitch=0.5\n"
"\n"
"; Updates the listener keeps its last position after a camera cut. The game\n"
"; hands the new camera to the sound before it has moved, the engine voices\n"
"; fall silent and restart from the beginning (audio cut-off). 0 = original.\n"
"fixCameraCutAudio=2\n"
"\n"
"; Distance at which city objects (trees, props), cars and traffic switch to\n"
"; lower-detail models,"
" as a multiple of the game's own (1 = original, 2 = twice as far)\n"
"lodDistanceScale=2.0\n"
"\n"
"; How far (metres, beyond the object's size) traffic, pedestrians, trains and\n"
"; props are still drawn. The game uses 150.\n"
"ambientDistance=300\n"
"\n"
"; Size of the area around you where traffic and pedestrians are created, as a\n"
"; multiple of the game's (175 / 275 / 350 m bands). 1 = original. The same\n"
"; number of cars and people is spread over the larger area.\n"
"spawnDistanceScale=2.0\n"
"\n"
"; Diagnostic: time every Miles / DirectMusic call and log what stalls frames\n"
"profileAudio=0\n"
"\n"
"; Diagnostic: log which sounds are started / stopped around camera switches\n"
"traceCameraAudio=0\n"
"\n"
"; Log file (MC2FrameFix.log) and how often stats are written, in seconds\n"
"log=1\n"
"statsSeconds=10\n";

static double IniD(const char* k, double def) {
    char b[64], d[64]; snprintf(d, sizeof d, "%g", def);
    GetPrivateProfileStringA("MC2FrameFix", k, d, b, sizeof b, g_iniPath);
    return atof(b);
}
static int IniI(const char* k, int def) { return GetPrivateProfileIntA("MC2FrameFix", k, def, g_iniPath); }

static void LoadConfig(HMODULE self) {
    GetModuleFileNameA(self, g_asiDir, MAX_PATH);
    char* s = strrchr(g_asiDir, '\\'); if (s) s[1] = 0; else g_asiDir[0] = 0;
    snprintf(g_iniPath, MAX_PATH, "%sMC2FrameFix.ini", g_asiDir);
    if (GetFileAttributesA(g_iniPath) == INVALID_FILE_ATTRIBUTES) {
        if (FILE* f = fopen(g_iniPath, "w")) { fputs(kDefaultIni, f); fclose(f); }
    }
    g_cfg.enabled        = IniI("enabled", 1);
    g_cfg.simHz          = IniD("simHz", 30.0);
    g_cfg.maxSteps       = IniI("maxStepsPerFrame", 8);
    g_cfg.fpsLimit       = IniD("fpsLimit", 0.0);
    g_cfg.interpolate    = IniI("interpolate", 1);
    g_cfg.interpCamera   = IniI("interpolateCamera", 1);
    g_cfg.interpModels   = IniI("interpolateModels", 1);
    g_cfg.interpTraffic  = IniI("interpolateTraffic", 1);
    g_cfg.interpPhysics  = IniI("interpolatePhysics", 1);
    g_cfg.interpPeds     = IniI("interpolatePeds", 1);
    g_cfg.interpGlows    = IniI("interpolateGlows", 1);
    g_cfg.profileAudio   = IniI("profileAudio", 0);
    g_cfg.asyncStreams   = IniI("asyncStreams", 1);
    g_cfg.fixExitCrash   = IniI("fixExitCrash", 1);
    g_cfg.dopplerHoldOnSwitch = IniD("dopplerHoldOnSwitch", 0.5);
    g_cfg.traceCameraAudio = IniI("traceCameraAudio", 0);
    g_cfg.fixCameraCutAudio = IniI("fixCameraCutAudio", 2);
    g_cfg.lodDistanceScale = IniD("lodDistanceScale", 2.0);
    g_cfg.ambientDistance = IniD("ambientDistance", 300.0);
    g_cfg.spawnDistanceScale = IniD("spawnDistanceScale", 2.0);
    if (!(g_cfg.spawnDistanceScale >= 0.5)) g_cfg.spawnDistanceScale = 0.5;
    if (g_cfg.spawnDistanceScale > 8.0) g_cfg.spawnDistanceScale = 8.0;
    if (!(g_cfg.ambientDistance >= 50.0)) g_cfg.ambientDistance = 50.0;
    if (g_cfg.ambientDistance > 3000.0) g_cfg.ambientDistance = 3000.0;
    if (!(g_cfg.lodDistanceScale >= 0.25)) g_cfg.lodDistanceScale = 0.25;
    if (g_cfg.lodDistanceScale > 20.0) g_cfg.lodDistanceScale = 20.0;
    if (g_cfg.fixCameraCutAudio < 0) g_cfg.fixCameraCutAudio = 0;
    if (g_cfg.fixCameraCutAudio > 8) g_cfg.fixCameraCutAudio = 8;
    g_cfg.maxStepDistance = (float)IniD("maxStepDistance", 12.0);
    g_cfg.maxStepAngle   = (float)IniD("maxStepAngle", 60.0);
    g_cfg.maxCamAngle    = (float)IniD("maxCameraAngle", 45.0);
    g_cfg.log            = IniI("log", 1);
    g_cfg.statsSeconds   = IniD("statsSeconds", 10.0);
    // the race timer clamps a step to 0.04 s (0x402F70): a slower rate would run
    // the game in slow motion
    if (g_cfg.simHz < 25) g_cfg.simHz = 25;
    if (g_cfg.simHz > 1000) g_cfg.simHz = 1000;
    if (g_cfg.maxSteps < 1) g_cfg.maxSteps = 1;
    if (g_cfg.maxSteps > 64) g_cfg.maxSteps = 64;
    if (g_cfg.statsSeconds < 1) g_cfg.statsSeconds = 1;
}

// ---------------------------------------------------------------------------
// Log
// ---------------------------------------------------------------------------
static FILE* g_log = nullptr;
static void Logf(const char* fmt, ...) {
    if (!g_cfg.log) return;
    if (!g_log) {
        char p[MAX_PATH]; snprintf(p, MAX_PATH, "%sMC2FrameFix.log", g_asiDir);
        g_log = fopen(p, "w");
        if (!g_log) return;
    }
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fflush(g_log);
}

// ---------------------------------------------------------------------------
// Patch helpers
// ---------------------------------------------------------------------------
static bool CheckBytes(DWORD at, const void* exp, size_t n) {
    return !IsBadReadPtr((void*)at, n) && !memcmp((void*)at, exp, n);
}
static bool CheckCall(DWORD at, DWORD target) {
    const BYTE* p = (const BYTE*)at;
    if (IsBadReadPtr(p, 5) || p[0] != 0xE8) return false;
    return at + 5 + *(const int32_t*)(p + 1) == target;
}
static void WriteRel(DWORD at, BYTE op, const void* target) {
    DWORD old; VirtualProtect((void*)at, 5, PAGE_EXECUTE_READWRITE, &old);
    *(BYTE*)at = op;
    *(int32_t*)(at + 1) = (int32_t)((DWORD)target - (at + 5));
    VirtualProtect((void*)at, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, 5);
}
static void WriteNops(DWORD at, int n) {
    DWORD old; VirtualProtect((void*)at, n, PAGE_EXECUTE_READWRITE, &old);
    memset((void*)at, 0x90, n);
    VirtualProtect((void*)at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, n);
}
static bool PatchVtable(DWORD slot, DWORD expect, const void* fn) {
    if (IsBadReadPtr((void*)slot, 4) || G<DWORD>(slot) != expect) return false;
    DWORD old; VirtualProtect((void*)slot, 4, PAGE_EXECUTE_READWRITE, &old);
    G<DWORD>(slot) = (DWORD)fn;
    VirtualProtect((void*)slot, 4, old, &old);
    return true;
}
template <class T> static bool SafeRead(DWORD a, T* out) {
    if (!a || IsBadReadPtr((void*)a, sizeof(T))) return false;
    *out = *(const T*)a; return true;
}

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------
static LARGE_INTEGER g_qpf;
static LONGLONG QpcNow() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
static LONGLONG g_lastFrame = 0, g_limNext = 0;
static double   g_accum = 0.0;
static bool     g_forcing = false;
static float    g_alpha = 0.0f;       // blend position between prev and cur step
static bool     g_frameSimOk = false; // this frame ran the fixed-step path
static unsigned long g_step = 0;      // fixed steps completed (snapshot index)
static double   g_drawDt = 0.0;       // display time since the previous frame (s)

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
static LONGLONG FrameTime() { return g_cfg.fpsLimit > 0.0 ? FrameLimit() : SmoothedNow(); }

static void __cdecl Hook_Timer() {
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

static BYTE* GameState() { BYTE* gs = nullptr; SafeRead(mc2::pGameState, (DWORD*)&gs); return gs; }
static bool RaceState() {
    BYTE* gs = GameState(); int st = 0;
    return gs && SafeRead((DWORD)(gs + 4), &st) && (st == 2 || st == 3);
}

// ---------------------------------------------------------------------------
// Interpolation: tracked transforms
// ---------------------------------------------------------------------------
enum { KIND_PHYS = 1, KIND_AMB = 2, KIND_CAM = 3, KIND_PED = 4 };
struct Ent {
    DWORD addr;           // matrix address (camera: camera object)
    BYTE  kind;
    int   n;              // floats: 12, camera 13
    unsigned long prevStep, curStep;
    float prev[13], cur[13], bl[13];
    bool  wrote;          // blended value written for this frame
    bool  handoff;        // camera: prev is the view's previous camera at the last step
};
static const int kMaxEnts = 1024;              // power of two
static Ent  g_ents[kMaxEnts];
static int  g_entUsed = 0;
static Ent* EntFind(DWORD addr) {
    unsigned h = (addr >> 2) * 2654435761u;
    for (int i = 0; i < kMaxEnts; i++) {
        Ent& e = g_ents[(h + i) & (kMaxEnts - 1)];
        if (e.addr == addr) return &e;
        if (!e.addr) return nullptr;
    }
    return nullptr;
}
static Ent* EntGet(DWORD addr, BYTE kind, int n) {
    unsigned h = (addr >> 2) * 2654435761u;
    for (int i = 0; i < kMaxEnts; i++) {
        Ent& e = g_ents[(h + i) & (kMaxEnts - 1)];
        if (e.addr == addr) return &e;
        if (!e.addr) {
            if (g_entUsed > kMaxEnts * 3 / 4) return nullptr;
            memset(&e, 0, sizeof e); e.addr = addr; e.kind = kind; e.n = n; g_entUsed++;
            return &e;
        }
    }
    return nullptr;
}
static void EntClear() { memset(g_ents, 0, sizeof g_ents); g_entUsed = 0; }

// registered camViewCS objects (seen at render time)
static const int kMaxViews = 4;
static DWORD g_views[kMaxViews]; static unsigned long g_viewSeen[kMaxViews]; static int g_nViews = 0;
static unsigned long g_frameNo = 0;

// statistics
static unsigned long st_frames, st_steps, st_blendFrames, st_cutFrames, st_written, st_mismatch,
    st_jump, st_restoreSkip, st_carDraws, st_carDelta, st_carNoEnt, st_ambDraws, st_ambDelta,
    st_worldDelta, st_camBlend, st_camRel, st_camAnchor, st_camHandoff, st_zeroStepFrames, st_multiStepFrames, st_entsLastStep,
    st_pedDraws, st_pedDelta, st_glowDelta, st_slowFrames, st_jumpLogged,
    st_ftHist[7], st_spfHist[4];   // frame interval buckets; steps per frame 0/1/2/3+
static double st_alphaSum, st_ftMin = 1e9, st_ftMax = 0;

static bool Finite(const float* v, int n) { for (int i = 0; i < n; i++) if (!std::isfinite(v[i]) || fabsf(v[i]) > 1e7f) return false; return true; }
static bool IsRotation(const float* m) {
    for (int r = 0; r < 3; r++) {
        float l = m[r*3]*m[r*3] + m[r*3+1]*m[r*3+1] + m[r*3+2]*m[r*3+2];
        if (fabsf(l - 1.0f) > 0.05f) return false;
    }
    float det = m[0]*(m[4]*m[8]-m[5]*m[7]) - m[1]*(m[3]*m[8]-m[5]*m[6]) + m[2]*(m[3]*m[7]-m[4]*m[6]);
    return fabsf(det - 1.0f) < 0.1f;
}

// Record one snapshot. The same address can be reached twice in a step (an
// ambient vehicle's matrix is also its collider's matrix): only the first
// read counts, or the second would wipe the previous step.
static void Record(Ent* e, const float* v, int n) {
    if (e->curStep == g_step) return;
    if (e->curStep && e->curStep == g_step - 1) { memcpy(e->prev, e->cur, n * 4); e->prevStep = e->curStep; }
    else e->prevStep = 0;
    memcpy(e->cur, v, n * 4); e->curStep = g_step;
}

static void Track(DWORD addr, BYTE kind) {
    if (!addr || IsBadReadPtr((void*)addr, 48)) return;
    Ent* e = EntGet(addr, kind, 12);
    if (!e) return;
    float v[12]; memcpy(v, (void*)addr, 48);
    // only real rigid transforms: a non-matrix field must never be lerped
    if (!Finite(v, 12) || !IsRotation(v)) { e->curStep = 0; e->prevStep = 0; return; }
    Record(e, v, 12);
}

static bool ViewLive(int i) { return g_frameNo - g_viewSeen[i] <= 2; }   // drawn in the last frames

// A camera switch with the game's swoop changes the view's camera object twice:
// to a transition camera (0x57C7A0, which starts as a copy of the old camera)
// and, when it ends, to the new camera (where the transition ended). Both are
// continuous with the camera before, so the new object's previous pose is
// taken from the view's previous camera at the last step ("handoff") and the
// step is blended instead of held. A real cut (look back, an instant switch)
// is told apart by how far the camera moved (BlendCamera).
static DWORD g_viewCam[kMaxViews];
static void TrackCamera(int vi) {
    DWORD cam = 0;
    if (!SafeRead(g_views[vi] + 0x2C, &cam) || !cam || IsBadReadPtr((void*)(cam + 0x0C), 0x74)) return;
    Ent* e = EntGet(cam, KIND_CAM, 13);
    if (!e || e->curStep == g_step) return;
    float v[13]; memcpy(v, (void*)(cam + 0x0C), 48); v[12] = G<float>(cam + 0x7C);
    DWORD old = g_viewCam[vi]; g_viewCam[vi] = cam;
    if (!Finite(v, 13)) { e->curStep = 0; e->prevStep = 0; return; }
    Record(e, v, 13);
    e->handoff = false;
    if (old && old != cam && e->prevStep != g_step - 1) {
        Ent* o = EntFind(old);
        if (o && o->kind == KIND_CAM && o->curStep == g_step - 1) {
            memcpy(e->prev, o->cur, sizeof e->prev); e->prevStep = g_step - 1; e->handoff = true;
        }
    }
}

// --- ped skeleton poses ---------------------------------------------------------
// A ped's skinned mesh is drawn from a world-space bone palette (the draw sets
// an identity world matrix 0x6796F0). The skeleton (model+0x20, up to 22
// bones, count = first byte of its data [model+0x24]) keeps two palettes:
// the skeleton update (0x5DE460) writes the new pose into [model+0x2C] and the
// ped sets model+0x71; the draw draws [model+0x2C] and then swaps it with
// [model+0x38] (0x541BE0). With one update per frame the newest pose is always
// drawn; with frames without a step the swapped-in older palette is drawn
// every other frame, and the ped's limbs jump back and forth. So the newest
// pose is copied after every step ([+0x2C] if a swap is pending, else [+0x38])
// and the draw gets a palette blended bone by bone between the last two steps.
static const int kPedBones = 22;
static const int kMaxPeds = 64;
struct PedPose {
    DWORD model; int bones; unsigned long prevStep, curStep;
    float prev[kPedBones * 12], cur[kPedBones * 12], bl[kPedBones * 12];
};
static PedPose g_peds[kMaxPeds];
static PedPose* PedFind(DWORD model, bool create) {
    PedPose* freeSlot = nullptr;
    for (int i = 0; i < kMaxPeds; i++) {
        if (g_peds[i].model == model) return &g_peds[i];
        if (!freeSlot && (!g_peds[i].model || g_peds[i].curStep + 64 < g_step)) freeSlot = &g_peds[i];
    }
    if (!create || !freeSlot) return nullptr;
    memset(freeSlot, 0, sizeof *freeSlot); freeSlot->model = model;
    return freeSlot;
}
static void SnapshotPedPose(DWORD model) {
    BYTE pending = 0, bones = 0; DWORD buf = 0, data = 0;
    if (!SafeRead(model + 0x71, &pending)) return;
    if (!SafeRead(model + 0x24, &data) || !SafeRead(data, &bones) || bones < 1 || bones > kPedBones) return;
    if (!SafeRead(model + (pending ? 0x2C : 0x38), &buf) || !buf) return;   // newest pose
    if (IsBadReadPtr((void*)buf, bones * 48)) return;
    const float* v = (const float*)buf;
    if (!Finite(v, bones * 12) || !IsRotation(v)) return;                   // bone 0 must be a transform
    PedPose* p = PedFind(model, true);
    if (!p || p->curStep == g_step) return;
    if (p->curStep && p->curStep == g_step - 1 && p->bones == bones) {
        memcpy(p->prev, p->cur, bones * 48); p->prevStep = p->curStep;
    } else p->prevStep = 0;
    memcpy(p->cur, v, bones * 48); p->curStep = g_step; p->bones = bones;
}

// Open addressing can't delete single entries: when the table gets full,
// rebuild it with only the entries seen at the last step.
static void EntCompact() {
    static Ent tmp[kMaxEnts];
    int n = 0;
    for (int i = 0; i < kMaxEnts; i++)
        if (g_ents[i].addr && g_ents[i].curStep + 1 >= g_step) tmp[n++] = g_ents[i];
    EntClear();
    for (int i = 0; i < n; i++)
        if (Ent* e = EntGet(tmp[i].addr, tmp[i].kind, tmp[i].n)) *e = tmp[i];
}

static void SnapshotStep() {
    g_step++; st_steps++;
    if (g_entUsed > kMaxEnts / 2) EntCompact();
    int n0 = 0;
    if (g_cfg.interpPhysics) {
        DWORD level = 0;
        if (SafeRead(mc2::pPhysLevel, &level) && level) {
            WORD cnt = 0, maxA = 0; DWORD list = 0, table = 0;
            if (SafeRead(level + 0x8A, &cnt) && SafeRead(level + 0x24, &maxA) &&
                SafeRead(level + 0x8C, &list) && SafeRead(level + 0x34, &table) &&
                list && table && cnt <= 512) {
                for (int i = 0; i < cnt; i++) {
                    WORD idx = 0; DWORD coll = 0, mtx = 0, ics = 0;
                    WORD state = 0;
                    if (!SafeRead(list + i * 2, &idx) || idx > maxA) continue;
                    // the game's iterator (0x587F20) drops slots whose state is < 2
                    if (!SafeRead(table + idx * 0x280 + 4, &state) || state < 2) continue;
                    if (!SafeRead(table + idx * 0x280 + 8, &coll) || !coll) continue;
                    if (SafeRead(coll + 0x14, &mtx) && mtx) { Track(mtx, KIND_PHYS); n0++; }
                    if (SafeRead(coll + 0x10, &ics) && ics && ics + 0x4C != mtx) { Track(ics + 0x4C, KIND_PHYS); n0++; }
                }
            }
        }
    }
    if (g_cfg.interpTraffic) {
        DWORD amb = 0;
        if (SafeRead(mc2::pAmbient, &amb) && amb) {
            short cnt = 0; DWORD arr = 0;
            if (SafeRead(amb + 0x108, &cnt) && SafeRead(amb + 0x10C, &arr) && arr && cnt > 0 && cnt <= 256) {
                for (int i = 0; i < cnt; i++) {
                    DWORD v = arr + i * 0x1B4;
                    Track(v + 0x34, KIND_AMB); Track(v + 0xC4, KIND_AMB); n0 += 2;
                }
            }
        }
    }
    if (g_cfg.interpPeds) {
        DWORD mgr = 0;
        if (SafeRead(mc2::pPedMgr, &mgr) && mgr) {
            auto trackPed = [&](DWORD ped) {
                WORD active = 0;
                if (!ped || !SafeRead(ped + 0x1EC, &active) || !active) return;
                Track(ped + 0x110 + 0x40, KIND_PED); n0++;          // ped model root matrix
                SnapshotPedPose(ped + 0x110);                        // and its skeleton pose
            };
            for (int i = 0; i < 26; i++) { DWORD ped = 0; if (SafeRead(mgr + 0x18 + i * 4, &ped)) trackPed(ped); }
            int n = 0; DWORD list = 0;
            if (SafeRead(mgr + 0x90, &n) && SafeRead(mgr + 0xE4, &list) && list && n > 0 && n <= 256)
                for (int i = 0; i < n; i++) { DWORD ped = 0; if (SafeRead(list + i * 4, &ped)) trackPed(ped); }
        }
    }
    if (g_cfg.interpCamera)
        for (int i = 0; i < g_nViews; i++) if (ViewLive(i)) TrackCamera(i);
    st_entsLastStep = n0;
}

// --- blending math ---------------------------------------------------------
struct Quat { float w, x, y, z; };
static Quat MatToQuat(const float* m) {
    Quat q; float tr = m[0] + m[4] + m[8];
    if (tr > 0) { float s = sqrtf(tr + 1.0f) * 2; q.w = 0.25f * s; q.x = (m[7] - m[5]) / s;
                  q.y = (m[2] - m[6]) / s; q.z = (m[3] - m[1]) / s; }
    else if (m[0] > m[4] && m[0] > m[8]) { float s = sqrtf(1.0f + m[0] - m[4] - m[8]) * 2;
                  q.w = (m[7] - m[5]) / s; q.x = 0.25f * s; q.y = (m[1] + m[3]) / s; q.z = (m[2] + m[6]) / s; }
    else if (m[4] > m[8]) { float s = sqrtf(1.0f + m[4] - m[0] - m[8]) * 2;
                  q.w = (m[2] - m[6]) / s; q.x = (m[1] + m[3]) / s; q.y = 0.25f * s; q.z = (m[5] + m[7]) / s; }
    else { float s = sqrtf(1.0f + m[8] - m[0] - m[4]) * 2;
                  q.w = (m[3] - m[1]) / s; q.x = (m[2] + m[6]) / s; q.y = (m[5] + m[7]) / s; q.z = 0.25f * s; }
    return q;
}
static void QuatToMat(const Quat& q, float* m) {
    float xx = q.x*q.x, yy = q.y*q.y, zz = q.z*q.z, xy = q.x*q.y, xz = q.x*q.z, yz = q.y*q.z,
          wx = q.w*q.x, wy = q.w*q.y, wz = q.w*q.z;
    m[0] = 1 - 2*(yy+zz); m[1] = 2*(xy-wz);     m[2] = 2*(xz+wy);
    m[3] = 2*(xy+wz);     m[4] = 1 - 2*(xx+zz); m[5] = 2*(yz-wx);
    m[6] = 2*(xz-wy);     m[7] = 2*(yz+wx);     m[8] = 1 - 2*(xx+yy);
}
// Matrix34 layout: rotation rows m[0..8], position m[9..11].
static bool BlendM34(const float* a, const float* b, float t, float* out, float maxDist, float maxAngDeg) {
    float dx = b[9]-a[9], dy = b[10]-a[10], dz = b[11]-a[11];
    if (dx*dx + dy*dy + dz*dz > maxDist * maxDist) return false;
    for (int i = 9; i < 12; i++) out[i] = a[i] + (b[i] - a[i]) * t;
    if (!memcmp(a, b, 36)) { memcpy(out, b, 36); return true; }
    if (!IsRotation(a) || !IsRotation(b)) {            // scaled/sheared: plain lerp
        for (int i = 0; i < 9; i++) out[i] = a[i] + (b[i] - a[i]) * t;
        return true;
    }
    Quat qa = MatToQuat(a), qb = MatToQuat(b);
    float d = qa.w*qb.w + qa.x*qb.x + qa.y*qb.y + qa.z*qb.z;
    if (d < 0) { qb.w = -qb.w; qb.x = -qb.x; qb.y = -qb.y; qb.z = -qb.z; d = -d; }
    if (d > 1) d = 1;
    if (2.0f * acosf(d) * 57.29578f > maxAngDeg) return false;
    Quat q;
    if (d > 0.9995f) {
        q.w = qa.w + (qb.w-qa.w)*t; q.x = qa.x + (qb.x-qa.x)*t;
        q.y = qa.y + (qb.y-qa.y)*t; q.z = qa.z + (qb.z-qa.z)*t;
    } else {
        float th = acosf(d), s = sinf(th), wa = sinf((1-t)*th)/s, wb = sinf(t*th)/s;
        q.w = qa.w*wa + qb.w*wb; q.x = qa.x*wa + qb.x*wb; q.y = qa.y*wa + qb.y*wb; q.z = qa.z*wa + qb.z*wb;
    }
    float l = sqrtf(q.w*q.w + q.x*q.x + q.y*q.y + q.z*q.z);
    if (l < 1e-6f) return false;
    q.w /= l; q.x /= l; q.y /= l; q.z /= l;
    QuatToMat(q, out);
    return true;
}
// row-vector convention: v' = v * R + p.  compose(A,B) = A then B.
static void Compose(const float* A, const float* B, float* o) {
    float r[12];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            r[i*3+j] = A[i*3]*B[j] + A[i*3+1]*B[3+j] + A[i*3+2]*B[6+j];
    for (int j = 0; j < 3; j++)
        r[9+j] = A[9]*B[j] + A[10]*B[3+j] + A[11]*B[6+j] + B[9+j];
    memcpy(o, r, sizeof r);
}
static void InvertRigid(const float* M, float* o) {
    float r[12];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) r[i*3+j] = M[j*3+i];   // transpose
    for (int j = 0; j < 3; j++) r[9+j] = -(M[9]*r[j] + M[10]*r[3+j] + M[11]*r[6+j]);
    memcpy(o, r, sizeof r);
}
static bool NearlyEqual(const float* a, const float* b, int n) {
    for (int i = 0; i < n; i++) {
        float d = fabsf(a[i] - b[i]), s = fabsf(a[i]) + fabsf(b[i]);
        if (d > 1e-5f * (1.0f + s)) return false;
    }
    return true;
}

// Camera: matrix rows are the camera axes; row 2 is the view direction (+/-),
// row 1 is up. A camera cut is a switch, a big jump, a big turn or a FOV jump.
static bool CameraJumped(const float* a, const float* b) {
    float dx = b[9]-a[9], dy = b[10]-a[10], dz = b[11]-a[11];
    if (dx*dx + dy*dy + dz*dz > g_cfg.maxStepDistance * g_cfg.maxStepDistance) return true;
    float c = a[6]*b[6] + a[7]*b[7] + a[8]*b[8];
    float cu = a[3]*b[3] + a[4]*b[4] + a[5]*b[5];
    float lim = cosf(g_cfg.maxCamAngle / 57.29578f);
    if (c < lim || cu < lim) return true;
    if (fabsf(b[12] - a[12]) > 0.2f * (fabsf(a[12]) + 1e-3f)) return true;
    return false;
}

// --- per-frame apply / restore ------------------------------------------------
static bool g_applied = false;
static bool g_pedHook = false;   // peds are only blended when their draw is delta-wrapped:
                                 // the skinned body uses step-time bones, the root alone must not move
static Ent* g_written[kMaxEnts]; static int g_nWritten = 0;   // non-camera entities written this frame
// Rotation angle (degrees) and travel between two rigid poses; Motion mixes
// them (10 degrees ~ 1 m) to compare how much a camera moves in two frames.
static float PoseAngle(const float* a, const float* b) {
    float c = (a[0]*b[0]+a[1]*b[1]+a[2]*b[2] + a[3]*b[3]+a[4]*b[4]+a[5]*b[5] + a[6]*b[6]+a[7]*b[7]+a[8]*b[8] - 1.0f) * 0.5f;
    if (c > 1) c = 1;
    if (c < -1) c = -1;
    return acosf(c) * 57.29578f;
}
static float PoseDist(const float* a, const float* b) {
    float dx = b[9]-a[9], dy = b[10]-a[10], dz = b[11]-a[11];
    return sqrtf(dx*dx + dy*dy + dz*dz);
}
static float Motion(const float* a, const float* b) { return PoseDist(a, b) + PoseAngle(a, b) * 0.1f; }

// Camera blending, after the objects are written. Most cameras follow the
// player's car (chase, hood, bumper, and the game's swoop from one to another
// when switching), so the camera is blended in the space of the nearest moving
// physics object: its pose relative to that car at the two steps is blended
// and put on the car's blended matrix. The camera then stays locked to the car
// however fast the car turns, and a switch swoop (which moves the camera
// quickly through world space) is smooth. A camera that moves less in world
// space than relative to the car (a fixed trackside camera) is blended in world
// space. A cut (new camera, look back, a jump in both spaces) has nothing to
// blend from: the camera rides on the car's blend at its step pose, so the
// world stays blended and nothing jumps; one step later it blends again.
static void BlendCamera(Ent* c, float t) {
    // candidates: moving physics objects within 50 m. With two steps of the
    // camera, the one it moves least relative to is the one it follows;
    // without (a new camera), the nearest.
    Ent* a = nullptr; float best = 1e30f;
    bool cont = c->prevStep == g_step - 1;
    float Lp[12], Lc[12];
    for (int i = 0; i < g_nWritten; i++) {
        Ent* e = g_written[i];
        if (e->kind != KIND_PHYS) continue;
        float dx = e->cur[9]-c->cur[9], dy = e->cur[10]-c->cur[10], dz = e->cur[11]-c->cur[11];
        float d2 = dx*dx + dy*dy + dz*dz;
        if (d2 > 50.0f * 50.0f) continue;
        float score = d2;
        if (cont) {
            float ia[12], p[12], q[12];
            InvertRigid(e->prev, ia); Compose(c->prev, ia, p);
            InvertRigid(e->cur, ia);  Compose(c->cur, ia, q);
            score = Motion(p, q);
            if (score < best) { memcpy(Lp, p, sizeof p); memcpy(Lc, q, sizeof q); }
        }
        if (score < best) { best = score; a = e; }
    }
    bool done = false;
    if (cont) {
        // a handoff to another camera object is only continuous if it barely
        // moved; anything more is a cut (an instant switch, look back)
        float maxD = c->handoff ? 2.0f : g_cfg.maxStepDistance, maxA = c->handoff ? 20.0f : g_cfg.maxCamAngle;
        bool wOk = c->handoff ? PoseDist(c->prev, c->cur) <= maxD && PoseAngle(c->prev, c->cur) <= maxA
                              : !CameraJumped(c->prev, c->cur);
        float wMove = Motion(c->prev, c->cur);
        bool rOk = a && PoseDist(Lp, Lc) <= maxD && PoseAngle(Lp, Lc) <= maxA;
        float Lb[12];
        if (rOk && (!wOk || best <= wMove) && BlendM34(Lp, Lc, t, Lb, 1e9f, 360.0f)) {
            Compose(Lb, a->bl, c->bl); done = true; st_camRel++; if (c->handoff) st_camHandoff++;
        } else if (wOk && BlendM34(c->prev, c->cur, t, c->bl, 1e9f, 360.0f)) {
            done = true; st_camBlend++; if (c->handoff) st_camHandoff++;
        }
        if (done) c->bl[12] = c->prev[12] + (c->cur[12] - c->prev[12]) * t;
    }
    if (!done) {
        if (!a) { st_cutFrames++; return; }              // nothing moving nearby: the step pose is right
        float inv[12], d[12];
        InvertRigid(a->cur, inv); Compose(inv, a->bl, d); Compose(c->cur, d, c->bl);
        c->bl[12] = c->cur[12]; st_camAnchor++;
    }
    memcpy((void*)(c->addr + 0x0C), c->bl, 48); G<float>(c->addr + 0x7C) = c->bl[12];
    c->wrote = true;
}

static void ApplyBlend() {
    g_nWritten = 0;
    g_applied = false;
    if (!g_cfg.interpolate || !g_frameSimOk || g_step < 2) return;
    st_frames++;
    float t = g_alpha;
    // Cameras: a camera moved outside a step (or not seen at the last step)
    // draws the whole frame unblended. The others are blended after the
    // objects, since the camera may be blended relative to the car it follows.
    bool cut = false;
    Ent* cams[kMaxViews]; int nc = 0;
    if (g_cfg.interpCamera) {
        for (int i = 0; i < g_nViews; i++) {
            DWORD cam = 0;
            if (!ViewLive(i)) continue;                                      // view of an old race
            if (!SafeRead(g_views[i] + 0x2C, &cam) || !cam) continue;
            Ent* e = EntFind(cam);
            if (!e || e->curStep != g_step) { cut = true; continue; }       // camera not seen at last step
            float now[13]; memcpy(now, (void*)(cam + 0x0C), 48); now[12] = G<float>(cam + 0x7C);
            if (!NearlyEqual(now, e->cur, 13)) { st_mismatch++; cut = true; continue; }
            if (nc < kMaxViews) cams[nc++] = e;
        }
    }
    if (cut) { st_cutFrames++; return; }
    st_blendFrames++; st_alphaSum += t;
    for (int i = 0; i < kMaxEnts; i++) {
        Ent& e = g_ents[i];
        if (!e.addr || e.kind == KIND_CAM) continue;
        e.wrote = false;
        if (e.kind == KIND_PED && !g_pedHook) continue;
        // read at the last step and the one before; the memory was valid then
        // and nothing frees objects between the update and the draw
        if (e.curStep != g_step || e.prevStep != g_step - 1) continue;
        if (!memcmp(e.prev, e.cur, 48)) continue;                      // not moving
        if (!NearlyEqual((const float*)e.addr, e.cur, 12)) { st_mismatch++; continue; }
        float maxAng = e.kind == KIND_PED ? 181.0f : g_cfg.maxStepAngle;   // peds can turn on the spot
        if (!BlendM34(e.prev, e.cur, t, e.bl, g_cfg.maxStepDistance, maxAng)) {
            st_jump++;
            if (st_jumpLogged < 8) {                       // what jumps? (first few per stats period)
                st_jumpLogged++;
                float dx = e.cur[9]-e.prev[9], dy = e.cur[10]-e.prev[10], dz = e.cur[11]-e.prev[11];
                float c = (e.prev[0]*e.cur[0]+e.prev[1]*e.cur[1]+e.prev[2]*e.cur[2] + e.prev[3]*e.cur[3]+e.prev[4]*e.cur[4]+
                           e.prev[5]*e.cur[5]+e.prev[6]*e.cur[6]+e.prev[7]*e.cur[7]+e.prev[8]*e.cur[8] - 1.0f) * 0.5f;
                if (c > 1) c = 1;
                if (c < -1) c = -1;
                Logf("  jump: %s %08X moved %.2f m, turned %.1f deg (pos %.1f %.1f %.1f)\n",
                     e.kind == KIND_PHYS ? "phys" : e.kind == KIND_AMB ? "traffic" : "ped", e.addr,
                     sqrtf(dx*dx + dy*dy + dz*dz), acosf(c) * 57.29578f, e.cur[9], e.cur[10], e.cur[11]);
            }
            continue;
        }
        memcpy((void*)e.addr, e.bl, 48);
        e.wrote = true; st_written++;
        g_written[g_nWritten++] = &e;
    }
    g_applied = true;
    for (int k = 0; k < nc; k++) BlendCamera(cams[k], t);
}

static void RestoreBlend() {
    if (!g_applied) return;
    for (int i = 0; i < kMaxEnts; i++) {
        Ent& e = g_ents[i];
        if (!e.addr || !e.wrote) continue;
        e.wrote = false;
        if (e.kind == KIND_CAM) {
            float now[13]; memcpy(now, (void*)(e.addr + 0x0C), 48); now[12] = G<float>(e.addr + 0x7C);
            if (!memcmp(now, e.bl, sizeof now)) { memcpy((void*)(e.addr + 0x0C), e.cur, 48); G<float>(e.addr + 0x7C) = e.cur[12]; }
            else st_restoreSkip++;
        } else {
            if (!memcmp((void*)e.addr, e.bl, 48)) memcpy((void*)e.addr, e.cur, 48);
            else st_restoreSkip++;
        }
    }
    g_nWritten = 0;
    g_applied = false;
}

// --- model delta -------------------------------------------------------------
static bool  g_deltaOn = false;
static float g_delta[12];

static void __cdecl Hook_SetWorld34(const float* m) {
    if (g_deltaOn && m) {
        float t[12]; Compose(m, g_delta, t); st_worldDelta++;
        ((M34to44Fn)mc2::fnM34to44)((void*)mc2::gWorld44, t);
    } else {
        ((M34to44Fn)mc2::fnM34to44)((void*)mc2::gWorld44, m);
    }
    ((VoidFn)mc2::fnFlushWorld)();
}
static void __cdecl Hook_SetWorld44(const float* m) {
    if (g_deltaOn && m) {
        // M (4x4 row-major) * D44, D44 = [R 0; p 1]
        float o[16];
        for (int i = 0; i < 4; i++) {
            const float* r = m + i * 4;
            for (int j = 0; j < 3; j++)
                o[i*4+j] = r[0]*g_delta[j] + r[1]*g_delta[3+j] + r[2]*g_delta[6+j] + r[3]*g_delta[9+j];
            o[i*4+3] = r[3];
        }
        st_worldDelta++;
        ((M44CopyFn)mc2::fnM44Copy)((void*)mc2::gWorld44, nullptr, o);
    } else {
        ((M44CopyFn)mc2::fnM44Copy)((void*)mc2::gWorld44, nullptr, m);
    }
    ((VoidFn)mc2::fnFlushWorld)();
}

// Inside a delta draw every object transform goes back to its exact step
// value: the model may read its body through another tracked address (the
// rigid body's ICS matrix as well as the instance matrix), and a blended value
// there would be moved twice. Cameras stay blended (the view is already set).
static void SwapWritten(bool toStep) {
    for (int i = 0; i < g_nWritten; i++) {
        Ent& e = *g_written[i];
        if (!e.wrote) continue;
        const float* from = toStep ? e.bl : e.cur;
        const float* to   = toStep ? e.cur : e.bl;
        if (!memcmp((void*)e.addr, from, 48)) memcpy((void*)e.addr, to, 48);
    }
}

// Draw one object with its exact step matrix and the delta applied to every
// world matrix it sets.
static int g_deltaDepth = 0;
static void DrawWithDelta(DWORD fn, void* self, int pass, DWORD mtx, unsigned long* nDelta) {
    Ent* e = (g_applied && g_cfg.interpModels && mtx && !g_deltaDepth) ? EntFind(mtx) : nullptr;
    if (!e || !e->wrote || !NearlyEqual((const float*)mtx, e->bl, 12)) {
        ((DrawFn)fn)(self, nullptr, pass); return;
    }
    float inv[12];
    InvertRigid(e->cur, inv); Compose(inv, e->bl, g_delta);
    SwapWritten(true);
    g_deltaOn = true; g_deltaDepth++;
    (*nDelta)++;
    ((DrawFn)fn)(self, nullptr, pass);
    g_deltaDepth--; g_deltaOn = false;
    SwapWritten(false);
}
static void __fastcall Hook_CarDraw(void* self, void*, int pass) {
    st_carDraws++;
    DWORD mtx = 0; SafeRead((DWORD)self + 0x18, &mtx);
    if (g_applied && mtx && !EntFind(mtx)) st_carNoEnt++;
    DrawWithDelta(mc2::fnCarDraw, self, pass, mtx, &st_carDelta);
}
static void __fastcall Hook_AmbDraw(void* self, void*, int pass) {
    st_ambDraws++;
    DrawWithDelta(mc2::fnAmbDraw, self, pass, (DWORD)self + 0x24, &st_ambDelta);
}
// Draw the ped with a blended palette in [model+0x2C]. The draw's own swap
// (0x541BE0, at its end) may move that pointer to +0x38; the game's buffers
// are then put back exactly as its swap would have left them.
static void __fastcall Hook_PedDraw(void* self, void*, int pass) {
    st_pedDraws++;
    DWORD model = (DWORD)self;
    PedPose* p = (g_applied && g_cfg.interpModels) ? PedFind(model, false) : nullptr;
    if (!p || p->curStep != g_step || p->prevStep != g_step - 1) { ((DrawFn)mc2::fnPedDraw)(self, nullptr, pass); return; }
    // bones past the skeleton's count are never read by the draw; keep the
    // game's values there so the palette is complete either way
    DWORD drawBuf = G<DWORD>(model + 0x2C);
    if (!drawBuf || IsBadReadPtr((void*)drawBuf, kPedBones * 48)) { ((DrawFn)mc2::fnPedDraw)(self, nullptr, pass); return; }
    memcpy(p->bl, (const void*)drawBuf, kPedBones * 48);
    for (int b = 0; b < p->bones; b++)
        if (!BlendM34(p->prev + b * 12, p->cur + b * 12, g_alpha, p->bl + b * 12, g_cfg.maxStepDistance, 181.0f))
            { ((DrawFn)mc2::fnPedDraw)(self, nullptr, pass); return; }      // teleport: draw as is
    G<DWORD>(model + 0x2C) = (DWORD)p->bl;
    st_pedDelta++;
    ((DrawFn)mc2::fnPedDraw)(self, nullptr, pass);
    if (G<DWORD>(model + 0x38) == (DWORD)p->bl) G<DWORD>(model + 0x38) = drawBuf;   // swapped
    else if (G<DWORD>(model + 0x2C) == (DWORD)p->bl) G<DWORD>(model + 0x2C) = drawBuf;
}

// Light glows are queued as world-space sprites: the light functions take the
// light's matrix (the car body or a matrix cached from it during the step)
// and transform the light positions with it. While a car / traffic delta draw
// is active they get matrix * delta, the transform the body is drawn with.
// The light functions call each other with the matrix they were given, so a
// matrix that is already one of our copies is left alone (no double move).
// A ring of copies in case the queue keeps the pointer until it draws.
static float g_glowMtx[256][12]; static unsigned g_glowIdx = 0;
extern "C" void __cdecl MC2FF_FixGlowMatrix(const float** pm) {
    if (!g_deltaOn || !g_cfg.interpGlows || !*pm) return;
    const float* m = *pm;
    const float* ring = &g_glowMtx[0][0];
    if (m >= ring && m < ring + 256 * 12) return;      // already moved (nested call)
    if (IsBadReadPtr(m, 48)) return;
    float* o = g_glowMtx[g_glowIdx++ & 255];
    Compose(m, g_delta, o);
    *pm = o; st_glowDelta++;
}
// Entry stubs, one per light function: fix the matrix argument, run the
// replaced "sub esp, imm32" and continue 6 bytes in.
#define LIGHT_STUB(name, argOfs, imm, cont)                     \
    extern "C" void name();                                     \
    __asm__(".text\n.globl _" #name "\n_" #name ":\n"           \
            "  pushl %ecx\n"                                    \
            "  leal " #argOfs "(%esp), %eax\n"                  \
            "  pushl %eax\n"                                    \
            "  call _MC2FF_FixGlowMatrix\n"                     \
            "  addl $4, %esp\n"                                 \
            "  popl %ecx\n"                                     \
            "  subl $" #imm ", %esp\n"                          \
            "  pushl $" #cont "\n"                              \
            "  ret\n");
// argOfs: after "push ecx" the return address is at 4, arg1 at 8, arg2 at 12
LIGHT_STUB(Stub_L51CEA0, 12, 0xCC,  0x51CEA6)
LIGHT_STUB(Stub_L51CB00,  8, 0xC8,  0x51CB06)
LIGHT_STUB(Stub_L51C6F0,  8, 0xCC,  0x51C6F6)
LIGHT_STUB(Stub_L51C8A0,  8, 0xD8,  0x51C8A6)
LIGHT_STUB(Stub_L51F1F0,  8, 0xE0,  0x51F1F6)
LIGHT_STUB(Stub_L51CFF0,  8, 0xD0,  0x51CFF6)
LIGHT_STUB(Stub_L51EF00,  8, 0x114, 0x51EF06)
static void (*const kLightStubs[])() = { Stub_L51CEA0, Stub_L51CB00, Stub_L51C6F0, Stub_L51C8A0,
                                         Stub_L51F1F0, Stub_L51CFF0, Stub_L51EF00 };

static void __fastcall Hook_ViewSetCam(void* view, void*) {
    DWORD v = (DWORD)view; bool known = false;
    for (int i = 0; i < g_nViews; i++) if (g_views[i] == v) { known = true; g_viewSeen[i] = g_frameNo; }
    if (!known) {
        int slot = g_nViews < kMaxViews ? g_nViews++ : 0;
        if (g_nViews == kMaxViews && slot == 0)
            for (int i = 1; i < kMaxViews; i++) if (g_viewSeen[i] < g_viewSeen[slot]) slot = i;
        g_views[slot] = v; g_viewSeen[slot] = g_frameNo; g_viewCam[slot] = 0;
        Logf("camera view registered: %08X (slot %d)\n", v, slot);
    }
    ((ThisFn)mc2::fnViewSetCam)(view, nullptr);
}

// ---------------------------------------------------------------------------
// Loop hooks
// ---------------------------------------------------------------------------
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
static int   g_profOn = 0;
static DWORD g_mainTid = 0;
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
static void ProfPollObjects() {       // DirectMusic objects exist once audio is up
    if (!g_profOn) return;
    DWORD obj = 0, vt = 0;
    if (!g_profLoader && SafeRead(0x8567C8, &obj) && obj && SafeRead(obj, &vt) && vt) {
        ProfPatchVtable(vt, 3, 14, "Loader", kLoaderNames); g_profLoader = true;
        Logf("profiler: DirectMusic loader hooked (%08X)\n", obj);
    }
    if (!g_profPerf && SafeRead(0x8567CC, &obj) && obj && SafeRead(obj, &vt) && vt) {
        ProfPatchVtable(vt, 3, 52, "Perf", kPerfNames); g_profPerf = true;
        Logf("profiler: DirectMusic performance hooked (%08X)\n", obj);
    }
}

static void ProfInit() {
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
static LONGLONG g_pfFrameStart = 0, g_pfUpdate = 0, g_pfDraw = 0; static int g_pfSteps = 0;
struct SlowFrame { double total, update, draw, other, audio, maxCall; int maxId, steps; };
static SlowFrame g_slowFrames[10]; static int g_nSlowFrames = 0; static unsigned long g_slowFramesAll = 0;
static void ProfFrameBoundary(LONGLONG now) {
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

static void ProfLogPeriod() {
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

// ---------------------------------------------------------------------------
// LOD distance (lodDistanceScale)
//
// Race cars (player, opponents, cop cars...): 0x4C4C00 measures the distance
// from the camera (0x858578) to the car, minus its bounding radius, and picks
// the model by the car's tuning (mcCarSim +0x16C Medium_Lod, +0x170 Low_Lod,
// +0x174 Very_Low_Lod) into [car+0xA168]. Ambient traffic: 0x4E5E60 compares
// the squared ground distance with [type+0x124 / 0x128 / 0x12C]. The measured
// distance is divided by the scale (squared one by scale^2) before those
// compares, so every LOD switch happens scale times farther away.
// City objects (trees, props, the "cullable" instances, 0x5171F0): the low
// model ([inst+5] = 1) is used once the camera is farther than the object's
// radius plus its model's LOD distance ([[[inst+8]+0x1C]+0x20]); that LOD
// distance is multiplied by the scale.
// ---------------------------------------------------------------------------
extern "C" { __attribute__((used)) float g_lodInv = 1.0f, g_lodInv2 = 1.0f, g_lodScale = 1.0f; }
// replaces `fld [ecx+0x20]; mov edx,[esi]` at 0x51720E
__attribute__((naked)) static void Stub_CityLod() {
    asm volatile(
        "flds 0x20(%ecx)\n\t"
        "fmuls _g_lodScale\n\t"
        "movl (%esi), %edx\n\t"
        "ret\n");
}
// replaces `fsubr [esp+4]; mov ecx,[esi+0x68]` at 0x4C4C5A
__attribute__((naked)) static void Stub_CarLod() {
    asm volatile(
        "fsubrs 8(%esp)\n\t"            // distance - radius (caller's [esp+4])
        "fmuls _g_lodInv\n\t"
        "movl 0x68(%esi), %ecx\n\t"
        "ret\n");
}
// replaces `faddp st(1); fcom [ecx+0x124]` at 0x4E5E85
__attribute__((naked)) static void Stub_TrafficLod() {
    asm volatile(
        "faddp %st, %st(1)\n\t"
        "fmuls _g_lodInv2\n\t"
        "fcoms 0x124(%ecx)\n\t"
        "ret\n");
}
static void InstallLodScale() {
    static const BYTE car[] = { 0xD8, 0x6C, 0x24, 0x04, 0x8B, 0x4E, 0x68 };
    static const BYTE amb[] = { 0xDE, 0xC1, 0xD8, 0x91, 0x24, 0x01, 0x00, 0x00 };
    static const BYTE city[] = { 0xD9, 0x41, 0x20, 0x8B, 0x16, 0xD9, 0x5C, 0x24, 0x10 };
    g_lodInv = (float)(1.0 / g_cfg.lodDistanceScale); g_lodInv2 = g_lodInv * g_lodInv;
    g_lodScale = (float)g_cfg.lodDistanceScale;
    bool c = CheckBytes(0x4C4C5A, car, sizeof car), a = CheckBytes(0x4E5E85, amb, sizeof amb);
    bool o = CheckBytes(0x51720E, city, sizeof city);
    if (o) WriteRel(0x51720E, 0xE8, (void*)&Stub_CityLod);
    if (c) { WriteRel(0x4C4C5A, 0xE8, (void*)&Stub_CarLod); WriteNops(0x4C4C5F, 2); }
    if (a) { WriteRel(0x4E5E85, 0xE8, (void*)&Stub_TrafficLod); WriteNops(0x4E5E8A, 3); }
    Logf("LOD distance x%.2f: cars %s, traffic %s, city objects %s\n", g_cfg.lodDistanceScale,
         c ? "on" : "code differs", a ? "on" : "code differs", o ? "on" : "code differs");
}

// ---------------------------------------------------------------------------
// ambientDistance: object types that keep instance lists (traffic car models
// "carmodeltype", peds "pedmodel", trains, bangers, city models) draw each
// instance only while the camera is within its radius plus the level's
// distance [level+0x218] (0x517460). The level constructor (0x5187E0) sets it
// to 150 m and nothing changes it; the constant is patched.
// ---------------------------------------------------------------------------
static void InstallAmbientDistance() {
    static const BYTE exp[] = { 0xC7, 0x86, 0x18, 0x02, 0x00, 0x00, 0x00, 0x00, 0x16, 0x43 };
    if (!CheckBytes(0x518828, exp, sizeof exp)) { Logf("ambient distance: code differs, OFF\n"); return; }
    float v = (float)g_cfg.ambientDistance;
    DWORD old; VirtualProtect((void*)0x51882E, 4, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)0x51882E, &v, 4);
    VirtualProtect((void*)0x51882E, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)0x51882E, 4);
    Logf("ambient distance: %.0f m (game 150)\n", g_cfg.ambientDistance);
}

// ---------------------------------------------------------------------------
// spawnDistanceScale: traffic (aiAmbientTraffic) and pedestrians are created
// on roads that a per-player region accepts (road sets 0x501870, classify
// 0x5014E0, region 0x502C60). The region is three bands in front of / around
// the player at 175, 275 and 350 m (globals 0x662DA8 / AC / B0, read when the
// region is built). Those are scaled. Ambient cars also drop to their lowest
// simulation level past 50 / 200 / 300 m from every player (squared, set in
// the aiAmbientTraffic constructor 0x4E8CE0); scaled the same way so cars
// created farther out aren't retired at once.
// ---------------------------------------------------------------------------
static void PatchFloat(DWORD at, float v) {
    DWORD old; VirtualProtect((void*)at, 4, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)at, &v, 4);
    VirtualProtect((void*)at, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, 4);
}
static void InstallSpawnDistance() {
    const float k = (float)g_cfg.spawnDistanceScale;
    static const DWORD bands[3] = { 0x662DA8, 0x662DAC, 0x662DB0 };
    static const float bandOrig[3] = { 175.0f, 275.0f, 350.0f };
    for (int i = 0; i < 3; i++)
        if (G<float>(bands[i]) != bandOrig[i]) { Logf("spawn distance: data differs, OFF\n"); return; }
    for (int i = 0; i < 3; i++) PatchFloat(bands[i], bandOrig[i] * k);
    static const DWORD sim[3] = { 0x4E8CE0, 0x4E8CEA, 0x4E8CF4 };
    static const float simOrig[3] = { 2500.0f, 40000.0f, 90000.0f };
    bool simOk = true;
    for (int i = 0; i < 3; i++) {
        BYTE exp[10] = { 0xC7, 0x86, (BYTE)(0x30 + 4 * i), 0x01, 0x00, 0x00 };
        memcpy(exp + 6, &simOrig[i], 4);
        if (!CheckBytes(sim[i], exp, 10)) simOk = false;
    }
    if (simOk) for (int i = 0; i < 3; i++) PatchFloat(sim[i] + 6, simOrig[i] * k * k);
    Logf("spawn distance x%.2f: bands %.0f/%.0f/%.0f m, ambient sim range %s\n", k, 175.0f * k, 275.0f * k, 350.0f * k,
         simOk ? "scaled" : "code differs");
}

// ---------------------------------------------------------------------------
// Exit crash (fixExitCrash=1)
//
// The debug font ("bigfont"/"smallfont", object at [0x85B1FC]) is created at
// 0x5F9DB0, which also registers 0x5F9D70 as a shutdown callback (list 0x860300).
// The main shutdown (0x401488 -> 0x53A870 -> 0x5F9E30) frees it first, with a
// null check, and clears the pointer; when the callback list runs afterwards,
// 0x5F9D70 does `dec [ecx+0x18]` on the cleared pointer: access violation at
// mc2.exe+1F9D76 (null+0x18). The callback now returns at once when the font
// is already gone (it would also free the shared font page 0x85AE78 again).
// ---------------------------------------------------------------------------
__attribute__((naked)) static void FontShutdownStub() {
    asm volatile(
        "movl 0x85B1FC, %ecx\n\t"
        "testl %ecx, %ecx\n\t"
        "jz 1f\n\t"
        "pushl $0x5F9D76\n\t"         // continue at dec [ecx+0x18]
        "ret\n"
        "1:\n\t"
        "ret\n");
}
static void InstallExitCrashFix() {
    static const BYTE exp[] = { 0x8B, 0x0D, 0xFC, 0xB1, 0x85, 0x00, 0xFF, 0x49, 0x18 };
    if (!CheckBytes(0x5F9D70, exp, sizeof exp)) { Logf("exit crash fix: code differs, OFF\n"); return; }
    WriteRel(0x5F9D70, 0xE9, (void*)&FontShutdownStub);
    Logf("exit crash fix: installed\n");
}

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
static bool g_sndTrace = false;
static double NowMs() { return (double)QpcNow() * 1000.0 / (double)g_qpf.QuadPart; }
static bool LooksLikeRet(DWORD v) {
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
static void SndTraceDump() {
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
            Logf("    %+7.1f ms %-4s voice %08X arg %08X from %06lX  stack %06lX %06lX %06lX %06lX\n", d,
                 names[e.slot & 7], e.self, e.arg0, e.ret, e.stk[0], e.stk[1], e.stk[2], e.stk[3]);
            shown++;
        }
        if (!shown) Logf("    (no voice started or stopped)\n");
    }
}
static void InstallSndTrace() {
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

static unsigned long st_dopplerSwitches = 0, st_dopplerHeld = 0;
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
static unsigned long st_listenerDelayed = 0;
extern "C" __attribute__((used)) int __cdecl MC2FF_ListenerHook(DWORD obj, DWORD* args) {
    ListenerState* L = nullptr;
    for (auto& x : g_ls) if (x.obj == obj) { L = &x; break; }
    if (!L) for (auto& x : g_ls) if (!x.obj) { L = &x; memset(L, 0, sizeof *L); L->obj = obj; L->type = 0xFFFF; break; }
    if (!L) return 0;
    double now = NowMs();
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
static void InstallDopplerFix() {
    if (!CheckCall(0x469C79, 0x5D6050)) { Logf("doppler fix: code differs, OFF\n"); return; }
    WriteRel(0x469C79, 0xE8, (void*)&Stub_AddListener);
    Logf("camera-switch audio fix: installed (Doppler hold %.2f s, listener hold %d ticks after a cut)\n", g_cfg.dopplerHoldOnSwitch, g_cfg.fixCameraCutAudio);
}

// ---------------------------------------------------------------------------
// Asynchronous Miles stream opens (asyncStreams=1)
//
// The profiler showed the in-race hitches: AIL_open_stream (city ambience,
// voices) runs on the main thread and reads/pre-buffers the file before it
// returns, ~100 ms on a cold read. The game's use of streams is simple
// (0x5DB472..0x5DBB5D): open -> start -> set volume/pan, poll stream_status
// (anything but 4/0x10 means finished -> close), pause, close.
// The open now returns a placeholder handle at once and a worker thread does
// the real open; start / pause / volume calls are kept and applied on the
// main thread as soon as the stream exists. While it is opening, status
// reports "playing" (4), as the game expects right after a start; if the open
// fails it reports "done" (2) and the game closes it.
// ---------------------------------------------------------------------------
namespace mc2 {
    const DWORD iatStreamStatus = 0x62D290, iatOpenStream = 0x62D294, iatStartStream = 0x62D298,
                iatPauseStream  = 0x62D29C, iatCloseStream = 0x62D2A0, iatStreamVolPan = 0x62D2A4;
}
typedef DWORD (__stdcall *AilOpenFn)(DWORD dig, const char* file, int mem);
typedef void  (__stdcall *AilH1Fn)(DWORD h);
typedef void  (__stdcall *AilPauseFn)(DWORD h, int onoff);
typedef int   (__stdcall *AilStatusFn)(DWORD h);
typedef void  (__stdcall *AilVolPanFn)(DWORD h, DWORD vol, DWORD pan);   // raw bits: F32 or S32
static AilOpenFn  g_ailOpen;  static AilH1Fn g_ailStart, g_ailClose;
static AilPauseFn g_ailPause; static AilStatusFn g_ailStatus; static AilVolPanFn g_ailVolPan;

enum { SP_FREE = 0, SP_PENDING, SP_READY, SP_FAILED };
struct StreamProxy {
    LONG state; DWORD real; DWORD dig; int mem; char file[260];
    bool startReq, startDone, pauseSet, volSet, closeReq; int pauseVal; DWORD vol, pan;
    LONGLONG tOpen;
};
static const int kMaxStreams = 64;
static StreamProxy g_streams[kMaxStreams];
static CRITICAL_SECTION g_streamLock;
static HANDLE g_streamEvent = nullptr;
static bool g_asyncStreams = false;
static unsigned long st_streamOpens = 0, st_streamSync = 0, st_streamFail = 0; static double st_streamMaxMs = 0;

struct StreamLogEnt { char name[48]; float prefetchMs, openMs, readyMs; bool ok; };
static StreamLogEnt g_streamLog[8]; static int g_nStreamLog = 0;   // first opens per stats period
static double QpcMs(LONGLONG a, LONGLONG b) { return (double)(b - a) * 1000.0 / (double)g_qpf.QuadPart; }
static StreamProxy* AsProxy(DWORD h) {
    if (h < (DWORD)&g_streams[0] || h >= (DWORD)&g_streams[kMaxStreams]) return nullptr;
    if ((h - (DWORD)&g_streams[0]) % sizeof(StreamProxy)) return nullptr;
    return (StreamProxy*)h;
}
// main thread, lock held: push queued state into a stream that is ready
static void StreamApply(StreamProxy* p) {
    if (p->state != SP_READY || !p->real) return;
    if (p->volSet)   { g_ailVolPan(p->real, p->vol, p->pan); p->volSet = false; }
    if (p->startReq && !p->startDone) { g_ailStart(p->real); p->startDone = true; }
    if (p->pauseSet) { g_ailPause(p->real, p->pauseVal); p->pauseSet = false; }
}
// The game's Miles file callbacks (0x5D95CA: AIL_set_file_callbacks(open,
// close, seek, read)), all stdcall. Reading the file through them first warms
// the OS cache without touching Miles: if Miles holds its API lock while it
// opens a stream, the open is then short (~1 ms warm, as the profiler showed)
// and the main thread's audio calls aren't held up behind a cold read.
// Off-thread use of these callbacks is nothing new: in the original game Miles
// already calls them from its own streaming thread to read the stream data.
typedef int   (__stdcall *FileOpenCb)(const char* name, DWORD* handle);
typedef void  (__stdcall *FileCloseCb)(DWORD handle);
typedef DWORD (__stdcall *FileReadCb)(DWORD handle, void* buf, DWORD bytes);
static const DWORD fnFileOpenCb = 0x5D94A0, fnFileCloseCb = 0x5D9500, fnFileReadCb = 0x5D9550;
static bool g_prefetchOk = false;
static unsigned long st_prefetchMB10 = 0;     // tenths of MB read
// Streams are MP3s under AUDIO\STREAMS\ (the game only routes .mp3 names to
// AIL_open_stream). A loose file is read with plain Win32 calls; only a name
// that isn't on disk (packed in an archive) goes through the game's callbacks.
// Only the head of the file is read: that is what Miles reads inside the open
// (header and first buffers). The rest it streams from its own thread while
// playing, as in the original game. Reading the whole file first only delayed
// the start of the sound.
static const DWORD kPrefetchBytes = 1024 * 1024;
static void PrefetchFile(const char* name) {
    static BYTE* buf = (BYTE*)VirtualAlloc(nullptr, 256 * 1024, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) return;
    HANDLE f = CreateFileA(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD total = 0, n = 0;
        while (total < kPrefetchBytes && ReadFile(f, buf, 256 * 1024, &n, nullptr) && n) total += n;
        CloseHandle(f);
        st_prefetchMB10 += total / (1024 * 1024 / 10);
        return;
    }
    if (!g_prefetchOk) return;
    DWORD h = 0;
    if (!((FileOpenCb)fnFileOpenCb)(name, &h) || !h) return;
    DWORD total = 0, n;
    while (total < kPrefetchBytes && (n = ((FileReadCb)fnFileReadCb)(h, buf, 256 * 1024)) > 0 && n <= 256 * 1024) total += n;
    ((FileCloseCb)fnFileCloseCb)(h);
    st_prefetchMB10 += total / (1024 * 1024 / 10);
}
static DWORD WINAPI StreamWorker(LPVOID) {
    for (;;) {
        WaitForSingleObject(g_streamEvent, INFINITE);
        for (;;) {
            StreamProxy* job = nullptr;
            EnterCriticalSection(&g_streamLock);
            for (int i = 0; i < kMaxStreams && !job; i++) {
                if (g_streams[i].state != SP_PENDING) continue;
                if (g_streams[i].closeReq) g_streams[i].state = SP_FREE;   // closed before we got to it
                else job = &g_streams[i];
            }
            LeaveCriticalSection(&g_streamLock);
            if (!job) break;
            LONGLONG t0 = QpcNow();
            PrefetchFile(job->file);                                    // cold read, off the main thread
            LONGLONG t1 = QpcNow();
            DWORD real = g_ailOpen(job->dig, job->file, job->mem);
            LONGLONG t2 = QpcNow();
            EnterCriticalSection(&g_streamLock);
            double ms = QpcMs(job->tOpen, t2);                          // request -> ready: the sound's delay
            if (ms > st_streamMaxMs) st_streamMaxMs = ms;
            if (g_nStreamLog < 8) {
                StreamLogEnt& l = g_streamLog[g_nStreamLog++];
                const char* b = job->file;
                for (const char* q = job->file; *q; q++) if (*q == '\\' || *q == '/') b = q + 1;
                lstrcpynA(l.name, b, sizeof l.name);
                l.prefetchMs = (float)QpcMs(t0, t1); l.openMs = (float)QpcMs(t1, t2); l.readyMs = (float)ms; l.ok = real != 0;
            }
            if (job->closeReq) {                       // closed while opening
                if (real) g_ailClose(real);
                job->state = SP_FREE;
            } else if (real) { job->real = real; job->state = SP_READY; }
            else { job->state = SP_FAILED; st_streamFail++; }
            LeaveCriticalSection(&g_streamLock);
        }
    }
    return 0;
}
static DWORD __stdcall Async_OpenStream(DWORD dig, const char* file, int mem) {
    StreamProxy* p = nullptr;
    EnterCriticalSection(&g_streamLock);
    if (file && strlen(file) < sizeof(p->file))
        for (int i = 0; i < kMaxStreams && !p; i++) if (g_streams[i].state == SP_FREE) p = &g_streams[i];
    if (p) {
        memset(p, 0, sizeof *p);
        p->state = SP_PENDING; p->dig = dig; p->mem = mem; strcpy(p->file, file); p->tOpen = QpcNow();
        st_streamOpens++;
    }
    LeaveCriticalSection(&g_streamLock);
    if (!p) { st_streamSync++; return g_ailOpen(dig, file, mem); }   // pool full / odd name: as before
    SetEvent(g_streamEvent);
    return (DWORD)p;
}
static void __stdcall Async_StartStream(DWORD h) {
    StreamProxy* p = AsProxy(h); if (!p) { g_ailStart(h); return; }
    EnterCriticalSection(&g_streamLock);
    p->startReq = true;
    if (p->state == SP_READY) {                 // flush queued volume/pause, then (re)start once
        p->startDone = true; StreamApply(p); g_ailStart(p->real);
    } else p->startDone = false;                // started by StreamApply once the open completes
    LeaveCriticalSection(&g_streamLock);
}
static void __stdcall Async_PauseStream(DWORD h, int onoff) {
    StreamProxy* p = AsProxy(h); if (!p) { g_ailPause(h, onoff); return; }
    EnterCriticalSection(&g_streamLock);
    if (p->state == SP_READY) { StreamApply(p); g_ailPause(p->real, onoff); }
    else { p->pauseSet = true; p->pauseVal = onoff; }
    LeaveCriticalSection(&g_streamLock);
}
static void __stdcall Async_StreamVolPan(DWORD h, DWORD vol, DWORD pan) {
    StreamProxy* p = AsProxy(h); if (!p) { g_ailVolPan(h, vol, pan); return; }
    EnterCriticalSection(&g_streamLock);
    if (p->state == SP_READY) { StreamApply(p); g_ailVolPan(p->real, vol, pan); }
    else { p->volSet = true; p->vol = vol; p->pan = pan; }
    LeaveCriticalSection(&g_streamLock);
}
static int __stdcall Async_StreamStatus(DWORD h) {
    StreamProxy* p = AsProxy(h); if (!p) return g_ailStatus(h);
    int r;
    EnterCriticalSection(&g_streamLock);
    if (p->state == SP_READY) { StreamApply(p); r = g_ailStatus(p->real); }
    else if (p->state == SP_PENDING) r = p->startReq ? 4 : 8;      // playing (as after a start) / stopped
    else r = 2;                                                      // failed: done
    LeaveCriticalSection(&g_streamLock);
    return r;
}
static void __stdcall Async_CloseStream(DWORD h) {
    StreamProxy* p = AsProxy(h); if (!p) { g_ailClose(h); return; }
    EnterCriticalSection(&g_streamLock);
    if (p->state == SP_PENDING) p->closeReq = true;                  // the worker closes and frees it
    else { if (p->state == SP_READY && p->real) g_ailClose(p->real); p->state = SP_FREE; }
    LeaveCriticalSection(&g_streamLock);
}
// main thread, once per race frame: start streams that finished opening
static void StreamPoll() {
    if (!g_asyncStreams) return;
    EnterCriticalSection(&g_streamLock);
    for (int i = 0; i < kMaxStreams; i++)
        if (g_streams[i].state == SP_READY && (g_streams[i].volSet || g_streams[i].pauseSet ||
            (g_streams[i].startReq && !g_streams[i].startDone))) StreamApply(&g_streams[i]);
    LeaveCriticalSection(&g_streamLock);
}
static void InstallAsyncStreams() {
    DWORD iat[6] = { mc2::iatStreamStatus, mc2::iatOpenStream, mc2::iatStartStream,
                     mc2::iatPauseStream, mc2::iatCloseStream, mc2::iatStreamVolPan };
    for (int i = 0; i < 6; i++) if (IsBadReadPtr((void*)iat[i], 4) || !G<DWORD>(iat[i])) { Logf("async streams: imports missing, OFF\n"); return; }
    g_ailStatus = (AilStatusFn)G<DWORD>(mc2::iatStreamStatus); g_ailOpen  = (AilOpenFn)G<DWORD>(mc2::iatOpenStream);
    g_ailStart  = (AilH1Fn)G<DWORD>(mc2::iatStartStream);      g_ailPause = (AilPauseFn)G<DWORD>(mc2::iatPauseStream);
    g_ailClose  = (AilH1Fn)G<DWORD>(mc2::iatCloseStream);      g_ailVolPan = (AilVolPanFn)G<DWORD>(mc2::iatStreamVolPan);
    static const BYTE cbSite[] = { 0x68,0x50,0x95,0x5D,0x00, 0x68,0x10,0x95,0x5D,0x00,
                                   0x68,0x00,0x95,0x5D,0x00, 0x68,0xA0,0x94,0x5D,0x00 };   // push read/seek/close/open
    g_prefetchOk = CheckBytes(0x5D95CA, cbSite, sizeof cbSite);
    InitializeCriticalSection(&g_streamLock);
    g_streamEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    HANDLE th = g_streamEvent ? CreateThread(nullptr, 0, StreamWorker, nullptr, 0, nullptr) : nullptr;
    if (!th) { Logf("async streams: worker thread failed, OFF\n"); return; }
    SetThreadPriority(th, THREAD_PRIORITY_ABOVE_NORMAL);
    CloseHandle(th);
    void* fns[6] = { (void*)&Async_StreamStatus, (void*)&Async_OpenStream, (void*)&Async_StartStream,
                     (void*)&Async_PauseStream, (void*)&Async_CloseStream, (void*)&Async_StreamVolPan };
    for (int i = 0; i < 6; i++) {
        DWORD old; VirtualProtect((void*)iat[i], 4, PAGE_READWRITE, &old);
        G<DWORD>(iat[i]) = (DWORD)fns[i];
        VirtualProtect((void*)iat[i], 4, old, &old);
    }
    g_asyncStreams = true;
    Logf("async streams: installed (prefetch %s)\n", g_prefetchOk ? "on" : "off");
}

static LONGLONG g_statsLast = 0;
static void LogStats() {
    LONGLONG now = QpcNow();
    if (!g_statsLast) { g_statsLast = now; return; }
    double sec = (double)(now - g_statsLast) / (double)g_qpf.QuadPart;
    if (sec < g_cfg.statsSeconds) return;
    g_statsLast = now;
    Logf("stats %.1fs: steps=%lu (%.1f/s) frames=%lu (%.1f fps) 0-step=%lu multi-step=%lu | blended=%lu "
         "cut=%lu cam-anchored=%lu cam-handoff=%lu avg t=%.2f | written=%lu mismatch=%lu jump=%lu restore-skipped=%lu tracked=%d (last step %lu) "
         "| cams world=%lu car-relative=%lu views=%d | car draws=%lu delta=%lu no-entry=%lu | traffic draws=%lu delta=%lu "
         "| world matrices moved=%lu | peds draws=%lu blended poses=%lu | glows moved=%lu "
         "| frame time min=%.2fms max=%.2fms slow=%lu\n",
         sec, st_steps, st_steps / sec, st_frames, st_frames / sec, st_zeroStepFrames, st_multiStepFrames,
         st_blendFrames, st_cutFrames, st_camAnchor, st_camHandoff, st_blendFrames ? st_alphaSum / st_blendFrames : 0.0,
         st_written, st_mismatch, st_jump, st_restoreSkip, g_entUsed, st_entsLastStep,
         st_camBlend, st_camRel, g_nViews, st_carDraws, st_carDelta, st_carNoEnt, st_ambDraws, st_ambDelta, st_worldDelta,
         st_pedDraws, st_pedDelta, st_glowDelta, st_ftMin < 1e8 ? st_ftMin : 0.0, st_ftMax, st_slowFrames);
    st_frames = st_steps = st_blendFrames = st_cutFrames = st_written = st_mismatch = st_jump =
    st_restoreSkip = st_carDraws = st_carDelta = st_carNoEnt = st_ambDraws = st_ambDelta =
    st_worldDelta = st_camBlend = st_camRel = st_camAnchor = st_camHandoff = st_zeroStepFrames = st_multiStepFrames = 0;
    st_alphaSum = 0;
    st_pedDraws = st_pedDelta = st_glowDelta = st_slowFrames = st_jumpLogged = 0;
    st_ftMin = 1e9; st_ftMax = 0;
    Logf("  frame intervals ms: <8:%lu 8-12:%lu 12-15.5:%lu 15.5-18:%lu 18-25:%lu 25-40:%lu >40:%lu"
         " | steps per frame 0:%lu 1:%lu 2:%lu 3+:%lu\n",
         st_ftHist[0], st_ftHist[1], st_ftHist[2], st_ftHist[3], st_ftHist[4], st_ftHist[5], st_ftHist[6],
         st_spfHist[0], st_spfHist[1], st_spfHist[2], st_spfHist[3]);
    memset(st_ftHist, 0, sizeof st_ftHist); memset(st_spfHist, 0, sizeof st_spfHist);
    if (g_asyncStreams) {
        Logf("  async streams: opened %lu (longest open %.1f ms off the main thread, %.1f MB prefetched), failed %lu, sync fallback %lu\n",
             st_streamOpens, st_streamMaxMs, st_prefetchMB10 / 10.0, st_streamFail, st_streamSync);
        EnterCriticalSection(&g_streamLock);
        for (int i = 0; i < g_nStreamLog; i++)
            Logf("    stream %-28s ready after %6.1f ms (prefetch %5.1f, open %5.1f)%s\n", g_streamLog[i].name,
                 g_streamLog[i].readyMs, g_streamLog[i].prefetchMs, g_streamLog[i].openMs, g_streamLog[i].ok ? "" : " FAILED");
        g_nStreamLog = 0;
        st_streamOpens = st_streamFail = st_streamSync = 0; st_streamMaxMs = 0; st_prefetchMB10 = 0;
        LeaveCriticalSection(&g_streamLock);
    }
    SndTraceDump();
    {   // traffic manager (aiAmbientTraffic, [0x69947C]) counters, to find what caps the traffic
        DWORD mgr = 0;
        if (SafeRead(0x69947C, &mgr) && mgr && !IsBadReadPtr((void*)(mgr + 0x50), 0xC0)) {
            const int* v = (const int*)(mgr + 0x50);
            Logf("  traffic mgr +50..: ");
            for (int i = 0; i < 0x30; i++) Logf("%d%s", v[i], (i % 8 == 7) ? " | " : " ");
            Logf(" full=%d\n", (int)G<BYTE>(mgr + 0x8F));
        }
    }
    if (st_dopplerSwitches || st_dopplerHeld) {
        Logf("  camera switches %lu: Doppler held for %lu listener updates, listener held for %lu\n",
             st_dopplerSwitches, st_dopplerHeld, st_listenerDelayed);
        st_dopplerSwitches = st_dopplerHeld = st_listenerDelayed = 0;
    }
    ProfLogPeriod();
}

// Pre-update (0x401960: input poll, network/frame begin, state requests) and
// post-update (0x4019B0) bracket each simulation tick. The loop calls them
// once per frame around Update; they are moved inside the fixed-step loop so
// they run once per tick, and not at all on frames without a tick (an input
// edge polled on such a frame would otherwise never reach the simulation).
static bool g_preOpen = false;
static void RunPre(void* self) { ((ThisFn)mc2::fnPreUpdate)(self, nullptr); g_preOpen = true; }
static void __fastcall Hook_RacePre(void*, void*) {}
static void __fastcall Hook_RacePost(void* self, void*) {
    if (g_preOpen) { g_preOpen = false; ((ThisFn)mc2::fnPostUpdate)(self, nullptr); }
}

static void __fastcall Hook_RaceUpdate(void* self, void*) {
    g_frameNo++;
    g_frameSimOk = false;
    if (g_preOpen) Hook_RacePost(self, nullptr);     // never two pre calls in a row
    if (!g_cfg.enabled) { RunPre(self); ((ThisFn)mc2::fnUpdate)(self, nullptr); return; }
    LONGLONG now = FrameTime();
    if (g_profOn) { g_mainTid = GetCurrentThreadId(); ProfPollObjects(); ProfFrameBoundary(QpcNow()); }
    StreamPoll();
    LONGLONG updT0 = QpcNow();
    {   // raw frame interval statistics (stutter diagnosis)
        static LONGLONG rawLast = 0;
        LONGLONG raw = QpcNow();
        if (rawLast) {
            double ms = (double)(raw - rawLast) * 1000.0 / (double)g_qpf.QuadPart;
            if (ms < st_ftMin) st_ftMin = ms;
            if (ms > st_ftMax) st_ftMax = ms;
            if (ms > 1.5 * (1000.0 / (g_cfg.fpsLimit > 0 ? g_cfg.fpsLimit : 60.0))) st_slowFrames++;
            static const double edges[6] = { 8, 12, 15.5, 18, 25, 40 };
            int b = 0; while (b < 6 && ms >= edges[b]) b++;
            st_ftHist[b]++;
        }
        rawLast = raw;
    }
    double step = 1.0 / g_cfg.simHz;
    double frame = g_lastFrame ? (double)(now - g_lastFrame) / (double)g_qpf.QuadPart : step;
    g_lastFrame = now;
    bool reset = frame > 0.25 || G<BYTE>(mc2::pTimerReset);
    if (reset) { frame = step; g_accum = 0.0; EntClear(); g_nViews = 0; memset(g_viewCam, 0, sizeof g_viewCam); }
    if (frame < 0.0) frame = 0.0;
    g_drawDt = frame;

    BYTE* gs = GameState();
    if (gs && gs[9]) {               // paused / in-race menu: one normal update per frame, no blending
        RunPre(self);
        ((ThisFn)mc2::fnUpdate)(self, nullptr);
        g_accum = 0.0; return;
    }
    g_accum += frame;
    // tiny epsilon: at fps = k * simHz the sum of frame periods lands exactly on
    // a step boundary, and rounding must not push a tick into the next frame
    int steps = (int)floor(g_accum / step + 1e-6);
    if (steps > g_cfg.maxSteps) { steps = g_cfg.maxSteps; g_accum = 0.0; }
    else g_accum -= steps * step;
    if (steps == 0) st_zeroStepFrames++; else if (steps > 1) st_multiStepFrames++;
    st_spfHist[steps > 3 ? 3 : steps]++;

    for (int i = 0; i < steps; i++) {
        if (i > 0) {
            if (!RaceState()) break;                      // race ended / state changed mid-frame
            Hook_RacePost(self, nullptr);
        }
        RunPre(self);
        g_forcing = true;
        ((ThisFn)mc2::fnUpdate)(self, nullptr);
        g_forcing = false;
        if (g_cfg.interpolate) SnapshotStep();
        g_pfSteps++;
    }
    g_pfUpdate += QpcNow() - updT0;
    if (g_accum < 0.0) g_accum = 0.0;                     // epsilon above may dip it below 0
    g_alpha = (float)(g_accum / step);
    if (g_alpha < 0) g_alpha = 0;
    if (g_alpha > 1) g_alpha = 1;
    g_frameSimOk = true;
    LogStats();
}

// Draw-time animation (HUD and pause-menu elements, the GO sign, some
// effects) advances by Seconds inside Draw. In the original game Seconds is
// the frame time, so that is right at any frame rate; with fixed steps it is
// the step size while Draw runs every frame, and those animations would run
// at (step / frame time) speed: 2x at 60 fps with 30 Hz steps. During Draw,
// Seconds is the real frame time (as the original computes it: slow-motion
// scale applied, clamped to the timer's limits); the step value is put back
// afterwards for the simulation.
static void __fastcall Hook_RaceDraw(void* self, void*) {
    float sec = G<float>(mc2::pSeconds), inv = G<float>(mc2::pInvSeconds);
    bool special = G<BYTE>(mc2::pTimerReset) || G<int32_t>(mc2::pTimerFixed) || G<BYTE>(mc2::pTimerFreeze);
    if (g_cfg.enabled && !special) {
        float dt = (float)g_drawDt;
        float sc = G<float>(mc2::pTimeScale);
        if (sc > 0.0f) dt *= sc;
        if (dt < G<float>(mc2::pMinSec)) dt = G<float>(mc2::pMinSec);
        float mx = G<float>(mc2::pMaxSec);
        if (mx > 0.0f && dt > mx) dt = mx;
        G<float>(mc2::pSeconds) = dt;
        G<float>(mc2::pInvSeconds) = 1.0f / dt;
    }
    LONGLONG drawT0 = QpcNow();
    ApplyBlend();
    ((ThisFn)mc2::fnDraw)(self, nullptr);
    RestoreBlend();
    g_pfDraw += QpcNow() - drawT0;
    G<float>(mc2::pSeconds) = sec;
    G<float>(mc2::pInvSeconds) = inv;
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------
static bool g_installed = false;
static bool TryInstall() {
    static const BYTE sw34[] = { 0x8B,0x44,0x24,0x04,0x50,0x68,0xC0,0x84,0x85,0x00 };
    static const BYTE sw44[] = { 0x8B,0x44,0x24,0x04,0x50,0xB9,0xC0,0x84,0x85,0x00 };
    bool loop = CheckCall(mc2::callUpdateRace, mc2::fnUpdate) && CheckCall(mc2::callTimer, mc2::fnTimer) &&
                CheckCall(mc2::callDrawRace, mc2::fnDraw) && CheckCall(mc2::callPreRace, mc2::fnPreUpdate) &&
                CheckCall(mc2::callPostRace, mc2::fnPostUpdate) && CheckCall(mc2::callPostRace2, mc2::fnPostUpdate);
    if (!loop) return false;
    WriteRel(mc2::callTimer, 0xE8, (void*)&Hook_Timer);
    WriteRel(mc2::callPreRace, 0xE8, (void*)&Hook_RacePre);
    WriteRel(mc2::callPostRace, 0xE8, (void*)&Hook_RacePost);
    WriteRel(mc2::callPostRace2, 0xE8, (void*)&Hook_RacePost);
    WriteRel(mc2::callUpdateRace, 0xE8, (void*)&Hook_RaceUpdate);
    WriteRel(mc2::callDrawRace, 0xE8, (void*)&Hook_RaceDraw);   // draw-time Seconds (+ blending)
    Logf("fixed step: installed (simHz=%.2f, maxSteps=%d, fpsLimit=%.1f)\n", g_cfg.simHz, g_cfg.maxSteps, g_cfg.fpsLimit);
    if (!g_cfg.interpolate) { Logf("interpolation: off\n"); return true; }
    bool rend = CheckBytes(mc2::fnSetWorld34, sw34, sizeof sw34) && CheckBytes(mc2::fnSetWorld44, sw44, sizeof sw44) &&
                G<DWORD>(mc2::vtCarDraw) == mc2::fnCarDraw && G<DWORD>(mc2::vtAmbDraw) == mc2::fnAmbDraw &&
                G<DWORD>(mc2::vtViewSetCam) == mc2::fnViewSetCam;
    if (!rend) { g_cfg.interpolate = 0; Logf("interpolation: render code differs, OFF\n"); return true; }
    PatchVtable(mc2::vtViewSetCam, mc2::fnViewSetCam, (void*)&Hook_ViewSetCam);
    if (g_cfg.interpModels) {
        WriteRel(mc2::fnSetWorld34, 0xE9, (void*)&Hook_SetWorld34);
        WriteRel(mc2::fnSetWorld44, 0xE9, (void*)&Hook_SetWorld44);
        PatchVtable(mc2::vtCarDraw, mc2::fnCarDraw, (void*)&Hook_CarDraw);
        PatchVtable(mc2::vtAmbDraw, mc2::fnAmbDraw, (void*)&Hook_AmbDraw);
        if (g_cfg.interpPeds) {
            if (PatchVtable(mc2::vtPedDraw, mc2::fnPedDraw, (void*)&Hook_PedDraw)) { g_pedHook = true; Logf("peds: installed\n"); }
            else Logf("peds: vtable differs, OFF\n");
        }
        if (g_cfg.interpGlows) {
            const int n = sizeof(mc2::lightFns) / sizeof(mc2::lightFns[0]);
            bool all = true;
            for (int i = 0; i < n; i++) {
                BYTE exp[6] = { 0x81, 0xEC };
                *(DWORD*)(exp + 2) = mc2::lightFns[i].subImm;
                if (!CheckBytes(mc2::lightFns[i].fn, exp, 6)) all = false;
            }
            if (all) {
                for (int i = 0; i < n; i++) WriteRel(mc2::lightFns[i].fn, 0xE9, (void*)kLightStubs[i]);
                Logf("glows: installed (%d light functions)\n", n);
            } else Logf("glows: code differs, OFF\n");
        }
    }
    Logf("interpolation: installed (camera=%d models=%d traffic=%d physics=%d)\n",
         g_cfg.interpCamera, g_cfg.interpModels, g_cfg.interpTraffic, g_cfg.interpPhysics);
    return true;
}

static DWORD WINAPI InstallThread(LPVOID) {
    for (int i = 0; i < 1000 && !g_installed; i++) { if (TryInstall()) g_installed = true; else Sleep(10); }
    if (!g_installed) Logf("ERROR: mc2.exe code doesn't match (SHA-1 7dc19475...). Nothing patched.\n");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        QueryPerformanceFrequency(&g_qpf);
        LoadConfig(inst);
        Logf("MC2FrameFix %s\n", MC2FF_VERSION);
        if (!g_cfg.enabled) { Logf("disabled in ini\n"); return TRUE; }
        if (g_cfg.profileAudio) ProfInit();
        if (g_cfg.asyncStreams) InstallAsyncStreams();
        if (g_cfg.fixExitCrash) InstallExitCrashFix();
        if (g_cfg.lodDistanceScale != 1.0) InstallLodScale();
        if (g_cfg.ambientDistance != 150.0) InstallAmbientDistance();
        if (g_cfg.spawnDistanceScale != 1.0) InstallSpawnDistance();
        if (g_cfg.traceCameraAudio) InstallSndTrace();
        if (g_cfg.dopplerHoldOnSwitch > 0 || g_cfg.fixCameraCutAudio > 0 || g_sndTrace)
            InstallDopplerFix();
        if (TryInstall()) g_installed = true;
        else CloseHandle(CreateThread(nullptr, 0, InstallThread, nullptr, 0, nullptr));
    }
    return TRUE;
}
