/**
 * The Sims Bustin' Out - Recompiled Game Entry Point
 *
 * This is the Windows executable that hosts the recompiled game code.
 * It performs the following initialization sequence:
 *
 * 1. Load the original XBE file from disk
 * 2. Initialize the Xbox memory layout (map data sections to original VAs)
 * 3. Initialize the Xbox kernel replacement layer
 * 4. Initialize the kernel bridge (thunk table in Xbox memory)
 * 5. Set up game file paths for I/O redirection
 * 6. Initialize the stack pointer
 * 7. Install VEH crash handler for diagnostics
 * 8. Call the game's original entry point (recompiled)
 *
 * Customize this file for your game:
 *   - Set YOUR_GAME_ENTRY_POINT to the XBE entry point address
 *   - Set YOUR_GAME_XBE_PATH to where the XBE file lives
 *   - Set YOUR_GAME_DIR to the game data directory
 *   - Add any CRT global pre-initialization your game needs
 *   - Customize the VEH handler for game-specific crash diagnosis
 *
 * XBE Details (fill in from xbe_parser output):
 *   Title:       The Sims Bustin' Out
 *   Title ID:    0x00000000
 *   Base addr:   0x00010000
 *   Entry point: 0x00000000
 *   Code size:   ~??? KB (.text)
 *   Sections:    ?? (list them)
 *   Kernel imports: ??
 */

#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#include <io.h>
#include <shellapi.h>
#include "iso_setup.h"

/* ── Launcher helpers (Sims BO) ────────────────────────────── */

static char g_exe_dir[MAX_PATH];

static void set_default_env(const char *name, const char *value)
{
    if (!getenv(name)) {
        char kv[512];
        snprintf(kv, sizeof(kv), "%s=%s", name, value);
        _putenv(kv);
        SetEnvironmentVariableA(name, value);
    }
}

/* settings.ini beside the exe: one NAME=VALUE per line, '#' or ';' comments.
 * Values there override the built-in defaults below; real environment
 * variables override both. */
static void load_settings_ini(void)
{
    char path[MAX_PATH], line[512];
    FILE *f;
    snprintf(path, sizeof(path), "%s\\settings.ini", g_exe_dir);
    f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        char *eq, *e, *k = line;
        while (*k == ' ' || *k == '\t') k++;
        if (*k == '#' || *k == ';' || *k == '[' || !*k) continue;
        eq = strchr(k, '=');
        if (!eq) continue;
        *eq = 0;
        for (e = eq - 1; e >= k && (*e == ' ' || *e == '\t'); e--) *e = 0;
        {
            char *v = eq + 1;
            while (*v == ' ' || *v == '\t') v++;
            for (e = v + strlen(v) - 1; e >= v && (*e == '\n' || *e == '\r' || *e == ' '); e--) *e = 0;
            set_default_env(k, v);
        }
    }
    fclose(f);
}

