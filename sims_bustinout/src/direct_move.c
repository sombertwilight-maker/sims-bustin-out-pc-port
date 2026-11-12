/*
 * Direct movement: the left stick walks your Sim, camera-relative, instead
 * of moving the cursor. The right stick (camera) is untouched.
 *
 * How it works, in the game's own terms (addresses are the USA XBE):
 *
 *  - 0x310C34[player] is the player's Sim. Its position, in 1/16 tile units,
 *    is the int32 pair at +0x110 / +0x114 (found by watching it walk).
 *  - 0x310C40[player] is the player's cursor controller. +0xBC is the cursor
 *    position (x, y, z floats), +0x3C the player number, +0xD8 the camera
 *    (eye at +0x374, look-at point at +0x394). sub_00039000 is its per-frame
 *    update: it reads the left stick and moves the cursor, rotated by the
 *    camera's heading.
 *  - Pressing A runs sub_0003A6E0 on the controller, which collects what is
 *    under the cursor and hands it to sub_00059400 on the controller's UI
 *    object (+0xE4). Over bare floor that is Go Here: sub_00059310 puts the
 *    marker on the cursor's tile (sub_00057580) and gives the Sim the walk.
 *
 * The mod runs at the top of sub_00039000 (a hook the post-processor adds):
 * while the stick is held it issues Go Here a short way ahead of the Sim in
 * the stick's direction, a few times a second, and keeps the cursor just in
 * front of the Sim, so the camera follows and A still uses whatever the Sim
 * is facing. The stick itself is hidden from the game (xbox_input_filter),
 * but only while the cursor update is running -- menus, pie menus and
 * dialogs still get it.
 */
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#include "input/xinput_xbox.h"
#include "video/gfx_config.h"

#if defined(_MSC_VER)
#  define RECOMP_TLS __declspec(thread)
#else
#  define RECOMP_TLS __thread
#endif
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern ptrdiff_t g_xbox_mem_offset;
void sub_00059400(void);

#define GMEM32(a)  (*(volatile uint32_t *)((uintptr_t)(uint32_t)(a) + g_xbox_mem_offset))
#define GMEMI(a)   (*(volatile int32_t  *)((uintptr_t)(uint32_t)(a) + g_xbox_mem_offset))
#define GMEMF(a)   (*(volatile float    *)((uintptr_t)(uint32_t)(a) + g_xbox_mem_offset))

#define SIM_TABLE     0x310C34u
#define CURSOR_TABLE  0x310C40u

static volatile LONG  s_stick_x, s_stick_y;   /* raw left stick, port 0 */
static volatile LONG  s_rstick_x, s_rstick_y;  /* raw right stick: the follow camera turns and zooms with it */
static float s_stick_scale = 1.0f;
static volatile DWORD s_cursor_tick;          /* last time the cursor update ran */

static int dm_on(void)
{
    return xbox_GfxConfig()->direct_move != 0;
}

/* Port 0's left stick: noted for the mod, and hidden from the game while
 * the in-lot cursor is what it would move. */
#ifdef RECOMP_CALL_PROFILE
void sims_prof_input(XBOX_INPUT_STATE *st);
void sims_prof_gohere_done(void);
extern volatile int g_prof_gohere;
#endif

static void dm_pick_input(XBOX_INPUT_STATE *st);

static void dm_input_filter(DWORD port, XBOX_INPUT_STATE *st)
{
    if (port != 0)
        return;
#ifdef RECOMP_CALL_PROFILE
    sims_prof_input(st);
#endif
    InterlockedExchange(&s_stick_x, st->Gamepad.sThumbLX);
    InterlockedExchange(&s_stick_y, st->Gamepad.sThumbLY);
    InterlockedExchange(&s_rstick_x, st->Gamepad.sThumbRX);
    InterlockedExchange(&s_rstick_y, st->Gamepad.sThumbRY);
    dm_pick_input(st);
    if (dm_on() && GetTickCount() - s_cursor_tick < 200) {
        st->Gamepad.sThumbLX = 0;
        st->Gamepad.sThumbLY = 0;
        st->Gamepad.sThumbRX = 0;      /* the mod's follow camera turns and zooms instead */
        st->Gamepad.sThumbRY = 0;
    }
}

void sims_direct_move_init(void)
{
    const char *e = getenv("SIMS_DM_STICK_SCALE");
    if (e && atof(e) > 0.0)
        s_stick_scale = (float)atof(e);
    xbox_input_filter = dm_input_filter;
}

static int valid_ptr(uint32_t p)
{
    return (p >= 0x00010000u && p < 0x04000000u) || (p >= 0x80000000u && p < 0x84000000u);
}

/* Go Here at (x, y): exactly what pressing A over bare floor does
 * (sub_0003A6E0 -> sub_00059400 with an empty "under the cursor" list ->
 * sub_00059310), with the cursor moved there for the call. The UI object
 * that takes it is the controller's +0xE4 (its +0x10 points back). Guest
 * registers the hook point does not expect changed are put back. */
static int dm_log(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("SIMS_DM_LOG");   /* on while the mod is experimental */
        on = !(e && *e == '0');
    }
    return on;
}

static void go_here(uint32_t ctrl, float x, float y)
{
    uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx, esp = g_esp, ui, empty;
    float ox, oy;

    ui = GMEM32(ctrl + 0xE4);
    if (!valid_ptr(ui) || GMEM32(ui + 0x10) != ctrl)
        return;
    ox = GMEMF(ctrl + 0xBC); oy = GMEMF(ctrl + 0xC0);
    GMEMF(ctrl + 0xBC) = x;
    GMEMF(ctrl + 0xC0) = y;
    g_esp -= 4;
    GMEM32(g_esp) = 0;              /* the empty list: nothing under the cursor */
    empty = g_esp;
    g_esp -= 4;
    GMEM32(g_esp) = empty;          /* the argument */
    g_esp -= 4;
    GMEM32(g_esp) = 0;              /* return address; the callee pops it and the argument */
    g_ecx = ui;
#ifdef RECOMP_CALL_PROFILE
    g_prof_gohere = 1;
#endif
    sub_00059400();
#ifdef RECOMP_CALL_PROFILE
    g_prof_gohere = 0;
    sims_prof_gohere_done();
#endif
    if (dm_log())
        fprintf(stderr, "[DM] go here %.2f %.2f -> %u (ui %08X, esp %08X/%08X)\n", x, y, g_eax & 0xFF,
                ui, g_esp, esp);
    g_esp = esp;
    g_eax = eax; g_ecx = ecx; g_edx = edx;
    GMEMF(ctrl + 0xBC) = ox;
    GMEMF(ctrl + 0xC0) = oy;
}

/* Calls a guest __thiscall function from a hook: arguments on the guest
 * stack, a zero return address, the callee pops them. Guest registers the
 * hook point does not expect changed are put back. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
static uint32_t guest_thiscall(uint32_t fn, uint32_t thisp, int nargs, const uint32_t *args)
{
    uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx, esp = g_esp, ret;
    recomp_func_t f = recomp_lookup(fn);
    int i;
    if (!f)
        return 0;
    for (i = nargs - 1; i >= 0; i--) {
        g_esp -= 4;
        GMEM32(g_esp) = args[i];
    }
    g_esp -= 4;
    GMEM32(g_esp) = 0;
    g_ecx = thisp;
    f();
    ret = g_eax;
    g_esp = esp;
    g_eax = eax; g_ecx = ecx; g_edx = edx;
    return ret;
}

/* Could the Sim stand at (x, y), in 1/16 tiles? The router's own test: it
 * moves her, then asks the Sim (through her virtual base: +4 holds the
 * offset table, entry 2 the offset) vfunc 0x1CC for her level and vfunc
 * 0xC0 (point, level, 0, 0) whether the new point is free, and stops her
 * if not. Asking first lets her slide along walls instead. */
static int dm_can_stand(uint32_t sim, int32_t x, int32_t y)
{
    uint32_t vbt = GMEM32(sim + 4), thisp, vt, level, args[4], esp = g_esp, ok;
    if (!valid_ptr(vbt))
        return 1;
    thisp = sim + 4 + GMEM32(vbt + 8);
    vt = GMEM32(thisp);
    if (!valid_ptr(thisp) || !valid_ptr(vt))
        return 1;
    level = guest_thiscall(GMEM32(vt + 0x1CC), thisp, 0, NULL);
    g_esp -= 8;                       /* the point, on the guest stack */
    GMEMI(g_esp) = x;
    GMEMI(g_esp + 4) = y;
    args[0] = g_esp; args[1] = level; args[2] = 0; args[3] = 0;
    ok = guest_thiscall(GMEM32(vt + 0xC0), thisp, 4, args) & 0xFF;
    g_esp = esp;
    return ok != 0;
}

/* ── Driving the game's own walk ───────────────────────────────────────
 * A walking Sim is moved by her motion controller (vtable 0x2E7EE0), which
 * sub_000B5BB0 ticks once a frame. Its fields, as this mod uses them:
 *   +0x04 the Sim             +0x0C path vector (int x,y points, 1/16 tile)
 *   +0x18 point walked to      +0x1C state: 1 turn to start, 2 walk,
 *                                            3 stop, 4 done, 5 final approach
 *   +0x28/+0x2C exact y, x (1/16 tile)       +0x38 body heading, radians:
 *                                            atan2(-dx, dy) of the motion
 *   +0x70/+0x74 final point y, x             +0x78 time scale this tick
 *   +0x80 running (64.96 vs 17.28 units/s top speed, run footsteps)
 *   +0x84 speed / top speed    +0x88 footstep loop started
 *   +0x8C speed, 1/16 tile per second
 * The router moves her toward the point ahead and turns her body toward the
 * direction of the path segment she is on, so the hook makes that segment
 * start where she stands and point where the stick (smoothed) points: body
 * and motion stay together, like a character in a 3D game. Speed follows
 * the stick: a light push walks (slower the lighter it is), a full push
 * runs. Letting go stops her where she is. A Go Here starts a walk when she
 * is standing still; once it is under way the stick drives it. */
