// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * mods/db/djdb.c - the rekordbox-media half of the provider.
 *
 * A rekordbox stick carries two databases side by side. db.c reads the SQLCipher
 * one -- exportLibrary.db, DEVICE LIBRARY PLUS -- which THIS FIRMWARE DOES NOT
 * USE, so it never borrows a handle at all, measured. What the CDJ-3000 actually
 * browses is the other: DEVICE LIBRARY, export.pdb, reached by the Dsql family
 * under DataBase/DeviceSQL through a `djdb` API addressed by table name.
 *
 * So everything that wants to change something on a DJ's media comes here. There
 * is no second route and no library on the deck to fall back to.
 *
 * WHEN THE DECK IS ACTUALLY IN HERE. Browsing, loading and cueing fire none of
 * the hooked entry points, and the context accessor answers NULL from an idle
 * thread throughout -- the context exists only inside one of the deck's own
 * operations and is cleared between them.
 *
 * THE LIBRARY IS WRITABLE, AND THE ONLY THING OUR COPY OF THE WRITE LACKS IS A
 * CONTEXT. Measured: editing a track's rating on the deck calls the update at
 * the bottom of this file and it returns 0 -- djdbContent/idxContent, key 11,
 * column 15, two columns -- and the pages it touches are marked dirty and
 * written to the media there and then, not at eject. So the tempo write is that
 * same call with column 8, and what stops it from an idle thread is -10001, no
 * context, never -10025, wrong state. It is retried from inside the deck's own
 * update, where a context exists by definition.
 *
 * THROUGH THE DECK'S OWN OBJECTS, NEVER THE FILE. The same rule db.h states, and
 * it bites harder here: export.pdb is a proprietary format the deck holds
 * indexes and caches over, so writing it behind the deck's back is corrupted by
 * the next flush even when the bytes are right.
 *
 * ---- reading first, and that order was the point ---------------------------
 *
 * Everything above the write is mod_safe_read over structures the deck already
 * has open: the table registry is a hash table hanging off the context, so
 * enumerating it is pointer-walking rather than API use, and pointer-walking
 * cannot have a side effect on a DJ's library.
 *
 * That is what made the one write safe to add. The questions a writer needs
 * answered -- which column a track's tempo is, what a value looks like, which
 * row belongs to the loaded track -- were all settled by looking, the last of
 * them against the library on a real stick, before a byte was written.
 *
 * ---- the shapes, from sub_1c66f80 and sub_1c915d0 --------------------------
 *
 *   ctx + 0x08     13 hash buckets, 8 bytes each -- the table registry
 *   table + 0x10   its name, as a packed djdb string
 *   table + 0x18   the column array, 32 bytes per column
 *   table + 0x78   next in the hash chain
 *   col + 0x08     -> a type object, whose +0x08 is the type id
 *
 * A packed string is the two encodings sub_1c88520 produces: a short form with a
 * one-byte header `((len+1) << 1) | 1` and the bytes after it, and a long form
 * whose header word is `((len+4) << 8) | 0x40` with the bytes at +4. The low bit
 * tells them apart.
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



/* THE STATE GATE IS NOT ON THE CONTEXT. sub_1c66f80 reads it from
 * sub_1c5dd00(ctx), not from ctx -- sub_1c97b70 is `x0 ? sub_1c5dd00(x0) : 0`,
 * not the identity, which is easy to read past. So a writer has to make that
 * second call before believing state 2; a reader does not need the gate at all,
 * because every access below is a bounded mod_safe_read and a context that is
 * not ready simply reads as empty. Left as a note rather than a wrong check. */



typedef void *(*djdb_ctx_fn)(void);



/* The live context, or NULL. Through the deck's accessor rather than the global
 * behind it, because the accessor is what honours the override hook. */
void *djdb_ctx(void)
{
    uintptr_t fn = ep122_sym(EP122_DJDB_CONTEXT);

    if (!fn)
        return NULL;
    return ((djdb_ctx_fn)fn)();
}

/* THE CONTEXT IS ONLY LIVE INSIDE AN OPERATION.
 *
 * Measured: with media mounted, the library browsed and its tables in use, the
 * accessor still answers NULL from the worker thread. It is not a "has media"
 * flag -- it is set for the duration of a djdb call and cleared between them, so
 * an idle thread can never see one. That is why this observes the deck's own
 * insert rather than polling: inside that call the context is by definition
 * valid, which is the only moment the registry can honestly be read.
 *
 * The stock call is made with its arguments untouched and its result returned
 * verbatim. [the deck's database thread] */
