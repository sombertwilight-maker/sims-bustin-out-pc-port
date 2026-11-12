/* Diagnostic-only call profiler (built with -DRECOMP_CALL_PROFILE).
 * Counts guest functions called while a gamepad button is held (the "hot"
 * set) against everything called the rest of the time (the baseline), and
 * writes the functions only the hot window ran to prof_N.txt on release.
 * Also records what one Go Here call runs (prof_gohere.txt). */
#ifdef RECOMP_CALL_PROFILE
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "input/xinput_xbox.h"

#define HSZ (1u << 18)
static uint32_t base_k[HSZ], hot_k[HSZ], gh_k[HSZ];
static uint32_t base_c[HSZ], hot_c[HSZ], gh_c[HSZ];
static volatile int s_hot, s_gh;
volatile int g_prof_gohere;

static void add(uint32_t *k, uint32_t *c, uint32_t va)
{
    uint32_t h = (va * 2654435761u) >> 14;
    for (int i = 0; i < 64; i++, h = (h + 1) & (HSZ - 1)) {
        if (k[h] == va) { c[h]++; return; }
        if (k[h] == 0) { k[h] = va; c[h] = 1; return; }
    }
}
static uint32_t get(uint32_t *k, uint32_t *c, uint32_t va)
{
    uint32_t h = (va * 2654435761u) >> 14;
    for (int i = 0; i < 64; i++, h = (h + 1) & (HSZ - 1)) {
        if (k[h] == va) return c[h];
        if (k[h] == 0) return 0;
    }
    return 0;
}

void recomp_call_profile_hit(uint32_t va)
{
    if (g_prof_gohere) add(gh_k, gh_c, va);
    if (s_hot) add(hot_k, hot_c, va); else add(base_k, base_c, va);
}

static void dump(const char *name, uint32_t *k, uint32_t *c, int only_new)
{
    FILE *f = fopen(name, "w");
    if (!f) return;
    for (uint32_t i = 0; i < HSZ; i++)
        if (k[i] && (!only_new || get(base_k, base_c, k[i]) == 0))
            fprintf(f, "%08X %u base %u\n", k[i], c[i], get(base_k, base_c, k[i]));
    fclose(f);
}

void sims_prof_gohere_done(void)
{
    static int n;
    char name[64];
    snprintf(name, sizeof name, "prof_gohere_%d.txt", n++);
    dump(name, gh_k, gh_c, 0);
    memset(gh_k, 0, sizeof gh_k); memset(gh_c, 0, sizeof gh_c);
}

/* Called from the input filter for port 0. */
void sims_prof_input(XBOX_INPUT_STATE *st)
{
    static int held, n;
    static DWORD released;
    static int mask = -1;
    int down;
    if (mask < 0) {
        const char *e = getenv("SIMS_PROF_BUTTON");    /* analog index: 0 A, 1 B, 2 X, 3 Y, 4 Black, 5 White */
        mask = e ? atoi(e) : 1;
    }
    down = st->Gamepad.bAnalogButtons[mask & 7] > 30;
    if (down) { held = 1; s_hot = 1; return; }
    if (held) { held = 0; released = GetTickCount(); }
    if (s_hot && GetTickCount() - released > 1500) {
        char name[64];
        s_hot = 0;
        snprintf(name, sizeof name, "prof_%d.txt", n++);
        dump(name, hot_k, hot_c, 1);
        memset(hot_k, 0, sizeof hot_k); memset(hot_c, 0, sizeof hot_c);
    }
}
#endif
