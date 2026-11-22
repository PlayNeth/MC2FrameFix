// What the other parts use from loop.cpp.
#pragma once
#include "common.h"

void __fastcall Hook_RacePre(void*, void*);
void __fastcall Hook_RacePost(void* self, void*);
void __fastcall Hook_RaceUpdate(void* self, void*);
void __fastcall Hook_RaceDraw(void* self, void*);
