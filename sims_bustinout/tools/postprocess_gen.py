#!/usr/bin/env python3
"""Post-process the generated C for The Sims Bustin' Out.

Run after `tools.recomp` has written src/recomp/gen/*.c:

    python postprocess_gen.py path/to/src/recomp/gen

1. PFB flush acknowledge. XDK D3D's KickOff sets bit 16 of NV2A register
   0x100410 and spins until the GPU clears it. Here nothing clears it except
   the executor thread between passes, so a kickoff could spin for a whole
   rendered frame (40% of CPU time in a profile). The flush it waits for
   (write-combine buffers reaching RAM) has no meaning for a host CPU, so the
   loop is rewritten to clear the bit itself: the wait ends at once.

2. Polling loops on the GPU's fence latch (PGRAPH 0x400B10) get a yield, so
   the waiting thread gives its core to the executor that will satisfy it.

Idempotent: a file already processed is left as it is.
"""
import os
import re
import sys

MARK = "/* sims-postprocess */"

PFB_LOOP = re.compile(
    r"(loc_(?P<lbl>[0-9A-F]{8}): ;\n)"
    r"(?P<body>    _fa = \(uint32_t\)\(MEM32\((?P<reg>\w+) \+ 0x100410\)\) & 0xFFFFFFFFu; "
    r"_fb = \(uint32_t\)\(0x10000\) & 0xFFFFFFFFu;\n)")

FENCE_LOOP = re.compile(
    r"(    if \((?:TEST|CMP)_\w+\(_fa, _fb\)\) goto loc_(?P<lbl>[0-9A-F]{8});[^\n]*\n)")


def func_span(text, name):
    """(start, end) of a generated function's body, or None."""
    m = re.search(r"^void %s\(void\)\n\{" % name, text, re.M)
    if not m:
        return None
    end = text.find("\n}\n", m.end())
    return (m.start(), end) if end > 0 else None


def patch_in(text, name, old, new):
    """Replace `old` with `new` once, inside function `name` only."""
    span = func_span(text, name)
    if not span:
        return text, 0
    body = text[span[0]:span[1]]
    if new in body or old not in body:
        return text, 0
    return text[:span[0]] + body.replace(old, new, 1) + text[span[1]:], 1


