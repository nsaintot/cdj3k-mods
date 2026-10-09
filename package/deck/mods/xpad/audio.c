// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * xpad/audio.c - eight voices, read at the pad's own rate and summed after the
 * stretcher.
 *
 * Two separate clocks:
 *
 *   The voice runs free. Its playhead advances one frame per output frame
 *   whatever the deck is doing, so a sample sounds on a paused deck, keeps its
 *   length when the tempo fader moves, and is pitched only by the X-PAD's Y axis.
 *
 *   The roll runs on the track. Re-triggers follow the track's beat grid, so a
 *   1/8 roll lands on the track's eighths at any tempo, and a paused deck has no
 *   boundaries to cross.
 *
 * Neither needs the sample's BPM, so the banks need no config file, unlike
 * GROOVE CIRCUIT's loops.
 *
 * The pitch shift preserves duration, as on the RMX-1000 (not varispeed). The
 * playhead runs at real time and the pitch comes from two read heads that move
 * back through the sample and wrap one window at a time; the crossfade between
 * them hides the wrap. A two-second sample lasts two seconds at any pitch. See
 * xp_heads for the mechanism and its cost.
 *
 * No value that grows with uptime is stored as a float. The playhead is a count
 * of whole frames, the wrap phase is a Q32 unsigned that wraps by overflow, and
 * the only float in the read path is a delay bounded by one window. A float
 * index with a sub-sample fraction loses resolution as it grows: the spacing is
 * already 1/256 sample after a third of a second at 96 kHz and doubles with
 * every doubling of uptime, so the output decays into aliasing.
 *
 * Threads. [audio] owns everything below the trigger line and may not allocate,
 * lock or log; [deck] fires and releases pads; [message] silences on close.
 */
#include "xpad/xpad.h"
#include "xpad/ext.h"
#include "stem/stem.h"
#include "stem/loop.h"       /* stem_beat_at: the grid, as a beat index */

#include <math.h>

/* ---- a voice --------------------------------------------------------------
 *
 * [audio] only. The deck never touches a voice: it sets a bit in xpad_g_pend and
 * the mix starts the voice on the next block, so a press cannot half-arrive. */
struct xp_voice {
    uint64_t pos;           /* the playhead: whole frames, one per output frame */
    uint32_t phi;           /* the wrap phase both heads follow, Q32 */
    float    ratio;         /* the bend this shot is playing at; see the mix */
    float    win;           /* its own crossfade window, in frames */
    int32_t  start;         /* which frame of this block it begins on */
    int      fresh;         /* started this block; the bend is not seeded yet */
    int      sounding;
};

static struct xp_voice xpad_g_voice[XP_BANKS];

/* "Stop everything", which the pending bitmask cannot express. [message] sets,
 * [audio] takes. */
static int xpad_g_hush;

/* What reached the output: blocks our sum was added to, and the peak of our own
 * contribution, not the block, so a loud track cannot hide a sample that never
 * played. [audio] accumulates, [message] reads and clears. A lost update costs
 * one window, so there is no lock. */
static unsigned xpad_g_mix_blocks;
static float    xpad_g_mix_peak;
static unsigned xpad_g_mix_fires;
static unsigned xpad_g_fired;       /* [deck] side of the same count */

/* The one-pole coefficient for the bend's glide, and the block length and rate
 * it was computed for. The glide state is per voice; the coefficient is shared
 * because it depends only on block length and rate. */
static float    xpad_g_pole;
static int64_t  xpad_g_pole_len;
static int      xpad_g_pole_rate;

/* ---- the clock ------------------------------------------------------------
 *
 * The tempo comes from the track, but the clock does not stop with the
 * transport, so the sampler can be played over a paused deck.
 *
 * Two sources, chosen every block:
 *
 *   The track, whenever the play head advanced. The beat position comes from
 *   the grid, so the roll sits on the track's eighths at any tempo, and the
 *   position is absolute, so it cannot drift.
 *
 *   Our own, once the play head has been still long enough to mean stopped
 *   rather than between pulls: `frames / spb` at the track's tempo.
 *
 * XP_STILL_BLOCKS separates the two. A burst gap is a handful of blocks and
 * this is about 20 ms, so a roll on a parked deck starts imperceptibly late and
 * a roll on a playing deck never switches to the free-running branch.
 *
 * There is one head per point in the pipeline. The sampler sums into what the
 * stretcher has just written; the stems' mute edits what it is about to read.
 * On a playing deck the read runs 8055..9271 frames ahead of the write (84 to
 * 97 ms at 96 kHz, varying by up to 13 ms between blocks). A shared head would
 * put one of them about 0.1 s off the grid. Each head is advanced by the
 * position of the audio its own caller touches. */
