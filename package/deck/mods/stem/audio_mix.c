// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * mods/stem/audio_mix.c - mixing the stems into the deck's own block, and the groove circuit's replacements.
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

/* The active slot's file as a loop over the track's timeline. The arithmetic
 * is in loop.c, which is pure and has its own tests. */
static struct stem_loop gc_loop_for(int64_t engage, int64_t span)
{
    struct stem_loop l = { 0, 0 };

    if (span <= 0)
        return l;
    l.from = engage;
    l.span = span;
    return l;
}

static double gc_ratio_for(double file_spb)
{
    double track_spb = stem_grid_spb();
    double r;

    if (!(file_spb > 0.0) || !(track_spb > 0.0))
        return 1.0;
    r = file_spb / track_spb;
    if (!(r >= GC_RATIO_MIN) || !(r <= GC_RATIO_MAX))
        return 1.0;
    return r;
}

static inline double gc_phase_at(struct gc_phase *p, int64_t at)
{
    if (!p->beats)
        return stem_loop_phase(&p->fl, at, p->ratio);
    return stem_loop_wrap((stem_beat_at(p->beats, p->count, at, &p->cursor)
                           - p->engaged) * p->file_spb, p->fl.span);
}

/* The file's stereo frame at fractional index `u`, linearly interpolated and
 * scaled out of s16. `u` is strictly below the span, so the successor is the
 * next frame or the first; interpolating across the wrap makes a rate-matched
 * loop seamless. The s16 scale rides on the interpolation weights: two
 * multiplies a frame rather than two a sample. */
static inline void gc_frame(const int16_t *pcm, int64_t span, double u,
                            float *l, float *r)
{
    const float q = 1.0f / 32767.0f;
    int64_t i0 = (int64_t)u;
    int64_t i1 = i0 + 1 < span ? i0 + 1 : 0;
    float b = (float)(u - (double)i0);
    float wb = b * q, wa = (1.0f - b) * q;

    *l = wa * (float)pcm[i0 * 2]     + wb * (float)pcm[i1 * 2];
    *r = wa * (float)pcm[i0 * 2 + 1] + wb * (float)pcm[i1 * 2 + 1];
}

/* Move the held MUTE onto the beat.
 *
 * The boundary comes from the X-PAD's clock, the only one on the audio path: it
 * runs off the track's grid while the play head moves and off the track's tempo
 * when it does not, so a mute lands on the beat on a parked deck too.
 *
 * Acts on a change of quarter, not on a boundary inside this call. This runs
 * pre-stretch, the clock advances post-stretch, and the stretcher pulls its
 * source in bursts, so this is not called once per block and a boundary may
 * never fall inside a call. Remembering the last quarter seen and acting when it
 * changes works whenever the next call happens.
 *
 * With the deck's QUANTIZE off, or with no clock, the commit is immediate. */
void stem_mute_commit(void)
{
    double beat;
    int64_t quarter = 0;
    int have, snap, i;

    have = xpad_beat_now(&beat);
    if (have)
        quarter = (int64_t)floor(beat * (double)STEM_MUTE_QUANT);

    for (i = 0; i < N_STEMS; i++)
        if (__atomic_load_n(&g_stem_mute_want[i], __ATOMIC_RELAXED) !=
            __atomic_load_n(&g_stem_mute_live[i], __ATOMIC_RELAXED))
            break;
    if (i == N_STEMS) {
        /* Nothing pending, so track the clock: a later request waits for the
         * next boundary, not a stale one. */
        stem_g_mute_quarter    = quarter;
        stem_g_mute_quarter_ok = have;
        return;
    }

    snap = xpad_quantize_div() != 0;
    if (snap && have && stem_g_mute_quarter_ok && quarter == stem_g_mute_quarter)
        return;                     /* still inside the quarter it was asked in */
    stem_g_mute_quarter    = quarter;
    stem_g_mute_quarter_ok = have;

    for (i = 0; i < N_STEMS; i++) {
        int want = __atomic_load_n(&g_stem_mute_want[i], __ATOMIC_RELAXED);
        float open_gain;

        if (want == __atomic_load_n(&g_stem_mute_live[i], __ATOMIC_RELAXED))
            continue;
        __atomic_store_n(&g_stem_mute_live[i], want, __ATOMIC_RELAXED);
        __atomic_load(&g_stem_gain_unmuted[i], &open_gain, __ATOMIC_RELAXED);
        stem_gain_set(i, want ? 0.0f : open_gain);
        __atomic_add_fetch(&g_stem_mute_commits, 1, __ATOMIC_RELAXED);
    }
}

