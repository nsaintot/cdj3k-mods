// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * stemd_client - the network half of the STEMS feature.
 *
 * A small daemon between the EP122 shim and a `stemd` server, so that no HTTP
 * client, retry loop, resolver or blocking socket runs in the process that owns
 * the deck's audio thread: if this hangs or crashes, EP122 keeps playing and
 * STEMS shows as unavailable.
 *
 * Decoding stays in the shim, using EP122's own decoder. An independent decode
 * could disagree on encoder delay and the deck pads its output, so the stems
 * would come back misaligned. The shim
 * streams PCM in; this process never opens a track file.
 *
 * Shape:
 *
 *   accept on /run/stemd-client.sock   (one client: there is one EP122)
 *     HELLO      -> version check
 *     JOB_BEGIN  -> open POST /v1/jobs, stream the body as PCM frames arrive
 *     JOB_END    -> finish the request; 200 = cache hit, 202 = queued
 *                   poll GET /v1/jobs/{id}, forwarding PROGRESS
 *                   GET  /v1/jobs/{id}/stems/{name} -> tmpfs WAV -> STEM_READY
 *     CANCEL     -> DELETE /v1/jobs/{id}
 *
 * Discovery and the health probe are in discovery.c, the HTTP/1.1 client in
 * http.c, the state machine in session.c. This file is the socket loop.
 */
#include "stemd_client.h"

static volatile sig_atomic_t g_stop;

/* session.c blocks in read() while a client is connected. With SA_RESTART
 * cleared the signal arrives as EINTR; the read loop checks this to tell a
 * shutdown from a retryable interrupt. */
int session_should_stop(void)
{
    return g_stop != 0;
}

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* sigaction with SA_RESTART cleared.
 *
 * signal() gives BSD semantics (SA_RESTART), so a SIGTERM during accept()
 * restarts the call, `while (!g_stop)` is never re-tested, and systemd has to
 * SIGKILL ("State 'stop-sigterm' timed out. Killing."), leaving any in-flight
 * job's server session unclosed. */
static void install_stop_handler(int sig)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(sig, &sa, NULL);
}

/* Clear the spool at startup.
 *
 * /dev/shm is RAM and survives this process until reboot. Stems are named per
 * part, so the live footprint is two files, but older builds left one file per
 * job.
 *
 * Only names this daemon owns are removed: the two parts, their .part staging
 * files, and the legacy stem-<uuid>-<part>.wav. Startup is the safe moment, as
 * nothing is in flight with the single client. */
static void spool_sweep(void)
{
    /* Matched on the stem name and any extension: the container is the
     * server's choice and has changed before (wav -> flac). */
    static const char *const own[] = { STEM_WIRE_HARMONICS, STEM_WIRE_VOCALS };
    char path[256];
    struct dirent *de;
    int removed = 0;
    DIR *d = opendir(STEM_SPOOL_DIR);

    if (!d)
        return;
    while ((de = readdir(d)) != NULL) {
        size_t n = strlen(de->d_name);
        int mine = 0, i;

        for (i = 0; i < (int)(sizeof(own) / sizeof(own[0])); i++) {
            size_t k = strlen(own[i]);

            if (!strncmp(de->d_name, own[i], k) && de->d_name[k] == '.')
                mine = 1;
        }
        /* Legacy per-job naming: stem-<uuid>-<part>.wav */
        if (!mine && !strncmp(de->d_name, "stem-", 5) && n > 4 &&
            !strcmp(de->d_name + n - 4, ".wav"))
            mine = 1;
        if (!mine)
            continue;

        if ((size_t)snprintf(path, sizeof(path), "%s/%s", STEM_SPOOL_DIR,
                             de->d_name) < sizeof(path) &&
            unlink(path) == 0)
            removed++;
    }
    closedir(d);
    if (removed)
        SINFO("swept %d stale spool file(s)\n", removed);
}

static int listen_socket(void)
{
    struct sockaddr_un addr;
    int fd;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, STEM_SOCK_PATH, sizeof(addr.sun_path) - 1);

    /* A stale socket from a previous run makes bind fail with EADDRINUSE, so
     * remove it. The path is ours and /run is tmpfs. */
    unlink(STEM_SOCK_PATH);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, 1) != 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

int g_stemd_log = LOG_ERROR;   /* quiet until STEMD_LOGLEVEL says otherwise */

int main(int argc, char **argv)
{
    int lfd;

    (void)argc;
    (void)argv;

    {
        const char *lvl = getenv("STEMD_LOGLEVEL");
        int         n   = log_level_from(lvl);

        g_stemd_log = n < 0 ? LOG_ERROR : n;
        if (n < 0)
            SERR("STEMD_LOGLEVEL=\"%s\" names no level "
                 "(error|warn|info|debug|trace, or 0-4); staying at error\n", lvl);
    }

    /* A client that goes away mid-upload yields EPIPE instead of killing the
     * daemon. */
    signal(SIGPIPE, SIG_IGN);
    install_stop_handler(SIGINT);
    install_stop_handler(SIGTERM);

    spool_sweep();

    lfd = listen_socket();
    if (lfd < 0)
        return 1;

    SINFO("listening on %s\n", STEM_SOCK_PATH);

    while (!g_stop) {
        int cfd = accept(lfd, NULL, NULL);

        if (cfd < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            break;
        }
        /* One EP122, so one client at a time; a second connection waits in
         * the backlog until this one ends. */
        session_run(cfd);
        close(cfd);
    }

    close(lfd);
    unlink(STEM_SOCK_PATH);
    return 0;
}