#define XP_STILL_BLOCKS 30

struct xp_head {
    int64_t last_pos;       /* -1 before the first block */
    int     still;
    double  beat;           /* carried, so a parked deck goes on counting */
};

static struct xp_head xpad_g_out = { -1, 0, 0.0 };  /* what is being written */
static struct xp_head xpad_g_in  = { -1, 0, 0.0 };  /* what is being read    */

/* Which route the beat came from this block: 0 none, 1 the flat samples-per-
 * beat, 2 the track's own beat array. Reported, because a roll that plays one
 * hit and stops has no clock. */
static int xpad_g_clock_kind;

/* Presses waiting for the next block, one bit per bank. [deck] sets, [audio]
 * takes. A bitmask because two presses of the same pad in one block are one
 * hit; it is the whole handoff, since the deck never touches a voice. */
static unsigned xpad_g_pend;

/* Which brick the roll last saw, so the block where a zone first takes the
 * finger can be told from the ones after it. [audio] only; reset when the panel
 * shuts. */
static int xpad_g_roll_div = XP_DIV_NONE;

/* The boundary an off-grid hit replaced, and whether it is still pending. See
 * xp_roll: a hit a few milliseconds from a boundary takes that boundary's
 * place. */
static int64_t xpad_g_roll_claim;
static int     xpad_g_roll_claim_ok;

/* [audio] The off-grid hit that just sounded claims the nearest boundary of `L`
 * beats, so the roll does not replay it a moment later.
 *
 * Nearest, not the one below: a hit can land either side of the boundary it was
 * aimed at. This keeps the gap to the next hit between 0.5 and 1.5 divisions;
 * rounding down would allow anything from 0 to 1. */
static void xp_claim_boundary(double beat, double L)
{
    if (!(L > 0.0))
        return;
    xpad_g_roll_claim    = (int64_t)floor(beat / L + 0.5);
    xpad_g_roll_claim_ok = 1;
}

/* ---- firing (deck) -------------------------------------------------------- */

/* The block being mixed, so a shot can start inside it rather than at its edge.
 * A block is 64 frames of the ALSA period. [audio] only, set once per mix. */
static int64_t xpad_g_blk_frames;
static double  xpad_g_blk_b0, xpad_g_blk_b1;

/* Which frame of this block a beat falls on. A beat outside the block maps to
 * now. */
static int32_t xp_frame_of(double beat)
{
    double span = xpad_g_blk_b1 - xpad_g_blk_b0;
    double k;

    if (!(span > 0.0) || xpad_g_blk_frames <= 0)
        return 0;
    k = (beat - xpad_g_blk_b0) / span * (double)xpad_g_blk_frames;
    if (!(k > 0.0))
        return 0;
    if (k >= (double)xpad_g_blk_frames)
        return (int32_t)xpad_g_blk_frames - 1;
    return (int32_t)k;
}

int xpad_g_sel = -1;

/* Select and fire. The selection is what the X-PAD rolls; the shot is handed to
 * the next block because the deck thread does not own a voice. Not quantized,
 * and there is no release. */
void xpad_fire(int bank)
{
    if (bank < 0 || bank >= XP_BANKS)
        return;
    __atomic_store_n(&xpad_g_sel, bank, __ATOMIC_RELAXED);
    __atomic_or_fetch(&xpad_g_pend, 1u << bank, __ATOMIC_RELAXED);
    __atomic_add_fetch(&xpad_g_fired, 1, __ATOMIC_RELAXED);
}

/* The bar's and the roll's trigger. Not xpad_fire: presses posted there are
 * recorded by the mix (xp_pending), so the bar would record its own replay and
 * grow every pass. */
void xpad_seq_fire_at(int bank, double beat)
{
    struct xp_voice *v;

    if (bank < 0 || bank >= XP_BANKS)
        return;
    v = &xpad_g_voice[bank];
    v->pos      = 0;
    v->phi      = 0;
    v->start    = xp_frame_of(beat);
    v->fresh    = 1;        /* the mix seeds the bend: the strip is read there */
    v->sounding = 1;
    __atomic_add_fetch(&xpad_g_mix_fires, 1, __ATOMIC_RELAXED);
}