typedef int64_t (*djdb_insert_fn)(const char *table, int ncols, void **values);
typedef int64_t (*djdb_any_fn)(void *, void *, void *, void *, void *, void *,
                               void *);

static uintptr_t djdb_g_tramp_insert;
static uintptr_t djdb_g_tramp_update;
static uintptr_t djdb_g_tramp_txn_a;
static uintptr_t djdb_g_tramp_txn_b;

/* Once, from whichever entry point the deck reaches first. */

/* Any hooked entry point is a chance to write a tempo that was refused; see
 * the note above its definition. */
static void djdb_drain_pending(const char *where);

/* ...and to run a reorder the UI asked for from a thread that could not. */
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

/* The remaining three are forwarded blind: their argument lists are not
 * established, so the wrapper takes the seven registers the ABI passes in and
 * hands them straight back. A wrapper that does not name its arguments cannot
 * get them wrong. */

static int64_t djdb_wrap_update(void *a, void *b, void *c, void *d, void *e,
                                void *f, void *g)
{
    int64_t r;

    djdb_try_dump("the update path", NULL);
    /* This IS the query entry point, so the deck's own playlist cursor comes
     * through here with the id the reorder needs. Read on the way in, before
     * the call, so a query that throws still leaves the id it named. */
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

/* THE FLUSH, which is the one moment a context is certainly live, is the page
 * writer -- and that belongs to pager.c, which understands its arguments. It
 * calls mod_djdb_note() above, so the registry is still read from inside it. */

void mod_djdb_poll(void)
{
    /* Nothing to poll: see the note above. Kept so the worker's call site does
     * not have to know why, and so a future readiness check has a home. */
}

/* ---- the one write ---------------------------------------------------------
 *
 * The tempo the BROWSER shows is not the beat grid. A grid lives in the track's
 * analysis file and the deck's own register writes it (see stem/grid.c); the
 * number beside the title in the list is `DJDBCONTENT.BPM` in the media library,
 * a different back end entirely. So a x2 that does not come here moves the grid
 * and the play screen and leaves the browser saying the old tempo.
 *
 * THE DECK HAS THE PRIMITIVE, it just never points it at this column.
 * `music_library::DsqlTrackUpdater` updates `djdbContent` BY COLUMN INDEX
 * through djdb's own update -- columns 15 (RATING) and 40, and 42 -- and BPM is
 * column 8 of that same table with the same integer flavour. This is that call
 * with a different column id: the deck's converter, the deck's index
 * maintenance, the deck's transaction, and the deck's own eject flush to put it
 * on the media.
 *
 * A VALUE IS A POINTER TO A SCALAR, width following the column's declared type
 * -- proved by the deck's own djdbSongHistory insert, which builds an int32, an
 * int16, an int32 and a byte on the stack and passes the address of each. The
 * width of type 1 is not established, so the value here is written into a zeroed
 * eight-byte buffer: whatever width the column turns out to be, the bytes it
 * reads are the right ones, little-endian.
 *
 * THE CONTENT ID IS trackid::TrackID's THIRD WORD. Checked read-only against the
 * library on a real stick, three tracks, before anything was ever written:
 * tempo 12000 sits in the row with id 8, 12500 with 11, 12600 with 7 -- and
 * those are exactly the third words of those tracks' TrackIDs. A wrong id here
 * writes one track's tempo onto another's row, which is why it was settled by
 * reading rather than by trying.
 *
 * Threading: [message], from the grid panel's save, at human rate.
 */

/* djdbContent's own name and index, and the column BPM is. */
#define DJDB_CONTENT_TABLE  "djdbContent"
#define DJDB_CONTENT_INDEX  "idxContent"
#define DJDB_COL_BPM        8


/* THE COLUMN CONVENTION IS ONE CONVENTION -- measured, not assumed.
 * sub_1c5c1b0(row, n) takes the COLUMN index, the same numbering colids use:
 *
 *   content key 7:      [0]=7  [8]=12600        id and BPM, and colid 8 is
 *                                               already proven by our own write
 *   songplaylist key 1: [0]=1  [1]=10  [2]=8    playlistid constant, and
 *                       [0]=1  [1]=9   [2]=7    CONTENTID != TRACKNO in these
 *                                               two rows, which is what makes
 *                                               them tell the two apart
 *
 * So the on-disk FIELD order (2/1/0 for this table) never enters a caller's
 * arithmetic. Both readings agree, from different tables. */

/* Wide enough for any width the column turns out to be. */
#define DJDB_VAL_BYTES      8

typedef int (*djdb_update_fn)(const char *table, const char *index,
                              void *a3, void *a4, const char *op,
                              int nkeys, void **keyvals,
                              int ncols, const int32_t *colids,
                              void **colvals);

/* WHAT THIS NEEDS, AND IT IS A CONTEXT RATHER THAN A STATE.
 *
 * The call itself is right -- content id, table, index, column and value all
 * check out against the deck's own DsqlTrackUpdater, and that updater reaches
 * this same function and is answered 0. So the library IS open for table
 * writes, and the two ways our copy has been refused are not the same refusal:
 *
 *     from the panel            -10001   no context
 *     inside the eject flush     -6503   "Storage manager is not open"
 *
 * -10025, "state is not 2", has never come back. The state gate is satisfied;
 * what an idle thread has not got is a context. The accessor manufactures one
 * per call through the override hook below rather than keeping one to borrow,
 * and it answers NULL for a thread the deck did not send here.
 *
 * So a tempo refused from the panel is HELD, and written from inside the deck's
 * own update, where the context and the transaction both already exist. A
 * playlist reorder is the same shape: DJDBSONGPLAYLIST.TRACKNO through the same
 * call. See [ep122_djdb_api].
 *
 * Threading: [message], from the grid panel's save, at human rate; the retry is
 * [the deck's database thread].
 */

/* WHAT THE DECK ITSELF PASSES, when it writes a rating or a colour. The
 * refusals below are ours; this reports the deck's own, which is the only way
 * to tell "the library is never writable" apart from "it is writable and we
 * asked at the wrong moment". Table, index, key and column come straight off
 * the arguments, and the result is djdb's own return.
 *
 * Our own write below reaches the stock call through the trampoline, so it is
 * reported here too rather than recursing. [the deck's database thread]
 *
 * IT IS ALSO THE MOMENT A TEMPO CAN BE WRITTEN. The call works -- the deck's
 * own rating edit returns 0 through it -- and what our copy of it lacks is a
 * context, which the accessor manufactures per thread and does not for ours.
 * Inside here there is one by definition, so a tempo refused earlier is
 * retried from the deck's own transaction on the deck's own thread. */

static uintptr_t djdb_g_tramp_update_row;

/* One tempo waiting for a context. The last edit wins: two rescales of the
 * same track before a moment arrives should leave the newer one. */
static uint32_t djdb_g_pending_id;
static int      djdb_g_pending_bpm;

static int djdb_write_bpm(uint32_t content_id, int bpm_x100);

/* Take the moment if this one has a context.
 *
 * Called from every hooked entry point rather than from the update alone,
 * because any of them can be the one the deck reaches first and the thread is
 * what matters, not which operation it is. The context check is the whole gate:
 * it is exactly what -10001 reports, so asking first costs nothing and a
 * refusal here means this was not a moment after all.
 *
 * The pending value SURVIVES a failure -- the eject flush has a context and a
 * closed storage manager, and dropping a DJ's edit there would be the worst
 * possible time.
 *
 * CONSECUTIVE failures are bounded, not total attempts. Counting every attempt
 * looks equivalent and is not: a moment now arrives within a message of the
 * edit, so each edit spends one attempt, and a total bound would silently stop
 * writing tempos after the sixty-fourth edit of a session -- long after anyone
 * would connect it to this. The count resets whenever one lands.
 *
 * Re-entrant by construction: the write goes through the patched update, which
 * calls back in here. [the deck's database thread] */
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
    /* IS THIS THE LIST THE DJ WAS LOOKING AT? The id is a latch off a query
     * cursor and nothing proves it names the list on screen, so the one fact
     * the caller can offer -- how many rows it counted -- is checked against
     * the playlist about to be rewritten. It does not separate two playlists
     * of the same length, and it does catch every other mismatch, which is
     * worth having between here and a shuffled playlist. */
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

    /* Everything between the two positions shifts one place away from where the
     * entry came from; the entry itself lands on `to_no`. */
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

/* ---- WHICH PLAYLIST THE LIST ON SCREEN IS, off the collector's own caches ----
 *
 * The UI carries a hierarchy and never a table key, and the deck does not query
 * djdb while browsing, so neither end has the id. The COLLECTOR does: every
 * cached list keeps the condition it was asked for, and a track list's condition
 * carries the hierarchy that named it. The deck reads it back the same way in
 * removePlaylistTrackListCache, whose predicate is where this shape comes from.
 *
 *   ListCache          +0x10  the ListCondition it answered
 *                      +0x28  the serial the collector stamped it with
 *   ListCondition      +0x08  u16 category; 4 == a track list
 *   TrackListCondition +0x10  u16 the source kind; 5 == a playlist
 *                      +0x18  vector<{u16 kind; u32 id; u32}> -- the hierarchy
 *
 * The playlist id is the id of the LAST step, which is what the deck's predicate
 * reads as *(u32 *)(end - 8). A condition is identified by its vptr rather than
 * by __dynamic_cast: the cast is an identity here -- TrackListCondition derives
 * from ListCondition at offset 0 -- so the pointer compare is the same test
 * without calling into the runtime. */
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
/* A vector this long is a pointer that is not a vector. The deck trims itself to
 * 0x14 caches at the top of every createListCache. */
#define LCC_SANE_CACHES   64

/* The id a single cached list was asked for, or 0 if it is not a playlist's. */
static uint32_t djdb_cache_playlist(uintptr_t cache, uint32_t *serial)
{
    uintptr_t cond, hb, he;
    uint16_t  kind;
    uint32_t  id;

    if (mod_safe_read(cache + LC_CONDITION, &cond, sizeof cond) != 0 || !cond)
        return 0;
    if (mod_safe_read(cond, &hb, sizeof hb) != 0 ||
        hb != ep122_sym(EP122_TRACK_LIST_CONDITION))
        return 0;
    if (mod_safe_read(cond + TLC_KIND, &kind, sizeof kind) != 0 ||
        kind != TLC_FROM_PLAYLIST)
        return 0;
    if (mod_safe_read(cond + TLC_HIER, &hb, sizeof hb) != 0 ||
        mod_safe_read(cond + TLC_HIER_END, &he, sizeof he) != 0)
        return 0;
    if (he <= hb || (he - hb) % HIER_STEP)
        return 0;
    if (mod_safe_read(he - HIER_STEP + HIER_STEP_ID, &id, sizeof id) != 0 || !id)
        return 0;
    if (serial)
        mod_safe_read(cache + LC_SERIAL, serial, sizeof *serial);
    return id;
}

/* The newest playlist list-cache, preferring one SOMEONE ELSE STILL HOLDS.
 *
 * Both halves are the collector's own vocabulary. The serial at +0x28 counts up
 * once per cached list, so the largest is the most recently opened. And the deck
 * purges caches whose use count is 1 -- see ListCacheCollector's own sweep --
 * because a use count of 1 means nobody but the collector is looking at it; the
 * list on screen is by definition held by something else.
 *
 * `*held`: the pick is a held one. `verbose`: log every candidate. */
static uint32_t djdb_scan_caches(int verbose, int *held)
{
    uintptr_t p, end;
    uint32_t  best = 0, best_serial = 0;
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
        int       is_held;

        if (mod_safe_read(p, &cache, sizeof cache) != 0 || !cache)
            continue;
        id = djdb_cache_playlist(cache, &serial);
        if (!id)
            continue;
        if (mod_safe_read(p + 8, &ctrl, sizeof ctrl) == 0 && ctrl)
            mod_safe_read(ctrl + 8, &use, sizeof use);
        is_held = use > 1;
        seen++;
        if (verbose)
            MDBG("djdb: cached list #%u is playlist %u, %s (use %u)\n",
                 (unsigned)serial, (unsigned)id, is_held ? "held" : "loose",
                 (unsigned)use);
        if (is_held < best_held)
            continue;
        if (is_held > best_held || serial >= best_serial) {
            best = id;
            best_serial = serial;
            best_held = is_held;
        }
    }
    if (verbose && !seen)
        MDBG("djdb: the collector holds no playlist list-cache\n");
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
    /* All three, in the order the deck drops them when a playlist's contents
     * change: the list's rows, the list of playlists, and the PLAYLIST branch of
     * the hierarchy. Dropping only the first leaves two other copies of the old
     * order for the deck to serve from. */
    ((void (*)(uintptr_t, uint32_t))fn)(djdb_g_collector, playlist_id);
    fn = ep122_sym(EP122_ML_DROP_PLAYLIST_LIST);
    if (fn)
        ((void (*)(uintptr_t, uint32_t))fn)(djdb_g_collector, 0xffffffffu);
    fn = ep122_sym(EP122_ML_DROP_HIERARCHY);
    if (fn)
        ((void (*)(uintptr_t, uint16_t))fn)(djdb_g_collector, DJDB_CATEGORY_PLAYLIST);
    MDBG("djdb: playlist %u's cached rows dropped\n", (unsigned)playlist_id);
}

