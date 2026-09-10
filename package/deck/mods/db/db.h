// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * mods/db/db.h - the media's Device Library Plus database, for mods to read.
 *
 * THE DECK HAS NO LIBRARY OF ITS OWN. Everything here used to be described as
 * one and that was wrong, in a way that cost real work: there is no database on
 * the deck, only on the media. A rekordbox stick carries TWO, side by side in
 * PIONEER/rekordbox/ --
 *
 *   export.pdb + exportExt.pdb   DEVICE LIBRARY, the DeviceSQL format, read and
 *                                written by the Dsql* family through djdb.
 *   exportLibrary.db             DEVICE LIBRARY PLUS, SQLCipher, read by the
 *                                Sqlite* family -- this file.
 *
 * -- and the CDJ-3000 uses the FIRST. Which is why db_ready() never leaves 0 on
 * a stick-only deck: not a bug, not a handle we failed to catch, but this
 * firmware simply not speaking Device Library Plus. Checked: `find /` on the
 * deck turns up no exportLibrary.db at all, and on a stick where the deck had
 * just flushed, the two .pdb files were stamped minutes old while the .db was
 * weeks old and untouched.
 *
 * So this file is a reader for a format a future firmware may start using. It
 * is NOT a way around the djdb wall in the sibling -- see djdb.c.
 *
 * WHY THIS EXISTS RATHER THAN EACH MOD DOING IT. Everything hard here is hard
 * once: reaching a handle without the SQLCipher key, knowing WHICH database that
 * handle is, sharing a connection with the app's own threads, and not being the
 * reason a DJ's library is corrupt. A mod that wants to reorder a playlist or
 * change a BPM should be writing one statement, not solving that again.
 *
 * ---- THE RULE, for this file and for the djdb sibling ----------------------
 *
 * EVERY ACCESS GOES THROUGH AN INSTANTIATED MECHANISM OF THE DECK'S. Never a
 * file path, never a format we parse ourselves, never a connection of our own.
 * That is not only about the SQLCipher key -- the deck holds caches, indexes and
 * open handles over these files, so a write behind its back is corrupted by the
 * next thing it flushes, or silently overwritten, or both. Borrowing the live
 * object is what makes a write mean the same thing the deck's own write means.
 *
 * ---- the connection is the app's, and that is the whole design -------------
 *
 * EP122 links libsqlcipher.so.0, and the library files are encrypted. We do not
 * open one and we do not hold a key: the deck opens its databases at startup and
 * keys them itself, and this borrows a handle that is already open and already
 * keyed. So the key never appears in shim code, in shim memory, or in a config
 * file -- there is nothing here to leak and nothing to circumvent.
 *
 * The handle is taken from a live call: the deck's own
 * SqliteUpdateTransaction::exec holds it in its first argument, and every
 * library write goes through that. See db.c.
 *
 * ---- what the sharing costs, and the rules that follow ---------------------
 *
 * The app's threads use this same connection. SQLite serialises calls on one
 * connection, so a call is safe; a TRANSACTION is not, because BEGIN..COMMIT
 * from us would enclose whatever the app's thread issued in between. So:
 *
 *   ONE STATEMENT AT A TIME. No BEGIN, no COMMIT, no multi-statement writes.
 *   A single statement is atomic on its own, which is enough for anything
 *   expressible as one -- a whole playlist reorder is one UPDATE with a CASE.
 *
 *   NOT FROM THE AUDIO THREAD. This allocates, takes SQLite's mutex and waits
 *   on a file. [worker] and [message] are fine; [deck] is fine for a small
 *   statement on a button press.
 *
 *   BINDS, NOT CONCATENATION. Every value goes through db_bind. A track title
 *   with an apostrophe in it is not an edge case, it is Tuesday.
 *
 * ---- WHICH LIBRARY THIS REACHES, WHICH IT DOES NOT -------------------------
 *
 * Three back ends, only two of them SQL, and all three describe MEDIA:
 *
 *   Sqlite*       DEVICE LIBRARY PLUS on the media -- exportLibrary.db, a
 *                 SQLCipher file driven through sqlite3_*. The CDJ-3000 does
 *                 not use it.                          <- this file
 *   CloudSqlite*  same shape, for cloud sources.       <- this file
 *   Dsql*         REKORDBOX MEDIA -- a USB stick or SD card. NOT SQL at all:
 *                 Source/Domain/MusicLibrary/Server/DataBase/DeviceSQL/ calls
 *                 a `djdb` API by table name (sub_1c66f80("djdbSongHistory",
 *                 ...)) and the file it writes is PIONEER/rekordbox/export.pdb.
 *
 * What is actually established, and what is not:
 *
 *   ESTABLISHED  db_ready() stays 0 on a deck whose only library is a stick --
 *                no SQL handle is ever borrowed, through browsing, loading and
 *                cueing. So the SQL route does not reach rekordbox media.
 *   ESTABLISHED  export.pdb and exportExt.pdb are rewritten around media mount
 *                and library open.
 *   REFUTED      that a hot cue writes export.pdb. An earlier reading said so;
 *                it was not controlled -- an EP122 restart and a media
 *                re-attach fell inside the same window. Repeated properly
 *                (note mtime, set two cues, wait, sync) the file does not
 *                change, and the djdb entry points are not reached either.
 *   OPEN         where a hot cue on rekordbox media actually persists, and
 *                when. Most likely at eject or on some flush; not looked at.
 *
 * So a mod that wants to change something ON THE DJ'S STICK -- a playlist order
 * in djdbSongPlaylist, a track's BPM -- does NOT want this file yet. It wants
 * the djdb sibling of it, which is the next thing to build and which obeys the
 * same rule above: sub_1c97b50() hands over the live djdb context, and the
 * operations take that context rather than a file. It is readier than this one
 * in one respect -- djdb has real transactions, so a multi-row change can be
 * atomic, where a shared SQL connection cannot have one at all.
 *
 * ---- absence is normal ------------------------------------------------------
 *
 * db_ready() is 0 until a handle has been seen, and stays 0 on any deck whose
 * only library is rekordbox media, or where libsqlcipher is not loaded. Callers
 * check it; nothing here is a hard dependency of anything.
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