void xpad_seq_fire(int bank)
{
    xpad_seq_fire_at(bank, xpad_g_blk_b0);
}

void xpad_silence(void)
{
    __atomic_store_n(&xpad_g_pend, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&xpad_g_hush, 1, __ATOMIC_RELEASE);
}

/* Three pad states; the X-PAD owns all eight pads while the panel is open.
 *
 * Green lit: the selected bank, or any bank sounding, whether a finger, the
 * roll or the bar started it (so OVERDUB replays show on the pads). Green dim:
 * loaded and quiet. Grey: no sample.
 *
 * An empty pad must still return 1. Returning 0 hands it back to the app, which
 * lights it in its HOT CUE colour, so it looks like a loaded bank.
 *
 * Returns 0 only when the panel is shut, so the pads revert to hot cues with
 * the app's own lamps. */

int xpad_pad_lamp(int pad, struct lamp *out)
{
    if (!xpad_open() || pad < 0 || pad >= XP_BANKS)
        return 0;

    if (!xpad_bank_ready(pad)) {
        /* White at the dim level, which this panel renders as near-neutral
         * grey: the colour the deck uses for a pad with no cue. */
        lamp_set(out, 255, 255, 255, LAMP_DIM);
        return 1;
    }
    lamp_set(out, 0, 255, 0,
             (pad == __atomic_load_n(&xpad_g_sel, __ATOMIC_RELAXED) ||
              xpad_voice_lit(pad)) ? LAMP_LIT : LAMP_DIM);
    return 1;
}

int xpad_voice_lit(int bank)
{
    if (bank < 0 || bank >= XP_BANKS)
        return 0;
    return __atomic_load_n(&xpad_g_voice[bank].sounding, __ATOMIC_RELAXED);
}

void xpad_mix_stat(struct xpad_mix_stat *out)
{
    out->blocks = __atomic_exchange_n(&xpad_g_mix_blocks, 0, __ATOMIC_RELAXED);
    out->fires  = __atomic_exchange_n(&xpad_g_mix_fires,  0, __ATOMIC_RELAXED);
    out->fired  = __atomic_exchange_n(&xpad_g_fired,      0, __ATOMIC_RELAXED);
    out->peak   = xpad_g_mix_peak;
    xpad_g_mix_peak = 0.0f;
    out->clock  = xpad_g_clock_kind;
    out->beat   = xpad_g_out.beat;
    out->span   = xpad_g_blk_b1 - xpad_g_blk_b0;
}

/* ---- the beat clock (audio) -----------------------------------------------
 *
 * The track's fractional beat index at a pool position, by whichever route the
 * track supports. The beat array handles a varying tempo or a hand-edited grid;
 * a single samples-per-beat covers a steady tempo and is all an un-analysed grid
 * offers. A track with no grid has neither, so there is no roll and no bar, and
 * the pads still work as one-shots.
 *
 * `beats`/`count` are borrowed for the block by the caller, so this only reads. */
struct xp_clock {
    const int64_t *beats;
    int32_t        count;
    double         spb;         /* the flat route, 0 when the array is in use */
    int64_t        beat0;       /* ...and where its beat 0 is */
    int32_t        cursor;
};

/* The flat route counts from beat0, not from zero. Pool position 0 is the start
 * of the file, not a grid line, so pos/spb puts every boundary a constant
 * beat0/spb off the visible grid (beat0 = 4483 at 148 BPM is 0.1152 beat,
 * 47 ms). The array route needs no offset: its cell 0 is that
 * beat. */
static double xp_beat_at(struct xp_clock *c, int64_t pos)
{
    if (c->beats)
        return stem_beat_at(c->beats, c->count, pos, &c->cursor);
    if (c->spb > 0.0)
        return ((double)pos - (double)c->beat0) / c->spb;
    return 0.0;
}

static int xp_clock_ok(const struct xp_clock *c)
{
    return c->beats != NULL || c->spb > 0.0;
}

/* Advance `h` to `pos` and return the span of beats this block covers.
 *
 * The span is [pos, pos + adv), where `adv` is how far the source moved over the
 * last block: the block's output length scaled by the tempo. A seek needs no
 * detection; it just moves the span, and a span is never longer than a block.
 *
 * `adv` is clamped, since the difference between two positions may be a seek,
 * and falls back to the output length, which is correct at 1.0x and before the
 * first block. */
