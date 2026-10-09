// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * mods/db/djdb.c - the rekordbox-media half of the provider.
 *
 * A rekordbox stick carries two databases. db.c reads the SQLCipher one
 * (exportLibrary.db, DEVICE LIBRARY PLUS), which this firmware does not use; it
 * never borrows a handle. The deck browses DEVICE LIBRARY, export.pdb,
 * through the Dsql family under DataBase/DeviceSQL and a `djdb` API addressed
 * by table name. Any change to the DJ's media goes through here.
 *
 * Browsing, loading and cueing call none of the hooked entry points, and the
 * context accessor returns NULL from an idle thread: the context exists only
 * inside one of the deck's own operations.
 *
 * The library is writable; our write only lacks a context. Editing a track's
 * rating on the deck calls the update at the bottom of this file and it
 * returns 0 (djdbContent/idxContent, column 15, two columns), and the
 * touched pages are marked dirty and written to the media immediately, not at
 * eject. The tempo write is the same call with column 8. From an idle thread it
 * fails with -10001 (no context), never -10025 (wrong state), so it is retried
 * from inside the deck's own update.
 *
 * Same rule as db.h: go through the deck's objects, never the file. export.pdb
 * is a proprietary format the deck keeps indexes and caches over, so a write
 * behind its back is corrupted by the next flush even if the bytes are right.
 *
 * ---- reading ----------------------------------------------------------------
 *
 * The table registry is a hash table hanging off the context; it is read with
 * mod_safe_read, which has no side effect on the library.
 *
 * ---- the shapes ------------------------------------------------------------
 *
 *   ctx + 0x08     13 hash buckets, 8 bytes each -- the table registry
 *   table + 0x10   its name, as a packed djdb string
 *   table + 0x18   the column array, 32 bytes per column
 *   table + 0x78   next in the hash chain
 *   col + 0x08     -> a type object, whose +0x08 is the type id
 *
 * A packed string has one of two encodings: a short form with a one-byte header
 * `((len+1) << 1) | 1` and the bytes after it, and a long form whose header
 * word is `((len+4) << 8) | 0x40` with the bytes at +4. The low bit tells them
 * apart.
 *
 * Threading. [worker], once, from the idle branch. Nothing here is on a hot path.
 */
#include "db/djdb_internal.h"

#include "core/mod_core.h"
#include "core/ep122_syms.h"
#include "kit/mod.h"

#ifndef SYS_gettid
#define SYS_gettid 178
#endif



/* The state gate is not on the context: the row insert reads it from an object
 * the deck derives from the context through a separate, NULL-safe call (not the
 * identity). A writer must make that call before trusting state 2. Readers do
 * not need the gate: every access is a bounded mod_safe_read, and a context
 * that is not ready reads as empty. */



typedef void *(*djdb_ctx_fn)(void);



/* The live context, or NULL. Uses the deck's accessor, not the global behind
 * it, because the accessor honours the override hook. */
void *djdb_ctx(void)
{
    uintptr_t fn = ep122_sym(EP122_DJDB_CONTEXT);

    if (!fn)
        return NULL;
    return ((djdb_ctx_fn)fn)();
}

/* The context is only live inside a djdb operation. With media mounted and
 * browsed, the accessor still returns NULL from the worker thread.
 * So the registry is read from inside the deck's own calls instead of by
 * polling.
 *
 * The stock call gets its arguments unchanged and its result is returned as
 * is. [the deck's database thread] */
typedef int64_t (*djdb_insert_fn)(const char *table, int ncols, void **values);
typedef int64_t (*djdb_any_fn)(void *, void *, void *, void *, void *, void *,
                               void *);

static uintptr_t djdb_g_tramp_insert;
static uintptr_t djdb_g_tramp_update;
static uintptr_t djdb_g_tramp_txn_a;
static uintptr_t djdb_g_tramp_txn_b;

/* Once, from whichever entry point the deck reaches first. */

/* Any hooked entry point may retry a refused tempo write; see
 * djdb_drain_pending. */
static void djdb_drain_pending(const char *where);

/* ...and run a reorder queued from the UI thread. */
static void djdb_drain_move(const char *where);

void mod_djdb_note(const char *where)
{
    djdb_try_dump(where, NULL);
    djdb_drain_pending(where);
    djdb_drain_move(where);
}

