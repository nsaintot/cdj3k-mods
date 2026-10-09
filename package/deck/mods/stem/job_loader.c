// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * mods/stem/job_loader.c - handing decoded stems to the audio side as the deck asks for them.
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

/* A RETRY from the store is re-run every JOB_RETRY_SEC up to this many times,
 * then the track is marked FAILED: a pair that does not fit in memory, or a
 * track the deck will not size, will not start working. Reset on a track
 * change. Loader thread only. */
#define LOAD_RETRY_MAX 12
static int g_load_retries;

static int load_retry(const char *why)
{
    if (++g_load_retries > LOAD_RETRY_MAX) {
        MDBG("stem_job: %s after %d attempts -> giving up on this track\n",
             why, LOAD_RETRY_MAX);
        ui_publish(STEM_STAGE_FAILED, 0, 0);
        g_retry_at = 0;             /* or the probe runs once more and gives up again */
        return 0;
    }
    MDBG("stem_job: %s, retrying in %d s (%d/%d)\n", why, JOB_RETRY_SEC,
         g_load_retries, LOAD_RETRY_MAX);
    job_retry_later();
    return 1;
}

/* The decoder's length for a track, from the last probe that succeeded.
 *
 * A VBR MP3 can only be sized through the deck's own tables, which are held for
 * the track the deck opened last. A track re-served from the page pool is not
 * reopened, so its probe fails, but the remembered length still finds the pair
 * in the cache. Loader thread only. */
#define LEN_MEMO 16
static struct {
    char    path[STEM_CACHE_PATH_MAX];
    int64_t frames;
} g_len[LEN_MEMO];
static int g_len_next;

static int64_t len_known(const char *path)
{
    int i;

    for (i = 0; i < LEN_MEMO; i++)
        if (g_len[i].frames > 0 && strcmp(g_len[i].path, path) == 0)
            return g_len[i].frames;
    return -1;
}

static void len_remember(const char *path, int64_t frames)
{
    int i;

    for (i = 0; i < LEN_MEMO; i++)
        if (g_len[i].frames > 0 && strcmp(g_len[i].path, path) == 0) {
            g_len[i].frames = frames;
            return;
        }
    i = g_len_next++ % LEN_MEMO;
    snprintf(g_len[i].path, sizeof(g_len[i].path), "%s", path);
    g_len[i].frames = frames;
}

/* Publish, abortable by a track change: the generation is latched here and
 * compared inside the store, since a publish takes seconds of decode per stem. */
static int loader_publish(const char *h, float hg, const char *v, float vg)
{
    int rc;

    __atomic_store_n(&g_load_gen,
                     __atomic_load_n(&g_cur_gen, __ATOMIC_ACQUIRE),
                     __ATOMIC_RELEASE);
    __atomic_store_n(&g_load_armed, 1, __ATOMIC_RELEASE);
    rc = stem_store_publish(h, hg, v, vg);
    __atomic_store_n(&g_load_armed, 0, __ATOMIC_RELEASE);
    return rc;
}

static void loader_serve(const char *path)
{
    struct stem_cache_entry e;
    int64_t frames;
    int rc, remembered = 0;

    /* The decoder's frame count is half the cache key: stems are aligned to
     * EP122's padded decode, so a firmware that pads differently must miss
     * instead of loading a misaligned pair. */
    frames = stem_decode_pull(path, STEM_UPLOAD_RATE, NULL, NULL);
    if (frames > 0) {
        len_remember(path, frames);
    } else {
        frames = len_known(path);
        remembered = frames > 0;
        if (!remembered) {
            load_retry("length probe failed");
            return;
        }
        MDBG("stem_job: length probe failed, using the earlier %lld\n",
             (long long)frames);
    }
    if (!track_is_current(path))
        return;                     /* the DJ moved on while we probed */

    if (stem_cache_lookup(path, frames, &e) != 0) {
        /* A remembered length finds a pair; it cannot decode a track the deck
         * will not size, so there is nothing to upload. */
        if (remembered) {
            load_retry("no pair on the media and the track cannot be decoded");
            return;
        }
        /* A miss: the only route to the separator. */
        sep_request(path, frames);
        return;
    }

    /* From the media: no upload or server, so LOADING is the only stage. */
    g_job_via_server = 0;
    ui_publish(STEM_STAGE_LOADING, 0, 0);
    rc = loader_publish(e.harmonics_path, e.harmonics_gain,
                        e.vocals_path, e.vocals_gain);
    if (rc == STEM_PUBLISH_ABORT)
        return;                     /* the new track's generation drives us now */
    if (rc == STEM_PUBLISH_RETRY) {
        load_retry("cache hit not loadable yet");
        return;
    }
    if (rc != STEM_PUBLISH_OK) {
        MDBG("stem_job: cached pair will not decode, re-separating\n");
        sep_request(path, frames);
        return;
    }
    if (!track_is_current(path)) {
        /* Loaded for a track that is no longer current: drop it so the audio
         * thread does not mix another song's stems. */
        stem_store_release_all();
        return;
    }
    __atomic_store_n(&g_stem_ready, 1, __ATOMIC_RELEASE);
    ui_publish(STEM_STAGE_DONE, 100, 0);
    wave_stems_track_ready(path);
    MDBG("stem_job: served from cache, no server needed\n");
}

