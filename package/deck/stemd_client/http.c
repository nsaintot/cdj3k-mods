// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * http.c - an HTTP/1.1 client sized for exactly five calls.
 *
 *   POST   /v1/jobs                    streamed PCM body, 200 or 202
 *   GET    /v1/jobs/{id}               small JSON, polled
 *   GET    /v1/jobs/{id}/stems/{name}  streamed to a file
 *   DELETE /v1/jobs/{id}
 *   GET    /v1/health
 *
 * Not a general-purpose client and it should not become one. The peer is a
 * known server on the LAN, there is no TLS, no redirects, no auth and no
 * chunked encoding to emit -- `stemd` accepts an exact Content-Length, which we
 * always know: the frame count comes from the decoder before the upload starts.
 *
 * The one property that matters is that **neither direction is ever fully
 * buffered**. The request body is pulled in slices as the socket drains, and
 * the response body is pushed to a callback as it arrives, so a 171 MB upload
 * and an 86 MB stem download both run in a fixed-size buffer.
 *
 * Three framings are accepted on the way back, because getting this wrong is a
 * silent truncation rather than an error: Content-Length (what stemd sends --
 * `get_stem` sets it explicitly), chunked, and read-until-EOF. The last is what
 * `Connection: close` licenses a server to do.
 */
/* strcasestr, for the Transfer-Encoding check. Without this it is an implicit
 * declaration returning int, which on aarch64 truncates the returned pointer --
 * a non-NULL result can then test as NULL and chunked decoding silently does
 * not happen. */
#define _GNU_SOURCE

#include "stemd_client.h"

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/time.h>

/* One buffer serves headers, the upload slices and the download chunks. 64 KB
 * is well past the point where syscall overhead matters on a LAN and still
 * small enough that two of these per connection are irrelevant next to the
 * page pool. */
#define HTTP_BUF 65536

/* A separation can take minutes, but no single read or write should. These
 * bound the socket so a server that wedges mid-transfer surfaces as a failed
 * job rather than a sidecar that never returns -- the shim's worker is blocked
 * on this call, and above it the UI is showing a progress bar that would
 * otherwise sit forever. */
#define HTTP_CONNECT_MS 5000
#define HTTP_IO_SEC     30

struct http_conn {
    int    fd;
    char   buf[HTTP_BUF];
    size_t len;      /* bytes held in buf */
    size_t pos;      /* consumed prefix */
};

/* ---- transport ------------------------------------------------------------ */

static int set_timeouts(int fd)
{
    struct timeval tv = { .tv_sec = HTTP_IO_SEC, .tv_usec = 0 };
    int one = 1;

    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0)
        return -1;
    /* The upload is one long stream and the polls are tiny; in neither case is
     * waiting to coalesce a win. */
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return 0;
}

/* Connect with a bounded wait. Non-blocking for the connect itself, then back
 * to blocking, so the rest of the function can be written straight-line. */
static int dial(const char *host, int port)
{
    struct addrinfo hints, *res = NULL, *ai;
    char portstr[16];
    int fd = -1;

    snprintf(portstr, sizeof(portstr), "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;      /* an mDNS answer may be v6 */
    hints.ai_socktype = SOCK_STREAM;

    /* Numeric first, and that is the path that actually runs: avahi-browse -rtp
     * hands back an address, not a hostname, and a manual entry typed on the
     * deck is an address too. Trying it first also keeps the common case off
     * the resolver entirely.
     *
     * The fallback is for someone who types a name. It resolves ordinary DNS --
     * this binary is musl-static (guest/Makefile builds tools with Alpine's
     * gcc), so unlike a glibc static build there is no NSS module to dlopen.
     * What it will NOT resolve is `.local`: musl has no mDNS support and no
     * nss-mdns equivalent, so an mDNS name has to arrive here already resolved,
     * which is exactly what discovery.c does. */
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
        hints.ai_flags = AI_NUMERICSERV;
        if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res)
            return -1;
    }

    for (ai = res; ai; ai = ai->ai_next) {
        struct pollfd pfd;
        int flags, err = 0;
        socklen_t elen = sizeof(err);

        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            goto connected;
        if (errno != EINPROGRESS)
            goto next;

        pfd.fd = fd;
        pfd.events = POLLOUT;
        if (poll(&pfd, 1, HTTP_CONNECT_MS) != 1)
            goto next;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 || err != 0)
            goto next;

    connected:
        fcntl(fd, F_SETFL, flags);
        if (set_timeouts(fd) != 0)
            goto next;
        freeaddrinfo(res);
        return fd;

    next:
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return -1;
}

