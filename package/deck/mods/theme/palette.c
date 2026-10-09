// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * palette.c - the colour model: the per-colour transform and its memo.
 *
 * Every theme is this code with different numbers. The stages are documented on
 * struct theme_palette in theme.h; this file is the arithmetic.
 *
 * It runs on every UI fill (thousands per frame) and on every sprite pixel when a
 * theme is switched, so it uses integer maths only, with no division by a variable
 * in the common path. There is no HSL round trip: each stage works directly on RGB
 * in a form that preserves what it claims to.
 */
#include "theme/theme.h"

/* Lightness, the HSL definition, doubled to keep it integral: max + min.
 * Used as the duotone ramp index and as the pivot for the chroma scale. */
static inline uint32_t luma2(uint32_t r, uint32_t g, uint32_t b)
{
    uint32_t max = r > g ? r : g; if (b > max) max = b;
    uint32_t min = r < g ? r : g; if (b < min) min = b;
    return max + min;
}


/* Hue as a 0..255 angle, the standard integer form. `c` is the caller's chroma and
 * must be non-zero. */
static inline uint32_t hue256(uint32_t r, uint32_t g, uint32_t b,
                              uint32_t max, uint32_t c)
{
    int h;

    if (max == r)      h =       43 * ((int)g - (int)b) / (int)c;
    else if (max == g) h =  85 + 43 * ((int)b - (int)r) / (int)c;
    else               h = 171 + 43 * ((int)r - (int)g) / (int)c;
    return (uint32_t)(h & 0xff);
}

/* Below this chroma a pixel's hue is noise and rotating it would tint the deck's
 * neutral chrome, so such pixels go to the duotone instead. */
#define HUE_MIN_CHROMA 24u

/* The arithmetic. `p` is never NULL here (theme_palette_rgb and theme_palette_map
 * handle that). Must stay a pure function of (p, r, g, b, is_fill) for the memo in
 * front of it to be transparent. */
