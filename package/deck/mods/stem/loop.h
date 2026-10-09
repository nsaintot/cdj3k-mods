// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * stem/loop.h - where a looped file reads, as arithmetic and nothing else.
 *
 * Split out of the mix so it can be tested. A negative remainder, an
 * off-by-one at the wrap, or an index landing exactly on the span is a bad
 * pointer on the audio thread, i.e. a crash or a dropout. As pure arithmetic
 * over its arguments, tests/test_stem_loop.c covers the boundary cases without
 * a deck.
 *
 * Track positions are pool-rate sample indices -- the timeline the stems were
 * decoded onto, which is also the one the cue table stores. File positions are
 * indices into the loop's own buffer, which is decoded at that same rate but
 * advances at its own tempo.
 */
#ifndef EP122_MODS_STEM_LOOP_H
#define EP122_MODS_STEM_LOOP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


/* One loop. `from` is the track position it was engaged at and `span` its length
 * in FILE frames; `span == 0` means there is no loop. */
struct stem_loop {
    int64_t from;
    int64_t span;
};

/* Where in the file track position `at` reads, as a fractional file index in
 * [0, span). The caller interpolates between floor(u) and its successor, so the
 * result being strictly below `span` is what keeps that successor in bounds.
 *
 * `ratio` is file frames per track frame. At 1.0 the file plays at its own
 * speed; at 0.5 a beat of the file takes two of the track's (a 60 BPM loop
 * under a 120 BPM track). The tempo fader needs no term of its own: `at`
 * already advances with it.
 *
 * One ratio is one tempo, so on a track whose tempo moves this drifts by
 * however much the grid drifts. stem_beat_at measures the same phase in beats;
 * this is for tracks with no grid.
 *
 * Phase comes from `from`, not from a cursor, so the loop sits where it would
 * have been had it run since it was engaged: engaging, releasing and
 * re-engaging all land in time, and a block boundary cannot slip it. Also
 * defined for `at` before `from`; the loop extends in both directions. */
double stem_loop_phase(const struct stem_loop *l, int64_t at, double ratio);

/* The same wrap on its own, for a caller that computed the file index some other
 * way. Answers in [0, span), and 0 for a span that is not a length. */
double stem_loop_wrap(double u, int64_t span);

/* The track's fractional beat index at position `at`, over `count` ascending
 * beat positions on the track's own timeline.
 *
 * A loop's file index is beats elapsed times the file's beat length, and beats
 * elapsed is (at - from) / spb only while the track holds one tempo. Measured
 * off the grid it is right for a track that speeds up, slows down, or was
 * gridded by hand a bar at a time.
 *
 * Whole numbers are beats: 4.5 is halfway between beats[4] and beats[5], so the
 * fraction is measured within its own interval and a tempo change between two
 * beats never moves the beats either side of it.
 *
 * Defined outside the grid too, by extending the first and last intervals: a
 * track has audio before its first analysed beat. Answers 0 for anything that
 * is not an array of at least two beats, and drops the fraction across an
 * interval that is not positive rather than dividing by it.
 *
 * `cursor` is an optional hint; NULL is fine. Give it one per block,
 * initialised to -1: the interval is found by bisection once and then walked,
 * so each later frame costs a compare (cheaper than the single-ratio route's
 * division). The hint is verified, so a wrong one (a seek, a reordered block,
 * an uninitialised cursor) only costs the bisection. */
double stem_beat_at(const int64_t *beats, int32_t count, int64_t at,
                    int32_t *cursor);


#ifdef __cplusplus
}
#endif

#endif /* EP122_MODS_STEM_LOOP_H */