static int64_t djdb_wrap_insert(const char *table, int ncols, void **values)
{
    int64_t r;

    djdb_try_dump("insert", table);
    r = ((djdb_insert_fn)djdb_g_tramp_insert)(table, ncols, values);
    djdb_drain_pending("an insert");
    return r;
}

/* The remaining three are forwarded blind: their argument lists are unknown,
 * so the wrapper passes the seven argument registers through unchanged. */

static int64_t djdb_wrap_update(void *a, void *b, void *c, void *d, void *e,
                                void *f, void *g)
{
    int64_t r;

    djdb_try_dump("the update path", NULL);
    /* This is the query entry point, so the deck's playlist cursor passes
     * through here with the id the reorder needs. Read before the call, so a
     * query that throws still records its id. */
    djdb_note_playlist((const char *)a, (int)(intptr_t)f, (void **)g);
    r = ((djdb_any_fn)djdb_g_tramp_update)(a, b, c, d, e, f, g);
    djdb_drain_pending("the update path");
    djdb_drain_move("the update path");
    djdb_try_walk();
    return r;
}

static int64_t djdb_wrap_txn_a(void *a, void *b, void *c, void *d, void *e,
                               void *f, void *g)
{
    djdb_try_dump("transaction A", NULL);
    return ((djdb_any_fn)djdb_g_tramp_txn_a)(a, b, c, d, e, f, g);
}

static int64_t djdb_wrap_txn_b(void *a, void *b, void *c, void *d, void *e,
                               void *f, void *g)
{
    djdb_try_dump("transaction B", NULL);
    return ((djdb_any_fn)djdb_g_tramp_txn_b)(a, b, c, d, e, f, g);
}

/* The flush, where a context is certainly live, is the page writer hooked in
 * pager.c. It calls mod_djdb_note() above, so the registry is read there too. */

void mod_djdb_poll(void)
{
    /* Nothing to poll (see above). Kept for the worker's call site and for a
     * future readiness check. */
}

/* ---- the tempo write -------------------------------------------------------
 *
 * The tempo the browser shows is not the beat grid. The grid lives in the
 * track's analysis file and is written by the deck's register (see
 * stem/grid.c); the number beside the title is `DJDBCONTENT.BPM` in the media
 * library. Without this write, a x2 changes the grid and the play screen but
 * the browser keeps the old tempo.
 *
 * `music_library::DsqlTrackUpdater` updates `djdbContent` by column index
 * through djdb's update (columns 15 (RATING), 40 and 42), and BPM is column 8 of
 * the same table with the same integer type. This is that call with column 8,
 * so it uses the deck's converter, index maintenance, transaction and flush.
 *
 * A value is a pointer to a scalar whose width follows the column's declared
 * type, as in the deck's djdbSongHistory insert (an int32, an int16, an int32
 * and a byte on the stack, passed by address). The width of type 1 is unknown,
 * so the value goes in a zeroed eight-byte buffer, which reads correctly
 * (little-endian) at any width.
 *
 * The content id is trackid::TrackID's third word.
 *
 * Threading: [message], from the grid panel's save.
 */

/* djdbContent's name, index, and BPM column. */
#define DJDB_CONTENT_TABLE  "djdbContent"
#define DJDB_CONTENT_INDEX  "idxContent"
#define DJDB_COL_BPM        8


/* EP122_DJDB_ROW_COL(row, n) takes the column index, the same numbering colids
 * use (djdbContent: [0] id, [8] BPM; djdbSongPlaylist: [0] PLAYLISTID,
 * [1] CONTENTID, [2] TRACKNO). Callers never use the on-disk field order
 * (2/1/0 for djdbSongPlaylist). */

/* Wide enough for any column width. */
#define DJDB_VAL_BYTES      8

typedef int (*djdb_update_fn)(const char *table, const char *index,
                              void *a3, void *a4, const char *op,
                              int nkeys, void **keyvals,
                              int ncols, const int32_t *colids,
                              void **colvals);

/* The write needs a context, not a state. Content id, table, index, column and
 * value match the deck's DsqlTrackUpdater, which reaches the same function and
 * gets 0, so the library is open for writes. Our refusals:
 *
 *     from the panel            -10001   no context
 *     inside the eject flush     -6503   "Storage manager is not open"
 *
 * -10025 ("state is not 2") has never been returned. The accessor creates a
 * context per call through the override hook below and returns NULL for a
 * thread the deck did not register.
 *
 * So a tempo refused from the panel is held and written from inside the deck's
 * own update, where context and transaction already exist. A playlist reorder
 * (DJDBSONGPLAYLIST.TRACKNO) uses the same call.
 *
 * Threading: [message], from the grid panel's save; the retry runs on [the
 * deck's database thread].
 */

