#include "common.h"
#include "crashfix.h"
#include "log.h"
#include "patch.h"

// ---------------------------------------------------------------------------
// Exit crash (fixExitCrash=1)
//
// The debug font ("bigfont"/"smallfont", object at [0x85B1FC]) is created at
// 0x5F9DB0, which also registers 0x5F9D70 as a shutdown callback (list 0x860300).
// The main shutdown (0x401488 -> 0x53A870 -> 0x5F9E30) frees it first, with a
// null check, and clears the pointer; when the callback list runs afterwards,
// 0x5F9D70 does `dec [ecx+0x18]` on the cleared pointer: access violation at
// mc2.exe+1F9D76 (null+0x18). The callback now returns at once when the font
// is already gone (it would also free the shared font page 0x85AE78 again).
// ---------------------------------------------------------------------------
__attribute__((naked)) static void FontShutdownStub() {
    asm volatile(
        "movl 0x85B1FC, %ecx\n\t"
        "testl %ecx, %ecx\n\t"
        "jz 1f\n\t"
        "pushl $0x5F9D76\n\t"         // continue at dec [ecx+0x18]
        "ret\n"
        "1:\n\t"
        "ret\n");
}
void InstallExitCrashFix() {
    static const BYTE exp[] = { 0x8B, 0x0D, 0xFC, 0xB1, 0x85, 0x00, 0xFF, 0x49, 0x18 };
    if (!CheckBytes(0x5F9D70, exp, sizeof exp)) { Logf("exit crash fix: code differs, OFF\n"); return; }
    WriteRel(0x5F9D70, 0xE9, (void*)&FontShutdownStub);
    Logf("exit crash fix: installed\n");
}

// ---------------------------------------------------------------------------
// Music crash (fixMusicCrash=1)
//
// DirectMusic songs live in a table at 0x8567D0 (0x420 each, handle -> 0x5DBDE0).
// The four songs' audio paths [song+0x41C] are created once at audio init
// (0x5DC3A0 -> 0x5DBE40, Performance8::CreateStandardAudioPath). When that
// fails ("Failed to create AudioPath for song %d"; seen with Wine's or a
// wrapper's DirectMusic) the path stays null for the whole session: the song
// still plays, through the performance's default audio path (PlaySegmentEx
// with a null path, 0x5DC0C7), but the music-volume command (0x5DC2E0, from the
// audio command queue 0x5D9860) does `mov ecx,[esi+0x41C]; mov edx,[ecx]` with
// no check and reads address 0 (crash at mc2+1DC334, at the menu after the
// intro). Without a path the volume is now set on the default audio path
// (Performance8::GetDefaultAudioPath, slot 51), where that song plays.
// ---------------------------------------------------------------------------
unsigned long st_musicNoPath = 0, st_musicDefault = 0;
typedef long (__stdcall *GetDefPathFn)(void* perf, void** path);
typedef long (__stdcall *SetVolFn)(void* path, long vol, DWORD ms);
typedef unsigned long (__stdcall *ReleaseFn)(void* obj);
extern "C" __attribute__((used)) void __cdecl MC2FF_MusicVolumeNoPath(DWORD song, long vol) {
    st_musicNoPath++;
    static bool logged = false;
    if (!logged) { logged = true; Logf("music: song %ld has no audio path (DirectMusic failed to create it): volume goes to the default path\n",
                                      (long)((song - 0x8567D0) / 0x420)); }
    void* perf = nullptr; void* path = nullptr; DWORD vt = 0, fn = 0;
    if (!SafeRead(0x8567CC, (DWORD*)&perf) || !perf || !SafeRead((DWORD)perf, &vt) || !SafeRead(vt + 51 * 4, &fn) || !fn) return;
    if (((GetDefPathFn)fn)(perf, &path) < 0 || !path) return;
    DWORD pvt = 0, setv = 0, rel = 0;
    if (SafeRead((DWORD)path, &pvt) && SafeRead(pvt + 5 * 4, &setv) && SafeRead(pvt + 2 * 4, &rel) && setv && rel) {
        ((SetVolFn)setv)(path, vol, 0); st_musicDefault++;
        ((ReleaseFn)rel)(path);                                   // Get... AddRef'd it
    }
}
// replaces `mov ecx,[esi+0x41C]; mov edx,[ecx]` at 0x5DC32E (eax = volume)
__attribute__((naked)) static void Stub_MusicVolume() {
    asm volatile(
        "movl 0x41C(%esi), %ecx\n\t"
        "testl %ecx, %ecx\n\t"
        "jz 1f\n\t"
        "movl (%ecx), %edx\n\t"
        "ret\n"
        "1:\n\t"
        "pushl %eax\n\t"
        "pushl %esi\n\t"
        "call _MC2FF_MusicVolumeNoPath\n\t"
        "addl $12, %esp\n\t"              // args + our return address
        "xorl %eax, %eax\n\t"             // S_OK, as SetVolume would return
        "pushl $0x5DC33D\n\t"             // pop esi; ret 4 (skip the call)
        "ret\n");
}
void InstallMusicCrashFix() {
    static const BYTE exp[] = { 0x8B, 0x8E, 0x1C, 0x04, 0x00, 0x00, 0x8B, 0x11, 0x6A, 0x00, 0x50, 0x51, 0xFF, 0x52, 0x14 };
    if (!CheckBytes(0x5DC32E, exp, sizeof exp)) { Logf("music crash fix: code differs, OFF\n"); return; }
    WriteRel(0x5DC32E, 0xE8, (void*)&Stub_MusicVolume);
    WriteNops(0x5DC333, 3);
    Logf("music crash fix: installed\n");
}
