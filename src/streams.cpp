#include "common.h"
#include "streams.h"
#include "game.h"
#include "log.h"
#include "patch.h"
#include "timing.h"

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
CRITICAL_SECTION g_streamLock;
static HANDLE g_streamEvent = nullptr;
bool g_asyncStreams = false;
unsigned long st_streamOpens = 0, st_streamSync = 0, st_streamFail = 0; double st_streamMaxMs = 0;
StreamLogEnt g_streamLog[8]; int g_nStreamLog = 0;   // first opens per stats period
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
unsigned long st_prefetchMB10 = 0;     // tenths of MB read
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
            DWORD orphan = 0;
            if (job->closeReq) {                       // closed while opening
                orphan = real;
                job->state = SP_FREE;
            } else if (real) { job->real = real; job->state = SP_READY; }
            else { job->state = SP_FAILED; st_streamFail++; }
            LeaveCriticalSection(&g_streamLock);
            if (orphan) g_ailClose(orphan);            // outside the lock: the main thread never waits on it
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
void StreamPoll() {
    if (!g_asyncStreams) return;
    EnterCriticalSection(&g_streamLock);
    for (int i = 0; i < kMaxStreams; i++)
        if (g_streams[i].state == SP_READY && (g_streams[i].volSet || g_streams[i].pauseSet ||
            (g_streams[i].startReq && !g_streams[i].startDone))) StreamApply(&g_streams[i]);
    LeaveCriticalSection(&g_streamLock);
}
void InstallAsyncStreams() {
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
