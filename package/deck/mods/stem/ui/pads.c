// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * ui/pads.c - pads F, G and H as mutes for DRUMS, HARMONICS and VOCALS
 *
 * Part of the STEMS play-screen UI. With STEM MUTE PADS on, the three pads do
 * what the captions above the wedges do, as set by STEM MUTE MODE, whether the row
 * is open or not. The X-PAD panel takes them first; they take them from the
 * groove circuit.
 *
 * Presses arrive on [deck] and the mute state is the row's ([message]), so the
 * handler only records them and stems_pad_poll applies them on the display tick.
 */
#include "stem/ui/ui.h"
#include "cue/cue.h"
#include "lamp/lamp.h"

#define PAD_F 5   /* pads F, G, H = 0-based 5, 6, 7 */

/* [deck] writes, [message] reads. TOGGLE counts presses; HOLD keeps the pad
 * down or up. */
static unsigned g_pad_taps[N_STEMS];
static int      g_pad_down[N_STEMS];

/* What the pads show while no stems are resident. [message] writes on the
 * display tick (stem_ui_read is [message]), the lamp reads. */
enum { PAD_WAIT_NONE, PAD_WAIT_LOADING, PAD_WAIT_FAILED };
static int g_pad_wait;

#define PAD_LOAD_BLINK_MS 250

/* The part a pad mutes, or -1 when it is not a mute pad right now. [any] */
static int stems_pad_part(int pad)
{
    if (!g_stem_mute_pads || !g_stems_on)
        return -1;
    if (pad < PAD_F || pad >= PAD_F + N_STEMS)
        return -1;
    return pad - PAD_F;
}

/* Taken even when there is nothing to mute: the pad is dark then, and a dark
 * pad must not jump to a hot cue or start a loop. */
static int stems_pad_claim(const struct cue_event *ev)
{
    return stems_pad_part(ev->pad) >= 0;
}

static void stems_pad(const struct cue_event *ev, enum cue_phase phase)
{
    int i;

    if (phase == CUE_PAD_UP) {
        /* Only for a pad this handler took. */
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
    if (!stems_ready() || stems_g_bypass_on) {
        MDBG("stems: pad %c pressed with nothing to mute -> ignored\n", 'A' + ev->pad);
        return;
    }
    if (g_stem_mute_toggle)
        __atomic_add_fetch(&g_pad_taps[i], 1, __ATOMIC_ACQ_REL);
    else
        __atomic_store_n(&g_pad_down[i], 1, __ATOMIC_RELEASE);
}

/* After the X-PAD (3), whose panel owns the pads while open, and ahead of the
 * groove circuit (5). */
CUE_HANDLER(k_cue_stem_mute,
            .name = "stem_mute", .prio = 4,
            .pad = stems_pad, .pad_claim = stems_pad_claim);

/* Loading: a job is on its way. Failed: the job failed, or there is no usable
 * server and nothing on the stick (the condition the STEMS warn badge shows). */
static int stems_pad_wait(void)
{
    struct stem_ui_state st;

    if (stems_ready())
        return PAD_WAIT_NONE;
    if (!stems_available())
        return PAD_WAIT_FAILED;
    if (!stem_ui_read(&st))
        return PAD_WAIT_NONE;
    if (st.stage == STEM_STAGE_FAILED)
        return PAD_WAIT_FAILED;
    if (st.stage != STEM_STAGE_IDLE && st.stage != STEM_STAGE_DONE)
        return PAD_WAIT_LOADING;
    return PAD_WAIT_NONE;
}

int stems_loading(void)
{
    return __atomic_load_n(&g_pad_wait, __ATOMIC_RELAXED) == PAD_WAIT_LOADING;
}

void stems_pad_poll(void)
{
    static unsigned seen[N_STEMS];
    int i;

    __atomic_store_n(&g_pad_wait, stems_pad_wait(), __ATOMIC_RELAXED);

    for (i = 0; i < N_STEMS; i++) {
        unsigned taps = __atomic_load_n(&g_pad_taps[i], __ATOMIC_ACQUIRE);
        int down;

        if (taps != seen[i]) {
            if ((taps - seen[i]) & 1u)
                stems_mute_flip(i);
            seen[i] = taps;
        }
        down = __atomic_load_n(&g_pad_down[i], __ATOMIC_ACQUIRE);
        if (down != ((stems_g_mute_by[i] & STEMS_MUTE_PAD) != 0))
            stems_mute_hold(i, STEMS_MUTE_PAD, down);
    }
}

/* Dim white / dark blink: stems are loading.
 * Orange: the stems failed, or there is no server and nothing on the stick.
 * Dark: nothing to mute (no track, or BYPASS on).
 * Part colour: the part plays.
 * Part colour / white blink: muted, on the caption's blink phase. */
int stems_pad_lamp(int pad, struct lamp *out)
{
    int i = stems_pad_part(pad), by;

    if (i < 0)
        return 0;
    if (!stems_ready()) {
        switch (__atomic_load_n(&g_pad_wait, __ATOMIC_RELAXED)) {
        case PAD_WAIT_LOADING:
            if ((lamp_now_ms() / PAD_LOAD_BLINK_MS) & 1)
                lamp_dark(out);
            else
                lamp_set(out, 255, 255, 255, LAMP_DIM);
            break;
        case PAD_WAIT_FAILED:
            lamp_set(out, 255, 128, 0, LAMP_LIT);     /* wheel orange */
            break;
        default:
            lamp_dark(out);
        }
        return 1;
    }
    if (stems_g_bypass_on) {
        lamp_dark(out);
        return 1;
    }
    by = __atomic_load_n(&stems_g_mute_by[i], __ATOMIC_RELAXED);
    if (!by || __atomic_load_n(&stems_g_mute_lit, __ATOMIC_RELAXED))
        lamp_set(out, k_stem_pad_hue[i][0], k_stem_pad_hue[i][1],
                 k_stem_pad_hue[i][2], LAMP_LIT);
    else
        lamp_set(out, 255, 255, 255, LAMP_LIT);
    return 1;
}
