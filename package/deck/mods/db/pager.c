// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * mods/db/pager.c - the page store underneath the media library's tables.
 *
 * Every row passes through here: DeviceSQL holds export.pdb as fixed-size pages
 * and moves them with exactly two calls. (djdb's table API refuses writes from
 * a thread without a context; see djdb.c.)
 *
 *   read (self, buf, index, &eof)   EP122_DJDB_PAGE_READ
 *   write(self, buf, index)         EP122_DJDB_PAGE_WRITE
 *
 * Both compute the file offset as `index * *(self + 0x10)` -- the page size --
 * seek the handle at `*(self + 0x00)` and move that many bytes, returning true
 * only on a full transfer. The write treats index 0 as the header: its length
 * comes from `*(self + 0x18)` when that is set, and it goes to a second handle
 * at `*(self + 0x08)` when that is set.
 *
 * Neither is called directly: both are entries 1 and 2 of a six-function
 * storage backend and are reached only through it.
 *
 * Observation only; no byte is changed. It logs:
 *
 *   - which page holds a row we can already identify,
 *   - whether a page is written from the buffer it was read into (a page
 *     cache) or from a scratch buffer,
 *   - which pages the deck writes at eject; a change to any other page would
 *     not reach the media.
 *
 * Threading: only the deck's database thread calls either op, so the scratch
 * buffer below can be static.
 */
#include "db/db.h"

#include "core/ep122_syms.h"
#include "kit/mod.h"
#include "core/mod_core.h"

/* ---- the buffer pool above it ---------------------------------------------
 *
 * DBService.cpp registers three services per database:
 *
 *   PGA:SINGLE   the page adaptor -- the two ops above, "ONEFD=T"
 *   PGM:PGM      the page manager -- "INIT_CLOSED=T,PGA=SINGLE,PAGESZ=4096,
 *                NBUFS=6144", i.e. a 24 MiB pool of 4 KiB page buffers
 *   SM:DEFAULT   the storage manager over both
 *
 * So the deck holds the media library in memory as pages, and every row the
 * browser draws is read from one of those buffers. The pool layout:
 *
 *   pgm = *(*handle + 0x138)      the manager
 *   pgm + 0x58                    the buffer table
 *   table + 0x08                  an array of descriptors, 16 bytes each
 *   table + 0x18                  how many (NBUFS)
 *   desc + 0x00                   flag byte: 1 stale, 2 DIRTY, 8, 0x10
 *   desc + 0x08                   the page's 4096 raw bytes
 *   page + 0x04                   which page index it is holding
 *
 * A page is written only if it is on the manager's modified list, not merely
 * because its bytes changed. PageMarkDirty (EP122_DJDB_MARK_DIRTY, and the plain
 * variant EP122_DJDB_MARK_DIRTY_PLAIN) sets flag 2, registers
 * the page index, sets `pgm + 0x1c`, and stamps the transaction at
 * `page + 0x10`. When `*(int16 *)(pgm + 0x20)` is set it writes the page
 * immediately instead of deferring. Both variants are hooked below.
 */

/* The backend object, as both ops index it. */
#define PG_HANDLE_OFF     0x00
#define PG_HANDLE2_OFF    0x08
#define PG_PAGESZ_OFF     0x10
#define PG_HDRSZ_OFF      0x18

/* A file handle: { int fd; int64 pos at +0x08 }. */
#define PG_FD_OFF         0x00

/* The page manager, reached from a handle PageMarkDirty was called with. */
#define PG_PGM_OFF        0x138
#define PG_PGM_MODE       0x20          /* int16: set means write-through */
#define PG_PGM_BUFTAB     0x58
#define PG_TAB_DESCS      0x08
#define PG_TAB_COUNT      0x18
#define PG_DESC_PAGE      0x08
#define PG_PAGE_INDEX     0x04

/* A larger page size means a misread object; do not copy it. */
#define PG_PAGE_MAX       0x2000

/* How many distinct page indices to track, and log limits. A browse touches
 * many pages. */
#define PG_SEEN_MAX       64
#define PG_QUIET_AFTER    40           /* plain reads logged before going quiet */
#define PG_DUMPS_MAX      12           /* row dumps, which are several lines each */

/* Tempos (x100) of known rows: content id 7 at 126.00, id 8 at 120.00, id 11
 * at 125.00. A tempo found in a page identifies the page and the row without
 * parsing the format. */