def title_patches(text):
    n = 0
    # 0. Direct movement (direct_move.c): the cursor controller's per-frame
    #    update calls the mod first, with g_ecx = the controller.
    text, k = patch_in(text, "sub_00039000", "loc_00039000: ;\n",
                       "loc_00039000: ;\n    { extern void sims_dm_cursor(void); sims_dm_cursor(); } "
                       + MARK + "\n")
    n += k
    # 0b. Direct movement: the walk router's per-tick update (sub_000B5BB0,
    #     g_ecx = the router) lets the mod steer the path ahead.
    text, k = patch_in(text, "sub_000B5BB0", "loc_000B5BB0: ;\n",
                       "loc_000B5BB0: ;\n    { extern void sims_dm_router(void); sims_dm_router(); } "
                       + MARK + "\n")
    n += k
    # 0c. Direct movement: the "animate" primitive (sub_000B5F60, g_ecx =
    #     the router, args: the VM stack frame and the operand). The mod
    #     notes which behaviour tree is animating (her idle loop or not); while
    #     the stick walks her out of the idle loop it holds that animation
    #     (returns 2, still playing), and ends it (1, done) when she stops.
    text, k = patch_in(text, "sub_000B5F60", "loc_000B5F60: ;\n",
                       "loc_000B5F60: ;\n    { extern int sims_dm_anim(void); int _dm = sims_dm_anim(); if (_dm) "
                       "{ eax = (uint32_t)_dm; g_ebp = ebp; g_seh_ebp = ebp; esp += 12; return; } } "
                       + MARK + "\n")
    n += k
    # 0d. Direct movement: the cursor controller's draw (sub_0003A960, g_ecx
    #     = the controller, one argument). In live mode with the left stick
    #     moving the Sim, the cursor (beam and rings) is not drawn; the Sim's
    #     own diamond is all that shows.
    text, k = patch_in(text, "sub_0003A960", "loc_0003A960: ;\n",
                       "loc_0003A960: ;\n    { extern int sims_dm_hide_cursor(void); if (sims_dm_hide_cursor()) "
                       "{ g_ebp = ebp; g_seh_ebp = ebp; esp += 8; return; } } " + MARK + "\n")
    n += k
    # 0e. ... and the rings under the cursor, which the HUD manager draws at
    #     the cursor's position (sub_00055950, its argument the controller).
    text, k = patch_in(text, "sub_00055950", "loc_00055950: ;\n",
                       "loc_00055950: ;\n    { extern int sims_dm_hide_rings(void); if (sims_dm_hide_rings()) "
                       "{ g_ebp = ebp; g_seh_ebp = ebp; esp += 8; return; } } " + MARK + "\n")
    n += k
    # 0g. Direct movement: the router's checks on her steps pass while the
    #     stick steers her (sub_001ACF00, thiscall, 4 args).
    text, k = patch_in(text, "sub_001ACF00", "loc_001ACF00: ;\n",
                       "loc_001ACF00: ;\n    { extern int sims_dm_stand_ok(void); if (sims_dm_stand_ok()) "
                       "{ eax = 1; g_ebp = ebp; g_seh_ebp = ebp; esp += 20; return; } } " + MARK + "\n")
    n += k
    # 3. Multisample type for the 480-line modes (sub_00128BD0): the title
    #    asks D3D for 0x2222, 4x supersampling, so every frame is drawn at
    #    1280x960 and filtered down. The pushbuffer executor draws on the CPU,
    #    so that is four times the work. sims_multisample() returns 0x11
    #    (none) unless settings.ini asks for the original.
    text, k = patch_in(text, "sub_00128BD0", "    eax = 0x2222;",
                       "    { extern uint32_t sims_multisample(uint32_t); "
                       "eax = sims_multisample(0x2222); } " + MARK)
    n += k
    # 3b. The title's own supersample scale (2.0 for the 480-line modes, read
    #     from the shared constant at 0x2E2394) in the three functions that
    #     set up viewport/projection scaling next to the multisample choice.
    #     It must follow sims_multisample(), or the scene is drawn at 2x zoom.
    for fn in ("sub_00128BF0", "sub_00128C60"):
        text, k = patch_in(text, fn, "    xmm0 = XMM_SCALAR(MEMF(0x2E2394)); /* movss */",
                           "    { extern float sims_ss_scale(unsigned); "
                           "xmm0 = XMM_SCALAR(sims_ss_scale(0x%sU)); } " % fn[4:] + MARK)
        n += k
    # 3c. D3D's vblank handler (sub_00251FF0) acks PCRTC_INTR and spins until
    #     PMC_INTR's PCRTC bit drops. On hardware the ack drops it; here a
    #     host thread notices the write later, so the handler spun (10% of
    #     the CPU in a title-screen profile). sims_vblank_ack() does what the
    #     write-1-to-clear registers would.
    text, k = patch_in(text, "sub_00251FF0",
                       "loc_002520A0: ;\n    MEM32(edi + 0x600100) = ecx;\n",
                       "loc_002520A0: ;\n    MEM32(edi + 0x600100) = ecx;\n"
                       "    { extern void sims_vblank_ack(uint32_t); sims_vblank_ack(edi); } "
                       + MARK + "\n")
    n += k
    # 4. D3D's make-space wait (sub_0024D300, loc_0024D430) spins on the GPU
    #    semaphore; yield so the executor that advances it gets the core.
    text, k = patch_in(text, "sub_0024D300", "loc_0024D430: ;\n",
                       "loc_0024D430: ;\n    { extern void sims_spin_yield(void); "
                       "sims_spin_yield(); } " + MARK + "\n")
    n += k
    return text, n


def process(text):
    text, n_title = title_patches(text)
    n_pfb = n_title

    def pfb(m):
        nonlocal n_pfb
        if text.count(MARK) and "MEM32(%s + 0x100410) &= ~0x10000u; %s" % (m.group("reg"), MARK) in text[m.end(1):m.end(1)+120]:
            return m.group(0)
        n_pfb += 1
        return (m.group(1) +
                "    MEM32(%s + 0x100410) &= ~0x10000u; %s\n" % (m.group("reg"), MARK) +
                m.group("body"))

    text = PFB_LOOP.sub(pfb, text)

    # Fence-latch loops: a backward goto inside a block that read 0x400B10.
    n_fence = 0
    out = []
    pos = 0
    for m in FENCE_LOOP.finditer(text):
        lbl = m.group("lbl")
        start = text.rfind("loc_%s: ;" % lbl, 0, m.start())
        if start < 0 or m.start() - start > 1500:
            continue
        if "+ 0x400B10)" not in text[start:m.start()]:
            continue
        if MARK in text[max(start, m.start() - 200):m.start()]:
            continue
        out.append(text[pos:m.start()])
        out.append("    { extern void sims_spin_yield(void); sims_spin_yield(); } %s\n" % MARK)
        out.append(m.group(1))
        pos = m.end()
        n_fence += 1
    out.append(text[pos:])
    text = "".join(out)
    return text, n_pfb, n_fence


def main():
    gen = sys.argv[1] if len(sys.argv) > 1 else "src/recomp/gen"
    tot_p = tot_f = 0
    for name in sorted(os.listdir(gen)):
        if not name.endswith(".c"):
            continue
        path = os.path.join(gen, name)
        with open(path, encoding="utf-8", errors="surrogateescape") as f:
            text = f.read()
        new, p, fe = process(text)
        if new != text:
            with open(path, "w", encoding="utf-8", errors="surrogateescape") as f:
                f.write(new)
        tot_p += p
        tot_f += fe
    print("postprocess: %d PFB flush loops acked, %d fence loops yield" % (tot_p, tot_f))


if __name__ == "__main__":
    main()
