// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * session.c - one shim connection, framed with stem_proto, driving stemd.
 *
 * The shape that matters is the upload. `http_perform` wants a pull callback
 * for the request body, and the PCM arrives as frames on the shim socket, so
 * the callback simply reads the shim socket: the POST body IS the frame stream,
 * unwrapped. Nothing is ever buffered -- the 171 MB of an eight-minute track
 * moves through a 64 KB window in http.c and a frame header's worth of state
 * here.
 *
 * That inversion is why JOB_BEGIN does the whole upload rather than just
 * starting one: by the time http_perform returns, every PCM frame has been
 * consumed and the next frame the loop reads is JOB_END.
 *
 * JSON is scanned, not parsed, for the same reason as in discovery.c: four
 * fields from a server whose shape we control, where an unrecognised document
 * should read as a failure anyway.
 */
#include "stemd_client.h"
#include "json.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>

/* Big enough for any control frame and for a job document. PCM normally goes
 * straight into the POST body and never lands here; the exception is a drain,
 * where there is no POST to feed it to. */
#define CTRL_MAX 4096

/* How long to wait between job polls. Separation of an eight-minute track runs
 * tens of seconds, so this is frequent enough for a smooth bar and far too
 * slow to matter as load. */
#define POLL_INTERVAL_US (500 * 1000)

/* Give up on a job that never reaches a terminal stage. Long, because a queued
 * job behind two others legitimately waits. */
#define POLL_MAX_SECONDS 900

/* The server's `dsp_mode` 1 is its copy of the deck's own 44.1 -> 96 kHz
 * converter, and covers that pair alone. Asked for at that output rate and
 * nowhere else; every other rate takes the server's default resampler. */
#define STEMD_DSP_MODE_MATCHED     1
#define STEMD_DSP_MODE_MATCHED_HZ  96000u