static void apply_title_defaults(void)
{
    /* The title statically links the XDK's D3D8, so it talks to the NV2A
     * directly: the pushbuffer has to be executed and vblanks raised. */
    set_default_env("RECOMP_PB_EXEC", "1");
    set_default_env("RECOMP_VBLANK", "paced");
    set_default_env("RECOMP_FB_WINDOW", "1");
    /* DirectSound is the XDK's own and probes the AC'97 codec. */
    set_default_env("RECOMP_AC97_READY", "1");
    set_default_env("RECOMP_TEX_BILINEAR", "1");
    /* Events the XDK builds inline (no KeInitializeEvent import). */
    set_default_env("RECOMP_TITLE_KEVENTS", "1");
    /* Give freed contiguous memory back: the title frees and reallocates its
     * 1280x960 render targets around every movie, and 64 MB does not hold
     * two sets. */
    set_default_env("RECOMP_HEAP_RECLAIM", "1");
    /* D3D's GPU-time wait compares PGRAPH 0x400B10 with the semaphore. */
    set_default_env("RECOMP_PB_LATCH_0310", "1");
    /* Walk the pushbuffer as the GPU does: follow every JUMP from GET to PUT.
     * This title's D3D wraps its ring to the middle as often as to the
     * start, which the PUT-to-PUT segment scan could only guess at. */
    set_default_env("RECOMP_PB_WALK", "dma");
    /* Flips wait for the vblank their swap asked for, as on the console:
     * frame pacing comes from the flip, not from when a GPU callback lands. */
    set_default_env("RECOMP_FLIP_PACING", "1");
    /* Pads: the title's own XAPI drives the USB host controllers. The
     * keyboard doubles as pad 1. */
    set_default_env("RECOMP_USB", "1");
    set_default_env("RECOMP_KEYBOARD", "1");
    {
        char saves[MAX_PATH];
        snprintf(saves, sizeof(saves), "%s\\saves", g_exe_dir);
        CreateDirectoryA(saves, NULL);
        set_default_env("RECOMP_SAVE_DIR", saves);
    }
}

/* Pull a disc-image path out of the command line (drag-and-drop onto the exe
 * passes it as the first argument). */
static const char *iso_from_cmdline(void)
{
    static char iso[MAX_PATH * 2];
    int argc = 0, i;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return NULL;
    for (i = 1; i < argc; i++) {
        size_t n = wcslen(argv[i]);
        if ((n > 4 && _wcsicmp(argv[i] + n - 4, L".iso") == 0) ||
            (n > 5 && _wcsicmp(argv[i] + n - 5, L".xiso") == 0)) {
            WideCharToMultiByte(CP_ACP, 0, argv[i], -1, iso, sizeof(iso), NULL, NULL);
            LocalFree(argv);
            return iso;
        }
    }
    LocalFree(argv);
    return NULL;
}

/* xboxrecomp runtime headers */
#include <xbox/xboxrecomp.h>

/*
 * If xboxrecomp.h is not an umbrella header in your setup, include
 * the individual headers directly:
 *
 * #include "kernel.h"
 * #include "xbox_memory_layout.h"
 * #include "d3d8_xbox.h"
 * #include "dsound_xbox.h"
 * #include "xinput_xbox.h"
 */

/* ── Global register state (defined in xbox_memory_layout.c) ── */

/* RECOMP_TLS is not optional here. The runtime defines these thread-local, and
 * a plain `extern` referencing a __declspec(thread) variable does not resolve
 * to the calling thread's copy -- it resolves to the image's TLS template. The
 * host side then writes g_esp somewhere the generated code never reads, so the
 * guest starts with every register at zero and faults immediately, having
 * apparently ignored the setup that visibly ran. */
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern RECOMP_TLS uint32_t g_seh_ebp;
/* x87 and SSE state. Global for the same reason the volatile GPRs are: one
 * guest routine can lift to several C functions, so a value written in one
 * body is read in the next. Defined in xbox_memory_layout.c like the rest of
 * the register file. */
extern RECOMP_TLS double g_fp_stack[8];
extern RECOMP_TLS int g_fp_top;
extern RECOMP_TLS uint16_t g_fp_control_word;
extern RECOMP_TLS uint32_t g_mxcsr;
extern RECOMP_TLS int g_fp_cmp;
extern RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
extern RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;
extern ptrdiff_t g_xbox_mem_offset;

/* ── XBE Constants ─────────────────────────────────────────── */

/*
 * TODO: Set these from your xbe_parser output.
 * Run: py -3 -m tools.xbe_parser game/default.xbe
 */
#define YOUR_GAME_ENTRY_POINT   0x00100EA2  /* XBE entry point VA */
#define YOUR_GAME_XBE_PATH      "game\\default.xbe"
#define YOUR_GAME_DIR            "game"

/* ── Forward declarations ──────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size);

/* Recompiled entry point (generated by recomp pipeline) */
extern void xbe_entry_point(void);
extern int recomp_dispatch_init(void);

