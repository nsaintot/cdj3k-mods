// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * mods/db/db.c - borrowed SQL connection; see db.h for the rules.
 *
 * ---- how the sqlite3 functions are reached --------------------------------
 *
 * Weak symbols, not ep122_sym. libsqlcipher.so.0 is a NEEDED of EP122, so its
 * symbols are in the global scope before any mod runs, and the dynamic linker
 * resolves them by name. An unresolved weak symbol is NULL, so a build without
 * the library degrades to db_ready() == 0 instead of failing to load.
 *
 * This also reaches functions EP122 does not import: the deck uses twenty
 * sqlite3 entry points, and the guards below need sqlite3_get_autocommit and
 * sqlite3_threadsafe, which are not among them.
 *
 * ---- how a handle is reached ----------------------------------------------
 *
 * By hooking the deck's SqliteUpdateTransaction::exec, whose first argument
 * holds the live sqlite3*. Nothing is opened: the files are encrypted and the
 * deck has already keyed them, so no key touches this code.
 *
 * Threading: the hook runs on the deck's database thread and only stores the
 * pointer. Identification runs later on a caller's thread, because a PRAGMA
 * inside the app's exec would re-enter the connection mid-statement.
 */
#include "db/db.h"

#include "core/mod_core.h"
#include "core/ep122_syms.h"
#include "kit/mod.h"

/* ---- the library, weakly ------------------------------------------------- */

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;

#define DB_WEAK __attribute__((weak))

extern DB_WEAK int         sqlite3_prepare_v2(sqlite3 *, const char *, int,
                                              sqlite3_stmt **, const char **);
extern DB_WEAK int         sqlite3_step(sqlite3_stmt *);
extern DB_WEAK int         sqlite3_finalize(sqlite3_stmt *);
extern DB_WEAK int         sqlite3_reset(sqlite3_stmt *);
extern DB_WEAK const char *sqlite3_errmsg(sqlite3 *);
extern DB_WEAK int         sqlite3_bind_int64(sqlite3_stmt *, int, int64_t);
extern DB_WEAK int         sqlite3_bind_text(sqlite3_stmt *, int, const char *,
                                             int, void (*)(void *));
extern DB_WEAK int         sqlite3_bind_null(sqlite3_stmt *, int);
extern DB_WEAK int         sqlite3_column_count(sqlite3_stmt *);
extern DB_WEAK const unsigned char *sqlite3_column_text(sqlite3_stmt *, int);
extern DB_WEAK int64_t     sqlite3_column_int64(sqlite3_stmt *, int);
extern DB_WEAK int         sqlite3_get_autocommit(sqlite3 *);
extern DB_WEAK int         sqlite3_threadsafe(void);

#define SQLITE_OK           0
#define SQLITE_ROW          100
#define SQLITE_DONE         101
/* SQLITE_TRANSIENT: SQLite copies the text before returning. Defined as in
 * sqlite3.h, a cast of -1 to the destructor pointer. */
#define SQLITE_TRANSIENT    ((void (*)(void *))-1)

/* ---- the borrowed handle -------------------------------------------------- */

/* [db thread] writes as the deck runs a statement, everyone reads. */
static sqlite3 *db_g_handle;

/* Resolved once, on a caller's thread, from PRAGMA database_list. */
static char db_g_path[512];
static int  db_g_named;

/* 0 unknown, 1 usable, -1 refused. Decided once. */
static int  db_g_usable;

static uintptr_t db_g_tramp;

/* Every sqlite3 entry point this file calls, checked together so a partial
 * library fails install with a log line. */
static int db_syms_ok(void)
{
    return sqlite3_prepare_v2 && sqlite3_step && sqlite3_finalize &&
           sqlite3_reset && sqlite3_errmsg && sqlite3_bind_int64 &&
           sqlite3_bind_text && sqlite3_bind_null && sqlite3_column_count &&
           sqlite3_column_text && sqlite3_column_int64 &&
           sqlite3_get_autocommit && sqlite3_threadsafe;
}