/* djdb_wrap_update_row logs the deck's own updates (a rating or a colour):
 * table, index, key, column and djdb's result. This tells "never writable"
 * apart from "writable, but we asked at the wrong moment".
 *
 * Our write reaches the stock call through the trampoline, so it is logged
 * there too without recursing. [the deck's database thread]
 *
 * A context is live inside that call, so a tempo refused earlier is retried
 * from there, in the deck's transaction on the deck's thread. */

static uintptr_t djdb_g_tramp_update_row;

/* One tempo waiting for a context. The last edit wins. */
static uint32_t djdb_g_pending_id;
static int      djdb_g_pending_bpm;

static int djdb_write_bpm(uint32_t content_id, int bpm_x100);

/* djdb_drain_pending: write the held tempo if this thread has a context.
 *
 * Called from every hooked entry point, since any of them can be reached first
 * and only the thread matters. The context check is the gate; it is exactly
 * what -10001 reports.
 *
 * The pending value survives a failure: the eject flush has a context but a
 * closed storage manager.
 *
 * DJDB_DRAIN_FAILS bounds consecutive failures, not total attempts; each edit
 * spends an attempt, so a total bound would stop tempo writes after 64 edits in
 * a session. The count resets on success.
 *
 * Re-entrant: the write goes through the patched update, which calls back in
 * here. [the deck's database thread] */
#define DJDB_DRAIN_FAILS  64


struct djdb_walk {
    const char *what;
    uint32_t    key;
    int         rows;
    int         col[DJDB_WALK_COLS];
};

typedef uintptr_t (*djdb_rowcol_fn)(uintptr_t row, int col);
/* Never true: see above. */
static int djdb_walk_row(void *ctx, uintptr_t row)
{
    struct djdb_walk *w = (struct djdb_walk *)ctx;
    uintptr_t colfn = ep122_sym(EP122_DJDB_ROW_COL);
    char line[128];
    int32_t v;
    int i, n = 0;

    for (i = 0; i < DJDB_WALK_COLS; i++) {
        uintptr_t p = 0;

        v = -1;
        if (colfn && row && w->col[i] >= 0) {
            p = ((djdb_rowcol_fn)colfn)(row, w->col[i]);
            if (p)
                (void)mod_safe_read(p, &v, sizeof(v));
        }
        if (w->col[i] >= 0)
            n += snprintf(line + n, sizeof(line) - (size_t)n, " [%d]=%d",
                          w->col[i], (int)v);
    }
    if (w->rows++ < 16)
        MDBG("djdb:   %s key %u row %2d:%s\n", w->what, (unsigned)w->key,
             w->rows - 1, line);
    return 0;
}

/* Read-only: the filter above never selects, so the query matches nothing. */
void djdb_walk(const char *what, const char *table, const char *index,
                      uint32_t key, int c0, int c1, int c2, int c3)
{
    uintptr_t fn = ep122_sym(EP122_DJDB_QUERY);
    struct djdb_walk w;
    uint32_t k = key;
    void *keyvals[1];
    int r;

    if (!fn)
        return;
    w.what = what;
    w.key = key;
    w.rows = 0;
    w.col[0] = c0; w.col[1] = c1; w.col[2] = c2; w.col[3] = c3;
    keyvals[0] = &k;
    r = ((djdb_query_fn)fn)(table, index, (void *)djdb_walk_row, &w, "=",
                            1, keyvals);
    MDBG("djdb: %s key %u -> %d rows, query %d\n", what, (unsigned)key,
         w.rows, r);
}

/* Counted once the tables are reachable; defined with the playlist reader. */
static void djdb_count_playlists(void);



/* Reads one column of a row, or `miss`. */
static int32_t djdb_row_i32(uintptr_t row, int col, int32_t miss)
{
    uintptr_t colfn = ep122_sym(EP122_DJDB_ROW_COL), p;
    int32_t v = miss;

    if (!colfn || !row)
        return miss;
    p = ((djdb_rowcol_fn)colfn)(row, col);
    if (!p || mod_safe_read(p, &v, sizeof(v)) != 0)
        return miss;
    return v;
}

