// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * mods/stem/audio_probe.c - watching the stretcher, capturing its blocks, and the periodic report.
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

void probe_tick(struct probe *p, int64_t len)
{
    uint64_t n = (uint64_t)(len > 0 ? len : 0);

    p->calls++;
    p->frames += n;
    p->win_calls++;
    p->win_frames += n;
    if (n > p->win_maxblk) p->win_maxblk = n;
}

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
    /* The waveform first, and from HERE rather than from the worker's cancel
     * branch. This is the instant the sourceId moves; the worker gets there up
     * to 100 ms later, and in that window the levels below snap back to unity,
     * the wave worker sees gains move, and it repaints the NEW track's array
     * from the OLD track's pristine copy -- which the deck then never corrects,
     * because it has already finished filling it. That is the waveform stuck
     * showing the previous track. */
    wave_stems_track_gone();
    /* Before the cancel, so the levels are already at unity by the time the old
     * set is dropped and there is no window where a leftover level could be
     * applied to whatever arrives next. Safe from here: this whole function runs
     * on the message thread, which is the one that owns the Sliders. */
    mod_stems_reset_levels();
    /* Naming the new track IS the teardown: it clears readiness and hands the
     * loader a new generation. A separation running for the track we just left
     * is deliberately untouched -- it finishes into the cache. */
    stem_job_set_track(path);
}

static void stem_track_watch(void)
{
    static uint32_t seen_track, seen_load;
    uint32_t gen = __atomic_load_n(&g_src.track_gen, __ATOMIC_RELAXED);
    uint32_t lgen = __atomic_load_n(&g_load.gen, __ATOMIC_RELAXED);
    char sid[64];

    /* The load event first: it is the earlier of the two on a deck that loads
     * and does not play, and on one that does the reads follow with the same id
     * and stem_track_act sees the same path. */
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

    /* Resolve WHICH track before anything else. The sourceId is the only honest
     * answer -- the deck opens nothing when the pool already holds the track, so
     * asking decode.c for "the last file opened" hands back the previous song. */
    snprintf(sid, sizeof(sid), "sid %llx:%llx",
             (unsigned long long)g_src.sid_hi,
             (unsigned long long)g_src.sid_lo);
    stem_track_act(stem_decode_path_for_sid(g_src.sid_lo, g_src.sid_hi), sid);
}

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
        /* The stretcher's window IS the engine rate, so it is published from
         * here rather than sampled again somewhere else. */
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

    /* The mix point, reported until it is established and then only on change.
     * `vt` is the one number worth carrying off the deck: it names the source
     * class through ep122sym.py and confirms the stretcher is fed by what we
     * think it is. A second vt means two players are alive and only one of them
     * is being mixed -- loud, because the symptom is otherwise just one stem
     * bank quietly doing nothing. */
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
            /* Printed every window while audio flows: this is the number that
             * says a slider move landed, and it is only useful as a series.
             * `posrate` is the pool's sample rate at 1.0x -- the alignment
             * constant for every stem -- and `sid` identifies the track the
             * pool is serving, which is what a track change moves. */
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

pcm_pos_t probe_read_stretch(void *self, void *dst, const void *src, int64_t len)
{
    struct probe *p = &g_probe[PROBE_STRETCH];
    int64_t pos_before = __atomic_load_n(&g_src.pos, __ATOMIC_RELAXED);
    pcm_pos_t r;

    probe_tick(p, len);
    r = ((read_fn_t)p->orig)(self, dst, src, len);

    /* THE POST-STRETCH MIX POINT. `dst` now holds the stretcher's output, so
     * anything summed here is past everything that would have warped it -- see
     * xpad/ext.h for why the sampler needs that and the stems do not. */
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
