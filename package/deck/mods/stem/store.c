// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * store.c - stem files and their life as page-pool tracks.  [worker]
 *
 * The stems live in our own RAM, resampled by the deck's own converter onto the
 * pool's timeline.
 *
 * The sidecar writes each stem to tmpfs as a WAV: a 44-byte header in front of
 * the s16 body lets audio_format::FileReadWav open it through the
 * createReaderFor path decode.c already uses.
 *
 * The file is opened and immediately unlinked, so the inode survives while a
 * reader holds a descriptor and vanishes when the last one closes: no refcount
 * of ours, and nothing left behind if either process dies mid-job.
 *
 * g_stem_ready is cleared and the audio thread is back on the stock path before
 * any buffer is freed. The order is the caller's to keep -- see job.c.
 */
#include "stem/stem.h"

#include <pthread.h>

/* One decoded stem, on the pool's timeline. `pcm` is interleaved stereo s16:
 * half the memory of float, and the reconstruction is exact anyway because the
 * derived part cancels the quantisation (see stem_mix in audio_mix.c). */
struct stem_buf {
    int16_t *pcm;
    int64_t  frames;
    /* 1.0/gain: turns the stored, normalised samples back into the stem the
     * model produced. Kept here rather than applied to the samples because the
     * result often exceeds full scale and would be clipped in an int16 buffer;
     * see fill_chunk. */
    float    scale;
};

/* The set the audio thread mixes from. Built entirely before it is published
 * and never mutated after, so the realtime side needs no lock, only g_inuse to
 * keep the memory alive.
 *
 * The drums slot stays empty: that part is derived from the other two and the
 * original, so indices match the wire and the UI without a mapping table. */
struct stem_set {
    struct stem_buf part[STEM_N_PARTS];
    int             rate;
};

static struct stem_set *g_set;      /* published/retired by the worker */
static int              g_inuse;    /* audio threads currently inside the set */

/* ---- realtime side -------------------------------------------------------
 *
 * Two atomics per 1024-frame block guarantee the worker cannot free a buffer
 * while a read is inside it. The order is load-after-increment on this side
 * and retire-then-drain on the other, so either the audio thread sees the set
 * and the worker waits for it, or it sees NULL and there is nothing to wait
 * for. */
int stem_store_acquire(struct stem_view *out)
{
    const struct stem_set *set;

    __atomic_fetch_add(&g_inuse, 1, __ATOMIC_SEQ_CST);
    set = __atomic_load_n(&g_set, __ATOMIC_SEQ_CST);
    if (!set) {
        __atomic_fetch_sub(&g_inuse, 1, __ATOMIC_SEQ_CST);
        return 0;
    }
    out->harmonics = set->part[STEM_PART_HARMONICS].pcm;
    out->vocals    = set->part[STEM_PART_VOCALS].pcm;
    out->h_scale   = set->part[STEM_PART_HARMONICS].scale;
    out->v_scale   = set->part[STEM_PART_VOCALS].scale;
    out->frames    = set->part[STEM_PART_HARMONICS].frames;
    if (set->part[STEM_PART_VOCALS].frames < out->frames)
        out->frames = set->part[STEM_PART_VOCALS].frames;
    return 1;
}

void stem_store_release(void)
{
    __atomic_fetch_sub(&g_inuse, 1, __ATOMIC_SEQ_CST);
}

/* ---- worker side ---------------------------------------------------------- */

static void set_free(struct stem_set *set)
{
    int i;

    if (!set)
        return;
    for (i = 0; i < STEM_N_PARTS; i++)
        free(set->part[i].pcm);
    free(set);
}

/* Retire the live set and wait until no audio thread is still inside it.
 *
 * The wait is bounded by one block: the audio thread holds the reference only
 * across a single read, so this settles within a block period (~10 ms for the
 * stretcher's 1024-frame pulls) and cannot deadlock, since nothing the audio
 * thread does inside can block on us. sched_yield rather than a busy spin so a
 * single-core system still makes progress. */
