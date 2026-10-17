// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * ui/pads.c - pads F, G and H as mutes for DRUMS, HARMONICS and VOCALS
 *
 * Part of the STEMS play-screen UI. With STEM MUTE PADS on, the three pads do what
 * the MUTE keys do, as set by STEM MUTE MODE, whether the row is open or not. The X-PAD
 * panel takes them first; they take them from the groove circuit.
 *
 * Unlike the rest of stem/ui/, this file also runs off the message thread: the cue
 * handler runs on [deck] and the lamp on [any]. The handler only records presses;
 * stems_pad_poll applies them on the display tick, where the mute state lives.
 */
#include "stem/ui/ui.h"
#include "cue/cue.h"
#include "lamp/lamp.h"

#define PAD_F 5   /* pads F, G, H = 0-based 5, 6, 7 */

/* [deck] writes, [message] reads. TOGGLE counts presses; HOLD stores whether the pad
 * is down. */
static unsigned g_pad_taps[N_STEMS];
static int      g_pad_down[N_STEMS];

/* A pad that was down when the mutes were cleared stays ignored until it is
 * released, so a held pad does not mute the next track. [message] */
static int      g_pad_ignore[N_STEMS];

/* Set by the first poll. Until the display tick runs, nothing would apply a press,
 * so the pads are not claimed. [message] writes, [deck] reads. */
static int      g_polled;

/* What the pads show while no stems are resident. [message] writes on the display
 * tick (stem_ui_read is [message]); [any] reads (the lamp). */
enum { LOAD_NONE, LOAD_BUSY, LOAD_FAILED };
static int g_load;

/* FAILED, once settled, holds until the track changes, stems land or a server
 * appears: a failed job is retried every few seconds, which would otherwise
 * alternate the pads between orange and the loading blink. [message] */
static int g_failed;
static int g_failed_settle;
static int g_was_available = 1;

#define PAD_LOAD_BLINK_MS 250
#define PAD_ORANGE        1    /* k_lamp_wheel index */

/* The part a pad mutes, or -1 when it is not a mute pad right now. [any] */
static int stems_pad_part(int pad)
{
    if (!g_stem_mute_pads || !g_stems_on)
        return -1;
    if (pad < PAD_F || pad >= PAD_F + N_STEMS)
        return -1;
    return pad - PAD_F;
}

/* Taken even when there is nothing to mute: the pad is dark then, and a dark pad
 * must not jump to a hot cue or start a loop. */
static int stems_pad_claim(const struct cue_event *ev)
{
    return __atomic_load_n(&g_polled, __ATOMIC_ACQUIRE) && stems_pad_part(ev->pad) >= 0;
}

static void stems_pad(const struct cue_event *ev, enum cue_phase phase)
{
    int i;

    if (phase == CUE_PAD_UP) {
        /* No setting check, so a release after STEM MUTE PADS is turned off still
         * clears the hold. */
        i = ev->pad - PAD_F;
        if (i >= 0 && i < N_STEMS)
            __atomic_store_n(&g_pad_down[i], 0, __ATOMIC_RELEASE);
        return;
    }
    if (phase != CUE_PAD_PRESSED)
        return;
    i = stems_pad_part(ev->pad);
    if (i < 0)
        return;
    if (!stems_ready() || __atomic_load_n(&stems_g_bypass_on, __ATOMIC_RELAXED)) {
        MDBG("stems: pad %c pressed with nothing to mute -> ignored\n", 'A' + ev->pad);
        return;
    }
    if (g_stem_mute_toggle)
        __atomic_add_fetch(&g_pad_taps[i], 1, __ATOMIC_ACQ_REL);
    else
        __atomic_store_n(&g_pad_down[i], 1, __ATOMIC_RELEASE);
}

/* Prio 4: after the X-PAD (3), whose panel owns the pads while open, and ahead of the
 * groove circuit (5). lamp/lamp.c asks the lamp sources in the same order. */
CUE_HANDLER(k_cue_stem_mute,
            .name = "stem_mute", .prio = 4,
            .pad = stems_pad, .pad_claim = stems_pad_claim);

/* Loading: a job is on its way. Failed: the job failed, or there is no usable server
 * and nothing on the stick (the condition the STEMS warn badge shows). */