static void xp_head_step(struct xp_head *h, struct xp_clock *k, int64_t pos,
                         int64_t frames, double *b0, double *b1)
{
    int64_t prev = h->last_pos;
    int64_t adv  = 0;

    if (pos >= 0) {
        if (prev >= 0 && pos > prev)
            adv = pos - prev;
        h->last_pos = pos;
    }
    if (adv <= 0 || adv > frames * 8)
        adv = frames;

    if (pos >= 0 && prev >= 0 && pos != prev) {
        h->still = 0;
        *b0 = xp_beat_at(k, pos);
        *b1 = xp_beat_at(k, pos + adv);
        if (!(*b1 > *b0))
            *b1 = *b0;          /* a grid that does not move here */
    } else if (h->still < XP_STILL_BLOCKS) {
        h->still++;             /* between pulls, not stopped */
        *b0 = h->beat;
        *b1 = *b0;
    } else if (k->spb > 0.0) {
        /* Parked: our own clock, continuing from where the track stopped so
         * the phase is preserved. */
        *b0 = h->beat;
        *b1 = *b0 + (double)frames / k->spb;
    } else {
        *b0 = h->beat;
        *b1 = *b0;
    }
    h->beat = *b1;
}

/* ---- the read heads ------------------------------------------------------- */

/* The crossfade window, computed inline with no table.
 *
 * The two branches meet C0-continuously at x = 0.28348, where both evaluate to
 * 0.632145 = 1 - 1/e. The x^2 (1 - 2x) form must be the lower segment, since it
 * goes negative past x = 0.5. */
static inline float xp_xfade_window(float x)
{
    if (x < 0.28348f)
        return 18.16515f * x * x * (1.0f - 2.0f * x);
    {
        float u = 1.0f - x;
        return 1.0f - u * u * u;
    }
}

/* One block's pitch parameters. Built per voice, because each voice keeps the
 * bend it was released at. */
struct xp_pitch {
    float    w;             /* the crossfade window, in frames                  */
    float    wq;            /* ...times 2^-32, so a Q32 phase reads out in frames */
    float    half;          /* the anchor: half a window, in frames             */
    uint32_t step;          /* what the phase advances by per frame, wrapping    */
};

/* One head, `d` frames from the playhead, linearly interpolated. `d` is signed:
 * the anchor puts up to half a window of it ahead of the playhead (see
 * xp_heads).
 *
 * Linear on purpose: its position-dependent lowpass is part of the sound.
 *
 * Reads off either end of the sample are silent, not clamped; holding the last
 * frame would leave a DC step until the voice ends. This happens on every shot,
 * since the heads straddle the playhead by half a window either way. */
static inline void xp_tap(const int16_t *pcm, int64_t frames, int64_t base,
                          float d, float *l, float *r)
{
    const float q = 1.0f / 32767.0f;
    int32_t  di = (int32_t)floorf(d);
    float    df = d - (float)di;
    int64_t  i  = base - di;
    float    a, b;

    if (i < 0 || i >= frames) {
        *l = 0.0f;
        *r = 0.0f;
        return;
    }
    /* The frame before the first is silence, not out of range. Refusing i == 0
     * would drop the first frame of every shot, audible on gated one-shots. */
    if (i >= 1) {
        a  = (float)pcm[i * 2];
        b  = (float)pcm[(i - 1) * 2];
        *l = q * (a + df * (b - a));
        a  = (float)pcm[i * 2 + 1];
        b  = (float)pcm[(i - 1) * 2 + 1];
        *r = q * (a + df * (b - a));
    } else {
        *l = q * (1.0f - df) * (float)pcm[0];
        *r = q * (1.0f - df) * (float)pcm[1];
    }
}

