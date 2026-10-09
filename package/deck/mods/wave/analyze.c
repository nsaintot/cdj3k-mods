// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * analyze.c - one pass over the track, into band powers per column.
 *
 * Part of the waveform-follows-the-faders feature. Shared declarations and the
 * design notes are in wave.h.
 */
#include "wave/wave.h"

/* ---- the band split -------------------------------------------------------
 *
 * The deck's crossovers are ~300 Hz and ~2500 Hz, with a wide handover (at 280 Hz
 * low and mid respond almost equally), as from a shallow second-order crossover.
 */
#define XOVER_LO 300.0f
#define XOVER_HI 2500.0f

struct biquad { float b0, b1, b2, a1, a2; };
struct bqz { float z1, z2; };

static void design(struct biquad *q, float f0, int rate, int highpass)
{
    const float w = 2.0f * (float)M_PI * f0 / (float)rate;
    const float cw = cosf(w), sw = sinf(w);
    const float alpha = sw / 1.41421356f;          /* Q = 1/sqrt(2) */
    const float a0 = 1.0f + alpha;

    if (highpass) {
        q->b0 = (1.0f + cw) / 2.0f / a0;
        q->b1 = -(1.0f + cw) / a0;
        q->b2 = q->b0;
    } else {
        q->b0 = (1.0f - cw) / 2.0f / a0;
        q->b1 = (1.0f - cw) / a0;
        q->b2 = q->b0;
    }
    q->a1 = (-2.0f * cw) / a0;
    q->a2 = (1.0f - alpha) / a0;
}

static inline float step(const struct biquad *q, struct bqz *z, float x)
{
    float y = q->b0 * x + z->z1;

    z->z1 = q->b1 * x - q->a1 * y + z->z2;
    z->z2 = q->b2 * x - q->a2 * y;
    return y;
}

/* One filter chain per stem: low is a lowpass, high a highpass, and mid is the
 * band between them. */
struct chain {
    struct bqz lo, hp, mid_lp, hi;
};

struct analysis {
    struct biquad lp_lo, hp_lo, lp_hi, hp_hi;
    struct chain  ch[N_STEMS];
    double        acc[N_STEMS][MOD_WAVE_BANDS];
    int           spc;              /* samples per column */
    int           in_col;           /* samples accumulated into the current one */
    uint32_t      col;
    uint32_t      ncols;
    float        *power;            /* [stem][band][col] */
    struct stem_view view;
    int64_t       pos;              /* frames consumed, for the stem lookup */
    int           aborted;
};

static void feed(struct analysis *a, int s, float x)
{
    struct chain *c = &a->ch[s];
    float lo = step(&a->lp_lo, &c->lo, x);
    float hi = step(&a->hp_hi, &c->hi, x);
    float mid = step(&a->lp_hi, &c->mid_lp, step(&a->hp_lo, &c->hp, x));

    a->acc[s][0] += (double)lo * lo;
    a->acc[s][1] += (double)mid * mid;
    a->acc[s][2] += (double)hi * hi;
}

static int mix_sink(const float *interleaved, int64_t frames, void *user)
{
    struct analysis *a = user;
    int64_t i;

    if (wave_g_track_gone || !g_stem_ready) {
        a->aborted = 1;
        return 1;
    }
    for (i = 0; i < frames; i++) {
        int64_t f = a->pos + i;
        float m = 0.5f * (interleaved[2*i] + interleaved[2*i + 1]);
        float h = 0.0f, v = 0.0f, s[N_STEMS];
        int k, b;

        if (f < a->view.frames) {
            h = 0.5f * ((float)a->view.harmonics[2*f] +
                        (float)a->view.harmonics[2*f + 1]) / 32768.0f
                * a->view.h_scale;
            v = 0.5f * ((float)a->view.vocals[2*f] +
                        (float)a->view.vocals[2*f + 1]) / 32768.0f
                * a->view.v_scale;
        }
        s[STEM_PART_DRUMS] = m - h - v;   /* the residual the deck plays */
        s[STEM_PART_HARMONICS] = h;
        s[STEM_PART_VOCALS] = v;

        for (k = 0; k < N_STEMS; k++)
            feed(a, k, s[k]);

        if (++a->in_col >= a->spc) {
            if (a->col < a->ncols) {
                for (k = 0; k < N_STEMS; k++) {
                    for (b = 0; b < MOD_WAVE_BANDS; b++) {
                        size_t idx = ((size_t)k * MOD_WAVE_BANDS + b) *
                                     a->ncols + a->col;

                        a->power[idx] = (float)(a->acc[k][b] / a->spc);
                    }
                }
            }
            memset(a->acc, 0, sizeof(a->acc));
            a->in_col = 0;
            a->col++;
        }
    }
    a->pos += frames;
    return 0;
}

