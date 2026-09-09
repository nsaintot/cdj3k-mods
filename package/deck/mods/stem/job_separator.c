// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * mods/stem/job_separator.c - talking to the stemd sidecar: upload, progress frames and delivery.
 */
#include "stem/job_internal.h"
#include "core/mod_settings.h"
#include "wave/wave.h"
#include "db/db.h"
#include "xpad/ext.h"
#include "kit/menu.h"
#include "kit/mod.h"
#include "kit/popup.h"
#include <pthread.h>

static void sep_deliver(const char *track, const char *h, float hg,
                        const char *v, float vg, int tmpfs)
{
    uint32_t gen = g_delivery.gen;

    __atomic_store_n(&g_delivery.gen, gen + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    snprintf(g_delivery.track, sizeof(g_delivery.track), "%s", track);
    snprintf(g_delivery.h, sizeof(g_delivery.h), "%s", h);
    snprintf(g_delivery.v, sizeof(g_delivery.v), "%s", v);
    g_delivery.hg = hg;
    g_delivery.vg = vg;
    g_delivery.tmpfs = tmpfs;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&g_delivery.gen, gen + 2, __ATOMIC_RELAXED);
    MDBG("stem_job: separated %s -> handed to the loader\n", track);
}

/* Separator -> UI, by way of the loader's judgement about relevance. */
static void sep_progress(const char *track, int stage, int pct, int queue)
{
    if (track_is_current(track))
        ui_publish(stage, pct, queue);
}

int upload_chunk(const float *pcm, int64_t frames, void *user)
{
    struct upload_ctx *ctx = user;

    /* Two very different reasons to stop, and telling them apart matters: a
     * cancel is the deck doing its job, a send failure is ours. */
    if (__atomic_load_n(&g_sep_supersede, __ATOMIC_ACQUIRE) || g_quit) {
        MDBG("stem_job: upload superseded at frame %lld\n", (long long)ctx->sent);
        ctx->cancelled = 1;
        return -1;
    }
    if (stem_ipc_send(STEM_MSG_PCM, pcm, (uint32_t)(frames * 2 * sizeof(float))) != 0)
        return -1;

    ctx->sent += frames;
    /* The STAGE's own progress, 0..100, exactly like every other stage reports. Where
     * that lands on the bar is the UI's problem, not this one's -- see the segment map
     * in ui.c. Publishing a pre-weighted number here is what made a finished upload read
     * as 20% and stalled. */
    if (ctx->total > 0)
        job_progress(STEM_STAGE_UPLOADING,
                     (int)(ctx->sent * 100 / ctx->total));
    return 0;
}

int handle_frame(uint32_t type, const void *buf, uint32_t len)
{
    switch (type) {
    case STEM_MSG_STATUS: {
        const struct stem_status *s = buf;
        char id[STEM_SEP_ID_LEN];

        if (len < sizeof(*s))
            return 0;
        ui_publish_status((int)s->reachable, (int)s->compatible);

        /* Learn the server's identity and keep it across reboots. The cache is
         * for the deck with no server to ask, so this has to survive the server
         * being gone -- a deck that has met one once can play from the stick
         * forever after. Saved only when it changes: this arrives on every
         * connect and the settings file lives on eMMC. */
        snprintf(id, sizeof(id), "%.*s", (int)sizeof(s->sep_id), s->sep_id);
        if (id[0] && strcmp(id, g_stem_sep_id) != 0) {
            MDBG("stem_job: separation id \"%s\" (was \"%s\")\n",
                 id, g_stem_sep_id);
            mods_set_sep_id(id);
            mods_settings_save();
        }
        return 0;
    }
    case STEM_MSG_PROGRESS: {
        const struct stem_progress *p = buf;

        if (len < sizeof(*p))
            return 0;
        /* Attributed to the track it is about. The bar belongs to whatever the
         * DJ is looking at, so a separation they have left must not paint on
         * it -- sep_progress drops it in that case. */
        sep_progress(g_job_path, (int)p->stage, (int)p->percent,
                     (int)p->queue_position);
        return 0;
    }
    case STEM_MSG_STEM_READY: {
        const struct stem_ready *r = buf;
        struct stem_cache_entry e;
        int part, stored;

        if (len < sizeof(*r) || len < sizeof(*r) + r->path_len)
            return 0;
        part = (int)r->part;
        if (part != STEM_PART_HARMONICS && part != STEM_PART_VOCALS)
            return 0;
        if (r->path_len == 0 || r->path_len >= sizeof(g_arrived[0].path))
            return 0;
        memcpy(g_arrived[part].path, r + 1, r->path_len);
        g_arrived[part].path[r->path_len] = '\0';
        g_arrived[part].gain = r->gain;
        g_arrived[part].have = 1;

        /* Both parts, or nothing: they are published together so the audio
         * thread never sees a half-built set. */
        if (!g_arrived[STEM_PART_HARMONICS].have ||
            !g_arrived[STEM_PART_VOCALS].have)
            return 0;

        /* The media cache is the separator's to write -- it is the thread that
         * has fresh stems -- and doing it HERE rather than after the handover is
         * what makes a separation the DJ switched away from still worth having:
         * the entry lands whether or not anyone is waiting for it. The store
         * rule decides whether this volume may hold it at all. */
        stored = stem_cache_store(g_job_path, g_job_frames,
                                  g_arrived[STEM_PART_HARMONICS].path,
                                  g_arrived[STEM_PART_HARMONICS].gain,
                                  g_arrived[STEM_PART_VOCALS].path,
                                  g_arrived[STEM_PART_VOCALS].gain) == 0;

        /* Then hand the loader the paths and let it decide whether they are
         * still wanted. Nothing here touches g_set.
         *
         * THE MEDIA'S COPY, once there is one, and the tmpfs pair goes first.
         * /dev/shm is RAM, and a 9-minute pair is 100 MB of it sitting there
         * for the whole of a 415 MB load -- the difference between the pair
         * fitting and the kernel killing EP122. The loader reads the media at
         * the speed a cache hit already does. */
        if (stored && stem_cache_lookup(g_job_path, g_job_frames, &e) == 0) {
            unlink(g_arrived[STEM_PART_HARMONICS].path);
            unlink(g_arrived[STEM_PART_VOCALS].path);
            sep_deliver(g_job_path, e.harmonics_path, e.harmonics_gain,
                        e.vocals_path, e.vocals_gain, 0);
        } else {
            sep_deliver(g_job_path,
                        g_arrived[STEM_PART_HARMONICS].path,
                        g_arrived[STEM_PART_HARMONICS].gain,
                        g_arrived[STEM_PART_VOCALS].path,
                        g_arrived[STEM_PART_VOCALS].gain, 1);
        }
        return 1;
    }
    case STEM_MSG_JOB_FAILED: {
        const struct stem_failed *f = buf;

        if (len >= sizeof(*f))
            MDBG("stem_job: failed, http=%u\n", f->http_status);
        job_failed();
        return 1;
    }
    default:
        return 0;
    }
}

