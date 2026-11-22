// ---------------------------------------------------------------------------
// Game addresses
// ---------------------------------------------------------------------------
#pragma once
#include "common.h"

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