/* Both heads, one frame, mixed. This is the whole pitch engine.
 *
 * The playhead advances at real time, never at the pitch ratio. The delay moves
 * at the ratio: head A sits `phi` of a window behind the playhead and phi moves
 * at (1 - r) / W, so head A's material advances at r while the timeline does
 * not. Pitching up shrinks the delay to zero and wraps it back a whole window,
 * repeating a window of material; pitching down grows it to a window and wraps
 * it to zero, skipping one. Neither direction is special-cased.
 *
 * Two heads half a phase apart. A head reads a given point of the sample once
 * per wrap, W/r apart; the second head's reads fall halfway between, so copies
 * of one transient come out W/(2r) apart. At full pitch a short burst comes
 * out as three copies 6.67 ms apart; one head would give two copies 13.3 ms
 * apart. The half phase is the
 * top bit of the phase word.
 *
 * For the same reason the comb spacing is 2|1 - r| / W, not |1 - r| / W: the two
 * heads wrap alternately, so the pattern repeats twice per phase cycle.
 *
 * Anchored half a window forward, which makes the engine transparent at unity.
 * A head is silent at both ends of its sweep, so the loud head is always near
 * the middle, which would delay every shot by half a window. Subtracting that
 * half from both delays makes the taps straddle the playhead: at unity on an
 * unbent voice, head B reads the playhead exactly and head A is silent, so the
 * sample plays bit for bit where it was fired. With a bend the onset moves up to
 * half a window either side of the beat. A fixed delay would be invisible on a
 * running loop, but these are one-shots fired on a grid.
 *
 * The gains are complementary and each head is silent at its own wrap, so the
 * wrap is inaudible: only one head jumps at a time, at zero gain, while the
 * other carries the signal. The window is a fade-in curve: over each half of
 * the phase the head about to wrap fades out by 1 - w while the other fades in
 * by w, and they swap roles at the half.
 *
 * Cost: duration is preserved on average, not per event. Within a wrap the read
 * runs at r, so a transient is scaled by 1/r. Pitching up gives each transient
 * a triple flam of compressed copies; pitching down stretches it into one.
 * Onsets move by about 9 ms across the range. */
static inline void xp_heads(const int16_t *pcm, int64_t frames, int64_t base,
                           uint32_t phi, const struct xp_pitch *p,
                           float *l, float *r)
{
    float ga, gb, al, ar, bl, br;

    xp_tap(pcm, frames, base, (float)phi * p->wq - p->half, &al, &ar);
    xp_tap(pcm, frames, base,
           (float)(phi + 0x80000000u) * p->wq - p->half, &bl, &br);

    ga = xp_xfade_window((float)(phi << 1) * (1.0f / 4294967296.0f));
    if (phi & 0x80000000u)
        ga = 1.0f - ga;
    gb = 1.0f - ga;

    *l = al * ga + bl * gb;
    *r = ar * ga + br * gb;
}

/* ---- the mix -------------------------------------------------------------- */

/* The pad's state for one block, read once so it cannot change mid-block. */
struct xp_gesture {
    int   div;              /* XP_DIV_NONE when nothing is engaged */
    float semis;
};

static void xp_gesture_read(struct xp_gesture *g)
{
    g->div   = XP_DIV_NONE;
    g->semis = 0.0f;
    if (!xpad_g_open)
        return;

    /* The gesture is live only while a finger is on it, or with HOLD set. The
     * strip's values can briefly outlive both (HOLD going off is cleared on the
     * next display tick), so whether they act is decided here.
     *
     * A finger takes priority over the bar: OVERDUB's recorded gesture plays
     * the strip only while nobody is touching it. */
    if (xpad_gesture_live()) {
        g->div   = xpad_g_touch.div;
        g->semis = xpad_g_touch.semis;
        return;
    }
    xpad_seq_auto(&g->div, &g->semis);
}

/* The bend's one-pole glide as a per-block coefficient, derived from the fixed
 * time constant. Recomputed only when the block length or the rate changes. */
static float xp_pole(int64_t len, int rate)
{
    if (len == xpad_g_pole_len && rate == xpad_g_pole_rate)
        return xpad_g_pole;
    xpad_g_pole_len  = len;
    xpad_g_pole_rate = rate;
    xpad_g_pole = (rate > 0)
        ? expf(-((float)len / (float)rate) / (XP_PITCH_TAU_MS * 0.001f))
        : 0.0f;
    return xpad_g_pole;
}

/* The block's pitch: the window it wraps at, and the phase step that moves the
 * delay through it.
 *
 * The window is a constant per direction, and the two differ (see
 * XP_XFADE_UP_MS). It glides between them on the bend's time constant, because
 * W scales both tap positions and a step would click at every crossing of
 * unity. The glide drags the taps by up to half the difference over one time
 * constant, a fraction of a semitone during a sweep.
 *
 * The step is signed but kept in the phase's unsigned arithmetic, so the wrap is
 * an overflow in both directions with no modulo or branch. At unity it is zero
 * and the wrap is inert, so a sine at centre comes back with no comb. */