/* One row as a callback sees it. The pointers are SQLite's own and are valid
 * only for the duration of the call -- copy anything that outlives it. */
struct db_row {
    int             ncol;
    const char    **text;       /* NULL for a NULL column */
    const int64_t  *num;        /* the same columns read as integers */
};

/* Return non-zero to stop walking the result. */
typedef int (*db_row_fn)(const struct db_row *row, void *user);

/* Is there a usable connection? 0 means every call below is a no-op, which is
 * the state on any deck where no library write has happened yet. */
int db_ready(void);

/* The file the borrowed handle is open on, as the database itself reports it
 * (PRAGMA database_list), or NULL. This is what says whether a write lands on
 * the USB export library or on the deck's own -- ASK BEFORE WRITING. */
const char *db_path(void);

/* Run a SELECT. `fn` is called per row until it returns non-zero or the rows run
 * out. Returns the number of rows visited, or -1 if the statement would not run.
 *
 * DB_MAX_COLS caps the width; a wider result is refused rather than truncated,
 * because a caller reading row->text[7] of a row that silently stopped at 6 is
 * exactly the bug this exists to prevent. */
#define DB_MAX_COLS  32
int db_select(const char *sql, const struct db_bind *binds, int nbind,
              db_row_fn fn, void *user);

/* Run one statement that returns no rows. Returns 0 on success, -1 otherwise.
 *
 * REFUSED while the app is inside its own transaction, because a write that
 * joins someone else's transaction is committed or rolled back by a decision
 * that is not ours. Try again later -- the deck's transactions are short. */
int db_run(const char *sql, const struct db_bind *binds, int nbind);

/* Identify a freshly borrowed connection. Called from the worker's idle branch;
 * nothing else needs it. [worker] */
void mod_db_poll(void);

/* ---- djdb.c: the rekordbox-media half ------------------------------------
 *
 * Reading only, for now, and by pointer-walking the deck's own table registry
 * rather than by calling into djdb -- so it cannot have a side effect on a DJ's
 * library while the questions a writer needs answered are still open. Says what
 * tables the mounted media has and what their columns are. [worker] */
void mod_djdb_poll(void);

/* The one write: the tempo the BROWSER shows, which is a library column and not
 * the beat grid -- so a rescaled grid does not move it and this does. Through
 * djdb's own column update, the same call the deck makes for a track's rating,
 * pointed at column 8. `content_id` is trackid::TrackID's third word.
 *
 * 0 when djdb took it. A refusal is usually the library not being open for
 * writing at that moment, which is ordinary rather than an error. [message] */