static void palette_compute(const struct theme_palette *p,
                            uint32_t *pr, uint32_t *pg, uint32_t *pb, int is_fill)
{
    uint32_t r = *pr, g = *pg, b = *pb;
    uint32_t max, min, c;

    /* The selection blue stated by the theme (see theme_palette::selection). Fill path
     * only: through drawImage the same blue is picture content such as the waveform. */
    if (is_fill && p->selection) {
        uint32_t src = (r << 16) | (g << 8) | b;

        if (src == THEME_DECK_SELECT || src == THEME_DECK_SELECT2) {
            uint32_t q = src == THEME_DECK_SELECT ? 256u : THEME_SELECT2_Q8;

            *pr = (((p->selection >> 16) & 0xffu) * q) >> 8;
            *pg = (((p->selection >>  8) & 0xffu) * q) >> 8;
            *pb = ((  p->selection        & 0xffu) * q) >> 8;
            return;
        }
    }

    max = r > g ? r : g; if (b > max) max = b;
    min = r < g ? r : g; if (b < min) min = b;
    c = max - min;                    /* chroma of the input colour */

    /* ---- 1. lightness inversion, hue and chroma exact --------------------
     *
     * L' = 255 - L leaves |2L-255| unchanged, so the chroma span survives and
     * every channel shifts by the same amount:
     *
     *   c' = c - min + (255 - max) = c + (255 - max - min)
     *
     * c is in [min,max], so c' lands in [255-max, 255-min]: always in range,
     * no clamping. Examples:
     *
     *   #191919 list bg   -> #e6e6e6   (dark grey  -> light grey)
     *   #ffffff text      -> #000000   (white      -> black)
     *   #007de1 accent    -> #1e9bff   (still blue, lifted for a light bg)
     *   #ff0000 warning   -> #ff0000   (fully saturated: unchanged)
     */
    if (p->invert_l) {
        const uint32_t k = 255u - max - min;
        r += k; g += k; b += k;
    }

    /* ---- 2. pull saturated colours darker -------------------------------
     * f = 1 - sat_darken*(C/255). All three channels scale together, so hue is
     * exact and greys (C == 0) are untouched. Not an involution, so image.c keeps
     * a pristine copy instead of transforming pixels back. */
    if (p->sat_darken_q8 > 0 && c > 0) {
        int exempt = is_fill && p->exempt_blue && b > r && b > g;

        if (!exempt) {
            uint32_t f = 255u * 256u - (uint32_t)p->sat_darken_q8 * c;
            r = r * f / (255u * 256u);
            g = g * f / (255u * 256u);
            b = b * f / (255u * 256u);
        }
    }

    /* ---- 3. chroma scale about the pixel's own lightness -----------------
     * Push each channel away from (or toward) the local mid-point. Signed
     * arithmetic and an explicit clamp: unlike stage 1 this can leave the
     * representable range, and a wrap would show as confetti. */
    /* 0 means unchanged, so a palette that omits sat_q8 is not greyscaled
     * ("a field left zero does nothing", theme.h). Greyscale is sat_q8 = 1. */
    if (p->sat_q8 != 0 && p->sat_q8 != 256) {
        int32_t mid = (int32_t)(luma2(r, g, b) / 2u);
        int32_t s = p->sat_q8;
        int32_t ch[3] = { (int32_t)r, (int32_t)g, (int32_t)b };
        int i;

        for (i = 0; i < 3; i++) {
            int32_t v = mid + ((ch[i] - mid) * s >> 8);

            ch[i] = v < 0 ? 0 : (v > 255 ? 255 : v);
        }
        r = (uint32_t)ch[0]; g = (uint32_t)ch[1]; b = (uint32_t)ch[2];
    }

    /* ---- 4a. hue map: the palette proper ----------------------------------
     *
     * Rotate a chromatic pixel to the nearest hue this theme owns, keeping its own
     * lightness and chroma, so the deck's hue distinctions survive with the theme's
     * colours. The duotone below has two anchors and produces only one hue.
     *
     * No inverse HSL: take each target channel's offset from the target's mid-point
     * and scale that spread to the source's chroma about the source's mid-point. Hue
     * comes from the target, weight from the source. */
    if (p->nhue > 0) {
        /* Recomputed: `max` and `c` at the top describe the input colour, and stages
         * 1-3 have changed it since. Hue and chroma must come from the same colour. */
        uint32_t nmax = r > g ? r : g; if (b > nmax) nmax = b;
        uint32_t nmin = r < g ? r : g; if (b < nmin) nmin = b;
        uint32_t nc = nmax - nmin;
        uint32_t hs, best = 0, bestd = 0xffffffffu;
        uint32_t i;

        if (nc < HUE_MIN_CHROMA) goto duotone;
        c  = nc;
        hs = hue256(r, g, b, nmax, nc);
        for (i = 0; i < p->nhue && i < 4; i++) {
            uint32_t tr = (p->hue[i] >> 16) & 0xffu;
            uint32_t tg = (p->hue[i] >> 8) & 0xffu;
            uint32_t tb = p->hue[i] & 0xffu;
            uint32_t tmax = tr > tg ? tr : tg; if (tb > tmax) tmax = tb;
            uint32_t tmin = tr < tg ? tr : tg; if (tb < tmin) tmin = tb;
            uint32_t tc = tmax - tmin, ht, d;

            if (tc == 0) continue;                /* a grey in the hue list is skipped */
            ht = hue256(tr, tg, tb, tmax, tc);
            d  = hs > ht ? hs - ht : ht - hs;
            if (d > 128u) d = 256u - d;           /* the angle wraps */
            if (d < bestd) { bestd = d; best = i; }
        }
        if (bestd != 0xffffffffu) {
            uint32_t tr = (p->hue[best] >> 16) & 0xffu;
            uint32_t tg = (p->hue[best] >> 8) & 0xffu;
            uint32_t tb = p->hue[best] & 0xffu;
            uint32_t tmax = tr > tg ? tr : tg; if (tb > tmax) tmax = tb;
            uint32_t tmin = tr < tg ? tr : tg; if (tb < tmin) tmin = tb;
            uint32_t tc = tmax - tmin;
            int32_t  tmid = (int32_t)(tmax + tmin) / 2;
            int32_t  smid = (int32_t)luma2(r, g, b) / 2;
            int32_t  ch[3] = { (int32_t)tr, (int32_t)tg, (int32_t)tb };
            int32_t  k = p->hue_pull_q8;
            int32_t  mid, spread;
            int j;

            /* How much of the target comes along besides its hue (see hue_pull_q8).
             * At k = 0 these reduce to `smid` and `c`, the hue-only mapping. */
            if (k < 0) k = 0; else if (k > 256) k = 256;
            mid    = smid + (tmid - smid) * k / 256;
            spread = (int32_t)c + ((int32_t)tc - (int32_t)c) * k / 256;

            for (j = 0; j < 3; j++) {
                int32_t v = mid + (ch[j] - tmid) * spread / (int32_t)tc;

                ch[j] = v < 0 ? 0 : (v > 255 ? 255 : v);
            }
            *pr = (uint32_t)ch[0];
            *pg = (uint32_t)ch[1];
            *pb = (uint32_t)ch[2];
            return;                               /* mapped: the duotone is for greys */
        }
    }

duotone:
    /* ---- 4b. duotone -------------------------------------------------------
     * Blend toward a ramp between two anchors, indexed by lightness: `shadow` is
     * what black becomes, `highlight` what white becomes. tint_q8 is how far.
     *
     * Last because it replaces hues; the stages above only re-polarise or
     * re-saturate the deck's own. */
    if (p->tint_q8 > 0) {
        uint32_t l = luma2(r, g, b) / 2u;         /* 0..255 */
        uint32_t t = (uint32_t)p->tint_q8;
        uint32_t sr = (p->shadow >> 16) & 0xffu, hr = (p->highlight >> 16) & 0xffu;
        uint32_t sg = (p->shadow >> 8) & 0xffu,  hg = (p->highlight >> 8) & 0xffu;
        uint32_t sb = p->shadow & 0xffu,         hb = p->highlight & 0xffu;
        uint32_t tr = sr + (hr - sr) * l / 255u;  /* the ramp at this lightness */
        uint32_t tg = sg + (hg - sg) * l / 255u;
        uint32_t tb = sb + (hb - sb) * l / 255u;

        /* Anchors must be authored dark-to-light so the subtractions above stay
         * non-negative; a reversed pair would wrap. Not checked on this hot path;
         * presets.c keeps to it. */
        r = (r * (256u - t) + tr * t) >> 8;
        g = (g * (256u - t) + tg * t) >> 8;
        b = (b * (256u - t) + tb * t) >> 8;
    }

    *pr = r > 255u ? 255u : r;
    *pg = g > 255u ? 255u : g;
    *pb = b > 255u ? 255u : b;
}