static const int32_t k_pg_needle[] = { 12600, 12000, 12500 };
#define PG_NEEDLES  ((int)(sizeof(k_pg_needle) / sizeof(k_pg_needle[0])))

struct pg_seen {
    uint32_t  index;
    uintptr_t buf;                     /* the buffer it was last read into */
    unsigned  reads, writes;
    int       moved;                   /* a second buffer address was seen */
};

static struct pg_seen pg_g[PG_SEEN_MAX];
static int  pg_g_nseen;
static int  pg_g_logged, pg_g_dumped;
static uintptr_t pg_g_tramp_read, pg_g_tramp_write;

/* A live page-manager handle, kept from the hooked calls. Not used here; it is
 * logged because a write mod would need it and an idle thread cannot reach
 * one otherwise. */
static uintptr_t pg_g_handle;

/* One page, copied out for scanning. Static because only one thread enters
 * either op, and 8 KiB is too much for the database thread's stack. */
static uint8_t pg_g_scratch[PG_PAGE_MAX];

typedef int64_t (*pg_read_fn)(uintptr_t self, void *buf, uint64_t index,
                              uint8_t *eof);
typedef int64_t (*pg_write_fn)(uintptr_t self, void *buf, uint64_t index);

static uint64_t pg_field(uintptr_t self, unsigned off)
{
    uint64_t v = 0;

    if (mod_safe_read(self + off, &v, sizeof(v)) != 0)
        return 0;
    return v;
}

/* The file behind a handle, named once per descriptor. */
static const char *pg_name(uint64_t handle)
{
    static char last[64];
    static int  last_fd = -1;
    char link[32], path[512];
    int32_t fd = -1;
    ssize_t n;
    const char *base = path, *s;

    if (!handle || mod_safe_read((uintptr_t)handle + PG_FD_OFF, &fd,
                                 sizeof(fd)) != 0 || fd < 0)
        return "?";
    if (fd == last_fd)
        return last;

    snprintf(link, sizeof(link), "/proc/self/fd/%d", (int)fd);
    n = readlink(link, path, sizeof(path) - 1);
    if (n <= 0)
        return "?";
    path[n] = '\0';
    for (s = path; *s; s++)
        if (*s == '/')
            base = s + 1;
    snprintf(last, sizeof(last), "%s", base);
    last_fd = fd;
    return last;
}

static struct pg_seen *pg_slot(uint64_t index)
{
    int i;

    for (i = 0; i < pg_g_nseen; i++)
        if (pg_g[i].index == (uint32_t)index)
            return &pg_g[i];
    if (pg_g_nseen == PG_SEEN_MAX)
        return NULL;
    pg_g[pg_g_nseen].index = (uint32_t)index;
    return &pg_g[pg_g_nseen++];
}

/* Log the words around a hit, to show where the id sits relative to the tempo
 * and how wide each field is. */
static void pg_dump_around(const uint8_t *page, size_t len, size_t at)
{
    size_t from = at > 32 ? at - 32 : 0;
    size_t to = at + 40 < len ? at + 40 : len;
    char line[192];
    size_t o;
    int n = 0;

    for (o = from & ~3u; o + 4 <= to; o += 4) {
        int32_t w;

        memcpy(&w, page + o, sizeof(w));
        n += snprintf(line + n, sizeof(line) - (size_t)n, "%s%d",
                      o == (at & ~3u) ? " >" : " ", (int)w);
        if ((size_t)n >= sizeof(line) - 16)
            break;
    }
    MDBG("pager:   +%04zu%s\n", from & ~3u, line);
}

/* Report any known tempo in the page. Returns how many were found. */
static int pg_scan(const uint8_t *page, size_t len, uint64_t index,
                   const char *what)
{
    size_t o;
    int found = 0;

    if (len < 4)
        return 0;
    for (o = 0; o + 4 <= len; o += 4) {
        int32_t w;
        int k;

        memcpy(&w, page + o, sizeof(w));
        for (k = 0; k < PG_NEEDLES; k++) {
            if (w != k_pg_needle[k])
                continue;
            found++;
            if (pg_g_dumped < PG_DUMPS_MAX) {
                pg_g_dumped++;
                MDBG("pager: %s page %llu holds %d at +%zu\n", what,
                     (unsigned long long)index, (int)w, o);
                pg_dump_around(page, len, o);
            }
        }
    }
    return found;
}