/* ---- the analysis, cached beside the stems ----------------------------------
 *
 * The band powers depend only on the track and the separation that produced its
 * stems, so they are cached (saving about a second of decode per load) in the
 * stem cache entry's directory. A different model gives a different sep-id and
 * so a different directory; no extra key is needed.
 *
 * The entry is found as the stem job finds it, by the decoder's frame count,
 * which stem_decode_pull reports without decoding when given no sink. About 3 MB
 * for an eight-minute track, next to ~60 MB of stem FLACs.
 */
#define BAND_CACHE_MAGIC 0x31425357u          /* "WSB1" */
#define BAND_CACHE_NAME  "bands.bin"

struct band_cache_head {
    uint32_t magic, ncols, nstems, nbands;
};

static int band_cache_path(const char *track, char *out, size_t cap)
{
    struct stem_cache_entry e;
    int64_t frames;
    char *slash;

    if (!track || !track[0])
        return -1;
    frames = stem_decode_pull(track, STEM_UPLOAD_RATE, NULL, NULL);
    if (frames <= 0 || stem_cache_lookup(track, frames, &e) != 0)
        return -1;
    snprintf(out, cap, "%s", e.harmonics_path);
    slash = strrchr(out, '/');
    if (!slash)
        return -1;
    snprintf(slash + 1, cap - (size_t)(slash + 1 - out), "%s", BAND_CACHE_NAME);
    return 0;
}

static int band_cache_load(const char *track, uint32_t ncols, float **out)
{
    char path[STEM_CACHE_PATH_MAX];
    struct band_cache_head h;
    size_t n = (size_t)N_STEMS * MOD_WAVE_BANDS * ncols;
    float *buf;
    FILE *f;

    if (band_cache_path(track, path, sizeof(path)) != 0)
        return -1;
    f = fopen(path, "rb");
    if (!f)
        return -1;
    if (fread(&h, sizeof(h), 1, f) != 1 || h.magic != BAND_CACHE_MAGIC ||
        h.ncols != ncols || h.nstems != N_STEMS || h.nbands != MOD_WAVE_BANDS) {
        fclose(f);
        return -1;
    }
    buf = malloc(n * sizeof(float));
    if (!buf) {
        fclose(f);
        return -1;
    }
    if (fread(buf, sizeof(float), n, f) != n) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);
    *out = buf;
    MDBG("wave_stems: band cache hit, %u columns from %s\n", ncols, path);
    return 0;
}

/* Best effort: a failure only costs a re-analysis next time. */
static void band_cache_store(const char *track, uint32_t ncols, const float *p)
{
    char path[STEM_CACHE_PATH_MAX], tmp[STEM_CACHE_PATH_MAX];
    struct band_cache_head h = { BAND_CACHE_MAGIC, ncols, N_STEMS,
                                 MOD_WAVE_BANDS };
    size_t n = (size_t)N_STEMS * MOD_WAVE_BANDS * ncols;
    FILE *f;

    if (band_cache_path(track, path, sizeof(path)) != 0)
        return;
    /* A truncated temp path names a different file, which the rename would move
     * over another track's cache entry. Give up instead. */
    if (snprintf(tmp, sizeof(tmp), "%s.new", path) >= (int)sizeof(tmp))
        return;
    f = fopen(tmp, "wb");
    if (!f)
        return;
    if (fwrite(&h, sizeof(h), 1, f) == 1 && fwrite(p, sizeof(float), n, f) == n) {
        fclose(f);
        /* Rename so a stick pulled mid-write leaves no half file to be trusted. */
        if (rename(tmp, path) == 0)
            MDBG("wave_stems: band cache stored, %u columns -> %s\n", ncols, path);
        else
            unlink(tmp);
    } else {
        fclose(f);
        unlink(tmp);
    }
}