/* Apply the stem levels to the block the stock read() just produced.
 *
 * Only two parts are stored; drums is the residual:
 *
 *     out = d*mix + (h-d)*H + (v-d)*V
 *
 * so the derived part costs nothing and s16 stems reconstruct exactly: the
 * quantisation enters twice with opposite signs and cancels. At unity
 * (d=h=v=1) this is `mix`, and the early-out below returns the block untouched
 * so BYPASS is bit-exact.
 *
 * `pos` is the pool's sample index and the stems were decoded onto that
 * timeline, so frame k of the block is frame pos+k of each stem. Outside the
 * stems' coverage the block is left alone; silencing it would turn a short stem
 * into a dropout.
 *
 * Audio-thread rules: no allocation, no I/O, no locks, bounded loop. `len`
 * frames are always valid because the stock read() zero-fills any tail it could
 * not satisfy. */
void stem_mix(void *self, const void *src, void *dst, int64_t pos,
                     int64_t len)
{
    struct stem_view v;
    struct gc_view g;
    struct gc_phase ph = { { 0, 0 }, 1.0, NULL, 0, 0.0, 0.0, -1 };
    float *s = (float *)dst;
    const float d = stem_gain_get(STEM_PART_DRUMS);
    const float gh = stem_gain_get(STEM_PART_HARMONICS);
    const float gv = stem_gain_get(STEM_PART_VOCALS);
    const float kh = gh - d, kv = gv - d;
    const float q = 1.0f / 32767.0f;
    int64_t engage = 0, n, i;
    int slot, have_gc = 0, have_beats = 0;

    (void)self; (void)src;
    /* Before the early-outs: a mute going on must be able to move the gains
     * away from unity, and the bit-exact bypass would otherwise skip it. */
    stem_mute_commit();
    if (!s || len <= 0 || pos < 0)
        return;
    if (!stem_store_acquire(&v))
        return;

    slot = gc_active(&engage);
    if (slot >= 0 && gc_acquire(slot, &g)) {
        ph.fl = gc_loop_for(engage, g.span);
        have_gc = ph.fl.span != 0;
        if (!have_gc)
            gc_release();
    }

    /* The phase route, decided once per block. The array needs the file's own
     * beat to measure against, so a slot whose config line states no BPM stays
     * on the ratio, which is then 1.0 (the file at its own speed).
     *
     * `engaged` is a beat index, not a position, so both ends of the
     * subtraction are in the same units however the tempo moved. It is an exact
     * whole number: the engage point is a beat of this grid, converted by the
     * same expression as the array. */
    if (have_gc) {
        struct stem_grid_view gv;

        ph.ratio    = gc_ratio_for(g.spb);
        ph.file_spb = g.spb;
        if (g.spb > 0.0 && stem_grid_beats_acquire(&gv)) {
            have_beats  = 1;
            ph.beats    = gv.beats;
            ph.count    = gv.count;
            /* No cursor: the engage point can be far from where this block
             * reads, so seeding the hint with it would only make the first
             * frame bisect twice. */
            ph.engaged  = stem_beat_at(gv.beats, gv.count, engage, NULL);
        }
    }

    /* Untouched, not multiplied by one, but only while no file replaces a stem:
     * a replacement changes the audio even at unity levels. */
    if (d == 1.0f && kh == 0.0f && kv == 0.0f && !have_gc) {
        stem_store_release();
        return;
    }

    n = v.frames - pos;            /* frames of this block the stems cover */
    if (n > len) n = len;

    /* The stems are stored as the server normalised them, and the scale is
     * undone here in the coefficient. Undoing it on the samples would clip a
     * stem that peaks above full scale, and that clipping lands in the derived
     * drums part and pushes the output past 0 dBFS.
     *
     * Unity stays exact: at d == gh == gv both coefficients are zero before the
     * scale is applied. */
    {
        const float sh = v.h_scale * q, sv = v.v_scale * q;
        const float ch = kh * sh, cv = kv * sv;
        const int16_t *hp = v.harmonics + pos * 2;
        const int16_t *wp = v.vocals    + pos * 2;

        if (!have_gc) {
            for (i = 0; i < n * 2; i++)
                s[i] = d * s[i] + ch * (float)hp[i] + cv * (float)wp[i];
        } else {
            /* One stem comes from the file, the other two from the track:
             *
             *     live       d*(M - H - V) + gh*H + gv*V   == d*M + kh*H + kv*V
             *     drums      d*F           + gh*H + gv*V
             *     harmonics  d*(M - H - V) + gh*F + gv*V
             *     vocals     d*(M - H - V) + gh*H + gv*F
             *
             * The residual is written out in the last two because the collapsed
             * form folds the drums level into the stem terms being replaced.
             * The file is plain s16 at the DJ's own level, so the server's
             * normalisation does not apply; gc_frame applies the s16 scale while
             * it interpolates.
             *
             * The file is read at a fractional index, so its phase is computed
             * per frame rather than stepped: a division a frame (or a hinted
             * search of the beat array) for a loop that cannot drift and a wrap
             * handled in one tested function. Well under a percent of a core,
             * and only while a slot is armed. */
            int64_t k;

            switch (g.part) {
            case STEM_PART_DRUMS:
                for (k = 0; k < n; k++) {
                    float fL, fR;

                    gc_frame(g.pcm, ph.fl.span, gc_phase_at(&ph, pos + k),
                             &fL, &fR);
                    s[k * 2]     = d * fL + gh * sh * (float)hp[k * 2]
                                          + gv * sv * (float)wp[k * 2];
                    s[k * 2 + 1] = d * fR + gh * sh * (float)hp[k * 2 + 1]
                                          + gv * sv * (float)wp[k * 2 + 1];
                }
                break;
            case STEM_PART_HARMONICS:
                for (k = 0; k < n; k++) {
                    float fL, fR;

                    gc_frame(g.pcm, ph.fl.span, gc_phase_at(&ph, pos + k),
                             &fL, &fR);
                    s[k * 2]     = d * (s[k * 2]
                                        - sh * (float)hp[k * 2]
                                        - sv * (float)wp[k * 2])
                                 + gh * fL + gv * sv * (float)wp[k * 2];
                    s[k * 2 + 1] = d * (s[k * 2 + 1]
                                        - sh * (float)hp[k * 2 + 1]
                                        - sv * (float)wp[k * 2 + 1])
                                 + gh * fR + gv * sv * (float)wp[k * 2 + 1];
                }
                break;
            default:
                for (k = 0; k < n; k++) {
                    float fL, fR;

                    gc_frame(g.pcm, ph.fl.span, gc_phase_at(&ph, pos + k),
                             &fL, &fR);
                    s[k * 2]     = d * (s[k * 2]
                                        - sh * (float)hp[k * 2]
                                        - sv * (float)wp[k * 2])
                                 + gh * sh * (float)hp[k * 2] + gv * fL;
                    s[k * 2 + 1] = d * (s[k * 2 + 1]
                                        - sh * (float)hp[k * 2 + 1]
                                        - sv * (float)wp[k * 2 + 1])
                                 + gh * sh * (float)hp[k * 2 + 1] + gv * fR;
                }
                break;
            }
        }
    }
    if (have_beats)
        stem_grid_beats_release();
    if (have_gc)
        gc_release();
    stem_store_release();
}
