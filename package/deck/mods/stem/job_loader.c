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
    int rc;

    /* The DECODER's frame count, which is half the cache key: stems are aligned
     * to EP122's padded decode, so a firmware that pads differently must miss
     * rather than load something silently misaligned. */
    frames = stem_decode_pull(path, STEM_UPLOAD_RATE, NULL, NULL);
    if (frames <= 0) {
        MDBG("stem_job: length probe failed (%lld)\n", (long long)frames);
        job_retry_later();
        return;
    }
    if (!track_is_current(path))
        return;                     /* the DJ moved on while we probed */

    if (stem_cache_lookup(path, frames, &e) != 0) {
        /* A miss, and the ONLY route to the separator. */
        sep_request(path, frames);
        return;
    }

    /* Off the media: no upload and no server, so LOADING is the whole of this run. */
    g_job_via_server = 0;
    ui_publish(STEM_STAGE_LOADING, 0, 0);
    rc = loader_publish(e.harmonics_path, e.harmonics_gain,
                        e.vocals_path, e.vocals_gain);
    if (rc == STEM_PUBLISH_ABORT)
        return;                     /* the new track's generation drives us now */
    if (rc == STEM_PUBLISH_RETRY) {
        MDBG("stem_job: cache hit not loadable yet, retrying in %d s\n",
             JOB_RETRY_SEC);
        job_retry_later();
        return;
    }
    if (rc != STEM_PUBLISH_OK) {
        MDBG("stem_job: cached pair will not decode, re-separating\n");
        sep_request(path, frames);
        return;
    }
    if (!track_is_current(path)) {
        /* Loaded, but for a track that is no longer on screen. Drop it rather
         * than let the audio thread mix another song's stems. */
        stem_store_release_all();
        return;
    }
    __atomic_store_n(&g_stem_ready, 1, __ATOMIC_RELEASE);
    ui_publish(STEM_STAGE_DONE, 100, 0);
    wave_stems_track_ready(path);
    MDBG("stem_job: served from cache, no server needed\n");
}

/* A delivery from the separator, taken only if it is still wanted. */
static void loader_take_delivery(void)
{
    static uint32_t seen;
    char track[STEM_CACHE_PATH_MAX], h[STEM_CACHE_PATH_MAX], v[STEM_CACHE_PATH_MAX];
    float hg, vg;
    int tmpfs;
    uint32_t gen = __atomic_load_n(&g_delivery.gen, __ATOMIC_ACQUIRE);

    if (gen == seen || (gen & 1u))
        return;
    seen = gen;
    snprintf(track, sizeof(track), "%s", g_delivery.track);
    snprintf(h, sizeof(h), "%s", g_delivery.h);
    snprintf(v, sizeof(v), "%s", g_delivery.v);
    hg = g_delivery.hg;
    vg = g_delivery.vg;
    tmpfs = g_delivery.tmpfs;

    if (track_is_current(track)) {
        ui_publish(STEM_STAGE_LOADING, 0, 0);
        if (loader_publish(h, hg, v, vg) == STEM_PUBLISH_OK &&
            track_is_current(track)) {
            __atomic_store_n(&g_stem_ready, 1, __ATOMIC_RELEASE);
            ui_publish(STEM_STAGE_DONE, 100, 0);
            wave_stems_track_ready(track);
        }
    } else {
        MDBG("stem_job: %s finished separating, but is no longer loaded --"
             " it is in the cache for when it is\n", track);
    }
    /* A pair still on tmpfs -- one the media would not take -- is pure
     * duplication from here: publish has it in RAM. The media's own copy is the
     * cache entry and stays. */
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
            /* g_stem_ready was cleared by the message thread the instant the
             * track moved, so the audio thread is already back on the stock
             * path; this is only the freeing, which spins for in-flight
             * readers and therefore cannot run there. */
            stem_store_release_all();
            memset(g_arrived, 0, sizeof(g_arrived));
            ui_publish(STEM_STAGE_IDLE, 0, 0);
            g_retry_at = 0;
        }

        loader_take_delivery();

        /* A scheduled retry OUTRANKS `served`. Requiring both meant the flag
         * won every time -- it is set the moment the separator is asked, which
         * is before anything can fail -- so job_failed's reschedule was never
         * acted on and a job that died on the wire stayed dead. */
        if (g_stems_on && g_cur_path[0] &&
            (g_retry_at ? job_now_sec() >= g_retry_at : !served)) {
            char path[STEM_CACHE_PATH_MAX];

            g_retry_at = 0;
            snprintf(path, sizeof(path), "%s", g_cur_path);
            loader_serve(path);
            /* Served, asked, or scheduled -- either way do not re-probe until
             * the track changes or a retry falls due. */
            served = (g_retry_at == 0);
        }
        /* The stick's own slot files, rescanned when the volume or the pool
         * rate moves. Cheap otherwise, and this is the one thread allowed to
         * decode. */
        mod_stem_gc_poll();
        /* The X-PAD's sample banks, on the same terms and for the same reason. */
        xpad_bank_poll();
        /* Not a stem concern, but this is the shim's only idle worker and the
         * question it answers -- which database the deck handed us -- is one to
         * have before anything writes rather than after. */
        mod_db_poll();
        mod_djdb_poll();
        usleep(100 * 1000);
    }
    return NULL;
}