#define R_PATH   0x0C
#define R_IDX    0x18
#define R_STATE  0x1C
#define R_Y      0x28
#define R_X      0x2C
#define R_HEAD   0x38
#define R_TY     0x70
#define R_TX     0x74
#define R_TSCALE 0x78
#define R_RUN    0x80
#define R_SNDON  0x88
#define R_SPEED  0x8C

#define DM_DEAD       0.25f   /* stick dead zone */
#define DM_FULLWALK   0.60f   /* full walking pace from here */
#define DM_RUN_ON     0.90f   /* run from here ... */
#define DM_RUN_OFF    0.80f   /* ... until the stick drops below this */
#define DM_TURN_RATE  14.0f   /* body turn, radians per second */
#define DM_WALK_TOP   17.28f  /* the router's walking top speed (1/16 tile/s) */
#define DM_RUN_TOP    64.96f

#define GMEM8(a)   (*(volatile uint8_t  *)((uintptr_t)(uint32_t)(a) + g_xbox_mem_offset))

static volatile LONG s_dm_active;           /* stick held, in live mode, mod on */
static volatile float s_dm_dx, s_dm_dy;     /* world direction of the stick */
static volatile float s_dm_mag;
static volatile LONG s_dm_run;              /* full push: run */
static volatile float s_dm_walkfrac;        /* light push: fraction of walking pace */
static volatile LONG s_dm_walking;          /* the player's router was seen walking */
static volatile LONG s_dm_boxed;            /* walls ahead and to both sides */
static volatile LONG s_dm_blocked;          /* ... remembered until the stick turns */
/* Walking her straight out of her idle loop, with no Go Here. */
static volatile LONG s_dm_direct;           /* the mod began this walk itself */
static volatile LONG s_dm_hold_anim;        /* her idle animation is held meanwhile */
static volatile LONG s_dm_end_anim;         /* ... and ended when she stops */
static volatile LONG s_dm_direct_failed;    /* no room in the path vector: use Go Here */
static volatile uint16_t s_dm_anim_tree;    /* tree that last animated her */
static volatile LONG s_dm_anim_idle;        /* ... was one of her idle trees */
static volatile uint32_t s_dm_anim_frame;   /* ... and when */
static volatile uint32_t s_dm_router_tick;  /* frame it was last seen */
static volatile uint32_t s_dm_frame;
static volatile uint32_t s_dm_sim;
static volatile float s_dm_mx, s_dm_my;     /* the way she is walking under the stick */
static volatile uint32_t s_dm_move_frame;   /* ... seen then */
/* Go Heres the mod sent that have not started yet (1/16 tiles). */
#define DM_ISSUED 8
static volatile int32_t  s_dm_issued_x[DM_ISSUED], s_dm_issued_y[DM_ISSUED];
static volatile LONG     s_dm_issued_n;
static volatile uint32_t s_dm_issue_frame;                /* frame the last was sent */

static void set_pt(uint32_t b, uint32_t i, int32_t x, int32_t y)
{
    GMEMI(b + i * 8) = x;
    GMEMI(b + i * 8 + 4) = y;
}

static float wrap_pi(float a)
{
    while (a > 3.14159265f) a -= 6.28318531f;
    while (a < -3.14159265f) a += 6.28318531f;
    return a;
}

static int dm_pick_dir(float px, float py, float want, float *out);
static volatile LONG s_dm_inside;      /* she stands partly inside something */
static void dm_door_align(float hx, float hy, float *dx, float *dy);

/* Starts a walk with no Go Here: the stick's path (where she stands, then
 * two and three tiles ahead) goes into the router's path vector, and its
 * vfunc 0x2C (sub_000B44B0, the route primitive's "begin") sets the walk up
 * from her position and facing: state 1, mode +0x10 = 1, point 0. The
 * router hook steers it from the next tick. Needs room for three points in
 * the vector (it keeps its buffer between walks). */
static int dm_begin_walk(uint32_t r)
{
    uint32_t vec = GMEM32(r + R_PATH), b, cap, vt;
    int32_t x, y;
    float dx = s_dm_dx, dy = s_dm_dy;
    if (!valid_ptr(vec))
        return 0;
    b = GMEM32(vec); cap = GMEM32(vec + 8);
    if (!valid_ptr(b) || cap < b + 24 || cap - b > 0x400)
        return 0;
    x = GMEMI(s_dm_sim + 0x110);
    y = GMEMI(s_dm_sim + 0x114);
    {
        /* The stick's way, lined up with a doorway she is heading through,
         * or the nearest free way beside it. */
        float a;
        dm_door_align(x / 16.0f, y / 16.0f, &dx, &dy);
        s_dm_inside = !dm_can_stand(s_dm_sim, x, y);
        if (dm_pick_dir((float)x, (float)y, atan2f(-dx, dy), &a) < 0) {
            static uint32_t lt;
            if (dm_log() && s_dm_frame - lt > 60) {
                lt = s_dm_frame;
                fprintf(stderr, "[DM] no way to start at %.2f %.2f toward %.2f %.2f\n", x / 16.0f, y / 16.0f, dx, dy);
            }
            return -1;                       /* hemmed in that way: stay put */
        }
        dx = -sinf(a); dy = cosf(a);
    }
    set_pt(b, 0, x, y);
    set_pt(b, 1, x + (int32_t)(dx * 10.0f), y + (int32_t)(dy * 10.0f));
    set_pt(b, 2, x + (int32_t)(dx * 16.0f), y + (int32_t)(dy * 16.0f));
    GMEM32(vec + 4) = b + 24;
    vt = GMEM32(r);
    guest_thiscall(GMEM32(vt + 0x2C), r, 0, NULL);
    if (dm_log())
        fprintf(stderr, "[DM] walk begun from idle at %.2f %.2f (state %u)\n", x / 16.0f, y / 16.0f,
                GMEM32(r + R_STATE));
    return 1;
}


/* ── Doors ─────────────────────────────────────────────────────────────
 * In normal play a route into another room runs the door's portal script
 * (semi-global tree 2017, "portal function"): walk up to the door and face
 * through it, then "play open sound" (2023) and "set door open" (201B, param
 * 1), walk to the other side, "play close sound" (2016) and "set door open"
 * (param 0). Those helpers act on the stack object -- the door -- and return
 * at once. So the mod runs just them, on her VM with the door as the stack
 * object (her VM's vfunc 0x20: door, params, tree, 0): as she comes within
 * reach of a door, walking, it opens with its sound; once she is clear of
 * it, it closes. She never stops.
 *   Each door is a pair of portal objects, one on each side of the wall, in
 * the world's portal list (world = [person + 0xBC] through her virtual base
 * 5; vfunc 0x88 the count, 0x84(i) the entry, entry vfunc 0 the other half).
 * The trees are pushed at her next animate call (her VM is in her idle loop,
 * held, then), which returns 3 -- "a frame was pushed, run me again after
 * it" -- as the route primitive does after pushing the portal script. */
#define DM_PORTALS 64
static struct { uint32_t obj; int32_t tx, ty, qx, qy, level, partner; uint8_t open; uint32_t far_since; }
    s_portals[DM_PORTALS];
static int s_portal_n;
static uint32_t s_portal_frame, s_portal_world;
#define DM_DQ 16
static struct { uint32_t door; uint16_t tree, param; } s_dq[DM_DQ];  /* trees to push, in order */
static volatile LONG s_dq_n;


/* Her person object (the route primitive's "this"): 0x5F0 below the Sim
 * pointer, which is its VM base (virtual base 3). */
static uint32_t dm_person(uint32_t sim)
{
    uint32_t p = sim - 0x5F0u, vbt = GMEM32(p + 4);
    if (!valid_ptr(vbt) || p + 4 + GMEM32(vbt + 0xC) != sim)
        return 0;
    return p;
}

static int dm_level(uint32_t iface)
{
    uint32_t vt = GMEM32(iface);
    if (!valid_ptr(iface) || !valid_ptr(vt))
        return -1;
    return (int)(guest_thiscall(GMEM32(vt + 0x1A8), iface, 0, NULL) & 0xFFFF);
}

static void dm_scan_portals(uint32_t person)
{
    uint32_t vbt = GMEM32(person + 4), world, vt, cnt, i, j;
    uint32_t was_open[DM_PORTALS];
    int was_n = 0;
    for (i = 0; i < (uint32_t)s_portal_n; i++)
        if (s_portals[i].open && was_n < DM_PORTALS)
            was_open[was_n++] = s_portals[i].obj;
    s_portal_n = 0;
    world = GMEM32(person + 0xBC + GMEM32(vbt + 0x14));
    s_portal_world = world;
    if (!valid_ptr(world) || !valid_ptr(vt = GMEM32(world)))
        return;
    cnt = guest_thiscall(GMEM32(vt + 0x88), world, 0, NULL);
    for (i = 0; i < cnt && s_portal_n < DM_PORTALS; i++) {
        uint32_t a = i, it = guest_thiscall(GMEM32(vt + 0x84), world, 1, &a), o;
        if (!valid_ptr(it) || !valid_ptr(GMEM32(it)))
            continue;
        o = guest_thiscall(GMEM32(GMEM32(it) + 0xC), it, 0, NULL);
        s_portals[s_portal_n].obj = it;
        s_portals[s_portal_n].tx = GMEMI(it + 0x110) >> 4;
        s_portals[s_portal_n].ty = GMEMI(it + 0x114) >> 4;
        s_portals[s_portal_n].level = valid_ptr(o) ? dm_level(o + GMEM32(GMEM32(o) + 8)) : -1;
        s_portals[s_portal_n].qx = 0x7FFF;
        s_portals[s_portal_n].qy = 0x7FFF;
        s_portals[s_portal_n].open = 0;
        s_portals[s_portal_n].far_since = 0;
        for (j = 0; j < (uint32_t)was_n; j++)
            if (was_open[j] == it)
                s_portals[s_portal_n].open = 1;
        {
            uint32_t q = guest_thiscall(GMEM32(GMEM32(it)), it, 0, NULL);
            if (valid_ptr(q)) {
                s_portals[s_portal_n].qx = GMEMI(q + 0x110) >> 4;
                s_portals[s_portal_n].qy = GMEMI(q + 0x114) >> 4;
            }
        }
        s_portal_n++;
    }
    /* The other half must be next door; if vfunc 0 gave something else,
     * the adjacent portal is used. */
    for (i = 0; i < (uint32_t)s_portal_n; i++) {
        int32_t ddx = s_portals[i].qx - s_portals[i].tx, ddy = s_portals[i].qy - s_portals[i].ty;
        if (abs(ddx) + abs(ddy) == 1)
            continue;
        s_portals[i].qx = s_portals[i].qy = 0x7FFF;
        for (j = 0; j < (uint32_t)s_portal_n; j++) {
            ddx = s_portals[j].tx - s_portals[i].tx; ddy = s_portals[j].ty - s_portals[i].ty;
            if (j != i && abs(ddx) + abs(ddy) == 1 && s_portals[j].level == s_portals[i].level) {
                s_portals[i].qx = s_portals[j].tx;
                s_portals[i].qy = s_portals[j].ty;
                break;
            }
        }
    }
    for (i = 0; i < (uint32_t)s_portal_n; i++) {
        s_portals[i].partner = -1;
        for (j = 0; j < (uint32_t)s_portal_n; j++)
            if (j != i && s_portals[j].tx == s_portals[i].qx && s_portals[j].ty == s_portals[i].qy)
                s_portals[i].partner = (int32_t)j;
    }
    if (dm_log()) {
        fprintf(stderr, "[DM] %d door halves in world %08X\n", s_portal_n, world);
        for (i = 0; i < (uint32_t)s_portal_n; i++)
            fprintf(stderr, "[DM]   %08X tile %d,%d -> %d,%d level %d\n", s_portals[i].obj, s_portals[i].tx,
                    s_portals[i].ty, s_portals[i].qx, s_portals[i].qy, s_portals[i].level);
    }
}

