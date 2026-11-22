// What the other parts use from patch.cpp.
#pragma once
#include "common.h"

bool CheckBytes(DWORD at, const void* exp, size_t n);
bool CheckCall(DWORD at, DWORD target);
void WriteRel(DWORD at, BYTE op, const void* target);
void WriteNops(DWORD at, int n);
bool PatchVtable(DWORD slot, DWORD expect, const void* fn);
template <class T> static bool SafeRead(DWORD a, T* out) {
    if (!a || IsBadReadPtr((void*)a, sizeof(T))) return false;
    *out = *(const T*)a; return true;
}