int djdb_collect_row(void *ctx, uintptr_t row)
{
    struct djdb_plist *pl = (struct djdb_plist *)ctx;

    if (pl->n < DJDB_PL_MAX) {
        pl->e[pl->n].content = djdb_row_i32(row, DJDB_COL_CONTENTID, -1);
        pl->e[pl->n].no      = djdb_row_i32(row, DJDB_COL_TRACKNO, -1);
        if (pl->e[pl->n].content >= 0 && pl->e[pl->n].no >= 0)
            pl->n++;
    }
    return 0;                      /* read-only: never select */
}


/* The filter that turns "this playlist" into "this entry". */
struct djdb_pick {
    int32_t content;
};

static int djdb_pick_row(void *ctx, uintptr_t row)
{
    struct djdb_pick *p = (struct djdb_pick *)ctx;

    return djdb_row_i32(row, DJDB_COL_CONTENTID, -1) == p->content;
}

static int djdb_write_trackno(uint32_t pid, int32_t content, int32_t no)
{
    uintptr_t fn = ep122_sym(EP122_DJDB_UPDATE);
    struct djdb_pick pick;
    uint32_t key = pid;
    int32_t  colid = DJDB_COL_TRACKNO;
    uint8_t  val[DJDB_VAL_BYTES];
    void    *keyvals[1], *colvals[1];

    if (!fn)
        return -1;
    pick.content = content;
    memset(val, 0, sizeof(val));
    memcpy(val, &no, sizeof(no));
    keyvals[0] = &key;
    colvals[0] = val;
    return ((djdb_update_fn)fn)(DJDB_PLAYLIST_TABLE, DJDB_PLAYLIST_INDEX,
                                (void *)djdb_pick_row, &pick, "=", 1, keyvals,
                                1, &colid, colvals);
}

int mod_djdb_move_track(uint32_t pid, int32_t from_no, int32_t to_no,
                        int32_t expect_rows)
{
    static struct djdb_plist pl;       /* 8 KiB: not on a worker's stack */
    int i, moved = 0, failed = 0;
    int32_t content_moved = -1;

    if (from_no == to_no || from_no < 1 || to_no < 1)
        return -1;
    if (djdb_read_playlist(pid, &pl) <= 0) {
        MWARN("djdb: playlist %u is empty or unreadable -> no move\n",
             (unsigned)pid);
        return -1;
    }
    /* The id may not name the list on screen, so check the caller's row count
     * against the playlist. Two playlists of the same length still pass. */
    if (expect_rows > 0 && pl.n != expect_rows) {
        MWARN("djdb: playlist %u holds %d entries but the list on screen showed"
             " %d -- refusing, this is not the same list\n",
             (unsigned)pid, pl.n, (int)expect_rows);
        return -1;
    }

    for (i = 0; i < pl.n; i++)
        if (pl.e[i].no == from_no)
            content_moved = pl.e[i].content;
    if (content_moved < 0) {
        MDBG("djdb: playlist %u has no entry at %d -> no move\n",
             (unsigned)pid, (int)from_no);
        return -1;
    }

    /* Entries between the two positions shift one place toward the source;
     * the moved entry lands on `to_no`. */
    for (i = 0; i < pl.n; i++) {
        int32_t n = pl.e[i].no, want = n;

        if (pl.e[i].content == content_moved)
            want = to_no;
        else if (from_no < to_no && n > from_no && n <= to_no)
            want = n - 1;
        else if (from_no > to_no && n >= to_no && n < from_no)
            want = n + 1;
        if (want == n)
            continue;
        if (djdb_write_trackno(pid, pl.e[i].content, want) == 0)
            moved++;
        else
            failed++;
    }
    MDBG("djdb: playlist %u move %d -> %d (content %d): %d rows written,"
         " %d refused\n", (unsigned)pid, (int)from_no, (int)to_no,
         (int)content_moved, moved, failed);
    return failed ? -1 : 0;
}

static unsigned djdb_g_fails;

static uint32_t djdb_g_move_pid;
static int32_t  djdb_g_move_from, djdb_g_move_to, djdb_g_move_rows;

static uintptr_t djdb_g_trim_tramp;
static uintptr_t djdb_g_collector;