static void xp_pitch_block(struct xp_pitch *out, float *win, float ratio,
                           float a, int rate)
{
    float target = (ratio >= 1.0f ? XP_XFADE_UP_MS : XP_XFADE_DOWN_MS)
                 * 0.001f * (float)rate;

    if (!(*win > 0.0f))
        *win = target;                  /* a fresh shot, or after a rate change */
    *win = a * *win + (1.0f - a) * target;

    out->w    = *win;
    out->wq   = *win * (1.0f / 4294967296.0f);
    out->step = (uint32_t)(int32_t)((1.0 - (double)ratio) / (double)*win
                                    * 4294967296.0);
    /* The anchor is fixed, not half of the gliding window: it puts the unbent
     * read exactly on the playhead, and a moving anchor would drag both taps.
     * Half of the up window, because unity resolves to that one. */
    out->half = XP_XFADE_UP_MS * 0.0005f * (float)rate;
}

/* Does a whole multiple of `L` beats fall in (b0, b1]?
 *
 * Compares the multiple's index, so there is no modulo of a growing number.
 * Half-open at the low end, so a boundary exactly on a block edge fires once. */
static int xp_crossed(double b0, double b1, double L)
{
    if (!(b1 > b0) || !(L > 0.0))
        return 0;
    return floor(b1 / L) > floor(b0 / L);
}

/* The clock, published as a position, not an edge.
 *
 * The sampler's own snapping tests whether a boundary fell inside this block,
 * which works because the same call advances the clock. Other callers cannot do
 * that: the pre-stretch mix runs at its own cadence (the stretcher pulls its
 * source in bursts), and on most blocks the play head has not moved, so the span
 * is empty. An edge test there may never fire.
 *
 * With a position, the reader compares the boundary it falls in against the one
 * it last saw and acts on the first call after it changes. */
static double xpad_g_beat_pub;
static int    xpad_g_beat_pub_ok;

int xpad_beat_now(double *beat)
{
    if (!xpad_g_beat_pub_ok)
        return 0;
    *beat = xpad_g_beat_pub;
    return 1;
}

/* The roll re-triggers the selected bank, i.e. whatever the pads last picked.
 * It goes through xpad_seq_fire so OVERDUB does not record it.
 *
 * The first hit of a zone is immediate; the repeats are on the grid. Sliding
 * into another zone counts as a new first hit, so a sweep across the bricks
 * stutters immediately instead of waiting for a boundary per zone.
 *
 * The repeats follow the absolute grid, not the first hit, so a roll started
 * late is in time from its second hit.
 *
 * The first hit claims the nearest boundary and the roll skips it. Otherwise a
 * touch a few milliseconds before a boundary fires twice in that gap. */
static void xp_roll(double b0, double b1, int div)
{
    int sel   = __atomic_load_n(&xpad_g_sel, __ATOMIC_RELAXED);
    int fresh = div != xpad_g_roll_div;
    double L;

    xpad_g_roll_div = div;
    if (sel < 0 || sel >= XP_BANKS || div < 0 || div >= XP_BRICKS)
        return;
    L = (double)xpad_div_beats[div];

    if (fresh) {
        xp_claim_boundary(b1, L);
        xpad_seq_fire(sel);
        return;
    }
    if (!xp_crossed(b0, b1, L))
        return;
    if (xpad_g_roll_claim_ok && (int64_t)floor(b1 / L) == xpad_g_roll_claim) {
        xpad_g_roll_claim_ok = 0;       /* the touch already played this one */
        return;
    }
    xpad_g_roll_claim_ok = 0;
    /* On the boundary's own frame inside this block, not the block's edge. */
    xpad_seq_fire_at(sel, floor(b1 / L) * L);
}

/* The strip's gesture, recorded while OVERDUB is armed so a sweep becomes part
 * of the loop. Sampled on the quantize grid, or XP_QUANTIZE_PAD when quantize is
 * off.
 *
 * Recorded only when it has moved, so a still finger costs nothing and a sweep
 * costs one event per audible step. The lift is recorded too, as XP_DIV_NONE,
 * or the bar would hold the last value forever. */