static void dm_portals_fresh(uint32_t person)
{
    uint32_t vbt = GMEM32(person + 4);
    if (!s_portal_n || s_dm_frame - s_portal_frame > 600 ||
        GMEM32(person + 0xBC + GMEM32(vbt + 0x14)) != s_portal_world) {
        s_portal_frame = s_dm_frame;
        dm_scan_portals(person);
    }
}

static void dq_add(uint32_t door, uint16_t tree, uint16_t param)
{
    if (s_dq_n >= DM_DQ)
        return;
    s_dq[s_dq_n].door = door;
    s_dq[s_dq_n].tree = tree;
    s_dq[s_dq_n].param = param;
    s_dq_n++;
}

/* Opens doors she walks up to and closes the ones she has left. (hx, hy):
 * her position in tiles; (mx, my): the way she is walking, or 0 standing. */
static void dm_doors(float hx, float hy, float mx, float my, int walking)
{
    uint32_t person = dm_person(s_dm_sim);
    int i, level;
    if (!person)
        return;
    dm_portals_fresh(person);
    level = dm_level(person + 4 + GMEM32(GMEM32(person + 4) + 8));
    for (i = 0; i < s_portal_n; i++) {
        int j = s_portals[i].partner;
        float cx, cy, dx, dy, d, toward;
        int in_tile;
        if (j < 0 || j < i)
            continue;                         /* each door once, from its first half */
        cx = (float)(s_portals[i].tx + s_portals[j].tx + 1) * 0.5f;
        cy = (float)(s_portals[i].ty + s_portals[j].ty + 1) * 0.5f;
        dx = cx - hx; dy = cy - hy;
        d = sqrtf(dx * dx + dy * dy);
        toward = dx * mx + dy * my;
        in_tile = ((int)hx == s_portals[i].tx && (int)hy == s_portals[i].ty) ||
                  ((int)hx == s_portals[j].tx && (int)hy == s_portals[j].ty);
        if (!s_portals[i].open) {
            /* Walking up to it (within a tile and a third, heading its way),
             * or in its doorway. Her side's half is the stack object. */
            float across = fabsf(mx * (float)(s_portals[j].tx - s_portals[i].tx) +
                                 my * (float)(s_portals[j].ty - s_portals[i].ty));
            if (walking && d < 1.35f && toward > 0.0f && across > 0.5f) {   /* going through, not past */
                int k = s_portals[i].level == level ? i : j;
                if (s_portals[k].level != level) {
                    float di = fabsf(s_portals[i].tx + 0.5f - hx) + fabsf(s_portals[i].ty + 0.5f - hy);
                    float dj = fabsf(s_portals[j].tx + 0.5f - hx) + fabsf(s_portals[j].ty + 0.5f - hy);
                    k = di <= dj ? i : j;
                }
                dq_add(s_portals[k].obj, 0x2023, 0);      /* play open sound */
                dq_add(s_portals[k].obj, 0x201B, 1);      /* set door open */
                s_portals[i].open = s_portals[j].open = 1;
                s_portals[i].far_since = 0;
                if (dm_log())
                    fprintf(stderr, "[DM] door %08X/%08X opening, she is %.2f tiles off\n",
                            s_portals[i].obj, s_portals[j].obj, d);
            }
        } else if (!in_tile && d > 1.9f) {
            /* Clear of it for a third of a second: shut it. */
            if (!s_portals[i].far_since)
                s_portals[i].far_since = s_dm_frame ? s_dm_frame : 1;
            else if (s_dm_frame - s_portals[i].far_since > 20) {
                dq_add(s_portals[i].obj, 0x2016, 0);      /* play close sound */
                dq_add(s_portals[i].obj, 0x201B, 0);      /* set door shut */
                s_portals[i].open = s_portals[j].open = 0;
                s_portals[i].far_since = 0;
                if (dm_log())
                    fprintf(stderr, "[DM] door %08X/%08X closing\n", s_portals[i].obj, s_portals[j].obj);
            }
        } else {
            s_portals[i].far_since = 0;
        }
    }
}

/* Pushes one of the door's semi-global trees onto her VM, the door as the
 * stack object and `param` as its first argument. */
static int dm_push_tree(uint32_t door, uint16_t tree, uint16_t param)
{
    uint32_t person = dm_person(s_dm_sim), vbt, iface, vt, vm, args[4], esp = g_esp, ok;
    if (!person || !valid_ptr(door) || !valid_ptr(GMEM32(door + 4)))
        return 0;
    iface = door + 4 + GMEM32(GMEM32(door + 4) + 8);
    vbt = GMEM32(person + 4);
    vm = person + 4 + GMEM32(vbt + 0xC);
    vt = GMEM32(vm);
    if (!valid_ptr(iface) || !valid_ptr(vt))
        return 0;
    g_esp -= 16;                                   /* the arguments, copied into the frame */
    GMEM32(g_esp) = param;                         /* u16 param, then zeros */
    GMEM32(g_esp + 4) = 0;
    GMEM32(g_esp + 8) = 0;
    GMEM32(g_esp + 12) = 0;
    args[0] = iface; args[1] = g_esp; args[2] = tree; args[3] = 0;
    ok = guest_thiscall(GMEM32(vt + 0x20), vm, 4, args) & 0xFF;
    g_esp = esp;
    if (dm_log())
        fprintf(stderr, "[DM] door %08X: tree %04X(%u) %s\n", door, tree, param, ok ? "pushed" : "refused");
    return ok != 0;
}

/* Ways to go: the one asked for, then fanned out to either side, nearest
 * first. The side that worked last time is tried first, so she does not
 * flip between two ways round an obstacle. A way is open when she can
 * stand half a tile and a tile along it, and it is not one the router
 * refused in the last half second. Returns the index tried (0: the way
 * asked for) or -1. */
static float s_bl_dx, s_bl_dy;
static uint32_t s_bl_frame;
static int s_pick_side = 1;

static int dm_way_open(float px, float py, float a)
{
    float cx = -sinf(a), cy = cosf(a);
    if (s_dm_frame - s_bl_frame < 30 && cx * s_bl_dx + cy * s_bl_dy > 0.85f)
        return 0;
    /* Standing partly inside something (where she got up from a chair, an
     * edge of a bookshelf): the points right by her fail whichever way she
     * goes, so only the way out is looked at -- half a tile and three
     * quarters out. */
    if (s_dm_inside)
        return dm_can_stand(s_dm_sim, (int32_t)(px + cx * 8.0f + 0.5f), (int32_t)(py + cy * 8.0f + 0.5f)) &&
               dm_can_stand(s_dm_sim, (int32_t)(px + cx * 12.0f + 0.5f), (int32_t)(py + cy * 12.0f + 0.5f));
    /* An eighth, a quarter, three eighths and half a tile out: she can
     * walk right up to furniture, and a wall or door jamb in between is
     * not stepped over. */
    return dm_can_stand(s_dm_sim, (int32_t)(px + cx * 2.0f + 0.5f), (int32_t)(py + cy * 2.0f + 0.5f)) &&
           dm_can_stand(s_dm_sim, (int32_t)(px + cx * 4.0f + 0.5f), (int32_t)(py + cy * 4.0f + 0.5f)) &&
           dm_can_stand(s_dm_sim, (int32_t)(px + cx * 6.0f + 0.5f), (int32_t)(py + cy * 6.0f + 0.5f)) &&
           dm_can_stand(s_dm_sim, (int32_t)(px + cx * 8.0f + 0.5f), (int32_t)(py + cy * 8.0f + 0.5f));
}

static int dm_pick_dir(float px, float py, float want, float *out)
{
    /* Up to 72 degrees off the stick, never sideways or back: pushing
     * straight at a wall stops her (sliding either way along it would only
     * flip back and forth), pushing at it slantwise slides her along it. */
    static const float fan[] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 1.25f };
    int i, k;
    for (i = 0; i < (int)(sizeof fan / sizeof fan[0]); i++)
        for (k = 0; k < 2; k++) {
            int side = k == 0 ? s_pick_side : -s_pick_side;
            float a;
            if (i == 0 && k == 1)
                continue;
            a = wrap_pi(want + (float)side * fan[i]);
            if (dm_way_open(px, py, a)) {
                if (i)
                    s_pick_side = side;
                *out = a;
                return i;
            }
        }
    return -1;
}

/* Heading through a doorway (pushing across its wall, in front of the gap,
 * up to a tile and a half short of it): steer for the middle of the gap,
 * so she does not catch on the frame. (hx, hy) in tiles; (dx, dy) the way
 * she means to go, changed in place. */