static int64_t pg_wrap_read(uintptr_t self, void *buf, uint64_t index,
                            uint8_t *eof)
{
    int64_t  r = ((pg_read_fn)pg_g_tramp_read)(self, buf, index, eof);
    uint64_t pagesz = pg_field(self, PG_PAGESZ_OFF);
    struct pg_seen *s;
    int hits = 0;

    if (!r || !pagesz || pagesz > PG_PAGE_MAX)
        return r;

    if (mod_safe_read((uintptr_t)buf, pg_g_scratch, (size_t)pagesz) == 0)
        hits = pg_scan(pg_g_scratch, (size_t)pagesz, index, "read");

    s = pg_slot(index);
    if (s) {
        if (s->reads && s->buf != (uintptr_t)buf)
            s->moved = 1;
        s->buf = (uintptr_t)buf;
        s->reads++;
    }

    if (pg_g_logged < PG_QUIET_AFTER) {
        pg_g_logged++;
        MDBG("pager: read %s page %llu -> %p, %llu bytes%s\n",
             pg_name(pg_field(self, PG_HANDLE_OFF)),
             (unsigned long long)index, buf, (unsigned long long)pagesz,
             hits ? "  <- holds a known row" : "");
    }
    return r;
}

/* Logged once as the flush starts: whether each page index kept one buffer
 * between reads (a cache) or got a new one each time (a scratch buffer, which
 * a change would not survive). */
static void pg_report(void)
{
    static int done;
    int i, stable = 0, moved = 0;

    if (done || !pg_g_nseen)
        return;
    done = 1;
    for (i = 0; i < pg_g_nseen; i++) {
        if (pg_g[i].moved)
            moved++;
        else if (pg_g[i].reads > 1)
            stable++;
    }
    MDBG("pager: %d page indices read, %d kept one buffer, %d moved; handle %#lx\n",
         pg_g_nseen, stable, moved, (unsigned long)pg_g_handle);
}

static int64_t pg_wrap_write(uintptr_t self, void *buf, uint64_t index)
{
    uint64_t pagesz = pg_field(self, PG_PAGESZ_OFF);
    uint64_t handle = pg_field(self, PG_HANDLE_OFF);
    uint64_t len = pagesz;
    struct pg_seen *s = pg_slot(index);
    int hits = 0;

    pg_report();

    /* Mirror the write's index-0 rule so the log shows the real length and
     * handle. */
    if (index == 0) {
        uint64_t hdrsz = pg_field(self, PG_HDRSZ_OFF);
        uint64_t alt = pg_field(self, PG_HANDLE2_OFF);

        if (hdrsz)
            len = hdrsz;
        if (alt)
            handle = alt;
    }

    /* Before the stock call: these are the bytes that reach the media. */
    if (pagesz && len && len <= PG_PAGE_MAX &&
        mod_safe_read((uintptr_t)buf, pg_g_scratch, (size_t)len) == 0)
        hits = pg_scan(pg_g_scratch, (size_t)len, index, "write");

    MDBG("pager: WRITE %s page %llu from %p, %llu bytes%s%s\n",
         pg_name(handle), (unsigned long long)index, buf,
         (unsigned long long)len,
         s && s->reads && s->buf == (uintptr_t)buf ? "  <- the buffer it was read into" : "",
         hits ? "  <- holds a known row" : "");

    if (s)
        s->writes++;

    /* The context is reliably live here, so the registry can be dumped. */
    mod_djdb_note("the eject flush");
    return ((pg_write_fn)pg_g_tramp_write)(self, buf, index);
}

/* ---- what the deck dirties, and how -----------------------------------------
 *
 * A page changed in memory is only written if it was registered as modified,
 * so each call is logged: which page, and whether the manager defers it or
 * writes it through. The handle is also kept, since PageMarkDirty takes one and
 * an idle thread cannot get one otherwise.
 */
static uintptr_t pg_g_tramp_dirty, pg_g_tramp_dirty_plain, pg_g_tramp_get;

typedef int64_t (*pg_dirty_fn)(uintptr_t self, uintptr_t desc);
typedef int64_t (*pg_get_fn)(uintptr_t self, uint32_t index, uintptr_t *desc);

