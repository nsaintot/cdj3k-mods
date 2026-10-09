// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * lamp/lamp.h - what a pad colour means on this panel, and which feature sets it.
 *
 * Three layers; this is the middle one:
 *
 *   cue/led.c   the transport. Hooks hui::MultiColor::update, learns which
 *               ordinal is which pad, holds each lamp's writer, and writes.
 *   lamp/       the control. What a lamp value is, what the panel can render,
 *               and which feature owns a pad when more than one wants it.
 *   xpad/ stem/ the sources. Each answers for its own pads only.
 *
 * A source never writes a lamp: it fills a struct lamp and says whether the pad
 * is its own.
 *
 * ================================================================== *
 * Rules
 * ================================================================== *
 *
 * The app passes the write a colour and a slot index, and the write runs the
 * pair through a transform before it reaches the panel. These rules describe
 * that transform.
 *
 * The slot is the brightness; the triple is a chroma.
 *
 *   1. The triple is first normalised by its own maximum. (0,64,0) and
 *      (0,255,0) render identically, as do (128,128,128) and (255,255,255).
 *      Smaller numbers do not dim a lamp.
 *
 *   2. The slot sets the brightness. Every hue tops out at 0x7f on slot 2 and
 *      0x0c on slot 1, the same ratio for all hues.
 *
 *   3. Saturation is boosted. (255,200,200) renders pure red and
 *      (200,255,200) pure green: a washed-out request renders as the nearest
 *      pure hue.
 *
 *   4. The channels are calibrated against each other. R, G and B each reach
 *      the slot maximum alone, but white renders #44787f and cyan #004b7f: a
 *      mix is weighted, not summed, and not by a single gamma. A secondary
 *      cannot be predicted from its primaries.
 *
 * So the panel has exactly three brightnesses: off, slot 1, slot 2. An
 * animation's envelope comes from those three and from how many lamps are lit
 * at once, not from fading one.
 *
 * There is no neutral grey. White renders as the panel's calibrated near-white,
 * #060c0c on slot 1, which is the colour the deck uses for a pad with no hot
 * cue. White on dim therefore means "nothing here".
 */
#ifndef EP122_MOD_LAMP_H
#define EP122_MOD_LAMP_H

#include "core/mod_core.h"

#ifdef __cplusplus
extern "C" {
#endif


/* The eight hot-cue pads. Same as CUE_PADS and XP_BANKS, redefined because this
 * layer sits under both and must not include either. */
#define LAMP_PADS 8

#define LAMP_OFF  0
#define LAMP_DIM  1   /* slot 1: every hue tops out at 0x0c */
#define LAMP_LIT  2   /* slot 2: every hue tops out at 0x7f */

/* One lamp's state. `rgb` is a hue with no brightness (see rule 1); use
 * LAMP_OFF for dark, since scaling the triple down has no effect. */
struct lamp {
    uint8_t rgb[3];
    uint8_t level;
};

static inline void lamp_set(struct lamp *l, uint8_t r, uint8_t g, uint8_t b,
                            int level)
{
    l->rgb[0] = r;
    l->rgb[1] = g;
    l->rgb[2] = b;
    l->level  = (uint8_t)level;
}

static inline void lamp_dark(struct lamp *l)
{
    lamp_set(l, 0, 0, 0, LAMP_OFF);
}

/* The hue wheel, as the panel renders each entry; all are distinct:
 *
 *   red #7f0000  orange #7f2400  yellow #7f7f00  chartreuse #267f00
 *   green #007f00  spring #007c3b  cyan #004b7f  azure #00247f
 *   blue #00007f  violet #33007f  magenta #7e007f  rose #7f0017
 *
 * Any other hue renders unpredictably (rule 4). */
#define LAMP_HUES 12
extern const uint8_t k_lamp_wheel[LAMP_HUES][3];

/* ---- the control --------------------------------------------------------- */

/* What one pad should look like, or 0 when no feature claims it and the app's
 * own colour stands. [any] */
int lamp_pad(int pad, struct lamp *out);

/* All pads' state hashed into one word; the transport writes only when it
 * changes. A 32-bit hash of eight (level, hue) pairs, so a collision costs one
 * skipped frame. [any] */
uint32_t lamp_word(void);

/* Milliseconds from CLOCK_MONOTONIC, so animations do not depend on the draw
 * rate. [any] */
uint32_t lamp_now_ms(void);

/* The transport has seen all eight lamps write: the hook is in, the ordinals
 * are mapped and the holders are known. Idempotent; only the first call
 * acts. [message] */
void lamp_panel_ready(void);

/* ---- internal to lamp/ --------------------------------------------------- */

void lamp_dance_start(void);              /* lamp.c, on lamp_panel_ready */
int  lamp_dance_ask(int pad, struct lamp *out);


#ifdef __cplusplus
}
#endif

#endif /* EP122_MOD_LAMP_H */
