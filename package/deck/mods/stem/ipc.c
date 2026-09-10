// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * ipc.c - length-prefixed framing to the stem sidecar.
 *
 * WORKER THREAD ONLY. Every function here can block, and that is deliberate:
 * the PCM upload throttling against the sidecar's drain is what keeps a whole
 * track from ever being resident on either side. It is only safe because no
 * other thread ever touches this descriptor -- the message thread reads the UI
 * snapshot job.c publishes and never comes near the socket.
 *
 * The wire itself is in ../../../shared/stem_proto.h, included by both ends so
 * a framing change breaks whichever side was not rebuilt at compile time.
 */
#include "stem/stem.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

/* Reconnect attempts are rate-limited so a sidecar that is down costs one
 * connect() per interval instead of a spin. Ten seconds is well under how long
 * a user would take to notice the warn icon and do something about it. */
#define IPC_RETRY_SEC   10

/* A send that cannot make progress for this long is treated as a dead peer
 * rather than waited on forever: the sidecar drains to a socket of its own, so
 * a stall this long means it is wedged, not merely slow. */
#define IPC_SEND_TIMEOUT_SEC 30

static int      g_fd = -1;
static uint64_t g_next_try;      /* CLOCK_MONOTONIC seconds */

static uint64_t ipc_now_sec(void)
{
    struct timespec ts;

    /* CLOCK_MONOTONIC, not the wall clock: the shim time-shifts gettimeofday
     * for the guest's benefit and a jump there must not stall reconnects. */
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec;
}

void stem_ipc_close(void)
{
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
}

int stem_ipc_hello(void)
{
    struct stem_hello hello;

    if (g_fd < 0)
        return -1;
    memset(&hello, 0, sizeof(hello));
    hello.version = STEM_PROTO_VERSION;
    hello.manual = g_stem_manual ? 1u : 0u;
    if (g_stem_manual)
        snprintf(hello.addr, sizeof(hello.addr), "%s", g_stem_addr);
    return stem_ipc_send(STEM_MSG_HELLO, &hello, sizeof(hello));
}

int stem_ipc_ensure(void)
{
    struct sockaddr_un addr;
    struct timeval tv;
    uint64_t now;
    int fd;

    if (g_fd >= 0) {
        /* Holding an fd is not the same as having a peer. A sidecar restart
         * leaves this open and readable-at-EOF, and returning 0 for it silently
         * lost the next job: the shim reported "starting for ..." and the
         * sidecar never saw a byte.
         *
         * MSG_PEEK|MSG_DONTWAIT is the cheap discriminator -- 0 means the peer
         * closed, EAGAIN means alive with nothing pending, and any actual data
         * stays queued for the real read. */
        char probe;
        ssize_t n = recv(g_fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);

        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            MDBG("stem_ipc: sidecar went away, reconnecting\n");
            stem_ipc_close();
            /* Reconnect now rather than after the retry interval. A peer that
             * just vanished is a different case from one that was never there:
             * the sidecar has almost certainly already come back up under
             * systemd's Restart=always, and making this job wait 10 s to find
             * out would just lose it too. */
            g_next_try = 0;
        } else {
            return 0;               /* reused: the peer is already at its read loop */
        }
    }

    now = ipc_now_sec();
    if (now < g_next_try)
        return -1;
    g_next_try = now + IPC_RETRY_SEC;

    /* CLOEXEC: the deck forks helpers (edb_streamd, a shell for ping) that
     * inherit every descriptor. A copy of this one in a child keeps the
     * sidecar's end open after we close ours, so its read never returns and
     * no reconnect is ever accepted. */
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, STEM_SOCK_PATH, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        /* Rate-limited by the retry gate above, so this cannot spam. Logged at
         * all because the alternative is what actually happened: a job that
         * silently does nothing, with no way to tell a missing sidecar from a
         * worker that never ran. */
        MDBG("stem_ipc: connect %s failed errno=%d (retry in %ds)\n",
             STEM_SOCK_PATH, errno, IPC_RETRY_SEC);
        close(fd);
        return -1;
    }

    /* Bound both directions. Without this a wedged sidecar turns a worker
     * thread into a permanently parked one, and the only symptom would be
     * stems that never arrive. */
    tv.tv_sec = IPC_SEND_TIMEOUT_SEC;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    g_fd = fd;

    if (stem_ipc_hello() != 0) {
        stem_ipc_close();
        return -1;
    }
    MDBG("stem_ipc: connected to %s\n", STEM_SOCK_PATH);
    /* 1, not 0: the caller has to know a HELLO just went out.
     *
     * HELLO is the only thing that makes the sidecar probe the server, and during
     * that probe it stops reading this socket -- the second a PCM flood dies in.
     * It is also the only thing that makes it answer with STATUS. So "fresh
     * connection" means "wait for STATUS before sending anything large", and a
     * reused one means "the peer is already draining, do not wait for a frame
     * that will never come". */
    return 1;
}