static int64_t djdb_wrap_cache_trim(uintptr_t self, uint32_t cap)
{
    if (self && self != djdb_g_collector) {
        djdb_g_collector = self;
        MDBG("djdb: the list-cache collector is %p\n", (void *)self);
    }
    return ((int64_t (*)(uintptr_t, uint32_t))djdb_g_trim_tramp)(self, cap);
}

/* ---- which playlist is on screen, from the collector's caches ---------------
 *
 * The UI carries a hierarchy, not a table key, and the deck does not query djdb
 * while browsing. The collector has the id: each cached list keeps the
 * condition it was requested with, and a track list's condition carries the
 * hierarchy that named it. The layout below comes from the predicate in
 * removePlaylistTrackListCache, which reads it the same way.
 *
 *   ListCache          +0x10  the ListCondition it answered
 *                      +0x28  the serial the collector stamped it with
 *   ListCondition      +0x08  u16 category; 4 == a track list
 *   TrackListCondition +0x10  u16 the source kind; 5 == a playlist
 *                      +0x18  vector<{u16 kind; u32 id; u32}> -- the hierarchy
 *
 * The playlist id is the last step's id, which the deck's predicate reads as
 * *(u32 *)(end - 8). A condition is identified by its vptr instead of
 * __dynamic_cast; TrackListCondition derives from ListCondition at offset 0, so
 * the compare is equivalent. */
#define LCC_CACHES        0x28      /* vector<shared_ptr<ListCache>> begin */
#define LCC_CACHES_END    0x30
#define LC_CONDITION      0x10
#define LC_SERIAL         0x28
#define TLC_KIND          0x10
#define TLC_FROM_PLAYLIST 5
#define TLC_HIER          0x18
#define TLC_HIER_END      0x20
#define HIER_STEP         12        /* {u16 kind; u32 id; u32}, padded */
#define HIER_STEP_ID      4
/* A longer vector means a bad pointer. The deck trims itself to 0x14 caches at
 * the top of every createListCache. */
#define LCC_SANE_CACHES   64

/* The source kind of a cached TRACK list (0 for any other cache), its serial,
 * and -- for a playlist's -- the playlist id, else 0. */
static uint16_t djdb_cache_kind(uintptr_t cache, uint32_t *serial, uint32_t *id)
{
    uintptr_t cond, hb, he;
    uint16_t  kind;

    *id = 0;
    if (mod_safe_read(cache + LC_CONDITION, &cond, sizeof cond) != 0 || !cond)
        return 0;
    if (mod_safe_read(cond, &hb, sizeof hb) != 0 ||
        hb != ep122_sym(EP122_TRACK_LIST_CONDITION))
        return 0;
    if (mod_safe_read(cond + TLC_KIND, &kind, sizeof kind) != 0 || !kind)
        return 0;
    mod_safe_read(cache + LC_SERIAL, serial, sizeof *serial);
    if (kind != TLC_FROM_PLAYLIST)
        return kind;
    if (mod_safe_read(cond + TLC_HIER, &hb, sizeof hb) != 0 ||
        mod_safe_read(cond + TLC_HIER_END, &he, sizeof he) != 0)
        return 0;
    if (he <= hb || (he - hb) % HIER_STEP)
        return kind;
    mod_safe_read(he - HIER_STEP + HIER_STEP_ID, id, sizeof *id);
    return kind;
}

/* The playlist of the newest track-list cache, preferring caches held outside
 * the collector. 0 when that newest cache is another kind of list (an album's,
 * an artist's), even if a playlist's is still held.
 *
 * The serial at +0x28 increments per cached list, so the largest is the most
 * recent. The deck's ListCacheCollector sweep purges caches with use count 1
 * (held only by the collector), so the list on screen has a higher count.
 * Going from a playlist into an album releases the playlist's cache about
 * 130 ms after the album's list appears, so the newest held cache is the right
 * answer, not any held playlist.
 *
 * `*held`: the pick is a held one. `verbose`: log every candidate. */
