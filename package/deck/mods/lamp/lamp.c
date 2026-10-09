// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * lamp/lamp.c - which feature owns a pad, in one readable order.
 *
 * The sources are called by name rather than registering themselves, so the
 * priority order is stated in one place.
 */
#include "lamp/lamp.h"
#include "xpad/xpad.h"       /* the panel's sample banks */
#include "stem/stem.h"       /* the groove circuit       */

const uint8_t k_lamp_wheel[LAMP_HUES][3] = {
    { 255,   0,   0 },   /* red        */
    { 255, 128,   0 },   /* orange     */
    { 255, 255,   0 },   /* yellow     */
    { 128, 255,   0 },   /* chartreuse */
    {   0, 255,   0 },   /* green      */
    {   0, 255, 128 },   /* spring     */
    {   0, 255, 255 },   /* cyan       */
    {   0, 128, 255 },   /* azure      */
    {   0,   0, 255 },   /* blue       */
    { 128,   0, 255 },   /* violet     */
    { 255,   0, 255 },   /* magenta    */
    { 255,   0, 128 },   /* rose       */
};

uint32_t lamp_now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint32_t)(ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000));
}

/* Priority order:
 *
 *   1. the startup dance, which owns every pad while it runs and then declines
 *      forever. It ends before a deck can be played.
 *   2. the X-PAD, because while its panel is open the pads are sample banks.
 *      The band holds one panel, so the stems row that arms the circuit is
 *      shut and this cannot collide with 3.
 *   3. the groove circuit, whose pads apply only while the stems row is open;
 *      gc_pad_lamp checks that itself.
 *
 * Unclaimed pads keep the colour the app painted. */
int lamp_pad(int pad, struct lamp *out)
{
    if (pad < 0 || pad >= LAMP_PADS)
        return 0;
    if (lamp_dance_ask(pad, out))
        return 1;
    if (xpad_pad_lamp(pad, out))
        return 1;
    return gc_pad_lamp(pad, out);
}

uint32_t lamp_word(void)
{
    uint32_t h = 2166136261u;   /* FNV-1a */
    int p;

    for (p = 0; p < LAMP_PADS; p++) {
        struct lamp l;
        uint8_t     v[4];
        int         i;

        if (!lamp_pad(p, &l))
            lamp_dark(&l);
        v[0] = l.level;
        v[1] = l.rgb[0];
        v[2] = l.rgb[1];
        v[3] = l.rgb[2];
        for (i = 0; i < 4; i++) {
            h ^= v[i];
            h *= 16777619u;
        }
    }
    return h;
}

void lamp_panel_ready(void)
{
    static int said;

    if (said)
        return;
    said = 1;
    lamp_dance_start();
}
