// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * codec.c - the CDJ-3000 3-band column codec.
 *
 * The deck's 3-band request ticket parses a PWV7 blob, which carries three
 * independent 7-bit band amplitudes per column, by sorting them descending into
 * h0 >= h1 >= h2 and recording the permutation in a header.
 *
 *     +0  u16 header   nibble k = colour index of ring k, nibble 3 = ring count
 *     +2  h0   +3  h1   +4  h2   +5  spare
 *
 * A colour index is the set of bands reaching that ring's height, so the sets
 * nest: ring 0 is the tallest band alone and each ring inward adds the next one
 * down. A band's amplitude is the height of the first ring that names it. Ties
 * collapse into one ring, so silence is 0x1008.
 *
 * Encoding re-sorts and rebuilds the header, so a fader move only scales the
 * three amplitudes; the ring colours follow from the new ordering.
 */
#include "wave/wave.h"

#include <math.h>

/* colour index -> bitmask of bands reaching that ring (bit0 low, 1 mid, 2 high) */
static const uint8_t k_set_of[16] = {
    /* 0 */ 0,
    /* 1 */ 4,          /* high           */
    /* 2 */ 2,          /* mid            */
    /* 3 */ 1,          /* low            */
    /* 4 */ 6,          /* mid + high     */
    /* 5 */ 5,          /* low + high     */
    /* 6 */ 3,          /* low + mid      */
    /* 7 */ 0,
    /* 8 */ 7,          /* all three      */
    0, 0, 0, 0, 0, 0, 0,
};

/* the inverse: band bitmask -> colour index */
static const uint8_t k_index_of[8] = { 0, 3, 2, 6, 1, 5, 4, 8 };

/* PWV7 amplitudes are 7-bit; a scaled band saturates here instead of
 * overflowing. */
#define BAND_MAX 127

void mod_wave_decode(uint16_t hdr, const uint8_t *h, uint8_t *out)
{
    int count = (hdr >> 12) & 0xF;
    int seen = 0, k, b;

    out[0] = out[1] = out[2] = 0;
    if (count > 3)
        count = 3;
    for (k = 0; k < count; k++) {
        int set = k_set_of[(hdr >> (4 * k)) & 0xF];

        for (b = 0; b < 3; b++) {
            if ((set & (1 << b)) && !(seen & (1 << b))) {
                out[b] = h[k];      /* first ring naming the band wins */
                seen |= 1 << b;
            }
        }
    }
}

void mod_wave_encode(const uint8_t *bands, uint16_t *hdr_out, uint8_t *h)
{
    uint8_t lv[3];
    int n = 0, i, k, hdr = 0;

    /* The distinct heights, descending (selection sort over three items). */
    for (i = 0; i < 3; i++) {
        int dup = 0, j;

        for (j = 0; j < n; j++) {
            if (lv[j] == bands[i]) {
                dup = 1;
                break;
            }
        }
        if (!dup)
            lv[n++] = bands[i];
    }
    for (i = 0; i < n; i++) {
        for (k = i + 1; k < n; k++) {
            if (lv[k] > lv[i]) {
                uint8_t t = lv[i];

                lv[i] = lv[k];
                lv[k] = t;
            }
        }
    }

    /* A band at zero gets no ring, unless zero is the only height (silence is
     * a one-ring column). */
    if (n > 1 && lv[n - 1] == 0)
        n--;

    h[0] = h[1] = h[2] = 0;
    for (k = 0; k < n; k++) {
        int reaching = 0;

        for (i = 0; i < 3; i++) {
            if (bands[i] >= lv[k])
                reaching |= 1 << i;
        }
        hdr |= (int)k_index_of[reaching] << (4 * k);
        h[k] = lv[k];
    }
    hdr |= n << 12;
    *hdr_out = (uint16_t)hdr;
}

/* ---- the high band is not linear in amplitude -------------------------------
 *
 * Low and mid are linear in amplitude (exponents 1.03 and 1.05 over 24 dB);
 * high instead follows
 *
 *     log h = -0.3854 (log a)^2 + 1.2963 (log a) + 4.4627
 *
 * and reaches zero around -21 dBFS. Scaling the stored byte directly would be off
 * by about 50% of the band's height on a -6 dB move, so it is converted to
 * amplitude, scaled, and converted back.
 *
 * Both directions use a table instead of logf/expf: 128 entries cover the 7-bit
 * byte, and the reverse is a binary search over the same monotone table. This
 * keeps libm out of a loop that runs 73,000 times per fader move.
 */
#define HIGH_LEVELS 128
static float k_high_amp[HIGH_LEVELS];       /* stored height -> amplitude */

static void high_tables_init(void)
{
    const double c2 = -0.3854, c1 = 1.2963, c0 = 4.4627;
    int h;

    k_high_amp[0] = 0.0f;
    for (h = 1; h < HIGH_LEVELS; h++) {
        double disc = c1 * c1 - 4.0 * c2 * (c0 - log((double)h));

        if (disc < 0.0)
            disc = 0.0;
        k_high_amp[h] = (float)exp((-c1 + sqrt(disc)) / (2.0 * c2));
    }
    /* The curve is only monotone over the 24 dB it covers; force monotonicity so
     * the reverse search works. */
    for (h = 2; h < HIGH_LEVELS; h++) {
        if (k_high_amp[h] < k_high_amp[h - 1])
            k_high_amp[h] = k_high_amp[h - 1];
    }
}