static int write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;

    while (len) {
        ssize_t n = write(fd, p, len);

        if (n > 0) {
            p += n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return -1;   /* EAGAIN here is SO_SNDTIMEO firing: treat as dead */
    }
    return 0;
}

/* Refill from the socket. Returns bytes added, 0 at EOF, -1 on error. */
static ssize_t conn_fill(struct http_conn *c)
{
    ssize_t n;

    if (c->pos == c->len)
        c->pos = c->len = 0;
    if (c->len == sizeof(c->buf)) {
        if (c->pos == 0)
            return -1;                       /* a header line longer than the buffer */
        memmove(c->buf, c->buf + c->pos, c->len - c->pos);
        c->len -= c->pos;
        c->pos = 0;
    }
    do {
        n = read(c->fd, c->buf + c->len, sizeof(c->buf) - c->len);
    } while (n < 0 && errno == EINTR);
    if (n > 0)
        c->len += (size_t)n;
    return n;
}

/* One CRLF-terminated line, NUL-terminated in place. NULL at EOF or on error. */
static char *conn_line(struct http_conn *c)
{
    for (;;) {
        char *base = c->buf + c->pos;
        size_t avail = c->len - c->pos;
        char *nl = memchr(base, '\n', avail);

        if (nl) {
            size_t n = (size_t)(nl - base);

            c->pos += n + 1;
            if (n && base[n - 1] == '\r')
                n--;
            base[n] = '\0';
            return base;
        }
        if (conn_fill(c) <= 0)
            return NULL;
    }
}

/* ---- response ------------------------------------------------------------- */

static int hdr_is(const char *line, const char *name, const char **val)
{
    size_t n = strlen(name);

    if (strncasecmp(line, name, n) != 0 || line[n] != ':')
        return 0;
    line += n + 1;
    while (*line == ' ' || *line == '\t')
        line++;
    *val = line;
    return 1;
}

/* Hand `len` bytes of body to push, pulling more from the socket as needed.
 * `len` of (uint64_t)-1 means "until EOF". */
static int drain_body(struct http_conn *c, uint64_t len,
                      http_body_push_fn push, void *user)
{
    int to_eof = (len == (uint64_t)-1);

    while (to_eof || len) {
        size_t have = c->len - c->pos;

        if (!have) {
            ssize_t n = conn_fill(c);

            if (n == 0)
                return to_eof ? 0 : -1;      /* short body is an error */
            if (n < 0)
                return -1;
            have = c->len - c->pos;
        }
        if (!to_eof && have > len)
            have = (size_t)len;
        if (push && push(c->buf + c->pos, have, user) != 0)
            return -1;
        c->pos += have;
        if (!to_eof)
            len -= have;
    }
    return 0;
}

static int drain_chunked(struct http_conn *c, http_body_push_fn push, void *user)
{
    for (;;) {
        char *line = conn_line(c);
        unsigned long long n;

        if (!line)
            return -1;
        n = strtoull(line, NULL, 16);        /* any ;extension is ignored */
        if (n == 0)
            break;
        if (drain_body(c, (uint64_t)n, push, user) != 0)
            return -1;
        if (!conn_line(c))                   /* the CRLF after each chunk */
            return -1;
    }
    /* Trailers, then the blank line. */
    for (;;) {
        char *line = conn_line(c);

        if (!line)
            return -1;
        if (!line[0])
            return 0;
    }
}

