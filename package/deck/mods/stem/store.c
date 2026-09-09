// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * store.c - stem files and their life as page-pool tracks.  [worker]
 *
 * Why the stems live in our own RAM, resampled by the deck's own converter, and
 * the lifetime rules around freeing them: docs/stem-engine.md.
 *
 * The sidecar writes each stem to tmpfs as a WAV -- a 44-byte header in front of
 * the s16 body makes it something audio_format::FileReadWav opens through the
 * createReaderFor path decode.c already exercises.
 *
 * The file is opened and immediately unlinked, so the inode survives while a
 * reader holds a descriptor and vanishes when the last one closes: no refcount of
 * ours, and nothing left behind if either process dies mid-job.
 *
 * g_stem_ready is cleared and the audio thread is back on the stock path before
 * any buffer is freed. The order is the caller's to keep -- see job.c.
 */
#include "stem/stem.h"

#include <pthread.h>

/* One decoded stem, on the pool's timeline. `pcm` is interleaved stereo s16:
 * half the memory of float for a reconstruction that is exact anyway, because
 * the derived part cancels the quantisation (see docs/stem-engine.md). */
struct stem_buf {
    int16_t *pcm;
    int64_t  frames;
    /* 1.0/gain: what turns the stored, normalised samples back into the stem
     * the model produced. Kept here rather than applied to the samples because
     * the result routinely exceeds full scale and would have to be clipped to
     * fit an int16 buffer -- see fill_chunk. */
    float    scale;
};

/* The set the audio thread mixes from. Built entirely before it is published
 * and never mutated after, so the realtime side needs no lock -- only proof
 * that the memory is still there, which is what g_inuse gives it.
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
 * Two atomics per block against a 1024-frame block is nothing, and they buy the
 * one guarantee that actually matters: the worker cannot free a buffer while a
 * read is inside it. The order is load-after-increment on this side and
 * retire-then-drain on the other, so either the audio thread sees the set and
 * the worker waits for it, or it sees NULL and there is nothing to wait for. */
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
 * The spin is bounded by one block: the audio thread holds the reference only
 * across a single read, so this settles within a block period (~10 ms for the
 * 1024-frame pulls the stretcher makes) and cannot deadlock -- nothing the
 * audio thread does inside can block on us. sched_yield rather than a spin so a
 * single-core emulator still makes progress. */
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
            /* Cannot happen unless the audio thread is wedged, and leaking a
             * few hundred MB beats freeing memory it is reading. */
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

/* Store what the server sent, NOT the restored stem.
 *
 * The server normalises: "at s16le a stem peaking past full scale is scaled to
 * fit, so gain is not always 1.0". A separated stem genuinely CAN exceed full
 * scale -- components that cancel in the mix do not cancel on their own -- so
 * 1.0/gain restores values above 1.0 by design.
 *
 * Undoing that here was wrong twice over. It cannot be represented in the s16
 * buffer, so it had to be clamped, and the clamp destroyed exactly the peaks
 * the gain mechanism exists to preserve. Since the drums part is derived as
 * `mix - H - V`, a clipped H makes that residual too LOUD wherever the clamp
 * bit -- which is where the +1.26 dB overshoot came from.
 *
 * So the normalised samples are stored verbatim: they are <= full scale by
 * construction, nothing clips, and all 16 bits stay meaningful. The restoration
 * moves into the mix coefficient, where it happens in float, once per block,
 * and costs nothing. See stem_mix. */
