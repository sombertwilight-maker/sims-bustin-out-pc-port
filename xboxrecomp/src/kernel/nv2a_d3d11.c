/*
 * NV2A on the host GPU: a Direct3D 11 back end for the pushbuffer executor.
 *
 * The executor (nv2a_pb_exec.c) decodes the title's pushbuffer, runs its
 * vertex programs, clips at the near plane, and hands the triangles here with
 * everything the software rasteriser would have used per pixel
 * (nv2a_backend.h, Nv2aGpuBackend). This file draws them with D3D11 instead:
 *
 *   - the register-combiner program (and the texture stage modes ahead of it)
 *     becomes an HLSL pixel shader, generated to match nv2a_combiner.c's
 *     nv2a_rc_eval term for term and compiled once per distinct program;
 *   - colour surfaces become render targets and zeta surfaces depth/stencil
 *     buffers, both keyed by guest address and kept on the GPU;
 *   - textures are decoded from guest memory with the executor's own sampler
 *     (every format it reads, palettes and DXT included) and re-checked once a
 *     frame by a hash of their bytes; a texture whose address is a render
 *     target -- render-to-texture -- binds that render target directly;
 *   - at each flip, the surface being presented is copied back into guest
 *     memory, where the framebuffer window (and everything else that reads
 *     the frame) finds it as before.
 *
 * Everything runs on the executor's thread, which is the only one touching
 * the immediate context.
 *
 * RECOMP_GPU=0 keeps the software rasteriser. RECOMP_GPU_STATS=1 prints a
 * line of counters every few seconds.
 */
#if defined(_WIN32)
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <objbase.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_backend.h"
#include "video/gfx_config.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);

static ID3D11Device        *D;
static ID3D11DeviceContext *C;
static uint32_t s_frame;
static int s_stats = -1;

static struct {
    uint32_t draws, verts, clears, tex_uploads, tex_hits_rt, shaders, readbacks, rt_stale;
    uint32_t vsh_draws, vprogs, const_uploads;
    uint64_t tex_bytes;
} S;

static uint8_t *gmem(void) { return (uint8_t *)xbox_GetMemoryOffset(); }

/* ---- hashing ------------------------------------------------------------ */

static uint64_t hash_bytes(const uint8_t *p, size_t n, uint64_t seed)
{
    /* Four independent 64-bit lanes (xxh64's core loop, simplified): fast
     * enough to re-check every texture once a frame. */
    const uint64_t P1 = 0x9E3779B185EBCA87ull, P2 = 0xC2B2AE3D27D4EB4Full;
    uint64_t a = seed + P1, b = seed ^ P2, c = seed, d = seed - P1;
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        uint64_t w[4];
        memcpy(w, p + i, 32);
        a = (a ^ (w[0] * P2)); a = (a << 31 | a >> 33) * P1;
        b = (b ^ (w[1] * P2)); b = (b << 31 | b >> 33) * P1;
        c = (c ^ (w[2] * P2)); c = (c << 31 | c >> 33) * P1;
        d = (d ^ (w[3] * P2)); d = (d << 31 | d >> 33) * P1;
    }
    a ^= (b << 7 | b >> 57) ^ (c << 12 | c >> 52) ^ (d << 18 | d >> 46);
    for (; i < n; i++)
        a = (a ^ p[i]) * P1;
    a ^= n;
    a ^= a >> 33; a *= P2; a ^= a >> 29;
    return a;
}

static uint64_t hash_u32s(const uint32_t *w, size_t n)
{
    return hash_bytes((const uint8_t *)w, n * 4, 0x51EDu);
}

/* ---- shaders ------------------------------------------------------------ */

static ID3DBlob *compile(const char *src, const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(src, strlen(src), "nv2a", NULL, NULL, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "[GPU-D3D11] shader compile failed (%08lX): %s\n",
                (unsigned long)hr, err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        if (getenv("RECOMP_GPU_SHADER_DUMP"))
            fprintf(stderr, "%s\n", src);
        if (err) ID3D10Blob_Release(err);
        return NULL;
    }
    if (err) ID3D10Blob_Release(err);
    return code;
}

/* Vertex: everything after the executor's transform. Position is in surface
 * pixels; the shader maps it to clip space keeping w, so the GPU interpolates
 * perspective-correct as the software path does. */
static const char s_vs_src[] =
    "cbuffer V : register(b0) { float4 vp; };\n"
    "struct VI { float4 p : POSITION; float4 d0 : COLOR0; float4 d1 : COLOR1;\n"
    "  float fog : FOG; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1;\n"
    "  float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; };\n"
    "struct PI { float4 p : SV_Position; float4 d0 : COLOR0; float4 d1 : COLOR1;\n"
    "  noperspective float fog : FOG; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1;\n"
    "  float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; };\n"
    "PI vs(VI i) { PI o; float w = i.p.w;\n"
    "  float x = i.p.x * vp.x - 1.0, y = 1.0 - i.p.y * vp.y, z = i.p.z * vp.z;\n"
    "  o.p = float4(x * w, y * w, z * w, w);\n"
    "  o.d0 = i.d0; o.d1 = i.d1; o.fog = i.fog;\n"
    "  o.t0 = i.t0; o.t1 = i.t1; o.t2 = i.t2; o.t3 = i.t3; return o; }\n";

/* Clears: a viewport-covering triangle writing a colour and a depth. */
static const char s_clear_src[] =
    "cbuffer K : register(b0) { float4 col; float4 dep; };\n"
    "float4 vs(uint i : SV_VertexID) : SV_Position {\n"
    "  float2 q = float2((i << 1) & 2, i & 2);\n"
    "  return float4(q * float2(2, -2) + float2(-1, 1), 0, 1); }\n"
    "void ps(float4 p : SV_Position, out float4 c : SV_Target, out float d : SV_Depth)\n"
    "  { c = col; d = dep.x; }\n";

static ID3D11VertexShader *s_vs, *s_clear_vs;
static ID3D11PixelShader  *s_clear_ps;
static ID3D11InputLayout  *s_layout;
static ID3D11Buffer       *s_vcb, *s_pcb, *s_ccb, *s_vb;
static UINT                s_vb_cap, s_vb_pos;    /* vertices */
static ID3D11RasterizerState *s_rs;

/* The pixel shader's constants. */
typedef struct {
    float f0[8][4], f1[8][4];         /* SET_COMBINER_FACTOR0/1 per stage */
    float fc0[4], fc1[4];             /* SET_SPECULAR_FOG_FACTOR 0/1      */
    float fogc[4];                    /* fog colour                       */
    float ts[4][4];                   /* per stage: uv scale xy           */
    int   aref[4];                    /* alpha reference                  */
} PsConst;

/* What a pixel shader depends on; the generated source is a function of
 * this alone. */
typedef struct {
    uint32_t use_rc, textured;
    uint32_t color_icw[8], alpha_icw[8], color_ocw[8], alpha_ocw[8];
    uint32_t final0, final1, control, stage_program, clip_plane_mode;
    uint32_t kind[4];        /* 0 unbound (reads white), 1 2D, 2 cube */
    uint32_t swap_rb[4];     /* texture reads a render target in the other order */
    uint32_t alpha_test, alpha_func;
    uint32_t dbg;            /* RECOMP_GPU_DEBUG experiments */
} PsKey;

typedef struct { uint64_t h; PsKey k; ID3D11PixelShader *ps; } PsEnt;
static PsEnt *s_ps;
static int s_nps, s_cap_ps;

/* -- HLSL generation, following nv2a_rc_eval (nv2a_combiner.c) -- */

typedef struct { char *p; size_t n, cap; } Buf;

static void bput(Buf *b, const char *fmt, ...)
{
    va_list ap;
    int k;
    for (;;) {
        va_start(ap, fmt);
        k = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
        va_end(ap);
        if (k >= 0 && b->n + (size_t)k < b->cap) { b->n += (size_t)k; return; }
        b->cap = b->cap * 2 + (k > 0 ? (size_t)k : 256);
        b->p = (char *)realloc(b->p, b->cap);
    }
}

static const char *REG[16] = {
    "rz", "c0", "c1", "fogr", "v0", "v1", "r6", "r7",
    "t0", "t1", "t2", "t3", "r0", "r1", "sm", "ef"
};

/* map_in: PS_INPUTMAPPING applied to expression x. */
static void emit_map(Buf *b, const char *x, uint32_t byte)
{
    switch ((byte >> 5) & 7) {
    case 0: bput(b, "max(%s,0)", x); break;
    case 1: bput(b, "(1-saturate(%s))", x); break;
    case 2: bput(b, "(2*max(%s,0)-1)", x); break;
    case 3: bput(b, "(-2*max(%s,0)+1)", x); break;
    case 4: bput(b, "(max(%s,0)-0.5)", x); break;
    case 5: bput(b, "(-max(%s,0)+0.5)", x); break;
    case 6: bput(b, "(%s)", x); break;
    default: bput(b, "(-(%s))", x); break;
    }
}

static void emit_rgb(Buf *b, uint32_t byte)
{
    char x[16];
    snprintf(x, sizeof x, "%s.%s", REG[byte & 0xF], (byte & 0x10) ? "aaa" : "rgb");
    emit_map(b, x, byte);
}

static void emit_alpha(Buf *b, uint32_t byte)
{
    char x[16];
    snprintf(x, sizeof x, "%s.%s", REG[byte & 0xF], (byte & 0x10) ? "a" : "b");
    emit_map(b, x, byte);
}

static void emit_mapout(Buf *b, const char *x, uint32_t mapping)
{
    switch (mapping) {
    case 0x08: bput(b, "clamp((%s)-0.5,-1,1)", x); break;
    case 0x10: bput(b, "clamp((%s)*2,-1,1)", x); break;
    case 0x18: bput(b, "clamp(((%s)-0.5)*2,-1,1)", x); break;
    case 0x20: bput(b, "clamp((%s)*4,-1,1)", x); break;
    case 0x30: bput(b, "clamp((%s)*0.5,-1,1)", x); break;
    default:   bput(b, "clamp(%s,-1,1)", x); break;
    }
}

static const char *cmp_op(uint32_t f)
{
    switch (f) {
    case 0x201: return "<";  case 0x202: return "==";
    case 0x203: return "<="; case 0x204: return ">";
    case 0x205: return "!="; case 0x206: return ">=";
    default:    return NULL;
    }
}

static char *gen_ps(const PsKey *k)
{
    Buf b = { (char *)malloc(16384), 0, 16384 };
    int st;

    bput(&b,
         "cbuffer P : register(b0) { float4 F0[8]; float4 F1[8]; float4 FC0; float4 FC1;\n"
         "  float4 FOGC; float4 TS[4]; int4 AREF; };\n"
         "SamplerState S0 : register(s0); SamplerState S1 : register(s1);\n"
         "SamplerState S2 : register(s2); SamplerState S3 : register(s3);\n");
    for (st = 0; st < 4; st++)
        bput(&b, "%s T%d : register(t%d);\n",
             k->kind[st] == 2 ? "TextureCube" : "Texture2D", st, st);
    bput(&b,
         "struct PI { float4 p : SV_Position; float4 d0 : COLOR0; float4 d1 : COLOR1;\n"
         "  noperspective float fog : FOG; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1;\n"
         "  float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; };\n"
         "float4 ps(PI i) : SV_Target {\n"
         "  float4 o;\n");

    if (!k->use_rc) {
        /* Before the title programs the combiners: stage-0 texel * diffuse,
         * texture coordinates taken as they are (raster_xf_triangle). */
        bput(&b, "  o = i.d0;\n");
        if (k->textured) {
            if (k->kind[0] == 1)
                bput(&b, "  { float4 t = T0.Sample(S0, i.t0.xy * TS[0].xy);%s o *= t; }\n",
                     k->swap_rb[0] ? " t.rgb = t.bgr;" : "");
        }
        bput(&b, "  o = saturate(o);\n");
    } else {
        uint32_t n = k->control & 0xFF, flags = k->control >> 8, s;
        float dummy = 0; (void)dummy;
        if (n > 8) n = 8;
        bput(&b,
             "  float4 rz = 0, r6 = 0, r7 = 0, r1 = 0, sm = 0, ef = 0, c0, c1;\n"
             "  float4 v0 = i.d0, v1 = i.d1, fogr = float4(FOGC.rgb, i.fog);\n"
             "  float4 t0 = float4(0,0,0,1), t1 = t0, t2 = t0, t3 = t0;\n");
        /* Texture stages, by SHADER_STAGE_PROGRAM mode (rc_stage_fetch). */
        for (st = 0; st < 4; st++) {
            uint32_t mode = (k->stage_program >> (st * 5)) & 0x1F;
            const char *tc = st == 0 ? "i.t0" : st == 1 ? "i.t1" : st == 2 ? "i.t2" : "i.t3";
            switch (mode) {
            case 0:
                break;
            case 1: case 2:
                if (!k->kind[st]) { bput(&b, "  t%d = 1;\n", st); break; }
                bput(&b, "  { float q = %s.w != 0 ? %s.w : 1;\n", tc, tc);
                if (k->kind[st] == 2)
                    bput(&b, "    t%d = T%d.Sample(S%d, float3(%s.xy / q, 1));", st, st, st, tc);
                else
                    bput(&b, "    t%d = T%d.Sample(S%d, %s.xy / q * TS[%d].xy);", st, st, st, tc, st);
                bput(&b, "%s }\n", k->swap_rb[st] ? (st == 0 ? " t0.rgb = t0.bgr;" :
                                                     st == 1 ? " t1.rgb = t1.bgr;" :
                                                     st == 2 ? " t2.rgb = t2.bgr;" : " t3.rgb = t3.bgr;") : "");
                break;
            case 3:
                if (!k->kind[st]) { bput(&b, "  t%d = 1;\n", st); break; }
                if (k->kind[st] == 2)
                    bput(&b, "  t%d = T%d.Sample(S%d, %s.xyz);\n", st, st, st, tc);
                else
                    bput(&b, "  t%d = T%d.Sample(S%d, %s.xy * TS[%d].xy);\n", st, st, st, tc, st);
                break;
            case 4:
                bput(&b, "  t%d = saturate(%s);\n", st, tc);
                break;
            case 5: {
                int j;
                bput(&b, "  t%d = 0;\n", st);
                for (j = 0; j < 4; j++) {
                    int ge = (k->clip_plane_mode >> (st * 4 + j)) & 1;
                    bput(&b, "  if (%s[%d] %s 0) discard;\n", tc, j, ge ? ">=" : "<");
                }
                break;
            }
            default:
                bput(&b, "  t%d = 0;\n", st);
                break;
            }
        }
        if (k->dbg & 1)
            for (st = 0; st < 4; st++)
                if (!((k->stage_program >> (st * 5)) & 0x1F))
                    bput(&b, "  t%d = 0;\n", st);
        bput(&b, "  float4 r0 = 0; r0.a = %s;\n", (k->stage_program & 0x1F) ? "t0.a" :
             (k->dbg & 2) ? "0" : "1");

        for (s = 0; s < n; s++) {
            uint32_t icw = k->color_icw[s], ocw = k->color_ocw[s];
            uint32_t aicw = k->alpha_icw[s], aocw = k->alpha_ocw[s];
            uint32_t fl = ocw >> 12, afl = aocw >> 12, d;
            bput(&b, "  { c0 = F0[%u]; c1 = F1[%u];\n",
                 (flags & 0x010) ? s : 0, (flags & 0x100) ? s : 0);
            if (flags & 1)
                bput(&b, "    bool mux = r0.a >= 0.5;\n");
            else
                bput(&b, "    bool mux = (((int)(r0.a * 255.0)) & 1) != 0;\n");
            bput(&b, "    float3 A = "); emit_rgb(&b, icw >> 24);
            bput(&b, ", B = ");          emit_rgb(&b, icw >> 16);
            bput(&b, ", C = ");          emit_rgb(&b, icw >> 8);
            bput(&b, ", D = ");          emit_rgb(&b, icw);
            bput(&b, ";\n    float aA = "); emit_alpha(&b, aicw >> 24);
            bput(&b, ", aB = ");            emit_alpha(&b, aicw >> 16);
            bput(&b, ", aC = ");            emit_alpha(&b, aicw >> 8);
            bput(&b, ", aD = ");            emit_alpha(&b, aicw);
            bput(&b, ";\n");
            bput(&b, "    float3 pab = %s, pcd = %s;\n",
                 (fl & 2) ? "dot(A,B).xxx" : "A*B", (fl & 1) ? "dot(C,D).xxx" : "C*D");
            bput(&b, "    float3 psum = %s;\n", (fl & 4) ? "mux ? pcd : pab" : "pab + pcd");
            bput(&b, "    float3 ab = "); emit_mapout(&b, "pab", fl & 0x38);
            bput(&b, ", cd = ");          emit_mapout(&b, "pcd", fl & 0x38);
            bput(&b, ", ms = ");          emit_mapout(&b, "psum", fl & 0x38);
            bput(&b, ";\n    float apab = aA*aB, apcd = aC*aD, apsum = %s;\n",
                 (afl & 4) ? "mux ? apcd : apab" : "apab + apcd");
            bput(&b, "    float aab = "); emit_mapout(&b, "apab", afl & 0x38);
            bput(&b, ", acd = ");         emit_mapout(&b, "apcd", afl & 0x38);
            bput(&b, ", ams = ");         emit_mapout(&b, "apsum", afl & 0x38);
            bput(&b, ";\n");
            /* Writes, in nv2a_rc_eval's order; register 0 stays zero. */
            if ((d = (ocw >> 4) & 0xF) != 0) {
                bput(&b, "    %s.rgb = ab;", REG[d]);
                if (fl & 0x80) bput(&b, " %s.a = ab.b;", REG[d]);
                bput(&b, "\n");
            }
            if ((d = ocw & 0xF) != 0) {
                bput(&b, "    %s.rgb = cd;", REG[d]);
                if (fl & 0x40) bput(&b, " %s.a = cd.b;", REG[d]);
                bput(&b, "\n");
            }
            if ((d = (ocw >> 8) & 0xF) != 0)
                bput(&b, "    %s.rgb = ms;\n", REG[d]);
            if ((d = (aocw >> 4) & 0xF) != 0) bput(&b, "    %s.a = aab;\n", REG[d]);
            if ((d = aocw & 0xF) != 0)        bput(&b, "    %s.a = acd;\n", REG[d]);
            if ((d = (aocw >> 8) & 0xF) != 0) bput(&b, "    %s.a = ams;\n", REG[d]);
            bput(&b, "  }\n");
        }

        if (k->final0 || k->final1) {
            uint32_t f0 = k->final0, f1 = k->final1, ff = f1 & 0xFF;
            bput(&b, "  c0 = FC0; c1 = FC1;\n");
            bput(&b, "  { float3 sa = %s, sb = %s; sm = float4(%s, 0); }\n",
                 (ff & 0x40) ? "1 - v1.rgb" : "v1.rgb",
                 (ff & 0x20) ? "1 - r0.rgb" : "r0.rgb",
                 (ff & 0x80) ? "saturate(sa + sb)" : "sa + sb");
            bput(&b, "  { float3 E = "); emit_rgb(&b, f1 >> 24);
            bput(&b, ", F = ");          emit_rgb(&b, f1 >> 16);
            bput(&b, "; ef = float4(E * F, 0); }\n");
            bput(&b, "  { float3 A = "); emit_rgb(&b, f0 >> 24);
            bput(&b, ", B = ");          emit_rgb(&b, f0 >> 16);
            bput(&b, ", C = ");          emit_rgb(&b, f0 >> 8);
            bput(&b, ", D = ");          emit_rgb(&b, f0);
            bput(&b, ";\n    o.rgb = D + C * (1 - A) + B * A; o.a = ");
            emit_alpha(&b, f1 >> 8);
            bput(&b, "; }\n");
        } else {
            bput(&b, "  o = r0;\n");
        }
        bput(&b, "  o = saturate(o);\n");
    }
    if (k->alpha_test) {
        const char *op = cmp_op(k->alpha_func);
        if (k->alpha_func == 0x200)
            bput(&b, "  discard;\n");
        else if (op)
            bput(&b, "  if (!((int)(o.a * 255.0 + 0.5) %s AREF.x)) discard;\n", op);
    }
    bput(&b, "  return o;\n}\n");
    return b.p;
}