/* ── VEH crash handler ─────────────────────────────────────── */

/*
 * Vectored Exception Handler for crash diagnostics.
 *
 * When the recompiled game hits an access violation, this handler prints
 * the faulting address, all Xbox register values, and a native stack trace.
 * This is your primary debugging tool during bring-up.
 *
 * Customize this for your game:
 *   - Add game-specific address checks (GPU register probes, etc.)
 *   - Add dumps of game-specific globals (heap handles, state flags)
 *   - Add SEH simulation if your game uses __try/__except
 */
/* Name the guest function a fault happened in, and recover the call chain.
 *
 * Recompiled code faults as ordinary native code, so the exception record
 * carries a host RIP and nothing else -- there is no guest program counter to
 * report, and the host address changes every build. Two things recover the
 * guest view:
 *
 *   - every generated function is a real symbol in the image (sub_005A03C0
 *     and so on), so the linker's PDB already maps host address back to guest
 *     function. dbghelp turns an anonymous RIP into that name.
 *
 *   - every lifted call pushes its guest return address onto the guest stack
 *     before jumping, so the stack still holds the chain. Scanning up from esp
 *     for values inside the code sections recovers it.
 *
 * ponytail: the stack scan is a scan, not a frame walk -- these are FPO frames
 * with no reliable ebp chain, so there is nothing to walk. It over-reports,
 * since addresses from returned-from calls linger below esp, but naming the
 * guest function is the whole question at a fault.
 *
 * Requires linking dbghelp and keeping the .pdb beside the .exe.
 */
static void print_guest_context(void *rip)
{
    /* SYMBOL_INFO is variable-length: the name is written past the struct, so
     * it must be over-allocated with MaxNameLen set to the slack. */
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;

    memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)rip, &disp, sym))
        fprintf(stderr, "  in %s+0x%llX\n",
                sym->Name, (unsigned long long)disp);

    if (g_xbox_mem_offset && g_esp) {
        const uint32_t *sp =
            (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        int shown = 0, i;
        fprintf(stderr, "  guest stack (return addresses, innermost first):\n");
        for (i = 0; i < 256 && shown < 24; i++) {
            uint32_t v = sp[i];
            if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "    [esp+%-4d] 0x%08X\n", i * 4, v);
                shown++;
            }
        }
        /* When the frame is gone, say so, and fall back on what is left.
         *
         * A fault inside a block copy leaves no return address in range: the
         * copy is a host memmove and its caller's frame is further up than
         * this reaches. Worse, a copy that ran past its buffer overwrites the
         * stack itself, so the list comes out empty exactly when it is most
         * needed -- and an empty list under a promising heading reads like
         * "nothing to see" rather than "the evidence is destroyed". */
        if (!shown)
            fprintf(stderr, "    (no return addresses in range)\n");

        /* Everything below only pays for itself when the scan came back
         * empty. A crash where the chain was recovered already says what
         * happened, and burying that under another twenty lines makes the
         * common report worse to read. */
        if (!shown) {
            uint32_t k;

            /* The indirect-call history, which the stack cannot overwrite.
             * recomp_manual.c prints this too, but only from
             * recomp_icall_fail_log -- a page fault inside a block copy
             * never reaches that path, and this is the same information by
             * the route the fault actually took.
             *
             * Declared here rather than included: main.c does not pull in
             * recomp_types.h, and recomp_manual.c spells it the same way. */
            extern volatile uint32_t g_icall_trace[16];
            extern volatile uint32_t g_icall_trace_idx;

            fprintf(stderr, "  recent ICALL targets:");
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %08X",
                        g_icall_trace[(g_icall_trace_idx + k) & 15]);
            fprintf(stderr, "\n");

            /* The frame unfiltered. The pointers and lengths handed to
             * whatever faulted live here and look nothing like code, so the
             * filter above drops exactly what is wanted. On the crash this
             * was written for, one of these words was the end of the mapped
             * RAM mirror -- which is what identified the fault as a copy
             * running off its buffer rather than a stray pointer. */
            for (i = 0; i < 20; i++)
                fprintf(stderr, "    raw[esp+%-4d] 0x%08X\n", i * 4, sp[i]);
        }
    }
}

