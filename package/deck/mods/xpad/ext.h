// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * xpad/ext.h - the X-PAD SAMPLER as the rest of the shim sees it.
 *
 * Entry points called from outside the X-PAD, in a header of their own so
 * callers do not pull in juce or xpad.h.
 */
#ifndef EP122_MOD_XPAD_EXT_H
#define EP122_MOD_XPAD_EXT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


/* Add every sounding voice into `dst`, which holds `frames` of interleaved
 * stereo float at the pool rate and begins at track position `pos`.
 *
 * Post-stretch, unlike the stems. Stems are summed into the stretcher's input so
 * the deck warps them with the track; a one-shot keeps its own pitch and length
 * whatever the tempo fader does, and only the X-PAD's Y axis moves either.
 *
 * `pos` is the block's place in the track and must be passed in, because only
 * the caller knows it. stem_source_pos(), where the stretcher last read its
 * source, runs 8055..9271 frames ahead on a playing deck (84 to 97 ms at
 * 96 kHz); timing off it puts every quantized hit about 0.1 s early.
 *
 * [audio]. No allocation, no I/O, no locks, bounded loops. Returns immediately
 * when nothing is sounding, which includes every block while the panel is shut. */
void xpad_mix(float *dst, int64_t frames, int64_t pos);

/* Rescan mods/loops/ if the volume or the pool rate has changed. Cheap otherwise.
 * [worker] -- the shim's one idle worker, the only thread allowed to decode. */
void xpad_bank_poll(void);

/* [any] The track's beat position as the X-PAD reads it: from the track's grid
 * while the play head moves, from the track's tempo when it does not, so it
 * keeps running on a parked deck. Returns 0 when there is no clock, i.e. the
 * track has no grid.
 *
 * This is a position, not an edge. Compare the boundary it falls in against the
 * last one you saw and act when it changes; do not test whether a boundary fell
 * inside an interval, because this is advanced post-stretch and every other
 * caller runs at its own cadence. See the note at the definition.
 *
 * Shared because it is the only clock on the audio path; used by the sampler's
 * snapping and the stem row's mute. */
int xpad_beat_now(double *beat);

/* ENABLE X-PAD, the master gate. Off by default, so a deck that never opts in
 * keeps its band slot and its full track title. Also declared in xpad.h; here
 * so the settings record can persist it without including xpad.h. */
extern int xpad_g_on;

/* [any] The deck's QUANTIZE as a divisor of a beat (1, 2, 4 or 8), or 0 when it
 * is off. Also declared in xpad.h; here for callers that only need this. */
int xpad_quantize_div(void);


#ifdef __cplusplus
}
#endif

#endif /* EP122_MOD_XPAD_EXT_H */
