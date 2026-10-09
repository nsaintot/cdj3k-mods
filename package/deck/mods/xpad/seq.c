// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * xpad/seq.c - OVERDUB: four beats of the track, as events.
 *
 * Stores events (what happened and where in the bar), not audio; replay
 * re-triggers them. The RMX-1000's OVERDUB mutes and deletes per X-PAD button,
 * which requires per-source storage rather than a mixed buffer.
 *
 * Two kinds. A HIT is a pad firing. A PAD event is the X-PAD's gesture (loop
 * length and bend), so a sweep across the strip becomes part of the loop. Both
 * use the same four-beat phase; only their replay differs.
 *
 * There is no capture buffer, feedback path or gain stacking, and an event is 24
 * bytes. A replayed hit goes through the same read head as a live one, so it is
 * pitched and rolled by the strip's current state, not its state when recorded.
 *
 * The phase is relative to the track's grid, modulo four beats. A hit at track
 * beat 10.7 is stored at 2.7 and fires at every beat congruent to 2.7 while
 * OVERDUB is on: no window, no bar counter, no drift. After a seek the bar
 * follows the track.
 *
 * Threads: [audio] records and plays, in that order within one block, so the
 * array has a single writer and needs no lock; [deck] (DELETE) and [message]
 * (the panel) arm and clear.
 */
#include "xpad/xpad.h"
#include "kit/mod.h"

#include <math.h>

enum xp_ev_kind { XP_EV_HIT, XP_EV_PAD };

struct xp_event {
    double  phase;      /* 0 .. XP_SEQ_BEATS */
    int32_t kind;
    int32_t bank;       /* HIT: which pad                     */
    int32_t div;        /* PAD: brick, or XP_DIV_NONE for off */
    float   semis;      /* PAD: the bend                      */
};

static struct xp_event xpad_g_ev[XP_SEQ_MAX];

/* Published last on a record and cleared first on a wipe, so a reader sees
 * either a fully written event or none. */
static int xpad_g_nev;

/* What the bar is playing back on the strip. Audio-thread state, cleared with
 * the bar. */
static int   xpad_g_auto_div = XP_DIV_NONE;
static float xpad_g_auto_semis;

int xpad_seq_count(void)
{
    return __atomic_load_n(&xpad_g_nev, __ATOMIC_ACQUIRE);
}

void xpad_seq_clear(void)
{
    __atomic_store_n(&xpad_g_nev, 0, __ATOMIC_RELEASE);
    xpad_g_auto_div   = XP_DIV_NONE;
    xpad_g_auto_semis = 0.0f;
}

int xpad_seq_auto(int *div, float *semis)
{
    if (xpad_g_auto_div == XP_DIV_NONE)
        return 0;
    *div   = xpad_g_auto_div;
    *semis = xpad_g_auto_semis;
    return 1;
}

static double xp_wrap(double b)
{
    double p = b - (double)XP_SEQ_BEATS * floor(b / (double)XP_SEQ_BEATS);

    /* floor keeps this in range for negative beats too: a track has audio
     * before its first analysed beat. */
    if (!(p >= 0.0) || !(p < (double)XP_SEQ_BEATS))
        return 0.0;
    return p;
}

/* Append, evicting the oldest when the bar is full.
 *
 * Oldest by recording time, not bar position: the array is in insertion order,
 * so this is a shift by one. A full bar never refuses a new event. */
static void xp_seq_push(const struct xp_event *ev)
{
    int n = xpad_g_nev;

    if (n >= XP_SEQ_MAX) {
        memmove(&xpad_g_ev[0], &xpad_g_ev[1],
                (XP_SEQ_MAX - 1) * sizeof(xpad_g_ev[0]));
        n = XP_SEQ_MAX - 1;
        __atomic_store_n(&xpad_g_nev, n, __ATOMIC_RELEASE);
    }
    xpad_g_ev[n] = *ev;
    __atomic_store_n(&xpad_g_nev, n + 1, __ATOMIC_RELEASE);
}

void xpad_seq_record(int bank, double beat)
{
    struct xp_event ev;

    if (!xpad_g_overdub || bank < 0 || bank >= XP_BANKS)
        return;
    ev.phase = xp_wrap(beat);
    ev.kind  = XP_EV_HIT;
    ev.bank  = bank;
    ev.div   = XP_DIV_NONE;
    ev.semis = 0.0f;
    xp_seq_push(&ev);
}

void xpad_seq_record_pad(int div, float semis, double beat)
{
    struct xp_event ev;

    if (!xpad_g_overdub)
        return;
    ev.phase = xp_wrap(beat);
    ev.kind  = XP_EV_PAD;
    ev.bank  = -1;
    ev.div   = div;
    ev.semis = semis;
    xp_seq_push(&ev);
}

/* Fire whatever falls in (b0, b1]. Half-open at the low end so a boundary landing
 * exactly on a block edge fires once rather than twice.
 *
 * The span is compared in bar phase. A block never covers a whole bar, so the
 * only special case is the span crossing the bar's wrap. */
void xpad_seq_play(double b0, double b1)
{
    double p0, p1;
    int n, i;

    n = __atomic_load_n(&xpad_g_nev, __ATOMIC_ACQUIRE);
    if (n <= 0 || !(b1 > b0))
        return;
    if (b1 - b0 >= (double)XP_SEQ_BEATS)
        return;                 /* a block cannot span a bar; this is a seek */

    p0 = xp_wrap(b0);
    p1 = xp_wrap(b1);

    for (i = 0; i < n; i++) {
        double p = xpad_g_ev[i].phase;
        int hit;

        if (p1 > p0)
            hit = (p > p0 && p <= p1);
        else
            hit = (p > p0 || p <= p1);      /* the span wrapped the bar */
        if (!hit)
            continue;
        if (xpad_g_ev[i].kind == XP_EV_HIT) {
            /* At the recorded phase, on its own frame inside this block. */
            double d = p - p0;

            if (d <= 0.0)
                d += (double)XP_SEQ_BEATS;      /* the span wrapped the bar */
            xpad_seq_fire_at(xpad_g_ev[i].bank, b0 + d);
        } else {
            /* In insertion order, so of two gesture events in one block the
             * later one wins. */
            xpad_g_auto_div   = xpad_g_ev[i].div;
            xpad_g_auto_semis = xpad_g_ev[i].semis;
        }
    }
}

/* ---- the arming, from the panel ------------------------------------------- */

/* Turning OVERDUB off clears the bar, as on the RMX. There is no spare button
 * for a per-layer delete. */
void xpad_overdub_set(int on)
{
    if (on == xpad_g_overdub)
        return;
    xpad_g_overdub = on;
    if (!on)
        xpad_seq_clear();
    MDBG("xpad: overdub %s%s\n", on ? "armed" : "off",
         on ? "" : " -> the bar is cleared");
}