/* The emulated APU.
 *
 * Two entry points that had no callers anywhere in the tree. apu_mmio_hook.c
 * documents apu_hook_handle_mmio as "called from VEH in main.c" and no main.c
 * called it; apu_decode_and_handle's first line is `if (!g_apu_state) return
 * false` and nothing assigned g_apu_state. So with RECOMP_AC97_READY set the
 * APU's registers were unmapped to be trapped, and every trap was then
 * declined and surfaced as an access violation on the first register
 * DirectSound touched. The VEH below now reaches apu_hook_handle_mmio
 * through the toolkit's xbox_VehMmioFault.
 *
 * Declared rather than included so this does not depend on src/apu being on
 * the include path. */
typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern MCPXAPUState *g_apu_state;

static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t fault_addr = ep->ExceptionRecord->ExceptionInformation[1];

        /*
         * GPU register probe at 0xFD000000 range.
         * Some games probe NV2A registers directly. On real hardware this
         * returns GPU state; here we just skip the instruction.
         * TODO: Implement mini x86-64 decoder for instruction skipping,
         * or connect to the xbox_nv2a library for proper handling.
         */
        if (fault_addr >= 0xFD000000 && fault_addr < 0xFE000000) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        /* Trapped hardware registers (APU, USB OHCI) -> their models.
         *
         * The other half of RECOMP_AC97_READY: reporting the codec ready is
         * what lets DirectSoundCreate past its first gate, and from there it
         * drives the APU directly, so the registers have to fault to be seen.
         * "Handled" means the access was decoded and the instruction stepped
         * over, so execution resumes instead of unwinding. */
        if (xbox_VehMmioFault(ep))
            return EXCEPTION_CONTINUE_EXECUTION;

        fprintf(stderr, "[CRASH] Access violation at RIP=0x%llX, fault addr=0x%llX (%s)\n",
            (unsigned long long)ep->ContextRecord->Rip,
            (unsigned long long)fault_addr,
            ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read");
        fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
            g_eax, g_ecx, g_edx, g_esp);
        fprintf(stderr, "  Xbox regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n",
            g_ebx, g_esi, g_edi);
        fprintf(stderr, "  Xbox VA of fault: 0x%08X\n",
            (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset));
        print_guest_context((void *)ep->ContextRecord->Rip);

        /*
         * TODO: Add game-specific diagnostics here. Examples:
         *
         * Dump CRT heap handle:
         *   uint32_t heap = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + HEAP_HANDLE_VA);
         *   fprintf(stderr, "  CRT heap handle: 0x%08X\n", heap);
         *
         * Dump game state:
         *   uint32_t state = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + GAME_STATE_VA);
         *   fprintf(stderr, "  Game state: %u\n", state);
         */

        /* Print native stack return addresses for debugging */
        {
            uintptr_t *sp = (uintptr_t *)ep->ContextRecord->Rsp;
            fprintf(stderr, "  Native stack (first 8 return addrs):\n");
            for (int i = 0; i < 64 && sp[i]; i++) {
                if (sp[i] >= 0x140000000ULL && sp[i] < 0x150000000ULL) {
                    fprintf(stderr, "    [%d] 0x%llX\n", i, (unsigned long long)sp[i]);
                }
            }
        }
        fflush(stderr);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

/* ── Title-specific GPU glue ───────────────────────────────── */