static void xp_automate(double b0, double b1)
{
    static int   was_div = XP_DIV_NONE;
    static float was_semis;
    int   div   = xpad_g_touch.held ? xpad_g_touch.div : XP_DIV_NONE;
    float semis = xpad_g_touch.held ? xpad_g_touch.semis : 0.0f;
    float d;
    int   q;

    if (!xpad_g_overdub || !xpad_g_open) {
        was_div = XP_DIV_NONE;
        was_semis = 0.0f;
        return;
    }
    q = xpad_quantize_div();
    if (!q)
        q = XP_QUANTIZE_PAD;    /* quantize off: still needs a sampling rate */
    if (!xp_crossed(b0, b1, 1.0 / (double)q))
        return;

    d = semis - was_semis;
    if (d < 0.0f) d = -d;
    if (div == was_div && (div == XP_DIV_NONE || d < XP_AUTO_ST))
        return;
    was_div   = div;
    was_semis = semis;
    xpad_seq_record_pad(div, semis, b1);
}

/* Start the presses the deck posted; [audio] owns the voices. Taken on the
 * next block, since a pad is not quantized. */
static void xp_pending(double beat)
{
    unsigned pend, i;

    pend = __atomic_exchange_n(&xpad_g_pend, 0, __ATOMIC_RELAXED);
    if (!pend)
        return;
    /* A pad press also claims a boundary, as a touchdown does, so a press next
     * to a boundary during a roll does not double. It claims on the grid of the
     * brick the roll is on. */
    if (xpad_g_roll_div >= 0 && xpad_g_roll_div < XP_BRICKS)
        xp_claim_boundary(beat, (double)xpad_div_beats[xpad_g_roll_div]);

    for (i = 0; i < XP_BANKS; i++) {
        if (!(pend & (1u << i)))
            continue;
        xpad_seq_fire((int)i);
        /* Recorded here, when it sounds, so the bar replays it at the phase it
         * was heard rather than when the press was posted. */
        xpad_seq_record((int)i, beat);
    }
}