static uint32_t djdb_scan_caches(int verbose, int *held)
{
    static uint32_t told_serial;
    uintptr_t p, end;
    uint32_t  best = 0, best_serial = 0;
    uint16_t  best_kind = 0;
    int       best_held = 0, seen = 0;

    *held = 0;
    if (!djdb_g_collector)
        return 0;
    if (mod_safe_read(djdb_g_collector + LCC_CACHES, &p, sizeof p) != 0 ||
        mod_safe_read(djdb_g_collector + LCC_CACHES_END, &end, sizeof end) != 0)
        return 0;
    if (end < p || (end - p) % 16 || (end - p) / 16 > LCC_SANE_CACHES)
        return 0;

    for (; p < end; p += 16) {
        uintptr_t cache, ctrl;
        uint32_t  serial = 0, use = 0, id;
        uint16_t  kind;
        int       is_held;

        if (mod_safe_read(p, &cache, sizeof cache) != 0 || !cache)
            continue;
        kind = djdb_cache_kind(cache, &serial, &id);
        if (!kind)
            continue;
        if (mod_safe_read(p + 8, &ctrl, sizeof ctrl) == 0 && ctrl)
            mod_safe_read(ctrl + 8, &use, sizeof use);
        is_held = use > 1;
        seen++;
        if (verbose)
            MDBG("djdb: cached list #%u kind %u playlist %u, %s (use %u)\n",
                 (unsigned)serial, (unsigned)kind, (unsigned)id,
                 is_held ? "held" : "loose", (unsigned)use);
        if (is_held < best_held)
            continue;
        if (is_held > best_held || serial >= best_serial) {
            best = id;
            best_serial = serial;
            best_kind = kind;
            best_held = is_held;
        }
    }
    if (verbose && !seen)
        MDBG("djdb: the collector holds no track-list cache\n");
    if (best_serial != told_serial) {
        told_serial = best_serial;
        MDBG("djdb: newest %s list-cache is #%u, kind %u, playlist %u\n",
             best_held ? "held" : "loose", (unsigned)best_serial,
             (unsigned)best_kind, (unsigned)best);
    }
    *held = best_held;
    return best;
}

uint32_t djdb_playlist_from_caches(void)
{
    int held;

    return djdb_scan_caches(1, &held);
}

uint32_t mod_djdb_playlist_shown(void)
{
    int held;
    uint32_t id = djdb_scan_caches(0, &held);

    return held ? id : 0;
}

void mod_djdb_drop_list_cache(uint32_t playlist_id)
{
    uintptr_t fn = ep122_sym(EP122_ML_DROP_PLAYLIST_CACHE);

    if (!djdb_g_collector || !fn) {
        MDBG("djdb: playlist %u reordered, but its cached rows cannot be "
             "dropped -- no collector seen yet\n", (unsigned)playlist_id);
        return;
    }
    /* All three, in the deck's own order when a playlist changes: the list's
     * rows, the list of playlists, and the PLAYLIST branch of the hierarchy.
     * Each holds a copy of the old order. */
    ((void (*)(uintptr_t, uint32_t))fn)(djdb_g_collector, playlist_id);
    fn = ep122_sym(EP122_ML_DROP_PLAYLIST_LIST);
    if (fn)
        ((void (*)(uintptr_t, uint32_t))fn)(djdb_g_collector, 0xffffffffu);
    fn = ep122_sym(EP122_ML_DROP_HIERARCHY);
    if (fn)
        ((void (*)(uintptr_t, uint16_t))fn)(djdb_g_collector, DJDB_CATEGORY_PLAYLIST);
    MDBG("djdb: playlist %u's cached rows dropped\n", (unsigned)playlist_id);
}

/* Run the queued reorder on a thread with a context. Cleared whether or not it
 * succeeds: mod_djdb_move_track logs its refusals, and retrying a rejected move
 * later could reorder the wrong thing. */
static void djdb_drain_move(const char *where)
{
    static int inside;
    uint32_t pid = djdb_g_move_pid;
    int32_t from = djdb_g_move_from, to = djdb_g_move_to;

    if (inside || !pid)
        return;
    if (!djdb_ctx())
        return;
    inside = 1;
    djdb_g_move_pid = 0;
    MDBG("djdb: the held move, inside %s\n", where);
    if (mod_djdb_move_track(pid, from, to, djdb_g_move_rows) == 0)
        mod_djdb_drop_list_cache(pid);
    inside = 0;
}

/* The context is per-thread, and the message thread has none, so the move is
 * queued. djdbGetContext (EP122_DJDB_CONTEXT) is a redirectable getter: a byte
 * at EP122_DJDB_CONTEXT_SLOT selects between a plain global at +0x08 and a
 * registered `fn(arg)` at +0x18. On the deck the byte is 1, and that function
 * looks up the current thread in a list of {thread, handle}. The UI thread is not
 * registered, so the move waits for a thread that is (djdb_wrap_msg_run). */
