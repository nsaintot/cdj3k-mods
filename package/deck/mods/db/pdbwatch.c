// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * mods/db/pdbwatch.c - logs when the deck writes a DJ's library files.
 *
 * djdb.c's hooked entry points do not fire during browsing, loading or cueing,
 * yet the library files change. The shim interposes open() and write(), so
 * this watches the files directly and logs when each is opened for writing,
 * how much is written, and which code wrote it (from the return address).
 *
 * Observation only: no flag, path or byte is changed.
 *
 * Only paths under PIONEER/ (and exportLibrary.db) are watched, and a session
 * writes each file only a few times, so the log stays short.
 */
#include "db/db.h"

#include "core/ep122_syms.h"
#include "kit/mod.h"
#include "core/mod_core.h"

/* Watches both media libraries: the rekordbox device files and the SQLCipher
 * one beside them. */
#define PDB_WATCH_MAX     8
#define PDB_NAME_MAX      24
#define PDB_SITES_MAX     4

struct pdb_watch {
    int       fd;                      /* -1 when the slot is free */
    char      name[PDB_NAME_MAX];
    uint64_t  bytes;
    unsigned  writes;
    uintptr_t site[PDB_SITES_MAX];     /* distinct return addresses seen */
    int       nsite;
};

static struct pdb_watch pdb_g[PDB_WATCH_MAX];
static int pdb_g_ready;

/* The tail of a path, for a log line that fits. */
static const char *pdb_base(const char *path)
{
    const char *s = path, *last = path;

    for (; *s; s++)
        if (*s == '/')
            last = s + 1;
    return last;
}

/* Whether this is a library file.
 *
 * The whole PIONEER directory, not just rekordbox/: a track's beat grid is the
 * Quantize atom of its ANLZ file under PIONEER/USBANLZ, not in the library. */
static int pdb_interesting(const char *path)
{
    return strstr(path, "/PIONEER/") != NULL ||
           strstr(path, "exportLibrary.db") != NULL;
}

void db_watch_open(const char *path, int fd, int flags)
{
    int i;

    if (!path || fd < 0 || !pdb_interesting(path))
        return;

    if (!pdb_g_ready) {
        for (i = 0; i < PDB_WATCH_MAX; i++)
            pdb_g[i].fd = -1;
        pdb_g_ready = 1;
    }

    /* Logged even without a following write: a writable open is the event of
     * interest. */
    MDBG("pdb: open %s fd %d flags %#x (%s)\n", pdb_base(path), fd, flags,
         (flags & 3) ? "WRITABLE" : "read-only");

    if (!(flags & 3))
        return;                        /* nothing to attribute */

    for (i = 0; i < PDB_WATCH_MAX; i++) {
        if (pdb_g[i].fd >= 0)
            continue;
        pdb_g[i].fd     = fd;
        pdb_g[i].bytes  = 0;
        pdb_g[i].writes = 0;
        pdb_g[i].nsite  = 0;
        snprintf(pdb_g[i].name, sizeof(pdb_g[i].name), "%s", pdb_base(path));
        return;
    }
}

void db_watch_write(int fd, size_t count, uintptr_t ra)
{
    int i, s;

    if (!pdb_g_ready || fd < 0)
        return;

    for (i = 0; i < PDB_WATCH_MAX; i++) {
        if (pdb_g[i].fd != fd)
            continue;

        pdb_g[i].bytes += count;
        pdb_g[i].writes++;

        /* Every write is logged; a session is about three writes per file
         * (one at mount, the rest at eject). Sites are tracked to flag a second
         * writer. */
        for (s = 0; s < pdb_g[i].nsite; s++)
            if (pdb_g[i].site[s] == ra)
                break;
        if (s == pdb_g[i].nsite && pdb_g[i].nsite < PDB_SITES_MAX)
            pdb_g[i].site[pdb_g[i].nsite++] = ra;

        MDBG("pdb: %s write #%u, %zu bytes, from %#lx%s\n", pdb_g[i].name,
             pdb_g[i].writes, count, (unsigned long)ra,
             s == pdb_g[i].nsite - 1 && s > 0 ? "  <- a NEW writer" : "");
        return;
    }
}

void db_watch_close(int fd)
{
    int i;

    if (!pdb_g_ready || fd < 0)
        return;

    for (i = 0; i < PDB_WATCH_MAX; i++) {
        if (pdb_g[i].fd != fd)
            continue;
        if (pdb_g[i].writes)
            MDBG("pdb: close %s -- %u writes, %llu bytes\n", pdb_g[i].name,
                 pdb_g[i].writes, (unsigned long long)pdb_g[i].bytes);
        pdb_g[i].fd = -1;
        return;
    }
}

