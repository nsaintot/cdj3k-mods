// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * test_stem_loop.c - where a looped file reads, against mods/stem/loop.h.
 *
 * The mix indexes the loop buffer with what this returns and interpolates
 * between that frame and its successor, so a result that reaches the span is an
 * out-of-bounds read on the audio thread. The cases cover the boundaries: the
 * wrap, a position before the loop was engaged, a ratio other than 1, and the
 * rounding at the top of the range.
 */
#include "mods/stem/loop.h"

#include "test.h"

/* Walk `frames` frames from `start` and check every one lands inside the loop
 * and advances by `ratio` (mod span), exactly as the mix reads them. */
static void walk(const struct stem_loop *l, int64_t start, double ratio,
                 int64_t frames)
{
    double prev = -1.0;
    int64_t k;

    for (k = 0; k < frames; k++) {
        double u = stem_loop_phase(l, start + k, ratio);

        CHECK(u >= 0.0 && u < (double)l->span);
        if (prev >= 0.0) {
            double want = prev + ratio;

            if (want >= (double)l->span)
                want -= (double)l->span;        /* wrapped */
            CHECK_NEAR(u, want, 1e-6);
        }
        prev = u;
    }
}

int main(void)
{
    T_CASE("not looping");
    {
        struct stem_loop l = { 0, 0 };

        /* span 0 is a slot with no file; 0 is the only in-bounds answer. */
        CHECK_NEAR(stem_loop_phase(&l, 0, 1.0), 0.0, 0.0);
        CHECK_NEAR(stem_loop_phase(&l, 12345, 1.0), 0.0, 0.0);
        l.from = 999;                       /* ignored while span is 0 */
        CHECK_NEAR(stem_loop_phase(&l, 12345, 1.0), 0.0, 0.0);
    }

    T_CASE("at its own tempo");
    {
        struct stem_loop l = { 1000, 400 };

        /* At ratio 1 the file advances one frame per track frame, so every
         * phase is exact. */
        CHECK_NEAR(stem_loop_phase(&l, 1000, 1.0), 0.0, 0.0);
        CHECK_NEAR(stem_loop_phase(&l, 1001, 1.0), 1.0, 0.0);
        CHECK_NEAR(stem_loop_phase(&l, 1399, 1.0), 399.0, 0.0);
    }

    T_CASE("the wrap");
    {
        struct stem_loop l = { 1000, 400 };

        CHECK_NEAR(stem_loop_phase(&l, 1400, 1.0), 0.0, 0.0);
        CHECK_NEAR(stem_loop_phase(&l, 1401, 1.0), 1.0, 0.0);
        CHECK_NEAR(stem_loop_phase(&l, 1800, 1.0), 0.0, 0.0);      /* two spans on */
        CHECK_NEAR(stem_loop_phase(&l, 1000 + 400 * 1000, 1.0), 0.0, 0.0);
    }

    T_CASE("before the loop was engaged");
    {
        struct stem_loop l = { 1000, 400 };

        /* The cast truncates toward zero, so a plain remainder is wrong here.
         * The loop extends backwards from where it was engaged. */
        CHECK_NEAR(stem_loop_phase(&l, 999, 1.0), 399.0, 0.0);
        CHECK_NEAR(stem_loop_phase(&l, 600, 1.0), 0.0, 0.0);       /* one span back */
        CHECK_NEAR(stem_loop_phase(&l, 601, 1.0), 1.0, 0.0);
        CHECK_NEAR(stem_loop_phase(&l, 200, 1.0), 0.0, 0.0);       /* two spans back */
        /* 1000 frames back is 2.5 spans, so this lands in the middle. */
        CHECK_NEAR(stem_loop_phase(&l, 0, 1.0), 200.0, 0.0);
    }

    T_CASE("phase does not depend on where playback started");
    {
        struct stem_loop l = { 96000, 12000 };
        double a = stem_loop_phase(&l, 500000, 1.0);

        /* Engaging, releasing and re-engaging must land in the same place: the
         * phase depends on the position alone. */
        CHECK_NEAR(stem_loop_phase(&l, 500000 + 12000, 1.0), a, 0.0);
        CHECK_NEAR(stem_loop_phase(&l, 500000 - 12000, 1.0), a, 0.0);
    }

    T_CASE("a slower file");
    {
        struct stem_loop l = { 0, 1000 };

        /* Half speed: a beat of the file takes two of the track's, so the loop
         * covers its 1000 frames over 2000 of the track's. */
        CHECK_NEAR(stem_loop_phase(&l, 0, 0.5), 0.0, 0.0);
        CHECK_NEAR(stem_loop_phase(&l, 1, 0.5), 0.5, 0.0);
        CHECK_NEAR(stem_loop_phase(&l, 1999, 0.5), 999.5, 0.0);
        CHECK_NEAR(stem_loop_phase(&l, 2000, 0.5), 0.0, 1e-9);
        CHECK_NEAR(stem_loop_phase(&l, 2001, 0.5), 0.5, 1e-9);
    }

    T_CASE("a faster file");
    {
        struct stem_loop l = { 0, 1000 };

        CHECK_NEAR(stem_loop_phase(&l, 1, 2.0), 2.0, 0.0);
        CHECK_NEAR(stem_loop_phase(&l, 499, 2.0), 998.0, 0.0);
        CHECK_NEAR(stem_loop_phase(&l, 500, 2.0), 0.0, 1e-9);
        /* Backwards past the start at a ratio other than 1: the sign of the
         * remainder and the wrap must agree. */
        CHECK_NEAR(stem_loop_phase(&l, -1, 2.0), 998.0, 1e-9);
        CHECK_NEAR(stem_loop_phase(&l, -500, 2.0), 0.0, 1e-9);
    }

    T_CASE("a ratio near one, which is the real case");
    {
        /* A 124 BPM loop under a 126 BPM track, four beats at 96 kHz. */
        struct stem_loop l = { 480000, 185806 };
        double ratio = 126.0 / 124.0;

        walk(&l, 480000, ratio, 96000);         /* a second of it */
        /* Far from the engage point the re-derived phase must still be inside
         * the span. */
        {
            double u = stem_loop_phase(&l, 480000 + 96000 * 600, ratio);

            CHECK(u >= 0.0 && u < (double)l.span);
        }
    }

    T_CASE("phase never reaches the span");
    {
        /* One past the last frame is the interpolator's second read, so a phase
         * that rounds up to the span indexes outside the buffer. Sweep the
         * positions either side of a wrap at a ratio with no exact binary
         * representation. */
        struct stem_loop l = { 7, 44100 };
        int64_t k;

        for (k = -50; k < 50; k++) {
            double u = stem_loop_phase(&l, 7 + 44100 * 37 + k, 0.9992);

            CHECK(u >= 0.0 && u < 44100.0);
        }
    }

    T_CASE("block walks");
    {
        struct stem_loop a = { 1000, 100 };     /* span shorter than a block */
        struct stem_loop b = { 4096, 4410 };
        struct stem_loop c = { 100000, 33333 }; /* started before the loop */

        walk(&a, 1000, 1.0, 2560);
        walk(&b, 4096 + 17, 1.03, 12800);
        walk(&c, 0, 0.97, 6400);
    }

    T_CASE("a one-frame span still lands in bounds");
    {
        struct stem_loop l = { 7, 1 };

        /* Too short to be a loop, but the maths must not divide by zero or
         * return the span itself. */
        CHECK_NEAR(stem_loop_phase(&l, 7, 1.0), 0.0, 0.0);
        CHECK_NEAR(stem_loop_phase(&l, 1000000, 1.0), 0.0, 0.0);
        CHECK(stem_loop_phase(&l, 1000000, 0.37) < 1.0);
    }

    T_CASE("a span or a ratio that is not a loop");
    {
        struct stem_loop l = { 1000, -5 };

        CHECK_NEAR(stem_loop_phase(&l, 42, 1.0), 0.0, 0.0);
        l.span = 400;
        CHECK_NEAR(stem_loop_phase(&l, 42, 0.0), 0.0, 0.0);
        CHECK_NEAR(stem_loop_phase(&l, 42, -1.0), 0.0, 0.0);
    }

    /* ---- the beat index: the same phase, measured off the grid ------------ */

    T_CASE("beats: nothing to measure against");
    {
        int64_t one[1] = { 500 };

        /* No array and a one-beat array both have no interval; 0 is the only
         * in-bounds answer. */
        CHECK_NEAR(stem_beat_at(NULL, 0, 1000, NULL), 0.0, 0.0);
        CHECK_NEAR(stem_beat_at(one, 1, 1000, NULL), 0.0, 0.0);
        CHECK_NEAR(stem_beat_at(one, 0, 1000, NULL), 0.0, 0.0);
    }

    T_CASE("beats: a fixed tempo agrees with the ratio it replaces");
    {
        /* Ten beats of a 120 BPM track at 48 kHz: 24000 frames each. */
        int64_t g[10];
        int i;

        for (i = 0; i < 10; i++)
            g[i] = 1000 + (int64_t)i * 24000;

        CHECK_NEAR(stem_beat_at(g, 10, 1000, NULL), 0.0, 0.0);
        CHECK_NEAR(stem_beat_at(g, 10, 1000 + 12000, NULL), 0.5, 1e-12);
        CHECK_NEAR(stem_beat_at(g, 10, 1000 + 24000, NULL), 1.0, 0.0);
        CHECK_NEAR(stem_beat_at(g, 10, 1000 + 24000 * 7, NULL), 7.0, 0.0);
        /* Across the covered range the beat index is exactly the elapsed frames
         * over the beat length, which is what the fixed-tempo ratio computes,
         * so both routes behave the same. */
        for (i = 0; i < 9 * 24000; i += 997)
            CHECK_NEAR(stem_beat_at(g, 10, 1000 + i, NULL), (double)i / 24000.0, 1e-9);
    }

    T_CASE("beats: outside the grid the end intervals extend");
    {
        int64_t g[4] = { 1000, 3000, 5000, 7000 };

        /* Before the first beat the result goes negative (the caller wraps
         * it); a track has audio before its analysis starts. */
        CHECK_NEAR(stem_beat_at(g, 4, 0, NULL), -0.5, 1e-12);
        CHECK_NEAR(stem_beat_at(g, 4, -1000, NULL), -1.0, 1e-12);
        /* After the last, the last interval carries on. */
        CHECK_NEAR(stem_beat_at(g, 4, 7000, NULL), 3.0, 0.0);
        CHECK_NEAR(stem_beat_at(g, 4, 8000, NULL), 3.5, 1e-12);
        CHECK_NEAR(stem_beat_at(g, 4, 11000, NULL), 5.0, 1e-12);
    }

    T_CASE("beats: a tempo that moves");
    {
        /* Beats that get shorter: 4000, then 2000, then 1000. A single ratio
         * cannot describe this. */
        int64_t g[5] = { 0, 4000, 6000, 7000, 8000 };

        CHECK_NEAR(stem_beat_at(g, 5, 2000, NULL), 0.5, 1e-12);
        CHECK_NEAR(stem_beat_at(g, 5, 4000, NULL), 1.0, 0.0);
        CHECK_NEAR(stem_beat_at(g, 5, 5000, NULL), 1.5, 1e-12);
        CHECK_NEAR(stem_beat_at(g, 5, 6500, NULL), 2.5, 1e-12);
        CHECK_NEAR(stem_beat_at(g, 5, 7500, NULL), 3.5, 1e-12);
        /* A beat boundary is exact from both sides: the interval either side of
         * it changes length, and the index must not. */
        CHECK_NEAR(stem_beat_at(g, 5, 5999, NULL), 1.9995, 1e-12);
        CHECK_NEAR(stem_beat_at(g, 5, 6000, NULL), 2.0, 0.0);
    }

    T_CASE("beats: monotone and inside its own range");
    {
        /* A drifting grid, walked frame by frame: the index must never go
         * backwards and must never jump, because the loop phase is this
         * multiplied by a beat length. */
        int64_t g[64];
        double prev;
        int i;
        int64_t at;

        g[0] = 500;
        for (i = 1; i < 64; i++)
            g[i] = g[i - 1] + 20000 + (i % 7) * 300 - (i % 3) * 500;

        prev = stem_beat_at(g, 64, 0, NULL);
        for (at = 1; at < g[63] + 40000; at += 13) {
            double b = stem_beat_at(g, 64, at, NULL);

            CHECK(b >= prev);
            CHECK(b - prev < 0.01);         /* 13 frames is a fraction of a beat */
            prev = b;
        }
        CHECK_NEAR(stem_beat_at(g, 64, g[63], NULL), 63.0, 0.0);
    }

    T_CASE("beats: an interval that is not a length");
    {
        /* Two beats in the same place are a beat of no duration. The index
         * steps a whole beat across it instead of dividing by the zero
         * interval, which would put a NaN index on the audio thread. */
        int64_t g[4] = { 0, 1000, 1000, 2000 };

        CHECK_NEAR(stem_beat_at(g, 4, 999, NULL), 0.999, 1e-12);
        CHECK_NEAR(stem_beat_at(g, 4, 1000, NULL), 2.0, 0.0);
        CHECK_NEAR(stem_beat_at(g, 4, 1500, NULL), 2.5, 1e-12);

        /* A grid with no extent at all: every answer is finite, in range, and
         * does not move. */
        {
            int64_t flat[2] = { 700, 700 };

            CHECK_NEAR(stem_beat_at(flat, 2, 0, NULL), 0.0, 0.0);
            CHECK_NEAR(stem_beat_at(flat, 2, 700, NULL), 1.0, 0.0);
            CHECK_NEAR(stem_beat_at(flat, 2, 9000, NULL), 1.0, 0.0);
        }
    }

    T_CASE("beats: the phase built out of one stays in bounds");
    {
        /* What the mix computes: (beat now - beat engaged) times the file's
         * beat length, wrapped. Swept across a wrap on a drifting grid, where
         * an off-by-one would read out of bounds. */
        int64_t g[32];
        double span = 4.0 * 18000.0;        /* a four-beat file at 18000/beat */
        double b0;
        int i;
        int64_t at;

        g[0] = 0;
        for (i = 1; i < 32; i++)
            g[i] = g[i - 1] + 19000 + (i % 5) * 700;

        b0 = stem_beat_at(g, 32, g[0], NULL);
        CHECK_NEAR(b0, 0.0, 0.0);
        for (at = -40000; at < g[31] + 40000; at += 7) {
            double u = stem_loop_wrap((stem_beat_at(g, 32, at, NULL) - b0) * 18000.0,
                                      (int64_t)span);

            CHECK(u >= 0.0 && u < span);
        }
    }

    T_CASE("beats: the cursor is a hint and never an answer");
    {
        /* The mix carries a cursor across a block so the interval is bisected
         * once instead of per frame. The cursor only speeds the lookup: any
         * cursor, in any state, must give exactly what NULL gives, or the
         * loop's phase would depend on where the blocks were cut. */
        int64_t g[48];
        int32_t cur = -1;
        int64_t at;
        int i;

        g[0] = 300;
        for (i = 1; i < 48; i++)
            g[i] = g[i - 1] + 17000 + (i % 11) * 900 - (i % 4) * 600;

        /* Walked forward, as a block does. */
        for (at = -5000; at < g[47] + 20000; at += 11)
            CHECK_NEAR(stem_beat_at(g, 48, at, &cur),
                       stem_beat_at(g, 48, at, NULL), 0.0);

        /* Jumped backwards past the hint (a seek, or a new track under an old
         * cursor). The hint no longer brackets and must be discarded. */
        cur = 40;
        CHECK_NEAR(stem_beat_at(g, 48, g[2] + 5, &cur),
                   stem_beat_at(g, 48, g[2] + 5, NULL), 0.0);
        CHECK(cur == 2);

        /* Jumped far forward, past the walk limit, so it bisects. */
        cur = 1;
        CHECK_NEAR(stem_beat_at(g, 48, g[44] + 5, &cur),
                   stem_beat_at(g, 48, g[44] + 5, NULL), 0.0);
        CHECK(cur == 44);

        /* Nonsense hints: out of range either way, and the one-past-the-end
         * index the loop above can leave behind. */
        {
            int32_t bad[5] = { -9, 0, 47, 48, 100000 };
            int b;

            for (b = 0; b < 5; b++) {
                for (at = -3000; at < g[47] + 9000; at += 4441) {
                    cur = bad[b];
                    CHECK_NEAR(stem_beat_at(g, 48, at, &cur),
                               stem_beat_at(g, 48, at, NULL), 0.0);
                }
            }
        }

        /* A hint carried past either end, where the answer does not come from
         * an interval and the cursor must not be consulted. */
        cur = 20;
        CHECK_NEAR(stem_beat_at(g, 48, -100000, &cur),
                   stem_beat_at(g, 48, -100000, NULL), 0.0);
        cur = 3;
        CHECK_NEAR(stem_beat_at(g, 48, g[47] + 100000, &cur),
                   stem_beat_at(g, 48, g[47] + 100000, NULL), 0.0);
    }

    T_CASE("the wrap on its own");
    {
        CHECK_NEAR(stem_loop_wrap(0.0, 400), 0.0, 0.0);
        CHECK_NEAR(stem_loop_wrap(399.5, 400), 399.5, 0.0);
        CHECK_NEAR(stem_loop_wrap(400.0, 400), 0.0, 0.0);
        CHECK_NEAR(stem_loop_wrap(-0.5, 400), 399.5, 0.0);
        CHECK_NEAR(stem_loop_wrap(-400.0, 400), 0.0, 0.0);
        CHECK_NEAR(stem_loop_wrap(1234.5, 400), 34.5, 1e-9);
        /* A span of 0 or less returns 0. */
        CHECK_NEAR(stem_loop_wrap(37.0, 0), 0.0, 0.0);
        CHECK_NEAR(stem_loop_wrap(37.0, -5), 0.0, 0.0);
    }

    return t_done("stem_loop");
}
