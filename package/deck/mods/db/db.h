// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * mods/db/db.h - the media's Device Library Plus database, for mods to read.
 *
 * The deck has no library of its own; the databases are on the media. A
 * rekordbox stick carries two, side by side in PIONEER/rekordbox/:
 *
 *   export.pdb + exportExt.pdb   DEVICE LIBRARY, the DeviceSQL format, read and
 *                                written by the Dsql* family through djdb.
 *   exportLibrary.db             DEVICE LIBRARY PLUS, SQLCipher, read by the
 *                                Sqlite* family -- this file.
 *
 * The deck uses the first, so db_ready() stays 0 on a stick-only deck: this
 * firmware does not use Device Library Plus and does not write
 * exportLibrary.db.
 *
 * So this file is a reader for a format a future firmware may use. For the
 * rekordbox media library, see djdb.c.
 *
 * It exists so mods do not each have to solve reaching a handle without the
 * SQLCipher key, identifying which database it is, sharing a connection with
 * the app's threads, and not corrupting the library.
 *
 * ---- the rule, for this file and for djdb.c --------------------------------
 *
 * Every access goes through a live object of the deck's: never a file path,
 * never our own parser, never our own connection. The deck holds caches,
 * indexes and open handles over these files, so a write behind its back gets
 * corrupted or overwritten by its next flush.
 *
 * ---- the connection is the app's -------------------------------------------
 *
 * EP122 links libsqlcipher.so.0, and the library files are encrypted. The deck
 * opens and keys its databases at startup; this borrows a handle that is
 * already open and keyed, so the key never appears in shim code, shim memory or
 * a config file.
 *
 * The handle is taken from a live call: the deck's
 * SqliteUpdateTransaction::exec holds it in its first argument, and every
 * library write goes through that. See db.c.
 *
 * ---- rules for sharing the connection --------------------------------------
 *
 * The app's threads use the same connection. SQLite serialises calls on one
 * connection, so a single call is safe, but a BEGIN..COMMIT from us would
 * enclose whatever the app issued in between. So:
 *
 *   One statement at a time. No BEGIN, no COMMIT, no multi-statement writes.
 *   A single statement is atomic; a whole playlist reorder is one UPDATE with
 *   a CASE.
 *
 *   Not from the audio thread. This allocates, takes SQLite's mutex and waits
 *   on a file. [worker] and [message] are fine; [deck] is fine for a small
 *   statement on a button press.
 *
 *   Bind every value through db_bind; never concatenate.
 *
 * ---- which library this reaches --------------------------------------------
 *
 * Three back ends, two of them SQL, all describing media:
 *
 *   Sqlite*       DEVICE LIBRARY PLUS on the media -- exportLibrary.db, a
 *                 SQLCipher file driven through sqlite3_*. The deck does
 *                 not use it.                          <- this file
 *   CloudSqlite*  same shape, for cloud sources.       <- this file
 *   Dsql*         REKORDBOX MEDIA -- a USB stick or SD card. Not SQL:
 *                 Source/Domain/MusicLibrary/Server/DataBase/DeviceSQL/ calls
 *                 a `djdb` API by table name (EP122_DJDB_ROW_INSERT with
 *                 "djdbSongHistory", ...) and the file it writes is
 *                 PIONEER/rekordbox/export.pdb.
 *
 * Facts:
 *
 *   db_ready() stays 0 on a deck whose only library is a stick: no SQL handle
 *   is borrowed through browsing, loading or cueing, so the SQL route does not
 *   reach rekordbox media.
 *
 *   export.pdb and exportExt.pdb are rewritten around media mount and library
 *   open.
 *
 *   Setting a hot cue does not write export.pdb and does not reach the djdb
 *   entry points. Where and when a hot cue on rekordbox media persists is not
 *   known (possibly at eject or on a flush).
 *
 * A mod that changes something on the DJ's stick (a playlist order in
 * djdbSongPlaylist, a track's BPM) uses djdb.c instead, under the same rule:
 * EP122_DJDB_CONTEXT returns the live djdb context and the operations take that
 * context, not a file. djdb has real transactions, so a multi-row change can be
 * atomic; a shared SQL connection cannot have one.
 *
 * ---- absence is normal ------------------------------------------------------
 *
 * db_ready() is 0 until a handle has been seen, and stays 0 on any deck whose
 * only library is rekordbox media or where libsqlcipher is not loaded. Callers
 * check it; nothing depends on it.
 */
#ifndef EP122_MODS_DB_H
#define EP122_MODS_DB_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif


/* A value bound to a `?` in a statement, in the order the `?`s appear. */
enum db_kind {
    DB_NULL = 0,
    DB_INT,                     /* int64, for ids and sequence numbers */
    DB_TEXT                     /* NUL-terminated, copied by SQLite */
};

struct db_bind {
    enum db_kind kind;
    int64_t      num;
    const char  *text;
};

/* One row as a callback sees it. The pointers are SQLite's and valid only
 * during the call; copy anything needed later. */
struct db_row {
    int             ncol;
    const char    **text;       /* NULL for a NULL column */
    const int64_t  *num;        /* the same columns read as integers */
};

/* Return non-zero to stop walking the result. */
typedef int (*db_row_fn)(const struct db_row *row, void *user);