static void set_retire(void)
{
    struct stem_set *old = __atomic_exchange_n(&g_set, (struct stem_set *)0,
                                               __ATOMIC_SEQ_CST);
    unsigned spins = 0;

    if (!old)
        return;
    while (__atomic_load_n(&g_inuse, __ATOMIC_SEQ_CST) != 0) {
        sched_yield();
        if (++spins > 100000) {
            /* Only if the audio thread is wedged; leaking a few hundred MB is
             * better than freeing memory it is reading. */
            MWARN("stem_store: inuse never drained, LEAKING the set\n");
            return;
        }
    }
    set_free(old);
}

/* Sink for stem_decode_pull: float -> s16 straight into the destination. */
struct fill_ctx {
    int16_t *pcm;
    int64_t  cap;      /* frames */
    int64_t  n;        /* frames written */
    int      report;   /* drive the progress bar from this decode's position */
    int      last_pct; /* what was last published, so the bar is not re-published */
};

/* Store what the server sent, not the restored stem.
 *
 * The server normalises: "at s16le a stem peaking past full scale is scaled to
 * fit, so gain is not always 1.0". A separated stem can exceed full scale
 * (components that cancel in the mix do not cancel on their own), so 1.0/gain
 * restores values above 1.0.
 *
 * Restoring here would need a clamp to fit s16, and since drums are derived as
 * `mix - H - V`, a clipped H makes the residual too loud wherever the clamp
 * acts.
 *
 * So the normalised samples are stored verbatim: <= full scale by
 * construction, nothing clips, and all 16 bits are used. The restoration is in
 * the mix coefficient, in float, once per block. See stem_mix. */
static int fill_chunk(const float *pcm, int64_t frames, void *user)
{
    struct fill_ctx *c = user;
    int64_t i, room = c->cap - c->n;
    int16_t *dst;

    /* The main abort point: this is where the time goes, and it is reached
     * from both decode threads. Non-zero unwinds stem_decode_pull. */
    if (!stem_job_load_wanted())
        return -1;
    if (frames > room)
        frames = room;
    if (frames <= 0)
        return 0;
    dst = c->pcm + c->n * 2;
    for (i = 0; i < frames * 2; i++) {
        float v = pcm[i];

        /* The converter can return inter-sample values slightly over full
         * scale; clip them as the deck's output stage would, since wrapping
         * would click. */
        if (v > 1.0f) v = 1.0f;
        else if (v < -1.0f) v = -1.0f;
        dst[i] = (int16_t)(v * 32767.0f);
    }
    c->n += frames;

    /* Only one of the two concurrent decodes reports: the snapshot in job.c is
     * a single-writer seqlock, and the two stems are the same length, so either
     * one represents the pair. */
    if (c->report && c->cap > 0) {
        int pct = (int)(c->n * 100 / c->cap);

        if (pct != c->last_pct) {
            c->last_pct = pct;
            stem_progress_set(STEM_STAGE_LOADING, pct);
        }
    }
    return 0;
}

/* What the deck keeps for itself while a pair is resident: its page pool,
 * the waveforms, the browser. EP122 idles at ~490 MB and is OOM-killed around
 * 1020 MB with a 415 MB pair resident. */
#define LOAD_HEADROOM   (128u * 1024 * 1024)

/* MemAvailable, or 0 when it cannot be read. */
static uint64_t mem_available(void)
{
    char line[96];
    unsigned long long kb = 0;
    FILE *fp = fopen("/proc/meminfo", "r");

    if (!fp)
        return 0;
    while (fgets(line, sizeof(line), fp))
        if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1)
            break;
    fclose(fp);
    return (uint64_t)kb * 1024;
}

/* Decode one stem onto the pool timeline. Returns frames, or a
 * STEM_PUBLISH_* code. Takes whatever the reader gives.
 *
 * No length check, deliberately: stem_store_acquire clamps to the shorter of
 * the two stems and stem_mix leaves the rest of the block alone, so a stem
 * that stops early just stops applying. Alignment is anchored at frame 0, so
 * nothing shifts. The converter's estimated output length overshoots by a
 * per-track amount, so any threshold would reject good loads over an inaudible
 * tail. decode.c logs where a stream ended; an unreadable stem stops four
 * orders of magnitude short. */