static uint8_t high_from_amp(float a)
{
    int lo = 0, hi = HIGH_LEVELS - 1;

    if (a <= k_high_amp[0])
        return 0;
    if (a >= k_high_amp[HIGH_LEVELS - 1])
        return HIGH_LEVELS - 1;
    while (lo + 1 < hi) {
        int mid = (lo + hi) / 2;

        if (k_high_amp[mid] <= a)
            lo = mid;
        else
            hi = mid;
    }
    /* Round to the nearer level. */
    return (a - k_high_amp[lo] < k_high_amp[hi] - a) ? (uint8_t)lo : (uint8_t)hi;
}

void mod_wave_codec_init(void)
{
    high_tables_init();
}

void mod_wave_scale_ratios(const uint8_t *src, uint8_t *dst, size_t columns,
                           const float (*ratio)[MOD_WAVE_BANDS])
{
    size_t i;

    for (i = 0; i < columns; i++) {
        const uint8_t *s = src + i * MOD_WAVE_STRIDE;
        uint8_t *d = dst + i * MOD_WAVE_STRIDE;
        uint8_t h[3], bands[3];
        uint16_t hdr;
        int b;

        hdr = (uint16_t)(s[0] | ((uint16_t)s[1] << 8));
        h[0] = s[2];
        h[1] = s[3];
        h[2] = s[4];
        mod_wave_decode(hdr, h, bands);

        for (b = 0; b < 2; b++) {           /* low, mid: linear in amplitude */
            float v = (float)bands[b] * ratio[i][b] + 0.5f;

            bands[b] = v <= 0.0f ? 0 : (v > BAND_MAX ? BAND_MAX : (uint8_t)v);
        }
        if (bands[2])                        /* high: through the curve */
            bands[2] = high_from_amp(k_high_amp[bands[2]] * ratio[i][2]);

        mod_wave_encode(bands, &hdr, h);
        d[0] = (uint8_t)(hdr & 0xff);
        d[1] = (uint8_t)(hdr >> 8);
        d[2] = h[0];
        d[3] = h[1];
        d[4] = h[2];
        d[5] = s[5];
    }
}

/* ---- the other two styles ---------------------------------------------------
 *
 * The deck holds all three detailed waveforms for the loaded track, at the same
 * column count. Their in-memory layouts differ from the files (PWV3 packs height
 * low and whiteness high, PWV5 is big-endian):
 *
 *   Blue   1 byte      height [7:3] (0-31)   whiteness [2:0] (0-7)
 *   RGB    2 bytes LE  height [13:9](0-31)   low [2:0]  mid [5:3]  high [8:6]
 *                      [15:14] unused, constant zero
 *
 *   height   power, not amplitude (exponent ~2 against level; Blue [7:3] and
 *            RGB [13:9] carry identical values), so an amplitude ratio r is
 *            applied as r^2. 3-band stores amplitude.
 *
 *   r/g/b    a normalised colour that does not move with level. Scaling it by a
 *            band ratio would desaturate the hue, so it is left alone.
 *
 * The colour is also not the 3-band split: PWV5 uses its own crossovers.
 * Shifting the hue when a stem is muted needs those crossovers; until then the
 * height follows the fader and the hue stays the mix's.
 */
#define BLUE_H_SHIFT 3
#define BLUE_H_MAX   31
#define BLUE_W_MAX   7
#define RGB_H_SHIFT  9
#define RGB_H_MAX    31
#define RGB_C_MAX    7

static inline unsigned scale_field(unsigned v, float r, unsigned max)
{
    float x = (float)v * r + 0.5f;

    if (x <= 0.0f)
        return 0;
    return (unsigned)x > max ? max : (unsigned)x;
}

void mod_wave_scale_blue(const uint8_t *src, uint8_t *dst, size_t columns,
                         const float *ratio_broad)
{
    size_t i;

    for (i = 0; i < columns; i++) {
        unsigned v = src[i];
        float r = ratio_broad[i] * ratio_broad[i];   /* stored value is power */
        unsigned h = scale_field(v >> BLUE_H_SHIFT, r, BLUE_H_MAX);
        /* Whiteness tracks loudness, so it follows the same curve. */
        unsigned w = scale_field(v & BLUE_W_MAX, r, BLUE_W_MAX);

        dst[i] = (uint8_t)((h << BLUE_H_SHIFT) | w);
    }
}

void mod_wave_scale_rgb(const uint8_t *src, uint8_t *dst, size_t columns,
                        const float (*ratio)[MOD_WAVE_BANDS],
                        const float *ratio_broad)
{
    size_t i;

    (void)ratio;    /* colour is not scaled yet; see above */

    for (i = 0; i < columns; i++) {
        unsigned w = (unsigned)src[2*i] | ((unsigned)src[2*i + 1] << 8);
        float r = ratio_broad[i] * ratio_broad[i];   /* stored value is power */
        unsigned h = scale_field((w >> RGB_H_SHIFT) & RGB_H_MAX, r, RGB_H_MAX);
        /* Bits below 9 are the level-invariant colour, carried through. */
        unsigned out = (w & ~(unsigned)(RGB_H_MAX << RGB_H_SHIFT)) |
                       (h << RGB_H_SHIFT);

        dst[2*i] = (uint8_t)(out & 0xff);
        dst[2*i + 1] = (uint8_t)(out >> 8);
    }
}
