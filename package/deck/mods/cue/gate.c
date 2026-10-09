// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * cue/gate.c - GATE CUE: momentary / play-while-press hot cues.
 *
 * With the deck paused:
 *   - press a hot-cue pad   -> jump to that cue and PLAY while held (stock)
 *   - release the pad       -> return to the cue point and pause (back-cue)
 *   - press PLAY while held -> LATCH; releasing then no longer back-cues
 *
 * With the deck playing, the pads behave as stock: a press jumps and keeps
 * playing, the release does nothing, and PLAY is PLAY. The mode is decided once
 * per session, on its first pad, from the deck's play state at that moment.
 *
 * This file only decides whether to back-cue. It names the op and the deck's
 * own release task performs it; issuing the op from here would run it twice
 * and skip the deck's post-release teardown.
 *
 * Known issue: a short press can still lose its back-cue, and this layer cannot
 * fix it. At release the deck's state is the same for a 30 ms hold as for a
 * 400 ms one (priority matched, one pad on the handler's stack), and the release
 * asks for the same return. The loss happens deeper, while the press's
 * jump-and-play is still settling. A 400 ms hold returns reliably; at 200 ms
 * and below the back-cue can be lost.
 */
#include "cue/cue.h"
#include "kit/menu.h"
#include "kit/mod.h"

int g_gate_on;                  /* persisted; see core/common.c */

/* One momentary session lasts from the first pad down to the last pad up. Armed
 * when it began with the deck paused; a second pad joins the running session. */
static int gate_g_armed;

/* A PLAY press during the hold promoted it to continuous playback, so the
 * release must not back-cue. */
static int gate_g_latched;

/* Bit per pad index: the press set a new cue instead of recalling one. A press
 * on an empty pad sets a hot cue at the play head without jumping, so it must
 * not back-cue. Per pad because several can be down at once. */
static unsigned gate_g_marked;

/* Bit per pad index: the pad had a cue when it went down. See gate_release_op
 * for why the deck's status is not enough. */
static unsigned gate_g_had;

/* Whether this pad's hot cue holds a position. Kind is pad index + 1. */
static int gate_pad_has_cue(const struct cue_event *ev)
{
    int64_t at = 0;

    return cue_slot_pos(ev, ev->pad + 1, &at);
}

static void gate_pad(const struct cue_event *ev, enum cue_phase phase)
{
    switch (phase) {
    case CUE_PAD_DOWN:
        /* Cleared on every press, whatever the row's setting, so a pad never
         * carries over the previous press's state. */
        gate_g_marked &= ~(1u << ev->pad);
        if (gate_pad_has_cue(ev))
            gate_g_had |= 1u << ev->pad;
        else
            gate_g_had &= ~(1u << ev->pad);
        /* The first pad of a session decides it; later pads join it. */
        if (cue_pads_held() == 1) {
            int armed = g_gate_on && cue_deck_paused(ev);

            __atomic_store_n(&gate_g_latched, 0, __ATOMIC_RELAXED);
            /* RELEASE: PLAY's task may already be running with held == 1. */
            __atomic_store_n(&gate_g_armed, armed, __ATOMIC_RELEASE);
            if (g_gate_on)
                MDBG("gate: pad %d down -> %s\n", ev->pad,
                     armed ? "gated" : "the deck's own hot cue");
        }
        break;

    case CUE_PAD_PRESSED:
        /* `assigned` is only known in this phase. */
        if (!ev->assigned)
            gate_g_marked |= 1u << ev->pad;
        break;

    case CUE_PAD_UP:
        if (cue_pads_held() == 0) {
            __atomic_store_n(&gate_g_latched, 0, __ATOMIC_RELAXED);
            __atomic_store_n(&gate_g_armed, 0, __ATOMIC_RELAXED);
        }
        break;
    }
}

/* Whether the deck's own release should come back to the cue. */
static int gate_release_op(const struct cue_event *ev)
{
    if (!g_gate_on || !__atomic_load_n(&gate_g_armed, __ATOMIC_ACQUIRE))
        return 0;
    if (__atomic_load_n(&gate_g_latched, __ATOMIC_ACQUIRE)) {
        MDBG("gate: latched -> keep playing\n");
        return 0;
    }
    if (gate_g_marked & (1u << ev->pad)) {
        MDBG("gate: pad %d was marked, not recalled -> no back-cue\n", ev->pad);
        return 0;
    }
    /* The cue must still exist. The press status says "pad was already set"
     * both when the press recalled the cue and when a held CALL/DELETE erased
     * it. A back-cue to an erased cue makes the deck set a new one at the play
     * head, so deleting a hot cue would move it instead.
     *
     * Checked at release, not press: the erase runs on its own task and has
     * landed by release time, while at press the cue still reads as set. */
    if ((gate_g_had & (1u << ev->pad)) && !gate_pad_has_cue(ev)) {
        MDBG("gate: pad %d lost its cue -> no back-cue\n", ev->pad);
        return 0;
    }
    MDBG("gate: pad %d momentary (had cue %d) -> ask the deck's release for a"
         " back-cue\n", ev->pad, (gate_g_had >> ev->pad) & 1);
    return CUE_OP_BACKCUE;
}

/* Claim the PLAY press: it means "keep going", not play/pause. */
static int gate_play_while_held(void)
{
    if (!g_gate_on || !__atomic_load_n(&gate_g_armed, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&gate_g_latched, __ATOMIC_ACQUIRE))
        return 0;
    __atomic_store_n(&gate_g_latched, 1, __ATOMIC_RELAXED);
    MDBG("gate: PLAY during hold -> latched\n");
    return 1;
}

CUE_HANDLER(k_cue_gate,
            .name = "gate", .prio = 10,
            .pad = gate_pad, .play_while_held = gate_play_while_held,
            .release_op = gate_release_op);

/* The hooks read g_gate_on on every event, so the row toggles the behaviour
 * live. `changed` keeps the play-screen shortcut in sync. */
static const struct kit_row k_rows[] = {
    KIT_ROW_BOOL("GATE CUE", &g_gate_on, .idx = KIT_IDX_GATE,
                 .changed = cue_shortcut_refresh),
};

static int gate_install(void)
{
    if (!cue_pad_ready()) {
        MDBG("gate: no cue interception -> no row\n");
        return -1;
    }
    kit_menu_add(k_rows, (int)(sizeof(k_rows) / sizeof(k_rows[0])));
    return 0;
}

KIT_MOD(k_mod_cue_gate,
        .name = "cue_gate", .prio = 10, .install = gate_install,
        .what = "momentary gate cue");
