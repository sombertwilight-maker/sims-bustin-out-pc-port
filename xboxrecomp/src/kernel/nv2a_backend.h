/*
 * NV2A render back-end interface.
 *
 * The pushbuffer executor (nv2a_pb_exec.c) decodes what a title asks the GPU
 * to do. By default it carries that out itself, on the CPU, into guest memory.
 * A game project can instead register a back end -- a D3D11 renderer, say --
 * and the executor hands it the work in already-decoded form:
 *
 *   surface state + clears, triangles (transformed to surface pixels, with
 *   per-vertex colour and texel-space UVs), the texture each batch samples,
 *   and the flip that ends a frame.
 *
 * Everything is called on the NV2A poll thread, one call at a time, so a back
 * end may create its window and device lazily on the first call and pump its
 * window messages from flip().
 *
 * Fixed-function, pre-transformed and vertex-program batches all arrive the
 * same way: the executor transforms fixed-function vertices itself, runs
 * vertex programs on the CPU (nv2a_vsh_interp.c) and clips at the near plane,
 * so a back end only ever sees surface pixels. Blend, depth, alpha, cull and
 * stencil state arrive in Nv2aRenderState.
 *
 * ponytail: register-combiner state and texture stages 1-3 are not passed on:
 * a back end sees stage 0 and the diffuse colour. Extend Nv2aBatch when that
 * matters rather than adding callbacks.
 */
#ifndef NV2A_BACKEND_H
#define NV2A_BACKEND_H

#include <stdint.h>

/* The colour surface being drawn into, in real (anti-aliased) pixels.
 * aa_sx/aa_sy give the anti-aliasing factor, so width/aa_sx is the logical
 * size the title thinks it renders at (e.g. 640x480). */
typedef struct {
    uint32_t color_va;          /* guest address of pixel (0,0) */
    uint32_t width, height;     /* clip rectangle size, real pixels */
    uint32_t pitch;             /* bytes per row */
    uint32_t bytes_per_pixel;   /* 2 or 4 */
    uint32_t aa_sx, aa_sy;      /* 1 or 2 each */
} Nv2aSurface;

/* A texture as the title programmed it. uv in Nv2aVertex are in texels. */
typedef struct {
    uint32_t offset;            /* guest address of texel (0,0) */
    uint32_t width, height;
    uint32_t pitch;             /* linear formats only */
    uint32_t color;             /* NV097 colour-format code */
    uint32_t addr_u, addr_v;    /* NV097 wrap mode per axis (1 wrap, 3 clamp) */
    /* Mip levels stored after level 0 (the format word's MIPMAP_LEVELS; 1 for
     * a linear texture) and the raw SET_TEXTURE_FILTER word (MIN in bits
     * 16-23). A back end that ignores them samples level 0. */
    uint32_t levels, filter;
} Nv2aTexture;

typedef struct {
    float    x, y, z;           /* surface pixels; z as the title produced it */
    float    rhw;               /* 1/w, 1 for pre-transformed batches */
    uint32_t diffuse;           /* 0xAARRGGBB */
    float    u, v;              /* texels */
} Nv2aVertex;

/* Render state as the title set it, in NV2A's own (OpenGL) enum values:
 * blend factors GL_ZERO/GL_ONE/0x300..0x308/0x8001..0x8004, equations
 * GL_FUNC_ADD 0x8006 etc., compare functions GL_NEVER 0x200 .. GL_ALWAYS 0x207.
 * color_mask uses NV2A's bytes: A 0x01000000, R 0x00010000, G 0x100, B 0x1. */
typedef struct {
    uint32_t blend_enable, blend_src, blend_dst, blend_eq, blend_color;
    uint32_t alpha_test_enable, alpha_func, alpha_ref;   /* ref 0..255 */
    uint32_t depth_test_enable, depth_func, depth_write;
    uint32_t color_mask;
    uint32_t cull_enable, cull_face, front_face;
    uint32_t zeta_va;                                    /* 0: no depth surface */
    float    depth_min, depth_max;                       /* SET_CLIP_MIN/MAX */
    /* SET_STENCIL_*: compare function as above; ops GL_KEEP 0x1E00, GL_ZERO 0,
     * GL_REPLACE 0x1E01, GL_INCR 0x1E02, GL_DECR 0x1E03, GL_INVERT 0x150A,
     * GL_INCR_WRAP 0x8507, GL_DECR_WRAP 0x8508. Reset values GL_ALWAYS, masks
     * 0xFF, GL_KEEP. A title can confine a pass to the pixels an earlier pass
     * marked (a full-screen quad that only marked pixels pass); a back end
     * that ignores these draws that pass over the whole surface. */
    uint32_t stencil_enable, stencil_func, stencil_ref, stencil_func_mask;
    uint32_t stencil_write_mask, stencil_fail, stencil_zfail, stencil_zpass;
} Nv2aRenderState;

