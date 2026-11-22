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

#include "src/common.h"
#include "src/game.h"
#include "src/config.h"
#include "src/log.h"
#include "src/patch.h"
#include "src/timing.h"
#include "src/interp.h"
#include "src/profiler.h"
#include "src/distance.h"
#include "src/crashfix.h"
#include "src/doppler.h"
#include "src/streams.h"
#include "src/loop.h"
#include "src/crashlog.h"

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
        if (g_cfg.log) {
            auto* dos = (IMAGE_DOS_HEADER*)inst; auto* nt = (IMAGE_NT_HEADERS*)((BYTE*)inst + dos->e_lfanew);
            g_modLo = (DWORD)inst; g_modHi = g_modLo + nt->OptionalHeader.SizeOfImage;
            AddVectoredExceptionHandler(1, CrashLog);
        }
        if (!g_cfg.enabled) { Logf("disabled in ini\n"); return TRUE; }
        if (g_cfg.profileAudio) ProfInit();
        if (g_cfg.asyncStreams) InstallAsyncStreams();
        if (g_cfg.fixExitCrash) InstallExitCrashFix();
        if (g_cfg.fixMusicCrash) InstallMusicCrashFix();
        if (g_cfg.lodDistanceScale != 1.0) InstallLodScale();
        if (g_cfg.ambientDistance != 150.0) InstallAmbientDistance();
        if (g_cfg.traceCameraAudio) InstallSndTrace();
        if (g_cfg.dopplerHoldOnSwitch > 0 || g_cfg.fixCameraCutAudio > 0 || g_sndTrace)
            InstallDopplerFix();
        if (TryInstall()) g_installed = true;
        else CloseHandle(CreateThread(nullptr, 0, InstallThread, nullptr, 0, nullptr));
    }
    return TRUE;
}