/* ---- the one entry point -------------------------------------------------- */

int http_perform(const struct stem_server *srv, const struct http_request *req)
{
    struct http_conn c;
    char head[1024];
    int status = -1, n;
    uint64_t body_len = (uint64_t)-1;
    int chunked = 0, have_len = 0;
    char *line;

    if (!srv || !req || !req->method || !req->path)
        return -1;

    memset(&c, 0, sizeof(c));
    c.fd = dial(srv->host, srv->port);
    if (c.fd < 0)
        return -1;

    /* `Connection: close` on purpose. Reuse would save a handshake on the
     * poll loop, but it also means a half-consumed body poisons the next call,
     * and on a LAN the handshake is not what this costs. */
    n = snprintf(head, sizeof(head),
                 "%s %s HTTP/1.1\r\n"
                 "Host: %s:%d\r\n"
                 "Connection: close\r\n",
                 req->method, req->path, srv->host, srv->port);
    if (n < 0 || (size_t)n >= sizeof(head))
        goto out;
    if (req->content_type) {
        int m = snprintf(head + n, sizeof(head) - (size_t)n,
                         "Content-Type: %s\r\n"
                         "Content-Length: %llu\r\n",
                         req->content_type,
                         (unsigned long long)req->content_length);
        if (m < 0 || (size_t)(n + m) >= sizeof(head))
            goto out;
        n += m;
    }
    if ((size_t)n + 2 >= sizeof(head))
        goto out;
    memcpy(head + n, "\r\n", 3);
    if (write_all(c.fd, head, (size_t)n + 2) != 0)
        goto out;

    /* Body, in slices. The pull callback is the decoder upstream, so this loop
     * is also what throttles it: nothing is produced faster than the socket
     * drains, which is the whole reason a track never becomes resident. */
    if (req->content_type && req->pull) {
        uint64_t left = req->content_length;

        while (left) {
            size_t want = left < sizeof(c.buf) ? (size_t)left : sizeof(c.buf);
            size_t got = req->pull(c.buf, want, req->pull_user);

            if (got == 0)
                goto out;                    /* short body; we promised a length */
            if (got > want)
                got = want;
            if (write_all(c.fd, c.buf, got) != 0)
                goto out;
            left -= got;
        }
        c.len = c.pos = 0;
    }

    /* Status line. */
    line = conn_line(&c);
    if (!line || strncmp(line, "HTTP/1.", 7) != 0)
        goto out;
    status = (int)strtol(line + 9, NULL, 10);
    if (status < 100 || status > 599) {
        status = -1;
        goto out;
    }

    for (;;) {
        const char *val;

        line = conn_line(&c);
        if (!line) {
            status = -1;
            goto out;
        }
        if (!line[0])
            break;
        if (hdr_is(line, "Content-Length", &val)) {
            body_len = strtoull(val, NULL, 10);
            have_len = 1;
        } else if (hdr_is(line, "Transfer-Encoding", &val)) {
            if (strcasestr(val, "chunked"))
                chunked = 1;
        }
    }

    /* Chunked wins if both are present, per RFC 7230. A 204 or a HEAD-like
     * empty response has neither and no body to read. */
    if (req->begin)
        req->begin(!chunked && have_len ? body_len : HTTP_BODY_LEN_UNKNOWN,
                   req->push_user);
    if (chunked) {
        if (drain_chunked(&c, req->push, req->push_user) != 0)
            status = -1;
    } else if (have_len) {
        if (body_len && drain_body(&c, body_len, req->push, req->push_user) != 0)
            status = -1;
    } else if (status != 204 && status != 304) {
        if (drain_body(&c, (uint64_t)-1, req->push, req->push_user) != 0)
            status = -1;
    }

out:
    close(c.fd);
    return status;
}