typedef struct {
    const Nv2aVertex  *vertices;    /* triangle list: count is a multiple of 3 */
    uint32_t           count;
    const Nv2aTexture *texture;     /* NULL: untextured */
    const Nv2aRenderState *state;
} Nv2aBatch;

typedef struct Nv2aGpuVsh {
    const uint32_t (*prog)[4];
    uint32_t nins;
    uint64_t hash;                 /* of prog[0..nins): a shader cache key  */
    uint32_t in_mask;              /* the inputs the program reads          */
    uint32_t stride;               /* floats per vertex: 4 * popcount(mask) */
    const float (*consts)[4];      /* c[0..191]                             */
    float    z_offset;             /* c[59].z: the near plane, screen z     */
    uint32_t near_z;               /* clip at it (RECOMP_CLIP_NEAR_Z)       */
    uint32_t fog_enable, fog_mode; /* fog_factor's inputs                   */
    float    fog_param[2];
    /* Small batches only (probed != 0): the screen-space box the program puts
     * the vertices in, and the range of their w, found by running it on the
     * CPU -- how a back end tells a flat 2D overlay from a 3D draw. */
    uint32_t probed;
    float    box[4];               /* x0 y0 x1 y1, surface pixels           */
    float    wmin, wmax;
} Nv2aGpuVsh;

typedef struct {
    /* flags: CLEAR_SURFACE bits (Z 0x1, stencil 0x2, colour 0xF0).
     * zstencil: SET_ZSTENCIL_CLEAR_VALUE (depth in the top 24 bits for Z24S8). */
    void (*clear)(const Nv2aSurface *s, const Nv2aRenderState *rs,
                  uint32_t flags, uint32_t argb, uint32_t zstencil);
    void (*draw)(const Nv2aSurface *s, const Nv2aBatch *b);
    void (*flip)(void);
} Nv2aBackend;

/* Register (or, with NULL, remove) the back end. Call before the title starts
 * submitting work; the executor must also be enabled (RECOMP_PB_EXEC). */
void nv2a_backend_register(const Nv2aBackend *backend);

/* Register a back end that must synchronize a surface into guest memory
 * before the flip's window snapshot or framebuffer dump. present_sync runs
 * synchronously on the executor thread and must complete pending GPU work and
 * readback before returning; it must not present a window. flip still runs
 * after the snapshot and dump. A NULL present_sync disables synchronization;
 * a NULL backend removes both registrations. A later nv2a_backend_register
 * call replaces both registrations. */
void nv2a_backend_register_with_present_sync(const Nv2aBackend *backend,
                                             void (*present_sync)(void));

/* Decode a whole texture to 0xAARRGGBB, row-major, width*height entries.
 * Handles every format the executor can sample (swizzled, linear, DXT).
 * Returns 0 if the format is not supported. */
int nv2a_backend_decode_texture(const Nv2aTexture *tex, uint32_t *argb_out);

/* Where BACK_END_WRITE_SEMAPHORE_RELEASE (0x1D70) values land: the guest VA
 * of the semaphore the title reads GPU progress from (for XDK D3D, the
 * pointer at device+0x30). The executor writes each release there once it
 * has executed everything before it -- which is
 * what lets D3D's fence waits and ring-space checks see real progress.
 *
 * ponytail: the title supplies the address instead of the executor resolving
 * the semaphore context DMA object through RAMIN. */
void nv2a_pb_set_semaphore_target(uint32_t guest_va);

/* ---- Full-pipeline GPU back end -------------------------------------------
 *
 * The Nv2aBackend above sees stage 0 and the diffuse colour only, which is
 * enough for a menu and not for a title that mixes textures through the
 * register combiners. An Nv2aGpuBackend gets everything the CPU rasteriser
 * itself uses per pixel: both vertex colours, the fog factor, four texture
 * coordinates, the four texture stages, the raw register-combiner program and
 * the render state -- so a GPU can reproduce the software result exactly,
 * and the CPU rasteriser is not run at all.
 *
 * Vertices are after the executor's own transform and near-plane clipping:
 * x, y in surface pixels, z as the title produced it (0..2^16-1 or
 * 0..2^24-1, by the zeta format), w the clip-space w (> 0), so attributes can
 * be interpolated perspective-correct.
 *
 * Guest memory is not written while one is registered. present_sync is asked
 * for each surface the window is about to show and must copy it into guest
 * memory (row-major, at `pitch`, bpp 2 = R5G6B5 or 4 = A8R8G8B8). Called on the
 * executor thread, like the rest. */
