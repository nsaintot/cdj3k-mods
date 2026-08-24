// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * test_wave_codec.c - the CDJ-3000 3-band column codec, against the format
 * documented at the top of mods/wave_codec.c and the Python reference in
 * scripts/codec3band.py.
 */
#include "mods/wave/wave.h"

#include "test.h"

#define COLS 512

static void col_put(uint8_t *col, uint16_t hdr, const uint8_t *h, uint8_t spare)
{
    col[0] = (uint8_t)(hdr & 0xff);
    col[1] = (uint8_t)(hdr >> 8);
    col[2] = h[0];
    col[3] = h[1];
    col[4] = h[2];
    col[5] = spare;
}

static void col_encode(uint8_t *col, const uint8_t *bands, uint8_t spare)
{
    uint8_t h[3];
    uint16_t hdr;

    mod_wave_encode(bands, &hdr, h);
    col_put(col, hdr, h, spare);
}

static void col_decode(const uint8_t *col, uint8_t *bands)
{
    uint16_t hdr = (uint16_t)(col[0] | ((uint16_t)col[1] << 8));
    uint8_t h[3] = { col[2], col[3], col[4] };

    mod_wave_decode(hdr, h, bands);
}

/* A track's worth of columns from a fixed sequence: every test that wants real
 * input wants the same input twice. */
static void fill_track(uint8_t *dst, size_t columns)
{
    unsigned s = 0x13579bdfu;
    size_t i;

    for (i = 0; i < columns; i++) {
        uint8_t bands[3];
        int b;

        for (b = 0; b < 3; b++) {
            s = s * 1103515245u + 12345u;
            bands[b] = (uint8_t)((s >> 16) & 0x7f);
        }
        s = s * 1103515245u + 12345u;
        col_encode(dst + i * MOD_WAVE_STRIDE, bands, (uint8_t)(s >> 16));
    }
}

/* Every (low, mid, high) triple the 7-bit format can hold. */
static void roundtrip_exhaustive(void)
{
    int lo, mi, hi, bad = 0, unsorted = 0, miscount = 0;

    T_CASE("roundtrip exhaustive");
    for (lo = 0; lo < 128; lo++) {
        for (mi = 0; mi < 128; mi++) {
            for (hi = 0; hi < 128; hi++) {
                uint8_t in[3] = { (uint8_t)lo, (uint8_t)mi, (uint8_t)hi };
                uint8_t h[3], out[3];
                uint16_t hdr;
                int count, k, distinct = 0;

                mod_wave_encode(in, &hdr, h);
                mod_wave_decode(hdr, h, out);
                if (memcmp(in, out, 3) != 0 && bad++ == 0)
                    printf("  first mismatch: %d,%d,%d -> hdr %04x h %d,%d,%d -> %d,%d,%d\n",
                           lo, mi, hi, hdr, h[0], h[1], h[2], out[0], out[1], out[2]);

                /* The parser draws the rings outermost first and never re-sorts. */
                if (!(h[0] >= h[1] && h[1] >= h[2]))
                    unsorted++;

                /* The ring count is the number of distinct heights, less the
                 * zero one unless zero is all there is. */
                for (k = 0; k < 3; k++) {
                    int j, dup = 0;

                    for (j = 0; j < k; j++)
                        if (in[j] == in[k]) dup = 1;
                    if (!dup && (in[k] != 0 || (!lo && !mi && !hi)))
                        distinct++;
                }
                count = (hdr >> 12) & 0xf;
                if (count != distinct)
                    miscount++;
            }
        }
    }
    CHECK_INT(bad, 0);
    CHECK_INT(unsorted, 0);
    CHECK_INT(miscount, 0);
}

static void silence_is_one_ring(void)
{
    uint8_t bands[3] = { 0, 0, 0 }, h[3], out[3];
    uint16_t hdr;

    T_CASE("silence");
    mod_wave_encode(bands, &hdr, h);
    CHECK_U32(hdr, 0x1008u);           /* one ring, colour 8 = all three bands */
    CHECK_INT(h[0], 0);
    CHECK_INT(h[1], 0);
    CHECK_INT(h[2], 0);
    mod_wave_decode(hdr, h, out);
    CHECK_INT(out[0] | out[1] | out[2], 0);
}