/* The deck's SqliteUpdateTransaction::exec(txn, query). `*txn` is the sqlite3*.
 *
 * Observation only: the stock call gets its arguments unchanged and its result
 * is returned as is. */
typedef int64_t (*db_exec_fn)(void *txn, const void *query);

static int64_t db_wrap_exec(void *txn, const void *query)
{
    uintptr_t h = 0;

    if (txn && !__atomic_load_n(&db_g_handle, __ATOMIC_ACQUIRE) &&
        mod_safe_read((uintptr_t)txn, &h, sizeof(h)) == 0 && h) {
        __atomic_store_n(&db_g_handle, (sqlite3 *)h, __ATOMIC_RELEASE);
        /* Only the pointer: identifying it runs a PRAGMA, which here would
         * re-enter the connection mid-statement. mod_db_poll does it. */
        MDBG("db: borrowed a connection (%p)\n", (void *)h);
    }

    return ((db_exec_fn)db_g_tramp)(txn, query);
}

/* One statement, prepared and bound. NULL if it will not compile, with a log
 * line so a typo in a mod's SQL is visible. */
static sqlite3_stmt *db_prepare(sqlite3 *h, const char *sql,
                                const struct db_bind *binds, int nbind)
{
    sqlite3_stmt *st = NULL;
    int i;

    if (sqlite3_prepare_v2(h, sql, -1, &st, NULL) != SQLITE_OK || !st) {
        MDBG("db: will not compile: %s -- %s\n", sqlite3_errmsg(h), sql);
        if (st)
            sqlite3_finalize(st);
        return NULL;
    }
    for (i = 0; i < nbind; i++) {
        int rc;

        /* Parameters are 1-based. */
        switch (binds[i].kind) {
        case DB_INT:
            rc = sqlite3_bind_int64(st, i + 1, binds[i].num);
            break;
        case DB_TEXT:
            rc = binds[i].text
               ? sqlite3_bind_text(st, i + 1, binds[i].text, -1,
                                   SQLITE_TRANSIENT)
               : sqlite3_bind_null(st, i + 1);
            break;
        default:
            rc = sqlite3_bind_null(st, i + 1);
            break;
        }
        if (rc != SQLITE_OK) {
            MDBG("db: bind %d refused: %s\n", i + 1, sqlite3_errmsg(h));
            sqlite3_finalize(st);
            return NULL;
        }
    }
    return st;
}

/* Which database this handle is on. Asked once, on a caller's thread.
 *
 * `PRAGMA database_list` gives (seq, name, file); the "main" entry's file is the
 * answer. It reads connection state only, no table. */
static void db_name_once(sqlite3 *h)
{
    sqlite3_stmt *st;

    if (db_g_named)
        return;
    db_g_named = 1;

    st = db_prepare(h, "PRAGMA database_list", NULL, 0);
    if (!st)
        return;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(st, 1);
        const unsigned char *file = sqlite3_column_text(st, 2);

        if (name && file && strcmp((const char *)name, "main") == 0) {
            snprintf(db_g_path, sizeof(db_g_path), "%s", (const char *)file);
            break;
        }
    }
    sqlite3_finalize(st);
    MDBG("db: the borrowed handle is on \"%s\"\n",
         db_g_path[0] ? db_g_path : "(no file -- in memory?)");
}

/* The connection, or NULL. Its preconditions are checked once. */
static sqlite3 *db_get(void)
{
    sqlite3 *h = __atomic_load_n(&db_g_handle, __ATOMIC_ACQUIRE);

    if (!h)
        return NULL;
    if (__atomic_load_n(&db_g_usable, __ATOMIC_ACQUIRE) < 0)
        return NULL;

    if (!__atomic_load_n(&db_g_usable, __ATOMIC_ACQUIRE)) {
        /* Sharing the app's connection is only safe if SQLite takes a mutex
         * per call. Without that, two threads on one connection corrupt it, so
         * the provider turns off. */
        if (sqlite3_threadsafe() == 0) {
            MDBG("db: sqlite is not serialised -> the connection is not"
                 " shareable, provider off\n");
            __atomic_store_n(&db_g_usable, -1, __ATOMIC_RELEASE);
            return NULL;
        }
        db_name_once(h);
        __atomic_store_n(&db_g_usable, 1, __ATOMIC_RELEASE);
    }
    return h;
}

