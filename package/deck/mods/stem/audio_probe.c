// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * mods/stem/audio_probe.c - watching the stretcher, the track watch, and the periodic report.
 */
#include "stem/audio_internal.h"
#include <math.h>
#include "xpad/ext.h"
#include "stem/loop.h"
#include "kit/mod.h"
#include "wave/wave.h"
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

/* Accumulate one call, nothing else. This runs on the audio thread and the
 * track loader thread, where MDBG is unsafe: it is fprintf+fflush to a stderr
 * that journald drains, so under backpressure it blocks. A blocked loader
 * thread times out dj_player::AsyncLoadFunctionHandler::waitForAsyncProcessing
 * and the deck sits at "Not Loaded" with the hot cues blinking. mod_safe_read
 * is out too (a pread syscall per call). mod_stem_audio_report() prints from
 * the message thread. */
void probe_tick(struct probe *p, int64_t len)
{
    uint64_t n = (uint64_t)(len > 0 ? len : 0);

    p->calls++;
    p->frames += n;
    p->win_calls++;
    p->win_frames += n;
    if (n > p->win_maxblk) p->win_maxblk = n;
}

/* Track change: drop the old stems and ask for the new track's. Runs from every
 * paint, outside the report window, which would otherwise add up to
 * PROBE_WIN_SEC before the upload starts. The remaining delay is the worker's
 * usleep (<=100 ms) and the length probe (~0.5-2 s for an 8-minute track). */
static void stem_track_act(const char *path, const char *why)
{
    static char acted_on[STEM_CACHE_PATH_MAX];

    /* Once per track, and once per "no track": a load with no path tears the
     * previous set down, and the read that follows it must not do so again. */
    if (path ? (acted_on[0] && strcmp(path, acted_on) == 0) : !acted_on[0])
        return;
    snprintf(acted_on, sizeof(acted_on), "%s", path ? path : "");

    MDBG("stem_audio: track change (%s) -> %s [%s]\n", why,
         g_stems_on ? "requesting stems" : "STEMS off, idle",
         path ? path : "no path yet");
    /* The waveform first, and from here rather than from the worker, which
     * gets there up to 100 ms later. In that window the levels below snap to
     * unity, the wave worker sees gains move and repaints the new track's
     * array from the old track's pristine copy, which the deck never corrects
     * because it has already filled it. */
    wave_stems_track_gone();
    /* Before the teardown, so the levels are at unity when the old set is
     * dropped and no leftover level applies to the next track. This runs on the
     * message thread, which owns the Sliders. */
    mod_stems_reset_levels();
    /* Naming the new track is the teardown: it clears readiness and gives the
     * loader a new generation. A separation running for the track we just left
     * is left alone and finishes into the cache. */
    stem_job_set_track(path);
}

/* Resolve the sourceId to a path and act on it here, on the message thread
 * (the sequence moves Sliders). Two ways of learning the id:
 *
 * The page pool's reads. A deck parked at the cue point still pre-buffers, so
 * those reads carry the track's id with a countdown in the top half; masking
 * that half (see stem_source_read) is what makes AUTO CUE work.
 *
 * The deck's own load result, for a deck loaded and left at 0:00 with AUTO CUE
 * off, which reads nothing.
 *
 * Both feed stem_track_act, deduplicated on the path. */
static void stem_track_watch(void)
{
    static uint32_t seen_track, seen_load;
    uint32_t gen = __atomic_load_n(&g_src.track_gen, __ATOMIC_RELAXED);
    uint32_t lgen = __atomic_load_n(&g_load.gen, __ATOMIC_RELAXED);
    char sid[64];

    /* The load event first: it comes first on a deck that loads and does not
     * play; on one that does, the reads follow with the same id and
     * stem_track_act sees the same path. */
    if (lgen != seen_load && !(lgen & 1u)) {
        uint64_t lo, hi;

        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        lo = g_load.sid_lo;
        hi = g_load.sid_hi;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&g_load.gen, __ATOMIC_RELAXED) == lgen) {
            seen_load = lgen;
            snprintf(sid, sizeof(sid), "loaded sid %llx:%llx",
                     (unsigned long long)hi, (unsigned long long)lo);
            stem_track_act(stem_decode_path_for_sid(lo, hi), sid);
        }
    }

    if (gen == seen_track)
        return;
    seen_track = gen;

    /* Resolve which track first. Only the sourceId is reliable: the deck opens
     * nothing when the pool already holds the track, so "the last file opened"
     * can be the previous song. */
    snprintf(sid, sizeof(sid), "sid %llx:%llx",
             (unsigned long long)g_src.sid_hi,
             (unsigned long long)g_src.sid_lo);
    stem_track_act(stem_decode_path_for_sid(g_src.sid_lo, g_src.sid_hi), sid);
}