int mod_djdb_set_bpm(uint32_t content_id, int bpm_x100);

/* Move one entry of a playlist to another position, renumbering the range
 * between them. Positions are 1-based TRACKNOs as the browser shows them.
 *
 * `expect_rows` is how many entries the CALLER believes that playlist has; 0 to
 * skip the check. A second opinion on the id, held independently of how it was
 * arrived at: a caller that counted rows on screen has the write refuse when the
 * playlist it is about to rewrite is a different length. It cannot separate two
 * playlists of the same length, so it is a backstop and not the gate.
 *
 * 0 when every affected row was written. Needs a djdb context, so it must run on
 * a library server thread -- see mod_djdb_note. [db] */
int mod_djdb_move_track(uint32_t playlist_id, int32_t from_no, int32_t to_no,
                        int32_t expect_rows);

/* The same move, asked for from a thread that has no djdb context -- the UI's.
 * Queued and run on the next library message, exactly as a held tempo is.
 * 0 when it was queued, which is NOT "it was written". [message] */
int mod_djdb_move_track_async(uint32_t playlist_id, int32_t from_no,
                              int32_t to_no, int32_t expect_rows);

/* Which playlist the list on screen belongs to, or 0.
 *
 * Neither end of the UI can answer this: the browse view carries a hierarchy
 * rather than a table key, and the deck does not query djdb while browsing at
 * all -- it warms a list cache when the media is announced and serves every list
 * out of that. The CACHE can, though. Every cached list keeps the condition it
 * was asked for, a track list's condition carries the hierarchy that named it,
 * and the deck reads the playlist id back out of exactly that when it drops a
 * playlist's rows. So this walks the collector and reports what the deck itself
 * would say.
 *
 * Which of several caches is the one being shown is answered the same way the
 * deck answers it: by the use count. It purges caches nothing holds but itself,
 * so a cache held by something else is a cache someone is looking at. MEASURED
 * across playlists -- the held one follows the screen, and the newest one does
 * not, so the serial only ever breaks a tie.
 *
 * Falls back, before a collector is seen, to the last playlist a query named
 * and then to the medium's only playlist. Both outlive the list they were true
 * for: this names a WRITE's target and does not say what is on screen. [any] */
uint32_t mod_djdb_playlist_now(void);

/* The PLAYLIST-ONLY GATE: the playlist the newest held track-list cache was
 * asked for, 0 when that cache is another kind of list or none is held. One
 * poll behind the screen, and the deck keeps the previous list's cache held
 * for ~130 ms after replacing it, so the caller re-asks on a list change. None
 * of the fallbacks above. Quiet: polled. [any] */
uint32_t mod_djdb_playlist_shown(void);

/* "The deck is inside a djdb operation right now, called from `where`" -- so the
 * table registry can be read at a moment its context is certainly live, which is
 * never true from an idle thread. Called by pager.c from the flush. [db] */
void mod_djdb_note(const char *where);


/* "Forget the rows you cached for that playlist." The deck's own call, on the
 * deck's own collector -- which is learned from a call the deck makes whenever
 * it caches a list, so it is known from the first list opened.
 *
 * Needed BEFORE asking for the list as well as after writing: a fetch that hits
 * the cache is answered in the UI thread and never troubles the library, so
 * without this the request that was supposed to carry the write in never leaves
 * the building. [any] */
void mod_djdb_drop_list_cache(uint32_t playlist_id);

/* pager.c is the layer beneath these tables -- the fixed-size page store the
 * rows actually live in. It has no entry point of its own: it hooks the two
 * page ops and reports what it sees. */

/* ---- pdbwatch.c: when does a library file actually get written? -----------
 *
 * Called from the shim's own open/write/close interposers, which is the one
 * place that can answer it without guessing. Observation only. `ra` is the
 * caller's return address, which is what turns "something wrote" into "this
 * wrote". A path that is not a library file costs one strstr and returns. */
void db_watch_open(const char *path, int fd, int flags);
void db_watch_write(int fd, size_t count, uintptr_t ra);
void db_watch_close(int fd);


#ifdef __cplusplus
}
#endif

#endif /* EP122_MODS_DB_H */
