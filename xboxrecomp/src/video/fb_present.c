/**
 * Show the guest framebuffer in a window.
 *
 * The title renders into its own framebuffer in guest RAM and tells the kernel
 * where it is through AvSetDisplayMode; on hardware the CRTC scans that memory
 * out. Nothing here scans anything out, so however much of the GPU is
 * implemented, none of it is observable. This is the other half: a window that
 * reads that memory and puts it on screen.
 *
 * Deliberately plain GDI rather than the D3D8 layer. The point is to display
 * whatever the guest actually wrote, so the fewer stages between guest memory
 * and the screen the better -- and it must keep working while the D3D8 layer
 * is busy with something else, such as the FMV player's own window.
 * RECOMP_D3D_PRESENT=1 shows the same frames through a D3D11 swap chain of
 * the window's own instead (gpu_present.c): scaled on the GPU, vsync-paced.
 *
 * The window opens at RECOMP_WINDOW_SCALE times the frame (default 2, made
 * smaller until it fits the desktop) and can be resized; the frame is
 * letterboxed to its 4:3 shape. F11 or Alt+Enter toggles borderless
 * fullscreen, RECOMP_FULLSCREEN=1 starts in it. Closing the window ends the
 * process, as the SDL window does.
 *
 * Off unless RECOMP_FB_WINDOW is set.
 */
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Which keys are down, for the pad stand-in in src/input: this window
 * records them in the shared key table (src/input/keyboard.c).
 *
 * GetAsyncKeyState looked like the cheaper way to ask and does not work
 * here: it reads a state Wine keeps for the X server, and a guest process
 * drawing through GDI never sees it change. The window that has the focus
 * is the thing that receives the keys, so that is what has to remember
 * them. */
#include "keyboard.h"   /* src/input */
#include "gpu_present.h"
#include "gfx_config.h"
#include "recomp_exit_hook.h"   /* src/platform */
#include <stdbool.h>
#include "pvideo_scanout.h"
#include "../nv2a/nv2a_mmio_hook.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);

static volatile LONG s_fb_running;
static uint32_t      s_fb_va, s_fb_pitch, s_fb_width = 640, s_fb_height = 480;
static uint32_t     *s_rgb;           /* converted 32-bit copy for GDI */

/* A finished frame, taken at the flip and shown until the next one.
 *
 * The window used to convert straight out of guest memory every 16 ms. Even
 * pointed at the buffer the title had just finished, that races the executor
 * drawing the next frame into the other one and, whenever the two swap, puts
 * a half-drawn image on the screen -- which is the flicker. Copying the
 * finished frame once per flip means the window never reads memory the
 * rasteriser is writing, so what it shows cannot be half of anything.
 *
 * The lock also protects the window's copy: two flips can otherwise reuse
 * the buffer the window is still reading. */
static uint32_t     *s_present[2];
static LONG          s_present_idx = -1;   /* -1 until the first flip */
static volatile LONG s_present_seq;        /* flips published */
static SRWLOCK       s_present_lock = SRWLOCK_INIT;

/* The last PVIDEO frame shown. A frame stays on screen until the next one or
 * STOP, but once its bank is consumed the title may reuse the memory, so the
 * pixels are kept here rather than read again. */
static uint8_t      *s_pvideo_source;
static size_t        s_pvideo_bytes;
static unsigned      s_pvideo_bank;
static uint32_t      s_pvideo_regs[0x1000 / 4];

/* Draw the video overlay over s_rgb: a newly submitted bank if there is one
 * (copied, drawn, then handed back to the title), else the last frame. */
static int s_pvideo_active;          /* the overlay plane drew this frame */

static void fb_overlay(void)
{
    uint32_t regs[0x1000 / 4];
    unsigned bank;
    bool presented = false;

    s_pvideo_active = 0;
    if (!s_rgb || !nv2a_hook_pvideo_snapshot(regs)) return;
    if ((regs[NV_PVIDEO_STOP / 4] & 1u) ||
        (s_pvideo_source &&
         regs[(NV_PVIDEO_SIZE_IN + s_pvideo_bank * 4) / 4] == UINT32_MAX)) {
        free(s_pvideo_source);
        s_pvideo_source = NULL;
        if (regs[NV_PVIDEO_STOP / 4] & 1u) return;
    }
    for (bank = 0; bank < 2; ++bank) {
        uint64_t physical = (uint64_t)regs[(NV_PVIDEO_BASE + bank * 4) / 4] +
                            regs[(NV_PVIDEO_OFFSET + bank * 4) / 4];
        unsigned pitch = regs[(NV_PVIDEO_FORMAT + bank * 4) / 4] & NV_PVIDEO_FORMAT_PITCH;
        unsigned height = (regs[(NV_PVIDEO_SIZE_IN + bank * 4) / 4] >> 16) & 0x7ff;
        uint64_t bytes = (uint64_t)pitch * height;
        uintptr_t va;
        MEMORY_BASIC_INFORMATION mapping;
        uint8_t *snapshot;

        if (!(regs[NV_PVIDEO_BUFFER / 4] & (1u << (bank * 4u)))) continue;
        if (!bytes || physical + bytes > 0x04000000u) continue;
        va = (uintptr_t)(0x80000000u + (uint32_t)physical) +
             (uintptr_t)xbox_GetMemoryOffset();
        if (!VirtualQuery((const void *)va, &mapping, sizeof(mapping)) ||
            mapping.State != MEM_COMMIT ||
            (mapping.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            bytes > mapping.RegionSize - (size_t)(va - (uintptr_t)mapping.BaseAddress))
            continue;
        snapshot = (uint8_t *)malloc((size_t)bytes);
        if (!snapshot) continue;
        memcpy(snapshot, (const void *)va, (size_t)bytes);
        if (!pvideo_scanout(regs, snapshot, (size_t)bytes, s_rgb,
                            s_fb_width, s_fb_height, bank)) {
            free(snapshot);
            continue;
        }
        free(s_pvideo_source);
        s_pvideo_source = snapshot;
        s_pvideo_bytes = (size_t)bytes;
        s_pvideo_bank = bank;
        memcpy(s_pvideo_regs, regs, sizeof(s_pvideo_regs));
        presented = true;
        nv2a_hook_pvideo_consume(bank);
    }
    if (!presented && s_pvideo_source)
        pvideo_scanout(s_pvideo_regs, s_pvideo_source, s_pvideo_bytes, s_rgb,
                       s_fb_width, s_fb_height, s_pvideo_bank);
    s_pvideo_active = presented || s_pvideo_source != NULL;
}

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    /* RECOMP_FB_VA pins the window to one guest address instead of following
     * whichever surface is being drawn into. A black window cannot distinguish
     * "the read path is broken" from "the title rendered black", and pointing
     * it at memory known to have content settles that. */
    const char *pin = getenv("RECOMP_FB_VA");

    s_fb_va = pin ? (uint32_t)strtoul(pin, NULL, 0) : fb_va;
    if (pitch)
        s_fb_pitch = pitch;
}