static void dm_door_align(float hx, float hy, float *dx, float *dy)
{
    int i;
    uint32_t person = dm_person(s_dm_sim);
    if (!person)
        return;
    dm_portals_fresh(person);
    for (i = 0; i < s_portal_n; i++) {
        int j = s_portals[i].partner;
        float cx, cy, ax, ay, sx, t, e, nx, ny, l;
        if (j < 0 || j < i)
            continue;
        ax = (float)(s_portals[j].tx - s_portals[i].tx);      /* across the wall */
        ay = (float)(s_portals[j].ty - s_portals[i].ty);
        sx = *dx * ax + *dy * ay;
        if (fabsf(sx) < 0.85f)
            continue;                                           /* not pushing (fairly straight) through it */
        if (sx < 0) { ax = -ax; ay = -ay; }
        cx = (float)(s_portals[i].tx + s_portals[j].tx + 1) * 0.5f;
        cy = (float)(s_portals[i].ty + s_portals[j].ty + 1) * 0.5f;
        t = (cx - hx) * ax + (cy - hy) * ay;                    /* how far ahead the wall is */
        if (t < -0.6f || t > 1.2f)
            continue;
        e = (hx - cx) * -ay + (hy - cy) * ax;                   /* off the middle, sideways */
        if (fabsf(e) > 0.65f)
            continue;
        nx = ax - (-ay) * e * 3.0f;                             /* firmly toward the middle */
        ny = ay - ax * e * 3.0f;
        l = sqrtf(nx * nx + ny * ny);
        *dx = nx / l;
        *dy = ny / l;
        return;
    }
}

/* Called at the top of sub_000B5BB0, g_ecx = a router. */
void sims_dm_router(void)
{
    static int was_active;
    uint32_t r = g_ecx, vec, b, e, n, idx, k, state;
    float px, py;
    if (!valid_ptr(r) || GMEM32(r) != 0x002E7EE0u || GMEM32(r + 4) != s_dm_sim || !s_dm_sim)
        return;
    state = GMEM32(r + R_STATE);
    if (state != 1 && state != 2) {
        if (s_dm_direct && state != 3 && state != 5) {
            /* Her walk is over: she is idle again. The held idle animation
             * is ended (next animate call) and her loop carries on. */
            GMEM32(r + 0x10) = 0;
            s_dm_direct = 0;
            s_dm_hold_anim = 0;
            s_dm_end_anim = 1;
            was_active = 0;
        }
        if (state == 3 || state == 5 || !s_dm_active || s_dm_direct || s_dm_blocked ||
            !s_dm_anim_idle || s_dm_frame - s_dm_anim_frame > 600)
            return;
        {
            int ok = dm_begin_walk(r);
            if (ok < 0) {
                s_dm_boxed = 1;              /* nowhere to go that way: wait for the stick to turn */
                return;
            }
            if (!ok) {
                s_dm_direct_failed = 1;
                return;
            }
        }
        s_dm_direct = 1;
        s_dm_hold_anim = 1;
        s_dm_issued_n = 0;
        state = GMEM32(r + R_STATE);
        if (state != 1 && state != 2)
            return;
    }
    s_dm_walking = 1;
    s_dm_router_tick = s_dm_frame;
    vec = GMEM32(r + R_PATH);
    if (!valid_ptr(vec))
        return;
    b = GMEM32(vec); e = GMEM32(vec + 4);
    if (!valid_ptr(b) || e < b || e - b > 0x400)
        return;
    n = (e - b) / 8;
    idx = GMEM32(r + R_IDX);
    if (n < 2 || idx >= n)
        return;
    /* Under the stick the router walks segment 1 (point 0 is where she
     * stands, point 1 ahead of her) forever; it stops when it reaches the
     * last point, so the path needs a third. A short path is lengthened
     * within the vector's capacity (the points are rewritten below). */
    if (s_dm_active && n < 3) {
        uint32_t cap = GMEM32(vec + 8);
        if (!(cap >= b + 24 && cap - b <= 0x400))
            return;
        GMEM32(vec + 4) = b + 24;
        n = 3;
    }
    px = GMEMF(r + R_X);
    py = GMEMF(r + R_Y);
    if (!(px > 0.0f && py > 0.0f && px < 4096.0f && py < 4096.0f))
        return;
    /* A walk the mod asked for that starts after the stick was let go (the
     * Sim was busy until now): stop it where she stands. Walks the player
     * orders with A are left alone. */
    if (s_dm_issued_n) {
        int mine = 0, j;
        for (k = 0; k < n && !mine; k++)
            for (j = 0; j < s_dm_issued_n && j < DM_ISSUED; j++)
                if (GMEMI(b + k * 8) == s_dm_issued_x[j] && GMEMI(b + k * 8 + 4) == s_dm_issued_y[j])
                    mine = 1;
        if (mine) {
            s_dm_issued_n = 0;
            if (!s_dm_active)
                was_active = 1;     /* falls through to the stop below */
        }
    }
    if (s_dm_active) {
        float scale = GMEMF(r + R_TSCALE), dt, cur, want, diff, step, h, mx, my, top, v, dev;
        int32_t ix = (int32_t)(px + 0.5f), iy = (int32_t)(py + 0.5f);
        int run = s_dm_run != 0;
        if (!(scale > 0.0f && scale < 8.0f)) scale = 1.0f;
        dt = scale / 60.0f;
        /* Body heading: toward the stick at DM_TURN_RATE. */
        cur = GMEMF(r + R_HEAD);
        /* Walls and furniture: the router stops her when a step lands
         * where she cannot stand. So before each step the way ahead is
         * tested (half a tile and a tile out): the stick's way first --
         * lined up with the middle of a doorway she is going through --
         * then ways fanned out to either side of it, nearest first, so she
         * slides along walls and round corners and furniture. Sliding,
         * she goes at the stick's speed along the obstacle. If no way is
         * open she stops. */
        dev = 0.0f;
        {
            float a, dx = s_dm_dx, dy = s_dm_dy;
            int found;
            /* The router's own test can refuse a step the look-ahead let
             * through (a diagonal brushing a corner): that direction counts
             * as a wall for half a second, and the mark is cleared so the
             * walk goes on. */
            if (GMEM32(r + 0x10) == 3) {
                s_bl_dx = -sinf(GMEMF(r + R_HEAD)); s_bl_dy = cosf(GMEMF(r + R_HEAD));
                s_bl_frame = s_dm_frame;
                if (dm_log())
                    fprintf(stderr, "[DM] router refused a step at %.2f %.2f heading %.2f\n", px / 16.0f, py / 16.0f,
                            GMEMF(r + R_HEAD));
                GMEM32(r + 0x10) = 1;
            }
            s_dm_inside = !dm_can_stand(s_dm_sim, (int32_t)(px + 0.5f), (int32_t)(py + 0.5f));
            dm_door_align(px / 16.0f, py / 16.0f, &dx, &dy);
            want = atan2f(-dx, dy);
            if (!was_active && state == 1)
                cur = want;
            found = dm_pick_dir(px, py, want, &a);
            if (found < 0 && dm_log()) {
                static uint32_t lt;
                if (s_dm_frame - lt > 30) {
                    float q;
                    lt = s_dm_frame;
                    fprintf(stderr, "[DM] boxed at %.2f %.2f want %.2f (blocked mark %.2f %.2f age %u):", px / 16.0f,
                            py / 16.0f, want, s_bl_dx, s_bl_dy, s_dm_frame - s_bl_frame);
                    for (q = -1.2f; q < 1.25f; q += 0.6f) {
                        float cx = -sinf(want + q), cy = cosf(want + q);
                        fprintf(stderr, " [%+.1f %d%d%d]", q,
                                dm_can_stand(s_dm_sim, (int32_t)(px + cx * 4.0f + 0.5f), (int32_t)(py + cy * 4.0f + 0.5f)),
                                dm_can_stand(s_dm_sim, (int32_t)(px + cx * 8.0f + 0.5f), (int32_t)(py + cy * 8.0f + 0.5f)),
                                dm_can_stand(s_dm_sim, (int32_t)(px + cx * 16.0f + 0.5f), (int32_t)(py + cy * 16.0f + 0.5f)));
                    }
                    fprintf(stderr, "\n");
                    if (getenv("SIMS_DM_GRID")) {                 /* debug */
                        int gx, gy;
                        for (gy = 12; gy >= -12; gy -= 3) {
                            fprintf(stderr, "[DMG] %+3d ", gy);
                            for (gx = -12; gx <= 12; gx += 3)
                                fputc(gx == 0 && gy == 0 ? '@' :
                                      dm_can_stand(s_dm_sim, (int32_t)px + gx, (int32_t)py + gy) ? '.' : '#', stderr);
                            fputc('\n', stderr);
                        }
                    }
                }
            }
            if (found < 0) {
                /* Boxed in: stop where she is; the cursor hook waits for
                 * the stick to point somewhere else. */
                for (k = 0; k < n; k++)
                    set_pt(b, k, ix, iy);
                GMEMF(r + R_TY) = py;
                GMEMF(r + R_TX) = px;
                GMEM32(r + R_IDX) = 1;
                was_active = 0;
                s_dm_boxed = 1;
                return;
            }
            if (getenv("SIMS_DM_TRACE"))                                  /* debug */
                fprintf(stderr, "[DMT] %u pos %.3f %.3f sim %.3f %.3f want %.2f got %.2f (#%d) inside %d\n", s_dm_frame,
                        px / 16.0f, py / 16.0f, GMEMI(s_dm_sim + 0x110) / 16.0f, GMEMI(s_dm_sim + 0x114) / 16.0f, want, a,
                        found, !dm_can_stand(s_dm_sim, (int32_t)(px + 0.5f), (int32_t)(py + 0.5f)));
            dev = fabsf(wrap_pi(a - want));
            if (fabsf(wrap_pi(a - cur)) > 0.6f)
                cur = a;                             /* a sharp deflection: at once */
            want = a;
        }
        diff = wrap_pi(want - cur);
        step = DM_TURN_RATE * dt;
        if (diff > step) diff = step;
        if (diff < -step) diff = -step;
        h = wrap_pi(cur + diff);
        GMEMF(r + R_HEAD) = h;
        mx = -sinf(h); my = cosf(h);
        s_dm_mx = mx; s_dm_my = my; s_dm_move_frame = s_dm_frame;
        dm_doors(px / 16.0f, py / 16.0f, -sinf(want), cosf(want), 1);
        /* The path: from where she stands, straight along her heading.
         * +0x368 is the last segment the router treats as already walked
         * (a Go Here can start with it at 1, which would skip segment 1). */
        set_pt(b, 0, ix, iy);
        for (k = 1; k < n; k++) {
            /* Close ahead: the router checks a point along this path, and
             * one far out would stop her short of furniture. */
            float d = 10.0f + 6.0f * (float)(k - 1);
            set_pt(b, k, ix + (int32_t)(mx * d), iy + (int32_t)(my * d));
        }
        GMEM32(r + R_IDX) = 1;
        GMEM32(r + 0x368) = 0;
        GMEMF(r + R_TY) = (float)GMEMI(b + (n - 1) * 8 + 4);
        GMEMF(r + R_TX) = (float)GMEMI(b + (n - 1) * 8);
        /* Walk or run. The footstep loop restarts when that changes. */
        if ((int)GMEM32(r + R_RUN) != run) {
            GMEM32(r + R_RUN) = (uint32_t)run;
            GMEM8(r + R_SNDON) = 0;
        }
        /* Speed: the router accelerates to its top speed in half a second;
         * a light push holds her below walking pace, and a push from
         * standing starts her at a brisk pace rather than from zero. */
        top = run ? DM_RUN_TOP : DM_WALK_TOP * s_dm_walkfrac;
        if (dev > 0.2f)
            top *= cosf(dev);                               /* sliding: the stick's share along it */
        v = GMEMF(r + R_SPEED);
        if (v > top)
            v = top;
        if (!was_active && v < top * 0.5f)
            v = top * 0.5f;
        GMEMF(r + R_SPEED) = v;
        was_active = 1;
    } else if (was_active) {
        /* Let go: stop where she is. (A blocked mark would leave the
         * router waiting for a re-plan nobody asks for.) */
        if (GMEM32(r + 0x10) == 3)
            GMEM32(r + 0x10) = 1;
        int32_t ix = (int32_t)(px + 0.5f), iy = (int32_t)(py + 0.5f);
        for (k = idx ? idx - 1 : 0; k < n; k++)
            set_pt(b, k, ix, iy);
        GMEMF(r + R_TY) = py;
        GMEMF(r + R_TX) = px;
        was_active = 0;
    }
    if (dm_log()) {
        static uint32_t t;
        if (s_dm_frame - t > 30) {
            t = s_dm_frame;
            fprintf(stderr, "[DM] router %08X st %u idx %u/%u pos %.2f %.2f hd %.2f v %.1f run %u active %ld\n",
                    r, state, idx, n, px / 16.0f, py / 16.0f, GMEMF(r + R_HEAD), GMEMF(r + R_SPEED),
                    GMEM32(r + R_RUN), (long)s_dm_active);
        }
    }
}