/* The title links the XDK's D3D8 statically. Its device lives at a fixed
 * address (the pointer at 0x00264948 -> 0x00264950), and device+0x30 holds
 * the GPU semaphore: the word the NV2A writes with
 * BACK_END_WRITE_SEMAPHORE_RELEASE as it finishes work. D3D's make-space and
 * fence waits poll it. The pushbuffer executor writes those releases only
 * once it has been told where the word is, so a host thread waits for D3D to
 * create its device and then hands the address over. Without this the title
 * spins forever in its first make-space wait (sub_0024D300). */
#define SIMS_D3D_DEVICE_PTR_VA  0x00264948u
#define SIMS_D3D_SEMAPHORE_OFF  0x30u

extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);

extern void xbox_DebugDumpThreads(uint32_t code_lo, uint32_t code_hi);

/* Called from the guest's GPU-wait loops (tools/postprocess_gen.py): give the
 * core to the pushbuffer executor, which is what the loop is waiting for. */
/* Multisample type for the title's 480-line modes (tools/postprocess_gen.py).
 * The title asks for 0x2222, 4x supersampling; drawn on the CPU that is four
 * times the work for a softer picture. SIMS_SUPERSAMPLE=1 keeps it. */
uint32_t sims_multisample(uint32_t requested)
{
    const char *e = getenv("SIMS_SUPERSAMPLE");
    if (e && *e == '1')
        return requested;
    return 0x11u;   /* D3DMULTISAMPLE_NONE */
}

/* The scale the title pairs with that choice: 2.0 under supersampling. */
float sims_ss_scale(unsigned site)
{
    const char *keep = getenv("SIMS_SSSCALE_ORIG");   /* hex site list, bring-up */
    char hex[16];
    snprintf(hex, sizeof hex, "%08X", site);
    if (keep && (strstr(keep, hex) || !strcmp(keep, "1"))) return 2.0f;
    /* 0x1293C0 derives something other than the viewport from it; scaled,
     * the title stalls on a black screen after the first movie. */
    if (site == 0x001293C0u) return 2.0f;
    return sims_multisample(0x2222) == 0x2222 ? 2.0f : 1.0f;
}

/* D3D's vblank handler acknowledging the vblank (tools/postprocess_gen.py):
 * PCRTC_INTR_0 and PMC_INTR_0 are write-1-to-clear on hardware, so the ack
 * takes both bits down at once. nv2a = the NV2A register base (0xFD000000). */
void sims_vblank_ack(uint32_t nv2a)
{
    uint8_t *mem = (uint8_t *)g_xbox_mem_offset;
    InterlockedAnd((volatile LONG *)(mem + nv2a + 0x600100u), ~1L);
    InterlockedAnd((volatile LONG *)(mem + nv2a + 0x000100u), ~(LONG)0x01000000L);
}

void sims_spin_yield(void)
{
    if (!SwitchToThread())
        Sleep(0);
}

/* SIMS_THREADDUMP=<seconds>: report every guest thread's latest kernel call
 * and guest call chain at that interval. A bring-up aid for hangs. */
static DWORD WINAPI sims_thread_dumper(LPVOID p)
{
    DWORD ms = (DWORD)(uintptr_t)p;
    for (;;) {
        Sleep(ms);
        xbox_DebugDumpThreads(0x00011000u, 0x002E1C00u);
    }
    return 0;
}

static DWORD WINAPI sims_semaphore_watch(LPVOID unused)
{
    int i;
    (void)unused;
    for (i = 0; i < 120000; i++) {          /* up to ~10 minutes */
        uint8_t *mem = (uint8_t *)g_xbox_mem_offset;
        uint32_t dev = *(volatile uint32_t *)(mem + SIMS_D3D_DEVICE_PTR_VA);
        if (dev >= 0x00010000u && dev < 0x04000000u) {
            uint32_t sem = *(volatile uint32_t *)(mem + dev + SIMS_D3D_SEMAPHORE_OFF);
            if (sem) {
                nv2a_pb_set_semaphore_target(sem);
                /* D3D's "GPU far behind" sleep (software method 5). */
                {
                    extern void xbox_GpuSetWakeEvent(uint32_t event_va);
                    xbox_GpuSetWakeEvent(dev + 0x1DCCu);
                }
                fprintf(stderr, "[SIMS] D3D device at 0x%08X, GPU semaphore at 0x%08X\n",
                        dev, sem);
                return 0;
            }
        }
        Sleep(5);
    }
    fprintf(stderr, "[SIMS] D3D device never appeared; GPU semaphore not registered\n");
    return 0;
}

