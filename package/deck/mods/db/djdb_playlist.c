// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * mods/db/djdb_playlist.c - which playlist the browser has open, from the deck's own caches.
 */
#include "db/djdb_internal.h"

static void djdb_count_playlists(void);

/* Set while this file runs its own queries, which go through the same hook as
 * the playlist latch; otherwise the latch would record our own query. */
static int djdb_g_ours;
/* The medium's only playlist, when it has exactly one -- see
 * djdb_count_playlists and mod_djdb_playlist_now. */
static uint32_t djdb_g_sole_playlist;
static int      djdb_g_playlists;
/* ---- the playlist the deck is showing --------------------------------------
 *
 * A reorder needs a playlist id; the browse view only knows a hierarchy. The id
 * comes from the list cache the deck serves the list from (see
 * djdb_playlist_from_caches). This latch holds the last id a djdbSongPlaylist
 * cursor named (idxSongPlaylist takes it as its only key); it is never cleared
 * and is only a fallback before a cache has been seen.
 *
 * The move is queued, never tried inline: it comes from the message thread,
 * which has no djdb context. It runs on the next library message, like a held
 * tempo. */
static uint32_t djdb_g_seen_playlist;

/* Once per run, from the first table-level hook that has a context. Not from
 * the page writer: a query nested inside a page flush re-enters the pool below
 * it. The guard is needed because the query itself is hooked.
 *
 * Keep this read-only. Do not add a test write: it would reorder the DJ's
 * playlist on every boot. */
void djdb_try_walk(void)
{
    static int done;

    if (done || !djdb_ctx())
        return;
    done = 1;
    /* The column convention: ID and BPM of content id 7. */
    djdb_walk("content", DJDB_CONTENT_TABLE, DJDB_CONTENT_INDEX, 7,
              0, DJDB_COL_BPM, 15, -1);
    /* And the playlist, for its row count and its constant column. Flagged as
     * ours so the playlist latch ignores it. */
    djdb_g_ours = 1;
    djdb_walk("songplaylist", DJDB_PLAYLIST_TABLE, DJDB_PLAYLIST_INDEX, 1,
              0, 1, 2, -1);
    djdb_count_playlists();
    djdb_g_ours = 0;
}

/* The playlist's entries, in whatever order the cursor yields them. */
int djdb_read_playlist(uint32_t pid, struct djdb_plist *pl)
{
    uintptr_t fn = ep122_sym(EP122_DJDB_QUERY);
    uint32_t key = pid;
    void *keyvals[1];

    pl->n = 0;
    if (!fn)
        return -1;
    keyvals[0] = &key;
    (void)((djdb_query_fn)fn)(DJDB_PLAYLIST_TABLE, DJDB_PLAYLIST_INDEX,
                              (void *)djdb_collect_row, pl, "=", 1, keyvals);
    return pl->n;
}

static void djdb_count_playlists(void)
{
    static struct djdb_plist pl;
    uint32_t id;
    int gap = 0;

    djdb_g_playlists = 0;
    djdb_g_sole_playlist = 0;
    for (id = 1; id <= DJDB_PLAYLIST_SCAN_MAX && gap < DJDB_PLAYLIST_SCAN_GAP;
         id++) {
        if (djdb_read_playlist(id, &pl) > 0) {
            gap = 0;
            if (++djdb_g_playlists == 1)
                djdb_g_sole_playlist = id;
        } else {
            gap++;
        }
    }
    if (djdb_g_playlists == 1)
        MDBG("djdb: this medium has one playlist, %u -- a reorder cannot be"
             " about any other\n", (unsigned)djdb_g_sole_playlist);
    else
        MDBG("djdb: this medium has %d playlists -- which one a reorder is about"
             " comes off the list cache it is being served from\n",
             djdb_g_playlists);
}

uint32_t mod_djdb_playlist_now(void)
{
    /* The list cache is the only source that can pick among several
     * playlists. Before a collector is seen, fall back to the deck's cursor (it
     * does not query while browsing) and then to the medium's only playlist.
     * 0 when none applies, so the write is refused. */
    uint32_t id = djdb_playlist_from_caches();

    if (id)
        return id;
    if (djdb_g_seen_playlist)
        return djdb_g_seen_playlist;
    return djdb_g_playlists == 1 ? djdb_g_sole_playlist : 0;
}

void djdb_note_playlist(const char *table, int nkeys, void **keyvals)
{
    if (djdb_g_ours)
        return;
    /* Wider than the one name compared, so other table names log in full. */
    char name[24];
    uint32_t pid = 0;

    if (!table)
        return;
    /* The deck's argument, read positionally and untrusted: probed with a
     * fixed length rather than dereferenced. */
    if (mod_safe_read((uintptr_t)table, name, sizeof(name)) != 0)
        return;
    name[sizeof(name) - 1] = '\0';
    djdb_note_table(name);
    if (nkeys != 1 || !keyvals || strcmp(name, DJDB_PLAYLIST_TABLE))
        return;
    if (!keyvals[0] ||
        mod_safe_read((uintptr_t)keyvals[0], &pid, sizeof(pid)) != 0 || !pid)
        return;
    if (pid != djdb_g_seen_playlist)
        MDBG("djdb: the list on screen is playlist %u\n", (unsigned)pid);
    djdb_g_seen_playlist = pid;
}
