# The Sims Bustin' Out — native Windows port

A static recompilation of the Xbox game *The Sims Bustin' Out* (USA) into a
64-bit Windows program. The game's code is translated function by function
into C and compiled natively: no emulator runs underneath it. Rendering goes
through Direct3D 11 on your graphics card.

**No game code or data is in this repository.** You build the port from your
own disc image, and the finished program asks for that image on first run.

## What's here

| Folder | Contents |
|---|---|
| `sims_bustinout/` | The port: start-up and ISO setup (`src/`), hand-written translations, the recompiler's seed lists, the post-processing script, CMake files, `build_windows.bat`, and the player README and `settings.ini` (`dist/`). |
| `xboxrecomp/` | The xboxrecomp 0.17.0 toolkit (MIT, by sp00nz) with this port's changes applied. `sims_bustinout/../xboxrecomp` is where the build looks for it. |

The generated C (`sims_bustinout/src/recomp/gen/`) and built `.exe` files are
derived from the game's own code, so they are not committed; the build makes
them from your disc.

## Building

Needs Python 3.10+, CMake 3.20+, and Visual Studio 2022 (Desktop C++) or
MinGW-w64.

```bat
cd sims_bustinout
build_windows.bat "C:\path\to\Sims, The - Bustin' Out (USA).iso"
```

It extracts `default.xbe` from the image, analyses it, translates about 26,000
functions to C, and compiles `SimsBustinOut.exe` (10–30 minutes).

## Changes to the toolkit

All under `xboxrecomp/src/`:

- **Direct3D 11 renderer** (`kernel/nv2a_d3d11.c`, new): register combiners
  and NV2A vertex programs as generated HLSL, render-target and texture caches,
  1x–4x internal resolution, swap-chain presentation, texture dump/replace
  (PNG).
- **Pushbuffer walking** (`kernel/nv2a_pb_scan.c`, `kernel/nv2a_pb_exec.c`):
  DMA walk with stricter command validation and resync; frame pacing through
  the flip hold with a frame-rate cap (`kernel/nv2a_flip_hold.*`).
- **Kernel** (`kernel/kernel_bridge.c`): GPU callbacks and D3D wake events
  delivered when the GPU reaches them rather than at the next vblank; 60 Hz or
  120 Hz vblank (the experimental 60 fps mode); per-call timing in the log.
- **Window** (`video/fb_present.c`, `video/gfx_config.h`): the F6 graphics
  menu (including the direct-movement and camera settings), FPS counter, `settings.ini` persistence, and a "Working, please
  wait..." note while the game draws nothing (saving, quitting).
- **Input** (`input/xinput_device.c`, `input/xinput_xbox.h`): a hook so the
  game side can read and rewrite the pad each frame (used by direct movement).
- **Diagnostics** (`kernel/xbox_memory_layout.c`, `kernel/kernel_bridge.c`): a
  memory-write watch (`RECOMP_MEMWATCH`), engine watchdog and
  sampling profiler (`[EPROF]` lines), GPU draw debugging (`RECOMP_GPU_DEBUG`).
- Rendering fixes: depth writes follow the NV2A/OpenGL rule (no write with the
  depth test off) — this fixed the black patches in house interiors; border
  sampling clamps unless a border colour is set.

## Direct movement (experimental)

`sims_bustinout/src/direct_move.c`, with hooks added to the generated code by
`tools/postprocess_gen.py`. Turned on in the F6 menu (Left stick: "Moves your
Sim"). The left stick walks the Sim by feeding the game's own router short
waypoints, steering round obstacles; doors open and close as she passes by
running the game's own door trees; the stick cancels a busy action. The right
stick orbits and zooms a follow camera tuned from F6. A opens the lit object's
menu, or, with nothing lit, the game's chooser bubbles listing every nearby
object that has interactions. See `sims_bustinout/dist/README.txt`.

## Playing

See `sims_bustinout/dist/README.txt` (first run, controls, saves, the F6 menu).