/* write(2) is allowed to write less than asked on a stream socket, and for a
 * 32 KB PCM chunk against a throttling peer it usually does. */
static int ipc_write_all(const void *buf, size_t len)
{
    const char *p = buf;
    size_t done = 0;

    while (done < len) {
        ssize_t n = write(g_fd, p + done, len - done);

        if (n > 0) {
            done += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        /* A zero return is not an error and leaves errno holding whatever an
         * earlier call left there. Only a negative return has an errno worth
         * printing. */
        if (n == 0) {
            MDBG("stem_ipc: write %zu/%zu returned 0 (peer not draining)\n",
                 done, len);
        } else {
            /* What fd %d is at that moment: a number the deck reused for
             * something else fails in ways a socket cannot. */
            char link[64], target[96] = "?";
            struct stat st;
            int e = errno;

            snprintf(link, sizeof(link), "/proc/self/fd/%d", g_fd);
            if (readlink(link, target, sizeof(target) - 1) < 0)
                snprintf(target, sizeof(target), "readlink errno=%d", errno);
            MDBG("stem_ipc: write %zu/%zu failed errno=%d on fd %d -> %s mode=%#o\n",
                 done, len, e, g_fd, target,
                 fstat(g_fd, &st) == 0 ? (unsigned)st.st_mode : 0u);
        }
        return -1;
    }
    return 0;
}

static int ipc_read_all(void *buf, size_t len, int timeout_ms)
{
    char *p = buf;
    size_t done = 0;

    while (done < len) {
        struct pollfd pfd;
        ssize_t n;
        int rc;

        pfd.fd = g_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        rc = poll(&pfd, 1, timeout_ms);
        if (rc == 0)
            return 1;               /* nothing ready: not an error */
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        n = read(g_fd, p + done, len - done);
        if (n > 0) {
            done += (size_t)n;
            continue;
        }
        if (n == 0)
            return -1;              /* peer closed */
        if (errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

int stem_ipc_send(uint32_t type, const void *payload, uint32_t len)
{
    struct stem_frame_hdr hdr;

    if (g_fd < 0)
        return -1;

    hdr.type = type;
    hdr.len = len;
    if (ipc_write_all(&hdr, sizeof(hdr)) != 0)
        return -1;
    if (len && payload && ipc_write_all(payload, len) != 0)
        return -1;
    return 0;
}

int stem_ipc_recv(uint32_t *type, void *buf, uint32_t cap, uint32_t *len,
                  int timeout_ms)
{
    struct stem_frame_hdr hdr;
    int rc;

    if (g_fd < 0)
        return -1;

    rc = ipc_read_all(&hdr, sizeof(hdr), timeout_ms);
    if (rc != 0)
        return rc < 0 ? -1 : 0;

    /* An oversized frame means the two ends disagree about the protocol, and
     * reading it would desynchronise the stream for good. Drop the connection
     * instead: reconnecting resynchronises, guessing does not. */
    if (hdr.len > cap) {
        MDBG("stem_ipc: frame type %u len %u exceeds %u, dropping link\n",
             hdr.type, hdr.len, cap);
        return -1;
    }

    /* The header is committed, so the body must arrive; a timeout here is a
     * desynchronised stream, not an idle one. Block for it. */
    if (hdr.len && ipc_read_all(buf, hdr.len, -1) != 0)
        return -1;

    *type = hdr.type;
    *len = hdr.len;
    return 1;
}