static int fill_chunk(const float *pcm, int64_t frames, void *user)
{
    struct fill_ctx *c = user;
    int64_t i, room = c->cap - c->n;
    int16_t *dst;

    /* The abort point that matters: this is where the seconds go, and it is
     * reached from BOTH decode threads. Non-zero unwinds stem_decode_pull. */
    if (!stem_job_load_wanted())
        return -1;
    if (frames > room)
        frames = room;
    if (frames <= 0)
        return 0;
    dst = c->pcm + c->n * 2;
    for (i = 0; i < frames * 2; i++) {
        float v = pcm[i];

        /* The converter can still hand back inter-sample values fractionally
         * over full scale, and wrapping those would be a click rather than the
         * clip the deck's own output stage would produce. */
        if (v > 1.0f) v = 1.0f;
        else if (v < -1.0f) v = -1.0f;
        dst[i] = (int16_t)(v * 32767.0f);
    }
    c->n += frames;

    /* Only ONE of the two concurrent decodes reports. The snapshot in job.c is a
     * seqlock with a single writer by design, and the two stems are the same length
     * anyway, so the one we are on is a faithful reading of the pair. */
    if (c->report && c->cap > 0) {
        int pct = (int)(c->n * 100 / c->cap);

        if (pct != c->last_pct) {
            c->last_pct = pct;
            stem_progress_set(STEM_STAGE_LOADING, pct);
        }
    }
    return 0;
}

/* Decode one stem file onto the pool's timeline. Returns frames, or -1. */
/* Decode one stem onto the pool timeline. Takes whatever the reader gives.
 *
 * There is deliberately no length check. Nothing downstream needs a stem to be
 * complete: stem_store_acquire clamps to the shorter of the two and stem_mix
 * bounds its loop by that, leaving the rest of the block alone -- so a stem that
 * stops early just stops applying, and the deck plays the untouched mix from
 * there. Alignment is anchored at frame 0 and per-sample, so nothing shifts.
 *
 * Every check that lived here was a guessed threshold against the converter's
 * estimated output length, which overshoots by a per-track amount. All they ever
 * did was turn that soft, self-limiting failure into a hard one and throw away a
 * whole load over a tail nobody hears. decode.c logs where a stream ended; a
 * genuinely unreadable stem stops four orders of magnitude short and is obvious
 * in that one line. */
/* What the deck keeps for itself while a pair is resident: its page pool,
 * the waveforms, the browser. Measured on the emulator: EP122 idles at
 * ~490 MB and the kernel killed it at 1020 MB with a 415 MB pair in. */
#define LOAD_HEADROOM   (128u * 1024 * 1024)

/* MemAvailable, or 0 when it cannot be read. */
static uint64_t mem_available(void)
{
    char line[96];
    uint64_t kb = 0;
    FILE *fp = fopen("/proc/meminfo", "r");

    if (!fp)
        return 0;
    while (fgets(line, sizeof(line), fp))
        if (sscanf(line, "MemAvailable: %llu kB", (unsigned long long *)&kb) == 1)
            break;
    fclose(fp);
    return kb * 1024;
}

