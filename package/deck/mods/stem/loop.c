// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * stem/loop.c - see loop.h. Pure: no shim state, no libc, nothing to stub.
 */
#include "stem/loop.h"

double stem_loop_wrap(double u, int64_t span)
{
    double s;
    int64_t turns;

    if (span <= 0)
        return 0.0;
    s = (double)span;

    /* fmod by hand, because this file links into the tests without libm. The
     * cast truncates toward zero, so a position before `from` comes out
     * negative and is wrapped forward below. */
    turns = (int64_t)(u / s);
    u -= (double)turns * s;
    if (u < 0.0)
        u += s;

    /* A `u` a hair under zero lands on `span` exactly once the span is added
     * back, and `span` is one past the last frame. */
    if (u >= s)
        u = 0.0;
    return u;
}

double stem_loop_phase(const struct stem_loop *l, int64_t at, double ratio)
{
    if (l->span <= 0 || !(ratio > 0.0))
        return 0.0;
    return stem_loop_wrap((double)(at - l->from) * ratio, l->span);
}

/* How far ahead of the hint to walk. A block is a few milliseconds and a beat a
 * few hundred, so the walk usually ends on its first or second compare.
 * Anything further is a seek, and bisecting is cheaper than stepping. */
#define BEAT_WALK_MAX   4

double stem_beat_at(const int64_t *beats, int32_t count, int64_t at,
                    int32_t *cursor)
{
    int32_t lo, hi;
    int64_t step;

    if (!beats || count < 2)
        return 0.0;

    /* Before the grid and after it, the nearest interval is extended. The first
     * answer is negative, which is what the caller's wrap is for. */
    if (at < beats[0]) {
        step = beats[1] - beats[0];
        return step > 0 ? (double)(at - beats[0]) / (double)step : 0.0;
    }
    if (at >= beats[count - 1]) {
        step = beats[count - 1] - beats[count - 2];
        if (step <= 0)
            return (double)(count - 1);
        return (double)(count - 1)
             + ((double)(at - beats[count - 1]) / (double)step);
    }

    lo = -1;

    /* Use the hint if it still brackets `at` or is a step or two behind it.
     * It is verified, so everything below is the same whether `lo` came from
     * here or from the bisection. */
    if (cursor) {
        int32_t i = *cursor, n;

        if (i >= 0 && i <= count - 2 && beats[i] <= at) {
            for (n = 0; n < BEAT_WALK_MAX && i < count - 2 &&
                        at >= beats[i + 1]; n++)
                i++;
            if (at < beats[i + 1])
                lo = i;
        }
    }

    if (lo < 0) {
        /* beats[lo] <= at < beats[hi] on entry, and every step keeps it. The
         * array is ascending: grid.c refuses one that is not, since a bisection
         * over an unordered array gives wrong answers without failing. */
        lo = 0;
        hi = count - 1;
        while (hi - lo > 1) {
            int32_t mid = lo + (hi - lo) / 2;

            if (beats[mid] <= at)
                lo = mid;
            else
                hi = mid;
        }
    }
    if (cursor)
        *cursor = lo;

    step = beats[lo + 1] - beats[lo];
    return step > 0 ? (double)lo + ((double)(at - beats[lo]) / (double)step)
                    : (double)lo;
}
