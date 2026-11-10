/*
 * iso_setup.c -- first-run setup for The Sims Bustin' Out (PC recomp).
 *
 * The executable ships without any game data. On first launch the player
 * points it at their own Xbox disc image (drag the .iso onto the .exe, put
 * it next to the .exe, or pick it in the file dialog) and this unpacks the
 * XDVDFS file system into the "game" folder beside the executable.
 *
 * The image is checked to be the right title (XBE certificate title ID
 * 0x45410043, The Sims Bustin' Out NTSC-U) before anything is written.
 */

#include <windows.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "iso_setup.h"

#define XSECTOR          2048ULL
#define SIMS_BO_TITLE_ID 0x45410043u

static const char XMAGIC[] = "MICROSOFT*XBOX*MEDIA";
static const uint64_t BASES[] = {
    0x00000000ULL, 0x0000FD90ULL, 0x00030600ULL, 0x0FD90000ULL, 0x18300000ULL
};

typedef struct {
    HANDLE   h;
    uint64_t base;
    uint32_t root_sector, root_size;
    uint64_t total_bytes, done_bytes;
    unsigned files;
} Xiso;

static BOOL read_at(Xiso *x, uint64_t off, void *buf, DWORD len)
{
    LARGE_INTEGER li;
    DWORD got = 0;
    li.QuadPart = (LONGLONG)off;
    if (!SetFilePointerEx(x->h, li, NULL, FILE_BEGIN)) return FALSE;
    return ReadFile(x->h, buf, len, &got, NULL) && got == len;
}

static BOOL xiso_open(Xiso *x, const char *path)
{
    char desc[2048];
    size_t i;

    memset(x, 0, sizeof(*x));
    x->h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                       OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (x->h == INVALID_HANDLE_VALUE) return FALSE;

    for (i = 0; i < sizeof(BASES) / sizeof(BASES[0]); i++) {
        if (read_at(x, BASES[i] + 32 * XSECTOR, desc, sizeof(desc)) &&
            memcmp(desc, XMAGIC, sizeof(XMAGIC) - 1) == 0) {
            x->base = BASES[i];
            memcpy(&x->root_sector, desc + 0x14, 4);
            memcpy(&x->root_size,   desc + 0x18, 4);
            return TRUE;
        }
    }
    CloseHandle(x->h);
    x->h = INVALID_HANDLE_VALUE;
    return FALSE;
}

typedef BOOL (*entry_cb)(Xiso *x, const char *rel, uint32_t sector,
                         uint32_t size, int is_dir, void *user);

/* Directory entries are a binary tree, but laid out contiguously per sector,
 * so a linear walk with a per-sector resync visits every one of them. */
static BOOL walk_dir(Xiso *x, uint32_t sector, uint32_t size,
                     const char *prefix, entry_cb cb, void *user, int depth)
{
    uint8_t *data;
    uint32_t base;
    BOOL ok = TRUE;

    if (depth > 16 || size == 0 || size > 64u * 1024 * 1024) return TRUE;
    data = (uint8_t *)malloc(size);
    if (!data) return FALSE;
    if (!read_at(x, x->base + (uint64_t)sector * XSECTOR, data, size)) {
        free(data);
        return FALSE;
    }
    for (base = 0; base < size && ok; base += (uint32_t)XSECTOR) {
        uint32_t lim = (size - base < XSECTOR) ? size - base : (uint32_t)XSECTOR;
        uint32_t off = 0;
        while (off + 0x0E <= lim) {
            const uint8_t *e = data + base + off;
            uint16_t l, r;
            uint32_t sec, sz;
            uint8_t attr, nlen;
            char name[256], rel[MAX_PATH];

            memcpy(&l, e, 2); memcpy(&r, e + 2, 2);
            memcpy(&sec, e + 4, 4); memcpy(&sz, e + 8, 4);
            attr = e[12]; nlen = e[13];
            if ((l == 0xFFFF && r == 0xFFFF) || nlen == 0) break;
            if (off + 0x0E + nlen > lim) break;
            memcpy(name, e + 0x0E, nlen);
            name[nlen] = 0;
            if (strchr(name, '\\') || strchr(name, '/') || strstr(name, ".."))
                break;   /* never let an image write outside the target */
            snprintf(rel, sizeof(rel), "%s%s%s", prefix, *prefix ? "\\" : "", name);
            if (!cb(x, rel, sec, sz, (attr & 0x10) != 0, user)) { ok = FALSE; break; }
            if (attr & 0x10)
                ok = walk_dir(x, sec, sz, rel, cb, user, depth + 1);
            off += (0x0E + nlen + 3) & ~3u;
        }
    }
    free(data);
    return ok;
}