static int load_raw(void)
{
    struct stem_ui_state st;

    if (stems_ready())
        return LOAD_NONE;
    if (!stems_available())
        return LOAD_FAILED;
    if (!stem_ui_read(&st))
        return LOAD_NONE;
    if (st.stage == STEM_STAGE_FAILED)
        return LOAD_FAILED;
    if (stem_stage_busy(st.stage))
        return LOAD_BUSY;
    return LOAD_NONE;
}

/* FAILED shows after the warn badge's settle time; until then the stems are still on
 * their way. An offline deck reports "unavailable" for a moment on every track
 * change, before the cache lookup starts. */
static int load_state(void)
{
    int raw = load_raw(), available = stems_available();

    if (stems_ready() || (available && !g_was_available))
        g_failed = 0;
    g_was_available = available;
    if (raw == LOAD_FAILED) {
        if (!g_failed && ++g_failed_settle > WARN_SETTLE_TICKS)
            g_failed = 1;
    } else
        g_failed_settle = 0;
    if (g_failed)
        return LOAD_FAILED;
    return raw == LOAD_FAILED ? LOAD_BUSY : raw;
}

int stems_loading(void)
{
    return __atomic_load_n(&g_load, __ATOMIC_RELAXED) == LOAD_BUSY;
}

void stems_pads_forget(void)
{
    int i;

    for (i = 0; i < N_STEMS; i++)
        if (__atomic_load_n(&g_pad_down[i], __ATOMIC_ACQUIRE))
            g_pad_ignore[i] = 1;
}

void stems_pads_track_changed(void)
{
    stems_pads_forget();
    g_failed = 0;
    g_failed_settle = 0;
}

void stems_pad_poll(void)
{
    static unsigned seen[N_STEMS];
    int i;

    __atomic_store_n(&g_polled, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_load, load_state(), __ATOMIC_RELAXED);

    for (i = 0; i < N_STEMS; i++) {
        unsigned taps = __atomic_load_n(&g_pad_taps[i], __ATOMIC_ACQUIRE);
        int down, held;

        if (taps != seen[i]) {
            if ((taps - seen[i]) & 1u)
                stems_mute_flip(i);
            seen[i] = taps;
        }
        down = __atomic_load_n(&g_pad_down[i], __ATOMIC_ACQUIRE);
        if (g_pad_ignore[i]) {
            if (!down) g_pad_ignore[i] = 0;
            down = 0;
        }
        held = (stems_g_mute_by[i] & STEMS_MUTE_PAD) != 0;
        /* A hold with no stems resident is refused; do not retry it every tick. */
        if (down != held && (!down || stems_ready()))
            stems_mute_hold(i, STEMS_MUTE_PAD, down);
    }
}

/* Dim white / dark blink: stems are loading.
 * Orange: the stems failed, or there is no server and nothing on the stick.
 * Dark: nothing to mute (no track, or the STEMS switch on OFF).
 * Part colour: the part plays.
 * White / part colour blink: muted, on the mute blink (stems_g_mute_lit). A new mute
 * starts on white, so even a short hold shows. */
int stems_pad_lamp(int pad, struct lamp *out)
{
    int i = stems_pad_part(pad), by;

    if (i < 0 || !__atomic_load_n(&g_polled, __ATOMIC_ACQUIRE))
        return 0;
    if (!stems_ready()) {
        switch (__atomic_load_n(&g_load, __ATOMIC_RELAXED)) {
        case LOAD_BUSY:
            if ((lamp_now_ms() / PAD_LOAD_BLINK_MS) & 1)
                lamp_dark(out);
            else
                lamp_set(out, 255, 255, 255, LAMP_DIM);
            break;
        case LOAD_FAILED:
            lamp_set(out, k_lamp_wheel[PAD_ORANGE][0], k_lamp_wheel[PAD_ORANGE][1],
                     k_lamp_wheel[PAD_ORANGE][2], LAMP_LIT);
            break;
        default:
            lamp_dark(out);
        }
        return 1;
    }
    if (__atomic_load_n(&stems_g_bypass_on, __ATOMIC_RELAXED)) {
        lamp_dark(out);
        return 1;
    }
    by = __atomic_load_n(&stems_g_mute_by[i], __ATOMIC_RELAXED);
    if (by && __atomic_load_n(&stems_g_mute_lit, __ATOMIC_RELAXED))
        lamp_set(out, 255, 255, 255, LAMP_LIT);
    else
        lamp_set(out, k_stem_pad_hue[i][0], k_stem_pad_hue[i][1],
                 k_stem_pad_hue[i][2], LAMP_LIT);
    return 1;
}