/* ── Idle animations ───────────────────────────────────────────────────
 * Called at the top of the animate primitive (sub_000B5F60): g_ecx is the
 * router, [esp+4] the VM stack frame (its first word is the behaviour tree
 * being run). Standing with nothing to do, a Sim runs tree 0x208A, which
 * plays the idle gestures in 0x208B..0x2093 (seen in the log). Noting that
 * tells the router hook she is idle, so the stick can walk her straight
 * off. Meanwhile her idle loop is held where it is: this returns 2 (still
 * playing); when she stops it returns 1 once (done) and the loop goes on.
 * Returns 0 to let the primitive run. SIMS_DM_IDLE_TREES=hex,... overrides
 * the idle trees; SIMS_DM_ANIMLOG=1 logs the trees. */
static int s_anim_log = -1;
static uint16_t s_idle_trees[32];
static int s_idle_n = -1;

static void load_idle_trees(void)
{
    const char *e = getenv("SIMS_DM_IDLE_TREES");
    s_idle_n = 0;
    if (!e || !*e) {
        uint16_t t;
        for (t = 0x208A; t <= 0x2093; t++)
            s_idle_trees[s_idle_n++] = t;
        return;
    }
    while (*e && s_idle_n < 32) {
        char *end;
        unsigned long v = strtoul(e, &end, 16);
        if (end == e) break;
        s_idle_trees[s_idle_n++] = (uint16_t)v;
        e = *end ? end + 1 : end;
    }
}

/* Is the animate call being made from one of her idle trees? */
static int dm_anim_is_idle(void)
{
    uint32_t frame = GMEM32(g_esp + 4);
    uint16_t tree;
    int i;
    if (!valid_ptr(frame))
        return 0;
    tree = *(volatile uint16_t *)((uintptr_t)frame + g_xbox_mem_offset);
    if (s_idle_n < 0)
        load_idle_trees();
    for (i = 0; i < s_idle_n; i++)
        if (s_idle_trees[i] == tree)
            return 1;
    return 0;
}

int sims_dm_anim(void)
{
    uint32_t r = g_ecx, frame;
    uint16_t tree;
    int i, idle = 0;
    if (!valid_ptr(r) || GMEM32(r) != 0x002E7EE0u || GMEM32(r + 4) != s_dm_sim || !s_dm_sim)
        return 0;
    /* Door trees waiting: one per call, pushed on top of her idle loop --
     * only then: never into the middle of something the game has her doing. */
    if (s_dq_n && dm_on() && (s_dm_hold_anim || dm_anim_is_idle())) {
        uint32_t door = s_dq[0].door;
        uint16_t tree = s_dq[0].tree, param = s_dq[0].param;
        LONG k;
        for (k = 1; k < s_dq_n; k++)
            s_dq[k - 1] = s_dq[k];
        s_dq_n--;
        if (dm_push_tree(door, tree, param))
            return 3;
    }
    if (s_dm_hold_anim && dm_on())
        return 2;
    if (s_dm_end_anim) {
        /* End it as the primitive does when an animation finishes:
         * "animating" (+0x68) and +0xE8 cleared, person data 0x13 set to
         * 0 through the Sim's vfunc 0x74. */
        uint32_t sim = s_dm_sim, vt = GMEM32(sim), args[2] = { 0x13, 0 };
        s_dm_end_anim = 0;
        GMEM8(r + 0x68) = 0;
        GMEM8(r + 0xE8) = 0;
        if (valid_ptr(vt))
            guest_thiscall(GMEM32(vt + 0x74), sim, 2, args);
        return 1;
    }
    frame = GMEM32(g_esp + 4);
    if (!valid_ptr(frame))
        return 0;
    tree = *(volatile uint16_t *)((uintptr_t)frame + g_xbox_mem_offset);
    if (s_idle_n < 0)
        load_idle_trees();
    for (i = 0; i < s_idle_n; i++)
        if (s_idle_trees[i] == tree)
            idle = 1;
    s_dm_anim_tree = tree;
    s_dm_anim_idle = idle;
    s_dm_anim_frame = s_dm_frame;

    if (s_anim_log < 0) {
        const char *e = getenv("SIMS_DM_ANIMLOG");
        s_anim_log = e && *e == '1';
    }
    if (s_anim_log) {
        static uint16_t last;
        if (tree != last) {
            last = tree;
            fprintf(stderr, "[DM] anim tree %04X idle %d\n", tree, idle);
        }
    }
    return 0;
}

/* Called at the top of the cursor controller's draw (sub_0003A960, g_ecx
 * the controller). Returns 1 to skip drawing: live mode, the player's own
 * cursor, the left stick moving the Sim. The draw also copies the cursor
 * position to 0x3100C8 (read elsewhere), so that is still done. */
static int dm_hide_for(uint32_t ctrl)
{
    uint32_t player, gm, a4, mode;
    if (!dm_on() || !valid_ptr(ctrl))
        return 0;
    player = GMEM32(ctrl + 0x3C);
    if (player > 1 || GMEM32(CURSOR_TABLE + player * 4) != ctrl)
        return 0;
    gm = GMEM32(0x310C54u);
    a4 = GMEM32(ctrl + 0xA4);
    mode = valid_ptr(gm) ? GMEM32(gm + 0x11C) : 99u;
    return !(a4 > 1 || (mode != 0 && mode != 5));
}

/* The HUD manager's marker rings at the cursor (sub_00055950, g_ecx the
 * manager, [esp+4] the cursor controller): hidden the same way. */
int sims_dm_hide_rings(void)
{
    return dm_hide_for(GMEM32(g_esp + 4));
}

int sims_dm_hide_cursor(void)
{
    uint32_t ctrl = g_ecx, player, gm, a4, mode;
    if (!dm_on() || !valid_ptr(ctrl))
        return 0;
    player = GMEM32(ctrl + 0x3C);
    if (player > 1 || GMEM32(CURSOR_TABLE + player * 4) != ctrl)
        return 0;
    gm = GMEM32(0x310C54u);
    a4 = GMEM32(ctrl + 0xA4);
    mode = valid_ptr(gm) ? GMEM32(gm + 0x11C) : 99u;
    if (a4 > 1 || (mode != 0 && mode != 5))
        return 0;
    GMEM32(0x3100C8u) = GMEM32(ctrl + 0xBC);
    GMEM32(0x3100CCu) = GMEM32(ctrl + 0xC0);
    GMEM32(0x3100D0u) = GMEM32(ctrl + 0xC4);
    return 1;
}