static int read_all(int fd, void *buf, size_t len)
{
    char *p = buf;
    size_t done = 0;

    while (done < len) {
        ssize_t n = read(fd, p + done, len - done);

        if (n > 0) {
            done += (size_t)n;
            continue;
        }
        if (n == 0)
            return -1;                 /* peer closed */
        if (errno == EINTR) {
            /* A retry here is what used to swallow SIGTERM: the signal arrived,
             * the read restarted, and systemd SIGKILLed five seconds later with
             * a job possibly still open on the server. */
            if (session_should_stop())
                return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

/* Consume a frame's payload and throw it away, so the stream stays framed
 * whatever the frame turned out to be. */
static int skip_payload(int fd, uint32_t len)
{
    static char scrap[CTRL_MAX];

    while (len) {
        size_t want = len > sizeof(scrap) ? sizeof(scrap) : len;

        if (read_all(fd, scrap, want) != 0)
            return -1;
        len -= (uint32_t)want;
    }
    return 0;
}

static int send_frame(int fd, uint32_t type, const void *payload, uint32_t len)
{
    struct stem_frame_hdr hdr = { type, len };
    const char *p = payload;
    size_t done = 0;

    if (write(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr))
        return -1;
    while (done < len) {
        ssize_t n = write(fd, p + done, len - done);

        if (n > 0) {
            done += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

static void report_status(int fd, const struct stem_server *srv)
{
    struct stem_status st;

    memset(&st, 0, sizeof(st));
    st.reachable = (uint32_t)srv->reachable;
    st.compatible = (uint32_t)srv->compatible;
    /* Carried on every status so the deck can persist it. That is what lets the
     * on-media cache work later with no server at all: a deck has to have met
     * this server once, but only once. */
    snprintf(st.sep_id, sizeof(st.sep_id), "%s", srv->sep_id);
    send_frame(fd, STEM_MSG_STATUS, &st, sizeof(st));
}

static void report_failed(int fd, int http_status)
{
    struct stem_failed f;

    memset(&f, 0, sizeof(f));
    f.http_status = (uint32_t)(http_status > 0 ? http_status : 0);
    send_frame(fd, STEM_MSG_JOB_FAILED, &f, sizeof(f));
}

/* stemd's Stage is serialised snake_case. Mapped onto our wire enum, which the
 * UI's progress row reads directly. */
static int stage_of(const char *doc)
{
    char s[32];

    if (json_str(doc, "stage", s, sizeof(s)) != 0)
        return -1;
    if (!strcmp(s, "queued"))         return STEM_STAGE_QUEUED;
    if (!strcmp(s, "analysing"))      return STEM_STAGE_ANALYZING;
    if (!strcmp(s, "separating"))     return STEM_STAGE_SEPARATING;
    if (!strcmp(s, "reconstructing")) return STEM_STAGE_RECONSTRUCTING;
    if (!strcmp(s, "writing"))        return STEM_STAGE_WRITING;
    if (!strcmp(s, "done"))           return STEM_STAGE_DONE;
    if (!strcmp(s, "failed"))         return STEM_STAGE_FAILED;
    return -1;
}

/* One PROGRESS frame. The percent is the SENDER'S OWN LEG, never a position on the
 * deck's bar -- see struct stem_progress. */
static void send_progress(int fd, int stage, int percent)
{
    struct stem_progress prog;

    memset(&prog, 0, sizeof(prog));
    prog.stage = (uint32_t)stage;
    prog.percent = (uint32_t)percent;
    send_frame(fd, STEM_MSG_PROGRESS, &prog, sizeof(prog));
}

/* ---- response capture ----------------------------------------------------- */

struct doc_buf {
    char   data[CTRL_MAX];
    size_t len;
};

static int doc_sink(const void *buf, size_t len, void *user)
{
    struct doc_buf *d = user;
    size_t room = sizeof(d->data) - 1 - d->len;

    if (len > room)
        len = room;
    memcpy(d->data + d->len, buf, len);
    d->len += len;
    d->data[d->len] = '\0';
    return 0;
}

/* ---- the upload body ------------------------------------------------------ */

/* Feeds http.c from the shim socket. `left` is what the POST promised, so this
 * stops exactly on the byte boundary and leaves JOB_END for the frame loop. */
struct upload_pull {
    int      fd;
    uint64_t left;         /* body bytes still owed */
    uint32_t frame_left;   /* bytes remaining in the PCM frame being drained */
    int      err;          /* 1 = broken/protocol, 2 = the shim cancelled */
};

static size_t upload_pull(void *buf, size_t cap, void *user)
{
    struct upload_pull *u = user;
    ssize_t n;

    if (u->err || u->left == 0)
        return 0;

    while (u->frame_left == 0) {
        struct stem_frame_hdr h;

        if (read_all(u->fd, &h, sizeof(h)) != 0) {
            u->err = 1;
            return 0;
        }
        if (h.type == STEM_MSG_PCM) {
            u->frame_left = h.len;
            continue;
        }
        /* A CANCEL mid-upload is the deck changing track. Distinguished from a
         * broken stream so the caller can stay quiet about it. */
        u->err = (h.type == STEM_MSG_CANCEL) ? 2 : 1;
        return 0;
    }

    if (cap > u->frame_left)
        cap = u->frame_left;
    if ((uint64_t)cap > u->left)
        cap = (size_t)u->left;

    do {
        n = read(u->fd, buf, cap);
    } while (n < 0 && errno == EINTR);
    if (n <= 0) {
        u->err = 1;
        return 0;
    }
    u->frame_left -= (uint32_t)n;
    u->left -= (uint64_t)n;
    return (size_t)n;
}

/* ---- fetching a stem ------------------------------------------------------ */

struct stem_out {
    int      fd;
    uint64_t written;
};

static int stem_sink(const void *buf, size_t len, void *user)
{
    struct stem_out *o = user;
    const char *p = buf;
    size_t done = 0;

    while (done < len) {
        ssize_t n = write(o->fd, p + done, len - done);

        if (n > 0) {
            done += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return -1;
    }
    o->written += len;
    return 0;
}

/* A 44-byte canonical WAV header. Written in front of the raw s16 body so the
 * file is something the deck's OWN FileReadWav opens -- store.c then decodes it
 * through the same createReaderFor path as a real track, which is what keeps
 * stems on the pool's timeline without a decoder of ours. */
static void wav_header(unsigned char *h, uint32_t rate, uint16_t ch,
                       uint16_t bits, uint32_t data_bytes)
{
    uint32_t byte_rate = rate * ch * (bits / 8u);
    uint16_t align = (uint16_t)(ch * (bits / 8u));

#define P32(o, v) do { uint32_t _v = (v);          \
        h[o] = (unsigned char)(_v);                \
        h[(o) + 1] = (unsigned char)(_v >> 8);     \
        h[(o) + 2] = (unsigned char)(_v >> 16);    \
        h[(o) + 3] = (unsigned char)(_v >> 24); } while (0)
#define P16(o, v) do { uint16_t _v = (v);          \
        h[o] = (unsigned char)(_v);                \
        h[(o) + 1] = (unsigned char)(_v >> 8); } while (0)

    memcpy(h, "RIFF", 4);
    P32(4, 36u + data_bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    P32(16, 16u);              /* PCM fmt chunk size */
    P16(20, 1u);               /* PCM */
    P16(22, ch);
    P32(24, rate);
    P32(28, byte_rate);
    P16(32, align);
    P16(34, bits);
    memcpy(h + 36, "data", 4);
    P32(40, data_bytes);
#undef P32
#undef P16
}

/* The query-string spelling of what we want back. FLAC is stemd's own default
 * and what the deck asks for: lossless, so the reconstruction stays exact, and
 * roughly 40% of the raw size -- a sparse stem such as vocals over an
 * instrumental compresses to a quarter. */
static const char *out_format_name(uint32_t f)
{
    switch (f) {
    case STEM_PCM_F32LE: return "f32le";
    case STEM_PCM_S16LE: return "s16le";
    default:             return "flac";
    }
}

/* GET one stem into tmpfs and tell the shim where it is. */
static int fetch_stem(int fd, const struct stem_server *srv, const char *job_id,
                      const char *name, uint32_t rate, uint32_t frames,
                      uint32_t format, float gain)
{
    char path[192], part[200], url[192];
    unsigned char hdr[44];
    struct http_request req;
    struct stem_out out;
    struct stem_ready *r;
    size_t name_len = strlen(name);
    /* FLAC arrives as a complete file and must be written through untouched.
     * Only the raw formats need a container put in front of them, and putting
     * one in front of FLAC would produce a file nothing can open. */
    const int raw = (format != STEM_PCM_FLAC);
    const char *ext = raw ? ".wav" : ".flac";
    char msg[CTRL_MAX];
    int status;

    /* FIXED NAMES, deliberately not keyed by job.
     *
     * The spool lives in /dev/shm, which is guest RAM on a 3 GiB ceiling. Naming
     * each download after its job made every one a new file, so a failed job or
     * a shim that never adopted its stems left ~170 MB behind and nothing ever
     * collected it -- five jobs had accumulated 380 MB. One name per part makes
     * the footprint bounded by construction rather than by everyone remembering
     * to clean up: the next track's stems land on the previous track's.
     *
     * Safe because there is exactly one job in flight -- one EP122, one client,
     * and the shim serialises jobs on its worker -- so nothing is reading these
     * while the next pair is written.
     *
     * Written under .part and renamed, so the path in STEM_READY never names a
     * file that is still being filled. Within one job the ordering already
     * guaranteed that; with reused names it also has to hold ACROSS jobs. */
    snprintf(path, sizeof(path), "%s/%s%s", STEM_SPOOL_DIR, name, ext);
    snprintf(part, sizeof(part), "%s.part", path);
    snprintf(url, sizeof(url), "/v1/jobs/%s/stems/%s", job_id, name);

    out.fd = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    out.written = 0;
    if (out.fd < 0) {
        SERR("cannot create %s: %s\n",
                part, strerror(errno));
        return -1;
    }
    /* The WAV header exists only so the deck's own FileReadWav can open a body
     * that is otherwise headerless samples. A FLAC stream carries its own, and
     * the bytes go to disk exactly as they arrive -- the encode already happened
     * on the server, so storing the stream verbatim costs no transcode in either
     * direction. */
    if (raw) {
        wav_header(hdr, rate, STEM_WIRE_CHANNELS, 16,
                   frames * STEM_WIRE_CHANNELS * 2u);
        if (write(out.fd, hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) {
            close(out.fd);
            unlink(part);
            return -1;
        }
    }

    memset(&req, 0, sizeof(req));
    req.method = "GET";
    req.path = url;
    req.push = stem_sink;
    req.push_user = &out;
    status = http_perform(srv, &req);
    close(out.fd);

    if (status != 200) {
        SWARN("%s -> %d\n", url, status);
        unlink(part);
        return -1;
    }
    if (rename(part, path) != 0) {
        SERR("rename %s: %s\n", part, strerror(errno));
        unlink(part);
        return -1;
    }
    SINFO("%s -> %s (%llu bytes, gain %.4f)\n",
          name, path, (unsigned long long)out.written, (double)gain);

    /* The shim unlinks the file once it has decoded it into its own RAM, so a
     * pair normally lives only as long as it takes to adopt. The fixed name is
     * the backstop for when it does not. */
    if (sizeof(*r) + name_len >= sizeof(msg))
        return -1;
    r = (struct stem_ready *)msg;
    memset(r, 0, sizeof(*r));
    r->part = !strcmp(name, STEM_WIRE_VOCALS) ? STEM_PART_VOCALS
                                              : STEM_PART_HARMONICS;
    r->gain = gain;
    r->path_len = (uint32_t)strlen(path);
    if (sizeof(*r) + r->path_len >= sizeof(msg))
        return -1;
    memcpy(msg + sizeof(*r), path, r->path_len);
    return send_frame(fd, STEM_MSG_STEM_READY, msg,
                      (uint32_t)(sizeof(*r) + r->path_len));
}

/* Per-stem gain, read out of the job document's `stems` array. The array is
 * small and ordered, so the scan starts at the stem's own name rather than
 * trying to model JSON nesting. */
static float stem_gain(const char *doc, const char *name)
{
    char pat[64];
    const char *p;

    snprintf(pat, sizeof(pat), "\"name\":\"%s\"", name);
    p = strstr(doc, pat);
    if (!p)
        return 1.0f;
    return (float)json_num(p, "gain", 1.0);
}

/* Wait out one poll interval, but WATCHING THE DECK rather than merely sleeping.
 *
 * A separation is where a job spends nearly all of its life, and until this
 * existed it was also where the deck could not be heard: the poll slept, the
 * shim's socket went unread, and a CANCEL sat in the buffer until the job it was
 * meant to stop had finished. Measured end to end -- cancelled at 10:20:29, the
 * cancel not seen until 10:20:50, and in between the deck's NEXT upload filled
 * the socket buffer and died, leaving the row grey.
 *
 * A CANCEL and NOTHING ELSE ends a running job. The deck's separator sits in its
 * own poll loop for the whole of a separation and sends exactly one kind of frame
 * from there, so any other frame is a protocol surprise -- and the wrong answer
 * to a surprise is to throw away a separation nobody asked to stop. It is skipped
 * and logged instead, which costs whatever that frame meant and keeps the stream
 * framed.
 *
 * That the deck cannot start a second job without cancelling the first is what
 * makes skipping safe: a JOB_BEGIN can never be the frame that gets here first.
 *
 *    1  the deck cancelled
 *    0  the interval elapsed, or a frame arrived that does not end the job
 *   -1  the deck went away */
static int await_poke(int fd, unsigned us)
{
    struct stem_frame_hdr hdr;
    struct pollfd pfd;
    int n;

    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    n = poll(&pfd, 1, (int)(us / 1000));
    if (n <= 0)
        return 0;                       /* the interval simply elapsed */
    if (read_all(fd, &hdr, sizeof(hdr)) != 0)
        return -1;                      /* the deck went away */
    if (hdr.type == STEM_MSG_CANCEL)
        return 1;                       /* carries no payload: nothing to drain */
    SWARN("frame %u during a separation -> skipped, the job stands\n",
          hdr.type);
    return skip_payload(fd, hdr.len) == 0 ? 0 : -1;
}

/* Poll the server until the job is done, or until the deck cancels it.
 *
 *    0  done, final_doc holds the job document
 *   -1  failed, timed out, or the link broke
 *    1  cancelled by the deck */
static int await_job(int fd, const struct stem_server *srv, const char *job_id,
                     struct doc_buf *final_doc)
{
    unsigned waited_us = 0;

    for (;;) {
        struct http_request req;
        struct stem_progress prog;
        char url[128];
        int status, stage;

        snprintf(url, sizeof(url), "/v1/jobs/%s", job_id);
        memset(final_doc, 0, sizeof(*final_doc));
        memset(&req, 0, sizeof(req));
        req.method = "GET";
        req.path = url;
        req.push = doc_sink;
        req.push_user = final_doc;

        status = http_perform(srv, &req);
        if (status != 200) {
            SWARN("poll %s -> %d\n", url, status);
            return -1;
        }

        stage = stage_of(final_doc->data);
        if (stage < 0) {
            SWARN("poll: no stage in job document\n");
            return -1;
        }

        memset(&prog, 0, sizeof(prog));
        prog.stage = (uint32_t)stage;
        prog.percent = (uint32_t)(json_num(final_doc->data, "fraction", 0.0)
                                  * 100.0);
        prog.queue_position = (uint32_t)json_num(final_doc->data, "completed", 0);
        send_frame(fd, STEM_MSG_PROGRESS, &prog, sizeof(prog));

        if (stage == STEM_STAGE_DONE)
            return 0;
        if (stage == STEM_STAGE_FAILED) {
            SERR("job %s failed\n", job_id);
            return -1;
        }

        /* The poll loop, not the frame read, is where a shutdown actually lands:
         * a job spends nearly all its life asleep here. usleep returns EINTR on
         * SIGTERM and continuing regardless is what kept systemd SIGKILLing
         * after five seconds -- measured, after fixing the read path first and
         * finding the process parked in nanosleep rather than read. */
        {
            int poked = await_poke(fd, POLL_INTERVAL_US);

            if (poked < 0)
                return -1;
            if (poked > 0) {
                SINFO("deck cancelled job %s\n", job_id);
                return 1;
            }
        }
        if (session_should_stop()) {
            SINFO("stopping, abandoning job %s\n",
                    job_id);
            return -1;
        }
        waited_us += POLL_INTERVAL_US;
        if (waited_us / 1000000u > POLL_MAX_SECONDS) {
            SERR("job %s timed out\n", job_id);
            return -1;
        }
    }
}

/* Let go of a job on the server.
 *
 * DELETE stops it wherever it is -- queued OR mid-separation.
 *
 * It used to be a handle drop with a best-effort cancel, on the reading that a running
 * separation would finish anyway and land in stemd's cache, so a DJ who came back got a
 * 200 instead of a second run. That is no longer the trade: the server cancels for real,
 * so skipping a track frees the machine now rather than after a separation nobody is
 * waiting for. stemd still only cancels once the last holder lets go, so this never
 * takes a separation out from under another deck. */
static void job_release(struct stem_server *srv, char *job_id)
{
    struct http_request req;
    int status;
    char url[128];

    if (!job_id[0])
        return;
    snprintf(url, sizeof(url), "/v1/jobs/%s", job_id);
    memset(&req, 0, sizeof(req));
    req.method = "DELETE";
    req.path = url;
    status = http_perform(srv, &req);
    SDBG("released %s -> %d\n", job_id, status);
    job_id[0] = '\0';
}

void session_run(int fd)
{
    struct stem_server srv;
    struct doc_buf doc;
    char manual[64] = "";
    char job_id[96] = "";
    uint64_t pcm_expected = 0;
    uint32_t job_frames = 0;
    /* What we asked the server to encode the stems as. Carried from JOB_BEGIN
     * to the fetch, because it decides both the file extension and whether a
     * WAV header goes in front of the body. */
    uint32_t job_format = STEM_PCM_FLAC;
    int hello_seen = 0;
    /* Readiness as of the last HELLO, so the 30 s refresh can report a CHANGE
     * instead of restating the same answer. -1 = nothing said yet, so the first
     * HELLO always reports. */
    int was_ready = -1;

    memset(&srv, 0, sizeof(srv));

    /* Discovery is deferred to HELLO, which is what carries the deck's STEM
     * LOCATION. Probing before it arrived is what made the MANUAL address dead:
     * the answer was always the mDNS one. */
    for (;;) {
        struct stem_frame_hdr hdr;

        if (read_all(fd, &hdr, sizeof(hdr)) != 0)
            goto done;

        switch (hdr.type) {
        case STEM_MSG_HELLO: {
            struct stem_hello h;

            if (hdr.len != sizeof(h) || read_all(fd, &h, sizeof(h)) != 0)
                goto done;
            if (h.version != STEM_PROTO_VERSION) {
                /* A half-updated deploy: fail loudly at connect rather than
                 * subtly at frame 900. */
                SERR("protocol %u, peer speaks %u -- half-updated deploy, "
                     "refusing the connection\n", STEM_PROTO_VERSION, h.version);
                goto done;
            }
            hello_seen = 1;
            /* Assigned on BOTH branches. A HELLO is also how a settings change
             * arrives on a connection that is already up, so leaving the old value
             * in place made MANUAL -> AUTO a no-op: the deck showed AUTO and the
             * sidecar kept talking to the address that had been typed. */
            h.addr[sizeof(h.addr) - 1] = '\0';
            {
                char was[sizeof(manual)];

                snprintf(was, sizeof(was), "%s", manual);
                snprintf(manual, sizeof(manual), "%s", h.manual ? h.addr : "");
                /* Only the FIRST hello and a genuine change of location need to
                 * go looking. The rest are the deck's 30 s status refresh, and
                 * answering those with a browse is what made the deck flap:
                 * avahi's cache is not always warm at the instant we ask, and an
                 * empty browse is indistinguishable from a server that is down.
                 * Re-probing the address we already have answers the question
                 * the refresh is actually asking -- is it still up -- and falls
                 * back to a browse only when it is not. */
                if (strcmp(was, manual) != 0 || discovery_recheck(&srv) != 0)
                    discovery_find(manual[0] ? manual : NULL, &srv);
            }
            {
                int ready = srv.compatible ? 1 : 0;

                SDBG("hello, location %s%s -> %s\n",
                     manual[0] ? "MANUAL " : "AUTO (mDNS)", manual,
                     ready ? "ready" : "no server");
                if (ready != was_ready) {
                    if (ready)
                        SINFO("stem server %s:%d ready (%s)\n",
                              srv.host, srv.port, srv.sep_id);
                    else
                        SWARN("no stem server, location %s%s\n",
                              manual[0] ? "MANUAL " : "AUTO (mDNS)", manual);
                    was_ready = ready;
                }
            }
            report_status(fd, &srv);
            break;
        }

        case STEM_MSG_JOB_BEGIN: {
            struct stem_job_begin b;
            struct upload_pull up;
            struct http_request req;
            char url[192];
            int status, n;

            if (!hello_seen || hdr.len != sizeof(b) ||
                read_all(fd, &b, sizeof(b)) != 0)
                goto done;
            if (!srv.reachable || !srv.compatible) {
                report_failed(fd, 0);
                break;
            }

            pcm_expected = b.frames * b.channels * sizeof(float);
            job_frames = (uint32_t)b.frames;
            job_format = b.output_format;
            SINFO("job %llu frames @ %u Hz, %u ch, %llu bytes\n",
                  (unsigned long long)b.frames, b.sample_rate, b.channels,
                  (unsigned long long)pcm_expected);

            /* `format` describes the body going up and is always f32le: that is
             * what the deck's decoder already produces, so sending it is a
             * memcpy. `output_format` describes what comes back, and is asked
             * for explicitly rather than left to the server's default -- what
             * arrives decides whether a WAV header is written in front of it. */
            n = snprintf(url, sizeof(url),
                         "/v1/jobs?sample_rate=%u&channels=%u"
                         "&format=f32le&output_format=%s",
                         b.sample_rate, b.channels,
                         out_format_name(b.output_format));
            /* Only when the deck asked for one. Omitting the parameter is what
             * gets the server's default, and that is the behaviour a deck that
             * could not establish its pool rate is asking for -- not 0 Hz. */
            if (b.output_sample_rate && n > 0 && (size_t)n < sizeof(url))
                n += snprintf(url + n, sizeof(url) - (size_t)n,
                              "&output_sample_rate=%u", b.output_sample_rate);
            /* A good resampler is not enough on this path. The deck subtracts
             * the stems from a mix IT resampled, and that only cancels if both
             * sides went through the same filter: two good ones disagree by
             * about -36 dB of the derived part. `dsp_mode` is opt-in and the
             * server's default is its own general resampler, so the match has to
             * be asked for. A server too old to know the parameter ignores it. */
            if (b.output_sample_rate == STEMD_DSP_MODE_MATCHED_HZ && n > 0 &&
                (size_t)n < sizeof(url))
                snprintf(url + n, sizeof(url) - (size_t)n, "&dsp_mode=%d",
                         STEMD_DSP_MODE_MATCHED);
            SDBG("POST %s\n", url);

            up.fd = fd;
            up.left = pcm_expected;
            up.frame_left = 0;
            up.err = 0;

            memset(&doc, 0, sizeof(doc));
            memset(&req, 0, sizeof(req));
            req.method = "POST";
            req.path = url;
            req.content_type = "application/octet-stream";
            req.content_length = pcm_expected;
            req.pull = upload_pull;
            req.pull_user = &up;
            req.push = doc_sink;
            req.push_user = &doc;

            /* This consumes every PCM frame, so on return the next frame the
             * loop reads is JOB_END. */
            status = http_perform(&srv, &req);

            if (up.err == 2) {
                SINFO("upload cancelled by the deck\n");
                job_id[0] = '\0';
                break;
            }
            if (up.err || (status != 200 && status != 202)) {
                SERR("POST %s -> %d (pull err %d)\n", url, status, up.err);
                report_failed(fd, status);
                job_id[0] = '\0';
                break;
            }
            if (json_str(doc.data, "id", job_id, sizeof(job_id)) != 0) {
                SERR("no job id in the response\n");
                report_failed(fd, status);
                job_id[0] = '\0';
                break;
            }
            SINFO("job %s accepted (%d)\n", job_id, status);
            break;
        }

        case STEM_MSG_PCM:
            /* Only reachable when a POST was not opened -- an unreachable
             * server, or a failed one. Drain so the stream stays framed. */
            if (skip_payload(fd, hdr.len) != 0)
                goto done;
            break;

        case STEM_MSG_JOB_END: {
            uint32_t rate;

            if (!job_id[0])
                break;                  /* already reported failed or cancelled */

            {
                int r = await_job(fd, &srv, job_id, &doc);

                if (r > 0) {
                    /* Another track needs the server. Let this job go -- DELETE
                     * interrupts a running separation, so it frees the machine
                     * rather than finishing for nobody. Not reported as a
                     * failure: nothing failed. The JOB_BEGIN for the track that
                     * caused the cancel is the next frame the loop reads. */
                    job_release(&srv, job_id);
                    job_id[0] = '\0';
                    break;
                }
                if (r != 0) {
                    report_failed(fd, 0);
                    job_id[0] = '\0';
                    break;
                }
            }

            /* The result's own frame count and rate, not the request's: a cache
             * hit is answered by whatever earlier job produced it. */
            rate = (uint32_t)json_num(doc.data, "sample_rate", STEM_WIRE_RATE);
            job_frames = (uint32_t)json_num(doc.data, "frames", job_frames);

            {
                static const char *const k_part[] = { STEM_WIRE_HARMONICS,
                                                      STEM_WIRE_VOCALS };
                const int n = (int)(sizeof(k_part) / sizeof(k_part[0]));
                int i, got = 0;

                /* THE DOWNLOAD'S OWN 0..100, exactly as every other stage reports --
                 * where it lands on the bar is the deck's to decide. A fixed figure
                 * here is a bar that steps BACKWARDS when the server hands over,
                 * because it is answering a different question than the stage before it.
                 *
                 * BOTH ENDS OF EACH STEM, which is what was missing: reporting only
                 * `i / n` before each pull sends 0 and 50 for a pair and stops. The leg
                 * ends at half its own length for ever, so the deck's third segment was
                 * left part-filled and the next thing drawn was the local decode at the
                 * start of the fourth -- a segment the bar visibly never crossed. */
                for (i = 0; i < n; i++) {
                    send_progress(fd, STEM_STAGE_FETCHING, (i * 100) / n);
                    if (fetch_stem(fd, &srv, job_id, k_part[i], rate, job_frames,
                                   job_format,
                                   stem_gain(doc.data, k_part[i])) != 0) {
                        report_failed(fd, 0);
                        break;
                    }
                    got = i + 1;
                }
                /* Only a COMPLETE fetch closes the leg. A pull that died half way has
                 * already reported failed, and following it with a full bar would be
                 * the last thing the DJ saw before the row went grey. */
                if (got == n)
                    send_progress(fd, STEM_STAGE_FETCHING, 100);
            }
            job_id[0] = '\0';
            break;
        }

        case STEM_MSG_CANCEL: {
            if (!job_id[0]) {
                SWARN("cancel, no job in flight\n");
                break;
            }
            job_release(&srv, job_id);
            break;
        }

        default:
            /* Unknown frame: skip its payload so the stream stays framed. */
            if (skip_payload(fd, hdr.len) != 0)
                goto done;
            break;
        }
    }

done:
    /* The deck's socket going away is a cancel too.
     *
     * Every other way out of that loop is an error path -- a short read, a failed write,
     * a version mismatch -- and each of them used to walk away from a job the server was
     * still working on, with nobody left to collect it: this process holds the only
     * handle and the deck reconnects with a clean slate. Now that a DELETE genuinely
     * interrupts, releasing here is the difference between a dropped connection freeing
     * the machine and a separation running to completion for a listener that hung up. */
    job_release(&srv, job_id);
}