/* The queued reorder, on a thread that has a context. Cleared whether it
 * succeeded or not: mod_djdb_move_track already refuses an entry it cannot find
 * and says so, and retrying a move the library rejected would only reorder
 * something else later. */
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

/* THE CONTEXT IS PER-THREAD, AND THE MESSAGE THREAD IS NOT ONE OF THEM.
 *
 * Worth stating as a measurement rather than as an assumption, because it is the
 * reason this queues at all. djdbGetContext (0x1c97b50) is a redirectable
 * getter: a byte at 0x66d6a00 chooses between a plain global at +0x08 and a
 * registered `fn(arg)` at +0x18. On the deck the byte is 1, so it takes the
 * second -- and that function (0xe5d978) reads a key off the CURRENT THREAD and
 * walks a list of {thread, handle} for it. A thread the deck never registered
 * has no handle to find, and the UI thread is one of those.
 *
 * So there is nothing to try inline and nothing to borrow: the move waits for a
 * thread that owns a handle, which is what djdb_wrap_msg_run is. */
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

/* The update, exactly as the deck makes it for a track's rating: the row picked
 * by content id through djdbContent's own index, one column stated by number,
 * the value handed over as a pointer for djdb's converter to take by the
 * column's declared type. */
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
        /* A fresh edit gets a fresh budget: whatever exhausted the last one was
         * about that moment, not about this DJ's next press. */
        djdb_g_fails = 0;
    }
    return r == 0 ? 0 : -1;
}

