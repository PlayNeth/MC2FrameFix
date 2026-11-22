// What the other parts use from interp.cpp.
#pragma once
#include "common.h"

extern int g_entUsed;
void EntClear();
// registered camViewCS objects (seen at render time)
static const int kMaxViews = 4;
extern int g_nViews;
extern unsigned long g_frameNo;
extern unsigned long st_frames, st_steps, st_blendFrames, st_cutFrames, st_written, st_mismatch,
 st_jump, st_restoreSkip, st_carDraws, st_carDelta, st_carNoEnt, st_ambDraws, st_ambDelta,
 st_worldDelta, st_camBlend, st_camRel, st_camAnchor, st_camHandoff, st_zeroStepFrames, st_multiStepFrames, st_entsLastStep,
 st_pedDraws, st_pedDelta, st_pedRagdoll, st_glowDelta, st_slowFrames, st_jumpLogged,
 st_ftHist[7], st_spfHist[4];
extern double st_alphaSum, st_ftMin, st_ftMax;
extern DWORD g_viewCam[kMaxViews];
void SnapshotStep();
extern bool g_pedHook;
void ApplyBlend();
void RestoreBlend();
void __cdecl Hook_SetWorld34(const float* m);
void __cdecl Hook_SetWorld44(const float* m);
void __fastcall Hook_CarDraw(void* self, void*, int pass);
void __fastcall Hook_AmbDraw(void* self, void*, int pass);
void __fastcall Hook_PedDraw(void* self, void*, int pass);
void __fastcall Hook_ViewSetCam(void* view, void*);
// The light functions' stubs, in the order of mc2::lightFns.
extern void (*const kLightStubs[7])();