/* Vertical anti-aliasing factor of the surface being shown: 2 for 2x2, where
 * the surface has twice the rows as well as twice the columns, and 1 for
 * everything else (2x1 included). Only the pushbuffer executor knows it. */
static uint32_t s_fb_aa_sy = 1;

void xbox_FramebufferWindowSetAA(uint32_t sy)
{
    s_fb_aa_sy = sy ? sy : 1;
}

/* Called by the pushbuffer executor when the title flips. */
void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch)
{
    const uint8_t *src;
    LONG next;
    uint32_t bpp, x, y;

    if (!s_fb_running || !fb_va || !pitch)
        return;
    if (getenv("RECOMP_FB_VA"))
        return;                       /* pinned: leave the old path alone */
    AcquireSRWLockExclusive(&s_present_lock);
    next = (s_present_idx == 0) ? 1 : 0;
    if (!s_present[next]) {
        s_present[next] = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height,
                                             4);
        if (!s_present[next]) {
            ReleaseSRWLockExclusive(&s_present_lock);
            fprintf(stderr, "  [FBWIN] cannot allocate a presentation frame\n");
            return;
        }
    }
    bpp = pitch / s_fb_width;
    src = (const uint8_t *)((uintptr_t)fb_va + xbox_GetMemoryOffset());
    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * pitch * (bpp == 8 ? s_fb_aa_sy : 1);
        uint32_t *dst = s_present[next] + (size_t)y * s_fb_width;

        if (bpp == 4) {
            memcpy(dst, row, (size_t)s_fb_width * 4);
        } else if (bpp == 8) {
            /* 2x-wide anti-aliased 32-bit surface: every other pixel, as in
             * fb_convert. */
            const uint32_t *p = (const uint32_t *)row;
            for (x = 0; x < s_fb_width; x++)
                dst[x] = p[x * 2];
        } else if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (x = 0; x < s_fb_width; x++) {
                uint16_t v = p[x];
                uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
                dst[x] = (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)s_fb_width * 4);
        }
    }
    s_present_idx = next;
    s_present_seq++;
    ReleaseSRWLockExclusive(&s_present_lock);
}

static int fb_copy_presented_frame(uint32_t *dst)
{
    int copied = 0;

    AcquireSRWLockShared(&s_present_lock);
    if (s_present_idx >= 0) {
        memcpy(dst, s_present[s_present_idx],
               (size_t)s_fb_width * s_fb_height * 4);
        copied = 1;
    }
    ReleaseSRWLockShared(&s_present_lock);
    return copied;
}

/* ---- graphics settings, high-resolution frames, the F6 menu ---------------- */

static XboxGfxConfig s_gfx;
static int s_gfx_init;

static int env_int(const char *name, int def, int lo, int hi)
{
    const char *e = getenv(name);
    int v = (e && *e) ? atoi(e) : def;
    return v < lo ? lo : (v > hi ? hi : v);
}

XboxGfxConfig *xbox_GfxConfig(void)
{
    if (!s_gfx_init) {
        s_gfx_init = 1;
        s_gfx.scale = env_int("RECOMP_GPU_SCALE", 1, 1, 4);
        s_gfx.show_fps = env_int("RECOMP_SHOW_FPS", 0, 0, 1);
        s_gfx.tex_dump = env_int("RECOMP_TEX_DUMP", 0, 0, 1);
        s_gfx.tex_replace = env_int("RECOMP_TEX_REPLACE", 0, 0, 1);
        s_gfx.fps60 = env_int("RECOMP_FPS60", 0, 0, 1);
        s_gfx.direct_move = env_int("SIMS_DIRECT_MOVE", 0, 0, 1);
        s_gfx.cam_dist = env_int("SIMS_CAM_DIST", 55, 15, 200);
        s_gfx.cam_angle = env_int("SIMS_CAM_ANGLE", 35, 5, 80);
        s_gfx.cam_aim = env_int("SIMS_CAM_AIM", 6, 0, 20);
        s_gfx.cam_follow = env_int("SIMS_CAM_FOLLOW", 2, 0, 3);
    }
    return &s_gfx;
}

/* Rewrite (or add) NAME=value in settings.ini beside the exe, keeping every
 * other line as the player left it. */
static void settings_save(const char *name, int value)
{
    char path[MAX_PATH], *slash, line[600], out[65536];
    size_t n = 0, nl = strlen(name);
    int done = 0;
    FILE *f;

    GetModuleFileNameA(NULL, path, sizeof path);
    slash = strrchr(path, '\\');
    if (!slash) return;
    strcpy(slash + 1, "settings.ini");
    out[0] = 0;
    if ((f = fopen(path, "r")) != NULL) {
        while (fgets(line, sizeof line, f)) {
            const char *k = line;
            int ours;
            while (*k == ' ' || *k == '\t' || *k == ';') k++;
            ours = !strncmp(k, name, nl) && (k[nl] == '=' || k[nl] == ' ');
            if (ours) {
                if (done) continue;
                snprintf(line, sizeof line, "%s=%d\n", name, value);
                done = 1;
            }
            if (n + strlen(line) + 1 < sizeof out) { strcpy(out + n, line); n += strlen(line); }
        }
        fclose(f);
    }
    if (!done) {
        snprintf(line, sizeof line, "%s%s=%d\n", (n && out[n - 1] != '\n') ? "\n" : "", name, value);
        if (n + strlen(line) + 1 < sizeof out) { strcpy(out + n, line); n += strlen(line); }
    }
    if ((f = fopen(path, "w")) != NULL) {
        fwrite(out, 1, n, f);
        fclose(f);
    }
}

/* The last frame rendered above 640x480, as the back end handed it over. */
static uint32_t *s_hires, *s_hires_show;
static uint32_t s_hires_w, s_hires_h, s_hires_cap, s_hires_show_cap;
static volatile LONG s_hires_seq;

void xbox_FramebufferWindowPresentHiRes(const uint32_t *argb, uint32_t width,
                                        uint32_t height, uint32_t stride_px)
{
    uint32_t y;
    size_t need = (size_t)width * height;

    if (!s_fb_running || !width || !height)
        return;
    AcquireSRWLockExclusive(&s_present_lock);
    if (need > s_hires_cap) {
        free(s_hires);
        s_hires = (uint32_t *)malloc(need * 4);
        s_hires_cap = s_hires ? (uint32_t)need : 0;
    }
    if (s_hires) {
        for (y = 0; y < height; y++)
            memcpy(s_hires + (size_t)y * width, argb + (size_t)y * stride_px, (size_t)width * 4);
        s_hires_w = width;
        s_hires_h = height;
        InterlockedIncrement(&s_hires_seq);
    }
    ReleaseSRWLockExclusive(&s_present_lock);
}

