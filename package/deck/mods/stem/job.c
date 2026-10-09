// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * job.c - the stem job state machine.
 *
 * This is the only module that writes state other threads read; a field that
 * crosses a thread boundary and is not written here should be questioned.
 *
 *   -> the message thread, via `struct stem_ui_state` and a generation counter
 *   -> the audio thread, via g_stem_ready and nothing else
 *
 * The job runs on our own worker thread, which owns the sidecar socket end to
 * end, so the fd needs no locking. Blocking on a PCM write is intended: it is
 * the backpressure that keeps a whole track from being resident anywhere.
 *
 * Lifecycle, one track at a time:
 *
 *   track load (decode.c) -> stem_job_request()
 *     worker: connect -> JOB_BEGIN -> stream PCM -> JOB_END
 *             -> poll PROGRESS -> adopt each STEM_READY -> g_stem_ready = 1
 *   track change          -> stem_track_gone()
 *     message: g_stem_ready = 0; loader frees and re-examines the new track
 */
#include "stem/job_internal.h"
/* For persisting the server's separation id and telling the waveform when a
 * set becomes playable. */
#include "core/mod_settings.h"
#include "wave/wave.h"
#include "db/db.h"
#include "xpad/ext.h"
#include "kit/menu.h"
#include "kit/mod.h"
#include "kit/popup.h"

#include <pthread.h>


/* How long the sidecar may stay silent before the job is abandoned. It sends a
 * PROGRESS frame twice a second during a separation, so this is 120x the normal
 * cadence: enough for the gap between JOB_END and the server accepting the
 * POST, and well under the sidecar's own 900 s cap. */
#define JOB_SILENCE_SEC 60

/* One PCM chunk on the wire. Matches the decoder's chunk so nothing is
 * re-buffered between fileRead and write. */
#define JOB_PCM_FRAMES      4096

/* The audio thread's entire view of us. See stem.h for the ordering rule. */
volatile int g_stem_ready;

/* ---- the UI snapshot ------------------------------------------------------
 *
 * A seqlock, because the message thread must never block and is our only
 * repaint tick. An odd generation means a write is in progress and the reader
 * retries; a torn read costs one repaint. */
static struct stem_ui_state g_ui;

/* Whether the run in flight goes through the server, carried into every
 * publish. Set where the path is chosen (loader_serve for a cache hit,
 * sep_request for the server), the only place it is known; the row cannot
 * derive it from a stage. See via_server in stem.h. */
int g_job_via_server;

/* The generation is updated with atomic adds because there are two writers:
 * the loader publishes a job's stages and the message thread clears them on a
 * track change. Two load-then-store writers could both write the same odd value
 * and leave it odd forever, so readers would never succeed. Interleaved writers
 * still cost a torn read, which costs one repaint. */
void ui_publish(int stage, int percent, int queue_position)
{
    __atomic_add_fetch(&g_ui.gen, 1, __ATOMIC_RELAXED);      /* -> odd */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    g_ui.stage = stage;
    g_ui.percent = percent;
    g_ui.queue_position = queue_position;
    g_ui.via_server = g_job_via_server;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_add_fetch(&g_ui.gen, 1, __ATOMIC_RELAXED);      /* -> even */
}

/* Whether the last published status was a usable server, and whether it has
 * just become one and nobody has acted on it yet.
 *
 * The edge is taken at the publish, not where a status is read, because "not
 * usable" is published from four places: no sidecar, a HELLO that could not be
 * sent, a dropped link, and the sidecar's own STATUS. All four must clear the
 * state or a returning server (e.g. stemd restarted under a loaded track)
 * produces no edge and nothing is re-requested.
 *
 * Separator thread only, like every caller of ui_publish_status. */
static int  g_status_ok;
static int  g_server_up_edge;