/* ── WinMain ───────────────────────────────────────────────── */


#ifdef SIMS_TLS_DIAG
/* Bring-up: what the loader set up for this thread's static TLS, written to
 * tls_diag.txt next to the exe. Reads nothing it has not VirtualQuery'd. */
extern unsigned int _tls_index;
extern void (*g_layout_probe)(const char *where);
static __thread volatile unsigned int s_tls_probe = 0x5A5A1234u;
static void tls_probe(const char *where)
{
    FILE *f = fopen("tls_diag.txt", "a");
    void **arr = (void **)__readgsqword(0x58);
    void *blk = NULL;
    MEMORY_BASIC_INFORMATION ma, mb;
    char exe[MAX_PATH];
    if (!f) return;
    GetModuleFileNameA(NULL, exe, sizeof exe);
    memset(&ma, 0, sizeof ma); memset(&mb, 0, sizeof mb);
    if (arr) VirtualQuery(arr, &ma, sizeof ma);
    if (arr && ma.State == MEM_COMMIT) blk = arr[_tls_index];
    if (blk) VirtualQuery(blk, &mb, sizeof mb);
    fprintf(f, "[%s] %s: image %p tls_index %u array %p (state %lx) block %p (state %lx prot %lx) var@%p\n",
            strrchr(exe, '\\') ? strrchr(exe, '\\') + 1 : exe, where,
            (void *)GetModuleHandleA(NULL), _tls_index, (void *)arr,
            (unsigned long)ma.State, blk, (unsigned long)mb.State,
            (unsigned long)mb.Protect, (void *)&s_tls_probe);
    {
        /* The directory as the loader saw it. */
        unsigned char *base = (unsigned char *)GetModuleHandleA(NULL);
        IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
        IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
        IMAGE_TLS_DIRECTORY64 *td = (IMAGE_TLS_DIRECTORY64 *)(base + dd->VirtualAddress);
        fprintf(f, "    dir: start %llx end %llx index@%llx (=%p here) callbacks %llx zerofill %lx chars %lx\n",
                (unsigned long long)td->StartAddressOfRawData, (unsigned long long)td->EndAddressOfRawData,
                (unsigned long long)td->AddressOfIndex, (void *)&_tls_index,
                (unsigned long long)td->AddressOfCallBacks, (unsigned long)td->SizeOfZeroFill,
                (unsigned long)td->Characteristics);
    }
    if (blk && mb.State == MEM_COMMIT) {
        unsigned int v = s_tls_probe;
        s_tls_probe = v + 1;
        fprintf(f, "    probe read %08X, wrote %08X ok\n", v, s_tls_probe);
    } else {
        fprintf(f, "    block not usable\n");
    }
    fclose(f);
}
#endif

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    void *xbe_data = NULL;
    size_t xbe_size = 0;

    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

#ifdef SIMS_TLS_DIAG
    {
        char d[MAX_PATH], *sl;
        GetModuleFileNameA(NULL, d, sizeof d);
        if ((sl = strrchr(d, '\\')) != NULL) { *sl = 0; SetCurrentDirectoryA(d); }
        tls_probe("winmain-start");
        g_layout_probe = tls_probe;
    }