/* ================================================================== */
/* The memo                                                           */
/* ================================================================== */

/*
 * palette_compute is pure and sees the same inputs repeatedly: setFill runs
 * thousands of times a frame over a handful of chrome colours, and the waveform
 * hands drawImage a 1200x128 strip every frame drawn from a small ink palette.
 * Each answer costs a dozen integer divisions, which the A72 does not pipeline.
 *
 * Without the memo a themed deck drops under 30 fps while ORIGINAL (which returns
 * at the top of theme_palette_map) stays fluid.
 *
 * The hit path is inline in theme.h; this file holds only the miss.
 *
 * Sizing: the stock overview strip holds 46 distinct colours and four of them
 * (#000000 ground, #0055e1 ink, #ffffff peaks, #ffa600) cover 99% of its pixels;
 * the browse previews hold 518. The per-pixel hit rate is 98.1% at 256 slots,
 * 99.3% at 1024 and 99.5% at 2048.
 *
 * 1024 slots is 8 KB, a quarter of the A72's 32 KB L1, leaving the rest for the
 * 600 KB strip being walked; a larger table that evicts those pixels costs more
 * than the 0.2% of hits it gains.
 *
 * The memo is a 2x speedup on WHITE and ~7x on a
 * palette with a hue map at a few hundred distinct colours, and a net loss at 4096
 * (every miss pays the hash and store on top of the arithmetic). The report prints
 * the hit rate so a falling rate shows the table is seeing input it was not sized
 * for.
 */

