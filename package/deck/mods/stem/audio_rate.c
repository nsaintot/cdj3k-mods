// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * mods/stem/audio_rate.c - the two clocks: the deck's pool rate and the stretcher's engine rate.
 */
#include "stem/audio_internal.h"

/* Every rate the deck's engine can be set to. A measured rate is snapped to
 * whichever of these it is within 1 % of; anything else is refused (snap_rate
 * returns 0), because an invented rate would misalign every stem in a way that
 * sounds like a bad separation. */
static const int k_pool_rates[] = { 44100, 48000, 88200, 96000, 176400, 192000 };

/* How long stem_engine_rate_measure watches the stretcher for. */
#define ENGINE_SAMPLE_US (250 * 1000)
#include <math.h>
#include "xpad/ext.h"
#include "stem/loop.h"
#include "kit/mod.h"
#include "wave/wave.h"
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

/* Any thread: a torn read costs one stale window, not worth a lock on a path
 * the audio thread touches every block. The waveform mod uses the playhead to
 * repaint the on-screen columns first after a fader move. */
int64_t stem_source_pos(void)
{
    int64_t p = __atomic_load_n(&g_src.pos, __ATOMIC_RELAXED);

    return p > 0 ? p : -1;
}

/* Relaxed loads of a pair the audio thread writes as two stores: a torn read is
 * possible and costs one comparison, and the caller polls. A lock would put the
 * realtime path behind a repaint. */
int stem_source_id(uint64_t *lo, uint64_t *hi)
{
    *lo = __atomic_load_n(&g_src.sid_lo, __ATOMIC_RELAXED);
    *hi = __atomic_load_n(&g_src.sid_hi, __ATOMIC_RELAXED);
    return *lo || *hi;
}

int snap_rate(uint64_t r)
{
    unsigned i;

    for (i = 0; i < sizeof(k_pool_rates) / sizeof(k_pool_rates[0]); i++) {
        int k = k_pool_rates[i];
        uint64_t tol = (uint64_t)k / 100;

        if (r + tol >= (uint64_t)k && r <= (uint64_t)k + tol)
            return k;
    }
    return 0;
}

/* The report publishes this once per window, but only while the play screen
 * paints, and a track is loaded from BROWSE, so the figure can arrive seconds
 * after the decode needs it. The worker measures on demand. Two samples a
 * quarter-second apart suffice at 96 kHz (24000 frames against a 1 %
 * tolerance). Worker thread only. */
int stem_engine_rate_measure(void)
{
    uint64_t a = stem_engine_frames(), b;
    int r;

    if (!a)
        return 0;
    usleep(ENGINE_SAMPLE_US);
    b = stem_engine_frames();
    r = snap_rate((b - a) * 1000000ull / ENGINE_SAMPLE_US);
    if (r && r != __atomic_load_n(&g_engine_rate, __ATOMIC_RELAXED)) {
        __atomic_store_n(&g_engine_rate, r, __ATOMIC_RELAXED);
        MDBG("stem_audio: engine rate %d Hz measured on demand\n", r);
    }
    return r;
}

int stem_pool_rate(void)
{
    int measured = __atomic_load_n(&g_pool_rate, __ATOMIC_RELAXED);
    int engine = __atomic_load_n(&g_engine_rate, __ATOMIC_RELAXED);

    /* Two sources, the more direct first.
     *
     * The position measurement observes the pool itself and wins when it
     * exists, but it needs playback (a paused deck is not read), so it never
     * answers for a track left at the cue point. It can also be slow to
     * commit, since it needs two consecutive agreeing windows.
     *
     * The stretcher runs on the same timeline without playback and covers the
     * rest. A caller that finds neither should request a fresh measurement;
     * see stem_engine_rate_measure.
     *
     * Do not add /proc/asound as a third source: it gives the DAC's rate, which
     * differs from the pool's when anything resamples between them (a DAC at
     * 48000 with the pool at 96000 would decode a stem set at half rate).
     *
     * Answering before playback is safe only because the decode is held off the
     * track loader; see wait_for_deck in store.c. */
    return measured ? measured : engine;
}

/* Called from the report with one window's Position advance. Accepts a rate
 * only when seen twice in a row: a single window can straddle a seek, a pause
 * or a tempo change. */
void pool_rate_observe(uint64_t rate)
{
    static int last_match;

    /* g_pool_rate, not stem_pool_rate(): that answers from the stretcher before
     * anything has played, and gating on it would stop this measurement, which
     * is what checks the stretcher, from ever running. */
    if (__atomic_load_n(&g_pool_rate, __ATOMIC_RELAXED))
        return;
    {
        int r = snap_rate(rate);

        if (!r) {
            last_match = 0;
            return;
        }
        if (last_match == r) {
            /* The rate the loaded stems were decoded at, asked of the store.
             * Inferring it from whichever source answers now can compare the
             * new rate with itself while the buffers were built at another. */
            int assumed = stem_store_rate();

            __atomic_store_n(&g_pool_rate, r, __ATOMIC_RELAXED);
            MDBG("stem_audio: pool rate %d Hz (measured %llu)\n",
                 r, (unsigned long long)rate);
            /* Stems decoded at the wrong rate drift against the mix for the
             * whole track and sound like a bad separation, so log it, drop the
             * set and request it again; the request now uses this rate. */
            if (assumed && assumed != r) {
                MDBG("stem_audio: ASSUMED %d Hz, POOL RUNS AT %d Hz"
                     " -> dropping the stems and reloading them\n", assumed, r);
                stem_track_gone();
                if (g_stems_on)
                    stem_job_request();
            }
        }
        last_match = r;
    }
}

/* The stretcher's output read is clocked by ALSA, not by the transport, so it
 * runs at the output rate whether the deck is playing or paused, and can drop
 * to about a quarter of that while a track load has the CPU. The pool itself is not read at all on a paused deck.
 *
 * Any thread: a torn 64-bit read costs one sample of a series the caller takes
 * several of. */
uint64_t stem_engine_frames(void)
{
    /* The stretcher's own read first, the manager's operate as the fallback.
     * Both see the same blocks, so either one is enough. */
    if (g_probe[PROBE_STRETCH].orig)
        return __atomic_load_n(&g_probe[PROBE_STRETCH].frames,
                               __ATOMIC_RELAXED);
    if (g_orig_operate)
        return __atomic_load_n(&g_op_probe.frames, __ATOMIC_RELAXED);
    return 0;
}