int mod_djdb_move_track_async(uint32_t playlist_id, int32_t from_no,
                              int32_t to_no, int32_t expect_rows)
{
    if (!playlist_id || from_no == to_no || from_no <= 0 || to_no <= 0)
        return -1;
    djdb_g_move_pid = playlist_id;
    djdb_g_move_from = from_no;
    djdb_g_move_to = to_no;
    djdb_g_move_rows = expect_rows;
    MDBG("djdb: playlist %u move %d -> %d queued for the library thread\n",
         (unsigned)playlist_id, from_no, to_no);
    return 0;
}

static void djdb_drain_pending(const char *where)
{
    static int inside;
    void    *ctx;
    uint32_t id = djdb_g_pending_id;
    int      bpm = djdb_g_pending_bpm, r;

    if (inside || !id || djdb_g_fails >= DJDB_DRAIN_FAILS)
        return;
    ctx = djdb_ctx();
    if (!ctx)
        return;

    inside = 1;
    r = djdb_write_bpm(id, bpm);
    if (r == 0) {
        djdb_g_pending_id = 0;
        djdb_g_fails = 0;
    } else {
        djdb_g_fails++;
    }
    MDBG("djdb: held tempo for content %u -> %d.%02d = %d, inside %s%s\n",
         (unsigned)id, bpm / 100, bpm % 100, r, where,
         r == 0 ? "  <- the browser's number moved" : "  (still held)");
    inside = 0;
}

static int64_t djdb_wrap_update_row(const char *table, const char *index,
                                    void *a3, void *a4, const char *op,
                                    int nkeys, void **keyvals,
                                    int ncols, const int32_t *colids,
                                    void **colvals)
{
    int64_t  r = ((djdb_update_fn)djdb_g_tramp_update_row)(
                     table, index, a3, a4, op, nkeys, keyvals, ncols,
                     colids, colvals);
    int32_t  col = -1;
    uint32_t key = 0;

    if (ncols > 0 && colids)
        (void)mod_safe_read((uintptr_t)colids, &col, sizeof(col));
    if (nkeys > 0 && keyvals && keyvals[0])
        (void)mod_safe_read((uintptr_t)keyvals[0], &key, sizeof(key));
    MDBG("djdb: UPDATE %s/%s key %u col %d (%d cols) -> %lld"
         "  [tid %ld, ctx %p]\n",
         table ? table : "?", index ? index : "-", (unsigned)key, (int)col,
         ncols, (long long)r, (long)syscall(SYS_gettid), djdb_ctx());
    djdb_drain_pending("the deck's own update");
    djdb_try_walk();
    return r;
}

/* The update as the deck makes it for a track's rating: the row selected by
 * content id through djdbContent's index, one column by number, and the value
 * passed by pointer for djdb's converter to read by the column's type. */
static int djdb_write_bpm(uint32_t content_id, int bpm_x100)
{
    uintptr_t fn = ep122_sym(EP122_DJDB_UPDATE);
    uint8_t   val[DJDB_VAL_BYTES];
    int32_t   colid = DJDB_COL_BPM;
    uint32_t  key = content_id;
    void     *keyvals[1], *colvals[1];

    if (!fn)
        return -1;
    memset(val, 0, sizeof(val));
    memcpy(val, &bpm_x100, sizeof(bpm_x100));
    keyvals[0] = &key;
    colvals[0] = val;
    return ((djdb_update_fn)fn)(DJDB_CONTENT_TABLE, DJDB_CONTENT_INDEX, NULL,
                                NULL, "=", 1, keyvals, 1, &colid, colvals);
}

int mod_djdb_set_bpm(uint32_t content_id, int bpm_x100)
{
    int r;

    if (!content_id || bpm_x100 <= 0)
        return -1;

    r = djdb_write_bpm(content_id, bpm_x100);
    MDBG("djdb: content %u BPM -> %d.%02d = %d%s  [tid %ld, ctx %p]\n",
         (unsigned)content_id, bpm_x100 / 100, bpm_x100 % 100, r,
         r == 0 ? "  <- the browser's number moved" : "  (held)",
         (long)syscall(SYS_gettid), djdb_ctx());
    if (r != 0) {
        djdb_g_pending_bpm = bpm_x100;
        djdb_g_pending_id  = content_id;
        /* A new edit resets the failure budget. */
        djdb_g_fails = 0;
    }
    return r == 0 ? 0 : -1;
}

