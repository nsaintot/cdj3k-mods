// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * lamp/dance.c - the startup animation: a rainbow brightness wave across the pads.
 *
 * Runs once, when the transport has confirmed the lamp path end to end (see
 * lamp_panel_ready), then releases the pads permanently.
 *
 * Each pad holds a fixed hue, red at A through violet at H, and a single broad
 * wave of brightness travels left to right over them.
 *
 * The panel has only three brightnesses (lamp.h, rules 1-2), so lamps cannot
 * fade. The smooth look comes from the wave's width: a lit core about 1.5 pads
 * across inside a dim body of about 4.5, so four or five lamps glow at once and
 * each goes off -> dim -> lit -> dim -> off as the wave passes. The wave starts
 * and ends beyond the row, so the row is dark at both ends.
 *
 * The wave's position comes from the clock, not a frame count: the display
 * tick's rate varies with load, so a slow tick drops frames instead of slowing
 * the animation.
 */
#include "lamp/lamp.h"

#include <math.h>

/* How long the whole pass takes, and how wide the wave is in pad-widths. */
#define DANCE_MS      1700u
#define DANCE_SPREAD  2.2f

/* The lit core, as a fraction of the spread. */
#define DANCE_CORE    0.34f

/* Red through violet, as indices into the wheel. Skips spring and
 * azure, which are too close to their neighbours, and magenta and rose, which
 * would turn the row back towards red. */
static const uint8_t k_dance_hue[LAMP_PADS] = {
    0,  /* red        */
    1,  /* orange     */
    2,  /* yellow     */
    3,  /* chartreuse */
    4,  /* green      */
    6,  /* cyan       */
    8,  /* blue       */
    9,  /* violet     */
};

static int      dance_g_on;
static uint32_t dance_g_t0;

void lamp_dance_start(void)
{
    __atomic_store_n(&dance_g_t0, lamp_now_ms(), __ATOMIC_RELAXED);
    __atomic_store_n(&dance_g_on, 1, __ATOMIC_RELEASE);
    MDBG("lamp: startup dance\n");
}

int lamp_dance_ask(int pad, struct lamp *out)
{
    uint32_t     el;
    float        centre, d;
    const uint8_t *hue;

    if (!__atomic_load_n(&dance_g_on, __ATOMIC_ACQUIRE))
        return 0;

    el = lamp_now_ms() - __atomic_load_n(&dance_g_t0, __ATOMIC_RELAXED);
    if (el >= DANCE_MS) {
        /* Finished: declines from now on, and the pads pass to the next
         * source on this same frame. */
        __atomic_store_n(&dance_g_on, 0, __ATOMIC_RELAXED);
        return 0;
    }

    /* The wave's centre in pad units, from one spread beyond the left end to
     * one spread beyond the right. */
    centre = (float)el / (float)DANCE_MS
             * ((float)(LAMP_PADS - 1) + 2.0f * DANCE_SPREAD) - DANCE_SPREAD;
    d = fabsf((float)pad - centre);

    hue = k_lamp_wheel[k_dance_hue[pad]];
    lamp_set(out, hue[0], hue[1], hue[2],
             d <= DANCE_SPREAD * DANCE_CORE ? LAMP_LIT :
             d <= DANCE_SPREAD             ? LAMP_DIM : LAMP_OFF);
    return 1;
}