static ID3D11PixelShader *get_ps(const PsKey *k)
{
    uint64_t h = hash_bytes((const uint8_t *)k, sizeof *k, 7);
    int i;
    char *src;
    ID3DBlob *blob;
    ID3D11PixelShader *ps = NULL;

    for (i = 0; i < s_nps; i++)
        if (s_ps[i].h == h && !memcmp(&s_ps[i].k, k, sizeof *k))
            return s_ps[i].ps;
    src = gen_ps(k);
    if (getenv("RECOMP_GPU_PS_DUMP"))
        fprintf(stderr, "[GPU-D3D11] pixel shader %d: rc %u stages %05X kinds %u%u%u%u control %08X"
                " final %08X %08X\n%s\n", s_nps, k->use_rc, k->stage_program, k->kind[0], k->kind[1],
                k->kind[2], k->kind[3], k->control, k->final0, k->final1, src);
    blob = compile(src, "ps", "ps_4_0");
    if (blob) {
        ID3D11Device_CreatePixelShader(D, ID3D10Blob_GetBufferPointer(blob),
                                       ID3D10Blob_GetBufferSize(blob), NULL, &ps);
        ID3D10Blob_Release(blob);
    }
    free(src);
    if (s_nps == s_cap_ps) {
        s_cap_ps = s_cap_ps ? s_cap_ps * 2 : 64;
        s_ps = (PsEnt *)realloc(s_ps, (size_t)s_cap_ps * sizeof *s_ps);
    }
    s_ps[s_nps].h = h;
    s_ps[s_nps].k = *k;
    s_ps[s_nps].ps = ps;            /* NULL on failure: remembered, not retried */
    s_nps++;
    S.shaders++;
    return ps;
}

/* ---- render targets and depth buffers ----------------------------------- */

/* w, h: the target in the title's pixels; pw, ph: as allocated, times the
 * internal-resolution scale (and 4/3 wider for a screen-sized target in
 * widescreen: the title squeezes a 16:9 view into its 4:3 frame, so the
 * extra columns make the stretched picture as sharp across as down).
 * Everything the title sees (texture coordinates, the vertex mapping) uses
 * w, h; the viewport, scissor, depth buffer and readback use pw, ph.
 *
 * With multisampling, drawing goes into `ms` (through rtv) and t is its
 * resolved copy, brought up to date (rt_resolve) before anything reads it:
 * sampling as a texture, presenting, the copy for guest memory. */
typedef struct {
    uint32_t va, w, h, swizzled, pw, ph;
    ID3D11Texture2D *t, *ms;
    ID3D11RenderTargetView *rtv;          /* drawn into: ms's view, or rrtv */
    ID3D11RenderTargetView *rrtv;         /* t's own view                   */
    ID3D11ShaderResourceView *srv;
    uint32_t used;
    int dirty;                            /* ms drawn since the last resolve */
    int wide;                             /* pw is 4/3 of w * scale         */
    uint32_t bytes;                       /* guest memory it covers         */
    uint64_t ghash;                       /* that memory when last drawn    */
    int shown;                            /* presented: guest memory gets it */
} RtEnt;

static uint32_t s_scale = 1;             /* internal resolution, applied */
static uint32_t s_msaa = 1;              /* samples per pixel, applied   */
static int s_wide;                       /* widescreen, applied          */
/* Widescreen overlays: whether the frame drawn before this one had 3D in it
 * (then a full-screen flat quad is an effect over the scene and stays full
 * width; in a menu it is a backdrop and keeps its shape like the rest). */
static int s_world_cur, s_world_prev;
/* The current draw's clip box is a small inset (a portrait in the HUD):
 * how far it moves sideways, title pixels, to stay with the HUD around it. */
static float s_inset_dx;

typedef struct {
    uint32_t va, w, h;
    ID3D11Texture2D *t;
    ID3D11DepthStencilView *dsv;
    uint32_t used;
} DsEnt;

#define MAX_RT 64
static RtEnt s_rt[MAX_RT];
static DsEnt s_ds[16];

static void k_reset(void);
static void free_rt(RtEnt *e)
{
    k_reset();
    if (e->srv) ID3D11ShaderResourceView_Release(e->srv);
    if (e->rtv && e->rtv != e->rrtv) ID3D11RenderTargetView_Release(e->rtv);
    if (e->rrtv) ID3D11RenderTargetView_Release(e->rrtv);
    if (e->ms) ID3D11Texture2D_Release(e->ms);
    if (e->t) ID3D11Texture2D_Release(e->t);
    memset(e, 0, sizeof *e);
}

/* plain: 1x, one sample, never widened (the copy for guest memory). */
static int make_rt_ex(RtEnt *e, uint32_t w, uint32_t h, int plain)
{
    D3D11_TEXTURE2D_DESC td;
    uint32_t sc = plain ? 1 : s_scale;
    /* Screen-shaped (4:3 to 3:2, full size) targets only: a square or small
     * one is a texture the title draws, not the picture. */
    int wide = !plain && s_wide && h >= 384 && w * 4 >= h * 5 && w * 2 <= h * 3;
    memset(e, 0, sizeof *e);
    e->w = w; e->h = h; e->wide = wide;
    e->pw = wide ? (w * sc * 4 + 1) / 3 : w * sc;
    e->ph = h * sc;
    memset(&td, 0, sizeof td);
    td.Width = e->pw; td.Height = e->ph; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(D, &td, NULL, &e->t)))
        return 0;
    ID3D11Device_CreateRenderTargetView(D, (ID3D11Resource *)e->t, NULL, &e->rrtv);
    ID3D11Device_CreateShaderResourceView(D, (ID3D11Resource *)e->t, NULL, &e->srv);
    e->rtv = e->rrtv;
    if (!plain && s_msaa > 1) {
        td.SampleDesc.Count = s_msaa;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (SUCCEEDED(ID3D11Device_CreateTexture2D(D, &td, NULL, &e->ms))
            && FAILED(ID3D11Device_CreateRenderTargetView(D, (ID3D11Resource *)e->ms, NULL, &e->rtv))) {
            ID3D11Texture2D_Release(e->ms);
            e->ms = NULL;
        }
        if (!e->ms)
            e->rtv = e->rrtv;
    }
    return 1;
}

static int make_rt(RtEnt *e, uint32_t w, uint32_t h) { return make_rt_ex(e, w, h, 0); }

/* The multisampled pixels into t, if drawn since last time. */
static void rt_resolve(RtEnt *e)
{
    if (e->ms && e->dirty) {
        ID3D11DeviceContext_ResolveSubresource(C, (ID3D11Resource *)e->t, 0, (ID3D11Resource *)e->ms,
                                               0, DXGI_FORMAT_B8G8R8A8_UNORM);
        e->dirty = 0;
    }
}

static void blit(ID3D11RenderTargetView *rtv, ID3D11ShaderResourceView *srv,
                 float u, float v, float x, float y, float w, float h,
                 ID3D11SamplerState *smp, ID3D11BlendState *bs);
static int blit_init(void);
static ID3D11SamplerState *s_blit_pt;

/* t written directly (seeded, carried over): the same into ms. */
static void rt_push(RtEnt *e)
{
    if (e->ms && blit_init())
        blit(e->rtv, e->srv, 1.0f, 1.0f, 0, 0, (float)e->pw, (float)e->ph, s_blit_pt, NULL);
    e->dirty = 0;
}

/* A new render target starts as what guest memory holds there, so a surface
 * the CPU prepared (or the software path drew into before) carries over. */
static void rt_seed_from_guest(RtEnt *e, const Nv2aGpuState *st)
{
    uint32_t w = e->w, h = e->h, y, x;
    uint32_t *px;
    const uint8_t *src = gmem() + st->color_va;
    if (st->swizzled || !st->pitch || (st->bpp != 4 && st->bpp != 2))
        return;
    if (w * st->bpp > st->pitch)
        w = st->pitch / st->bpp;
    px = (uint32_t *)malloc((size_t)e->w * e->h * 4);
    if (!px)
        return;
    memset(px, 0, (size_t)e->w * e->h * 4);
    for (y = 0; y < h; y++) {
        const uint8_t *row = src + (size_t)y * st->pitch;
        if (st->bpp == 4)
            memcpy(px + (size_t)y * e->w, row, (size_t)w * 4);
        else
            for (x = 0; x < w; x++) {
                uint32_t t = ((const uint16_t *)row)[x];
                px[(size_t)y * e->w + x] = 0xFF000000u | ((t & 0xF800u) << 8)
                    | ((t & 0x07E0u) << 5) | ((t & 0x001Fu) << 3);
            }
    }
    if (e->pw != e->w || e->ph != e->h) {
        /* Each texel repeated to fill the bigger target. */
        uint32_t *big = (uint32_t *)malloc((size_t)e->pw * e->ph * 4), sx;
        if (big) {
            for (y = 0; y < e->ph; y++)
                for (sx = 0; sx < e->pw; sx++)
                    big[(size_t)y * e->pw + sx] = px[(size_t)(y * e->h / e->ph) * e->w
                                                     + (size_t)sx * e->w / e->pw];
            ID3D11DeviceContext_UpdateSubresource(C, (ID3D11Resource *)e->t, 0, NULL, big,
                                                  e->pw * 4, 0);
            free(big);
        }
    } else {
        ID3D11DeviceContext_UpdateSubresource(C, (ID3D11Resource *)e->t, 0, NULL, px,
                                              e->w * 4, 0);
    }
    free(px);
    rt_push(e);
}

/* Guest memory under a target, sampled. The GPU path never writes it (but
 * for a presented frame), so if it changed, the CPU put something else
 * there: a texture loaded where an old target was. */
static uint64_t rt_mem_hash(const RtEnt *e)
{
    const uint8_t *p = gmem() + e->va;
    uint32_t n = e->bytes, k, step;
    uint64_t h = 0x9E37u;
    if (n < 64) return 0;
    step = (n - 32) / 63;
    for (k = 0; k < 64; k++)
        h = hash_bytes(p + (size_t)k * step, 32, h);
    return h;
}

static RtEnt *find_rt(uint32_t va)
{
    int i;
    for (i = 0; i < MAX_RT; i++)
        if (s_rt[i].t && s_rt[i].va == va)
            return &s_rt[i];
    return NULL;
}

static RtEnt *get_rt(const Nv2aGpuState *st)
{
    uint32_t w, h;
    RtEnt *e = find_rt(st->color_va);
    int i, oldest = 0;

    if (st->swizzled) {
        w = st->swz_w; h = st->swz_h;
    } else {
        w = st->bpp ? st->pitch / st->bpp : 0;
        h = st->clip_y + st->clip_h;
        if (w < st->clip_x + st->clip_w) w = st->clip_x + st->clip_w;
    }
    if (!w || !h || w > 4096 || h > 4096)
        return NULL;
    if (e) {
        if (e->w >= w && e->h >= h && e->swizzled == st->swizzled) {
            e->used = s_frame;
            return e;
        }
        /* Grown (a taller clip than before): a bigger target, old pixels
         * carried over. */
        {
            RtEnt n;
            memset(&n, 0, sizeof n);
            if (w < e->w) w = e->w;
            if (h < e->h) h = e->h;
            if (!make_rt(&n, w, h))
                return e;
            if (e->swizzled == st->swizzled && n.pw >= e->pw && n.ph >= e->ph) {
                D3D11_BOX box = { 0, 0, 0, e->pw, e->ph, 1 };
                rt_resolve(e);
                ID3D11DeviceContext_CopySubresourceRegion(C, (ID3D11Resource *)n.t, 0, 0, 0, 0,
                                                          (ID3D11Resource *)e->t, 0, &box);
                rt_push(&n);
            }
            free_rt(e);
            *e = n;
            e->va = st->color_va;
            e->swizzled = st->swizzled;
            e->used = s_frame;
            e->bytes = (st->swizzled ? w * (st->bpp ? st->bpp : 4) : st->pitch) * h;
            return e;
        }
    }
    for (i = 0; i < MAX_RT; i++) {
        if (!s_rt[i].t) { oldest = i; break; }
        if (s_rt[i].used < s_rt[oldest].used) oldest = i;
    }
    e = &s_rt[oldest];
    if (e->t)
        free_rt(e);
    if (!make_rt(e, w, h))
        return NULL;
    e->va = st->color_va;
    e->swizzled = st->swizzled;
    e->used = s_frame;
    e->bytes = (st->swizzled ? w * (st->bpp ? st->bpp : 4) : st->pitch) * h;
    rt_seed_from_guest(e, st);
    return e;
}