int db_ready(void)
{
    return db_get() != NULL;
}

const char *db_path(void)
{
    if (!db_get() || !db_g_path[0])
        return NULL;
    return db_g_path;
}

int db_select(const char *sql, const struct db_bind *binds, int nbind,
              db_row_fn fn, void *user)
{
    const char *text[DB_MAX_COLS];
    int64_t     num[DB_MAX_COLS];
    struct db_row row = { 0, text, num };
    sqlite3 *h = db_get();
    sqlite3_stmt *st;
    int rows = 0, ncol;

    if (!h || !sql || !fn)
        return -1;
    st = db_prepare(h, sql, binds, nbind);
    if (!st)
        return -1;

    ncol = sqlite3_column_count(st);
    if (ncol > DB_MAX_COLS) {
        MDBG("db: %d columns is past the %d a row can carry -> refused: %s\n",
             ncol, DB_MAX_COLS, sql);
        sqlite3_finalize(st);
        return -1;
    }
    row.ncol = ncol;

    while (sqlite3_step(st) == SQLITE_ROW) {
        int i;

        for (i = 0; i < ncol; i++) {
            text[i] = (const char *)sqlite3_column_text(st, i);
            num[i]  = sqlite3_column_int64(st, i);
        }
        rows++;
        if (fn(&row, user))
            break;
    }
    sqlite3_finalize(st);
    return rows;
}

int db_run(const char *sql, const struct db_bind *binds, int nbind)
{
    sqlite3 *h = db_get();
    sqlite3_stmt *st;
    int rc;

    if (!h || !sql)
        return -1;

    /* Autocommit off means the app has a BEGIN open on this connection, and a
     * statement issued now would be committed or rolled back with it. The
     * deck's transactions are short; the caller retries. */
    if (!sqlite3_get_autocommit(h)) {
        MDBG("db: the deck is mid-transaction -> refused: %s\n", sql);
        return -1;
    }

    st = db_prepare(h, sql, binds, nbind);
    if (!st)
        return -1;

    rc = sqlite3_step(st);
    if (rc != SQLITE_DONE) {
        /* The statement returned rows or failed; both are caller errors. */
        MDBG("db: step %d (%s): %s\n", rc, sqlite3_errmsg(h), sql);
        sqlite3_finalize(st);
        return -1;
    }
    sqlite3_finalize(st);
    return 0;
}

/* Identify the connection as soon as one is borrowed, without waiting for a
 * caller. Which database it is decides whether a write travels with the stick,
 * and the log line shows the hook works even with no client. Costs one atomic
 * load per idle tick once it has run.
 *
 * [worker] */
void mod_db_poll(void)
{
    if (__atomic_load_n(&db_g_handle, __ATOMIC_ACQUIRE) && !db_g_named)
        (void)db_get();
}

/* ---- install -------------------------------------------------------------- */

static int db_install(void)
{
    if (!db_syms_ok()) {
        MDBG("db: libsqlcipher is not in this process -> no database access\n");
        return -1;
    }
    if (mod_patch_fn("sqliteUpdateExec", ep122_sym(EP122_SQLITE_UPDATE_EXEC),
                     (void *)db_wrap_exec, &db_g_tramp) != 0) {
        MDBG("db: no update-transaction hook -> no handle to borrow\n");
        return -1;
    }
    return 0;
}

KIT_MOD(k_mod_db,
        .name = "db", .prio = 3, .install = db_install,
        .what = "database: borrow the media's Device Library Plus connection");