int await_sidecar_ready(void)
{
    static char frame[4096];
    int waited = 0;

    while (waited < JOB_READY_TIMEOUT_MS) {
        uint32_t type = 0, len = 0;
        int rc;

        if (__atomic_load_n(&g_sep_supersede, __ATOMIC_ACQUIRE) || g_quit)
            return -1;
        rc = stem_ipc_recv(&type, frame, sizeof(frame), &len,
                           JOB_RECV_TIMEOUT_MS);
        if (rc < 0) {
            stem_ipc_close();
            MDBG("stem_job: sidecar dropped the link before it was ready\n");
            return -1;
        }
        if (rc == 0) {
            waited += JOB_RECV_TIMEOUT_MS;
            continue;
        }
        /* Routed through the normal handler so STATUS still teaches us the
         * separation id and still drives the warn icon. */
        handle_frame(type, frame, len);
        if (type != STEM_MSG_STATUS)
            continue;
        {
            const struct stem_status *s = (const struct stem_status *)frame;

            if (len < sizeof(*s) || !s->reachable || !s->compatible) {
                MDBG("stem_job: server not usable (reachable=%u compatible=%u)"
                     " -> no upload\n",
                     len >= sizeof(*s) ? s->reachable : 0,
                     len >= sizeof(*s) ? s->compatible : 0);
                return -1;
            }
        }
        return 0;
    }
    MDBG("stem_job: no STATUS from the sidecar in %d ms -> giving up\n",
         JOB_READY_TIMEOUT_MS);
    return -1;
}

uint32_t sep_output_rate(const char *path)
{
    int waited, rate = stem_pool_rate();

    for (waited = 0; rate <= 0 && waited < SEP_RATE_WAIT_MS;
         waited += SEP_RATE_POLL_MS) {
        /* ASK, rather than wait for the passive figure: the report only runs
         * while the play screen paints, and a track is loaded from BROWSE. */
        rate = stem_engine_rate_measure();
        if (rate > 0)
            break;
        usleep(SEP_RATE_POLL_MS * 1000);
        if (!track_is_current(path))
            return 0;               /* the DJ moved on; the job is about to die */
        rate = stem_pool_rate();
    }

    if (rate <= 0) {
        MDBG("stem_job: pool rate unknown after %d ms, letting the server pick"
             " the rate -- the deck will resample\n", SEP_RATE_WAIT_MS);
        return 0;
    }
    if (waited)
        MDBG("stem_job: pool rate %d Hz after about %d ms\n", rate, waited);
    return (uint32_t)rate;
}

/* ---- the separator: owns the socket and at most one server job ------------ */
void * separator_main(void *arg)
{
    uint32_t seen_want = 0;
    uint64_t next_status = 0;

    (void)arg;
    while (!g_quit) {
        uint32_t want = __atomic_load_n(&g_want_gen, __ATOMIC_ACQUIRE);

        if (want != seen_want) {
            seen_want = want;
            /* Whatever was in flight is now unwanted: the supersede flag has
             * already told the upload and the poll loop to unwind. */
            __atomic_store_n(&g_sep_supersede, 0, __ATOMIC_RELEASE);
            memset(g_arrived, 0, sizeof(g_arrived));
            __atomic_store_n(&g_sep_busy, 1, __ATOMIC_RELEASE);
            run_separation();
            __atomic_store_n(&g_sep_busy, 0, __ATOMIC_RELEASE);
            /* Only a job that got somewhere defers the probe. Deferring on the
             * failures too is what let a retry loop starve its own status. */
            if (!g_probe_now)
                next_status = job_now_sec() + STATUS_REFRESH_SEC;
            continue;
        }
        if (g_resettle || g_probe_now || job_now_sec() >= next_status) {
            if (g_resettle) {
                g_resettle = 0;
                MDBG("stem_job: stem settings changed -> re-settling the sidecar\n");
            }
            g_probe_now = 0;
            next_status = job_now_sec() + STATUS_REFRESH_SEC;
            refresh_status();
            /* AFTER the refresh and unconditionally, because the answer it just
             * published is the one worth acting on -- and because three of that
             * call's four exits never reach its own bottom. */
            server_arrived();
            continue;
        }
        usleep(100 * 1000);
    }
    stem_ipc_close();
    return NULL;
}