/* ── pass 1: find default.xbe, sum sizes ── */
typedef struct { uint32_t xbe_sector, xbe_size; } ScanInfo;

static BOOL scan_cb(Xiso *x, const char *rel, uint32_t sector, uint32_t size,
                    int is_dir, void *user)
{
    ScanInfo *si = (ScanInfo *)user;
    if (!is_dir) {
        x->total_bytes += size;
        x->files++;
        if (_stricmp(rel, "default.xbe") == 0) {
            si->xbe_sector = sector;
            si->xbe_size = size;
        }
    }
    return TRUE;
}

/* ── pass 2: extract ── */
typedef struct { char dest[MAX_PATH]; } ExtractInfo;

static BOOL extract_cb(Xiso *x, const char *rel, uint32_t sector, uint32_t size,
                       int is_dir, void *user)
{
    ExtractInfo *ei = (ExtractInfo *)user;
    char path[MAX_PATH * 2];
    HANDLE out;
    uint64_t pos = x->base + (uint64_t)sector * XSECTOR;
    uint32_t left = size;
    static uint8_t buf[1 << 20];

    snprintf(path, sizeof(path), "%s\\%s", ei->dest, rel);
    if (is_dir) {
        CreateDirectoryA(path, NULL);
        return TRUE;
    }
    out = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                      FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[SETUP] cannot create %s (error %lu)\n", path, GetLastError());
        return FALSE;
    }
    while (left) {
        DWORD n = left > sizeof(buf) ? (DWORD)sizeof(buf) : left, w = 0;
        if (!read_at(x, pos, buf, n) || !WriteFile(out, buf, n, &w, NULL) || w != n) {
            CloseHandle(out);
            fprintf(stderr, "[SETUP] read/write failed on %s\n", rel);
            return FALSE;
        }
        pos += n;
        left -= n;
        x->done_bytes += n;
    }
    CloseHandle(out);
    {
        int pct = x->total_bytes ? (int)(x->done_bytes * 100 / x->total_bytes) : 100;
        char title[128];
        printf("[SETUP] %3d%%  %s\n", pct, rel);
        snprintf(title, sizeof(title), "The Sims Bustin' Out - installing from disc image (%d%%)", pct);
        SetConsoleTitleA(title);
    }
    return TRUE;
}

/* The XBE certificate holds the title ID at +0x08; the header gives the
 * certificate address as a VA relative to the base address. */
static BOOL check_title(Xiso *x, const ScanInfo *si, uint32_t *title_out)
{
    uint8_t hdr[4096];
    uint32_t base_addr, cert_va, title;
    uint64_t at = x->base + (uint64_t)si->xbe_sector * XSECTOR;

    if (si->xbe_size < sizeof(hdr) || !read_at(x, at, hdr, sizeof(hdr))) return FALSE;
    if (memcmp(hdr, "XBEH", 4) != 0) return FALSE;
    memcpy(&base_addr, hdr + 0x104, 4);
    memcpy(&cert_va,   hdr + 0x118, 4);
    if (cert_va < base_addr || cert_va - base_addr + 0x0C > sizeof(hdr)) return FALSE;
    memcpy(&title, hdr + (cert_va - base_addr) + 0x08, 4);
    *title_out = title;
    return title == SIMS_BO_TITLE_ID;
}

static BOOL pick_iso_dialog(char *out, DWORD cap)
{
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    out[0] = 0;
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "Xbox disc image (*.iso;*.xiso)\0*.iso;*.xiso\0All files\0*.*\0";
    ofn.lpstrFile = out;
    ofn.nMaxFile = cap;
    ofn.lpstrTitle = "Select your The Sims Bustin' Out (Xbox) disc image";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&ofn);
}