/* The window shows the high-resolution frame while scaling is on, a frame
 * has arrived, and the video overlay is not in use (it is composited into
 * the 640x480 copy). Returns the pixels and size to show, or NULL. */
static const uint32_t *hires_frame(uint32_t *w, uint32_t *h)
{
    XboxGfxConfig *g = xbox_GfxConfig();
    static LONG seen;
    if (g->scale_applied <= 1 || s_pvideo_active || !s_hires_seq)
        return NULL;
    AcquireSRWLockShared(&s_present_lock);
    if (seen != s_hires_seq || !s_hires_show) {
        size_t need = (size_t)s_hires_w * s_hires_h;
        if (need > s_hires_show_cap) {
            free(s_hires_show);
            s_hires_show = (uint32_t *)malloc(need * 4);
            s_hires_show_cap = s_hires_show ? (uint32_t)need : 0;
        }
        if (s_hires_show)
            memcpy(s_hires_show, s_hires, need * 4);
        seen = s_hires_seq;
    }
    *w = s_hires_w; *h = s_hires_h;
    ReleaseSRWLockShared(&s_present_lock);
    return s_hires_show;
}

/* The menu. */
enum { MENU_SCALE, MENU_FPS60, MENU_DM, MENU_CAMDIST, MENU_CAMANGLE, MENU_CAMAIM, MENU_CAMFOLLOW, MENU_FPS,
       MENU_DUMP, MENU_REPLACE, MENU_RELOAD, MENU_CLOSE, MENU_N };
static int s_menu_open, s_menu_sel;
static double s_fps_now;

static void menu_change(int item, int dir)
{
    XboxGfxConfig *g = xbox_GfxConfig();
    switch (item) {
    case MENU_SCALE:
        g->scale = ((g->scale - 1 + dir + 4) % 4) + 1;
        settings_save("RECOMP_GPU_SCALE", g->scale);
        break;
    case MENU_FPS60:
        g->fps60 = !g->fps60;
        settings_save("RECOMP_FPS60", g->fps60);
        break;
    case MENU_DM:
        g->direct_move = !g->direct_move;
        settings_save("SIMS_DIRECT_MOVE", g->direct_move);
        break;
    case MENU_CAMDIST:
        g->cam_dist += dir * 5;
        if (g->cam_dist < 15) g->cam_dist = 15;
        if (g->cam_dist > 200) g->cam_dist = 200;
        settings_save("SIMS_CAM_DIST", g->cam_dist);
        break;
    case MENU_CAMANGLE:
        g->cam_angle += dir * 5;
        if (g->cam_angle < 5) g->cam_angle = 5;
        if (g->cam_angle > 80) g->cam_angle = 80;
        settings_save("SIMS_CAM_ANGLE", g->cam_angle);
        break;
    case MENU_CAMAIM:
        g->cam_aim += dir;
        if (g->cam_aim < 0) g->cam_aim = 0;
        if (g->cam_aim > 20) g->cam_aim = 20;
        settings_save("SIMS_CAM_AIM", g->cam_aim);
        break;
    case MENU_CAMFOLLOW:
        g->cam_follow = (g->cam_follow + dir + 4) % 4;
        settings_save("SIMS_CAM_FOLLOW", g->cam_follow);
        break;
    case MENU_FPS:
        g->show_fps = !g->show_fps;
        settings_save("RECOMP_SHOW_FPS", g->show_fps);
        break;
    case MENU_DUMP:
        g->tex_dump = !g->tex_dump;
        if (g->tex_dump)
            g->reload_seq++;             /* what is already loaded is saved too */
        settings_save("RECOMP_TEX_DUMP", g->tex_dump);
        break;
    case MENU_REPLACE:
        g->tex_replace = !g->tex_replace;
        g->reload_seq++;                 /* textures already loaded change too */
        settings_save("RECOMP_TEX_REPLACE", g->tex_replace);
        break;
    case MENU_RELOAD:
        if (dir) g->reload_seq++;
        break;
    case MENU_CLOSE:
        if (dir) s_menu_open = 0;
        break;
    }
}

/* Keys for the menu; returns 1 if the key is the menu's (it does not reach
 * the game). F6 opens and closes it. */
static int menu_key(UINT m, WPARAM vk)
{
    int down = (m == WM_KEYDOWN || m == WM_SYSKEYDOWN);
    if (vk == VK_F6) {
        if (down) s_menu_open = !s_menu_open;
        return 1;
    }
    if (!s_menu_open)
        return 0;
    if (vk != VK_UP && vk != VK_DOWN && vk != VK_LEFT && vk != VK_RIGHT
        && vk != VK_RETURN && vk != VK_ESCAPE && vk != VK_SPACE)
        return 0;
    if (!down)
        return 1;
    switch (vk) {
    case VK_UP:     s_menu_sel = (s_menu_sel + MENU_N - 1) % MENU_N; break;
    case VK_DOWN:   s_menu_sel = (s_menu_sel + 1) % MENU_N; break;
    case VK_LEFT:   menu_change(s_menu_sel, -1); break;
    case VK_RIGHT:
    case VK_RETURN:
    case VK_SPACE:  menu_change(s_menu_sel, +1); break;
    case VK_ESCAPE: s_menu_open = 0; break;
    }
    return 1;
}

/* Set while the title has shown no new frame for a while (a save, a quit,
 * a load that does not draw): a note says it is still working. */
static int s_busy_note;

static SRWLOCK s_pick_lock = SRWLOCK_INIT;
static char s_pick_title[64], s_pick_hint[96], s_pick_items[XBOX_PICK_MAX][64];
static int s_pick_n, s_pick_sel = -1;
static volatile LONG s_pick_seq;

void xbox_PickListShow(const char *title, const char *const *items, int n, int sel, const char *hint)
{
    int i;
    if (n > XBOX_PICK_MAX) n = XBOX_PICK_MAX;
    AcquireSRWLockExclusive(&s_pick_lock);
    snprintf(s_pick_title, sizeof s_pick_title, "%s", title ? title : "");
    snprintf(s_pick_hint, sizeof s_pick_hint, "%s", hint ? hint : "");
    for (i = 0; i < n; i++)
        snprintf(s_pick_items[i], sizeof s_pick_items[i], "%s", items[i]);
    s_pick_n = n;
    s_pick_sel = sel;
    ReleaseSRWLockExclusive(&s_pick_lock);
    InterlockedIncrement(&s_pick_seq);
}