static int64_t load_one(struct stem_buf *slot, const char *path, int rate,
                        float gain, int report)
{
    struct fill_ctx ctx;
    int64_t len, got;

    len = stem_decode_pull(path, rate, NULL, NULL);
    if (len <= 0) {
        /* The file itself: it opened and the decoder would not read it. */
        MDBG("stem_store: %s: length probe failed (%lld)\n", path, (long long)len);
        return STEM_PUBLISH_BAD;
    }
    slot->pcm = malloc((size_t)len * 2 * sizeof(int16_t));
    if (!slot->pcm) {
        /* Our problem, not the file's. BAD here would condemn a good cache
         * entry over a momentary shortage. The stage stays LOADING through the
         * retry: IDLE reads as "nothing asked", which offline shows the
         * warning badge. */
        MDBG("stem_store: %s: out of memory for %lld frames (%lld MB)\n",
             path, (long long)len, (long long)(len * 4 / (1024 * 1024)));
        return STEM_PUBLISH_RETRY;
    }
    ctx.pcm = slot->pcm;
    ctx.cap = len;
    ctx.n = 0;
    ctx.report = report;
    ctx.last_pct = -1;
    got = stem_decode_pull(path, rate, fill_chunk, &ctx);
    if (got <= 0) {
        free(slot->pcm);
        slot->pcm = NULL;
        /* An abort also arrives here as a failed decode; calling it BAD would
         * condemn a good cache entry and start a 171 MB upload for a track
         * already on the stick. Check what stopped it before judging the
         * file. */
        return stem_job_load_wanted() ? STEM_PUBLISH_BAD : STEM_PUBLISH_ABORT;
    }
    slot->frames = ctx.n;
    /* Carried, not applied: the mix undoes the server's scaling in float, where
     * the result can exceed full scale without clipping. */
    slot->scale = gain > 0.0f ? 1.0f / gain : 1.0f;
    MDBG("stem_store: %s -> %lld frames @%d Hz (%lld MB) scale %d/1000\n",
         path, (long long)ctx.n, rate,
         (long long)(ctx.n * 4 / (1024 * 1024)),
         (int)(slot->scale * 1000.0f));
    return ctx.n;
}

/* How long a publish waits for the pool rate to exist at all.
 *
 * Short, because the rate is measured on demand: the stretcher can be measured
 * without playback, so the only wait is for a deck whose audio engine is not
 * running yet. */
#define POOL_RATE_WAIT_SEC  10
#define POOL_RATE_POLL_US   (100 * 1000)
/* What one attempt costs: the sleep plus the measurement window it drives. */
#define POOL_RATE_STEP_MS   (POOL_RATE_POLL_US / 1000 + 250)

static int wait_for_pool_rate(void)
{
    int waited, rate = stem_pool_rate();

    if (rate > 0)
        return rate;

    /* Measure rather than wait for the passive figure: stem_pool_rate() is fed
     * by the report, which only runs while the play screen paints, and a track
     * is loaded from BROWSE, so the passive figure can arrive seconds after the
     * load commits. */
    rate = stem_engine_rate_measure();
    if (rate > 0)
        return rate;

    /* A deck that has just come up is not pulling anything yet, so there is
     * nothing to count for a few seconds. Keep measuring; each attempt is its own quarter-second window and the
     * first one after the engine starts answers. stem_pool_rate() is checked
     * too in case the report or a playback measurement gets there first. */
    MDBG("stem_store: waiting for the pool rate to be known\n");
    /* Count elapsed time, not iterations: each attempt costs its sleep plus the
     * measurement window. */
    for (waited = 0; waited < POOL_RATE_WAIT_SEC * 1000; waited += POOL_RATE_STEP_MS) {
        usleep(POOL_RATE_POLL_US);
        if (!stem_job_load_wanted())
            return 0;                   /* the DJ loaded something else */
        rate = stem_pool_rate();
        if (!rate)
            rate = stem_engine_rate_measure();
        if (rate > 0) {
            MDBG("stem_store: pool rate %d Hz after about %d ms\n", rate, waited);
            return rate;
        }
    }
    return 0;
}