/* ── Things nearby (A) ────────────────────────────────────────────────
 * With the stick moving her, the cursor rides in front of her, and some
 * things -- a plate on the stove, a tray on the floor -- are hard or
 * impossible to get it onto. So A opens a list of the things within reach,
 * nearest first (the D-pad or stick picks, A chooses, B closes), and the
 * chosen one gets the game's own action menu, as if the cursor were on it.
 *   Everything the cursor can point at is in the game's table of
 * selectables ([[0x310C48]+4]+4: a list of nodes, node+0x10 the next, +0x18
 * the object id, +0x1C the selectable; selectable +0xA8/+0xAC its x, y in
 * tiles, +0x324 the game object). Furniture and the like (selectable
 * classes 0x2E5870, 0x2E5958, 0x2E5B28) have their definition at
 * object - 0x68 and its name at +0x14, and things several tiles big (class
 * 0x2E5A40 with object class 0x2F52B0: stereo, bed, dining table) at
 * object + 0x15C; people (0x2E4588) are the Sim's VM
 * part (her person object + 0x5C8), named from the character table by the
 * GUID in their definition (person + 0x720, +0x1C). Doors and windows
 * (0x2E5A40 with object class 0x2F4EB0) are left out.
 *   The chosen one gets its menu the way the game's A handler opens one
 * (dm_pick_menu). */
#define DM_PICK_N      10
#define DM_PICK_REACH  4.0f      /* tiles */
static struct { uint32_t sel; char name[64]; } s_pick[DM_PICK_N];
static int s_pick_n;
static volatile LONG s_pick_req;       /* input: A pressed -- gather what's nearby */
static volatile LONG s_lit;            /* something under the cursor glows: A is the game's */
static volatile LONG s_game_menu;      /* the menu the list opened is up: A is the game's */

static int dm_printable(uint32_t p, char *out, int n)
{
    int i;
    if (!valid_ptr(p))
        return 0;
    for (i = 0; i < n - 1; i++) {
        uint8_t c = GMEM8(p + i);
        if (!c) break;
        if (c < 32 || c > 126) return 0;
        out[i] = (char)c;
    }
    out[i] = 0;
    return i >= 2;
}

/* "appliance_stove_x_steel" -> "Stove steel"; "counter_c_kitchen_ikea" ->
 * "Counter kitchen ikea"; "food_meal_light" -> "Plate of food". */
static void dm_pretty(const char *raw, char *out, int n)
{
    static const char *drop[] = { "appliance", "electronics", "plumbing", "seating", "art", "plant", "trash",
                                  "misc", "deco", "zz", "object", "toy", "career", NULL };
    char buf[64], *tok, *ctx = NULL;
    int first = 1, k = 0, i;
    if (!strncmp(raw, "food_", 5)) {
        snprintf(out, n, "%s", strstr(raw, "empty") || strstr(raw, "dirty") ? "Dirty dishes" : "Plate of food");
        return;
    }
    snprintf(buf, sizeof buf, "%s", raw);
    out[0] = 0;
    for (tok = strtok_s(buf, "_", &ctx); tok; tok = strtok_s(NULL, "_", &ctx)) {
        int skip = strlen(tok) <= 1;
        if (first)
            for (i = 0; drop[i]; i++)
                if (!_stricmp(tok, drop[i]) && ctx && *ctx) skip = 1;
        first = 0;
        if (skip) continue;
        k += snprintf(out + k, n - k > 0 ? n - k : 0, "%s%s", k ? " " : "", tok);
        if (k >= n - 1) break;
    }
    if (!out[0]) snprintf(out, n, "%s", raw);
    if (out[0] >= 'a' && out[0] <= 'z') out[0] -= 32;
}

/* A person's first name, found once per GUID in the character table (a run
 * of 0x28-byte records: id, GUID, first name, last name). */
static int dm_sim_name(uint32_t person, char *out, int n)
{
    static struct { uint32_t guid; char name[32]; } cache[16];
    static int nc;
    uint32_t d = GMEM32(person + 0x720), guid, a;
    int i;
    if (!valid_ptr(d) || !(guid = GMEM32(d + 0x1C)))
        return 0;
    for (i = 0; i < nc; i++)
        if (cache[i].guid == guid) {
            snprintf(out, n, "%s", cache[i].name);
            return out[0] != 0;
        }
    if (nc >= 16)
        return 0;
    cache[nc].guid = guid;
    cache[nc].name[0] = 0;
    for (a = 0x82000000u; a < 0x83800000u; a += 4)
        if (GMEM32(a) == guid && valid_ptr(GMEM32(a + 4)) && dm_printable(GMEM32(a + 4), cache[nc].name, 32) &&
            GMEM32(a + 0x28) != guid)
            break;
    snprintf(out, n, "%s", cache[nc].name);
    nc++;
    return out[0] != 0;
}

/* How many actions the game offers her on a selectable just now (its own
 * list: sub_00074510(object, &list, player), object = selectable +0x324). */
static int dm_actions(uint32_t sel)
{
    uint32_t esp = g_esp, vec, a[3], n;
    g_esp -= 16;
    vec = g_esp;
    GMEM32(vec) = 0;
    GMEM32(vec + 4) = 0;
    a[0] = GMEM32(sel + 0x324); a[1] = vec; a[2] = 0;
    guest_thiscall(0x00074510u, 0, 3, a);
    n = guest_thiscall(0x001C4770u, vec, 0, NULL);
    guest_thiscall(0x001C4A20u, vec, 0, NULL);
    g_esp = esp;
    return (int)n;
}

/* Is something under the cursor lit (the gold glow)? The game flags it on
 * its selectable (+0x328, bit 1 for the first player). */
static int dm_lit(void)
{
    uint32_t mgr = GMEM32(0x310C48u), x, node;
    int k = 0;
    if (!valid_ptr(mgr) || !valid_ptr(x = GMEM32(mgr + 4)))
        return 0;
    for (node = GMEM32(x + 4); valid_ptr(node) && k < 4000; node = GMEM32(node + 0x10), k++) {
        uint32_t sel = GMEM32(node + 0x1C);
        if (valid_ptr(sel) && (GMEM32(sel + 0x328) & 1))
            return 1;
    }
    return 0;
}

static void dm_pick_build(uint32_t sim)
{
    uint32_t mgr = GMEM32(0x310C48u), x, node, me = sim - 0x28u;
    float px = GMEMI(sim + 0x110) / 16.0f, py = GMEMI(sim + 0x114) / 16.0f;
    float dist[DM_PICK_N];
    int k = 0, i, j;
    s_pick_n = 0;
    if (!valid_ptr(mgr) || !valid_ptr(x = GMEM32(mgr + 4)))
        return;
    for (node = GMEM32(x + 4); valid_ptr(node) && k < 4000; node = GMEM32(node + 0x10), k++) {
        uint32_t sel = GMEM32(node + 0x1C), vt, g;
        float ox, oy, d;
        char raw[64], name[64];
        if (!valid_ptr(sel))
            continue;
        vt = GMEM32(sel);
        g = GMEM32(sel + 0x324);
        ox = GMEMF(sel + 0xA8);
        oy = GMEMF(sel + 0xAC);
        d = sqrtf((ox - px) * (ox - px) + (oy - py) * (oy - py));
        if (!(d < DM_PICK_REACH) || !valid_ptr(g))
            continue;
        if (vt == 0x002E4588u) {
            if (g == me || !dm_sim_name(g + 0x28u - 0x5F0u, name, sizeof name))
                continue;
        } else if (vt == 0x002E5870u || vt == 0x002E5958u || vt == 0x002E5B28u ||
                   (vt == 0x002E5A40u && GMEM32(g) == 0x002F52B0u)) {
            uint32_t def = GMEM32(vt == 0x002E5A40u ? g + 0x15Cu : g - 0x68u);
            if (!valid_ptr(def) || !dm_printable(GMEM32(def + 0x14), raw, sizeof raw))
                continue;
            if (!strncmp(raw, "portal", 6) || !strncmp(raw, "TemplateModule", 14) || !strncmp(raw, "phone_line", 10) ||
                !strncmp(raw, "event_", 6) || !strncmp(raw, "window", 6) || !strncmp(raw, "door", 4) ||
                !strncmp(raw, "wall", 4) || !strncmp(raw, "floor", 5) || !strncmp(raw, "fence", 5))
                continue;
            dm_pretty(raw, name, sizeof name);
        } else
            continue;
        if (dm_actions(sel) <= 0)
            continue;                        /* nothing she can do with it */
        /* One line per name (a bed or table is several tiles): the nearest. */
        for (i = 0; i < s_pick_n; i++)
            if (!strcmp(s_pick[i].name, name))
                break;
        if (i < s_pick_n) {
            if (d >= dist[i]) continue;
            for (j = i; j < s_pick_n - 1; j++) { s_pick[j] = s_pick[j + 1]; dist[j] = dist[j + 1]; }
            s_pick_n--;
        }
        for (i = s_pick_n; i > 0 && dist[i - 1] > d; i--)
            if (i < DM_PICK_N) { s_pick[i] = s_pick[i - 1]; dist[i] = dist[i - 1]; }
        if (i < DM_PICK_N) {
            s_pick[i].sel = sel;
            snprintf(s_pick[i].name, sizeof s_pick[i].name, "%s", name);
            dist[i] = d;
            if (s_pick_n < DM_PICK_N) s_pick_n++;
        }
    }
}


/* Opens the game's action menu for one selectable, as its A handler
 * (sub_0003A6E0) does for what the cursor is on: the controller's candidate
 * list (+0x164) cleared and given the one thing, the menu built from it by
 * the cursor UI (+0xE4, sub_00059400 -- which first clears whatever prompt
 * is up), then its sound and state, and the cursor marked "menu up"
 * (+0xA4 = 1). */
static void dm_pick_menu(uint32_t ctrl)
{
    uint32_t list = ctrl + 0x164, ui = GMEM32(ctrl + 0xE4), snd = GMEM32(0x310B3Cu), st = GMEM32(ctrl + 0x10);
    uint32_t a[2], ok;
    int i;
    if (!valid_ptr(ui) || !s_pick_n)
        return;
    guest_thiscall(0x00142B90u, list, 0, NULL);
    for (i = s_pick_n - 1; i >= 0; i--) {          /* each goes in front: nearest ends up first */
        a[0] = s_pick[i].sel;
        guest_thiscall(0x001429F0u, list, 1, a);
    }
    a[0] = list;
    ok = guest_thiscall(0x00059400u, ui, 1, a) & 0xFF;
    if (ok) {
        if (valid_ptr(snd)) {
            a[0] = 0x7D99927Fu;
            guest_thiscall(0x0006B050u, snd, 1, a);
        }
        if (valid_ptr(st) && valid_ptr(GMEM32(st))) {
            a[0] = ctrl; a[1] = 0x13;
            guest_thiscall(GMEM32(GMEM32(st) + 0x18), st, 2, a);
        }
        GMEM32(ctrl + 0xA4) = 1;
        s_game_menu = 1;
    }
    if (dm_log())
        fprintf(stderr, "[DM] nearby: %d things -> menu %s\n", s_pick_n, ok ? "up" : "refused");
}