static DsEnt *get_ds(uint32_t va, uint32_t w, uint32_t h)
{
    int i, slot = -1, oldest = 0;
    D3D11_TEXTURE2D_DESC td;

    for (i = 0; i < 16; i++) {
        if (s_ds[i].t && s_ds[i].va == va) {
            if (s_ds[i].w >= w && s_ds[i].h >= h) {
                s_ds[i].used = s_frame;
                return &s_ds[i];
            }
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (i = 0; i < 16; i++) {
            if (!s_ds[i].t) { oldest = i; break; }
            if (s_ds[i].used < s_ds[oldest].used) oldest = i;
        }
        slot = oldest;
    }
    if (s_ds[slot].t) {
        if (w < s_ds[slot].w) w = s_ds[slot].w;
        if (h < s_ds[slot].h) h = s_ds[slot].h;
        k_reset();
        ID3D11DepthStencilView_Release(s_ds[slot].dsv);
        ID3D11Texture2D_Release(s_ds[slot].t);
    }
    memset(&s_ds[slot], 0, sizeof s_ds[slot]);
    memset(&td, 0, sizeof td);
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.SampleDesc.Count = s_msaa;          /* as every target drawn with it */
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(ID3D11Device_CreateTexture2D(D, &td, NULL, &s_ds[slot].t)))
        return NULL;
    ID3D11Device_CreateDepthStencilView(D, (ID3D11Resource *)s_ds[slot].t, NULL, &s_ds[slot].dsv);
    s_ds[slot].va = va; s_ds[slot].w = w; s_ds[slot].h = h; s_ds[slot].used = s_frame;
    /* Fresh depth: cleared to the far plane and stencil 0, as a title that
     * never clears would find the CPU path's buffer. */
    ID3D11DeviceContext_ClearDepthStencilView(C, s_ds[slot].dsv,
        D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    return &s_ds[slot];
}

/* ---- textures ----------------------------------------------------------- */

typedef struct {
    uint32_t va, fmt, w, h, pitch, palette, cube, bytes;
    uint64_t hash, shash;             /* whole texture; sampled (tex_sparse_hash) */
    uint32_t checked, used;
    uint32_t tw, th;                  /* as created: a replacement may differ */
    ID3D11Texture2D *t;
    ID3D11ShaderResourceView *srv;
} TexEnt;

#define TEX_SLOTS 1024
static TexEnt s_tex[TEX_SLOTS];
static uint32_t *s_decode;
static size_t s_decode_cap;

static int tex_same(const TexEnt *e, const Nv2aGpuTexture *t)
{
    return e->t && e->va == t->offset && e->fmt == t->color && e->w == t->width
        && e->h == t->height && e->pitch == t->pitch && e->palette == t->palette
        && e->cube == t->cube;
}

static uint64_t tex_content_hash(const Nv2aGpuTexture *t)
{
    uint64_t h = hash_bytes(gmem() + t->offset, t->bytes, t->color);
    if (t->palette && t->color == 0x0B)
        h ^= hash_bytes(gmem() + t->palette, 1024, 0xC1u);
    return h;
}

/* ---- texture dump and replacement (F6 menu) -------------------------------
 *
 * Every texture the title uploads has a name: the hash of its bytes (and
 * palette), its size and its NV2A format -- the same name wherever in memory
 * and whenever it is loaded. Dumping writes each one once to
 * textures\dump\<name>.png; replacing uses textures\replace\<name>.png
 * instead, at any resolution (coordinates are normalised, so a 4x larger
 * replacement simply has 4x the detail). PNG through WIC, which every Windows
 * has. */
static IWICImagingFactory *s_wic;

static int wic_ready(void)
{
    static int tried;
    static const GUID clsid = { 0xcacaf262, 0x9370, 0x4615, { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a } };
    static const GUID iid = { 0xec5ec8a9, 0xc395, 0x4314, { 0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70 } };
    if (!tried) {
        tried = 1;
        CoInitializeEx(NULL, COINIT_MULTITHREADED);
        if (FAILED(CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER, &iid, (void **)&s_wic)))
            s_wic = NULL;
        if (!s_wic)
            fprintf(stderr, "[GPU-D3D11] WIC unavailable: no texture dump/replace\n");
    }
    return s_wic != NULL;
}

static const GUID k_png = { 0x1b7cfaf4, 0x713f, 0x473c, { 0xbb, 0xcd, 0x61, 0x37, 0x42, 0x5f, 0xae, 0xaf } };
static const GUID k_bgra = { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0f } };

static int png_write_rows(const wchar_t *path, const uint32_t *px, uint32_t w, uint32_t h);
static int png_write(const wchar_t *path, const uint32_t *px, uint32_t w, uint32_t h)
{
    /* This title stores texture rows bottom to top: flipped, the dump opens
     * the right way up, and png_read flips a replacement back. */
    uint32_t *f = (uint32_t *)malloc((size_t)w * h * 4), y;
    int ok;
    if (!f) return 0;
    for (y = 0; y < h; y++)
        memcpy(f + (size_t)y * w, px + (size_t)(h - 1 - y) * w, (size_t)w * 4);
    ok = png_write_rows(path, f, w, h);
    free(f);
    return ok;
}

static int png_write_rows(const wchar_t *path, const uint32_t *px, uint32_t w, uint32_t h)
{
    IWICStream *st = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *fr = NULL;
    IPropertyBag2 *props = NULL;
    WICPixelFormatGUID fmt = k_bgra;
    int ok = 0;

    if (!wic_ready()) return 0;
    if (SUCCEEDED(IWICImagingFactory_CreateStream(s_wic, &st))
        && SUCCEEDED(IWICStream_InitializeFromFilename(st, path, GENERIC_WRITE))
        && SUCCEEDED(IWICImagingFactory_CreateEncoder(s_wic, &k_png, NULL, &enc))
        && SUCCEEDED(IWICBitmapEncoder_Initialize(enc, (IStream *)st, WICBitmapEncoderNoCache))
        && SUCCEEDED(IWICBitmapEncoder_CreateNewFrame(enc, &fr, &props))
        && SUCCEEDED(IWICBitmapFrameEncode_Initialize(fr, props))
        && SUCCEEDED(IWICBitmapFrameEncode_SetSize(fr, w, h))
        && SUCCEEDED(IWICBitmapFrameEncode_SetPixelFormat(fr, &fmt))
        && SUCCEEDED(IWICBitmapFrameEncode_WritePixels(fr, h, w * 4, w * h * 4, (BYTE *)px))
        && SUCCEEDED(IWICBitmapFrameEncode_Commit(fr))
        && SUCCEEDED(IWICBitmapEncoder_Commit(enc)))
        ok = 1;
    if (fr) IWICBitmapFrameEncode_Release(fr);
    if (props) IPropertyBag2_Release(props);
    if (enc) IWICBitmapEncoder_Release(enc);
    if (st) IWICStream_Release(st);
    return ok;
}

static uint32_t *png_read(const wchar_t *path, uint32_t *w, uint32_t *h)
{
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *fr = NULL;
    IWICFormatConverter *cv = NULL;
    uint32_t *px = NULL;
    UINT ww = 0, hh = 0;

    if (!wic_ready()) return NULL;
    if (SUCCEEDED(IWICImagingFactory_CreateDecoderFromFilename(s_wic, path, NULL, GENERIC_READ,
                                                               WICDecodeMetadataCacheOnDemand, &dec))
        && SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &fr))
        && SUCCEEDED(IWICImagingFactory_CreateFormatConverter(s_wic, &cv))
        && SUCCEEDED(IWICFormatConverter_Initialize(cv, (IWICBitmapSource *)fr, &k_bgra,
                                                    WICBitmapDitherTypeNone, NULL, 0,
                                                    WICBitmapPaletteTypeCustom))
        && SUCCEEDED(IWICFormatConverter_GetSize(cv, &ww, &hh))
        && ww && hh && ww <= 16384 && hh <= 16384) {
        px = (uint32_t *)malloc((size_t)ww * hh * 4);
        if (px && FAILED(IWICFormatConverter_CopyPixels(cv, NULL, ww * 4, ww * hh * 4, (BYTE *)px))) {
            free(px);
            px = NULL;
        }
        if (px) {                         /* back to the title's row order */
            uint32_t y, *row = (uint32_t *)malloc((size_t)ww * 4);
            for (y = 0; row && y < hh / 2; y++) {
                memcpy(row, px + (size_t)y * ww, (size_t)ww * 4);
                memcpy(px + (size_t)y * ww, px + (size_t)(hh - 1 - y) * ww, (size_t)ww * 4);
                memcpy(px + (size_t)(hh - 1 - y) * ww, row, (size_t)ww * 4);
            }
            free(row);
        }
    }
    if (cv) IWICFormatConverter_Release(cv);
    if (fr) IWICBitmapFrameDecode_Release(fr);
    if (dec) IWICBitmapDecoder_Release(dec);
    *w = ww; *h = hh;
    return px;
}

static void tex_name(wchar_t *out, size_t n, const wchar_t *dir, const TexEnt *e)
{
    _snwprintf(out, n, L"textures\\%ls\\%016llX_%ux%u_%02X.png", dir,
               (unsigned long long)e->hash, e->w, e->h, e->fmt);
    out[n - 1] = 0;
}

/* The replacement folder's names, by hash, read once per reload. */
static uint64_t *s_repl;
static int s_nrepl;
/* Hashes already dumped this run (and on disk from earlier runs). */
static uint64_t *s_dumped;
static int s_ndumped, s_cap_dumped;

