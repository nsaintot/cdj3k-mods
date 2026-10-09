/* SPDX-License-Identifier: MIT OR Apache-2.0 */
/*
 * mods/db/djdb_internal.h - what the djdb_*.c files share.
 *
 * The provider is split by job: djdb.c owns the hooks, the trampolines and the
 * write path, djdb_playlist.c the browser's open list, djdb_dump.c the debug
 * dump. Private to db/; db.h is the public interface.
 */
#ifndef EP122_MOD_DJDB_INTERNAL_H
#define EP122_MOD_DJDB_INTERNAL_H

#include "core/mod_core.h"
#include "db/db.h"

/* The content table, which carries the tempo column. */
#define DJDB_CONTENT_TABLE  "djdbContent"
#define DJDB_CONTENT_INDEX  "idxContent"
#define DJDB_COL_BPM        8

/* The deck's own table, index and column layout, read by every file here. */
/* The table registry. */
#define DJDB_BUCKETS        13
#define DJDB_BUCKETS_OFF    0x08
#define DJDB_TBL_NAME_OFF   0x10
#define DJDB_TBL_COLS_OFF   0x18
#define DJDB_TBL_NEXT_OFF   0x78
#define DJDB_COL_STRIDE     0x20
#define DJDB_COL_TYPE_OFF   0x08
/* The index chain: a table's first index, each index's packed name, and the
 * next in the chain. The columns an index covers are not
 * known, so the words around the name are dumped for reading. */
#define DJDB_TBL_IDX_OFF    0x20
#define DJDB_IDX_NAME_OFF   0x40
#define DJDB_IDX_NEXT_OFF   0x58
#define DJDB_IDX_WORDS      14        /* int32s from the index, for reading */
/* Enough for any table name the app uses; the longest is 22 characters. */
#define DJDB_NAME_MAX       64
/* A larger column count means the pointer is not a table; do not walk it. */
#define DJDB_COLS_MAX       128
/* djdbSongPlaylist: PLAYLISTID, CONTENTID, TRACKNO are columns 0, 1, 2 and
 * on-disk fields 2, 1, 0 (see the walk below). Lower-camel, as the djdb calls
 * take it. */
#define DJDB_PLAYLIST_TABLE "djdbSongPlaylist"
#define DJDB_PLAYLIST_INDEX "idxSongPlaylist"
#define DJDB_COL_CONTENTID  1
#define DJDB_COL_TRACKNO    2
/* ---- walking a playlist, read-only ----------------------------------------
 *
 * EP122_DJDB_QUERY finds the rows an index key matches and for which a row
 * filter returns true. idxSongPlaylist keys on PLAYLISTID alone, so the
 * key selects a whole playlist and only the filter narrows it to one entry.
 *
 * This walk selects nothing: the filter logs each row's columns and returns
 * false.
 *
 * EP122_DJDB_ROW_COL(row, n) takes a column index: djdbSongPlaylist's column
 * order (PLAYLISTID/CONTENTID/TRACKNO = 0/1/2) is the reverse of its on-disk
 * field order (2/1/0), and n follows the column order.
 *
 * Threading: [a library server thread]; the query needs a context like every
 * other djdb call. */
/* The debug walk reads djdbContent, where column 0 is the ID and column 8 the
 * BPM. A playlist whose CONTENTID equals TRACKNO in every row cannot show the
 * column order. */
#define DJDB_WALK_COLS  4
/* ---- the reorder ----------------------------------------------------------
 *
 * An entry's position is `DJDBSONGPLAYLIST.TRACKNO`, addressed by the playlist
 * key plus a row filter on CONTENTID. Otherwise it is the same call, converter
 * and transaction as the tempo write, with the same durability: pages are marked
 * dirty and written to the media synchronously, so a pulled stick keeps the
 * change.
 *
 * A move renumbers a range: taking position 6 to 2 shifts 2..5 by one. Only
 * that span is rewritten.
 *
 * Threading: [a library server thread]; it needs the per-thread context. */
#define DJDB_PL_MAX  1024
/* Playlist scan: the medium's only playlist id, or 0 if it has more than one.
 * Last fallback for mod_djdb_playlist_now before the list cache is seen.
 *
 * Ids are dense from 1 on this format, so the scan stops after a run of
 * DJDB_PLAYLIST_SCAN_GAP empty ids. [a library server thread] */
#define DJDB_PLAYLIST_SCAN_MAX  256
#define DJDB_PLAYLIST_SCAN_GAP  16
/* ---- telling the deck its cached copy of that list is stale ----------------
 *
 * The deck fills a list cache when the media is announced and browses from it,
 * sorting in memory, so a reorder written to the database does not show until
 * the cached rows are dropped with
 * music_library::ListCacheCollector::removePlaylistTrackListCache(id).
 *
 * The collector is a member of the InformationUpdater, which we cannot reach,
 * so the call is hooked to capture it. The deck invalidates every playlist's
 * rows when it builds the library for a mounted medium, before any reorder can
 * be requested.
 *
 * Called from the drain on the library thread, the same thread the deck uses
 * for this call. */
/* The browse category the deck passes when it drops a playlist's hierarchy
 * cache, the same value its delete path passes. */
#define DJDB_CATEGORY_PLAYLIST 5

typedef int (*djdb_query_fn)(const char *table, const char *index,
                            void *rowfn, void *rowarg, const char *op,
                            int nkeys, void **keyvals);

struct djdb_entry {
    int32_t content;
    int32_t no;
};

struct djdb_plist {
    struct djdb_entry e[DJDB_PL_MAX];
    int n;
};

/* The deck's per-thread database handle, or NULL outside one of its own
 * operations. */
void *djdb_ctx(void);

/* Run `index` over `table` for `key` and log the four columns named. */
void djdb_walk(const char *what, const char *table, const char *index,
               uint32_t key, int c0, int c1, int c2, int c3);

/* Collect a playlist's rows into a struct djdb_plist. */
int djdb_collect_row(void *ctx, uintptr_t row);

/* Report both halves of the context slot, naming the function that makes one. */
void djdb_report_slot(void);

/* Dump the registry once per (call site, table), when the debug log is on. */
void djdb_try_dump(const char *where, const char *table);

/* Walk the playlist tables once, when the debug log is on. */
void djdb_try_walk(void);

/* Latch the playlist a hooked call names. */
void djdb_note_playlist(const char *table, int nkeys, void **keyvals);

/* Note a table name seen on a hooked call, for the dump. */
void djdb_note_table(const char *name);

/* Read one playlist's rows into `pl`. 0 on success. */
int djdb_read_playlist(uint32_t pid, struct djdb_plist *pl);

/* Which playlist the browser has open, from the deck's own list caches. */
uint32_t djdb_playlist_from_caches(void);

#endif /* EP122_MOD_DJDB_INTERNAL_H */