void xpad_mix(float *dst, int64_t frames, int64_t pos)
{
    struct stem_grid_view gv;
    struct xp_clock clk = { NULL, 0, 0.0, 0, -1 };
    struct xp_gesture g;
    float target, a, vol;
    int rate, i, engaged = 0, any = 0, have_beats = 0, mixed = 0, have_now = 0;
    double b0 = 0.0, b1 = 0.0, pub0 = 0.0, pub1 = 0.0;

    if (!dst || frames <= 0)
        return;

    if (__atomic_exchange_n(&xpad_g_hush, 0, __ATOMIC_ACQUIRE)) {
        for (i = 0; i < XP_BANKS; i++)
            xpad_g_voice[i].sounding = 0;
        xpad_g_roll_div = XP_DIV_NONE;   /* the next zone is a fresh one */
        xpad_g_roll_claim_ok = 0;
    }

    rate = stem_pool_rate();
    if (rate <= 0)
        return;

    /* The clock (see XP_STILL_BLOCKS): the track's beat position while the play
     * head moves, our own at the track's tempo once it stops. Either way it is
     * the track's beat index, so boundaries match the grid on screen.
     *
     * `pos` is this block's place in the track, passed in by the caller: the
     * sampler sums into post-stretch audio, and the stretcher's last read
     * position is up to 0.1 s further on. Advanced even when there is nothing
     * to mix, because the span is a difference: skipping quiet blocks would
     * make the first block after a press span the whole time the panel was
     * shut, and the bar would fire every event at once. */
    clk.spb   = stem_grid_spb();
    clk.beat0 = stem_grid_beat0();
    if (stem_grid_beats_acquire(&gv)) {
        have_beats = 1;
        clk.beats  = gv.beats;
        clk.count  = gv.count;
    }
    xpad_g_clock_kind = clk.beats ? 2 : clk.spb > 0.0 ? 1 : 0;
    if (xp_clock_ok(&clk)) {
        xp_head_step(&xpad_g_out, &clk, pos, frames, &b0, &b1);
        /* The other end of the pipeline, for callers that edit the block the
         * stretcher is about to read. */
        xp_head_step(&xpad_g_in, &clk, stem_source_pos(), frames, &pub0, &pub1);
        have_now = 1;
    }
    xpad_g_blk_frames = frames;
    xpad_g_blk_b0     = b0;
    xpad_g_blk_b1     = b1;
    /* Released as soon as the clock is read; nothing below touches the array.
     * Holding it longer risks a deadlock on track change: the writer unpublishes
     * the array and spins until every reader has gone, so a reader leaked by an
     * early-out below stalls the deck thread, and the deck's controls go dead
     * while the screen keeps drawing. */
    if (have_beats) {
        stem_grid_beats_release();
        have_beats = 0;
    }

    /* Published before the sampler's early-out, because the stem row's mute uses
     * this clock whether or not the X-PAD is active. The in head, because a mute
     * applies to the block the stretcher is about to read. */
    xpad_g_beat_pub    = pub1;
    xpad_g_beat_pub_ok = have_now;

    /* Anything to do? A voice sounding, a press pending, a bar to replay, or a
     * finger on the strip (the roll must run before anything is audible). */
    for (i = 0; i < XP_BANKS; i++)
        if (xpad_g_voice[i].sounding) {
            any = 1;
            break;
        }
    if (!any && !__atomic_load_n(&xpad_g_pend, __ATOMIC_RELAXED) &&
        xpad_seq_count() == 0 &&
        !(xpad_g_open && xpad_g_touch.div != XP_DIV_NONE))
        return;

    /* The bar first, then the roll, then new presses, so a fresh press wins a
     * boundary it shares with either.
     *
     * The gesture is read after the bar, whose replay may have just moved it,
     * so automation rolls in the block it arrives. */
    if (b1 > b0)
        xpad_seq_play(b0, b1);
    xp_gesture_read(&g);

    /* The strip's target ratio and whether it is engaged. The glide is per
     * voice, in the loop below. */
    target  = exp2f(g.semis / 12.0f);
    engaged = g.div != XP_DIV_NONE;
    a       = xp_pole(frames, rate);

    /* Not under the b1 > b0 guard: a zone's first hit fires immediately even if
     * the clock has not moved. The crossing test inside checks the span. */
    xp_roll(b0, b1, g.div);
    /* After the replay, so a live finger's sample overwrites the automation the
     * bar just replayed, not the reverse. */
    if (b1 > b0)
        xp_automate(b0, b1);
    xp_pending(b1);

    vol = (float)__atomic_load_n(&xpad_g_vol, __ATOMIC_RELAXED) * 0.01f;
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;

    for (i = 0; i < XP_BANKS; i++) {
        struct xp_voice *v = &xpad_g_voice[i];
        struct xpad_bank_view bv;
        struct xp_pitch pitch;
        float pk = xpad_g_mix_peak;
        int64_t k, tail;
        uint64_t ph;
        uint32_t phi;

        if (!v->sounding)
            continue;
        if (!xpad_bank_acquire(i, &bv)) {
            v->sounding = 0;        /* the stick went, or the table is rebuilding */
            continue;
        }

        /* A shot keeps the bend it was played at. The strip moves a voice only
         * while engaged (finger, HOLD or the bar); once released, sounding
         * voices keep their pitch and ring out instead of sliding back to
         * unity. The next shot is seeded from the strip, so a pad hit with no
         * finger down plays at unity.
         *
         * A fresh shot takes the target without gliding, so the attack is at
         * the right pitch. */
        if (v->fresh) {
            v->fresh = 0;
            v->ratio = engaged ? target : 1.0f;
            v->win   = 0.0f;
        } else if (engaged) {
            v->ratio = a * v->ratio + (1.0f - a) * target;
        }
        xp_pitch_block(&pitch, &v->win, v->ratio, a, rate);

        /* The playhead runs past the last frame by the trailing head's delay,
         * which is still playing the end of the sample; stopping at the last
         * frame would cut the tail. */
        tail = bv.frames + (int64_t)(pitch.w - pitch.half) + 2;
        ph   = v->pos;
        phi  = v->phi;
        /* From the frame the boundary fell on, not the block's edge (see
         * xp_frame_of): a block is 64 frames, so rounding down would play up
         * to 0.67 ms early. */
        for (k = v->start; k < frames; k++) {
            float sl, sr, a;

            if ((int64_t)ph >= tail) {
                v->sounding = 0;
                break;
            }
            xp_heads(bv.pcm, bv.frames, (int64_t)ph, phi, &pitch, &sl, &sr);
            phi += pitch.step;
            ph++;
            sl *= vol;
            sr *= vol;
            dst[k * 2]     += sl;
            dst[k * 2 + 1] += sr;
            a = sl < 0.0f ? -sl : sl;
            if (a > pk) pk = a;
            a = sr < 0.0f ? -sr : sr;
            if (a > pk) pk = a;
        }
        v->pos   = ph;
        v->phi   = phi;
        v->start = 0;           /* the offset applies to this block only */
        xpad_bank_release();
        xpad_g_mix_peak = pk;
        mixed = 1;
    }
    if (mixed)
        __atomic_add_fetch(&xpad_g_mix_blocks, 1, __ATOMIC_RELAXED);
}