/* Called from the play-screen paint hook on the juce message thread, where
 * blocking on stderr costs a dropped frame rather than an audio stall. */
void mod_stem_audio_report(void)
{
    static uint64_t last;
    uint64_t now = stem_cntvct(), hz = stem_cntfrq(), dt;
    int i;

    /* Before any windowing: the trigger has to be prompt, the numbers do not. */
    stem_track_watch();

    if (!hz) return;
    if (!last) { last = now; return; }
    dt = now - last;
    if (dt < hz * PROBE_WIN_SEC) return;
    last = now;

    if (g_xp_gate.miss) {
        MDBG("stem_audio: stretch gate hit %llu miss %llu (want %llx, saw %llx)\n",
             (unsigned long long)g_xp_gate.hit,
             (unsigned long long)g_xp_gate.miss,
             (unsigned long long)__atomic_load_n(&g_src.sid_lo, __ATOMIC_RELAXED),
             (unsigned long long)g_xp_gate.saw);
        g_xp_gate.hit = g_xp_gate.miss = 0;
    }

    for (i = 0; i <= N_PROBE; i++) {
        struct probe *p = (i < N_PROBE) ? &g_probe[i] : &g_op_probe;
        uint64_t calls = p->win_calls, frames = p->win_frames, mx = p->win_maxblk;

        p->win_calls = p->win_frames = p->win_maxblk = 0;
        if (!calls) continue;
        /* The stretcher's window is the engine rate, so publish it here. */
        if (i == PROBE_STRETCH) {
            int r = snap_rate((frames * hz) / dt);

            if (r && r != __atomic_load_n(&g_engine_rate, __ATOMIC_RELAXED)) {
                __atomic_store_n(&g_engine_rate, r, __ATOMIC_RELAXED);
                MDBG("stem_audio: engine rate %d Hz from the stretcher\n", r);
            }
        }
        /* frames/s is the discriminator: whatever carries playback has to move
         * samples at the stream rate; a loader or preview will not. */
        MTRACE("stem_audio: RATE %-14s %4llu calls/s %6llu frames/s avg blk %4llu "
             "max blk %5llu (total %llu)\n",
             p->name,
             (unsigned long long)((calls * hz) / dt),
             (unsigned long long)((frames * hz) / dt),
             (unsigned long long)(frames / calls),
             (unsigned long long)mx,
             (unsigned long long)p->calls);
    }

    /* The mix point, reported until established and then only on change. `vt`
     * identifies the source class feeding the stretcher. A second vt means two players are alive and only one is being
     * mixed; warned loudly because otherwise one stem bank silently does
     * nothing. */
    {
        static uintptr_t reported_vt;
        static uint64_t  reported_sets;
        static uint64_t  reported_hits;

        if (g_src.vt != reported_vt || g_src.sets != reported_sets ||
            (g_src.hits != 0) != (reported_hits != 0)) {
            reported_vt = g_src.vt;
            reported_sets = g_src.sets;
            reported_hits = g_src.hits;
            MDBG("stem_audio: source vt %#lx obj %p sets %llu hit %llu "
                 "miss %llu last %p\n",
                 (unsigned long)g_src.vt, g_src.obj,
                 (unsigned long long)g_src.sets,
                 (unsigned long long)g_src.hits,
                 (unsigned long long)g_src.misses, g_src.last);
        }
        if (g_src.hits) {
            /* Printed every window while audio flows, to show slider moves
             * landing. `posrate` is the pool's sample rate at 1.0x (the
             * alignment constant for every stem); `sid` is the track the pool
             * is serving. */
            static int64_t last_pos;
            int64_t dpos = g_src.pos - last_pos;
            uint64_t posrate = (dpos > 0 ? (uint64_t)dpos : 0) * hz / dt;

            last_pos = g_src.pos;
            pool_rate_observe(posrate);
            MTRACE("stem_audio: MIX peak %d/1000 gain %d/1000 stems %d bypass %d "
                 "pos %lld posrate %lld sid %llx:%llx\n",
                 (int)(g_src.peak * 1000.0f),
                 (int)(stem_gain_get(0) * 1000.0f),
                 g_stems_on ? 1 : 0, stem_bypass_get() ? 1 : 0,
                 (long long)g_src.pos, (long long)posrate,
                 (unsigned long long)g_src.sid_hi,
                 (unsigned long long)g_src.sid_lo);
            g_src.peak = 0.0f;
        }
        if (g_xp_lag.n) {
            MTRACE("stem_audio: XPLAG after %lld before %lld lo %lld hi %lld "
                 "n %llu (frames of pool rate)\n",
                 (long long)g_xp_lag.after, (long long)g_xp_lag.before,
                 (long long)g_xp_lag.lo, (long long)g_xp_lag.hi,
                 (unsigned long long)g_xp_lag.n);
            g_xp_lag.n = 0;
        }
        if (g_src.seen_vt) {
            MWARN("stem_audio: SECOND source class vt %#lx NOT patched "
                 "(mixing only vt %#lx)\n",
                 (unsigned long)g_src.seen_vt, (unsigned long)g_src.vt);
            g_src.seen_vt = 0;
        }
        {
            static uint64_t reported_prov;

            if (g_src.sid_provisional != reported_prov) {
                MDBG("stem_audio: %llu reads under a pre-buffering id, masked "
                     "to the track (total %llu)\n",
                     (unsigned long long)(g_src.sid_provisional - reported_prov),
                     (unsigned long long)g_src.sid_provisional);
                reported_prov = g_src.sid_provisional;
            }
        }
    }

    if (g_orig_operate && g_op.calls) {
        uint64_t n = g_op.calls, avg_ns = (g_op.ticks * 1000000000ull) / (n * hz);
        MTRACE("stem_audio: operate cost avg=%lluus max=%lluus -> 3x avg=%lluus\n",
             (unsigned long long)(avg_ns / 1000),
             (unsigned long long)((g_op.max_ticks * 1000000000ull) / hz / 1000),
             (unsigned long long)(3 * avg_ns / 1000));
        g_op.calls = g_op.ticks = g_op.max_ticks = g_op.frames = 0;
    }
}