/* The context accessor is
 *
 *     flag = *(uint8 *)slot
 *     if (flag) { arg = *(void **)(slot + 0x10); fn = *(slot + 0x18); return fn(arg); }
 *     else        return *(void **)(slot + 0x08);
 *
 * A context is a per-thread DB handle and no djdb entry point fires during
 * browsing, so without this hook a held write waits for a rating edit or a
 * media mount. This is the run slot of the AsyncTask that
 * ServerThreadBase::receiveMessage posts: the library server thread
 * dispatching a queued message.
 *
 * The drain runs after the stock call, when the message is handled, the queue
 * mutex is released and nothing is in flight, so the write is not nested in a
 * library operation. Every ServerThreadBase shares this vtable, so it also
 * fires on the SD, cloud and repository threads; only a thread with a DB handle
 * has a context, and the others cost one comparison. [a library server
 * thread] */
static uintptr_t djdb_g_stock_msg_run;

static uint64_t djdb_wrap_msg_run(uintptr_t task)
{
    uint64_t r = ((uint64_t (*)(uintptr_t))djdb_g_stock_msg_run)(task);

    if (djdb_g_pending_id)
        djdb_drain_pending("a library message");
    /* Same for the reorder. The djdb entry points may never fire: the deck
     * browses from its list cache, so a whole session can pass with no query.
     * A cached list fill still runs a message. */
    if (djdb_g_move_pid)
        djdb_drain_move("a library message");

    /* Once per run, on the first message with a context: read a few playlists
     * and log their columns. Guarded because the query is itself hooked and
     * would re-enter. */
    djdb_try_walk();
    return r;
}

static int djdb_install(void)
{
    if (!ep122_sym(EP122_DJDB_CONTEXT)) {
        MDBG("djdb: no context accessor -> media library unreachable\n");
        return -1;
    }
    djdb_report_slot();
    /* Any one hook is enough; none is required. Each is a separate observation
     * point on the same context, and mod_patch_fn refuses a prologue it cannot
     * displace, so a refusal only loses that hook. */
    static const struct {
        const char *name;
        int         sym;
        void       *wrapper;
        uintptr_t  *tramp;
    } k_hooks[] = {
        { "djdbInsert", EP122_DJDB_ROW_INSERT, (void *)djdb_wrap_insert,
          &djdb_g_tramp_insert },
        { "djdbUpdate", EP122_DJDB_QUERY, (void *)djdb_wrap_update,
          &djdb_g_tramp_update },
        { "djdbTxnA",   EP122_DJDB_TXN_A,      (void *)djdb_wrap_txn_a,
          &djdb_g_tramp_txn_a },
        { "djdbTxnB",   EP122_DJDB_TXN_B,      (void *)djdb_wrap_txn_b,
          &djdb_g_tramp_txn_b },
        { "djdbUpdateRow", EP122_DJDB_UPDATE,  (void *)djdb_wrap_update_row,
          &djdb_g_tramp_update_row },
        { "mlCacheTrim", EP122_ML_CACHE_TRIM,
          (void *)djdb_wrap_cache_trim, &djdb_g_trim_tramp },
    };
    int i, ok = 0;

    for (i = 0; i < (int)(sizeof(k_hooks) / sizeof(k_hooks[0])); i++)
        ok += (mod_patch_fn(k_hooks[i].name, ep122_sym(k_hooks[i].sym),
                            k_hooks[i].wrapper, k_hooks[i].tramp) == 0);

    if (!ok) {
        MDBG("djdb: no entry point could be observed -> registry unreadable\n");
        return -1;
    }
    MDBG("djdb: watching %d of 5 entry points\n", ok);

    /* Optional: without it a held tempo lands at the next rating edit or media
     * mount instead of the next library message. */
    (void)mod_patch_vslot("djdbMsgRun", EP122_SRV_MSG_TASK, 0x10,
                          (void *)djdb_wrap_msg_run, &djdb_g_stock_msg_run);
    return 0;
}

KIT_MOD(k_mod_djdb,
        .name = "djdb", .prio = 4, .install = djdb_install,
        .what = "media library: read the deck's own rekordbox tables");