static int hash_in(const uint64_t *v, int n, uint64_t h)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (v[mid] == h) return 1;
        if (v[mid] < h) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static int scan_dir(const char *pattern, uint64_t **out)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    int n = 0, cap = 0;
    uint64_t *v = NULL;
    if (h == INVALID_HANDLE_VALUE) { *out = NULL; return 0; }
    do {
        unsigned long long x;
        if (sscanf(fd.cFileName, "%16llX", &x) == 1) {
            if (n == cap) { cap = cap ? cap * 2 : 256; v = (uint64_t *)realloc(v, cap * sizeof *v); }
            v[n++] = x;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    qsort(v, n, sizeof *v, cmp_u64);
    *out = v;
    return n;
}

static void repl_scan(void)
{
    XboxGfxConfig *g = xbox_GfxConfig();
    free(s_repl);
    s_nrepl = scan_dir("textures\\replace\\*.png", &s_repl);
    g->replacements_found = s_nrepl;
    g->replaced = 0;
}

static void dump_texture(TexEnt *e, const uint32_t *px)
{
    XboxGfxConfig *g = xbox_GfxConfig();
    wchar_t path[MAX_PATH];
    static int dirs, scanned;
    if (!scanned) {
        scanned = 1;
        s_ndumped = scan_dir("textures\\dump\\*.png", &s_dumped);
        s_cap_dumped = s_ndumped;
    }
    if (hash_in(s_dumped, s_ndumped, e->hash))
        return;
    if (!dirs) { dirs = 1; CreateDirectoryA("textures", NULL); CreateDirectoryA("textures\\dump", NULL); }
    tex_name(path, MAX_PATH, L"dump", e);
    if (png_write(path, px, e->w, e->h))
        g->dumped++;
    /* Remembered either way: a failed write is not retried every frame. */
    if (s_ndumped == s_cap_dumped) {
        s_cap_dumped = s_cap_dumped ? s_cap_dumped * 2 : 256;
        s_dumped = (uint64_t *)realloc(s_dumped, s_cap_dumped * sizeof *s_dumped);
    }
    {
        int k = s_ndumped++;
        while (k > 0 && s_dumped[k - 1] > e->hash) { s_dumped[k] = s_dumped[k - 1]; k--; }
        s_dumped[k] = e->hash;
    }
}

static int tex_upload(TexEnt *e, const Nv2aGpuTexture *t)
{
    XboxGfxConfig *g = xbox_GfxConfig();
    size_t n = (size_t)t->width * t->height;
    uint32_t faces = t->cube ? 6 : 1, f, tw = t->width, th = t->height;
    D3D11_SUBRESOURCE_DATA sd[6];
    D3D11_TEXTURE2D_DESC td;
    const uint32_t *px;
    uint32_t *repl = NULL;

    if (n * faces > s_decode_cap) {
        free(s_decode);
        s_decode_cap = n * faces;
        s_decode = (uint32_t *)malloc(s_decode_cap * 4);
        if (!s_decode) { s_decode_cap = 0; return 0; }
    }
    for (f = 0; f < faces; f++)
        if (!nv2a_gpu_decode_texture(t, f, s_decode + n * f))
            return 0;
    S.tex_uploads++;
    S.tex_bytes += n * faces * 4;
    px = s_decode;
    if (!t->cube) {
        /* First upload only: a texture rewritten every frame (a movie, a
         * scrolling effect) is saved once, not once a frame. */
        if (g->tex_dump && !e->t)
            dump_texture(e, s_decode);
        if (g->tex_replace && hash_in(s_repl, s_nrepl, e->hash)) {
            wchar_t path[MAX_PATH];
            uint32_t rw, rh;
            tex_name(path, MAX_PATH, L"replace", e);
            repl = png_read(path, &rw, &rh);
            if (repl) {
                px = repl; tw = rw; th = rh;
                g->replaced++;
            }
        }
    }
    if (e->t && e->tw == tw && e->th == th) {
        /* Same shape, new content: update in place. */
        for (f = 0; f < faces; f++)
            ID3D11DeviceContext_UpdateSubresource(C, (ID3D11Resource *)e->t, f, NULL,
                                                  px + (size_t)tw * th * f, tw * 4, 0);
        free(repl);
        return 1;
    }
    if (e->t) {
        k_reset();
        if (e->srv) ID3D11ShaderResourceView_Release(e->srv);
        ID3D11Texture2D_Release(e->t);
        e->t = NULL; e->srv = NULL;
    }
    memset(&td, 0, sizeof td);
    td.Width = tw; td.Height = th; td.MipLevels = 1;
    td.ArraySize = faces;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = t->cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
    for (f = 0; f < faces; f++) {
        sd[f].pSysMem = px + (size_t)tw * th * f;
        sd[f].SysMemPitch = tw * 4;
        sd[f].SysMemSlicePitch = 0;
    }
    if (FAILED(ID3D11Device_CreateTexture2D(D, &td, sd, &e->t))) {
        e->t = NULL;
        free(repl);
        return 0;
    }
    ID3D11Device_CreateShaderResourceView(D, (ID3D11Resource *)e->t, NULL, &e->srv);
    e->tw = tw; e->th = th;
    free(repl);
    return 1;
}

static void tex_cache_clear(void)
{
    int i;
    k_reset();
    for (i = 0; i < TEX_SLOTS; i++) {
        if (s_tex[i].srv) ID3D11ShaderResourceView_Release(s_tex[i].srv);
        if (s_tex[i].t) ID3D11Texture2D_Release(s_tex[i].t);
        memset(&s_tex[i], 0, sizeof s_tex[i]);
    }
}

/* 64 spread 32-byte pieces of the texture (and its palette): what the
 * per-frame check reads. The whole texture is hashed when they change, and
 * anyway once every 16 frames, staggered by slot, so an edit between the
 * samples is picked up within a quarter of a second. Hashing every texture
 * in full every frame was megabytes a frame on the engine thread. */
static uint64_t tex_sparse_hash(const Nv2aGpuTexture *t)
{
    const uint8_t *p = gmem() + t->offset;
    size_t n = t->bytes, step, k;
    uint64_t h = t->color;
    if (n <= 4096)
        h = hash_bytes(p, n, h);
    else {
        step = (n - 32) / 63;
        for (k = 0; k < 64; k++)
            h = hash_bytes(p + k * step, 32, h);
    }
    if (t->palette && t->color == 0x0B)
        h ^= hash_bytes(gmem() + t->palette, 1024, 0xC1u);
    return h;
}

static ID3D11ShaderResourceView *get_tex(const Nv2aGpuTexture *t)
{
    uint32_t h0 = (t->offset >> 6) ^ (t->offset >> 17) ^ (t->color * 131u) ^ (t->width << 3);
    uint32_t i, slot = TEX_SLOTS, oldest = TEX_SLOTS;

    if (!t->width || !t->height || t->width > 4096 || t->height > 4096)
        return NULL;
    for (i = 0; i < 16; i++) {
        uint32_t k = (h0 + i) & (TEX_SLOTS - 1);
        if (tex_same(&s_tex[k], t)) { slot = k; break; }
        if (!s_tex[k].t && slot == TEX_SLOTS) slot = k;
        if (oldest == TEX_SLOTS || s_tex[k].used < s_tex[oldest].used) oldest = k;
    }
    if (slot == TEX_SLOTS)
        slot = oldest;
    {
        TexEnt *e = &s_tex[slot];
        if (!tex_same(e, t)) {
            k_reset();
            if (e->srv) ID3D11ShaderResourceView_Release(e->srv);
            if (e->t) ID3D11Texture2D_Release(e->t);
            memset(e, 0, sizeof *e);
            e->va = t->offset; e->fmt = t->color; e->w = t->width; e->h = t->height;
            e->pitch = t->pitch; e->palette = t->palette; e->cube = t->cube;
            e->bytes = t->bytes;
            e->hash = tex_content_hash(t);
            e->shash = tex_sparse_hash(t);
            e->checked = s_frame;
            if (!tex_upload(e, t)) {
                if (e->t) ID3D11Texture2D_Release(e->t);
                memset(e, 0, sizeof *e);
                return NULL;
            }
        } else if (e->checked != s_frame) {
            uint64_t sh = tex_sparse_hash(t);
            e->checked = s_frame;
            if (sh != e->shash || ((s_frame + slot) & 15) == 0) {
                uint64_t h = tex_content_hash(t);
                e->shash = sh;
                if (h != e->hash) {
                    e->hash = h;
                    tex_upload(e, t);
                }
            }
        }
        e->used = s_frame;
        return e->srv;
    }
}

/* ---- fixed-function state ------------------------------------------------ */

static D3D11_BLEND blend_of(uint32_t f, int alpha)
{
    switch (f) {
    case 0:      return D3D11_BLEND_ZERO;
    case 1:      return D3D11_BLEND_ONE;
    case 0x300:  return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 0x301:  return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 0x302:  return D3D11_BLEND_SRC_ALPHA;
    case 0x303:  return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x304:  return D3D11_BLEND_DEST_ALPHA;
    case 0x305:  return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x306:  return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 0x307:  return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 0x308:  return D3D11_BLEND_SRC_ALPHA_SAT;   /* source only: see get_bs */
    case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    default:     return D3D11_BLEND_ONE;
    }
}

static D3D11_BLEND_OP blend_op(uint32_t eq)
{
    switch (eq) {
    case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 0x8007: return D3D11_BLEND_OP_MIN;
    case 0x8008: return D3D11_BLEND_OP_MAX;
    default:     return D3D11_BLEND_OP_ADD;
    }
}

static UINT8 write_mask(uint32_t cm)
{
    UINT8 m = 0;
    if (cm & 0x00010000u) m |= D3D11_COLOR_WRITE_ENABLE_RED;
    if (cm & 0x00000100u) m |= D3D11_COLOR_WRITE_ENABLE_GREEN;
    if (cm & 0x00000001u) m |= D3D11_COLOR_WRITE_ENABLE_BLUE;
    if (cm & 0x01000000u) m |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    return m;
}

typedef struct { uint64_t key; void *obj; } StateEnt;
static StateEnt s_bs[256], s_dss[256], s_ss[256];
static int s_nbs, s_ndss, s_nss;

static void *state_find(StateEnt *t, int n, uint64_t key)
{
    int i;
    for (i = 0; i < n; i++)
        if (t[i].key == key)
            return t[i].obj;
    return NULL;
}

static void state_add(StateEnt *t, int *n, uint64_t key, void *obj)
{
    if (*n < 256) { t[*n].key = key; t[*n].obj = obj; (*n)++; }
}

static ID3D11BlendState *get_bs(int enable, uint32_t src, uint32_t dst, uint32_t eq, UINT8 mask)
{
    uint64_t key = ((uint64_t)enable << 63) | ((uint64_t)(src & 0xFFFF) << 40)
                 | ((uint64_t)(dst & 0xFFFF) << 24) | ((uint64_t)(eq & 0xFFFF) << 8) | mask;
    ID3D11BlendState *bs = (ID3D11BlendState *)state_find(s_bs, s_nbs, key);
    D3D11_BLEND_DESC d;
    if (bs) return bs;
    memset(&d, 0, sizeof d);
    d.RenderTarget[0].BlendEnable = enable;
    d.RenderTarget[0].SrcBlend = blend_of(src, 0);
    d.RenderTarget[0].DestBlend = blend_of(dst, 0);
    d.RenderTarget[0].BlendOp = blend_op(eq);
    d.RenderTarget[0].SrcBlendAlpha = blend_of(src, 1);
    d.RenderTarget[0].DestBlendAlpha = blend_of(dst, 1);
    if (dst == 0x308)                 /* invalid as a destination factor */
        d.RenderTarget[0].DestBlend = d.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    d.RenderTarget[0].BlendOpAlpha = blend_op(eq);
    d.RenderTarget[0].RenderTargetWriteMask = mask;
    if (FAILED(ID3D11Device_CreateBlendState(D, &d, &bs)))
        return NULL;
    state_add(s_bs, &s_nbs, key, bs);
    return bs;
}

static D3D11_COMPARISON_FUNC cmp_of(uint32_t f)
{
    if (f >= 0x200 && f <= 0x207)
        return (D3D11_COMPARISON_FUNC)(f - 0x200 + 1);
    return D3D11_COMPARISON_ALWAYS;
}

static D3D11_STENCIL_OP sop_of(uint32_t op)
{
    switch (op) {
    case 0:      return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    default:     return D3D11_STENCIL_OP_KEEP;
    }
}

static ID3D11DepthStencilState *get_dss(const D3D11_DEPTH_STENCIL_DESC *d)
{
    uint64_t key = hash_bytes((const uint8_t *)d, sizeof *d, 3);
    ID3D11DepthStencilState *s = (ID3D11DepthStencilState *)state_find(s_dss, s_ndss, key);
    if (s) return s;
    if (FAILED(ID3D11Device_CreateDepthStencilState(D, d, &s)))
        return NULL;
    state_add(s_dss, &s_ndss, key, s);
    return s;
}

static D3D11_TEXTURE_ADDRESS_MODE addr_of(uint32_t m)
{
    switch (m) {
    case 1:  return D3D11_TEXTURE_ADDRESS_WRAP;
    case 2:  return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 4:  return D3D11_TEXTURE_ADDRESS_BORDER;
    default: return D3D11_TEXTURE_ADDRESS_CLAMP;
    }
}

static ID3D11SamplerState *get_ss(int linear, uint32_t au, uint32_t av, uint32_t border,
                                  int border_on)
{
    uint64_t key;
    /* BORDER (4) reads the border colour only when FORMAT's border source
     * says colour; otherwise the hardware -- and the software path's
     * wrap_coord -- clamp to the edge texel. A transparent-black border there
     * painted the floor's shadow and light maps black past their edges. */
    if (!border_on) {
        if (au == 4) au = 3;
        if (av == 4) av = 3;
        border = 0;
    }
    key = ((uint64_t)linear << 40) | ((uint64_t)(au & 0xF) << 36)
        | ((uint64_t)(av & 0xF) << 32) | border;
    ID3D11SamplerState *s = (ID3D11SamplerState *)state_find(s_ss, s_nss, key);
    D3D11_SAMPLER_DESC d;
    if (s) return s;
    memset(&d, 0, sizeof d);
    d.Filter = linear ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
    d.AddressU = addr_of(au);
    d.AddressV = addr_of(av);
    d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    d.MaxLOD = D3D11_FLOAT32_MAX;
    d.ComparisonFunc = D3D11_COMPARISON_NEVER;
    d.BorderColor[0] = (float)((border >> 16) & 0xFF) / 255.0f;
    d.BorderColor[1] = (float)((border >> 8) & 0xFF) / 255.0f;
    d.BorderColor[2] = (float)(border & 0xFF) / 255.0f;
    d.BorderColor[3] = (float)(border >> 24) / 255.0f;
    if (FAILED(ID3D11Device_CreateSamplerState(D, &d, &s)))
        return NULL;
    state_add(s_ss, &s_nss, key, s);
    return s;
}

static void argb_to_f4(uint32_t c, float o[4])
{
    o[0] = (float)((c >> 16) & 0xFF) / 255.0f;
    o[1] = (float)((c >> 8) & 0xFF) / 255.0f;
    o[2] = (float)(c & 0xFF) / 255.0f;
    o[3] = (float)(c >> 24) / 255.0f;
}

/* ---- binding a surface -------------------------------------------------- */

static RtEnt *s_cur_rt;

/* What is bound now, so a draw that changes nothing re-sends nothing: the
 * club scenes issue thousands of draws a frame, most of them with the
 * previous draw's target, states and shaders. Reset whenever an object that
 * could be bound is freed (its address may come back as a new object). */
static struct {
    ID3D11RenderTargetView *rtv; ID3D11DepthStencilView *dsv;
    D3D11_VIEWPORT vp; D3D11_RECT sc;
    ID3D11DepthStencilState *dss; UINT sref;
    ID3D11BlendState *bs; float bf[4];
    ID3D11PixelShader *ps; ID3D11VertexShader *vs; ID3D11InputLayout *il;
    ID3D11ShaderResourceView *srv[4]; ID3D11SamplerState *smp[4];
    ID3D11Buffer *vb; UINT vstride; ID3D11Buffer *ib;
    int valid, rs, topo, vcb_n, pcb, cb_vsh;
    float vcb[12];
    unsigned char pc[1024];
} K;
static void k_reset(void) { memset(&K, 0, sizeof K); }
static void k_targets(ID3D11RenderTargetView *rtv, ID3D11DepthStencilView *dsv)
{
    if (K.valid && K.rtv == rtv && K.dsv == dsv) return;
    ID3D11DeviceContext_OMSetRenderTargets(C, 1, &rtv, dsv);
    /* Binding a target unbinds any view of it from the shader stages,
     * behind our back: forget what we think is bound there. */
    memset(K.srv, 0xFF, sizeof K.srv);
    K.rtv = rtv; K.dsv = dsv; K.valid = 1;
}
static void k_view(const D3D11_VIEWPORT *vp, const D3D11_RECT *sc)
{
    if (!K.valid || memcmp(&K.vp, vp, sizeof *vp)) {
        ID3D11DeviceContext_RSSetViewports(C, 1, vp); K.vp = *vp;
    }
    if (!K.valid || memcmp(&K.sc, sc, sizeof *sc)) {
        ID3D11DeviceContext_RSSetScissorRects(C, 1, sc); K.sc = *sc;
    }
}
static void k_vcb(const float *d)
{
    if (K.valid && K.vcb_n && !memcmp(K.vcb, d, sizeof K.vcb)) return;
    ID3D11DeviceContext_UpdateSubresource(C, (ID3D11Resource *)s_vcb, 0, NULL, d, 0, 0);
    memcpy(K.vcb, d, sizeof K.vcb); K.vcb_n = 1;
}
static void k_dss(ID3D11DepthStencilState *d, UINT ref)
{
    if (K.valid && K.dss == d && K.sref == ref) return;
    ID3D11DeviceContext_OMSetDepthStencilState(C, d, ref); K.dss = d; K.sref = ref;
}
static void k_bs(ID3D11BlendState *b, const float bf[4])
{
    if (K.valid && K.bs == b && !memcmp(K.bf, bf, sizeof K.bf)) return;
    ID3D11DeviceContext_OMSetBlendState(C, b, bf, 0xFFFFFFFF);
    K.bs = b; memcpy(K.bf, bf, sizeof K.bf);
}
/* The vertex shaders' b0: vp (2/W, 2/H, 1/zmax, -), then for translated
 * programs (z offset, clip at it, -, -) and fog (enable, mode, p0, p1). */
static float s_vcb_data[12];

/* Bind the colour target (and depth, if `want_depth`) for state `st`; sets
 * the viewport to the whole target and the scissor to rect. */
static int bind_surface(const Nv2aGpuState *st, int want_depth, const uint32_t rect[4],
                        DsEnt **ds_out)
{
    RtEnt *rt = get_rt(st);
    DsEnt *ds = NULL;
    D3D11_VIEWPORT vp;
    D3D11_RECT sc;

    if (!rt)
        return 0;
    if (want_depth && st->zeta_va)
        ds = get_ds(st->zeta_va, rt->pw, rt->ph);
    k_targets(rt->rtv, ds ? ds->dsv : NULL);
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width = (float)rt->pw; vp.Height = (float)rt->ph;
    vp.MinDepth = 0; vp.MaxDepth = 1;

    {
        float fx = (float)rt->pw / (float)rt->w, fy = (float)rt->ph / (float)rt->h;
        float r2 = (float)(rect[2] < rt->w ? rect[2] : rt->w);
        float r3 = (float)(rect[3] < rt->h ? rect[3] : rt->h);
        float l = (float)rect[0];
        s_inset_dx = 0;
        if (rt->wide && (r2 - l) * 2 < (float)rt->w && (r3 - (float)rect[1]) * 2 < (float)rt->h) {
            /* Where overlay_fit puts this spot of the 4:3 layout, 3/4 as
             * wide like the HUD drawn round it (which in 4:3 covers the
             * box's corners: a portrait's background is a cleared box). */
            float c = (l + r2) * 0.5f, hw = (r2 - l) * 0.375f;
            s_inset_dx = ((float)rt->w * 0.5f - c) * 0.25f;
            l = c + s_inset_dx - hw; r2 = c + s_inset_dx + hw;
            if (l < 0) l = 0;
            if (r2 > (float)rt->w) r2 = (float)rt->w;
        }
        sc.left = (LONG)(l * fx + 0.5f); sc.top = (LONG)(rect[1] * fy + 0.5f);
        sc.right = (LONG)(r2 * fx + 0.5f);
        sc.bottom = (LONG)(r3 * fy + 0.5f);
    }
    rt->dirty = 1;
    if (rt != s_cur_rt && !rt->shown)
        rt->ghash = rt_mem_hash(rt);
    k_view(&vp, &sc);
    memset(s_vcb_data, 0, sizeof s_vcb_data);
    s_vcb_data[0] = 2.0f / (float)rt->w;
    s_vcb_data[1] = 2.0f / (float)rt->h;
    s_vcb_data[2] = 1.0f / (st->zeta_bits == 16 ? 65535.0f : 16777215.0f);
    k_vcb(s_vcb_data);
    s_cur_rt = rt;
    if (ds_out) *ds_out = ds;
    return 1;
}

/* ---- the back end ------------------------------------------------------- */

static int dbg_logging(void);
static void d3d_clear(const Nv2aGpuState *st, uint32_t flags, uint32_t argb,
                      uint32_t zs, const uint32_t rect[4])
{
    DsEnt *ds = NULL;
    float k[8];
    D3D11_DEPTH_STENCIL_DESC dd;
    UINT8 mask = 0;
    int z = (flags & 1) != 0, s = (flags & 2) != 0;

    if (!bind_surface(st, z || s, rect, &ds))
        return;
    if (dbg_logging())
        fprintf(stderr, "[CLEAR] rt %08X flags %X argb %08X rect %u %u %u %u\n", st->color_va, flags, argb,
                rect[0], rect[1], rect[2], rect[3]);
    S.clears++;
    if (flags & 0x10) mask |= D3D11_COLOR_WRITE_ENABLE_BLUE;
    if (flags & 0x20) mask |= D3D11_COLOR_WRITE_ENABLE_GREEN;
    if (flags & 0x40) mask |= D3D11_COLOR_WRITE_ENABLE_RED;
    if (flags & 0x80) mask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    argb_to_f4(argb, k);
    k[4] = st->zeta_bits == 16 ? (float)(zs & 0xFFFF) / 65535.0f
                               : (float)(zs >> 8) / 16777215.0f;
    k[5] = k[6] = k[7] = 0;
    ID3D11DeviceContext_UpdateSubresource(C, (ID3D11Resource *)s_ccb, 0, NULL, k, 0, 0);
    memset(&dd, 0, sizeof dd);
    if (ds && z) {
        dd.DepthEnable = TRUE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    }
    if (ds && s) {
        dd.StencilEnable = TRUE;
        dd.StencilReadMask = 0xFF;
        dd.StencilWriteMask = 0xFF;
        dd.FrontFace.StencilFunc = dd.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
        dd.FrontFace.StencilPassOp = dd.BackFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
        dd.FrontFace.StencilFailOp = dd.BackFace.StencilFailOp = D3D11_STENCIL_OP_REPLACE;
        dd.FrontFace.StencilDepthFailOp = dd.BackFace.StencilDepthFailOp = D3D11_STENCIL_OP_REPLACE;
    }
    if (!dd.DepthEnable) {
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    }
    ID3D11DeviceContext_OMSetDepthStencilState(C, get_dss(&dd), zs & 0xFF);
    ID3D11DeviceContext_OMSetBlendState(C, get_bs(0, 1, 0, 0x8006, mask), NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_IASetInputLayout(C, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(C, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(C, s_clear_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(C, s_clear_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(C, 0, 1, &s_ccb);
    ID3D11DeviceContext_RSSetState(C, s_rs);
    ID3D11DeviceContext_Draw(C, 3, 0);
    {   /* the clear bound its own shaders, states and buffers */
        ID3D11RenderTargetView *rtv = K.rtv; ID3D11DepthStencilView *dsv = K.dsv;
        D3D11_VIEWPORT vp = K.vp; D3D11_RECT sc = K.sc; float vcb[12];
        memcpy(vcb, K.vcb, sizeof vcb);
        k_reset();
        K.rtv = rtv; K.dsv = dsv; K.vp = vp; K.sc = sc; memcpy(K.vcb, vcb, sizeof vcb);
        K.vcb_n = 1; K.valid = 1;
    }
}

/* Everything a draw needs but its vertices: target, depth, blend, pixel
 * shader, textures. Returns 0 if the batch cannot be drawn. */

/* ---- draw debugging ---------------------------------------------------------
 * RECOMP_GPU_DEBUG=1: gpudbg.txt beside the exe is re-read when it changes.
 *   skip LO HI   -- draws LO..HI-1 of each frame are not drawn
 *   only LO HI   -- only draws LO..HI-1 are drawn
 *   log          -- the next frame's draws are listed on stderr
 * Draw numbers count setup_pixels calls from the start of a frame. */
static struct { int on, mode; uint32_t lo, hi; int log; FILETIME ft; uint32_t dix, flags; } s_dbg = { -1 };
/* flags: 1 unsampled stages read 0, 2 r0.a starts 0 without stage 0, 8 skip combiner draws with no texture stages, 16 print each new pixel shader */

static int dbg_logging(void) { return s_dbg.log == 1; }

static void dbg_poll(void)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    FILE *f;
    char line[128];
    if (s_dbg.on < 0) {
        const char *e = getenv("RECOMP_GPU_DEBUG");
        s_dbg.on = e && *e && *e != '0';
    }
    s_dbg.dix = 0;
    if (s_dbg.log > 0) s_dbg.log--;
    if (!s_dbg.on || (s_frame & 15))
        return;
    if (!GetFileAttributesExA("gpudbg.txt", GetFileExInfoStandard, &fa))
        return;
    if (!memcmp(&fa.ftLastWriteTime, &s_dbg.ft, sizeof s_dbg.ft))
        return;
    s_dbg.ft = fa.ftLastWriteTime;
    f = fopen("gpudbg.txt", "r");
    if (!f) return;
    s_dbg.mode = 0; s_dbg.flags = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned lo, hi;
        if (sscanf(line, "skip %u %u", &lo, &hi) == 2) { s_dbg.mode = 1; s_dbg.lo = lo; s_dbg.hi = hi; }
        else if (sscanf(line, "only %u %u", &lo, &hi) == 2) { s_dbg.mode = 2; s_dbg.lo = lo; s_dbg.hi = hi; }
        else if (!strncmp(line, "log", 3)) s_dbg.log = 2;
        else if (sscanf(line, "flags %u", &lo) == 1) s_dbg.flags = lo;
    }
    fclose(f);
    fprintf(stderr, "[GPU-DBG] frame %u mode %d %u..%u log %d flags %X\n", s_frame, s_dbg.mode, s_dbg.lo, s_dbg.hi, s_dbg.log, s_dbg.flags);
}

static void dbg_log(uint32_t dix, const Nv2aGpuState *st, const PsKey *key)
{
    const Nv2aRenderState *rs = &st->rs;
    int i;
    fprintf(stderr, "[DRAW %4u] rt %08X z %08X/%u ps %016llX rc %u sp %05X blend %u %X %X %X bc %08X"
            " at %u %X %u depth %u %X %u st %u %X %u m%X/%X %X %X %X cm %08X",
            dix, st->color_va, st->zeta_va, st->zeta_bits,
            (unsigned long long)hash_bytes((const uint8_t *)key, sizeof *key, 7), st->use_rc,
            st->rc.stage_program, rs->blend_enable, rs->blend_src, rs->blend_dst, rs->blend_eq, rs->blend_color,
            rs->alpha_test_enable, rs->alpha_func, rs->alpha_ref,
            rs->depth_test_enable, rs->depth_func, rs->depth_write,
            rs->stencil_enable, rs->stencil_func, rs->stencil_ref, rs->stencil_func_mask, rs->stencil_write_mask,
            rs->stencil_fail, rs->stencil_zfail, rs->stencil_zpass, rs->color_mask);
    for (i = 0; i < 4; i++)
        if (st->tex[i].valid)
            fprintf(stderr, " t%d %08X f%02X %ux%u a%u%u", i, st->tex[i].offset, st->tex[i].color,
                    st->tex[i].width, st->tex[i].height, st->tex[i].addr_u, st->tex[i].addr_v);
    fputc('\n', stderr);
    if (s_dbg.flags & 16) {
        static uint64_t seen[256]; static int nseen;
        uint64_t h = hash_bytes((const uint8_t *)key, sizeof *key, 7);
        int q;
        for (q = 0; q < nseen && seen[q] != h; q++) ;
        if (q == nseen && nseen < 256) {
            char *src = gen_ps(key);
            seen[nseen++] = h;
            fprintf(stderr, "[PS %016llX] control %08X final %08X %08X icw %08X %08X ocw %08X %08X"
                    " aicw %08X %08X aocw %08X %08X f0 %08X f1 %08X\n%s\n", (unsigned long long)h,
                    key->control, key->final0, key->final1, key->color_icw[0], key->color_icw[1],
                    key->color_ocw[0], key->color_ocw[1], key->alpha_icw[0], key->alpha_icw[1],
                    key->alpha_ocw[0], key->alpha_ocw[1], st->rc.factor0[0], st->rc.factor1[0], src);
            free(src);
        }
    }
}

static int setup_pixels(const Nv2aGpuState *st)
{
    const Nv2aRenderState *rs = &st->rs;
    uint32_t rect[4];
    DsEnt *ds = NULL;
    PsKey key;
    PsConst pc;
    ID3D11ShaderResourceView *srv[4] = { NULL, NULL, NULL, NULL };
    ID3D11SamplerState *smp[4] = { NULL, NULL, NULL, NULL };
    ID3D11PixelShader *ps;
    D3D11_DEPTH_STENCIL_DESC dd;
    float bf[4];
    int i, need_depth;
    uint32_t dix = s_dbg.dix++;

    if (s_dbg.mode == 1 && dix >= s_dbg.lo && dix < s_dbg.hi) return 0;
    if (s_dbg.mode == 2 && (dix < s_dbg.lo || dix >= s_dbg.hi)) return 0;
    rect[0] = st->clip_x; rect[1] = st->clip_y;
    rect[2] = st->clip_x + st->clip_w; rect[3] = st->clip_y + st->clip_h;
    if (st->swizzled) {
        rect[0] = rect[1] = 0;
        if (rect[2] > st->swz_w) rect[2] = st->swz_w;
        if (rect[3] > st->swz_h) rect[3] = st->swz_h;
    }
    need_depth = rs->depth_test_enable || rs->depth_write || rs->stencil_enable;
    if (!bind_surface(st, need_depth, rect, &ds))
        return 0;

    /* Pixel shader and its constants. */
    memset(&key, 0, sizeof key);
    memset(&pc, 0, sizeof pc);
    key.use_rc = st->use_rc;
    key.textured = st->use_rc ? 0 : st->textured;
    if (st->use_rc) {
        memcpy(key.color_icw, st->rc.color_icw, sizeof key.color_icw);
        memcpy(key.alpha_icw, st->rc.alpha_icw, sizeof key.alpha_icw);
        memcpy(key.color_ocw, st->rc.color_ocw, sizeof key.color_ocw);
        memcpy(key.alpha_ocw, st->rc.alpha_ocw, sizeof key.alpha_ocw);
        key.final0 = st->rc.final0;
        key.final1 = st->rc.final1;
        key.control = st->rc.control;
        key.stage_program = st->rc.stage_program;
        key.clip_plane_mode = st->clip_plane_mode;
        for (i = 0; i < 8; i++) {
            argb_to_f4(st->rc.factor0[i], pc.f0[i]);
            argb_to_f4(st->rc.factor1[i], pc.f1[i]);
        }
        argb_to_f4(st->rc.final_c0, pc.fc0);
        argb_to_f4(st->rc.final_c1, pc.fc1);
        /* FOG_COLOR is R in bits 0-7 (raster_xf_triangle swaps it). */
        pc.fogc[0] = (float)(st->fog_color & 0xFF) / 255.0f;
        pc.fogc[1] = (float)((st->fog_color >> 8) & 0xFF) / 255.0f;
        pc.fogc[2] = (float)((st->fog_color >> 16) & 0xFF) / 255.0f;
        pc.fogc[3] = (float)(st->fog_color >> 24) / 255.0f;
    }
    for (i = 0; i < 4; i++) {
        const Nv2aGpuTexture *t = &st->tex[i];
        uint32_t mode = st->use_rc ? (st->rc.stage_program >> (i * 5)) & 0x1F
                                   : (i == 0 && st->textured ? 1u : 0u);
        RtEnt *rt;
        uint32_t mag = (t->filter >> 24) & 0xF, min = (t->filter >> 16) & 0xFF;
        int linear;

        pc.ts[i][0] = pc.ts[i][1] = 1.0f;
        if (!(mode == 1 || mode == 2 || mode == 3) || !t->valid)
            continue;
        linear = st->bilinear || mag == 2 || min == 2 || min == 4 || min == 6;
        rt = t->cube ? NULL : find_rt(t->offset);
        if (rt && rt != s_cur_rt && !rt->shown && rt->ghash != rt_mem_hash(rt)) {
            free_rt(rt);                  /* overwritten: the texture now */
            rt = NULL;
            S.rt_stale++;
        }
        if (rt) {
            /* Render-to-texture: sample the target itself. Its texels hold
             * A8R8G8B8 as drawn; a texture read in the reverse order swaps. */
            if (rt != s_cur_rt)
                rt_resolve(rt);
            srv[i] = rt->srv;
            key.kind[i] = 1;
            key.swap_rb[i] = (t->color == 0x3A || t->color == 0x3F);
            if (t->normalized) {
                pc.ts[i][0] = (float)t->width / (float)rt->w;
                pc.ts[i][1] = (float)t->height / (float)rt->h;
            } else {
                pc.ts[i][0] = 1.0f / (float)rt->w;
                pc.ts[i][1] = 1.0f / (float)rt->h;
            }
            S.tex_hits_rt++;
            if (s_stats > 0) {
                static uint32_t seen[64][4]; static int nseen;
                int q, hit = 0;
                for (q = 0; q < nseen; q++)
                    if (seen[q][0] == t->offset && seen[q][1] == t->color && seen[q][2] == t->width)
                        hit = 1;
                if (!hit && nseen < 64) {
                    seen[nseen][0] = t->offset; seen[nseen][1] = t->color; seen[nseen][2] = t->width; nseen++;
                    fprintf(stderr, "[GPU-D3D11] render-to-texture read: va %08X fmt %02X %ux%u norm %u"
                            " (target %ux%u swz %u) stage %d mode %u\n", t->offset, t->color,
                            t->width, t->height, t->normalized, rt->w, rt->h, rt->swizzled, i, mode);
                }
            }
        } else {
            srv[i] = get_tex(t);
            if (!srv[i])
                continue;
            key.kind[i] = t->cube ? 2 : 1;
            if (!t->normalized) {
                pc.ts[i][0] = 1.0f / (float)t->width;
                pc.ts[i][1] = 1.0f / (float)t->height;
            }
        }
        smp[i] = get_ss(linear, t->addr_u, t->addr_v, t->border, t->border_color != 0);
    }
    key.dbg = s_dbg.flags & 3;
    if ((s_dbg.flags & 8) && st->use_rc && !(st->rc.stage_program & 0x7FFF)) return 0;
    key.alpha_test = rs->alpha_test_enable;
    key.alpha_func = rs->alpha_test_enable ? rs->alpha_func : 0;
    pc.aref[0] = (int)(rs->alpha_ref & 0xFF);
    if (s_dbg.log == 1) dbg_log(dix, st, &key);
    ps = get_ps(&key);
    if (!ps)
        return 0;

    /* Depth and stencil, with OpenGL's rule (the NV2A's): depth test off
     * means no depth write either, whatever the mask says. The Sims draws
     * its floor shadows with the test off and the mask on; writing their
     * depth hid the walls and furniture drawn after them, leaving black
     * holes where the cleared background showed through. */
    memset(&dd, 0, sizeof dd);
    if (ds) {
        dd.DepthEnable = rs->depth_test_enable != 0;
        dd.DepthFunc = rs->depth_test_enable ? cmp_of(rs->depth_func) : D3D11_COMPARISON_ALWAYS;
        dd.DepthWriteMask = rs->depth_write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        if (rs->stencil_enable) {
            dd.StencilEnable = TRUE;
            dd.StencilReadMask = (UINT8)rs->stencil_func_mask;
            dd.StencilWriteMask = (UINT8)rs->stencil_write_mask;
            dd.FrontFace.StencilFunc = cmp_of(rs->stencil_func);
            dd.FrontFace.StencilFailOp = sop_of(rs->stencil_fail);
            dd.FrontFace.StencilDepthFailOp = sop_of(rs->stencil_zfail);
            dd.FrontFace.StencilPassOp = sop_of(rs->stencil_zpass);
            dd.BackFace = dd.FrontFace;
        }
    }
    if (!dd.DepthEnable) {
        dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    }
    k_dss(get_dss(&dd), rs->stencil_ref & 0xFF);
    argb_to_f4(rs->blend_color, bf);
    if (rs->blend_src == 0x8003 || rs->blend_src == 0x8004
        || rs->blend_dst == 0x8003 || rs->blend_dst == 0x8004)
        bf[0] = bf[1] = bf[2] = bf[3];
    k_bs(get_bs(rs->blend_enable && !(rs->blend_src == 1 && rs->blend_dst == 0),
                rs->blend_src, rs->blend_dst, rs->blend_eq, write_mask(rs->color_mask)), bf);

    if (!K.valid || !K.pcb || memcmp(K.pc, &pc, sizeof pc)) {
        ID3D11DeviceContext_UpdateSubresource(C, (ID3D11Resource *)s_pcb, 0, NULL, &pc, 0, 0);
        memcpy(K.pc, &pc, sizeof pc);
    }
    if (!K.valid || K.ps != ps) {
        ID3D11DeviceContext_PSSetShader(C, ps, NULL, 0);
        K.ps = ps;
    }
    if (!K.valid || !K.pcb) {
        ID3D11DeviceContext_PSSetConstantBuffers(C, 0, 1, &s_pcb);
        K.pcb = 1;
    }
    /* A target is never sampled while bound: a pass that reads the surface it
     * draws into would be undefined on hardware too. */
    for (i = 0; i < 4; i++)
        if (srv[i] && s_cur_rt && srv[i] == s_cur_rt->srv)
            srv[i] = NULL;
    if (!K.valid || memcmp(K.srv, srv, sizeof K.srv)) {
        ID3D11DeviceContext_PSSetShaderResources(C, 0, 4, srv);
        memcpy(K.srv, srv, sizeof K.srv);
    }
    if (!K.valid || memcmp(K.smp, smp, sizeof K.smp)) {
        ID3D11DeviceContext_PSSetSamplers(C, 0, 4, smp);
        memcpy(K.smp, smp, sizeof K.smp);
    }
    if (!K.valid || !K.rs) {
        ID3D11DeviceContext_RSSetState(C, s_rs);
        K.rs = 1;
    }
    return 1;
}

/* Widescreen: the title draws its 3D wider (its views are 16:9) and the
 * picture is stretched to 16:9, so flat 2D -- HUD, menus, text, all with w
 * exactly 1 -- would come out a third too wide. Those are drawn 3/4 as wide
 * about the middle instead: right shape, inside the 4:3 area the title laid
 * them out in. A full-width flat quad over a 3D scene is an effect on the
 * whole picture and keeps its width. 3D clipped to a small box (a live
 * portrait in the HUD) has the right shape already and only moves with the
 * HUD around it. Sets x' = x * vx.z + vx.w (vx.z 0: as drawn). */
static void overlay_fit(int flat, float x0, float x1)
{
    s_vcb_data[6] = s_vcb_data[7] = 0;
    if (!s_cur_rt || !s_cur_rt->wide)
        return;
    if (!flat) {
        s_world_cur = 1;
        if (s_inset_dx != 0) {
            s_vcb_data[6] = 1.0f;
            s_vcb_data[7] = s_inset_dx;
        }
        return;
    }
    if (x0 <= 1.0f && x1 >= (float)s_cur_rt->w - 1.0f && s_world_prev)
        return;
    s_vcb_data[6] = 0.75f;
    s_vcb_data[7] = (float)s_cur_rt->w * 0.5f * 0.25f;
}

static void d3d_draw(const Nv2aGpuState *st, const Nv2aGpuVertex *v, uint32_t count)
{
    float ok = 0, ox = 0;
    uint32_t done = 0;

    if (!count || !setup_pixels(st))
        return;
    if (s_dbg.log == 1) {
        float x0 = 1e9f, x1 = -1e9f, w0 = 1e9f, w1 = -1e9f; uint32_t q;
        for (q = 0; q < count; q++) {
            float x = v[q].pos[0], w = v[q].pos[3];
            if (x < x0) x0 = x; if (x > x1) x1 = x; if (w < w0) w0 = w; if (w > w1) w1 = w;
        }
        fprintf(stderr, "  [CPU] n %u x %.1f..%.1f w %.4f..%.4f\n", count, x0, x1, w0, w1);
    }
    if (s_cur_rt && s_cur_rt->wide) {
        float x0 = 1e30f, x1 = -1e30f; uint32_t q; int flat = 1;
        for (q = 0; q < count; q++) {
            if (v[q].pos[3] != 1.0f) { flat = 0; break; }
            if (v[q].pos[0] < x0) x0 = v[q].pos[0];
            if (v[q].pos[0] > x1) x1 = v[q].pos[0];
        }
        overlay_fit(flat, x0, x1);
        ok = s_vcb_data[6]; ox = s_vcb_data[7];
        s_vcb_data[6] = s_vcb_data[7] = 0;        /* this path moves the vertices */
    }
    if (K.il != s_layout) { ID3D11DeviceContext_IASetInputLayout(C, s_layout); K.il = s_layout; }
    if (!K.topo) { ID3D11DeviceContext_IASetPrimitiveTopology(C, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); K.topo = 1; }
    if (K.vs != s_vs) { ID3D11DeviceContext_VSSetShader(C, s_vs, NULL, 0); K.vs = s_vs; }
    if (K.cb_vsh != 1) { ID3D11DeviceContext_VSSetConstantBuffers(C, 0, 1, &s_vcb); K.cb_vsh = 1; }
    K.vb = NULL;                       /* rebound below each chunk */

    /* Vertices through a ring buffer, in triangle-aligned chunks. */
    while (done < count) {
        UINT stride = sizeof(Nv2aGpuVertex), off = 0, n = count - done;
        D3D11_MAPPED_SUBRESOURCE m;
        D3D11_MAP how = D3D11_MAP_WRITE_NO_OVERWRITE;
        if (n > s_vb_cap) n = s_vb_cap - s_vb_cap % 3;
        if (s_vb_pos + n > s_vb_cap) { s_vb_pos = 0; how = D3D11_MAP_WRITE_DISCARD; }
        if (FAILED(ID3D11DeviceContext_Map(C, (ID3D11Resource *)s_vb, 0, how, 0, &m)))
            return;
        memcpy((Nv2aGpuVertex *)m.pData + s_vb_pos, v + done, (size_t)n * sizeof *v);
        if (ok != 0) {
            Nv2aGpuVertex *d = (Nv2aGpuVertex *)m.pData + s_vb_pos;
            UINT q;
            for (q = 0; q < n; q++)
                d[q].pos[0] = d[q].pos[0] * ok + ox;
        }
        ID3D11DeviceContext_Unmap(C, (ID3D11Resource *)s_vb, 0);
        ID3D11DeviceContext_IASetVertexBuffers(C, 0, 1, &s_vb, &stride, &off);
        ID3D11DeviceContext_Draw(C, n, s_vb_pos);
        s_vb_pos += n;
        done += n;
    }
    S.draws++;
    S.verts += count;
}

/* ---- vertex programs as vertex shaders ------------------------------------
 *
 * The NV2A program, instruction for instruction, as HLSL: the same reads,
 * the same MAC and ILU arithmetic (run_mac / run_ilu in nv2a_vsh_interp.c),
 * the same write rules (both units read before either writes; an ILU op
 * paired with a MAC op writes R1), the same output defaults and clamps.
 * Then the executor's own post-processing: w = +0 becomes 1, fog_factor,
 * and the clip planes raster_xf_clipped applies (w above 1e-3, and screen z
 * at or beyond the viewport's z offset) as clip distances. */

static uint32_t vf(const uint32_t *w, int dw, int lo, int width)
{
    return (w[dw] >> lo) & ((1u << width) - 1u);
}

static void vs_src(Buf *b, const uint32_t *w, int which)
{
    static const char sw[] = "xyzw";
    uint32_t neg, swz, mux, reg;
    char base[32];

    if (which == 0) {
        neg = vf(w, 1, 8, 1); swz = vf(w, 1, 0, 8);
        mux = vf(w, 2, 26, 2); reg = vf(w, 2, 28, 4);
    } else if (which == 1) {
        neg = vf(w, 2, 25, 1); swz = vf(w, 2, 17, 8);
        mux = vf(w, 2, 11, 2); reg = vf(w, 2, 13, 4);
    } else {
        neg = vf(w, 2, 10, 1); swz = vf(w, 2, 2, 8);
        mux = vf(w, 3, 28, 2);
        reg = (vf(w, 2, 0, 2) << 2) | vf(w, 3, 30, 2);
    }
    if (mux == 1)
        snprintf(base, sizeof base, "R%u", reg <= 12 ? reg : 0u);   /* 13..15 read R0 */
    else if (mux == 2)
        snprintf(base, sizeof base, "i.v%u", vf(w, 1, 9, 4));
    else if (vf(w, 3, 1, 1))
        snprintf(base, sizeof base, "CR(a0 + %u)", vf(w, 1, 13, 8));
    else
        snprintf(base, sizeof base, "c[%u]", vf(w, 1, 13, 8) < 192 ? vf(w, 1, 13, 8) : 0);
    bput(b, "%s(%s.%c%c%c%c)", neg ? "-" : "", base,
         sw[(swz >> 6) & 3], sw[(swz >> 4) & 3], sw[(swz >> 2) & 3], sw[swz & 3]);
}

static const char *mask_str(uint32_t m)
{
    static char t[5];
    int n = 0;
    if (m & 8) t[n++] = 'x';
    if (m & 4) t[n++] = 'y';
    if (m & 2) t[n++] = 'z';
    if (m & 1) t[n++] = 'w';
    t[n] = 0;
    return t;
}

static char *gen_vs(const Nv2aGpuVsh *vs, int clipd)
{
    Buf b = { (char *)malloc(32768), 0, 32768 };
    uint32_t k, n;

    bput(&b,
         "cbuffer V : register(b0) { float4 vp; float4 vx; float4 fogp; };\n"
         "cbuffer K : register(b1) { float4 c[192]; };\n"
         "float4 CR(int i) { return (i >= 0 && i < 192) ? c[i] : c[0]; }\n"
         "struct VI {");
    for (k = 0; k < 16; k++)
        if (vs->in_mask >> k & 1)
            bput(&b, " float4 v%u : ATTR%u;", k, k);
    bput(&b, " };\n"
         "struct VO { float4 p : SV_Position; float4 d0 : COLOR0; float4 d1 : COLOR1;\n"
         "  noperspective float fog : FOG; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1;\n"
         "  float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;%s };\n"
         "float fogf(float d) {\n"
         "  if (fogp.x == 0) return 1;\n"
         "  int mode = (int)fogp.y; float f;\n"
         "  if (mode == 0x800 || mode == 0x802) f = fogp.z + exp2(d * fogp.w * 16.0) - 1.5;\n"
         "  else if (mode == 0x801 || mode == 0x803) f = fogp.z + exp2(-d * d * fogp.w * fogp.w * 32.0) - 1.5;\n"
         "  else f = fogp.z + d * fogp.w - 1.0;\n"
         "  if (mode == 0x802 || mode == 0x803 || mode == 0x804) f = abs(f);\n"
         "  if ((asuint(f) & 0x7FFFFFFF) > 0x7F800000) f = 1;\n"
         "  return saturate(f); }\n"
         "VO vs(VI i) {\n"
         "  float4 R0 = 0, R1 = 0, R2 = 0, R3 = 0, R4 = 0, R5 = 0, R6 = 0, R7 = 0,\n"
         "         R8 = 0, R9 = 0, R10 = 0, R11 = 0, R12 = 0;\n"
         "  float4 O1 = 0, O2 = 0, O3 = float4(0,0,0,1), O4 = float4(0,0,0,1), O5 = 0,\n"
         "         O6 = 0, O7 = 0, O8 = 0, O9 = float4(0,0,0,1), O10 = float4(0,0,0,1),\n"
         "         O11 = float4(0,0,0,1), O12 = float4(0,0,0,1);\n"
         "  int a0 = 0;\n", clipd ? " float2 cd : SV_ClipDistance0;" : "");
    n = vs->nins;
    for (k = 0; k < n; k++) {
        const uint32_t *w = vs->prog[k];
        uint32_t mac = vf(w, 1, 21, 4), ilu = vf(w, 1, 25, 3);
        uint32_t mmask = vf(w, 3, 24, 4), imask = vf(w, 3, 16, 4), tdst = vf(w, 3, 20, 4);
        uint32_t omask = vf(w, 3, 12, 4), oaddr = vf(w, 3, 3, 8);
        uint32_t reads = (mac ? 1u : 0u) | ((mac == 2 || (mac >= 4 && mac <= 12)) ? 2u : 0u)
                       | ((mac == 3 || mac == 4 || ilu) ? 4u : 0u);
        uint32_t it = mac ? 1u : tdst;
        if (!mac && !ilu)
            goto next;
        bput(&b, "  { float4 A = 0, B = 0, C = 0, m = 0, u = 0;\n");
        if (reads & 1) { bput(&b, "    A = "); vs_src(&b, w, 0); bput(&b, ";\n"); }
        if (reads & 2) { bput(&b, "    B = "); vs_src(&b, w, 1); bput(&b, ";\n"); }
        if (reads & 4) { bput(&b, "    C = "); vs_src(&b, w, 2); bput(&b, ";\n"); }
        switch (mac) {
        case 0:  break;
        case 1:  bput(&b, "    m = A;\n"); break;
        case 2:  bput(&b, "    m = A * B;\n"); break;
        case 3:  bput(&b, "    m = A + C;\n"); break;
        case 4:  bput(&b, "    m = A * B + C;\n"); break;
        case 5:  bput(&b, "    m = dot(A.xyz, B.xyz).xxxx;\n"); break;
        case 6:  bput(&b, "    m = (dot(A.xyz, B.xyz) + B.w).xxxx;\n"); break;
        case 7:  bput(&b, "    m = dot(A, B).xxxx;\n"); break;
        case 8:  bput(&b, "    m = float4(1.0, A.y * B.y, A.z, B.w);\n"); break;
        case 9:  bput(&b, "    m = float4(A.x < B.x ? A.x : B.x, A.y < B.y ? A.y : B.y,"
                          " A.z < B.z ? A.z : B.z, A.w < B.w ? A.w : B.w);\n"); break;
        case 10: bput(&b, "    m = float4(A.x >= B.x ? A.x : B.x, A.y >= B.y ? A.y : B.y,"
                          " A.z >= B.z ? A.z : B.z, A.w >= B.w ? A.w : B.w);\n"); break;
        case 11: bput(&b, "    m = float4(A < B);\n"); break;
        case 12: bput(&b, "    m = float4(A >= B);\n"); break;
        case 13: bput(&b, "    m = A; a0 = (int)floor(A.x + 0.001);\n"); break;
        default: bput(&b, "    m = A;\n"); break;
        }
        switch (ilu) {
        case 0:  break;
        case 1:  bput(&b, "    u = C;\n"); break;
        case 2:  bput(&b, "    u = (C.x == 0 ? asfloat(0x7F800000) : 1.0 / C.x).xxxx;\n"); break;
        case 3:  bput(&b, "    { float f = 1.0 / C.x;\n"
                          "      if (abs(f) < 5.42101e-20) f = f < 0 ? -5.42101e-20 : 5.42101e-20;\n"
                          "      if (abs(f) > 1.884467e19) f = f < 0 ? -1.884467e19 : 1.884467e19;\n"
                          "      u = f.xxxx; }\n"); break;
        case 4:  bput(&b, "    u = rsqrt(abs(C.x)).xxxx;\n"); break;
        case 5:  bput(&b, "    { float fl = floor(C.x); u = float4(exp2(fl), C.x - fl, exp2(C.x), 1.0); }\n"); break;
        case 6:  bput(&b, "    { float ax = abs(C.x);\n"
                          "      if (ax == 0) u = float4(asfloat(0xFF800000), 1.0, asfloat(0xFF800000), 1.0);\n"
                          "      else { float e = floor(log2(ax)); u = float4(e, ax / exp2(e), log2(ax), 1.0); } }\n"); break;
        case 7:  bput(&b, "    { float nl = max(C.x, 0), nh = max(C.y, 0), pw = clamp(C.w, -127.9961, 127.9961);\n"
                          "      u = float4(1.0, nl, C.x > 0 ? pow(nh, pw) : 0.0, 1.0); }\n"); break;
        default: break;
        }
        if (mac && mac != 13 && mmask && tdst <= 12)
            bput(&b, "    R%u.%s = m.%s;\n", tdst, mask_str(mmask), mask_str(mmask));
        if (ilu && imask && it <= 12)
            bput(&b, "    R%u.%s = u.%s;\n", it, mask_str(imask), mask_str(imask));
        if (omask && vf(w, 3, 11, 1)) {
            const char *src = vf(w, 3, 2, 1) ? "u" : "m";
            if (oaddr == 0)
                bput(&b, "    R12.%s = %s.%s;\n", mask_str(omask), src, mask_str(omask));
            else if (oaddr < 13)
                bput(&b, "    O%u.%s = %s.%s;\n", oaddr, mask_str(omask), src, mask_str(omask));
        }
        bput(&b, "  }\n");
    next:
        if (vf(w, 3, 0, 1))
            break;
    }
    bput(&b,
         "  VO o;\n"
         "  float4 P = R12;\n"
         "  if (P.w == 0 && asuint(P.w) == 0) P.w = 1;\n"
         "  if (vx.z != 0) P.x = P.x * vx.z + vx.w;\n"
         "  float wv = P.w;\n"
         "  o.p = float4((P.x * vp.x - 1.0) * wv, (1.0 - P.y * vp.y) * wv, P.z * vp.z * wv, wv);\n"
         "  o.d0 = saturate(O3); o.d1 = saturate(O4);\n"
         "  o.fog = fogf(O5.x);\n"
         "  o.t0 = O9; o.t1 = O10; o.t2 = O11; o.t3 = O12;\n"
         "%s"
         "  return o; }\n",
         clipd ? "  o.cd = float2(wv - 1e-3, vx.y != 0 ? (P.z - vx.x) * wv : 1.0);\n" : "");
    return b.p;
}

typedef struct {
    uint64_t hash;
    uint32_t nins, in_mask;
    ID3D11VertexShader *vs;
    ID3D11InputLayout *il;
    int failed;
} VsEnt;

static VsEnt *s_vsc;
static int s_nvsc, s_cap_vsc;
static ID3D11Buffer *s_kcb, *s_vb2, *s_ib2;
static UINT s_vb2_cap, s_vb2_pos, s_ib2_cap, s_ib2_pos;     /* bytes */
static float s_kcb_data[192][4];
static int s_kcb_valid;

static VsEnt *get_vs(const Nv2aGpuVsh *vs)
{
    int i;
    VsEnt *e;
    char *src;
    ID3DBlob *blob;

    for (i = 0; i < s_nvsc; i++)
        if (s_vsc[i].hash == vs->hash && s_vsc[i].nins == vs->nins
            && s_vsc[i].in_mask == vs->in_mask)
            return s_vsc[i].failed ? NULL : &s_vsc[i];
    if (s_nvsc == s_cap_vsc) {
        s_cap_vsc = s_cap_vsc ? s_cap_vsc * 2 : 32;
        s_vsc = (VsEnt *)realloc(s_vsc, (size_t)s_cap_vsc * sizeof *s_vsc);
    }
    e = &s_vsc[s_nvsc++];
    memset(e, 0, sizeof *e);
    e->hash = vs->hash; e->nins = vs->nins; e->in_mask = vs->in_mask;
    src = gen_vs(vs, 1);
    blob = compile(src, "vs", "vs_4_0");
    if (!blob) {
        /* A compiler without SV_ClipDistance (wine's): no exact clip planes,
         * the GPU's own clipping only. */
        free(src);
        src = gen_vs(vs, 0);
        blob = compile(src, "vs", "vs_4_0");
    }
    if (getenv("RECOMP_GPU_VSH_DUMP"))
        fprintf(stderr, "[GPU-D3D11] vertex program %016llX (%u instructions):\n%s\n",
                (unsigned long long)vs->hash, vs->nins, src);
    free(src);
    if (blob) {
        D3D11_INPUT_ELEMENT_DESC il[16];
        UINT k, n = 0;
        static const char *sem = "ATTR";
        for (k = 0; k < 16; k++)
            if (vs->in_mask >> k & 1) {
                il[n].SemanticName = sem;
                il[n].SemanticIndex = k;
                il[n].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
                il[n].InputSlot = 0;
                il[n].AlignedByteOffset = n * 16;
                il[n].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
                il[n].InstanceDataStepRate = 0;
                n++;
            }
        if (FAILED(ID3D11Device_CreateVertexShader(D, ID3D10Blob_GetBufferPointer(blob),
                       ID3D10Blob_GetBufferSize(blob), NULL, &e->vs))
            || (n && FAILED(ID3D11Device_CreateInputLayout(D, il, n,
                       ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &e->il))))
            e->failed = 1;
        ID3D10Blob_Release(blob);
    } else {
        e->failed = 1;
    }
    S.vprogs++;
    return e->failed ? NULL : e;
}

/* Bytes into a dynamic ring buffer; returns the byte offset or -1. */
static long ring_put(ID3D11Buffer *buf, UINT cap, UINT *pos, const void *data, UINT bytes, UINT align)
{
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_MAP how = D3D11_MAP_WRITE_NO_OVERWRITE;
    UINT at = (*pos + align - 1) / align * align;
    if (bytes > cap)
        return -1;
    if (at + bytes > cap) { at = 0; how = D3D11_MAP_WRITE_DISCARD; }
    if (FAILED(ID3D11DeviceContext_Map(C, (ID3D11Resource *)buf, 0, how, 0, &m)))
        return -1;
    memcpy((uint8_t *)m.pData + at, data, bytes);
    ID3D11DeviceContext_Unmap(C, (ID3D11Resource *)buf, 0);
    *pos = at + bytes;
    return (long)at;
}

static int d3d_draw_vsh(const Nv2aGpuState *st, const Nv2aGpuVsh *vs,
                        const float *in, uint32_t nverts,
                        const uint32_t *idx, uint32_t nidx)
{
    VsEnt *e;
    long vo, io;
    UINT stride = vs->stride * 4, zero = 0;

    if (!s_vb2 || !nidx)
        return 0;
    e = get_vs(vs);
    if (!e)
        return 0;
    if (!setup_pixels(st))
        return 1;                     /* nothing the CPU could do better */
    if (s_dbg.log == 1) fprintf(stderr, "  [VSH] n %u nins %u probed %u x %.1f..%.1f y %.1f..%.1f w %.5f..%.5f"
        " clip %u %u %u %u vp %.1f %.1f %.1f %.1f\n",
        nidx, vs->nins, vs->probed, vs->box[0], vs->box[2], vs->box[1], vs->box[3], vs->wmin, vs->wmax,
        st->clip_x, st->clip_y, st->clip_w, st->clip_h, vs->consts[58][0], vs->consts[58][1],
        vs->consts[59][0], vs->consts[59][1]);
    /* Constants: re-uploaded only when they changed. */
    if (!s_kcb_valid || memcmp(s_kcb_data, vs->consts, sizeof s_kcb_data)) {
        memcpy(s_kcb_data, vs->consts, sizeof s_kcb_data);
        ID3D11DeviceContext_UpdateSubresource(C, (ID3D11Resource *)s_kcb, 0, NULL, s_kcb_data, 0, 0);
        s_kcb_valid = 1;
        S.const_uploads++;
    }
    overlay_fit(vs->probed && vs->wmin == 1.0f && vs->wmax == 1.0f, vs->box[0], vs->box[2]);
    s_vcb_data[4] = vs->z_offset;
    s_vcb_data[5] = vs->near_z ? 1.0f : 0.0f;
    s_vcb_data[8] = vs->fog_enable ? 1.0f : 0.0f;
    s_vcb_data[9] = (float)vs->fog_mode;
    s_vcb_data[10] = vs->fog_param[0];
    s_vcb_data[11] = vs->fog_param[1];
    k_vcb(s_vcb_data);

    vo = stride ? ring_put(s_vb2, s_vb2_cap, &s_vb2_pos, in, nverts * stride, stride) : 0;
    io = ring_put(s_ib2, s_ib2_cap, &s_ib2_pos, idx, nidx * 4, 4);
    if (vo < 0 || io < 0)
        return 0;
    if (K.il != e->il) { ID3D11DeviceContext_IASetInputLayout(C, e->il); K.il = e->il; }
    if (!K.topo) { ID3D11DeviceContext_IASetPrimitiveTopology(C, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); K.topo = 1; }
    if (stride && (K.vb != s_vb2 || K.vstride != stride)) {
        ID3D11DeviceContext_IASetVertexBuffers(C, 0, 1, &s_vb2, &stride, &zero);
        K.vb = s_vb2; K.vstride = stride;
    }
    if (K.ib != s_ib2) { ID3D11DeviceContext_IASetIndexBuffer(C, s_ib2, DXGI_FORMAT_R32_UINT, 0); K.ib = s_ib2; }
    if (K.vs != e->vs) { ID3D11DeviceContext_VSSetShader(C, e->vs, NULL, 0); K.vs = e->vs; }
    if (K.cb_vsh != 2) {
        ID3D11Buffer *cb[2] = { s_vcb, s_kcb };
        ID3D11DeviceContext_VSSetConstantBuffers(C, 0, 2, cb);
        K.cb_vsh = 2;
    }
    ID3D11DeviceContext_DrawIndexed(C, nidx, (UINT)(io / 4), stride ? (INT)(vo / stride) : 0);
    S.draws++;
    S.verts += nidx;
    S.vsh_draws++;
    return 1;
}

/* ---- presenting ------------------------------------------------------------
 *
 * The finished frame goes to the screen straight from the GPU: a swap chain
 * on the game window, the render target scaled into it (letterboxed, linear
 * filter), the F6 menu / FPS overlay composited on top. Nothing is read back
 * for that, so the internal resolution costs GPU time only.
 *
 * Guest memory still gets the frame, at the title's own size: a title can
 * read its front buffer, and the GDI window shows it when the GPU does not
 * present (no swap chain, or the video overlay plane in use). That copy is
 * asynchronous -- copied to a staging texture now, read the next frame -- so
 * the CPU never waits for the GPU to finish. */

static const char s_blit_src[] =
    "cbuffer B : register(b0) { float4 uv; };\n"
    "Texture2D T : register(t0); SamplerState S : register(s0);\n"
    "void vs(uint i : SV_VertexID, out float4 p : SV_Position, out float2 t : TEXCOORD0) {\n"
    "  float2 q = float2((i << 1) & 2, i & 2);\n"
    "  p = float4(q * float2(2, -2) + float2(-1, 1), 0, 1); t = q * uv.xy; }\n"
    "float4 ps(float4 p : SV_Position, float2 t : TEXCOORD0) : SV_Target { return T.Sample(S, t); }\n";

static ID3D11VertexShader *s_blit_vs;
static ID3D11PixelShader *s_blit_ps;
static ID3D11Buffer *s_blit_cb;
static ID3D11SamplerState *s_blit_lin, *s_blit_pt;
static ID3D11BlendState *s_blit_alpha;
static IDXGISwapChain *s_swap;
static ID3D11RenderTargetView *s_swap_rtv;
static UINT s_swap_w, s_swap_h;
static HWND s_swap_hwnd;
static int s_swap_failed;
static ID3D11Texture2D *s_ovl_tex;
static ID3D11ShaderResourceView *s_ovl_srv;
static uint32_t s_ovl_w, s_ovl_h;
static int s_ovl_seq = -1;
/* 1x copies for guest memory: render target (for a scaled frame), and two
 * staging textures used in turn. */
static RtEnt s_small;
static ID3D11Texture2D *s_stage[2];
static uint32_t s_stage_w[2], s_stage_h[2];
static struct { int valid; uint32_t va, pitch, bpp, w, h; } s_pend[2];
static int s_stage_i;

static int blit_init(void)
{
    static int done;
    ID3DBlob *b;
    D3D11_BUFFER_DESC bd;
    D3D11_SAMPLER_DESC sd;
    D3D11_BLEND_DESC bl;
    if (done) return s_blit_ps != NULL;
    done = 1;
    if (!(b = compile(s_blit_src, "vs", "vs_4_0"))) return 0;
    ID3D11Device_CreateVertexShader(D, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s_blit_vs);
    ID3D10Blob_Release(b);
    if (!(b = compile(s_blit_src, "ps", "ps_4_0"))) return 0;
    ID3D11Device_CreatePixelShader(D, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s_blit_ps);
    ID3D10Blob_Release(b);
    memset(&bd, 0, sizeof bd);
    bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.ByteWidth = 16;
    ID3D11Device_CreateBuffer(D, &bd, NULL, &s_blit_cb);
    memset(&sd, 0, sizeof sd);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11Device_CreateSamplerState(D, &sd, &s_blit_lin);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    ID3D11Device_CreateSamplerState(D, &sd, &s_blit_pt);
    memset(&bl, 0, sizeof bl);
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ID3D11Device_CreateBlendState(D, &bl, &s_blit_alpha);
    return s_blit_ps != NULL;
}

/* Draw `srv` (the region uv of it) into `rtv` at viewport x,y,w,h. */
static void blit(ID3D11RenderTargetView *rtv, ID3D11ShaderResourceView *srv,
                 float u, float v, float x, float y, float w, float h,
                 ID3D11SamplerState *smp, ID3D11BlendState *bs)
{
    D3D11_VIEWPORT vp = { x, y, w, h, 0, 1 };
    D3D11_RECT sc = { (LONG)x, (LONG)y, (LONG)(x + w), (LONG)(y + h) };
    float cb[4] = { u, v, 0, 0 };
    float bf[4] = { 0, 0, 0, 0 };
    ID3D11ShaderResourceView *none = NULL;
    D3D11_DEPTH_STENCIL_DESC dd;

    memset(&dd, 0, sizeof dd);
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ID3D11DeviceContext_OMSetRenderTargets(C, 1, &rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(C, 1, &vp);
    ID3D11DeviceContext_RSSetScissorRects(C, 1, &sc);
    ID3D11DeviceContext_RSSetState(C, s_rs);
    ID3D11DeviceContext_OMSetBlendState(C, bs, bf, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(C, get_dss(&dd), 0);
    ID3D11DeviceContext_UpdateSubresource(C, (ID3D11Resource *)s_blit_cb, 0, NULL, cb, 0, 0);
    ID3D11DeviceContext_IASetInputLayout(C, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(C, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(C, s_blit_vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(C, 0, 1, &s_blit_cb);
    ID3D11DeviceContext_PSSetShader(C, s_blit_ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(C, 0, 1, &srv);
    ID3D11DeviceContext_PSSetSamplers(C, 0, 1, &smp);
    ID3D11DeviceContext_Draw(C, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(C, 0, 1, &none);
    k_reset();                      /* everything above bypassed the shadow */
}

static void swap_release(void)
{
    if (s_swap_rtv) { ID3D11RenderTargetView_Release(s_swap_rtv); s_swap_rtv = NULL; }
    if (s_swap) { IDXGISwapChain_Release(s_swap); s_swap = NULL; }
}

static int swap_ready(HWND hwnd)
{
    RECT c;
    UINT cw, ch;
    if (s_swap_failed || !hwnd || !blit_init())
        return 0;
    if (!GetClientRect(hwnd, &c))
        return 0;
    cw = (UINT)(c.right - c.left); ch = (UINT)(c.bottom - c.top);
    if (!cw || !ch)
        return 0;                        /* minimised */
    if (s_swap && s_swap_hwnd != hwnd)
        swap_release();
    if (!s_swap) {
        static const GUID iid_dev = { 0x54ec77fa, 0x1377, 0x44e6, { 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c } };
        static const GUID iid_fac = { 0x7b7166ec, 0x21c7, 0x44ae, { 0xb2, 0x1a, 0xc9, 0xae, 0x32, 0x1a, 0xe3, 0x69 } };
        IDXGIDevice *xd = NULL; IDXGIAdapter *ad = NULL; IDXGIFactory *fa = NULL;
        DXGI_SWAP_CHAIN_DESC sd;
        HRESULT hr = E_FAIL;
        memset(&sd, 0, sizeof sd);
        sd.BufferDesc.Width = cw; sd.BufferDesc.Height = ch;
        sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 1;
        sd.OutputWindow = hwnd;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        if (SUCCEEDED(ID3D11Device_QueryInterface(D, &iid_dev, (void **)&xd))
            && SUCCEEDED(IDXGIDevice_GetAdapter(xd, &ad))
            && SUCCEEDED(IDXGIAdapter_GetParent(ad, &iid_fac, (void **)&fa)))
            hr = IDXGIFactory_CreateSwapChain(fa, (IUnknown *)D, &sd, &s_swap);
        if (fa) {
            if (SUCCEEDED(hr))
                IDXGIFactory_MakeWindowAssociation(fa, hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
            IDXGIFactory_Release(fa);
        }
        if (ad) IDXGIAdapter_Release(ad);
        if (xd) IDXGIDevice_Release(xd);
        if (FAILED(hr)) {
            fprintf(stderr, "[GPU-D3D11] no swap chain (%08lX): the window keeps drawing with GDI\n",
                    (unsigned long)hr);
            s_swap_failed = 1;
            s_swap = NULL;
            return 0;
        }
        s_swap_hwnd = hwnd;
        s_swap_w = s_swap_h = 0;
        fprintf(stderr, "[GPU-D3D11] presenting to the window from the GPU\n");
    }
    if (cw != s_swap_w || ch != s_swap_h || !s_swap_rtv) {
        ID3D11Texture2D *bb = NULL;
        static const GUID iid_tex = { 0x6f15aaf2, 0xd208, 0x4e89, { 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c } };
        if (s_swap_rtv) { ID3D11RenderTargetView_Release(s_swap_rtv); s_swap_rtv = NULL; }
        ID3D11DeviceContext_OMSetRenderTargets(C, 0, NULL, NULL);
        k_reset();
        if (s_swap_w && FAILED(IDXGISwapChain_ResizeBuffers(s_swap, 1, cw, ch, DXGI_FORMAT_B8G8R8A8_UNORM, 0)))
            return 0;
        if (FAILED(IDXGISwapChain_GetBuffer(s_swap, 0, &iid_tex, (void **)&bb)))
            return 0;
        ID3D11Device_CreateRenderTargetView(D, (ID3D11Resource *)bb, NULL, &s_swap_rtv);
        ID3D11Texture2D_Release(bb);
        s_swap_w = cw; s_swap_h = ch;
    }
    return s_swap_rtv != NULL;
}

static void overlay_update(void)
{
    uint32_t w, h;
    int seq;
    const uint32_t *px;
    xbox_FramebufferWindowOverlayLock(1);
    px = xbox_FramebufferWindowOverlayImage(&w, &h, &seq);
    if (seq != s_ovl_seq) {
        s_ovl_seq = seq;
        if (px && (!s_ovl_tex || s_ovl_w != w || s_ovl_h != h)) {
            D3D11_TEXTURE2D_DESC td;
            if (s_ovl_srv) { ID3D11ShaderResourceView_Release(s_ovl_srv); s_ovl_srv = NULL; }
            if (s_ovl_tex) { ID3D11Texture2D_Release(s_ovl_tex); s_ovl_tex = NULL; }
            memset(&td, 0, sizeof td);
            td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (SUCCEEDED(ID3D11Device_CreateTexture2D(D, &td, NULL, &s_ovl_tex)))
                ID3D11Device_CreateShaderResourceView(D, (ID3D11Resource *)s_ovl_tex, NULL, &s_ovl_srv);
            s_ovl_w = w; s_ovl_h = h;
        }
        if (px && s_ovl_tex)
            ID3D11DeviceContext_UpdateSubresource(C, (ID3D11Resource *)s_ovl_tex, 0, NULL, px, w * 4, 0);
        if (!px && s_ovl_srv) {
            ID3D11ShaderResourceView_Release(s_ovl_srv); s_ovl_srv = NULL;
            ID3D11Texture2D_Release(s_ovl_tex); s_ovl_tex = NULL;
        }
    }
    xbox_FramebufferWindowOverlayLock(0);
}

/* The frame to the window. Returns 0 if the GPU did not present it. */
static int present_window(RtEnt *rt, uint32_t w, uint32_t h)
{
    HWND hwnd = (HWND)xbox_FramebufferWindowHwnd();
    float cw, ch, dw, dh;
    float black[4] = { 0, 0, 0, 1 };
    static int off = -1;

    if (off < 0) {
        const char *e = getenv("RECOMP_GPU_PRESENT");
        off = e && e[0] == '0';
    }
    if (off || xbox_FramebufferWindowOverlayPlane() || !swap_ready(hwnd)) {
        xbox_FramebufferWindowSetGpuPresent(0);
        return 0;
    }
    cw = (float)s_swap_w; ch = (float)s_swap_h;
    {
        uint32_t aw, ah;
        xbox_DisplayShape(w, h, &aw, &ah);
        if (cw * ah <= ch * aw) { dw = cw; dh = cw * ah / aw; }
        else { dh = ch; dw = ch * aw / ah; }
    }
    rt_resolve(rt);
    ID3D11DeviceContext_ClearRenderTargetView(C, s_swap_rtv, black);
    blit(s_swap_rtv, rt->srv, (float)w / rt->w, (float)h / rt->h,
         (float)(int)((cw - dw) / 2), (float)(int)((ch - dh) / 2), (float)(int)dw, (float)(int)dh,
         s_blit_lin, NULL);
    overlay_update();
    if (s_ovl_srv)
        blit(s_swap_rtv, s_ovl_srv, 1.0f, 1.0f, 0, 0, (float)s_ovl_w, (float)s_ovl_h,
             s_blit_pt, s_blit_alpha);
    IDXGISwapChain_Present(s_swap, 0, 0);
    xbox_FramebufferWindowSetGpuPresent(1);
    return 1;
}

/* The 1x copy for guest memory: scaled frames are shrunk on the GPU first;
 * the copy lands in staging now and is read next time round. */
static void guest_copy(RtEnt *rt, uint32_t va, uint32_t pitch, uint32_t bpp,
                       uint32_t w, uint32_t h)
{
    int cur = s_stage_i, prev = s_stage_i ^ 1;
    ID3D11Resource *src = (ID3D11Resource *)rt->t;
    D3D11_MAPPED_SUBRESOURCE m;
    uint32_t sw = rt->w, sh = rt->h, y, x;

    rt_resolve(rt);
    if (rt->pw != rt->w || rt->ph != rt->h) {
        if (!s_small.t || s_small.w != rt->w || s_small.h != rt->h) {
            if (s_small.t) free_rt(&s_small);
            make_rt_ex(&s_small, rt->w, rt->h, 1);
        }
        if (!s_small.t || !blit_init())
            return;
        blit(s_small.rtv, rt->srv, 1.0f, 1.0f, 0, 0, (float)rt->w, (float)rt->h,
             rt->pw > rt->w * 2 ? s_blit_lin : s_blit_pt, NULL);
        src = (ID3D11Resource *)s_small.t;
    }
    if (!s_stage[cur] || s_stage_w[cur] != sw || s_stage_h[cur] != sh) {
        D3D11_TEXTURE2D_DESC td;
        if (s_stage[cur]) ID3D11Texture2D_Release(s_stage[cur]);
        s_stage[cur] = NULL;
        memset(&td, 0, sizeof td);
        td.Width = sw; td.Height = sh; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(D, &td, NULL, &s_stage[cur])))
            return;
        s_stage_w[cur] = sw; s_stage_h[cur] = sh;
    }
    ID3D11DeviceContext_CopyResource(C, (ID3D11Resource *)s_stage[cur], src);
    s_pend[cur].valid = 1; s_pend[cur].va = va; s_pend[cur].pitch = pitch;
    s_pend[cur].bpp = bpp; s_pend[cur].w = w; s_pend[cur].h = h;
    s_stage_i = prev;
    /* Last time's copy: finished on the GPU by now, so no wait. */
    if (!s_pend[prev].valid || !s_stage[prev])
        return;
    if (FAILED(ID3D11DeviceContext_Map(C, (ID3D11Resource *)s_stage[prev], 0, D3D11_MAP_READ, 0, &m)))
        return;
    {
        uint8_t *dst = gmem() + s_pend[prev].va;
        uint32_t pw = s_pend[prev].w < s_stage_w[prev] ? s_pend[prev].w : s_stage_w[prev];
        uint32_t ph = s_pend[prev].h < s_stage_h[prev] ? s_pend[prev].h : s_stage_h[prev];
        for (y = 0; y < ph; y++) {
            const uint8_t *srow = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
            uint8_t *row = dst + (size_t)y * s_pend[prev].pitch;
            if (s_pend[prev].bpp == 4) {
                memcpy(row, srow, (size_t)pw * 4);
            } else {
                for (x = 0; x < pw; x++) {
                    uint32_t c = ((const uint32_t *)srow)[x];
                    ((uint16_t *)row)[x] = (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0)
                                                    | ((c >> 3) & 0x001F));
                }
            }
        }
    }
    ID3D11DeviceContext_Unmap(C, (ID3D11Resource *)s_stage[prev], 0);
    s_pend[prev].valid = 0;
    S.readbacks++;
}

static void d3d_present_sync(uint32_t va, uint32_t pitch, uint32_t bpp,
                             uint32_t w, uint32_t h)
{
    RtEnt *rt = find_rt(va);

    if (!rt || rt->swizzled || (bpp != 4 && bpp != 2))
        return;
    if (w > rt->w) w = rt->w;
    if (h > rt->h) h = rt->h;
    if (!w || !h)
        return;
    rt->shown = 1;
    present_window(rt, w, h);
    guest_copy(rt, va, pitch, bpp, w, h);
    s_world_prev = s_world_cur;
    s_world_cur = 0;
}

/* The F6 menu's choices, taken between frames. A new internal resolution
 * drops every render target and depth buffer: the title redraws them from
 * the next frame on, at the new size. */
static void apply_gfx_settings(void)
{
    XboxGfxConfig *g = xbox_GfxConfig();
    static int reload_seen = -1;
    uint32_t want = g->scale < 1 ? 1u : (g->scale > 4 ? 4u : (uint32_t)g->scale);
    uint32_t want_ms = g->msaa >= 8 ? 8u : g->msaa >= 4 ? 4u : g->msaa >= 2 ? 2u : 1u;
    int want_wide = g->widescreen != 0;
    int i;

    /* The most samples up to the choice that both formats take. */
    while (want_ms > 1) {
        UINT qc = 0, qd = 0;
        if (SUCCEEDED(ID3D11Device_CheckMultisampleQualityLevels(D, DXGI_FORMAT_B8G8R8A8_UNORM, want_ms, &qc))
            && SUCCEEDED(ID3D11Device_CheckMultisampleQualityLevels(D, DXGI_FORMAT_D24_UNORM_S8_UINT, want_ms, &qd))
            && qc && qd)
            break;
        want_ms >>= 1;
    }
    if (want != s_scale || want_ms != s_msaa || want_wide != s_wide) {
        for (i = 0; i < MAX_RT; i++)
            if (s_rt[i].t) free_rt(&s_rt[i]);
        for (i = 0; i < 16; i++)
            if (s_ds[i].t) {
                ID3D11DepthStencilView_Release(s_ds[i].dsv);
                ID3D11Texture2D_Release(s_ds[i].t);
                memset(&s_ds[i], 0, sizeof s_ds[i]);
            }
        if (s_small.t) free_rt(&s_small);
        for (i = 0; i < 2; i++) {
            if (s_stage[i]) { ID3D11Texture2D_Release(s_stage[i]); s_stage[i] = NULL; }
            s_pend[i].valid = 0;
        }
        k_reset();
        ID3D11DeviceContext_OMSetRenderTargets(C, 0, NULL, NULL);
        s_cur_rt = NULL;
        s_scale = want;
        s_msaa = want_ms;
        s_wide = want_wide;
        fprintf(stderr, "[GPU-D3D11] internal resolution %ux (%ux%u)%s, antialiasing %s%u\n",
                s_scale, (s_wide ? 853 : 640) * s_scale, 480 * s_scale, s_wide ? " 16:9" : "",
                s_msaa > 1 ? "MSAA " : "off", s_msaa > 1 ? s_msaa : 0);
    }
    g->scale_applied = (int)s_scale;
    g->msaa_applied = (int)s_msaa;
    if (reload_seen != g->reload_seq) {
        reload_seen = g->reload_seq;
        tex_cache_clear();
        repl_scan();
    }
}

static void d3d_end_frame(void)
{
    static DWORD last;
    DWORD now = GetTickCount();
    s_frame++;
    dbg_poll();
    apply_gfx_settings();
    if (s_stats < 0) {
        const char *e = getenv("RECOMP_GPU_STATS");
        s_stats = e && *e && *e != '0';
    }
    if (s_stats && now - last > 5000) {
        fprintf(stderr, "[GPU-D3D11] frame %u: %u draws (%u by vertex program) %u verts %u clears,"
                " %u uploads (%.1f MB), %u render-target reads, %d pixel shaders, %d vertex"
                " programs, %u constant uploads\n",
                s_frame, S.draws, S.vsh_draws, S.verts, S.clears, S.tex_uploads,
                (double)S.tex_bytes / 1048576.0, S.tex_hits_rt, s_nps, s_nvsc, S.const_uploads);
        memset(&S, 0, sizeof S);
        last = now;
    }
}

static const Nv2aGpuBackend s_backend = {
    d3d_clear, d3d_draw, d3d_present_sync, d3d_end_frame, d3d_draw_vsh
};

int nv2a_d3d11_init(void)
{
    static const D3D11_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,   D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "FOG",      0, DXGI_FORMAT_R32_FLOAT,          0, 48,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 52,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 68,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 84,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 100, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    const char *e = getenv("RECOMP_GPU");
    D3D_FEATURE_LEVEL fl;
    ID3DBlob *b;
    D3D11_BUFFER_DESC bd;
    D3D11_RASTERIZER_DESC rd;
    HRESULT hr;

    if (e && *e == '0')
        return 0;
    {
        /* Shader model 4: feature level 10.0 or better. An older adapter
         * keeps the software rasteriser rather than failing shader by shader. */
        static const D3D_FEATURE_LEVEL want[] = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
        };
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, want, 4,
                               D3D11_SDK_VERSION, &D, &fl, &C);
        if (hr == E_INVALIDARG)        /* the 11.1 runtime is missing (Windows 7) */
            hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, want + 1, 3,
                                   D3D11_SDK_VERSION, &D, &fl, &C);
    }
    if (FAILED(hr)) {
        fprintf(stderr, "[GPU-D3D11] no hardware device (%08lX); using the CPU rasteriser\n",
                (unsigned long)hr);
        return 0;
    }
    if (sizeof(Nv2aGpuVertex) != 116) {
        fprintf(stderr, "[GPU-D3D11] unexpected vertex size %u\n", (unsigned)sizeof(Nv2aGpuVertex));
        return 0;
    }
    b = compile(s_vs_src, "vs", "vs_4_0");
    if (!b) return 0;
    ID3D11Device_CreateVertexShader(D, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s_vs);
    hr = ID3D11Device_CreateInputLayout(D, il, sizeof il / sizeof il[0],
                                        ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), &s_layout);
    ID3D10Blob_Release(b);
    if (FAILED(hr)) return 0;
    b = compile(s_clear_src, "vs", "vs_4_0");
    if (!b) return 0;
    ID3D11Device_CreateVertexShader(D, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s_clear_vs);
    ID3D10Blob_Release(b);
    b = compile(s_clear_src, "ps", "ps_4_0");
    if (!b) return 0;
    ID3D11Device_CreatePixelShader(D, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s_clear_ps);
    ID3D10Blob_Release(b);

    memset(&bd, 0, sizeof bd);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.ByteWidth = sizeof s_vcb_data;
    ID3D11Device_CreateBuffer(D, &bd, NULL, &s_vcb);
    bd.ByteWidth = (sizeof(PsConst) + 15) & ~15u;
    ID3D11Device_CreateBuffer(D, &bd, NULL, &s_pcb);
    bd.ByteWidth = 32;
    ID3D11Device_CreateBuffer(D, &bd, NULL, &s_ccb);
    s_vb_cap = 3 * 40000;
    bd.ByteWidth = s_vb_cap * sizeof(Nv2aGpuVertex);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(D, &bd, NULL, &s_vb))) return 0;
    /* Translated vertex programs: raw inputs and triangle indices. */
    s_vb2_cap = 32u << 20;
    bd.ByteWidth = s_vb2_cap;
    if (FAILED(ID3D11Device_CreateBuffer(D, &bd, NULL, &s_vb2))) s_vb2 = NULL;
    s_ib2_cap = 8u << 20;
    bd.ByteWidth = s_ib2_cap;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    if (FAILED(ID3D11Device_CreateBuffer(D, &bd, NULL, &s_ib2))) s_vb2 = NULL;
    memset(&bd, 0, sizeof bd);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.ByteWidth = sizeof s_kcb_data;
    if (FAILED(ID3D11Device_CreateBuffer(D, &bd, NULL, &s_kcb))) s_vb2 = NULL;

    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;     /* the software path draws both faces */
    rd.DepthClipEnable = FALSE;        /* ...and does not clip at the far plane */
    rd.ScissorEnable = TRUE;
    ID3D11Device_CreateRasterizerState(D, &rd, &s_rs);

    {
        XboxGfxConfig *g = xbox_GfxConfig();
        IDXGIDevice *xd = NULL;
        IDXGIAdapter *ad = NULL;
        static const GUID iid_dxgidev = { 0x54ec77fa, 0x1377, 0x44e6, { 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c } };
        strcpy(g->adapter, "Direct3D 11");
        if (SUCCEEDED(ID3D11Device_QueryInterface(D, &iid_dxgidev, (void **)&xd))
            && SUCCEEDED(IDXGIDevice_GetAdapter(xd, &ad))) {
            DXGI_ADAPTER_DESC desc;
            if (SUCCEEDED(IDXGIAdapter_GetDesc(ad, &desc)))
                WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, g->adapter,
                                    sizeof g->adapter, NULL, NULL);
        }
        if (ad) IDXGIAdapter_Release(ad);
        if (xd) IDXGIDevice_Release(xd);
        g->gpu_active = 1;
        fprintf(stderr, "[GPU-D3D11] adapter: %s\n", g->adapter);
    }
    nv2a_gpu_backend_register(&s_backend);
    fprintf(stderr, "[GPU-D3D11] NV2A drawing on the host GPU (feature level %X)\n", (unsigned)fl);
    return 1;
}

#else  /* !_WIN32 */
int nv2a_d3d11_init(void) { return 0; }
#endif