/* Wait until the deck's track loader has stopped working.
 *
 * This decode is two threads, seconds of FLAC plus resampling, and ~350 MB of
 * allocation. Run while the loader is still working, it stalls
 * dj_player::AsyncLoadFunctionHandler::waitForAsyncProcessing past its timeout
 * and the deck sits at "Not Loaded" with the hot cues blinking; nothing
 * crashes and nothing in the log mentions stems.
 *
 * Do not gate on the stretcher rate: it reports whether ALSA is pulling the
 * engine, which stays healthy through a load while the deck is still opening
 * readers.
 *
 * stem_decode_deck_quiet_ms() measures the condition directly. Every reader the
 * deck builds passes through our open hook and ours are excluded, so a quiet
 * period with no opens means the loader has finished. A load that is already
 * done clears immediately. */
#define SETTLE_WAIT_MS    45000
#define SETTLE_QUIET_MS     150   /* no reader opened by the deck for this long */

/* With no open hook armed there is nothing to observe, so wait a fixed time.
 * Long enough to clear a typical load (about six seconds of loader reads);
 * only applies when the hooks did not come up. */
#define SETTLE_BLIND_SEC      10

static int wait_for_deck(void)
{
    if (stem_decode_deck_quiet_ms() == STEM_DECK_NEVER_OPENED) {
        MDBG("stem_store: no open hook to watch the deck with"
             " -> waiting %d s before loading\n", SETTLE_BLIND_SEC);
        sleep(SETTLE_BLIND_SEC);
        return 1;
    }
    /* The shortest quiet that still means the loader stopped: on a cache hit
     * this wait is latency the DJ sees. */
    return stem_decode_wait_deck_quiet(SETTLE_QUIET_MS, SETTLE_WAIT_MS);
}

/* One stem's worth of work, so the pair can be decoded side by side. */
struct load_arg {
    struct stem_buf *slot;
    const char      *path;
    int              rate;
    float            gain;
    int64_t          result;
};

static void *load_thread_fn(void *p)
{
    struct load_arg *a = p;

    /* Never reports: only the caller's own decode drives the bar. */
    a->result = load_one(a->slot, a->path, a->rate, a->gain, 0);
    return NULL;
}

/* Whether the deck can hold a pair of `len` frames at the pool rate, with
 * LOAD_HEADROOM to spare. Asked before either part is allocated: on this kernel
 * an unbackable malloc does not fail, it overcommits and EP122 is killed when
 * the pages are touched. Asked once for the pair, since the parts load side by
 * side and a part asking after its sibling has touched its buffer would refuse
 * a pair that fits. */
static int pair_fits(const char *path, int64_t len)
{
    uint64_t need = 2 * (uint64_t)len * 2 * sizeof(int16_t) + LOAD_HEADROOM;
    uint64_t avail = mem_available();

    if (!avail || need <= avail)
        return 1;
    MDBG("stem_store: %s: the pair needs %llu MB with headroom, %llu MB"
         " available -> not loading\n", path,
         (unsigned long long)(need >> 20), (unsigned long long)(avail >> 20));
    return 0;
}