/* Each frame in live mode (the cursor hook). */
static void dm_pick_tick(uint32_t ctrl, uint32_t sim)
{
    /* The menu the list opened stays the game's (A picks in it) until it
     * goes (the cursor's +0xA4 back to 0). */
    if (s_game_menu && GMEM32(ctrl + 0xA4) != 1)
        s_game_menu = 0;
    s_lit = dm_lit();
    if (InterlockedExchange(&s_pick_req, 0)) {
        dm_pick_build(sim);
        if (dm_log())
            fprintf(stderr, "[DM] nearby: %d things she can use\n", s_pick_n);
        if (s_pick_n)
            dm_pick_menu(ctrl);
    }
}

/* Input (port 0): A, with nothing lit in front of her and no menu up,
 * asks for the things nearby instead of reaching the game. */
static void dm_pick_input(XBOX_INPUT_STATE *st)
{
    static int pa, swallow_a;
    int a = st->Gamepad.bAnalogButtons[XBOX_BUTTON_A] > 30;
    if (dm_on() && !s_game_menu && !s_lit && a && !pa && !swallow_a && GetTickCount() - s_cursor_tick < 600) {
        InterlockedExchange(&s_pick_req, 1);
        swallow_a = 1;
    }
    pa = a;
    if (swallow_a) {
        if (a) st->Gamepad.bAnalogButtons[XBOX_BUTTON_A] = 0;
        else swallow_a = 0;
    }
}

/* ── Cancelling what she is doing ──────────────────────────────────────
 * The stick pushed while she is busy (an action queued or running) cancels
 * her actions the way the game's own cancel does (the action queue
 * manager, around 0x54393): her VM part (the Sim pointer) lists them --
 * vfunc 0x50 the count queued, 0x54(i) one, 0x58 the running one; each
 * one's +0x14 says whether it can be cancelled, +0x44 is its id -- and
 * sub_00050ED0(owner, id) sends the cancel, owner being the pointer at
 * Sim + [[Sim + 4] + 8] + 0xC. A cancelled Sim finishes the step she is
 * on (gets out of the bath, puts the plate down) and goes idle. Returns 1
 * while that is under way (nothing else to do meanwhile). */
static int dm_cancel_busy(uint32_t frame)
{
    static uint32_t sent_frame, held_since;
    uint32_t sim = s_dm_sim, vt, vbt, owner, n, i, ids[16], nid = 0, e, a[2];
    if (!valid_ptr(sim) || !valid_ptr(vt = GMEM32(sim)) || !valid_ptr(vbt = GMEM32(sim + 4)))
        return 0;
    if (sent_frame && frame - sent_frame < 240)
        return 1;                              /* sent: give her time to wind down */
    n = guest_thiscall(GMEM32(vt + 0x50), sim, 0, NULL);
    for (i = 0; i < n && i < 15; i++) {
        a[0] = i;
        e = guest_thiscall(GMEM32(vt + 0x54), sim, 1, a);
        if (valid_ptr(e) && GMEM32(e + 0x14))
            ids[nid++] = GMEM32(e + 0x44);
    }
    e = guest_thiscall(GMEM32(vt + 0x58), sim, 0, NULL);
    if (valid_ptr(e) && GMEM32(e + 0x14))
        ids[nid++] = GMEM32(e + 0x44);
    (void)held_since;
    if (!nid)
        return 0;                              /* nothing that can be cancelled: Go Here */
    owner = GMEM32(sim + GMEM32(vbt + 8) + 0xC);
    for (i = 0; i < nid; i++) {
        a[0] = owner; a[1] = ids[i];
        guest_thiscall(0x00050ED0u, 0, 2, a);
    }
    sent_frame = frame ? frame : 1;
    if (dm_log())
        fprintf(stderr, "[DM] stick while busy: cancelled %u action(s)\n", nid);
    return 1;
}

/* ── Follow camera ─────────────────────────────────────────────────────
 * The lot camera (the controller's +0xD8) eases toward what it is asked to
 * show: look-at point +0x394 (x, y, z), distance +0x3A0, yaw +0x3A4 (degrees;
 * the view runs at 45 + yaw degrees in the lot's x/y), pitch +0x3A8; the
 * eased values are at +0x3AC.. (and the eye at +0x374). With the mod on the
 * mod owns these: it keeps its own heading, zoom, distance, angle and aim,
 * set in the F6 menu, and writes them every frame -- it never reads back
 * what the game left there (some of the game's own camera moves re-derive
 * those fields from where the camera is; scaling them again each frame
 * collapsed the camera onto her and set it spinning). The right stick turns
 * the camera round her (left/right) and zooms (up/down); the game does not
 * see it. While she walks it swings round behind her. */
#define DM_ORBIT_RATE   150.0f   /* degrees a second, right stick fully over */
#define DM_ZOOM_RATE      0.7f   /* zoom factor change a second, right stick fully up or down */

static void dm_orbit_camera(uint32_t cam, float px, float py, float stick_fwd)
{
    static DWORD last;
    static float yaw, zoom = 1.0f, base_z, my_z = -99.0f, prev_x, prev_y;
    static int have_prev;
    static int owned;
    static uint32_t last_frame;
    XboxGfxConfig *g = xbox_GfxConfig();
    DWORD now = GetTickCount();
    float dt = last ? (float)(now - last) / 1000.0f : 0.0f, rx, ry, dist, pitch;
    (void)stick_fwd;
    last = now;
    if (dt > 0.1f) dt = 0.1f;
    /* Taking over (first frame, or back in live mode after a while): start
     * from where the game's camera is looking. */
    if (!owned || s_dm_frame - last_frame > 30) {
        float y = GMEMF(cam + 0x3BC), z = GMEMF(cam + 0x39C);
        yaw = (y > -100000.0f && y < 100000.0f) ? y : 0.0f;
        if (z != my_z)                     /* the game's own aim (its floor), not what the mod left */
            base_z = (z > -50.0f && z < 50.0f) ? z : 0.25f;
        owned = 1;
        have_prev = 0;
    }
    last_frame = s_dm_frame;
    rx = (float)s_rstick_x / 32767.0f;
    ry = (float)s_rstick_y / 32767.0f;
    if (fabsf(rx) > 0.25f && fabsf(rx) > fabsf(ry) * 0.7f) {
        float turn;
        rx = (rx > 0 ? rx - 0.25f : rx + 0.25f) / 0.75f;
        turn = rx * DM_ORBIT_RATE * dt;
        yaw += turn;
    }
    /* Following her: a camera on a leash. Her step since the last frame
     * pulls the camera's line of sight round by exactly the angle it makes
     * with that line at the leash's length -- straight ahead or back, not
     * at all; sideways, as much as she moved. It cannot run on by itself. */
    if (g->cam_follow && have_prev) {
        static const float leash[4] = { 0.0f, 3.0f, 1.6f, 0.8f };    /* x the distance */
        float ddx = px - prev_x, ddy = py - prev_y, th = (yaw + 45.0f) / 57.29578f, fwx = cosf(th), fwy = sinf(th);
        float r = (float)g->cam_dist / 10.0f * zoom * cosf((float)g->cam_angle / 57.29578f) * leash[g->cam_follow & 3];
        if (ddx * ddx + ddy * ddy < 0.25f && r > 0.3f)               /* a real step, not a jump */
            yaw += atan2f(fwx * ddy - fwy * ddx, r + fwx * ddx + fwy * ddy) * 57.29578f;
    }
    prev_x = px; prev_y = py; have_prev = 1;
    if (fabsf(ry) > 0.25f && fabsf(ry) >= fabsf(rx) * 0.7f) {
        ry = (ry > 0 ? ry - 0.25f : ry + 0.25f) / 0.75f;
        zoom *= 1.0f - ry * DM_ZOOM_RATE * dt;     /* up: in, down: out */
        if (zoom < 0.4f) zoom = 0.4f;
        if (zoom > 3.0f) zoom = 3.0f;
    }
    /* Kept in 0..360: the game eases its view toward this the short way
     * round only then (its own yaw is kept in 0..360; one a turn or more
     * outside sends the eased yaw round a whole turn every frame -- the
     * rapid spin after turning the camera the same way for long enough). */
    yaw = fmodf(yaw, 360.0f);
    if (yaw < 0.0f) yaw += 360.0f;
    if (!(yaw >= 0.0f && yaw < 360.0f)) yaw = 0.0f;
    dist = (float)g->cam_dist / 10.0f * zoom;
    pitch = (float)g->cam_angle + (zoom > 1.0f ? (zoom - 1.0f) * 12.0f : 0.0f);
    if (pitch > 80.0f) pitch = 80.0f;
    GMEMF(cam + 0x394) = px;
    GMEMF(cam + 0x398) = py;
    GMEMF(cam + 0x39C) = my_z = base_z + (float)g->cam_aim / 10.0f;
    GMEMF(cam + 0x3A0) = dist;
    GMEMF(cam + 0x3A4) = yaw;
    GMEMF(cam + 0x3A8) = pitch;
}