/* Is this read the loaded track's? Decided by the sourceId in the Position,
 * not by the object. The adapter's own +0x18 is a CascadedTimeStretchManager
 * but not the one setSource is called on, so the objects never match. The id
 * is masked as in stem_source_read: the top half of `hi` counts down while a
 * track is still arriving. */
static int stretch_is_play_path(const void *src)
{
    uint64_t lo, hi;

    if (!src) return 0;
    memcpy(&lo, (const char *)src + POS_SOURCEID_OFF, sizeof(lo));
    memcpy(&hi, (const char *)src + POS_SOURCEID_OFF + 8, sizeof(hi));
    hi &= 0xffffffffull;
    if ((!lo && !hi) ||
        lo != __atomic_load_n(&g_src.sid_lo, __ATOMIC_RELAXED) ||
        hi != __atomic_load_n(&g_src.sid_hi, __ATOMIC_RELAXED)) {
        g_xp_gate.miss++;
        g_xp_gate.saw = lo;
        return 0;
    }
    g_xp_gate.hit++;
    return 1;
}

/* ReadableTimeStretchAdapter::read is a three-line forwarder:
 *
 *     x4 = *(this + 0x18);                    // the CascadedTimeStretchManager
 *     (*(*x4 + 0x98))(x4, src_pos, dst);      // operate(), not IReadable::read
 *     return sret;
 *
 * so it sees the stretcher's output. It carries steady playback (constant
 * 64-frame blocks matching the ALSA period), which lets the rate report tell
 * the play path from a loader or the preview player. */
pcm_pos_t probe_read_stretch(void *self, void *dst, const void *src, int64_t len)
{
    struct probe *p = &g_probe[PROBE_STRETCH];
    int64_t pos_before = __atomic_load_n(&g_src.pos, __ATOMIC_RELAXED);
    pcm_pos_t r;

    probe_tick(p, len);
    r = ((read_fn_t)p->orig)(self, dst, src, len);

    /* The post-stretch mix point. `dst` now holds the stretcher's output, so
     * anything summed here is not warped by it; see xpad/ext.h for why the
     * sampler needs that and the stems do not. */
    if (dst && len > 0 && stretch_is_play_path(src)) {
        int64_t rp = 0, d;

        memcpy(&rp, (const char *)src + POS_POS_OFF, sizeof(rp));
        d = __atomic_load_n(&g_src.pos, __ATOMIC_RELAXED) - rp;
        if (!g_xp_lag.n || d < g_xp_lag.lo) g_xp_lag.lo = d;
        if (!g_xp_lag.n || d > g_xp_lag.hi) g_xp_lag.hi = d;
        g_xp_lag.after  = d;
        g_xp_lag.before = pos_before - rp;
        g_xp_lag.n++;

        xpad_mix((float *)dst, len, rp);
    }
    return r;
}