/* The delivery the loader has not consumed yet, if any. */
static uint32_t g_delivery_seen;

static int loader_delivery_pending(void)
{
    uint32_t gen = __atomic_load_n(&g_delivery.gen, __ATOMIC_ACQUIRE);

    return gen != g_delivery_seen && !(gen & 1u);
}

/* Take a delivery from the separator if it is still wanted. A pair the store
 * answers RETRY to stays pending (tmpfs copy included) and is taken again when
 * the retry falls due. */
static void loader_take_delivery(void)
{
    char track[STEM_CACHE_PATH_MAX], h[STEM_CACHE_PATH_MAX], v[STEM_CACHE_PATH_MAX];
    float hg, vg;
    int tmpfs;
    uint32_t gen = __atomic_load_n(&g_delivery.gen, __ATOMIC_ACQUIRE);

    if (gen == g_delivery_seen || (gen & 1u))
        return;
    if (g_retry_at && job_now_sec() < g_retry_at)
        return;                     /* a RETRY is waiting its turn */
    snprintf(track, sizeof(track), "%s", g_delivery.track);
    snprintf(h, sizeof(h), "%s", g_delivery.h);
    snprintf(v, sizeof(v), "%s", g_delivery.v);
    hg = g_delivery.hg;
    vg = g_delivery.vg;
    tmpfs = g_delivery.tmpfs;

    if (track_is_current(track)) {
        int rc;

        ui_publish(STEM_STAGE_LOADING, 0, 0);
        rc = loader_publish(h, hg, v, vg);
        if (rc == STEM_PUBLISH_RETRY &&
            load_retry("the separated pair cannot be loaded yet"))
            return;                 /* still pending */
        if (rc == STEM_PUBLISH_OK && track_is_current(track)) {
            __atomic_store_n(&g_stem_ready, 1, __ATOMIC_RELEASE);
            ui_publish(STEM_STAGE_DONE, 100, 0);
            wave_stems_track_ready(track);
        }
    } else {
        MDBG("stem_job: %s finished separating, but is no longer loaded --"
             " it is in the cache for when it is\n", track);
    }
    g_delivery_seen = gen;
    /* A pair on tmpfs (the media would not take it) is no longer needed:
     * publish has it in RAM or gave up on it. A media copy is the cache entry
     * and stays. */
    if (tmpfs) {
        unlink(h);
        unlink(v);
    }
}

void * loader_main(void *arg)
{
    uint32_t seen_gen = 0;
    int served = 0;

    (void)arg;
    while (!g_quit) {
        uint32_t gen = __atomic_load_n(&g_cur_gen, __ATOMIC_ACQUIRE);

        if (gen != seen_gen) {
            seen_gen = gen;
            served = 0;
            /* The message thread already cleared g_stem_ready, so the audio
             * thread is on the stock path. Freeing spins for in-flight readers,
             * so it cannot run on the message thread. */
            stem_store_release_all();
            memset(g_arrived, 0, sizeof(g_arrived));
            ui_publish(STEM_STAGE_IDLE, 0, 0);
            g_retry_at = 0;
            g_load_retries = 0;
        }

        loader_take_delivery();

        /* A scheduled retry overrides `served`, which is set as soon as the
         * separator is asked, before anything can fail; otherwise job_failed's
         * reschedule would never run. A pending delivery owns the retry it
         * scheduled; probing would find the same pair and load it twice. */
        if (!loader_delivery_pending() && g_stems_on && g_cur_path[0] &&
            (g_retry_at ? job_now_sec() >= g_retry_at : !served)) {
            char path[STEM_CACHE_PATH_MAX];

            g_retry_at = 0;
            snprintf(path, sizeof(path), "%s", g_cur_path);
            loader_serve(path);
            /* Served, requested or scheduled: do not re-probe until the track
             * changes or a retry falls due. */
            served = (g_retry_at == 0);
        }
        /* The stick's slot files, rescanned when the volume or the pool rate
         * changes. Cheap otherwise; this is the one thread allowed to decode. */
        mod_stem_gc_poll();
        /* The X-PAD's sample banks, likewise. */
        xpad_bank_poll();
        /* Not stem-related, but this is the shim's only idle worker, and which
         * database the deck handed us must be known before anything writes. */
        mod_db_poll();
        mod_djdb_poll();
        usleep(100 * 1000);
    }
    return NULL;
}
