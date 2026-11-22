#include "common.h"
#include "interp.h"
#include "game.h"
#include "config.h"
#include "log.h"
#include "patch.h"
#include "timing.h"
#include "streams.h"

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
int  g_entUsed = 0;
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
// objects (not cameras) recorded at the last step with the step before: the
// only ones a frame can blend, so the per-frame passes don't sweep the table
static Ent* g_active[kMaxEnts]; static int g_nActive = 0;
void EntClear() { memset(g_ents, 0, sizeof g_ents); g_entUsed = 0; g_nActive = 0; }
static DWORD g_views[kMaxViews]; static unsigned long g_viewSeen[kMaxViews]; int g_nViews = 0;
unsigned long g_frameNo = 0;

// statistics
unsigned long st_frames, st_steps, st_blendFrames, st_cutFrames, st_written, st_mismatch,
    st_jump, st_restoreSkip, st_carDraws, st_carDelta, st_carNoEnt, st_ambDraws, st_ambDelta,
    st_worldDelta, st_camBlend, st_camRel, st_camAnchor, st_camHandoff, st_zeroStepFrames, st_multiStepFrames, st_entsLastStep,
    st_pedDraws, st_pedDelta, st_pedRagdoll, st_glowDelta, st_slowFrames, st_jumpLogged,
    st_ftHist[7], st_spfHist[4];   // frame interval buckets; steps per frame 0/1/2/3+