/* One naturally-aligned 64-bit word per slot, which aarch64 loads and stores
 * single-copy atomically, so the paint thread and the waveform bake need no lock.
 * Each entry carries the question beside the answer, so a collision reads as a
 * miss. Layout in theme.h. */
uint64_t g_theme_memo[THEME_MEMO_N];

/* The palette the table's answers belong to. Callers pass mod_theme()->palette and
 * the presets are const statics, so a pointer change means a theme switch, the only
 * event that invalidates answers, and the 8 KB table is cleared. If callers ever
 * alternated palettes the cost would be a memset per call, not a wrong colour. */
const struct theme_palette *g_theme_memo_pal;

/* Indexed by is_fill. 1 is setFill: one call per rect, path or glyph. 0 is image
 * pixels: one call per pixel of every waveform frame, three orders of magnitude
 * more. Kept separate so the larger count does not hide changes in the smaller. */
unsigned g_theme_memo_hit[2], g_theme_memo_miss[2];

/* The transform on a separated triplet. A NULL palette is ORIGINAL and leaves the
 * triplet untouched, matching theme_palette_map. */
void theme_palette_rgb(const struct theme_palette *p,
                       uint32_t *pr, uint32_t *pg, uint32_t *pb, int is_fill)
{
    if (p == NULL)
        return;
    palette_compute(p, pr, pg, pb, is_fill);
}

/* The miss path, kept out of line so the inlined hit path stays small. */
uint32_t theme_palette_slow(const struct theme_palette *p, uint32_t rgb, int is_fill)
{
    uint32_t r = (rgb >> 16) & 0xffu, g = (rgb >> 8) & 0xffu, b = rgb & 0xffu;
    uint32_t q = (rgb << 1) | (is_fill ? 1u : 0u);
    uint64_t ent;

    if (p != g_theme_memo_pal) {
        memset(g_theme_memo, 0, sizeof(g_theme_memo));
        g_theme_memo_pal = p;
    }

    palette_compute(p, &r, &g, &b, is_fill);
    rgb = (r << 16) | (g << 8) | b;
    g_theme_memo_miss[is_fill ? 1 : 0]++;

    ent = THEME_MEMO_OCCUPIED | ((uint64_t)q << THEME_MEMO_Q_SHIFT) | (uint64_t)rgb;
    __atomic_store_n(&g_theme_memo[THEME_MEMO_SLOT(q)], ent, __ATOMIC_RELAXED);
    return rgb;
}

/* [message] Print at most every 3 s, from the setFill tick. Cheap enough to call
 * unconditionally: MDBG tests the level before it evaluates anything. */
void theme_memo_report(void)
{
    static unsigned last_t, last_h[2], last_m[2];
    unsigned h[2] = { g_theme_memo_hit[0], g_theme_memo_hit[1] };
    unsigned m[2] = { g_theme_memo_miss[0], g_theme_memo_miss[1] };
    unsigned now = (unsigned)time(NULL), dt, dh[2], dm[2], i;

    if (last_t == 0) goto rearm;
    dt = now - last_t;
    if (dt < 3u) return;

    for (i = 0; i < 2; i++) {
        dh[i] = h[i] - last_h[i];
        dm[i] = m[i] - last_m[i];
    }
    MDBG("theme: memo  pixels %u/s (%u%% hit)  fills %u/s (%u%% hit)  over %us\n",
         (dh[0] + dm[0]) / dt,
         (dh[0] + dm[0]) ? (unsigned)(100ull * dh[0] / (dh[0] + dm[0])) : 0u,
         (dh[1] + dm[1]) / dt,
         (dh[1] + dm[1]) ? (unsigned)(100ull * dh[1] / (dh[1] + dm[1])) : 0u, dt);

rearm:
    last_t = now;
    for (i = 0; i < 2; i++) { last_h[i] = h[i]; last_m[i] = m[i]; }
}

uint32_t theme_palette_argb(const struct theme_palette *p, uint32_t argb,
                            int is_fill)
{
    return (argb & 0xff000000u) |                 /* alpha untouched */
           theme_palette_map(p, argb & 0xffffffu, is_fill);
}
