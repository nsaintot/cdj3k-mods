// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * stemd_client.h - shared plumbing for the stem sidecar.
 *
 * Translation units:
 *
 *   main.c        the unix socket, the accept loop, signals
 *   session.c     the per-connection state machine over stem_proto frames
 *   discovery.c   finding a stemd server (mDNS or a configured address) and
 *                 probing /v1/health
 *   http.c        an HTTP/1.1 client just large enough for the five calls the
 *                 stemd API needs, with streamed request and response bodies
 *   json.c        flat-object JSON scanning (json.h)
 *
 * http.c is deliberately not a general-purpose HTTP library: the calls are known,
 * the peer is on the LAN, and the bodies are either tiny JSON or a hundred
 * megabytes of PCM that must never be buffered whole.
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

/* Where stems are written for the shim to adopt. tmpfs (RAM); the shim unlinks
 * each file as soon as it has a descriptor. */
#define STEM_SPOOL_DIR "/dev/shm"

/* The server found; reachable/compatible drive the UI's warn icon. */
struct stem_server {
    char host[128];
    int  port;
    int  reachable;      /* a /v1/health round trip succeeded */
    int  compatible;     /* 44.1 kHz, 2 ch, and the stems we can play */
    /* Identifies what this server produces. Scopes the deck's on-media cache,
     * so a change of model or preset keeps old and new cache entries apart.
     * Derived in health_probe. */
    char sep_id[STEM_SEP_ID_LEN];
};

/* ---- logging ----
 *
 * Five levels, default ERROR, read once from STEMD_LOGLEVEL (a name or a digit,
 * same grammar as the shim's EP122_MOD_LOGLEVEL). A separate variable so the
 * sidecar, its own systemd unit, can be made verbose while the deck stays quiet.
 *
 * The deck re-HELLOs every 30 s. Steady-state answers log at DEBUG; only
 * transitions log at WARN, so a server that disappears mid-set is one line. */
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
 * session loop checks this on EINTR, so a retry does not swallow a shutdown. */
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
 * Used for the deck's 30 s re-HELLO. A full browse each time forks avahi once or
 * twice and depends on avahi's cache at that instant; about a quarter came back
 * empty on a live deck with the server unchanged. Rediscover only when the
 * re-probe fails. */
int discovery_recheck(struct stem_server *out);

/* ---- http.c ---- */

/* A streamed request body: `pull` is called repeatedly until it returns 0, so a
 * POST can carry a whole track without buffering it. */
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