/* Two columns worked out by hand from the k_set_of / k_index_of tables. */
static void known_encodings(void)
{
    uint8_t h[3], out[3];
    uint16_t hdr;

    T_CASE("known encodings");
    {   /* three distinct heights: rings high / mid+high / all */
        uint8_t bands[3] = { 10, 20, 30 };

        mod_wave_encode(bands, &hdr, h);
        CHECK_U32(hdr, 0x3841u);
        CHECK_INT(h[0], 30);
        CHECK_INT(h[1], 20);
        CHECK_INT(h[2], 10);
        mod_wave_decode(hdr, h, out);
        CHECK_INT(out[0], 10);
        CHECK_INT(out[1], 20);
        CHECK_INT(out[2], 30);
    }
    {   /* a tie collapses into one ring, and the zero band costs none */
        uint8_t bands[3] = { 50, 50, 0 };

        mod_wave_encode(bands, &hdr, h);
        CHECK_U32(hdr, 0x1006u);
        CHECK_INT(h[0], 50);
        mod_wave_decode(hdr, h, out);
        CHECK_INT(out[0], 50);
        CHECK_INT(out[1], 50);
        CHECK_INT(out[2], 0);
    }
    {   /* a header naming more rings than the format holds is clamped */
        uint8_t heights[3] = { 90, 60, 30 };

        mod_wave_decode(0xf841u, heights, out);
        CHECK_INT(out[0], 30);
        CHECK_INT(out[1], 60);
        CHECK_INT(out[2], 90);
    }
}

static void unity_is_identity(void)
{
    static uint8_t src[COLS * MOD_WAVE_STRIDE], dst[COLS * MOD_WAVE_STRIDE];
    static float ratio[COLS][MOD_WAVE_BANDS];
    size_t i;

    T_CASE("unity ratio is identity");
    fill_track(src, COLS);
    for (i = 0; i < COLS; i++)
        ratio[i][0] = ratio[i][1] = ratio[i][2] = 1.0f;

    memset(dst, 0xa5, sizeof dst);
    mod_wave_scale_ratios(src, dst, COLS, (const float (*)[MOD_WAVE_BANDS])ratio);
    CHECK(memcmp(src, dst, sizeof src) == 0);
}

static void scale_one(const uint8_t *bands, const float *ratio, uint8_t *out)
{
    uint8_t in[MOD_WAVE_STRIDE], scaled[MOD_WAVE_STRIDE];

    col_encode(in, bands, 0);
    mod_wave_scale_ratios(in, scaled, 1, (const float (*)[MOD_WAVE_BANDS])ratio);
    col_decode(scaled, out);
}

/* The high band goes through a fitted curve and back, so unity has to be a
 * fixed point at every one of the 128 stored levels, not just on average. */
static void unity_high_band_fixed_points(void)
{
    static const float unity[MOD_WAVE_BANDS] = { 1.0f, 1.0f, 1.0f };
    int level, moved = 0;

    T_CASE("high band fixed points");
    for (level = 0; level < 128; level++) {
        uint8_t bands[3] = { 0, 0, (uint8_t)level }, got[3];

        scale_one(bands, unity, got);
        if (got[2] != level)
            moved++;
    }
    CHECK_INT(moved, 0);
}

/* Low and mid track amplitude and scale linearly. High does not: it is stored
 * on a fitted curve, so the same ratio applied to the stored byte lands well
 * clear of the curve's answer. Both directions, at every level. */
static void high_band_is_not_linear(void)
{
    static const float half[MOD_WAVE_BANDS] = { 0.5f, 0.5f, 0.5f };
    static const float twice[MOD_WAVE_BANDS] = { 2.0f, 2.0f, 2.0f };
    static const float floor_ratio[MOD_WAVE_BANDS] = { 0.02f, 0.02f, 0.02f };
    int level, i, prev = -1, non_monotone = 0, silent = 0;

    T_CASE("high band is not linear");
    for (level = 0; level < 128; level++) {
        uint8_t bands[3] = { (uint8_t)level, (uint8_t)level, (uint8_t)level }, got[3];
        int linear = (int)((float)level * 0.5f + 0.5f);

        scale_one(bands, half, got);
        CHECK_INT(got[0], linear);
        CHECK_INT(got[1], linear);
        if (level >= 10)
            CHECK(got[2] < linear);
        if (got[2] < prev)
            non_monotone++;
        prev = got[2];

        scale_one(bands, floor_ratio, got);
        if (got[2] != 0)
            silent++;
    }
    CHECK_INT(non_monotone, 0);
    CHECK_INT(silent, 0);

    for (level = 10; level <= 50; level += 10) {
        uint8_t bands[3] = { (uint8_t)level, 0, (uint8_t)level }, got[3];

        scale_one(bands, twice, got);
        CHECK_INT(got[0], level * 2);
        CHECK(got[2] > level * 2);
    }

    /* A fader that moves one way must not move the band the other. */
    prev = -1;
    non_monotone = 0;
    for (i = 1; i <= 40; i++) {
        float ratio[MOD_WAVE_BANDS] = { 1.0f, 1.0f, 0.0f };
        uint8_t bands[3] = { 0, 0, 100 }, got[3];

        ratio[2] = (float)i / 40.0f;
        scale_one(bands, ratio, got);
        if (got[2] < prev)
            non_monotone++;
        prev = got[2];
    }
    CHECK_INT(non_monotone, 0);
}

