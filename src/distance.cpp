#include "common.h"
#include "distance.h"
#include "config.h"
#include "log.h"
#include "patch.h"

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
void InstallLodScale() {
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
void InstallAmbientDistance() {
    static const BYTE exp[] = { 0xC7, 0x86, 0x18, 0x02, 0x00, 0x00, 0x00, 0x00, 0x16, 0x43 };
    if (!CheckBytes(0x518828, exp, sizeof exp)) { Logf("ambient distance: code differs, OFF\n"); return; }
    float v = (float)g_cfg.ambientDistance;
    DWORD old; VirtualProtect((void*)0x51882E, 4, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)0x51882E, &v, 4);
    VirtualProtect((void*)0x51882E, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)0x51882E, 4);
    Logf("ambient distance: %.0f m (game 150)\n", g_cfg.ambientDistance);
}