/* The page index a descriptor is holding, or -1. */
static int32_t pg_desc_index(uintptr_t desc)
{
    uintptr_t page = 0;
    int32_t idx = -1;

    if (mod_safe_read(desc + PG_DESC_PAGE, &page, sizeof(page)) != 0 || !page)
        return -1;
    (void)mod_safe_read(page + PG_PAGE_INDEX, &idx, sizeof(idx));
    return idx;
}

/* The pool's shape, once, from the first handle that reaches us. */
static void pg_pool_report(uintptr_t self)
{
    static int done;
    uintptr_t pgm = 0, tab = 0, descs = 0;
    int32_t nbufs = 0;
    int16_t mode = 0;

    if (done || !self)
        return;
    if (mod_safe_read(self, &pgm, sizeof(pgm)) != 0 || !pgm ||
        mod_safe_read(pgm + PG_PGM_OFF, &pgm, sizeof(pgm)) != 0 || !pgm)
        return;
    done = 1;
    (void)mod_safe_read(pgm + PG_PGM_MODE, &mode, sizeof(mode));
    if (mod_safe_read(pgm + PG_PGM_BUFTAB, &tab, sizeof(tab)) == 0 && tab) {
        (void)mod_safe_read(tab + PG_TAB_DESCS, &descs, sizeof(descs));
        (void)mod_safe_read(tab + PG_TAB_COUNT, &nbufs, sizeof(nbufs));
    }
    MDBG("pager: pool %#lx, %d buffers at %#lx, %s\n", (unsigned long)pgm,
         (int)nbufs, (unsigned long)descs,
         mode ? "WRITE-THROUGH" : "deferred to the modified list");
}

static int64_t pg_wrap_dirty(uintptr_t self, uintptr_t desc)
{
    static unsigned seen;

    pg_g_handle = self;
    pg_pool_report(self);
    if (seen++ < PG_QUIET_AFTER)
        MDBG("pager: mark dirty page %d\n", (int)pg_desc_index(desc));
    return ((pg_dirty_fn)pg_g_tramp_dirty)(self, desc);
}

static int64_t pg_wrap_dirty_plain(uintptr_t self, uintptr_t desc)
{
    static unsigned seen;

    pg_g_handle = self;
    pg_pool_report(self);
    if (seen++ < PG_QUIET_AFTER)
        MDBG("pager: mark dirty (plain) page %d\n", (int)pg_desc_index(desc));
    return ((pg_dirty_fn)pg_g_tramp_dirty_plain)(self, desc);
}

/* Every page the deck reads is fetched through this call, so it reliably
 * provides the handle. */
/* Do not drain a held write here, even though it is the right thread: the
 * caller is still resolving the buffer, and a table write would re-enter the
 * pool and could evict or reuse it. The page writer is safe because the caller
 * is done with the page by then. */
static int64_t pg_wrap_get(uintptr_t self, uint32_t index, uintptr_t *desc)
{
    pg_g_handle = self;
    pg_pool_report(self);
    return ((pg_get_fn)pg_g_tramp_get)(self, index, desc);
}

static int pg_install(void)
{
    int ok = 0;

    ok += (mod_patch_fn("pagerRead", ep122_sym(EP122_DJDB_PAGE_READ),
                        (void *)pg_wrap_read, &pg_g_tramp_read) == 0);
    ok += (mod_patch_fn("pagerWrite", ep122_sym(EP122_DJDB_PAGE_WRITE),
                        (void *)pg_wrap_write, &pg_g_tramp_write) == 0);
    if (!ok) {
        MDBG("pager: neither page op could be taken\n");
        return -1;
    }
    (void)mod_patch_fn("pagerGetPage", ep122_sym(EP122_DJDB_BUFFER_GET_PAGE),
                       (void *)pg_wrap_get, &pg_g_tramp_get);
    (void)mod_patch_fn("pagerDirty", ep122_sym(EP122_DJDB_MARK_DIRTY),
                       (void *)pg_wrap_dirty, &pg_g_tramp_dirty);
    (void)mod_patch_fn("pagerDirtyPlain", ep122_sym(EP122_DJDB_MARK_DIRTY_PLAIN),
                       (void *)pg_wrap_dirty_plain, &pg_g_tramp_dirty_plain);
    return 0;
}

KIT_MOD(k_mod_pager,
        .name = "pager", .prio = 5, .install = pg_install,
        .what = "media library: watch the page store under the tables");
