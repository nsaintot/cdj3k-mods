// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * stemd_client.h - shared plumbing for the stem sidecar.
 *
 * Three translation units, one concern each:
 *
 *   main.c        the unix socket, the accept loop, signals
 *   session.c     the per-connection state machine over stem_proto frames
 *   discovery.c   finding a stemd server (mDNS or a configured address) and
 *                 probing /v1/health
 *   http.c        an HTTP/1.1 client just large enough for the five calls the
 *                 stemd API needs, with streamed request and response bodies
 *
 * Nothing here is a general-purpose HTTP library and it should not become one.
 * The five calls are known, the peer is on the LAN, and the bodies are either
 * tiny JSON or a hundred megabytes of PCM that must never be buffered whole.
 */
#ifndef STEMD_CLIENT_H
#define STEMD_CLIENT_H

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

#include "loglevel.h"
#include "stem_proto.h"

/* Where stems are written for the shim to adopt. tmpfs, so these are RAM and
 * the shim unlinks each one as soon as it has a descriptor. */
#define STEM_SPOOL_DIR "/dev/shm"

/* How the server was found, which is also what the UI's warn icon reflects. */
struct stem_server {
    char host[128];
    int  port;
    int  reachable;      /* a /v1/health round trip succeeded */
    int  compatible;     /* 44.1 kHz, 2 ch, and the stems we can play */
    /* How this server identifies what it produces. Scopes the deck's on-media
     * cache, so a change of model or preset leaves the old cache beside the new
     * one rather than mixing them. See health_probe for how it is derived. */
    char sep_id[STEM_SEP_ID_LEN];
};

/* ---- logging ----
 *
 * FIVE LEVELS, SHIPPING AT ERROR, read once from STEMD_LOGLEVEL -- a name or a
 * digit, the same grammar the shim reads from EP122_MOD_LOGLEVEL. Separate
 * variables on purpose: this is its own systemd unit and turning the sidecar up
 * while the deck stays quiet is the common case when a separation misbehaves.
 *
 * The level matters more here than it looks. The deck re-HELLOs every 30 s to
 * refresh its status line, and answering each one used to print two lines
 * whether or not anything had changed -- about 2900 lines a day on a deck with
 * no stem server on the network, which is most of them. Steady state is DEBUG;
 * what gets reported at WARN is the TRANSITION, so a server that disappears
 * mid-set is one line rather than one line every half minute. */
extern int g_stemd_log;

#define SLOG_AT(lvl) (g_stemd_log >= (lvl))

#define SLOG_(lvl, tag, ...) do { if (SLOG_AT(lvl)) { \
    fprintf(stderr, "stemd_client: " tag __VA_ARGS__); fflush(stderr); } } while (0)

#define SERR(...)   SLOG_(LOG_ERROR, "ERROR: ", __VA_ARGS__)
#define SWARN(...)  SLOG_(LOG_WARN,  "WARN: ",  __VA_ARGS__)
#define SINFO(...)  SLOG_(LOG_INFO,  "",        __VA_ARGS__)
#define SDBG(...)   SLOG_(LOG_DEBUG, "",        __VA_ARGS__)

/* ---- main.c ---- */

/* Non-zero once SIGINT/SIGTERM has been seen. Every blocking read in the
 * session loop consults this on EINTR, so a shutdown is not swallowed by a
 * retry. */
int session_should_stop(void);

/* ---- session.c ---- */

/* Own one shim connection until it closes. Returns when the peer goes away. */
void session_run(int fd);

/* ---- discovery.c ---- */

/* Resolve a server: `manual` when non-empty is used verbatim, otherwise mDNS
 * `_stemd._tcp` via the avahi-daemon already running on the deck. Fills `out`
 * and returns 0 when a reachable, compatible server was found. */
int discovery_find(const char *manual, struct stem_server *out);

/* Re-probe a server already in `out`, without asking mDNS again.
 *
 * The deck re-HELLOs every 30 s to refresh the status line, and answering that
 * with a full browse was wrong twice over: it forks avahi once or twice per
 * refresh, and it puts the answer at the mercy of what happens to be in avahi's
 * cache at that instant. A quarter of them came back empty on a live deck while
 * the server never moved, and the deck showed offline each time.
 *
 * Where the server is changes far more rarely than whether it is up, so ask the
 * cheap question first and only rediscover when it says no. */
int discovery_recheck(struct stem_server *out);

/* ---- http.c ---- */

/* A streamed request body: `pull` is called repeatedly until it returns 0.
 * That is what lets a POST carry a whole track without holding it. */
typedef size_t (*http_body_pull_fn)(void *buf, size_t cap, void *user);

/* A streamed response body: `push` receives each chunk as it arrives. */
typedef int (*http_body_push_fn)(const void *buf, size_t len, void *user);

/* The response body's size, called once before the first push. HTTP_BODY_LEN_UNKNOWN
 * when the headers do not say (chunked, or read to EOF). */
#define HTTP_BODY_LEN_UNKNOWN ((uint64_t)-1)
typedef void (*http_body_begin_fn)(uint64_t len, void *user);

struct http_request {
    const char *method;
    const char *path;             /* including any query string */
    const char *content_type;     /* NULL for no body */
    uint64_t    content_length;   /* exact, so no chunked encoding is needed */
    http_body_pull_fn pull;
    void       *pull_user;
    http_body_begin_fn begin;     /* NULL when the size is of no interest */
    http_body_push_fn push;       /* NULL to discard the response body */
    void       *push_user;        /* handed to both begin and push */
};

/* Perform one request. Returns the HTTP status, or -1 on a transport failure. */
int http_perform(const struct stem_server *srv, const struct http_request *req);

#endif /* STEMD_CLIENT_H */
