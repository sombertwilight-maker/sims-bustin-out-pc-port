/*
 * Graphics settings shared by the framebuffer window (which owns the F6 menu)
 * and the GPU back end (which applies them). The window writes the user's
 * choices; the back end reads them once a frame and writes back what it can
 * report. Plain volatile ints: each field has one writer.
 *
 * Settings start from the environment (settings.ini is loaded into it), and
 * the menu writes changes back to settings.ini next to the exe:
 *   RECOMP_GPU_SCALE    1-4  internal resolution, times 640x480
 *   RECOMP_SHOW_FPS     0/1  frame-rate counter in the window
 *   RECOMP_TEX_DUMP     0/1  save each texture once to textures\dump\*.png
 *   RECOMP_TEX_REPLACE  0/1  use textures\replace\<same name>.png instead
 */
#ifndef XBOXRECOMP_GFX_CONFIG_H
#define XBOXRECOMP_GFX_CONFIG_H

#include <stdint.h>

typedef struct {
    /* chosen in the menu */
    volatile int scale;              /* 1..4 */
    volatile int show_fps;
    volatile int tex_dump;
    volatile int tex_replace;
    volatile int reload_seq;         /* bumped: re-read textures\replace */
    volatile int fps60;              /* experimental: 120 Hz vblank (RECOMP_FPS60) */
    volatile int direct_move;        /* title option: left stick moves the character (SIMS_DIRECT_MOVE) */
    volatile int cam_dist;           /* title option: follow camera distance, tenths of a tile (SIMS_CAM_DIST) */
    volatile int cam_angle;          /* ... looking down, degrees (SIMS_CAM_ANGLE) */
    volatile int cam_aim;            /* ... aim height above the floor, tenths of a tile (SIMS_CAM_AIM) */
    volatile int cam_follow;         /* ... swinging behind her: 0 off, 1 slow, 2 normal, 3 fast (SIMS_CAM_FOLLOW) */
    /* reported by the back end */
    volatile int gpu_active;         /* 0: software renderer (no scaling) */
    volatile int scale_applied;
    volatile int dumped, replaced, replacements_found;
    char adapter[128];
} XboxGfxConfig;

XboxGfxConfig *xbox_GfxConfig(void);

/* A frame rendered above 640x480: the window shows it instead of the guest
 * framebuffer copy that follows (unless the video overlay is in use). */
void xbox_FramebufferWindowPresentHiRes(const uint32_t *argb, uint32_t width,
                                        uint32_t height, uint32_t stride_px);

/* GPU presentation: the back end draws the frame straight into the game
 * window (a swap chain) and the window stops drawing it with GDI. */
void *xbox_FramebufferWindowHwnd(void);          /* HWND, NULL before open */
void xbox_FramebufferWindowSetGpuPresent(int on);
int  xbox_FramebufferWindowOverlayPlane(void);   /* video overlay in use */
/* The menu/FPS overlay as a client-sized BGRA image with alpha (NULL when
 * nothing is shown). *seq changes when the image does. Call with the lock
 * functions around reading the pixels. */
const uint32_t *xbox_FramebufferWindowOverlayImage(uint32_t *w, uint32_t *h, int *seq);
void xbox_FramebufferWindowOverlayLock(int lock);


/* A list the game's code can put on screen over the picture (the direct
 * movement mod's "things nearby" list). Items are copied; sel < 0 hides. */
#define XBOX_PICK_MAX 12
void xbox_PickListShow(const char *title, const char *const *items, int n, int sel, const char *hint);
void xbox_PickListHide(void);

#endif