/* Build a set from two already-written files and publish it. Worker thread. */
int stem_store_publish(const char *harmonics_path, float harmonics_gain,
                       const char *vocals_path, float vocals_gain)
{
    int rate;
    int64_t len;
    struct stem_set *set;

    /* Before either wait: both block, and the bar must not stay frozen on the
     * previous stage. */
    stem_progress_set(STEM_STAGE_LOADING, 0);
    rate = wait_for_pool_rate();
    if (rate <= 0) {
        /* Either the rate is not known yet -- nothing wrong with the files, ask
         * again -- or the track changed underneath us, which is neither a
         * failure nor something to come back to. */
        if (!stem_job_load_wanted())
            return STEM_PUBLISH_ABORT;
        MDBG("stem_store: pool rate still unknown after %d s, not loading yet\n",
             POOL_RATE_WAIT_SEC);
        return STEM_PUBLISH_RETRY;
    }
    /* Not until the deck's load is done. Also not an error: the job comes back
     * in a few seconds, after the load it was racing. */
    if (!wait_for_deck())
        return stem_job_load_wanted() ? STEM_PUBLISH_RETRY : STEM_PUBLISH_ABORT;
    /* The shortage may be the deck's own load still settling: RETRY, not BAD.
     * A file the probe cannot size is left to load_one, which knows what to
     * say about it. */
    len = stem_decode_pull(harmonics_path, rate, NULL, NULL);
    if (len > 0 && !pair_fits(harmonics_path, len))
        return STEM_PUBLISH_RETRY;
    set = calloc(1, sizeof(*set));
    if (!set)
        return STEM_PUBLISH_RETRY;
    set->rate = rate;

    /* The two stems decode concurrently, one on a second thread.
     *
     * They are independent (different file, different buffer, no shared state)
     * and the work is CPU, not I/O: the cached FLACs come off the stick in tens
     * of milliseconds, while each decode spends seconds in FLAC, the 44.1k ->
     * pool-rate resample and the write into its own 150-odd MB. Serially that
     * is the whole cache-hit latency, and there is more than one core.
     *
     * The caller runs one of them itself, so this costs a single
     * pthread_create.
     *
     * Both decodes write decode.c's diagnostic capture, so an interleaved
     * "open" log line is possible. Only the log: the path that decides what
     * gets uploaded is thread-local. */
    {
        struct load_arg va = {
            &set->part[STEM_PART_VOCALS], vocals_path, rate, vocals_gain, -1
        };
        pthread_t th;
        int threaded = (pthread_create(&th, NULL, load_thread_fn, &va) == 0);
        int64_t hr;

        if (!threaded) {
            MDBG("stem_store: no thread for the second stem, loading serially\n");
            va.result = load_one(va.slot, va.path, va.rate, va.gain, 0);
        }
        hr = load_one(&set->part[STEM_PART_HARMONICS], harmonics_path, rate,
                      harmonics_gain, 1);
        if (threaded)
            pthread_join(th, NULL);

        if (hr < 0 || va.result < 0) {
            /* One unreadable stem condemns the pair; a resource failure on
             * either does not. An abort outranks both: the pair was never
             * judged, so it must not be marked either way. */
            int abort_ = (hr == STEM_PUBLISH_ABORT ||
                          va.result == STEM_PUBLISH_ABORT);
            int bad = (hr == STEM_PUBLISH_BAD || va.result == STEM_PUBLISH_BAD);

            set_free(set);
            if (abort_) {
                MDBG("stem_store: load abandoned, the track moved on\n");
                return STEM_PUBLISH_ABORT;
            }
            return bad ? STEM_PUBLISH_BAD : STEM_PUBLISH_RETRY;
        }
    }

    set_retire();
    __atomic_store_n(&g_set, set, __ATOMIC_SEQ_CST);
    MDBG("stem_store: published %lld frames @%d Hz\n",
         (long long)set->part[STEM_PART_HARMONICS].frames, rate);
    return 0;
}

int stem_store_complete(void)
{
    return __atomic_load_n(&g_set, __ATOMIC_SEQ_CST) != 0;
}

/* The rate the resident stems were decoded at, or 0 if none are. Not the rate
 * we would choose now: the pool measurement must be checked against what these
 * buffers were built at. */
int stem_store_rate(void)
{
    const struct stem_set *set = __atomic_load_n(&g_set, __ATOMIC_SEQ_CST);

    return set ? set->rate : 0;
}

void stem_store_release_all(void)
{
    /* The caller has already cleared g_stem_ready, so the audio thread is on
     * its way back to the stock path; set_retire() makes that certain before
     * anything is freed. See job.c. */
    set_retire();
}
