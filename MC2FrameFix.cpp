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
//  - Camera cuts (camera switch, big jump/turn, FOV jump) draw the whole frame
//    at the newest step, like HP2FrameFix.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define MC2FF_VERSION "0.2.0"

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
"; Frame limiter, 0 = off (use vsync). Set to your refresh rate without vsync.\n"
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
"\n"
"; A step longer than this (metres) or turning more than this (degrees) is\n"
"; treated as a teleport/reset and drawn unblended\n"
"maxStepDistance=12\n"
"maxStepAngle=60\n"
"; Camera turning more than this per step counts as a cut (look back, switch)\n"
"maxCameraAngle=45\n"
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

static void FrameLimit() {
    if (g_cfg.fpsLimit <= 0.0) return;
    static bool res = false;
    if (!res) { timeBeginPeriod(1); res = true; }
    LONGLONG period = (LONGLONG)((double)g_qpf.QuadPart / g_cfg.fpsLimit);
    LONGLONG now = QpcNow();
    if (!g_limNext || now - g_limNext > period * 4) g_limNext = now;
    for (;;) {
        now = QpcNow();
        LONGLONG left = g_limNext - now;
        if (left <= 0) break;
        double ms = (double)left * 1000.0 / (double)g_qpf.QuadPart;
        if (ms > 2.0) Sleep((DWORD)(ms - 1.5)); else YieldProcessor();
    }
    g_limNext += period;
}

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
enum { KIND_PHYS = 1, KIND_AMB = 2, KIND_CAM = 3 };
struct Ent {
    DWORD addr;           // matrix address (camera: camera object)
    BYTE  kind;
    int   n;              // floats: 12, camera 13
    unsigned long prevStep, curStep;
    float prev[13], cur[13], bl[13];
    bool  wrote;          // blended value written for this frame
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
    st_worldDelta, st_camBlend, st_zeroStepFrames, st_multiStepFrames, st_entsLastStep;
static double st_alphaSum;

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

static void TrackCamera(DWORD view) {
    DWORD cam = 0;
    if (!SafeRead(view + 0x2C, &cam) || !cam || IsBadReadPtr((void*)(cam + 0x0C), 0x74)) return;
    Ent* e = EntGet(cam, KIND_CAM, 13);
    if (!e) return;
    float v[13]; memcpy(v, (void*)(cam + 0x0C), 48); v[12] = G<float>(cam + 0x7C);
    if (!Finite(v, 13)) { e->curStep = 0; e->prevStep = 0; return; }
    Record(e, v, 13);
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
    if (g_cfg.interpCamera)
        for (int i = 0; i < g_nViews; i++) if (ViewLive(i)) TrackCamera(g_views[i]);
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
static Ent* g_written[kMaxEnts]; static int g_nWritten = 0;   // non-camera entities written this frame
static void ApplyBlend() {
    g_nWritten = 0;
    g_applied = false;
    if (!g_cfg.interpolate || !g_frameSimOk || g_step < 2) return;
    st_frames++;
    float t = g_alpha;
    // cameras first: a cut draws the whole frame unblended
    bool cut = false;
    struct CamW { Ent* e; } cams[kMaxViews]; int nc = 0;
    if (g_cfg.interpCamera) {
        for (int i = 0; i < g_nViews; i++) {
            DWORD cam = 0;
            if (!ViewLive(i)) continue;                                      // view of an old race
            if (!SafeRead(g_views[i] + 0x2C, &cam) || !cam) continue;
            Ent* e = EntFind(cam);
            if (!e || e->curStep != g_step) { cut = true; continue; }       // camera not seen at last step
            if (e->prevStep != g_step - 1) { cut = true; continue; }        // new camera (switch)
            float now[13]; memcpy(now, (void*)(cam + 0x0C), 48); now[12] = G<float>(cam + 0x7C);
            if (!NearlyEqual(now, e->cur, 13)) { st_mismatch++; cut = true; continue; }
            if (CameraJumped(e->prev, e->cur)) { cut = true; continue; }
            if (nc < kMaxViews) cams[nc++].e = e;
        }
    }
    if (cut) { st_cutFrames++; return; }
    st_blendFrames++; st_alphaSum += t;
    for (int k = 0; k < nc; k++) {
        Ent* e = cams[k].e;
        if (!BlendM34(e->prev, e->cur, t, e->bl, 1e9f, 360.0f)) continue;
        e->bl[12] = e->prev[12] + (e->cur[12] - e->prev[12]) * t;
        memcpy((void*)(e->addr + 0x0C), e->bl, 48); G<float>(e->addr + 0x7C) = e->bl[12];
        e->wrote = true; st_camBlend++;
    }
    for (int i = 0; i < kMaxEnts; i++) {
        Ent& e = g_ents[i];
        if (!e.addr || e.kind == KIND_CAM) continue;
        e.wrote = false;
        // read at the last step and the one before; the memory was valid then
        // and nothing frees objects between the update and the draw
        if (e.curStep != g_step || e.prevStep != g_step - 1) continue;
        if (!memcmp(e.prev, e.cur, 48)) continue;                      // not moving
        if (!NearlyEqual((const float*)e.addr, e.cur, 12)) { st_mismatch++; continue; }
        if (!BlendM34(e.prev, e.cur, t, e.bl, g_cfg.maxStepDistance, g_cfg.maxStepAngle)) { st_jump++; continue; }
        memcpy((void*)e.addr, e.bl, 48);
        e.wrote = true; st_written++;
        g_written[g_nWritten++] = &e;
    }
    g_applied = true;
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
static void __fastcall Hook_ViewSetCam(void* view, void*) {
    DWORD v = (DWORD)view; bool known = false;
    for (int i = 0; i < g_nViews; i++) if (g_views[i] == v) { known = true; g_viewSeen[i] = g_frameNo; }
    if (!known) {
        int slot = g_nViews < kMaxViews ? g_nViews++ : 0;
        if (g_nViews == kMaxViews && slot == 0)
            for (int i = 1; i < kMaxViews; i++) if (g_viewSeen[i] < g_viewSeen[slot]) slot = i;
        g_views[slot] = v; g_viewSeen[slot] = g_frameNo;
        Logf("camera view registered: %08X (slot %d)\n", v, slot);
    }
    ((ThisFn)mc2::fnViewSetCam)(view, nullptr);
}

// ---------------------------------------------------------------------------
// Loop hooks
// ---------------------------------------------------------------------------
static LONGLONG g_statsLast = 0;
static void LogStats() {
    LONGLONG now = QpcNow();
    if (!g_statsLast) { g_statsLast = now; return; }
    double sec = (double)(now - g_statsLast) / (double)g_qpf.QuadPart;
    if (sec < g_cfg.statsSeconds) return;
    g_statsLast = now;
    Logf("stats %.1fs: steps=%lu (%.1f/s) frames=%lu (%.1f fps) 0-step=%lu multi-step=%lu | blended=%lu "
         "cut=%lu avg t=%.2f | written=%lu mismatch=%lu jump=%lu restore-skipped=%lu tracked=%d (last step %lu) "
         "| cams blended=%lu views=%d | car draws=%lu delta=%lu no-entry=%lu | traffic draws=%lu delta=%lu "
         "| world matrices moved=%lu\n",
         sec, st_steps, st_steps / sec, st_frames, st_frames / sec, st_zeroStepFrames, st_multiStepFrames,
         st_blendFrames, st_cutFrames, st_blendFrames ? st_alphaSum / st_blendFrames : 0.0,
         st_written, st_mismatch, st_jump, st_restoreSkip, g_entUsed, st_entsLastStep,
         st_camBlend, g_nViews, st_carDraws, st_carDelta, st_carNoEnt, st_ambDraws, st_ambDelta, st_worldDelta);
    st_frames = st_steps = st_blendFrames = st_cutFrames = st_written = st_mismatch = st_jump =
    st_restoreSkip = st_carDraws = st_carDelta = st_carNoEnt = st_ambDraws = st_ambDelta =
    st_worldDelta = st_camBlend = st_zeroStepFrames = st_multiStepFrames = 0;
    st_alphaSum = 0;
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
    FrameLimit();
    LONGLONG now = QpcNow();
    double step = 1.0 / g_cfg.simHz;
    double frame = g_lastFrame ? (double)(now - g_lastFrame) / (double)g_qpf.QuadPart : step;
    g_lastFrame = now;
    bool reset = frame > 0.25 || G<BYTE>(mc2::pTimerReset);
    if (reset) { frame = step; g_accum = 0.0; EntClear(); g_nViews = 0; }
    if (frame < 0.0) frame = 0.0;

    BYTE* gs = GameState();
    if (gs && gs[9]) {               // paused / in-race menu: one normal update, no blending
        RunPre(self);
        ((ThisFn)mc2::fnUpdate)(self, nullptr);
        g_accum = 0.0; return;
    }
    g_accum += frame;
    int steps = (int)floor(g_accum / step);
    if (steps > g_cfg.maxSteps) { steps = g_cfg.maxSteps; g_accum = 0.0; }
    else g_accum -= steps * step;
    if (steps == 0) st_zeroStepFrames++; else if (steps > 1) st_multiStepFrames++;

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
    }
    g_alpha = (float)(g_accum / step);
    if (g_alpha < 0) g_alpha = 0;
    if (g_alpha > 1) g_alpha = 1;
    g_frameSimOk = true;
    LogStats();
}

static void __fastcall Hook_RaceDraw(void* self, void*) {
    ApplyBlend();
    ((ThisFn)mc2::fnDraw)(self, nullptr);
    RestoreBlend();
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
    Logf("fixed step: installed (simHz=%.2f, maxSteps=%d, fpsLimit=%.1f)\n", g_cfg.simHz, g_cfg.maxSteps, g_cfg.fpsLimit);
    if (!g_cfg.interpolate) { Logf("interpolation: off\n"); return true; }
    bool rend = CheckBytes(mc2::fnSetWorld34, sw34, sizeof sw34) && CheckBytes(mc2::fnSetWorld44, sw44, sizeof sw44) &&
                G<DWORD>(mc2::vtCarDraw) == mc2::fnCarDraw && G<DWORD>(mc2::vtAmbDraw) == mc2::fnAmbDraw &&
                G<DWORD>(mc2::vtViewSetCam) == mc2::fnViewSetCam;
    if (!rend) { g_cfg.interpolate = 0; Logf("interpolation: render code differs, OFF\n"); return true; }
    WriteRel(mc2::callDrawRace, 0xE8, (void*)&Hook_RaceDraw);
    PatchVtable(mc2::vtViewSetCam, mc2::fnViewSetCam, (void*)&Hook_ViewSetCam);
    if (g_cfg.interpModels) {
        WriteRel(mc2::fnSetWorld34, 0xE9, (void*)&Hook_SetWorld34);
        WriteRel(mc2::fnSetWorld44, 0xE9, (void*)&Hook_SetWorld44);
        PatchVtable(mc2::vtCarDraw, mc2::fnCarDraw, (void*)&Hook_CarDraw);
        PatchVtable(mc2::vtAmbDraw, mc2::fnAmbDraw, (void*)&Hook_AmbDraw);
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
        if (TryInstall()) g_installed = true;
        else CloseHandle(CreateThread(nullptr, 0, InstallThread, nullptr, 0, nullptr));
    }
    return TRUE;
}