void ui_publish_status(int reachable, int compatible)
{
    int ok = reachable && compatible;

    if (ok && !g_status_ok)
        g_server_up_edge = 1;
    g_status_ok = ok;

    __atomic_add_fetch(&g_ui.gen, 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    g_ui.reachable = reachable;
    g_ui.compatible = compatible;
    g_ui.status_seen = 1;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_add_fetch(&g_ui.gen, 1, __ATOMIC_RELAXED);
}

void stem_progress_set(int stage, int percent)
{
    ui_publish(stage, percent, 0);
}

int stem_ui_read(struct stem_ui_state *out)
{
    int spins;

    for (spins = 0; spins < 8; spins++) {
        uint32_t before = __atomic_load_n(&g_ui.gen, __ATOMIC_RELAXED);
        uint32_t after;

        if (before & 1u)
            continue;                    /* writer mid-update */
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        *out = g_ui;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        after = __atomic_load_n(&g_ui.gen, __ATOMIC_RELAXED);
        if (before == after)
            return before != 0;
    }
    /* The writer is busy: skip this frame instead of spinning on a thread that
     * must not block. */
    memset(out, 0, sizeof(*out));
    return 0;
}

/* ---- worker state --------------------------------------------------------- */

/* See job_internal.h. */
#define JOB_RETRY_SEC 5

/* How long the deck's loader must be quiet before a separation starts pushing
 * PCM, and the longest wait for that. Wide because the wait costs nothing on
 * this path (see run_separation). */
#define SEP_QUIET_MS       1500
#define SEP_SETTLE_MAX_MS 45000

void job_retry_later(void);

/* A job died, so the server's state is in doubt: the next loop turn re-probes
 * instead of waiting out the refresh interval.
 *
 * A finished job defers the periodic probe, since it proves the server is up.
 * Failed jobs must not: with a retry every JOB_RETRY_SEC and a probe every
 * STATUS_REFRESH_SEC, the probe would never fall due and the status line would
 * keep showing a server that went away mid-job. */
volatile int  g_probe_now;

static pthread_t     g_loader, g_separator;
static int           g_worker_up;
volatile uint64_t g_retry_at; /* the loader may not re-probe before this */
volatile int  g_quit;         /* process is going away */
volatile int  g_resettle;     /* a stem setting moved: re-HELLO now */

/* ---- the current track: written by the message thread, read by the loader ---
 *
 * A generation, so the loader cannot miss two changes within one tick or
 * mistake "changed back" for "never changed". */
char              g_cur_path[STEM_CACHE_PATH_MAX];
volatile uint32_t g_cur_gen;

/* ---- loader -> separator ---------------------------------------------------
 *
 * Requested only on a cache miss. A track whose stems are already on the media
 * never gets here, so switching to one cannot cancel a running separation. */
static char              g_want_path[STEM_CACHE_PATH_MAX];
static int64_t           g_want_frames;
volatile uint32_t g_want_gen;
volatile int      g_sep_supersede;   /* the job in flight is unwanted */
volatile int      g_sep_busy;        /* a separation is running now */

/* ---- separator -> loader ---------------------------------------------------
 *
 * Paths only, never memory. set_retire spins for in-flight audio readers and
 * must never race a publish, which is guaranteed by the loader being the only
 * thread that touches g_set. */
struct job_delivery g_delivery;

/* g_stems_on as of the last settings change, so the master gate's edge can be
 * told from its level. Seeded at install from the setting restored from eMMC,
 * so a deck that boots with STEMS on does not read its first settings change as
 * a switch-on. -1 means not seeded yet. */
int g_stems_was_on = -1;

/* Stems announced by the sidecar but not yet decoded. The two parts arrive as
 * separate frames and the store publishes both at once, so they are held here
 * until the pair is complete. Worker thread only, reset per job. */
struct job_arrived g_arrived[STEM_N_PARTS];

/* The track the job in flight is for. Worker thread only, so unsynchronised:
 * set at the top of run_separation and read when the stems land. A copy,
 * because decode.c's pointer is only stable until the next track load and a job
 * can outlive its track. */
char    g_job_path[STEM_CACHE_PATH_MAX];
int64_t g_job_frames;


/* ---- the seam between the two workers ------------------------------------- */

/* Whether `path` is the track currently loaded; the relevance test both
 * workers use. */
int track_is_current(const char *path)
{
    return path && path[0] && strcmp(path, g_cur_path) == 0;
}

/* Publish the job's own stage only if its track is still loaded: a stage
 * belongs to the loaded track. A separation deliberately outlives the track
 * that asked for it (it lands in the cache), so it can run for minutes after the
 * DJ has moved on. Every publisher applies this test: sep_progress, job_failed,
 * the delivery and the cache hit. */
void job_progress(int stage, int pct)
{
    if (track_is_current(g_job_path))
        ui_publish(stage, pct, 0);
}

/* A job that died on the wire: publish FAILED and schedule a retry.
 *
 * The most common cause is transient: the sidecar stops reading this socket
 * while it finishes the previous job (a cancel does not drop what it already
 * handed to the server), so the next upload fills the buffer and dies. Example
 * timeline: cancel at 10:20:29, next job 10:20:39, FAILED 10:20:43, sidecar
 * drains at 10:20:50. A permanent cause fails once per JOB_RETRY_SEC, visibly
 * in the log.
 *
 * The stage and the retry are skipped if the job's track is no longer loaded:
 * FAILED would paint over the new track's IDLE, and the retry would re-serve
 * the new track, which the loader has already handled, decoding a cached pair
 * twice.
 *
 * The probe is unconditional, since the server is in doubt regardless of which
 * track the job was for. */
void job_failed(void)
{
    int mine = track_is_current(g_job_path);

    /* Re-probe: after a failure the cached status is least likely to be true. */
    g_probe_now = 1;

    if (!mine) {
        MDBG("stem_job: %s failed, but is no longer loaded -> nothing to say\n",
             g_job_path);
        return;
    }
    ui_publish(STEM_STAGE_FAILED, 0, 0);
    MDBG("stem_job: failed -> another attempt in %d s\n", JOB_RETRY_SEC);
    job_retry_later();
}

/* Loader -> separator: this track needs the server.
 *
 * Reached only from a cache miss. A new request supersedes whatever is in
 * flight; this is the only place a separation is abandoned. */
void sep_request(const char *path, int64_t frames)
{
    if (!g_worker_up)
        return;
    /* Re-requesting a separation that is still running (leaving a track and
     * coming back) would restart it from zero, so skip it. A request that is no
     * longer running is spent and asking again is the retry, so the path alone
     * must not gate it or job_failed's reschedule would do nothing. */
    if (strcmp(path, g_want_path) == 0 && g_want_gen &&
        __atomic_load_n(&g_sep_busy, __ATOMIC_ACQUIRE))
        return;
    /* Set at request time, not when the upload starts: asking is what decides
     * that the run goes through the server and the bar is divided. */
    g_job_via_server = 1;
    snprintf(g_want_path, sizeof(g_want_path), "%s", path);
    g_want_frames = frames;
    __atomic_store_n(&g_sep_supersede, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_want_gen, g_want_gen + 1, __ATOMIC_RELEASE);
    MDBG("stem_job: %s needs the server -> asking the separator\n", path);
}

/* The separator's job: upload the requested track and see it through. Tied to
 * a path, not to whatever is loaded: a track change does not cancel it, only a
 * supersede does. */

void run_separation(void)
{
    const char *path = g_want_path;
    struct stem_job_begin begin;
    struct upload_ctx ctx;
    int64_t frames = g_want_frames;
    int fresh;

    if (!path[0] || frames <= 0)
        return;

    /* Let the deck finish loading before a full-track decode and a 171 MB
     * upload compete with it. The cache path uses the shortest quiet it can,
     * since that wait is latency the DJ sees; here the job will spend tens of
     * seconds on the server anyway, so a wide window costs nothing.
     *
     * A timeout is not fatal: a deck still opening readers after
     * SEP_SETTLE_MAX_MS is doing something other than the load, and refusing to
     * separate would strand the track. */
    stem_decode_wait_deck_quiet(SEP_QUIET_MS, SEP_SETTLE_MAX_MS);
    if (!track_is_current(path))
        return;                  /* the DJ moved on while we deferred */

    snprintf(g_job_path, sizeof(g_job_path), "%s", path);
    g_job_frames = frames;

    fresh = stem_ipc_ensure();
    if (fresh < 0) {
        /* Go through job_failed so a retry is scheduled: the loader marked the
         * track served when it asked, and only a retry or a track change brings
         * it back. stem_ipc_ensure fails for transient reasons, typically the
         * sidecar restarting under systemd. */
        ui_publish_status(0, 0);
        job_failed();            /* stem_ipc_ensure logged why */
        return;
    }

    /* On a new connection, send no PCM until the sidecar reports ready.
     *
     * A HELLO makes the sidecar probe the server, blocking for about a second
     * without reading this socket. The decoder runs at over 200x realtime and
     * fills the socket buffer in that window (28672 frames, 229 KB), killing the
     * upload. Waiting for the STATUS after the probe avoids that, and avoids
     * pushing 171 MB at an unreachable server.
     *
     * A reused connection sent no HELLO, so no STATUS will come; waiting there
     * would hang the job until the timeout. */
    if (fresh && await_sidecar_ready() != 0) {
        /* Go through job_failed: "server not usable" must be shown (it puts the
         * ! on the button) and retried, since the server may come back. A bare
         * return would leave the stage at IDLE with nothing scheduled, and
         * stems_available() would keep reporting success. */
        job_failed();
        return;
    }

    /* A reused connection gets no STATUS, so the last status received decides
     * whether to upload. Without this check, each retry against a down server
     * would decode the track and push 171 MB, once per JOB_RETRY_SEC, only to get
     * JOB_FAILED at the end.
     *
     * job_failed re-arms the probe, so a stale status costs one retry interval:
     * the next idle turn asks the sidecar again. */
    if (!fresh && !g_status_ok) {
        MDBG("stem_job: last status says no usable server -> not uploading %s\n", path);
        job_failed();
        return;
    }

    MDBG("stem_job: starting for %s\n", path);

    memset(&begin, 0, sizeof(begin));
    begin.frames = (uint64_t)frames;
    begin.sample_rate = 44100;
    begin.channels = 2;
    begin.output_sample_rate = sep_output_rate(path);
    /* FLAC, because the result is written to the DJ's media: lossless so the
     * reconstruction stays exact, and about a third of the raw size (a sparse
     * vocals stem compresses further). The deck's own FileReadFlac opens it through the same
     * createReaderFor path as a WAV. */
    begin.output_format = STEM_PCM_FLAC;
    if (stem_ipc_send(STEM_MSG_JOB_BEGIN, &begin, sizeof(begin)) != 0) {
        /* Same handling as every other failed send on this path. */
        stem_ipc_close();
        job_failed();
        return;
    }

    ctx.sent = 0;
    ctx.total = frames;
    ctx.cancelled = 0;
    job_progress(STEM_STAGE_UPLOADING, 0);
    if (stem_decode_pull(path, STEM_UPLOAD_RATE, upload_chunk, &ctx) < 0) {
        /* Send CANCEL instead of dropping the socket. A dropped socket makes the
         * sidecar treat the short stream as broken, answer JOB_FAILED into a
         * closed socket and tear the session down, so the next track pays for a
         * reconnect and HELLO probe. Its upload pull expects CANCEL (err 2,
         * "cancelled by the deck") and keeps the connection up.
         *
         * Nothing to DELETE here: the POST never completed, so there is no job
         * id. The poll loop below handles that case with the same frame. */
        if (ctx.cancelled) {
            stem_ipc_send(STEM_MSG_CANCEL, NULL, 0);
            return;
        }
        stem_ipc_close();
        job_failed();
        return;
    }

    /* Pad to the length JOB_BEGIN promised.
     *
     * That byte count becomes the POST's Content-Length and comes from the
     * converter's out_len, which the decode may not match (e.g. a mono WAV
     * delivers short). A short body makes the sidecar hit JOB_END with bytes
     * still owed and the POST fails as `pull err 1`. The shortfall is at the
     * tail, so silence is the right filler; the true length is only known after
     * the decode has been streamed. */
    if (ctx.sent < frames) {
        static const float k_silence[JOB_PCM_FRAMES * 2];
        int64_t missing = frames - ctx.sent;

        MDBG("stem_job: decode gave %lld of %lld frames, padding %lld\n",
             (long long)ctx.sent, (long long)frames, (long long)missing);
        while (missing > 0) {
            int64_t n = missing > JOB_PCM_FRAMES ? JOB_PCM_FRAMES : missing;

            if (stem_ipc_send(STEM_MSG_PCM, k_silence,
                              (uint32_t)(n * 2 * sizeof(float))) != 0) {
                stem_ipc_close();
                job_failed();
                return;
            }
            missing -= n;
        }
    }

    if (stem_ipc_send(STEM_MSG_JOB_END, NULL, 0) != 0) {
        stem_ipc_close();
        job_failed();
        return;
    }

    /* Wait for the separation, with a silence budget instead of a total
     * deadline: the sidecar polls the server every 500 ms and forwards a
     * PROGRESS frame each time, so a live job is never quiet for long while a
     * wedged one is silent. A total-time cap would kill long separations. */
    {
        uint64_t quiet_since = job_now_sec();

        for (;;) {
            static char frame[4096];
            uint32_t type = 0, len = 0;
            int rc;

            if (__atomic_load_n(&g_sep_supersede, __ATOMIC_ACQUIRE) || g_quit) {
                stem_ipc_send(STEM_MSG_CANCEL, NULL, 0);
                return;
            }
            rc = stem_ipc_recv(&type, frame, sizeof(frame), &len,
                               JOB_RECV_TIMEOUT_MS);
            if (rc < 0) {
                stem_ipc_close();
                job_failed();
                return;
            }
            if (rc == 0) {
                if (job_now_sec() - quiet_since < JOB_SILENCE_SEC)
                    continue;
                MDBG("stem_job: sidecar silent for %d s -> abandoning the job\n",
                     JOB_SILENCE_SEC);
                stem_ipc_send(STEM_MSG_CANCEL, NULL, 0);
                stem_ipc_close();
                job_failed();
                return;
            }
            quiet_since = job_now_sec();
            if (handle_frame(type, frame, len))
                return;
        }
    }
}

#define STATUS_REPROBE_MS    3000

/* CLOCK_MONOTONIC: the shim time-shifts gettimeofday for EP122, and a
 * wall-clock jump must not stall the health check for hours. */
uint64_t job_now_sec(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec;
}

/* Look at the current track again after JOB_RETRY_SEC. The loader re-examines
 * whatever is loaded when the delay falls due, and a track change in the
 * meantime clears the delay. */
void job_retry_later(void)
{
    g_retry_at = job_now_sec() + JOB_RETRY_SEC;
}

/* Restart a job that was given up for lack of a server once one appears.
 *
 * When run_separation returns because stem_ipc_ensure or await_sidecar_ready
 * found no server, the request has been consumed, so without this a track
 * loaded while the server was down is never separated.
 *
 * Triggered by the server's arrival rather than a timer, so nothing is spent
 * and the length probe is not re-run while the server is absent. Latency is up
 * to STATUS_REFRESH_SEC, or immediate when a press requests a probe
 * (stem_job_probe_now).
 *
 * Idle branch only, where nothing is in flight. The separator calls it after
 * refresh_status, because three of refresh_status's four exits return early. */
void server_arrived(void)
{
    /* An edge latched at the publish; a level would re-request on every refresh
     * for any track without stems. Consumed even if nothing is done with it, so
     * it cannot fire on a later unrelated idle turn. */
    if (!g_server_up_edge)
        return;
    g_server_up_edge = 0;
    if (!g_stems_on)
        return;
    /* A track must be loaded and still without stems; stem_store_complete tells
     * whether stems actually landed. */
    if (!g_cur_path[0] || stem_store_complete())
        return;
    MDBG("stem_job: a server appeared and %s has no stems -> asking again\n",
         g_cur_path);
    stem_job_request();
}

void refresh_status(void)
{
    static char frame[4096];
    int fresh = stem_ipc_ensure();
    int waited;

    if (fresh < 0) {
        /* No sidecar counts as unreachable. */
        ui_publish_status(0, 0);
        return;
    }
    if (fresh == 0 && stem_ipc_hello() != 0) {
        stem_ipc_close();
        ui_publish_status(0, 0);
        return;
    }

    for (waited = 0; waited < STATUS_REPROBE_MS; waited += JOB_RECV_TIMEOUT_MS) {
        uint32_t type = 0, len = 0;
        int rc;

        if (__atomic_load_n(&g_sep_supersede, __ATOMIC_ACQUIRE) || g_quit)
            return;                  /* real work outranks a health check */
        rc = stem_ipc_recv(&type, frame, sizeof(frame), &len,
                           JOB_RECV_TIMEOUT_MS);
        if (rc < 0) {
            stem_ipc_close();
            ui_publish_status(0, 0);
            return;
        }
        if (rc == 0)
            continue;
        /* STATUS only: a late STEM_READY from a cancelled job would otherwise
         * publish the previous track's stems over the loaded one. */
        if (type != STEM_MSG_STATUS)
            continue;
        handle_frame(type, frame, len);
        return;
    }
}

/* ---- the loader: owns the resident stems and the UI snapshot --------------
 *
 * Always works on the current track and abandons its work when that changes.
 * It is the only thread that touches g_set. */

/* The generation the running publish belongs to, and whether one is running.
 * Globals rather than parameters because the store's two decode threads check
 * them per chunk, where only a plain atomic compare is cheap enough. */
volatile uint32_t g_load_gen;
volatile int      g_load_armed;

int stem_job_load_wanted(void)
{
    if (!__atomic_load_n(&g_load_armed, __ATOMIC_ACQUIRE))
        return 1;                    /* not a publish the loader started */
    return __atomic_load_n(&g_load_gen, __ATOMIC_ACQUIRE) ==
           __atomic_load_n(&g_cur_gen, __ATOMIC_ACQUIRE);
}

/* ---- entry points --------------------------------------------------------- */

/* The message thread naming the track that is now loaded.
 *
 * The only place the current track changes. It only clears readiness, so the
 * audio thread is on the stock path before the next block; freeing, probing and
 * requesting are left to the loader, which sees the new generation. */
void stem_job_set_track(const char *path)
{
    if (path && path[0] && strcmp(path, g_cur_path) == 0)
        return;
    __atomic_store_n(&g_stem_ready, 0, __ATOMIC_RELEASE);
    /* The groove circuit's phase and replacement refer to the old track's
     * timeline and stems; carried over, it would land somewhere arbitrary in
     * the new one. */
    gc_disarm();
    stem_grid_forget();
    snprintf(g_cur_path, sizeof(g_cur_path), "%s", path ? path : "");
    __atomic_store_n(&g_cur_gen, g_cur_gen + 1, __ATOMIC_RELEASE);
    /* Blank the row now: the loader only checks the generation between jobs,
     * and a separation can take minutes. Published after g_cur_path changes, so
     * the running job's job_progress cannot restore its stage. */
    ui_publish(STEM_STAGE_IDLE, 0, 0);
}

/* Ask the sidecar what it can see at the next idle turn instead of waiting up
 * to the 30 s refresh interval; used when the DJ presses STEMS.
 *
 * Only sets a flag: the socket belongs to the worker and this runs on the
 * message thread. The separator picks it up between jobs, so a press during a
 * separation is honoured when the job ends. */
void stem_job_probe_now(void)
{
    if (!g_worker_up)
        return;
    g_probe_now = 1;
}

/* Ask for the loaded track to be looked at again.
 *
 * The loader decides whether a job is needed; a cached track needs no server.
 * Used by the settings gate and when a server appears. */
void stem_job_request(void)
{
    if (!g_worker_up)
        return;
    g_retry_at = 0;
    __atomic_store_n(&g_cur_gen, g_cur_gen + 1, __ATOMIC_RELEASE);
}

/* Drop the stems held for the track that has gone.
 *
 * This does not cancel the separation: it still finishes and lands in the
 * media cache for when the track comes back. Only another track needing the
 * server abandons one (sep_request). */
void stem_track_gone(void)
{
    stem_job_set_track(NULL);
}


/* ---- the feature's MOD SETTINGS rows --------------------------------------
 *
 * Declared here because they configure the sidecar link, which
 * mods_stem_settings_changed pushes them to. STEM SERVER LOCATION hangs off
 * ENABLE STEMS and the address off MANUAL, so no row can be edited into a state
 * nothing reads. [message] for the callbacks; registration is [init]. */
static const char *const k_stem_where[2] = { "AUTO", "MANUAL" };

static const struct kit_row k_rows[] = {
    KIT_ROW_BOOL("ENABLE STEMS", &g_stems_on,
                 .idx = KIT_IDX_STEMS, .changed = mods_stem_settings_changed),
    { .label = "STEM SERVER LOCATION", .idx = 0, .parent = &k_rows[0], .show_when = 1,
      .state = &g_stem_manual, .values = k_stem_where, .nvalues = 2,
      .changed = mods_stem_settings_changed },
    { .label = "STEM SERVER ADDRESS", .idx = 0, .parent = &k_rows[1], .show_when = 1,
      .text = g_stem_addr, .text_cap = STEM_ADDR_MAX, .changed = addr_changed },
};

void stem_job_poll(void)
{
    /* Message thread. Must not touch the socket (see stem.h). The widget
     * update is in ui.c, which owns every Component. */
}

static int stem_job_install(void)
{
    if (pthread_create(&g_separator, NULL, separator_main, NULL) != 0 ||
        pthread_create(&g_loader, NULL, loader_main, NULL) != 0) {
        MERR("stem_job: worker thread failed to start; STEMS unavailable\n");
        return -1;
    }
    pthread_detach(g_separator);
    pthread_detach(g_loader);
    g_worker_up = 1;
    /* Runs after mods_settings_load(), so this is the saved setting, not the
     * compiled default. */
    g_stems_was_on = g_stems_on ? 1 : 0;
    kit_menu_add(k_rows, (int)(sizeof(k_rows) / sizeof(k_rows[0])));
    MDBG("stem_job: worker thread up (STEMS %s)\n",
         g_stems_was_on ? "on" : "off");
    return 0;
}

KIT_MOD(k_mod_stem_job,
        .name = "stem_job", .prio = 60, .install = stem_job_install,
        .what = "the worker thread that owns the sidecar link");