void xbox_PickListHide(void)
{
    AcquireSRWLockExclusive(&s_pick_lock);
    s_pick_sel = -1;
    ReleaseSRWLockExclusive(&s_pick_lock);
    InterlockedIncrement(&s_pick_seq);
}

static void pick_draw(HDC dc, int cw, int ch, int fh)
{
    int i, lh = fh * 3 / 2, pw, ph, x0, y0;
    HBRUSH bg, hl;
    HPEN pen, op;
    HBRUSH ob;
    (void)ch;
    AcquireSRWLockShared(&s_pick_lock);
    if (s_pick_sel < 0 || s_pick_n <= 0) {
        ReleaseSRWLockShared(&s_pick_lock);
        return;
    }
    pw = fh * 18;
    ph = lh * (s_pick_n + 1) + fh * 2 + (s_pick_hint[0] ? lh : 0);
    x0 = cw / 2 - pw / 2;
    y0 = fh * 2;
    bg = CreateSolidBrush(RGB(16, 22, 48)); hl = CreateSolidBrush(RGB(48, 70, 150));
    pen = CreatePen(PS_SOLID, 2, RGB(120, 150, 255));
    op = (HPEN)SelectObject(dc, pen);
    ob = (HBRUSH)SelectObject(dc, bg);
    RoundRect(dc, x0, y0, x0 + pw, y0 + ph, fh, fh);
    {
        RECT r = { x0 + fh, y0 + lh / 3, x0 + pw - fh, y0 + lh * 4 / 3 };
        SetTextColor(dc, RGB(255, 255, 255));
        DrawTextA(dc, s_pick_title, -1, &r, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    for (i = 0; i < s_pick_n; i++) {
        RECT row = { x0 + fh / 2, y0 + lh * (i + 1) + fh / 2, x0 + pw - fh / 2, 0 };
        row.bottom = row.top + lh;
        if (i == s_pick_sel)
            FillRect(dc, &row, hl);
        row.left += fh / 2;
        SetTextColor(dc, i == s_pick_sel ? RGB(255, 255, 255) : RGB(210, 218, 245));
        DrawTextA(dc, s_pick_items[i], -1, &row, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    if (s_pick_hint[0]) {
        RECT r = { x0 + fh, y0 + lh * (s_pick_n + 1) + fh, x0 + pw - fh, y0 + ph - fh / 4 };
        SetTextColor(dc, RGB(160, 170, 210));
        DrawTextA(dc, s_pick_hint, -1, &r, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    ReleaseSRWLockShared(&s_pick_lock);
    SelectObject(dc, op); SelectObject(dc, ob);
    DeleteObject(pen); DeleteObject(bg); DeleteObject(hl);
}

static void overlay_draw(HDC dc, int cw, int ch)
{
    XboxGfxConfig *g = xbox_GfxConfig();
    int fh = ch / 28 < 14 ? 14 : ch / 28;
    HFONT font = CreateFontA(-fh, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                             CLEARTYPE_QUALITY, FF_SWISS, "Segoe UI");
    HFONT old = (HFONT)SelectObject(dc, font);
    char t[256];

    SetBkMode(dc, TRANSPARENT);
    if (s_busy_note) {
        static const char *dots[4] = { "", ".", "..", "..." };
        RECT r = { cw / 2 - fh * 6, ch - fh * 3, cw / 2 + fh * 6, ch - fh * 3 / 2 };
        HBRUSH bb = CreateSolidBrush(RGB(10, 12, 24));
        FillRect(dc, &r, bb);
        DeleteObject(bb);
        snprintf(t, sizeof t, "Working, please wait%s", dots[(GetTickCount() / 400) & 3]);
        SetTextColor(dc, RGB(230, 235, 255));
        DrawTextA(dc, t, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    if (g->show_fps || s_menu_open) {
        RECT r;
        snprintf(t, sizeof t, "%.1f FPS", s_fps_now);
        r.left = cw - fh * 6; r.top = fh / 3; r.right = cw - fh / 2; r.bottom = r.top + fh * 3 / 2;
        {
            RECT b = r;
            HBRUSH bb = CreateSolidBrush(RGB(10, 12, 24));
            b.left = r.right - fh * 9 / 2; b.right = r.right + fh / 4;
            FillRect(dc, &b, bb);
            DeleteObject(bb);
        }
        SetTextColor(dc, RGB(255, 255, 120));
        DrawTextA(dc, t, -1, &r, DT_RIGHT | DT_SINGLELINE);
    }
    if (s_menu_open) {
        static const char *names[MENU_N] = {
            "Internal resolution", "Frame rate", "Left stick", "  Camera distance", "  Camera angle",
            "  Camera aim height", "  Camera follows her", "Show FPS", "Dump textures", "Replace textures",
            "Reload replacement textures", "Close menu"
        };
        int pw = fh * 26, lh = fh * 3 / 2, ph = lh * (MENU_N + 2) + fh * 4, x0 = fh, y0 = fh, i;
        RECT box = { x0, y0, x0 + pw, y0 + ph };
        HBRUSH bg = CreateSolidBrush(RGB(16, 22, 48)), hl = CreateSolidBrush(RGB(48, 70, 150));
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(120, 150, 255));
        HPEN op = (HPEN)SelectObject(dc, pen);
        HBRUSH ob = (HBRUSH)SelectObject(dc, bg);
        Rectangle(dc, box.left, box.top, box.right, box.bottom);
        SetTextColor(dc, RGB(255, 255, 255));
        {
            RECT r = { x0 + fh, y0 + lh / 3, x0 + pw - fh, y0 + lh * 4 / 3 };
            DrawTextA(dc, "Graphics", -1, &r, DT_LEFT | DT_SINGLELINE);
        }
        for (i = 0; i < MENU_N; i++) {
            RECT row = { x0 + fh / 2, y0 + lh * (i + 1) + lh / 2, x0 + pw - fh / 2, 0 };
            RECT rl, rv;
            row.bottom = row.top + lh;
            if (i == s_menu_sel)
                FillRect(dc, &row, hl);
            rl = row; rl.left += fh / 2;
            rv = row; rv.right -= fh / 2;
            SetTextColor(dc, RGB(230, 235, 255));
            DrawTextA(dc, names[i], -1, &rl, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            t[0] = 0;
            switch (i) {
            case MENU_SCALE:
                if (!g->gpu_active) snprintf(t, sizeof t, "1x (software renderer)");
                else snprintf(t, sizeof t, "< %dx  %dx%d >", g->scale, 640 * g->scale, 480 * g->scale);
                break;
            case MENU_FPS60:   snprintf(t, sizeof t, "%s", g->fps60 ? "60 (experimental)" : "30 (original)"); break;
            case MENU_DM:      snprintf(t, sizeof t, "%s", g->direct_move ? "Moves your Sim" : "Moves the cursor"); break;
            case MENU_CAMDIST: snprintf(t, sizeof t, "< %d.%d tiles >", g->cam_dist / 10, g->cam_dist % 10); break;
            case MENU_CAMANGLE: snprintf(t, sizeof t, "< %d degrees down >", g->cam_angle); break;
            case MENU_CAMAIM:  snprintf(t, sizeof t, "< %d.%d >", g->cam_aim / 10, g->cam_aim % 10); break;
            case MENU_CAMFOLLOW: {
                static const char *fl[4] = { "Off", "Slowly", "Normal", "Quickly" };
                snprintf(t, sizeof t, "< %s >", fl[g->cam_follow & 3]);
                break;
            }
            case MENU_FPS:     snprintf(t, sizeof t, "%s", g->show_fps ? "On" : "Off"); break;
            case MENU_DUMP:    snprintf(t, sizeof t, "%s  (%d saved)", g->tex_dump ? "On" : "Off", g->dumped); break;
            case MENU_REPLACE: snprintf(t, sizeof t, "%s  (%d in use, %d found)", g->tex_replace ? "On" : "Off",
                                        g->replaced, g->replacements_found); break;
            }
            SetTextColor(dc, RGB(255, 230, 140));
            DrawTextA(dc, t, -1, &rv, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
        {
            RECT r = { x0 + fh, y0 + lh * (MENU_N + 1) + fh * 2 / 3, x0 + pw - fh, y0 + ph - fh / 4 };
            HFONT small = CreateFontA(-(fh * 4 / 5), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                      CLEARTYPE_QUALITY, FF_SWISS, "Segoe UI");
            HFONT prev = (HFONT)SelectObject(dc, small);
            SetTextColor(dc, RGB(160, 170, 210));
            snprintf(t, sizeof t, "GPU: %s\nArrows: select and change     F6: close\n"
                     "Folders: textures\\dump and textures\\replace",
                     g->gpu_active ? g->adapter : "software renderer");
            DrawTextA(dc, t, -1, &r, DT_LEFT | DT_NOPREFIX);
            SelectObject(dc, prev);
            DeleteObject(small);
        }
        SelectObject(dc, op); SelectObject(dc, ob);
        DeleteObject(pen); DeleteObject(bg); DeleteObject(hl);
    }
    pick_draw(dc, cw, ch, fh);
    SelectObject(dc, old);
    DeleteObject(font);
}

/* The frame (any size) letterboxed into the client area, with the overlay on
 * top, composed off screen and copied in once: no flicker from the text. */
static void composed_blit(HWND hwnd, HDC hdc, const uint32_t *px, uint32_t w, uint32_t h)
{
    static HDC mdc;
    static HBITMAP mbm;
    static LONG mw, mh;
    BITMAPINFO bi;
    RECT c;
    LONG cw, ch, dw, dh;

    GetClientRect(hwnd, &c);
    cw = c.right - c.left; ch = c.bottom - c.top;
    if (cw <= 0 || ch <= 0)
        return;
    if (!mdc) mdc = CreateCompatibleDC(hdc);
    if (!mbm || mw != cw || mh != ch) {
        if (mbm) DeleteObject(mbm);
        mbm = CreateCompatibleBitmap(hdc, cw, ch);
        SelectObject(mdc, mbm);
        mw = cw; mh = ch;
    }
    FillRect(mdc, &c, (HBRUSH)GetStockObject(BLACK_BRUSH));
    if ((long long)cw * h <= (long long)ch * w) {
        dw = cw; dh = (LONG)((long long)cw * h / w);
    } else {
        dh = ch; dw = (LONG)((long long)ch * w / h);
    }
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = (LONG)w;
    bi.bmiHeader.biHeight = -(LONG)h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(mdc, HALFTONE);
    SetBrushOrgEx(mdc, 0, 0, NULL);
    StretchDIBits(mdc, (cw - dw) / 2, (ch - dh) / 2, dw, dh, 0, 0, (int)w, (int)h,
                  px, &bi, DIB_RGB_COLORS, SRCCOPY);
    overlay_draw(mdc, cw, ch);
    BitBlt(hdc, 0, 0, cw, ch, mdc, 0, 0, SRCCOPY);
}

/* ---- GPU presentation hand-over ------------------------------------------ */

static HWND s_hwnd;
static volatile LONG s_gpu_presenting;
static SRWLOCK s_ovl_lock = SRWLOCK_INIT;
static uint32_t *s_ovl;                 /* client-sized BGRA, alpha 0 = clear */
static uint32_t s_ovl_w, s_ovl_h;
static int s_ovl_seq, s_ovl_shown;

void *xbox_FramebufferWindowHwnd(void) { return (void *)s_hwnd; }
void xbox_FramebufferWindowSetGpuPresent(int on) { InterlockedExchange(&s_gpu_presenting, on ? 1 : 0); }
int xbox_FramebufferWindowOverlayPlane(void) { return s_pvideo_active; }

const uint32_t *xbox_FramebufferWindowOverlayImage(uint32_t *w, uint32_t *h, int *seq)
{
    *w = s_ovl_w; *h = s_ovl_h; *seq = s_ovl_seq;
    return s_ovl_shown ? s_ovl : NULL;
}

void xbox_FramebufferWindowOverlayLock(int lock)
{
    if (lock) AcquireSRWLockShared(&s_ovl_lock);
    else ReleaseSRWLockShared(&s_ovl_lock);
}

static void overlay_draw(HDC dc, int cw, int ch);

/* While the GPU presents, the menu and FPS counter reach the screen as an
 * image it composites on top: drawn here with GDI onto a key colour, which
 * becomes transparent. Rebuilt only when what it shows changes. */
static void overlay_publish(HWND hwnd)
{
    XboxGfxConfig *g = xbox_GfxConfig();
    static HDC dc;
    static HBITMAP bm;
    static uint32_t *bits;
    static int bw, bh;
    static char last[512];
    char sig[512];
    RECT c;
    int cw, ch, show = s_menu_open || g->show_fps || s_pick_sel >= 0;
    size_t i, n;

    GetClientRect(hwnd, &c);
    cw = c.right - c.left; ch = c.bottom - c.top;
    if (!show || cw <= 0 || ch <= 0) {
        if (s_ovl_shown) {
            AcquireSRWLockExclusive(&s_ovl_lock);
            s_ovl_shown = 0; s_ovl_seq++;
            ReleaseSRWLockExclusive(&s_ovl_lock);
        }
        last[0] = 0;
        return;
    }
    snprintf(sig, sizeof sig, "%ld %d %d %d %d %d %d %d %d %d %d %d %d %d %.1f %s", (long)s_pick_seq, cw, ch, s_menu_open, s_menu_sel,
             g->fps60 * 2 + g->direct_move, g->scale, g->show_fps, g->tex_dump, g->tex_replace, g->dumped, g->replaced,
             g->replacements_found, g->gpu_active, s_fps_now, g->adapter);
    if (!strcmp(sig, last))
        return;
    strcpy(last, sig);
    if (!dc) dc = CreateCompatibleDC(NULL);
    if (!bm || bw != cw || bh != ch) {
        BITMAPINFO bi;
        if (bm) DeleteObject(bm);
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = cw;
        bi.bmiHeader.biHeight = -ch;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
        SelectObject(dc, bm);
        bw = cw; bh = ch;
    }
    if (!bm || !bits)
        return;
    n = (size_t)cw * ch;
    for (i = 0; i < n; i++) bits[i] = 0x00FF00FFu;      /* the key colour */
    GdiFlush();
    overlay_draw(dc, cw, ch);
    GdiFlush();
    AcquireSRWLockExclusive(&s_ovl_lock);
    if (!s_ovl || s_ovl_w != (uint32_t)cw || s_ovl_h != (uint32_t)ch) {
        free(s_ovl);
        s_ovl = (uint32_t *)malloc(n * 4);
        s_ovl_w = (uint32_t)cw; s_ovl_h = (uint32_t)ch;
    }
    if (s_ovl) {
        for (i = 0; i < n; i++) {
            uint32_t p = bits[i] & 0x00FFFFFFu;
            s_ovl[i] = p == 0x00FF00FFu ? 0 : (p | 0xFF000000u);
        }
        s_ovl_shown = 1;
        s_ovl_seq++;
    }
    ReleaseSRWLockExclusive(&s_ovl_lock);
}

/* Title bar, the way ps3recomp's window shows it. Written by the flip,
 * read once a second by the window thread; a torn read shows one stale
 * number for a second, which nobody can tell apart from a real one. */
static wchar_t       s_title[48] = L"Xbox Recomp";
static volatile LONG s_flips;
static volatile LONG s_frame_draws;

void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars)
{
    int i;
    for (i = 0; i < max_chars && i < 47 && name[i]; i++)
        s_title[i] = (wchar_t)name[i];
    if (i)
        s_title[i] = 0;
}

void xbox_FramebufferWindowFrameStats(uint32_t draws)
{
    InterlockedIncrement(&s_flips);
    InterlockedExchange(&s_frame_draws, (LONG)draws);
}

/* Alt+Enter / F11: borderless fullscreen on the window's monitor, and back.
 * (DXGI's own Alt+Enter, exclusive mode at the frame's 640x480, is off --
 * gpu_present.c; both presenters letterbox to any size.) */
static void fb_toggle_fullscreen(HWND h)
{
    static int full;
    static RECT saved;
    static LONG_PTR saved_style;
    if (!full) {
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        if (!GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi))
            return;
        GetWindowRect(h, &saved);
        saved_style = GetWindowLongPtrA(h, GWL_STYLE);
        SetWindowLongPtrA(h, GWL_STYLE, (saved_style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        full = 1;
    } else {
        SetWindowLongPtrA(h, GWL_STYLE, saved_style);
        SetWindowPos(h, NULL, saved.left, saved.top, saved.right - saved.left,
                     saved.bottom - saved.top, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);
        full = 0;
    }
}

static LRESULT CALLBACK fb_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    /* First press only (bit 30: the key was already down), and with Alt held
     * (bit 29) for Enter; neither reaches the key table. */
    if ((m == WM_SYSKEYDOWN && w == VK_RETURN && (l & (1 << 29)) && !(l & (1 << 30)))
        || (m == WM_KEYDOWN && w == VK_F11 && !(l & (1 << 30)))) {
        fb_toggle_fullscreen(h);
        return 0;
    }
    if (m == WM_SYSKEYDOWN && w == VK_RETURN)
        return 0;                       /* auto-repeat of Alt+Enter */
    if ((m == WM_KEYDOWN || m == WM_KEYUP || m == WM_SYSKEYDOWN || m == WM_SYSKEYUP)
        && menu_key(m, w))
        return 0;                       /* the F6 menu's, not the game's */
    switch (m) {
    case WM_CLOSE:
    case WM_DESTROY:
        /* Closing the window ends the run. Only stopping this thread left
         * every other one running -- audio played on, and the process had
         * to be ended from Task Manager. The guest has no exit path of its
         * own to ask. TerminateProcess rather than ExitProcess: the guest
         * threads are still running, and ExitProcess's cleanup can wait on
         * a lock one of them held. The exit hook does the reporting that
         * would otherwise be skipped. RECOMP_CLOSE_KEEPS_RUNNING=1 only
         * closes the window, as before. */
        InterlockedExchange(&s_fb_running, 0);
        if (!getenv("RECOMP_CLOSE_KEEPS_RUNNING")) {
            fprintf(stderr, "  [FBWIN] window closed: exiting\n");
            recomp_exit_hook_run("window");
            fflush(stdout);
            fflush(stderr);
            TerminateProcess(GetCurrentProcess(), 0);
        }
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        xbox_InputKeySet((int)w, 1);
        /* A Shift, Ctrl or Alt is also its side's key, for bindings to
         * "lshift" and the like; wParam stays the either-side one. */
        {
            int side = xbox_InputWin32SideVk((int)w, (uint32_t)l);
            if (side)
                xbox_InputKeySet(side, 1);
        }
        /* RECOMP_KEY_TRACE: each key as it arrives, edge-triggered.
         *
         * The obvious diagnostic -- sampling which keys are held, once a
         * second, from the input path -- cannot tell a key that was never
         * pressed from one that was tapped: a 100 ms press is caught about
         * one time in ten. That ambiguity is expensive when the only way
         * to test is to ask someone to press a key and describe what
         * happened. This answers "did it arrive" on its own. */
        if (getenv("RECOMP_KEY_TRACE")) {
            static unsigned n;
            if (n++ < 40) {
                fprintf(stderr, "  [KEY] down vk=0x%02X\n", (unsigned)w);
                fflush(stderr);
            }
        }
        /* System keys still go to Windows, or Alt+F4 stops closing us. */
        return m == WM_SYSKEYDOWN ? DefWindowProcA(h, m, w, l) : 0;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        xbox_InputKeySet((int)w, 0);
        {
            int side = xbox_InputWin32SideVk((int)w, (uint32_t)l);
            if (side)
                xbox_InputKeySet(side, 0);
            /* With both Shifts down Windows sends one key-up, for the
             * last released, so the other side would stay held. Autorepeat
             * sets a Shift still down again. */
            if (w == VK_SHIFT) {
                xbox_InputKeySet(VK_LSHIFT, 0);
                xbox_InputKeySet(VK_RSHIFT, 0);
            }
        }
        return m == WM_SYSKEYUP ? DefWindowProcA(h, m, w, l) : 0;

    /* Mouse buttons go in the same table, for bindings that name them.
     * Capturing the mouse while one is down delivers its release even
     * outside the window. */
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP:
        {
            int down = 0;
            int vk = xbox_InputWin32MouseVk(m, (uint32_t)w, &down);
            if (vk)
                xbox_InputKeySet(vk, down);
            if (down)
                SetCapture(h);
            else if (!(w & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON | MK_XBUTTON1 | MK_XBUTTON2)))
                ReleaseCapture();
        }
        return m == WM_XBUTTONDOWN || m == WM_XBUTTONUP ? TRUE : 0;

    /* Alt-tabbing away with a key held would leave it held for ever. */
    case WM_KILLFOCUS:
        xbox_InputKeysClear();
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

/* The display formats an Xbox front buffer is actually set to. The pitch says
 * how wide a row is in bytes, so pitch/width gives the pixel size; the exact
 * component layout only matters for 16-bit, where 5:6:5 and 1:5:5:5 differ. */
static void fb_convert(const uint8_t *src, uint32_t bpp)
{
    uint32_t x, y;

    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * s_fb_pitch
                           * (bpp == 8 ? s_fb_aa_sy : 1);
        uint32_t *dst = s_rgb + (size_t)y * s_fb_width;

        if (bpp == 4) {
            memcpy(dst, row, (size_t)s_fb_width * 4);
        } else if (bpp == 8) {
            /* A 2x-wide anti-aliased 32-bit surface (pitch = 2 * width * 4):
             * take every other pixel. X-Men Legends renders its 640-wide
             * frame into a 1280-wide surface, which read as "8 bytes per
             * pixel" and showed black. */
            const uint32_t *p = (const uint32_t *)row;
            for (x = 0; x < s_fb_width; x++)
                dst[x] = p[x * 2];
        } else if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (x = 0; x < s_fb_width; x++) {
                uint16_t v = p[x];
                uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
                dst[x] = (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)s_fb_width * 4);
        }
    }
}

/* Write what the window is currently showing to a 24-bit BMP.
 *
 * A black window is ambiguous: it means either that the read path is wrong or
 * that the title really did render black. Dumping the same converted pixels
 * the window draws settles which, and does it without a screenshot. */
int xbox_FramebufferDumpBmp(const char *path)
{
    FILE *f;
    uint32_t row = ((s_fb_width * 3u) + 3u) & ~3u;
    uint32_t img = row * s_fb_height, total = 54u + img, y, x;
    uint8_t hdr[54], *line;

    if (!s_rgb || !s_fb_va)
        return -1;
    f = fopen(path, "wb");
    if (!f)
        return -1;
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &total, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &s_fb_width, 4);
    memcpy(hdr + 22, &s_fb_height, 4);
    hdr[26] = 1; hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, sizeof(hdr), f);

    line = (uint8_t *)calloc(1, row);
    for (y = 0; y < s_fb_height; y++) {
        const uint32_t *src = s_rgb + (size_t)(s_fb_height - 1 - y) * s_fb_width;
        for (x = 0; x < s_fb_width; x++) {
            line[x * 3 + 0] = (uint8_t)(src[x] & 0xFF);
            line[x * 3 + 1] = (uint8_t)((src[x] >> 8) & 0xFF);
            line[x * 3 + 2] = (uint8_t)((src[x] >> 16) & 0xFF);
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
    fprintf(stderr, "  [FBWIN] wrote %s (%ux%u from 0x%08X)\n",
            path, s_fb_width, s_fb_height, s_fb_va);
    return 0;
}

/* GDI: the frame letterboxed into the client area, like the D3D11
 * presenter; the borders are cleared when the size changes. */
static void gdi_blit(HWND hwnd, HDC hdc, const BITMAPINFO *bi)
{
    static LONG lw, lh;
    RECT c;
    LONG cw, ch, w, h;
    GetClientRect(hwnd, &c);
    cw = c.right - c.left; ch = c.bottom - c.top;
    if (cw <= 0 || ch <= 0)
        return;
    if (cw != lw || ch != lh) {
        FillRect(hdc, &c, (HBRUSH)GetStockObject(BLACK_BRUSH));
        lw = cw; lh = ch;
    }
    if ((long long)cw * s_fb_height <= (long long)ch * s_fb_width) {
        w = cw; h = (LONG)((long long)cw * s_fb_height / s_fb_width);
    } else {
        h = ch; w = (LONG)((long long)ch * s_fb_width / s_fb_height);
    }
    SetStretchBltMode(hdc, HALFTONE);
    SetBrushOrgEx(hdc, 0, 0, NULL);     /* HALFTONE wants it after the mode */
    StretchDIBits(hdc, (cw - w) / 2, (ch - h) / 2, w, h,
                  0, 0, (int)s_fb_width, (int)s_fb_height,
                  s_rgb, bi, DIB_RGB_COLORS, SRCCOPY);
}

/* The converted frame in s_rgb, on screen: D3D11 when it is up (which waits
 * for vsync, so returns 1 when the loop is already paced), else GDI. */
static int fb_show(HWND hwnd, HDC hdc, const BITMAPINFO *bi)
{
    XboxGfxConfig *g = xbox_GfxConfig();
    const uint32_t *hi;
    uint32_t hw = 0, hh = 0;

    if (gpu_present_active())
        return gpu_present_frame(s_rgb);
    hi = hires_frame(&hw, &hh);
    if (hi)
        composed_blit(hwnd, hdc, hi, hw, hh);
    else if (s_menu_open || g->show_fps)
        composed_blit(hwnd, hdc, s_rgb, s_fb_width, s_fb_height);
    else
        gdi_blit(hwnd, hdc, bi);
    return 0;
}

static DWORD WINAPI fb_thread(LPVOID unused)
{
    HWND hwnd;
    HDC hdc;
    BITMAPINFO bi;
    RECT r;
    int fb_pinned;

    (void)unused;

    {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc   = fb_wndproc;
        wc.hInstance     = GetModuleHandleA(NULL);
        wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
        wc.lpszClassName = "XboxRecompFramebuffer";
        RegisterClassA(&wc);
    }
    {
        /* RECOMP_WINDOW_SCALE (1-8, default 2, as in the SDL window): the
         * client area is the frame times this, made smaller until the window
         * fits the desktop work area, and centred on it. */
        const char *e = getenv("RECOMP_WINDOW_SCALE");
        int scale = e ? atoi(e) : 2;
        RECT work;
        if (scale < 1 || scale > 8)
            scale = 2;
        if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0)) {
            work.left = work.top = 0; work.right = 1920; work.bottom = 1080;
        }
        for (;; scale--) {
            r.left = 0; r.top = 0;
            r.right = (LONG)s_fb_width * scale; r.bottom = (LONG)s_fb_height * scale;
            AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
            if (scale <= 1 || (r.right - r.left <= work.right - work.left
                               && r.bottom - r.top <= work.bottom - work.top))
                break;
        }
        hwnd = CreateWindowExA(0, "XboxRecompFramebuffer", "Xbox Recomp - Framebuffer",
                               WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               work.left + ((work.right - work.left) - (r.right - r.left)) / 2,
                               work.top + ((work.bottom - work.top) - (r.bottom - r.top)) / 2,
                               r.right - r.left, r.bottom - r.top,
                               NULL, NULL, GetModuleHandleA(NULL), NULL);
    }
    s_hwnd = hwnd;
    if (!hwnd) {
        InterlockedExchange(&s_fb_running, 0);
        return 0;
    }
    hdc = GetDC(hwnd);
    {
        const char *fs = getenv("RECOMP_FULLSCREEN");
        if (fs && *fs && *fs != '0')
            fb_toggle_fullscreen(hwnd);
    }

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = (LONG)s_fb_width;
    bi.bmiHeader.biHeight      = -(LONG)s_fb_height;   /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    s_rgb = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height, 4);
    /* A window pinned to one address shows that memory and nothing else. */
    fb_pinned = getenv("RECOMP_FB_VA") != NULL;

    fprintf(stderr, "  [FBWIN] framebuffer window open (%ux%u)\n",
            s_fb_width, s_fb_height);
    if (gpu_present_enabled())
        gpu_present_init(hwnd, s_fb_width, s_fb_height);

    while (InterlockedCompareExchange(&s_fb_running, 1, 1)) {
        MSG msg;
        int paced = 0;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (s_gpu_presenting) {
            /* The GPU draws the frame into this window itself. GDI only
             * takes over while the video overlay plane is in use (the GPU
             * then leaves the window alone). */
            static DWORD last_frame, last_note;
            static LONG seen_seq;
            DWORD now = GetTickCount();
            overlay_publish(hwnd);
            if (s_present_seq != seen_seq) {
                seen_seq = s_present_seq;
                last_frame = now;
                s_busy_note = 0;
            }
            if (s_rgb && fb_copy_presented_frame(s_rgb)) {
                if (!fb_pinned)
                    fb_overlay();
                if (s_pvideo_active)
                    paced = fb_show(hwnd, hdc, &bi);
            }
            if (s_rgb && last_frame && !s_pvideo_active && now - last_frame > 1500
                && now - last_note > 200) {
                /* No frame for 1.5 s: the GPU has nothing new to present,
                 * so GDI shows the last one with the note on it. */
                s_busy_note = 1;
                last_note = now;
                composed_blit(hwnd, hdc, s_rgb, s_fb_width, s_fb_height);
            }
        } else if (s_rgb && fb_copy_presented_frame(s_rgb)) {
            /* A finished frame, published by the flip. Copied into s_rgb so
             * the dump path and GDI see one consistent image even if the
             * next flip lands mid-blit. */
            if (!fb_pinned)
                fb_overlay();
            paced = fb_show(hwnd, hdc, &bi);
        } else if (s_fb_va && s_fb_pitch && s_rgb) {
            /* No flip yet, or pinned with RECOMP_FB_VA: read guest memory as
             * before, which is also what a title that never flips needs. */
            const uint8_t *src =
                (const uint8_t *)((uintptr_t)s_fb_va + xbox_GetMemoryOffset());
            fb_convert(src, s_fb_pitch / s_fb_width);
            if (!fb_pinned)
                fb_overlay();
            paced = fb_show(hwnd, hdc, &bi);
        }
        {
            /* One dump a few seconds in, so the title has had time to render
             * something rather than catching the first blank frame.
             *
             * RECOMP_FB_WINDOW_DUMP_EVERY=<frames> dumps repeatedly instead.
             * This window follows the address AvSetDisplayMode gave, which is
             * what the CRTC scans and therefore what a person sees; the
             * pushbuffer executor's own dump follows its draw surface. With
             * double buffering those are different buffers, and measuring
             * progress from the executor's dump reports a blank screen while
             * the window is showing the title's logo. Ask the window. */
            const char *dump = getenv("RECOMP_FB_DUMP");
            const char *every = getenv("RECOMP_FB_WINDOW_DUMP_EVERY");
            static int frames;
            int period = every ? atoi(every) : 0;
            frames++;
            if (dump && period > 0) {
                if (frames % period == 0)
                    xbox_FramebufferDumpBmp(dump);
            } else if (dump && frames == 600) {
                xbox_FramebufferDumpBmp(dump);
            }
        }
        {
            static DWORD t0;
            static LONG f0;
            DWORD now = GetTickCount();
            if (now - t0 >= 1000) {
                LONG f = s_flips;
                wchar_t tb[128];
                s_fps_now = t0 ? (f - f0) * 1000.0 / (now - t0) : 0.0;
                _snwprintf(tb, 127, L"%ls | FPS: %.1f | draws: %ld  (F6: graphics)", s_title,
                           s_fps_now, (long)s_frame_draws);
                tb[127] = 0;
                SetWindowTextW(hwnd, tb);
                t0 = now; f0 = f;
            }
        }
        if (!paced)
            Sleep(16);                  /* D3D11's Present waited for vsync */
    }

    gpu_present_shutdown();
    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
    free(s_rgb);
    s_rgb = NULL;
    free(s_pvideo_source);
    s_pvideo_source = NULL;
    return 0;
}

void xbox_FramebufferWindowStart(void)
{
    HANDLE th;

    if (!getenv("RECOMP_FB_WINDOW"))
        return;
    if (InterlockedCompareExchange(&s_fb_running, 1, 0) != 0)
        return;
    th = CreateThread(NULL, 0, fb_thread, NULL, 0, NULL);
    if (th)
        CloseHandle(th);
    else
        InterlockedExchange(&s_fb_running, 0);
}

#endif /* _WIN32: POSIX hosts get the SDL2 window in fb_present_sdl.c */