double st_alphaSum, st_ftMin = 1e9, st_ftMax = 0;

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
    if (e->prevStep && e->kind != KIND_CAM && g_nActive < kMaxEnts) g_active[g_nActive++] = e;
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
DWORD g_viewCam[kMaxViews];
static void TrackCamera(int vi) {
    DWORD cam = 0;
    if (!SafeRead(g_views[vi] + 0x2C, &cam) || !cam || IsBadReadPtr((void*)(cam + 0x0C), 0x74)) return;
    DWORD old = g_viewCam[vi]; g_viewCam[vi] = cam;    // also when another view already recorded it
    Ent* e = EntGet(cam, KIND_CAM, 13);
    if (!e || e->kind != KIND_CAM || e->curStep == g_step) return;
    float v[13]; memcpy(v, (void*)(cam + 0x0C), 48); v[12] = G<float>(cam + 0x7C);
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
// A ped that was hit is a ragdoll: its owner's flag [[model+0x14]+0x1D8] is set
// (mcPedStaticInst+0x54, ped+0x184), the animation update 0x542200 is skipped
// (0x541C40), and the ragdoll update (0x543140) poses the skeleton and writes
// the palette into [model+0x2C] every step without setting model+0x71. Nothing
// is swapped, so the newest pose is in [+0x2C]: reading [+0x38] there gave the
// stale pre-hit pose, and the hit animation was not blended.
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
    BYTE pending = 0, bones = 0, ragdoll = 0; DWORD buf = 0, data = 0, owner = 0;
    if (!SafeRead(model + 0x71, &pending)) return;
    if (SafeRead(model + 0x14, &owner) && owner && SafeRead(owner + 0x1D8, &ragdoll) && ragdoll) {
        pending = 1; st_pedRagdoll++;                                        // written in place
        static bool once = false;
        if (!once) { once = true; Logf("ragdoll pose seen (model %08lX owner %08lX)\n", model, owner); }
    }
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

void SnapshotStep() {
    g_step++; st_steps++;
    if (g_entUsed > kMaxEnts / 2) EntCompact();   // (moves entries: before the list is built)
    g_nActive = 0;
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
bool g_pedHook = false;   // peds are only blended when their draw is delta-wrapped:
                                 // the skinned body uses step-time bones, the root alone must not move
static Ent* g_written[kMaxEnts]; static int g_nWritten = 0;   // non-camera entities written this frame
static Ent* g_camsWritten[kMaxViews]; static int g_nCamsWritten = 0;
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

void ApplyBlend() {
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
    for (int i = 0; i < g_nActive; i++) {
        Ent& e = *g_active[i];
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
                Logf("  jump: %s %08lX moved %.2f m, turned %.1f deg (pos %.1f %.1f %.1f)\n",
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
    g_nCamsWritten = 0;
    for (int k = 0; k < nc; k++) {
        BlendCamera(cams[k], t);
        if (cams[k]->wrote) g_camsWritten[g_nCamsWritten++] = cams[k];
    }
}

void RestoreBlend() {
    if (!g_applied) return;
    for (int k = 0; k < g_nCamsWritten; k++) {
        Ent& e = *g_camsWritten[k];
        if (!e.wrote) continue;                         // (two views sharing one camera)
        e.wrote = false;
        float now[13]; memcpy(now, (void*)(e.addr + 0x0C), 48); now[12] = G<float>(e.addr + 0x7C);
        if (!memcmp(now, e.bl, sizeof now)) { memcpy((void*)(e.addr + 0x0C), e.cur, 48); G<float>(e.addr + 0x7C) = e.cur[12]; }
        else st_restoreSkip++;
    }
    for (int i = 0; i < g_nWritten; i++) {
        Ent& e = *g_written[i];
        if (!e.wrote) continue;
        e.wrote = false;
        if (!memcmp((void*)e.addr, e.bl, 48)) memcpy((void*)e.addr, e.cur, 48);
        else st_restoreSkip++;
    }
    g_nWritten = 0; g_nCamsWritten = 0;
    g_applied = false;
}

// --- model delta -------------------------------------------------------------
static bool  g_deltaOn = false;
static float g_delta[12];

void __cdecl Hook_SetWorld34(const float* m) {
    if (g_deltaOn && m) {
        float t[12]; Compose(m, g_delta, t); st_worldDelta++;
        ((M34to44Fn)mc2::fnM34to44)((void*)mc2::gWorld44, t);
    } else {
        ((M34to44Fn)mc2::fnM34to44)((void*)mc2::gWorld44, m);
    }
    ((VoidFn)mc2::fnFlushWorld)();
}
void __cdecl Hook_SetWorld44(const float* m) {
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
void __fastcall Hook_CarDraw(void* self, void*, int pass) {
    st_carDraws++;
    DWORD mtx = 0; SafeRead((DWORD)self + 0x18, &mtx);
    if (g_applied && mtx && !EntFind(mtx)) st_carNoEnt++;
    DrawWithDelta(mc2::fnCarDraw, self, pass, mtx, &st_carDelta);
}
void __fastcall Hook_AmbDraw(void* self, void*, int pass) {
    st_ambDraws++;
    DrawWithDelta(mc2::fnAmbDraw, self, pass, (DWORD)self + 0x24, &st_ambDelta);
}
// Draw the ped with a blended palette in [model+0x2C]. The draw's own swap
// (0x541BE0, at its end) may move that pointer to +0x38; the game's buffers
// are then put back exactly as its swap would have left them.
void __fastcall Hook_PedDraw(void* self, void*, int pass) {
    st_pedDraws++;
    DWORD model = (DWORD)self;
    PedPose* p = (g_applied && g_cfg.interpModels) ? PedFind(model, false) : nullptr;
    if (!p || p->curStep != g_step || p->prevStep != g_step - 1) { ((DrawFn)mc2::fnPedDraw)(self, nullptr, pass); return; }
    // bones past the skeleton's count are never read by the draw; keep the
    // game's values there so the palette is complete either way
    DWORD drawBuf = G<DWORD>(model + 0x2C);
    if (!drawBuf || IsBadReadPtr((void*)drawBuf, p->bones * 48)) { ((DrawFn)mc2::fnPedDraw)(self, nullptr, pass); return; }
    memcpy(p->bl, (const void*)drawBuf, p->bones * 48);   // (the game's buffer may hold only these)
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
void (*const kLightStubs[7])() = { Stub_L51CEA0, Stub_L51CB00, Stub_L51C6F0, Stub_L51C8A0,
                                         Stub_L51F1F0, Stub_L51CFF0, Stub_L51EF00 };

void __fastcall Hook_ViewSetCam(void* view, void*) {
    DWORD v = (DWORD)view; bool known = false;
    for (int i = 0; i < g_nViews; i++) if (g_views[i] == v) { known = true; g_viewSeen[i] = g_frameNo; }
    if (!known) {
        int slot = g_nViews < kMaxViews ? g_nViews++ : 0;
        if (g_nViews == kMaxViews && slot == 0)
            for (int i = 1; i < kMaxViews; i++) if (g_viewSeen[i] < g_viewSeen[slot]) slot = i;
        g_views[slot] = v; g_viewSeen[slot] = g_frameNo; g_viewCam[slot] = 0;
        Logf("camera view registered: %08lX (slot %d)\n", v, slot);
    }
    ((ThisFn)mc2::fnViewSetCam)(view, nullptr);
}