/* ---- stdio writes -----------------------------------------------------------
 *
 * The DB engine uses stdio: track_info_repository::CFpHandle opens with fopen
 * and writes with fwrite, and glibc calls open64/write internally, bound within
 * libc rather than through EP122's PLT, so the shim's interposers never see
 * them (e.g. a beat grid registered through the repository grows the track's
 * ANLZ by 13 kB with no log line). Interposing fopen/fwrite is not an option:
 * the shim has no dlsym by design and every passthrough is a raw syscall.
 *
 * The handle's methods are virtual, so they are hooked by vtable slot. The
 * return address is then the DB layer's caller, not a frame inside libc.
 *
 * Observation only: each wrapper calls the stock one and logs.
 */

/* Where a CFpHandle keeps its FILE*: its open stores the fopen result here,
 * after fclosing the previous one. */
#define PDB_FP_OFF        0x28

/* Its three I/O slots, named after the libc call each one makes. */
#define PDB_FP_SLOT_OPEN  0x38
#define PDB_FP_SLOT_CLOSE 0x40
#define PDB_FP_SLOT_WRITE 0x78

static uintptr_t pdb_g_fp_open, pdb_g_fp_close, pdb_g_fp_write;

/* The fd behind the handle, or -1, so stdio writes share the fd-keyed table
 * above. */
static int pdb_fp_fd(uintptr_t handle)
{
    uintptr_t fp = 0;

    if (!handle ||
        mod_safe_read(handle + PDB_FP_OFF, &fp, sizeof(fp)) != 0 || !fp)
        return -1;
    return fileno((FILE *)fp);
}

static uint64_t pdb_wrap_fp_open(uintptr_t self, void *src, int mode, void *err)
{
    /* A re-opened handle fcloses its previous file. Retire that entry, or a
     * reused fd number would be logged under the old name. */
    int was = pdb_fp_fd(self);
    uint64_t r;
    char link[32], path[512];
    int  fd;
    ssize_t n;

    if (was >= 0)
        db_watch_close(was);
    r = ((uint64_t (*)(uintptr_t, void *, int, void *))
         pdb_g_fp_open)(self, src, mode, err);
    fd = pdb_fp_fd(self);

    if (fd < 0)
        return r;
    /* Path and flags come from the fd, not the arguments: the path argument
     * is an object the handle downcasts, and `mode` indexes an internal table.
     * F_GETFL gives the flags the file actually got. */
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    n = readlink(link, path, sizeof(path) - 1);
    if (n <= 0)
        return r;
    path[n] = '\0';
    db_watch_open(path, fd, fcntl(fd, F_GETFL));
    return r;
}

static uint64_t pdb_wrap_fp_write(uintptr_t self, const void *buf, size_t len)
{
    int fd = pdb_fp_fd(self);

    if (fd >= 0)
        db_watch_write(fd, len, (uintptr_t)__builtin_return_address(0));
    return ((uint64_t (*)(uintptr_t, const void *, size_t))
            pdb_g_fp_write)(self, buf, len);
}

static uint64_t pdb_wrap_fp_close(uintptr_t self)
{
    /* Before the stock close, which releases the fd. */
    int fd = pdb_fp_fd(self);

    if (fd >= 0)
        db_watch_close(fd);
    return ((uint64_t (*)(uintptr_t))pdb_g_fp_close)(self);
}

/* The open hook is required, since it supplies the file names. The other two
 * are optional. */
static int pdb_install(void)
{
    if (mod_patch_vslot("dbFpOpen", EP122_DB_FPHANDLE, PDB_FP_SLOT_OPEN,
                        (void *)pdb_wrap_fp_open, &pdb_g_fp_open) != 0) {
        MDBG("pdb: no CFpHandle -> the DB engine's own files stay unwatched\n");
        return -1;
    }
    (void)mod_patch_vslot("dbFpWrite", EP122_DB_FPHANDLE, PDB_FP_SLOT_WRITE,
                          (void *)pdb_wrap_fp_write, &pdb_g_fp_write);
    (void)mod_patch_vslot("dbFpClose", EP122_DB_FPHANDLE, PDB_FP_SLOT_CLOSE,
                          (void *)pdb_wrap_fp_close, &pdb_g_fp_close);
    return 0;
}

KIT_MOD(k_mod_pdbwatch,
        .name = "pdbwatch", .prio = 62, .install = pdb_install,
        .what = "library files: watch the DB engine's own stdio handle too");