#endif
    /* Unbuffered output for immediate visibility during debugging */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    /* Run from the exe's folder whatever the shortcut's working dir is. */
    {
        char *slash;
        GetModuleFileNameA(NULL, g_exe_dir, sizeof(g_exe_dir));
        slash = strrchr(g_exe_dir, '\\');
        if (slash) *slash = 0;
        SetCurrentDirectoryA(g_exe_dir);
    }

    /* Launched by double-click (no console): keep a log for bug reports. */
    if (!GetConsoleWindow()) {
        /* A GUI process starts with no valid stdout/stderr, so both are
         * reopened onto the file (in append mode, so the two streams'
         * writes interleave instead of overwriting each other). */
        FILE *t = fopen("sims_bustinout.log", "w");
        if (t) fclose(t);
        freopen("sims_bustinout.log", "a", stdout);
        freopen("sims_bustinout.log", "a", stderr);
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
    }

    load_settings_ini();
    apply_title_defaults();

    /* First run: unpack the player's own disc image into game\\. */
    {
        char game_dir[MAX_PATH];
        snprintf(game_dir, sizeof(game_dir), "%s\\%s", g_exe_dir, YOUR_GAME_DIR);
        if (!iso_setup_ensure_game(g_exe_dir, game_dir, iso_from_cmdline()))
            return 1;
    }

    printf("=== The Sims Bustin' Out - Static Recompilation ===\n");
    printf("Loading XBE...\n");

    /* Install VEH handler (first handler in chain) */
    /* Load symbols up front rather than from inside the handler: at fault
     * time the process is already in a bad way, and SymInitialize
     * allocates. Failure is not fatal -- the handler prints no name. */
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    AddVectoredExceptionHandler(1, veh_handler);

    /* Step 1: Load XBE */
    if (!load_xbe(YOUR_GAME_XBE_PATH, &xbe_data, &xbe_size)) {
        MessageBoxA(NULL, "Failed to load game\\default.xbe.\n"
                    "Delete the 'game' folder and start again to re-run setup.",
                    "Recomp", MB_ICONERROR);
        return 1;
    }
    printf("XBE loaded: %zu bytes\n", xbe_size);

    /* Step 2: Initialize Xbox memory layout */
    printf("Initializing Xbox memory layout...\n");
#ifdef SIMS_TLS_DIAG
    tls_probe("before-layout");
