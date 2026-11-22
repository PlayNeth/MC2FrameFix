#include "common.h"
#include "config.h"

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

Config g_cfg;
char g_asiDir[MAX_PATH];
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
"; Fix a crash at the menu after the intro (music volume set on a DirectMusic\n"
"; audio path that failed to be created; it goes to the default path instead)\n"
"fixMusicCrash=1\n"
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
"; Diagnostic: time every Miles / DirectMusic call and log what stalls frames\n"
"profileAudio=0\n"
"\n"
"; Diagnostic: log which sounds are started / stopped around camera switches\n"
"traceCameraAudio=0\n"
"\n"
"; Log file (MC2FrameFix.log) and how often stats are written, in seconds\n"
"log=0\n"
"statsSeconds=10\n";

static double IniD(const char* k, double def) {
    char b[64], d[64]; snprintf(d, sizeof d, "%g", def);
    GetPrivateProfileStringA("MC2FrameFix", k, d, b, sizeof b, g_iniPath);
    return atof(b);
}
static int IniI(const char* k, int def) { return GetPrivateProfileIntA("MC2FrameFix", k, def, g_iniPath); }

void LoadConfig(HMODULE self) {
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
    g_cfg.fixMusicCrash  = IniI("fixMusicCrash", 1);
    g_cfg.dopplerHoldOnSwitch = IniD("dopplerHoldOnSwitch", 0.5);
    g_cfg.traceCameraAudio = IniI("traceCameraAudio", 0);
    g_cfg.fixCameraCutAudio = IniI("fixCameraCutAudio", 2);
    g_cfg.lodDistanceScale = IniD("lodDistanceScale", 2.0);
    g_cfg.ambientDistance = IniD("ambientDistance", 300.0);
    if (!(g_cfg.ambientDistance >= 50.0)) g_cfg.ambientDistance = 50.0;
    if (g_cfg.ambientDistance > 3000.0) g_cfg.ambientDistance = 3000.0;
    if (!(g_cfg.lodDistanceScale >= 0.25)) g_cfg.lodDistanceScale = 0.25;
    if (g_cfg.lodDistanceScale > 20.0) g_cfg.lodDistanceScale = 20.0;
    if (g_cfg.fixCameraCutAudio < 0) g_cfg.fixCameraCutAudio = 0;
    if (g_cfg.fixCameraCutAudio > 8) g_cfg.fixCameraCutAudio = 8;
    g_cfg.maxStepDistance = (float)IniD("maxStepDistance", 12.0);
    g_cfg.maxStepAngle   = (float)IniD("maxStepAngle", 60.0);
    g_cfg.maxCamAngle    = (float)IniD("maxCameraAngle", 45.0);
    g_cfg.log            = IniI("log", 0);
    g_cfg.statsSeconds   = IniD("statsSeconds", 10.0);
    // the race timer clamps a step to 0.04 s (0x402F70): a slower rate would run
    // the game in slow motion
    if (g_cfg.simHz < 25) g_cfg.simHz = 25;
    if (g_cfg.simHz > 1000) g_cfg.simHz = 1000;
    if (g_cfg.maxSteps < 1) g_cfg.maxSteps = 1;
    if (g_cfg.maxSteps > 64) g_cfg.maxSteps = 64;
    if (g_cfg.statsSeconds < 1) g_cfg.statsSeconds = 1;
}