/* WHERE A CONTEXT COMES FROM. The accessor is
 *
 *     flag = *(uint8 *)slot
 *     if (flag) { arg = *(void **)(slot + 0x10); fn = *(slot + 0x18); return fn(arg); }
 *     else        return *(void **)(slot + 0x08);
/* THE MOMENT THAT MAKES A HELD WRITE PROMPT.
 *
 * A context is a per-thread DB handle and no djdb entry point fires during
 * browsing, so waiting for one means waiting for a rating edit or a media mount.
 * This is the library server's own thread popping a message off its queue and
 * dispatching it -- the run slot of the AsyncTask that
 * ServerThreadBase::receiveMessage posts.
 *
 * AFTER the stock call, deliberately: by then the message has been handled, the
 * queue mutex is released and nothing is in flight, so a write from here is not
 * nested inside any library operation. Every ServerThreadBase shares this
 * vtable, so it fires on the SD, cloud and repository threads too; only a thread
 * with a DB handle has a context, so the drain selects itself and the rest costs
 * one comparison. [a library server thread] */
static uintptr_t djdb_g_stock_msg_run;

static uint64_t djdb_wrap_msg_run(uintptr_t task)
{
    uint64_t r = ((uint64_t (*)(uintptr_t))djdb_g_stock_msg_run)(task);

    if (djdb_g_pending_id)
        djdb_drain_pending("a library message");
    /* And the reorder, for exactly the same reason. It was left on the djdb
     * entry points alone, which never fire: the deck warms a list cache when the
     * media is announced and browses out of it, so a whole session can pass with
     * no query at all -- measured, and a move parked there simply never
     * happened. A cached list fill still runs a MESSAGE, which is this. */
    if (djdb_g_move_pid)
        djdb_drain_move("a library message");

    /* Once per run, on the first message that brings a context with it: read a
     * few playlists so the column convention can be settled against the stick.
     * Guarded because the query is itself hooked and would otherwise re-enter. */
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
    /* ANY of them will do, so none of them is required. Each is a separate
     * observation point on the same context, and mod_patch_fn refuses a
     * prologue it cannot displace rather than corrupting it -- so a refusal
     * here costs one of four ways in, not the feature. */
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

    /* Not required: without it a held tempo still lands, just at the next rating
     * edit or media mount instead of the next library message. */
    (void)mod_patch_vslot("djdbMsgRun", EP122_SRV_MSG_TASK, 0x10,
                          (void *)djdb_wrap_msg_run, &djdb_g_stock_msg_run);
    return 0;
}

KIT_MOD(k_mod_djdb,
        .name = "djdb", .prio = 4, .install = djdb_install,
        .what = "media library: read the deck's own rekordbox tables");
