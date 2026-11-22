#include "common.h"
#include "log.h"
#include "config.h"

// ---------------------------------------------------------------------------
// Log
// ---------------------------------------------------------------------------
static FILE* g_log = nullptr;
void Logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Logf(const char* fmt, ...) {
    if (!g_cfg.log) return;
    if (!g_log) {
        char p[MAX_PATH]; snprintf(p, MAX_PATH, "%sMC2FrameFix.log", g_asiDir);
        g_log = fopen(p, "w");
        if (!g_log) return;
    }
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fflush(g_log);
}
