#include "common.h"
#include "loop.h"
#include "game.h"
#include "config.h"
#include "log.h"
#include "timing.h"
#include "interp.h"
#include "profiler.h"
#include "crashfix.h"
#include "doppler.h"
#include "streams.h"

// ---------------------------------------------------------------------------
// Loop hooks
// ---------------------------------------------------------------------------
static LONGLONG g_statsLast = 0;
static void LogStats() {
    if (!g_cfg.log) return;                    // nothing to write to
    LONGLONG now = QpcNow();
    if (!g_statsLast) { g_statsLast = now; return; }
    double sec = (double)(now - g_statsLast) / (double)g_qpf.QuadPart;
    if (sec < g_cfg.statsSeconds) return;
    g_statsLast = now;
    Logf("stats %.1fs: steps=%lu (%.1f/s) frames=%lu (%.1f fps) 0-step=%lu multi-step=%lu | blended=%lu "
         "cut=%lu cam-anchored=%lu cam-handoff=%lu avg t=%.2f | written=%lu mismatch=%lu jump=%lu restore-skipped=%lu tracked=%d (last step %lu) "
         "| cams world=%lu car-relative=%lu views=%d | car draws=%lu delta=%lu no-entry=%lu | traffic draws=%lu delta=%lu "
         "| world matrices moved=%lu | peds draws=%lu blended poses=%lu ragdoll steps=%lu | glows moved=%lu "
         "| frame time min=%.2fms max=%.2fms slow=%lu\n",
         sec, st_steps, st_steps / sec, st_frames, st_frames / sec, st_zeroStepFrames, st_multiStepFrames,
         st_blendFrames, st_cutFrames, st_camAnchor, st_camHandoff, st_blendFrames ? st_alphaSum / st_blendFrames : 0.0,
         st_written, st_mismatch, st_jump, st_restoreSkip, g_entUsed, st_entsLastStep,
         st_camBlend, st_camRel, g_nViews, st_carDraws, st_carDelta, st_carNoEnt, st_ambDraws, st_ambDelta, st_worldDelta,
         st_pedDraws, st_pedDelta, st_pedRagdoll, st_glowDelta, st_ftMin < 1e8 ? st_ftMin : 0.0, st_ftMax, st_slowFrames);
    st_frames = st_steps = st_blendFrames = st_cutFrames = st_written = st_mismatch = st_jump =
    st_restoreSkip = st_carDraws = st_carDelta = st_carNoEnt = st_ambDraws = st_ambDelta =
    st_worldDelta = st_camBlend = st_camRel = st_camAnchor = st_camHandoff = st_zeroStepFrames = st_multiStepFrames = 0;
    st_alphaSum = 0;
    st_pedDraws = st_pedDelta = st_pedRagdoll = st_glowDelta = st_slowFrames = st_jumpLogged = 0;
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
    if (st_musicNoPath) { Logf("  music volume without an audio path: %lu (set on the default path %lu)\n", st_musicNoPath, st_musicDefault); st_musicNoPath = st_musicDefault = 0; }
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
void __fastcall Hook_RacePre(void*, void*) {}
void __fastcall Hook_RacePost(void* self, void*) {
    if (g_preOpen) { g_preOpen = false; ((ThisFn)mc2::fnPostUpdate)(self, nullptr); }
}

void __fastcall Hook_RaceUpdate(void* self, void*) {
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
void __fastcall Hook_RaceDraw(void* self, void*) {
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