static BOOL find_iso_beside_exe(const char *exe_dir, char *out, DWORD cap)
{
    static const char *pats[] = { "*.iso", "*.xiso" };
    size_t i;
    for (i = 0; i < 2; i++) {
        char pat[MAX_PATH];
        WIN32_FIND_DATAA fd;
        HANDLE h;
        snprintf(pat, sizeof(pat), "%s\\%s", exe_dir, pats[i]);
        h = FindFirstFileA(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            snprintf(out, cap, "%s\\%s", exe_dir, fd.cFileName);
            FindClose(h);
            return TRUE;
        }
    }
    return FALSE;
}

int iso_setup_ensure_game(const char *exe_dir, const char *game_dir,
                          const char *iso_arg)
{
    char xbe[MAX_PATH], iso[MAX_PATH * 2];
    Xiso x;
    ScanInfo si = { 0, 0 };
    ExtractInfo ei;
    uint32_t title = 0;
    char msg[1024];

    char marker[MAX_PATH];
    snprintf(xbe, sizeof(xbe), "%s\\default.xbe", game_dir);
    snprintf(marker, sizeof(marker), "%s\\install_in_progress.txt", game_dir);
    /* The marker exists only while unpacking, so a setup that was interrupted
     * is redone. A game folder copied in by hand is accepted as is. */
    if (!iso_arg && GetFileAttributesA(xbe) != INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesA(marker) == INVALID_FILE_ATTRIBUTES)
        return 1;

    if (iso_arg && *iso_arg)
        snprintf(iso, sizeof(iso), "%s", iso_arg);
    else if (!find_iso_beside_exe(exe_dir, iso, sizeof(iso))) {
        MessageBoxA(NULL,
            "First-time setup\n\n"
            "This port does not include any game data. Please select your own\n"
            "disc image of The Sims Bustin' Out (Xbox, USA).\n\n"
            "Tip: you can also drag the .iso onto the .exe, or put it next to it.",
            "The Sims Bustin' Out", MB_ICONINFORMATION);
        if (!pick_iso_dialog(iso, sizeof(iso)))
            return 0;
    }

    printf("[SETUP] disc image: %s\n", iso);
    if (!xiso_open(&x, iso)) {
        snprintf(msg, sizeof(msg),
                 "\"%s\" is not an Xbox disc image (no XDVDFS file system found).", iso);
        MessageBoxA(NULL, msg, "The Sims Bustin' Out", MB_ICONERROR);
        return 0;
    }
    if (!walk_dir(&x, x.root_sector, x.root_size, "", scan_cb, &si, 0) || !si.xbe_size) {
        CloseHandle(x.h);
        MessageBoxA(NULL, "The disc image has no default.xbe.", "The Sims Bustin' Out", MB_ICONERROR);
        return 0;
    }
    if (!check_title(&x, &si, &title)) {
        CloseHandle(x.h);
        snprintf(msg, sizeof(msg),
                 "This disc image is not The Sims Bustin' Out (USA).\n"
                 "Expected title ID 0x%08X, found 0x%08X.", SIMS_BO_TITLE_ID, title);
        MessageBoxA(NULL, msg, "The Sims Bustin' Out", MB_ICONERROR);
        return 0;
    }

    printf("[SETUP] extracting %u files (%.1f MB) to %s\n",
           x.files, x.total_bytes / 1048576.0, game_dir);
    CreateDirectoryA(game_dir, NULL);
    {
        FILE *m = fopen(marker, "w");
        if (m) { fprintf(m, "unpacking: %s\n", iso); fclose(m); }
    }
    snprintf(ei.dest, sizeof(ei.dest), "%s", game_dir);
    if (!walk_dir(&x, x.root_sector, x.root_size, "", extract_cb, &ei, 0)) {
        CloseHandle(x.h);
        /* marker stays, so the next launch retries */
        MessageBoxA(NULL, "Extracting the disc image failed (disk full?).",
                    "The Sims Bustin' Out", MB_ICONERROR);
        return 0;
    }
    CloseHandle(x.h);
    DeleteFileA(marker);
    printf("[SETUP] done.\n");
    SetConsoleTitleA("The Sims Bustin' Out");
    return 1;
}