/* The hook: top of sub_00039000, g_ecx = the cursor controller. */
void sims_dm_cursor(void)
{
    static float face_x = 0.0f, face_y = 1.0f;
    static uint32_t last_issue;
    static int tries;
    static float issue_dx, issue_dy;
    static float block_dx, block_dy;
    uint32_t ctrl = g_ecx, sim, cam, player, frame;
    float sx, sy, mag, px, py, fx, fy, yaw, c, s, dx, dy, lot;
    DWORD now = GetTickCount();

    if (!dm_on() || !valid_ptr(ctrl)) {
        s_dm_active = 0;
        return;
    }
    player = GMEM32(ctrl + 0x3C);
    if (player > 1 || GMEM32(CURSOR_TABLE + player * 4) != ctrl)
        return;
    /* Live mode only: the cursor's own mode (+0xA4: 0 in live mode) and the
     * game mode at [0x310C54]+0x11C (0 in live mode). */
    /* +0xA4 is 1 and the mode 5 while an action prompt ("Go Here", "Sit")
     * shows under the cursor -- nearly always, with the cursor riding in
     * front of the Sim -- so those count as live mode too. */
    {
        uint32_t gm = GMEM32(0x310C54u), a4 = GMEM32(ctrl + 0xA4), mode;
        mode = valid_ptr(gm) ? GMEM32(gm + 0x11C) : 99u;
        if (dm_log()) {
            static uint32_t la = 99, lm = 99;
            if (a4 != la || mode != lm) {
                fprintf(stderr, "[DM] cursor mode %u game mode %u\n", a4, mode);
                la = a4; lm = mode;
            }
        }
        if (a4 > 1 || (mode != 0 && mode != 5)) {
            s_dm_active = 0;
            return;
        }
    }
    sim = GMEM32(SIM_TABLE + player * 4);
    cam = GMEM32(ctrl + 0xD8);
    if (!valid_ptr(sim) || !valid_ptr(cam))
        return;
    s_cursor_tick = now;
    s_dm_sim = sim;
    dm_pick_tick(ctrl, sim);
    frame = ++s_dm_frame;
    if (frame - s_dm_router_tick > 3)
        s_dm_walking = 0;
    if (getenv("SIMS_DM_TRACE") && frame % 60 == 0 && (s_dm_direct || s_dm_hold_anim || s_dm_end_anim))   /* debug */
        fprintf(stderr, "[DMS] direct %ld hold %ld end %ld active %ld walking %ld boxed %ld router age %u\n",
                (long)s_dm_direct, (long)s_dm_hold_anim, (long)s_dm_end_anim, (long)s_dm_active, (long)s_dm_walking,
                (long)s_dm_boxed, frame - s_dm_router_tick);
    /* A walk the mod began whose router has gone quiet (the game took her
     * over: an event, a dialog): let her idle animation go. */
    if ((s_dm_direct || s_dm_hold_anim) && frame - s_dm_router_tick > 15) {
        if (dm_log())
            fprintf(stderr, "[DM] direct walk dropped (router quiet %u frames)\n", frame - s_dm_router_tick);
        s_dm_direct = 0;
        s_dm_hold_anim = 0;
        s_dm_end_anim = 1;
    }
    if (dm_log() && s_stick_x * s_stick_x + s_stick_y * s_stick_y > 100000000) {
        static uint32_t t;
        if (frame - t > 45) {
            t = frame;
            fprintf(stderr, "[DM] stick: walking %ld direct %ld hold %ld idle %ld (age %u) blocked %ld failed %ld router age %u\n",
                    (long)s_dm_walking, (long)s_dm_direct, (long)s_dm_hold_anim, (long)s_dm_anim_idle,
                    frame - s_dm_anim_frame, (long)s_dm_blocked, (long)s_dm_direct_failed, frame - s_dm_router_tick);
        }
    }
    /* Boxed in (the router hook found walls ahead and to both sides): no
     * Go Here in that direction until the stick turns or is let go. */
    if (s_dm_boxed && s_dm_active) {
        s_dm_boxed = 0;
        s_dm_blocked = 1;
        block_dx = s_dm_dx; block_dy = s_dm_dy;
    }

    px = (float)GMEMI(sim + 0x110) / 16.0f;
    py = (float)GMEMI(sim + 0x114) / 16.0f;
    if (!(px > 0.0f && py > 0.0f && px < 256.0f && py < 256.0f))
        return;
    /* Doors she has left behind close even when she is not walking. */
    if ((!s_dm_active || !s_dm_hold_anim) && frame % 5 == 0)
        dm_doors(px, py, 0.0f, 0.0f, 0);
    lot = GMEMF(0x3100E0);
    if (!(lot >= 8.0f && lot <= 256.0f))
        lot = 64.0f;

    /* The stick, turned by the camera's heading as sub_00039000 turns it
     * for the cursor: up on the stick is away from the camera. */
    sx = (float)s_stick_x / 32767.0f;
    sy = (float)s_stick_y / 32767.0f;
    mag = sqrtf(sx * sx + sy * sy);
    /* The stick is read against the view on screen: the camera's eased yaw
     * (the view runs at 45 + yaw degrees in the lot's x/y). */
    {
        float th = (GMEMF(cam + 0x3BC) + 45.0f) / 57.29578f;
        if (!(th > -1000.0f && th < 1000.0f)) th = 0.7854f;
        fx = cosf(th);
        fy = sinf(th);
    }
    yaw = atan2f(fx, fy);
    c = cosf(yaw); s = sinf(yaw);

    if (s_stick_scale != 1.0f) {     /* testing aid: SIMS_DM_STICK_SCALE */
        sx *= s_stick_scale; sy *= s_stick_scale; mag *= s_stick_scale;
    }
    if (mag > DM_DEAD) {
        float l;
        if (mag > 1.0f) mag = 1.0f;
        if (mag >= DM_RUN_ON) s_dm_run = 1;
        else if (mag < DM_RUN_OFF) s_dm_run = 0;
        l = (mag - DM_DEAD) / (DM_FULLWALK - DM_DEAD);
        s_dm_walkfrac = l < 0.35f ? 0.35f : (l > 1.0f ? 1.0f : l);
        dx = sx * c + sy * s;
        dy = -sx * s + sy * c;
        l = sqrtf(dx * dx + dy * dy);
        if (l > 0.0001f) { dx /= l; dy /= l; }
        face_x = dx; face_y = dy;
        s_dm_dx = dx; s_dm_dy = dy; s_dm_mag = mag;
        s_dm_active = 1;
        if (s_dm_blocked && dx * block_dx + dy * block_dy < 0.82f)
            s_dm_blocked = 0;
        /* Standing still: start a walk with a Go Here; once it is under
         * way the router hook drives it, so the target only has to be a
         * tile she can reach: two tiles along the stick's main axis (a
         * diagonal through a doorway is dropped without a word), and if she
         * has not set off half a second later, two along the other axis. The game queues them behind whatever she is doing, so then
         * the mod waits five seconds, or for the stick to turn. */
        if (s_dm_walking || s_dm_direct) {
            tries = 0;
        } else if (s_dm_anim_idle && frame - s_dm_anim_frame < 600 && !s_dm_direct_failed) {
            /* Idle: the router hook walks her off with no Go Here. */
        } else if (dm_cancel_busy(frame)) {
            /* Busy: her actions are being cancelled, as the game's own
             * cancel does; once she is back in her idle loop the stick
             * walks her. */
        } else if (!s_dm_blocked) {
            if (dx * issue_dx + dy * issue_dy < 0.8f)
                tries = 0;                     /* a new direction: start over */
            if (tries >= 2 && frame - last_issue > 300)
                tries = 0;
            if (tries < 2 && (tries == 0 || frame - last_issue > 30)) {
                float cx, cy, dist = 2.0f, tx, ty;
                int major_x = fabsf(dx) >= fabsf(dy);
                int use_x = tries == 0 ? major_x : !major_x;
                float c = use_x ? dx : dy;
                if (tries == 1 && fabsf(c) < 0.25f) {
                    tries = 2;                 /* straight along an axis: nothing else to try */
                    goto issued;
                }
                cx = use_x ? (c > 0 ? 1.0f : -1.0f) : 0.0f;
                cy = use_x ? 0.0f : (c > 0 ? 1.0f : -1.0f);
                tx = floorf(px + cx * dist) + 0.5f;
                ty = floorf(py + cy * dist) + 0.5f;
                if (tx < 0.5f) tx = 0.5f;
                if (ty < 0.5f) ty = 0.5f;
                if (tx > lot - 0.5f) tx = lot - 0.5f;
                if (ty > lot - 0.5f) ty = lot - 0.5f;
                go_here(ctrl, tx, ty);
                {
                    LONG j = s_dm_issued_n < DM_ISSUED ? s_dm_issued_n : DM_ISSUED - 1;
                    s_dm_issued_x[j] = (int32_t)(tx * 16.0f);
                    s_dm_issued_y[j] = (int32_t)(ty * 16.0f);
                    s_dm_issued_n = j + 1;
                }
                s_dm_issue_frame = frame;
                if (tries == 0) { issue_dx = dx; issue_dy = dy; }
                tries++;
                last_issue = frame;
            issued: ;
            }
        }
    } else {
        s_dm_active = 0;
        s_dm_run = 0;
        s_dm_blocked = 0;
        s_dm_direct_failed = 0;
        tries = 0;
    }

    /* The cursor rides just in front of the Sim: A acts on what she faces. */
    GMEMF(ctrl + 0xBC) = px + face_x * 1.0f;
    GMEMF(ctrl + 0xC0) = py + face_y * 1.0f;
    dm_orbit_camera(cam, px, py, sy);
}

/* Top of the Sim's "can she stand here" (sub_001ACF00, thiscall: point,
 * level, 0, 0; vfunc 0xC0). When she stands partly inside something (got up
 * from a chair, brushed a bookshelf) every point by her fails, so the
 * router would refuse every step and she could never leave. Then, while the
 * stick steers her, the router's checks on her steps pass, and the mod's
 * look-ahead (dm_way_open, the way out only) decides alone. Everywhere else
 * the router's own check stands: it also stops her at walls a test of
 * single points can miss. */
int sims_dm_stand_ok(void)
{
    uint32_t ret = GMEM32(g_esp), sim = s_dm_sim, vbt;
    if (!dm_on() || !s_dm_active || !s_dm_hold_anim || !s_dm_inside || !valid_ptr(sim))
        return 0;
    if (ret != 0x000B047Bu && ret != 0x000B0E6Eu && ret != 0x000B1F90u && ret != 0x000B32EFu &&
        ret != 0x000B3ED9u && ret != 0x000B5800u)
        return 0;
    vbt = GMEM32(sim + 4);
    return valid_ptr(vbt) && g_ecx == sim + 4 + GMEM32(vbt + 8);
}