static void scale_saturates(void)
{
    uint8_t bands[3] = { 100, 100, 100 }, got[3];
    uint8_t in[MOD_WAVE_STRIDE], out[MOD_WAVE_STRIDE];
    float ratio[1][MOD_WAVE_BANDS] = { { 16.0f, 16.0f, 16.0f } };

    T_CASE("scale saturates");
    col_encode(in, bands, 0x5a);
    mod_wave_scale_ratios(in, out, 1, (const float (*)[MOD_WAVE_BANDS])ratio);
    col_decode(out, got);
    CHECK_INT(got[0], 127);
    CHECK_INT(got[1], 127);
    CHECK_INT(got[2], 127);
    CHECK_INT(out[5], 0x5a);           /* the spare byte is the parser's */
}

static void scale_in_place(void)
{
    static uint8_t src[COLS * MOD_WAVE_STRIDE], apart[COLS * MOD_WAVE_STRIDE];
    static float ratio[COLS][MOD_WAVE_BANDS];
    size_t i;

    T_CASE("scale in place");
    fill_track(src, COLS);
    for (i = 0; i < COLS; i++) {
        ratio[i][0] = 0.5f;
        ratio[i][1] = 1.25f;
        ratio[i][2] = 1.0f;
    }
    mod_wave_scale_ratios(src, apart, COLS, (const float (*)[MOD_WAVE_BANDS])ratio);
    mod_wave_scale_ratios(src, src, COLS, (const float (*)[MOD_WAVE_BANDS])ratio);
    CHECK(memcmp(src, apart, sizeof src) == 0);
}

static void blue_and_rgb(void)
{
    static uint8_t bsrc[256], bdst[256];
    static uint8_t rsrc[512], rdst[512];
    static float broad[256], ratio[256][MOD_WAVE_BANDS];
    int i, colour_moved = 0, height_moved = 0;

    T_CASE("blue and rgb styles");
    for (i = 0; i < 256; i++) {
        bsrc[i] = (uint8_t)i;
        rsrc[2 * i] = (uint8_t)(i * 7);
        rsrc[2 * i + 1] = (uint8_t)((i * 13) & 0x3f);
        broad[i] = 1.0f;
        ratio[i][0] = ratio[i][1] = ratio[i][2] = 1.0f;
    }

    mod_wave_scale_blue(bsrc, bdst, 256, broad);
    CHECK(memcmp(bsrc, bdst, sizeof bsrc) == 0);
    mod_wave_scale_rgb(rsrc, rdst, 256, (const float (*)[MOD_WAVE_BANDS])ratio, broad);
    CHECK(memcmp(rsrc, rdst, sizeof rsrc) == 0);

    /* Height follows the fader, the colour does not: measured level invariant,
     * see wave_codec.c. */
    for (i = 0; i < 256; i++)
        broad[i] = 0.5f;
    mod_wave_scale_rgb(rsrc, rdst, 256, (const float (*)[MOD_WAVE_BANDS])ratio, broad);
    for (i = 0; i < 256; i++) {
        unsigned a = (unsigned)rsrc[2 * i] | ((unsigned)rsrc[2 * i + 1] << 8);
        unsigned b = (unsigned)rdst[2 * i] | ((unsigned)rdst[2 * i + 1] << 8);

        if ((a & 0x1ff) != (b & 0x1ff) || (a & 0xc000) != (b & 0xc000))
            colour_moved++;
        if ((a >> 9) != (b >> 9))
            height_moved++;
    }
    CHECK_INT(colour_moved, 0);
    CHECK(height_moved > 200);

    /* Blue packs height and whiteness in one byte; halving must not let one
     * field carry into the other. */
    mod_wave_scale_blue(bsrc, bdst, 256, broad);
    for (i = 0; i < 256; i++) {
        CHECK(((unsigned)bdst[i] >> 3) <= ((unsigned)bsrc[i] >> 3));
        CHECK((bdst[i] & 7) <= (bsrc[i] & 7));
    }
}

int main(void)
{
    mod_wave_codec_init();

    roundtrip_exhaustive();
    silence_is_one_ring();
    known_encodings();
    unity_is_identity();
    unity_high_band_fixed_points();
    high_band_is_not_linear();
    scale_saturates();
    scale_in_place();
    blue_and_rgb();

    return t_done("wave_codec");
}