#endif
    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        MessageBoxA(NULL, "Failed to initialize Xbox memory layout.\n"
                    "The required virtual address range may be unavailable.",
                    "Recomp", MB_ICONERROR);
        free(xbe_data);
        return 1;
    }

    g_xbox_mem_offset = xbox_GetMemoryOffset();
    printf("Xbox memory mapped. Offset: 0x%llX\n", (unsigned long long)g_xbox_mem_offset);

    /* Bring up the emulated APU.
     *
     * Gated on the same variable that unmaps its registers, because the two
     * halves are useless apart: a trap with no model declines every access,
     * and a model nothing traps into never sees a register. The APU walks
     * Xbox physical RAM to find voice buffers, so it gets the guest RAM
     * base. */
    if (getenv("RECOMP_AC97_READY")) {
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
        fprintf(stderr, "[BOOT] emulated APU %s\n",
                g_apu_state ? "up" : "FAILED to initialise");
    }

    /* USB host controllers (pads), after the guest memory exists. */
    {
        extern void xbox_OhciInit(void);
        xbox_OhciInit();
    }

    /* Draw on the host GPU (Direct3D 11). SIMS_GPU=0 / RECOMP_GPU=0 keeps the
     * software rasteriser; so does a machine without a D3D11 device. */
    {
        extern int nv2a_d3d11_init(void);
        const char *g = getenv("SIMS_GPU");
        if (g && *g == '0')
            set_default_env("RECOMP_GPU", "0");
        if (!nv2a_d3d11_init())
            printf("Drawing with the software rasteriser.\n");
    }
    {
        extern void xbox_MemWatchStart(void);
        xbox_MemWatchStart();                 /* RECOMP_MEMWATCH=1: debugging */
        extern void sims_direct_move_init(void);
        sims_direct_move_init();              /* left stick walks the Sim */
    }

    /* Step 3: Initialize Xbox kernel */
    printf("Initializing Xbox kernel replacement...\n");
    xbox_kernel_init();

    /* Step 4: Set game directory for file I/O path translation */
    {
        extern void xbox_path_init(const char *game_dir, const char *save_dir);
        xbox_path_init(YOUR_GAME_DIR, NULL);
    }

    /* Step 5: Initialize kernel bridge (thunk table in Xbox memory) */
    printf("Initializing kernel bridge...\n");
    xbox_kernel_bridge_init();

    /* Step 6: Initialize stack */
    g_esp = XBOX_STACK_TOP;

    /*
     * TODO: Pre-initialize CRT globals if needed.
     *
     * Many Xbox games use the MSVC CRT. The CRT's __heap_init sets up a
     * heap descriptor at a game-specific address. You may need to
     * pre-initialize __active_heap to avoid small-block heap issues:
     *
     *   uint32_t *active_heap = (uint32_t *)((uint8_t *)g_xbox_mem_offset + ACTIVE_HEAP_VA);
     *   *active_heap = 1;  // 1 = system heap (HeapAlloc), avoids SBH init
     *
     * Find ACTIVE_HEAP_VA by searching the disassembly for __heap_init
     * or by looking for the CRT's __active_heap global in the data section.
     */

    printf("\n=== Initialization complete ===\n");
    printf("Entry point: 0x%08X\n", YOUR_GAME_ENTRY_POINT);
    printf("ESP: 0x%08X\n", g_esp);

    /* Build the flat dispatch table before the guest runs.
     *
     * Without this recomp_lookup falls back to a binary search over the
     * whole function table -- roughly log2(n) branches on *every*
     * indirect call, which for a 45,000-function C++ title is about 16
     * every time the game goes through a vtable. Half-Life 2 spent most
     * of its static initialisation inside recomp_lookup for exactly this
     * reason, and it read as a hang.
     *
     * Optional by design: if the allocation fails the search still works,
     * so a failure is worth one line and not a fatal error. */
    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; "
                        "indirect calls will use the binary search\n");

    /* Arm the hang watchdog. Does nothing unless RECOMP_WATCHDOG_SECS is set,
     * and must be called from this thread -- the guest registers it samples are
     * thread-local, so it has to be handed the copies belonging to the thread
     * that runs guest code.
     *
     * Not optional boilerplate: without this call RECOMP_WATCHDOG_SECS is
     * silently inert, and the one diagnostic that tells a hang from slowness
     * does nothing while appearing to be set. */
    xbox_WatchdogStart();

    CloseHandle(CreateThread(NULL, 0, sims_semaphore_watch, NULL, 0, NULL));
    if (getenv("SIMS_THREADDUMP")) {
        DWORD secs = (DWORD)atoi(getenv("SIMS_THREADDUMP"));
        if (secs < 1) secs = 10;
        CloseHandle(CreateThread(NULL, 0, sims_thread_dumper,
                                 (LPVOID)(uintptr_t)(secs * 1000), 0, NULL));
    }

    /* Step 7: Call the recompiled entry point */
    printf("\nStarting game...\n");
    fflush(stdout);

    xbe_entry_point();

    printf("\nGame returned. Cleaning up...\n");

    /* Cleanup */
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe_data);

    return 0;
}

/* ── XBE Loading ───────────────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open XBE: %s\n", path);
        return FALSE;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return FALSE;
    }

    void *data = malloc((size_t)size);
    if (!data) {
        fclose(f);
        return FALSE;
    }

    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return FALSE;
    }

    fclose(f);
    *out_data = data;
    *out_size = (size_t)size;
    return TRUE;
}

/* Console entry point (for debugging -- lets you see printf output) */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return WinMain(GetModuleHandle(NULL), NULL, GetCommandLineA(), SW_SHOW);
}