/* Runs for about a second on our own thread. Return codes: see wave.h. */
int wave_run_analysis(const char *path)
{
    struct analysis *a;
    int rate = stem_pool_rate();
    int64_t got;

    /* The 3-band copy gives the column count: its codec is exact, and all three
     * styles have the same count. */
    if (!wave_g_st[WS_STYLE_3BAND].pristine)
        return 0;                        /* the worker gates on this too */
    if (!path || !path[0]) {
        MDBG("wave_stems: analysis has no track path\n");
        return -1;
    }
    /* The pool rate is measured from playback and stays 0 until something has
     * played (e.g. a track loaded and left paused): retry later. */
    if (rate <= 0)
        return 0;
    if (rate % COLUMNS_PER_SEC) {
        MDBG("wave_stems: pool rate %d is not a multiple of %d columns/s\n",
             rate, COLUMNS_PER_SEC);
        return -1;
    }

    /* Which track the copies are of is checked by wave_stale every tick. */
    wave_g_ncols = wave_g_st[WS_STYLE_3BAND].ncols;
    free(wave_g_ratio);
    free(wave_g_ratio_broad);
    wave_g_ratio = malloc((size_t)wave_g_ncols * sizeof(*wave_g_ratio));
    wave_g_ratio_broad = malloc((size_t)wave_g_ncols * sizeof(*wave_g_ratio_broad));
    if (!wave_g_ratio || !wave_g_ratio_broad) {
        MDBG("wave_stems: out of memory for %u ratios\n", wave_g_ncols);
        return -1;
    }

    {
        float *cached = NULL;

        if (band_cache_load(path, wave_g_ncols, &cached) == 0) {
            free(wave_g_power);
            wave_g_power = cached;
            wave_g_have_analysis = 1;
            return 1;
        }
    }

    a = calloc(1, sizeof(*a));
    if (!a)
        return -1;
    if (!stem_store_acquire(&a->view)) {
        /* A set being swapped (two quick track changes, the second's stems not
         * yet published), not gone. Retry. */
        MDBG("wave_stems: stems not acquirable yet, retrying the analysis\n");
        free(a);
        return 0;
    }

    a->spc = rate / COLUMNS_PER_SEC;
    a->ncols = wave_g_ncols;
    a->power = calloc((size_t)N_STEMS * MOD_WAVE_BANDS * wave_g_ncols, sizeof(float));
    if (!a->power) {
        stem_store_release();
        free(a);
        return -1;
    }
    design(&a->lp_lo, XOVER_LO, rate, 0);
    design(&a->hp_lo, XOVER_LO, rate, 1);
    design(&a->lp_hi, XOVER_HI, rate, 0);
    design(&a->hp_hi, XOVER_HI, rate, 1);

    got = stem_decode_pull(path, rate, mix_sink, a);
    stem_store_release();

    if (got < 0 || a->aborted) {
        /* An abort means the track or stems changed: retry (cheap, mix_sink
         * refuses on its first chunk). A failed decode would fail again. */
        int again = a->aborted;

        MDBG("wave_stems: analysis abandoned (frames=%lld aborted=%d)\n",
             (long long)got, a->aborted);
        free(a->power);
        free(a);
        return again ? 0 : -1;
    }

    free(wave_g_power);
    wave_g_power = a->power;
    wave_g_have_analysis = 1;
    MDBG("wave_stems: analyzed %u columns from %lld frames at %d Hz\n",
         a->col < wave_g_ncols ? a->col : wave_g_ncols, (long long)got, rate);
    band_cache_store(path, wave_g_ncols, wave_g_power);
    free(a);
    return 1;
}