#include "nv2a_combiner.h"

typedef struct {
    float pos[4];
    float d0[4], d1[4];       /* r g b a, 0..1 */
    float fog;                /* fog factor, 1 = no fog */
    float tex[4][4];          /* per stage, as the vertex program wrote them */
} Nv2aGpuVertex;

typedef struct {
    uint32_t valid;
    uint32_t offset;          /* guest address (DMA resolved) of texel (0,0) */
    uint32_t width, height, pitch, color, levels, filter;
    uint32_t addr_u, addr_v;  /* 1 wrap, 2 mirror, 3/5 clamp, 4 border */
    uint32_t palette;         /* guest address of the CLUT, P8 formats */
    uint32_t border;          /* border colour, ARGB */
    uint32_t border_color;    /* border texels read `border` */
    uint32_t cube;
    uint32_t normalized;      /* coordinates arrive 0..1 (swizzled, DXT) */
    uint32_t bytes;           /* bytes of level 0 (all faces for a cube) */
} Nv2aGpuTexture;

typedef struct {
    /* colour surface */
    uint32_t color_va, pitch, bpp, color_fmt;
    uint32_t swizzled, swz_w, swz_h;        /* swizzled: 1<<log size */
    uint32_t clip_x, clip_y, clip_w, clip_h;
    /* depth/stencil surface: zeta_va 0 = none; zeta_bits 16 or 24 */
    uint32_t zeta_va, zeta_bits;
    Nv2aRenderState rs;
    /* pixel pipeline */
    Nv2aCombiner rc;
    uint32_t use_rc;          /* 0: "stage-0 texel * diffuse" (pre-combiner) */
    uint32_t textured;        /* use_rc 0: the batch samples stage 0 */
    uint32_t clip_plane_mode;
    uint32_t fog_color;       /* SET_FOG_COLOR: R in bits 0-7 */
    uint32_t bilinear;        /* RECOMP_TEX_BILINEAR: tent-filter everything */
    Nv2aGpuTexture tex[4];
} Nv2aGpuState;

typedef struct {
    /* flags: CLEAR_SURFACE bits (Z 0x1, stencil 0x2, colour 0xF0: A 0x80,
     * R 0x40, G 0x20, B 0x10). rect: x0, y0, x1, y1 (exclusive). */
    void (*clear)(const Nv2aGpuState *st, uint32_t flags, uint32_t argb,
                  uint32_t zstencil, const uint32_t rect[4]);
    void (*draw)(const Nv2aGpuState *st, const Nv2aGpuVertex *v, uint32_t count);
    void (*present_sync)(uint32_t color_va, uint32_t pitch, uint32_t bpp,
                         uint32_t width, uint32_t height);
    /* A frame ended (after present_sync): a back end may trim caches. */
    void (*end_frame)(void);
    /* Optional: a batch whose vertex program runs on the GPU. `prog` holds
     * the program from its start slot through FINAL (nins instructions, the
     * NV2A's 128-bit encoding, nv2a_vsh_interp.c); `consts` the 192
     * constants. `in` carries one vertex per `stride` floats: the program's
     * inputs in ascending order of the set bits of in_mask (v0..v15), four
     * floats each, read as the interpreter would read them. `idx` lists
     * triangles (three per). Returns 0 if it cannot draw the batch; the
     * executor then runs the program on the CPU as before. */
    int (*draw_vsh)(const Nv2aGpuState *st, const struct Nv2aGpuVsh *vs,
                    const float *in, uint32_t nverts,
                    const uint32_t *idx, uint32_t nidx);
} Nv2aGpuBackend;

void nv2a_gpu_backend_register(const Nv2aGpuBackend *backend);
int  nv2a_gpu_backend_active(void);

/* Decode level 0 of a texture (face `face` of a cube) to 0xAARRGGBB,
 * width*height entries, with the executor's own sampler: every format it
 * can sample, palettes included. Returns 0 if the format is not supported. */
int nv2a_gpu_decode_texture(const Nv2aGpuTexture *tex, uint32_t face,
                            uint32_t *argb_out);

#endif /* NV2A_BACKEND_H */