static int64_t load_one(struct stem_buf *slot, const char *path, int rate,
                        float gain, int report)
{
    struct fill_ctx ctx;
    int64_t len, got;
    uint64_t need, avail;

    len = stem_decode_pull(path, rate, NULL, NULL);
    if (len <= 0) {
        /* The file itself: it opened and the decoder would not read it. */
        MDBG("stem_store: %s: length probe failed (%lld)\n", path, (long long)len);
        return STEM_PUBLISH_BAD;
    }
    /* THE PAIR, not this part: both load side by side, so each asks for both.
     * Asked up front because a malloc that cannot be honoured does not fail on
     * this kernel -- it overcommits, and kills EP122 when the pages are touched
     * -- and a deck that restarts mid-set is worse than a track without stems.
     * RETRY, like the malloc below: the shortage may be the deck's own load
     * still settling. */
    need = 2 * (uint64_t)len * 2 * sizeof(int16_t) + LOAD_HEADROOM;
    avail = mem_available();
    if (avail && need > avail) {
        MDBG("stem_store: %s: the pair needs %llu MB with headroom, %llu MB"
             " available -> not loading\n", path,
             (unsigned long long)(need >> 20), (unsigned long long)(avail >> 20));
        stem_progress_set(STEM_STAGE_IDLE, 0);
        return STEM_PUBLISH_RETRY;
    }
    slot->pcm = malloc((size_t)len * 2 * sizeof(int16_t));
    if (!slot->pcm) {
        /* Us, not the file. Saying BAD here would condemn a good cache entry
         * over a momentary shortage and re-separate the track. */
        MDBG("stem_store: %s: out of memory for %lld frames (%lld MB)\n",
             path, (long long)len, (long long)(len * 4 / (1024 * 1024)));
        stem_progress_set(STEM_STAGE_IDLE, 0);
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
        /* An abort arrives here as a failed decode, and calling that BAD would
         * condemn a good cache entry and start a 171 MB upload for a track
         * already on the stick -- for no reason but the DJ loading something
         * else. Ask what stopped it before judging the file. */
        return stem_job_load_wanted() ? STEM_PUBLISH_BAD : STEM_PUBLISH_ABORT;
    }
    slot->frames = ctx.n;
    /* Carried, not applied. The server reports what it scaled by to make the
     * stem fit; undoing it belongs in the mix, where the result can exceed full
     * scale without being clipped into an int16. */
    slot->scale = gain > 0.0f ? 1.0f / gain : 1.0f;
    MDBG("stem_store: %s -> %lld frames @%d Hz (%lld MB) scale %d/1000\n",
         path, (long long)ctx.n, rate,
         (long long)(ctx.n * 4 / (1024 * 1024)),
         (int)(slot->scale * 1000.0f));
    return ctx.n;
}

/* How long a publish will wait for the pool rate to exist at all.
 *
 * Short, because the rate is asked for directly rather than waited on: the
 * stretcher can be measured without playback, so the only thing left to wait for
 * is a deck that is not running its audio engine yet. */
#define POOL_RATE_WAIT_SEC  10
#define POOL_RATE_POLL_US   (100 * 1000)
/* What one attempt costs: the sleep plus the measurement window it drives. */
#define POOL_RATE_STEP_MS   (POOL_RATE_POLL_US / 1000 + 250)