/* Whether there is a usable connection. When 0, every call below is a no-op;
 * that is the state until a library write has happened. */
int db_ready(void);

/* The file the borrowed handle is open on, from PRAGMA database_list, or NULL.
 * Check it before writing to know which library the write lands in. */
const char *db_path(void);

/* Run a SELECT. `fn` is called per row until it returns non-zero or the rows run
 * out. Returns the number of rows visited, or -1 if the statement would not run.
 *
 * DB_MAX_COLS caps the width; a wider result is refused, not truncated. */
#define DB_MAX_COLS  32
int db_select(const char *sql, const struct db_bind *binds, int nbind,
              db_row_fn fn, void *user);

/* Run one statement that returns no rows. Returns 0 on success, -1 otherwise.
 *
 * Refused while the app is inside its own transaction, since the write would
 * be committed or rolled back with it. Retry later; the deck's transactions
 * are short. */
int db_run(const char *sql, const struct db_bind *binds, int nbind);

/* Identify a freshly borrowed connection. Called from the worker's idle branch;
 * nothing else needs it. [worker] */
void mod_db_poll(void);

/* ---- djdb.c: the rekordbox-media half ------------------------------------
 *
 * Logs the mounted media's tables and their columns. Read-only: it walks the
 * deck's table registry instead of calling into djdb, so it has no side effect
 * on the library. [worker] */
void mod_djdb_poll(void);

/* Set the tempo the browser shows. It is a library column, separate from the
 * beat grid, so rescaling the grid does not change it. Uses djdb's column
 * update, the same call the deck makes for a track's rating, on column 8.
 * `content_id` is trackid::TrackID's third word.
 *
 * 0 when djdb accepted it. A refusal usually means the library is not open for
 * writing at that moment, which is normal. [message] */
int mod_djdb_set_bpm(uint32_t content_id, int bpm_x100);

/* Move one entry of a playlist to another position, renumbering the range
 * between them. Positions are 1-based TRACKNOs as the browser shows them.
 *
 * `expect_rows` is how many entries the caller believes the playlist has, or 0
 * to skip the check. The write is refused when the playlist has a different
 * length. It is a backstop on the id: two playlists of the same length pass.
 *
 * 0 when every affected row was written. Needs a djdb context, so it must run on
 * a library server thread -- see mod_djdb_note. [db] */
int mod_djdb_move_track(uint32_t playlist_id, int32_t from_no, int32_t to_no,
                        int32_t expect_rows);

/* The same move from a thread with no djdb context (the UI's). Queued and run
 * on the next library message, like a held tempo. 0 means queued, not
 * written. [message] */
int mod_djdb_move_track_async(uint32_t playlist_id, int32_t from_no,
                              int32_t to_no, int32_t expect_rows);

/* Which playlist the list on screen belongs to, or 0.
 *
 * The browse view carries a hierarchy, not a table key, and the deck does not
 * query djdb while browsing: it fills a list cache when the media is announced
 * and serves every list from it. Each cached list keeps the condition it was
 * requested with, a track list's condition carries the hierarchy that named it,
 * and the deck reads the playlist id from that when it drops a playlist's rows.
 * This walks the collector and reads the id the same way.
 *
 * The cache on screen is chosen by use count, as the deck does: it purges
 * caches only it holds, so one held elsewhere is being viewed. The held one
 * follows the screen and the newest one does not, so the serial only breaks
 * ties.
 *
 * Before a collector is seen, falls back to the last playlist a query named,
 * then to the medium's only playlist. Both can outlive the list they were true
 * for, so this names a write's target, not what is on screen. [any] */
uint32_t mod_djdb_playlist_now(void);

/* The playlist-only gate: the playlist the newest held track-list cache was
 * requested for, or 0 when that cache is another kind of list or none is held.
 * One poll behind the screen, and the deck keeps the previous list's cache held
 * for ~130 ms after replacing it, so the caller re-asks on a list change. No
 * fallbacks. Does not log, since it is polled. [any] */
uint32_t mod_djdb_playlist_shown(void);

/* Signals that the deck is inside a djdb operation, called from `where`, so the
 * table registry can be read while its context is live (never the case from an
 * idle thread). Called by pager.c from the flush. [db] */
void mod_djdb_note(const char *where);


/* Drop the deck's cached rows for a playlist, using the deck's own call on its
 * collector. The collector is captured from a call the deck makes whenever it
 * caches a list, so it is known from the first list opened.
 *
 * Needed before requesting the list as well as after writing: a fetch that hits
 * the cache is answered on the UI thread without reaching the library, so the
 * request that should carry the write would never be sent. [any] */
void mod_djdb_drop_list_cache(uint32_t playlist_id);

/* pager.c is the layer beneath these tables, the fixed-size page store holding
 * the rows. It has no entry point: it hooks the two page ops and logs them. */

/* ---- pdbwatch.c: when does a library file actually get written? -----------
 *
 * Called from the shim's open/write/close interposers. Observation only. `ra`
 * is the caller's return address, identifying the writer. A path that is not
 * a library file costs one strstr. */
void db_watch_open(const char *path, int fd, int flags);
void db_watch_write(int fd, size_t count, uintptr_t ra);
void db_watch_close(int fd);


#ifdef __cplusplus
}
#endif

#endif /* EP122_MODS_DB_H */
