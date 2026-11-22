// What the other parts use from config.cpp.
#pragma once
#include "common.h"

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
    int    fixMusicCrash = 1;    // music volume with no audio path (0x5DC32E)
    double dopplerHoldOnSwitch = 0.5;   // s of frozen Doppler after a camera change (0 = off)
    int    traceCameraAudio = 0;        // diagnostic: log voice play/stop around camera switches
    int    fixCameraCutAudio = 2;       // ticks the listener keeps its pose after a camera change
    double lodDistanceScale = 2.0;      // car / traffic LOD switch distances x this
    double ambientDistance = 300.0;     // m beyond an object's radius it is still drawn (game: 150)
    float  maxStepDistance = 12.0f;  // metres per step
    float  maxStepAngle = 60.0f;     // degrees per step (objects)
    float  maxCamAngle = 45.0f;      // degrees per step (camera direction)
    int    log = 0;
    double statsSeconds = 10.0;
};
extern Config g_cfg;
extern char g_asiDir[MAX_PATH];
void LoadConfig(HMODULE self);