static int wait_for_pool_rate(void)
{
    int waited, rate = stem_pool_rate();

    if (rate > 0)
        return rate;

    /* ASK, rather than wait for the passive one to turn up.
     *
     * stem_pool_rate() is fed by the report, which only runs while the play
     * screen paints -- and a track is loaded from the BROWSE screen, which does
     * not. Waiting here meant the decode either stalled or, worse, ran on
     * whatever stale figure was lying around. Measured on the deck: the load
     * committed at 12:44:22 and the passive figure did not arrive until
     * 12:44:27. */
    rate = stem_engine_rate_measure();
    if (rate > 0)
        return rate;

    /* Asking too early is a real case rather than a failure: a deck that has
     * just come up is not pulling anything yet, so there is nothing to count.
     * Measured, that was 4.6 s of waiting on a load a second after start-up. So
     * keep ASKING rather than falling back to watching -- each attempt is its
     * own quarter-second window, and the first one after the engine starts
     * answers. stem_pool_rate() is checked alongside in case the report or a
     * playback measurement gets there first. */
    MDBG("stem_store: waiting for the pool rate to be known\n");
    /* Elapsed rather than a loop count: an attempt now costs its own sleep plus
     * however long the measurement window is, so counting iterations would quote
     * one timeout and enforce another. */
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
 * allocation. Run against a track loader that is still working, on a machine
 * with nothing spare, it does not merely slow the load down: it stalls
 * dj_player::AsyncLoadFunctionHandler::waitForAsyncProcessing past its timeout
 * and the deck sits at "Not Loaded" with the hot cues blinking. Nothing has
 * crashed and nothing in the log says stems -- which is what made it expensive
 * to find the first time.
 *
 * What to wait ON is the part that had to be measured rather than reasoned
 * about. The stretcher rate is the obvious candidate and it is the wrong one: it
 * reports whether ALSA is pulling the engine, which stays healthy right through
 * a load. Traced on a cache hit, four consecutive half-seconds all read healthy
 * while the deck was still opening readers for the track 1.3 s in -- so that
 * gate never once observed the condition it exists to catch, and the two seconds
 * it spent confirming were protecting the load by accident rather than by
 * measurement.
 *
 * stem_decode_deck_quiet_ms() is the condition itself. Every reader the deck
 * builds passes through our open hook and ours are excluded, so a quiet period
 * with no opens is the loader having finished. It also costs nothing when there
 * is nothing to wait for, which is the common case: a load that is already done
 * clears immediately instead of buying 2 s of confirmation. */
#define SETTLE_WAIT_MS    45000
#define SETTLE_QUIET_MS     150   /* no reader opened by the deck for this long */

/* With no open hook armed there is nothing to observe, and starting immediately
 * is the one option that is certainly wrong. Long enough to clear a load on this
 * deck (the loader's own reads spanned about six seconds), and it only applies
 * to a deck whose hooks did not come up. */
#define SETTLE_BLIND_SEC      10

static int wait_for_deck(void)
{
    if (stem_decode_deck_quiet_ms() == STEM_DECK_NEVER_OPENED) {
        MDBG("stem_store: no open hook to watch the deck with"
             " -> waiting %d s before loading\n", SETTLE_BLIND_SEC);
        sleep(SETTLE_BLIND_SEC);
        return 1;
    }
    /* The least patience that still means the loader stopped. Every millisecond
     * here is one the DJ spends watching a cached track not have stems yet. */
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

/* Build a set from two already-written files and publish it. Worker thread. */
int stem_store_publish(const char *harmonics_path, float harmonics_gain,
                       const char *vocals_path, float vocals_gain)
{
    int rate;
    struct stem_set *set;

    /* Said before either wait, not after: both of them block, and a bar frozen on
     * the previous stage is the thing this whole path exists to stop. */
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
    /* And not until the deck can afford it. Also not an error: the job comes
     * back in a few seconds, by which time the load it was racing is over. */
    if (!wait_for_deck())
        return stem_job_load_wanted() ? STEM_PUBLISH_RETRY : STEM_PUBLISH_ABORT;
    set = calloc(1, sizeof(*set));
    if (!set)
        return STEM_PUBLISH_RETRY;
    set->rate = rate;

    /* The two stems decode CONCURRENTLY, on a second thread of our own.
     *
     * They are wholly independent -- different file, different buffer, no shared
     * state -- and the work is CPU, not I/O: the cached FLACs come off the stick
     * in tens of milliseconds (page-cached, and the volume is not the limit),
     * while each decode spends seconds in FLAC + the 44.1k -> pool-rate resample
     * + the write into its own 150-odd MB. Serially that was the whole
     * cache-hit latency; there is more than one core, and nothing to contend on.
     *
     * The caller runs one of them rather than spawning two and idling, so the
     * whole thing costs a single pthread_create.
     *
     * Note the two decodes both write decode.c's diagnostic capture, so an
     * interleaved "open" log line is possible. Only the log: the path that
     * decides what gets uploaded is thread-local and untouched by either. */
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
             * either does not. An abort outranks both -- the pair was never
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

/* The rate the RESIDENT stems were decoded at, or 0 if none are.
 *
 * Not "the rate we would choose now" -- the rate these particular buffers were
 * built against, which is the only thing the pool measurement can meaningfully
 * be checked against. Those two are the same number right up until the moment
 * they are not, and that moment is exactly when the check has to fire. */
int stem_store_rate(void)
{
    const struct stem_set *set = __atomic_load_n(&g_set, __ATOMIC_SEQ_CST);

    return set ? set->rate : 0;
}

void stem_store_release_all(void)
{
    /* The caller has already cleared g_stem_ready, so the audio thread is on
     * its way back to the stock path. set_retire() is what makes that certain
     * before anything is freed. Order is the caller's to keep: see job.c. */
    set_retire();
}
