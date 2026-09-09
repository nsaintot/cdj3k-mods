// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * cue/gate.c - GATE CUE: momentary / play-while-press hot cues.
 *
 * With the deck PAUSED:
 *   - press a hot-cue pad   -> jump to that cue and PLAY while held (stock)
 *   - release the pad       -> return to the cue point and PAUSE (back-cue)
 *   - press PLAY while held -> LATCH; releasing then no longer back-cues
 *
 * With the deck PLAYING the pads are the deck's own: a press jumps and keeps
 * playing, the release does nothing, and PLAY is PLAY. Decided once per
 * session, on its first pad, from the deck's play state at that moment.
 *
 * Nothing here performs the back-cue: it asks the deck's own release for one by
 * naming an op, and the deck's release task does it. Everything in this file is
 * the decision of WHETHER, which is all a behaviour is.
 *
 * WHY IT IS NOT DONE HERE. Issuing the operation alongside the deck's release
 * meant two invocations of it -- ours and, once the last pad pops, the deck's
 * own -- and the out-of-band one also skipped the teardown the deck does after.
 * Naming the op puts the whole gesture in one task.
 *
 * A SHORT PRESS STILL LOSES ITS BACK-CUE, and it is not decided here or anywhere
 * else in this layer. At release time the deck's state is identical either way
 * -- priority matched, one pad on the handler's stack -- so its release reaches
 * the cue operation and is told to return to the same hot cue for a 30 ms hold
 * as for a 400 ms one. The difference appears below that, where the press's own
 * jump-and-play is still settling. Measured: 400 ms returns three times out of
 * three, 200 ms none, shorter is a coin toss.
 */
#include "cue/cue.h"
#include "kit/menu.h"
#include "kit/mod.h"

int g_gate_on;                  /* persisted; see mods/common.c */

/* One momentary session: from the first pad down to the last pad up. Armed
 * when it began with the deck paused; a second pad joins whichever session is
 * running rather than deciding again. */
static int gate_g_armed;

/* A PLAY press during the hold promoted it to continuous playback, so the
 * release must not back-cue. */
static int gate_g_latched;

/* Pads whose press did anything OTHER than go to a cue that was already there,
 * by pad index.
 *
 * A press on an empty pad puts a hot cue at the play head, and there is nothing
 * to come back from: the deck did not jump, so returning is a move the DJ never
 * asked for. Marking a cue and recalling one are different instructions and only
 * the second one gates. Kept per pad because several can be down at once. */
static unsigned gate_g_marked;

/* Pads that HAD a cue when they went down, so the press's effect on it can be
 * seen. See gate_pad for why the deck's own answer is not enough. */
static unsigned gate_g_had;

/* Whether this pad's hot cue currently holds a position. Hot cues are kinds
 * 1..8 against a pad index of 0..7. */
static int gate_pad_has_cue(const struct cue_event *ev)
{
    int64_t at = 0;

    return cue_slot_pos(ev, ev->pad + 1, &at);
}

static void gate_pad(const struct cue_event *ev, enum cue_phase phase)
{
    switch (phase) {
    case CUE_PAD_DOWN:
        /* Cleared here rather than on the release, and regardless of the row's
         * setting, so a pad never carries the last press's answer. */
        gate_g_marked &= ~(1u << ev->pad);
        if (gate_pad_has_cue(ev))
            gate_g_had |= 1u << ev->pad;
        else
            gate_g_had &= ~(1u << ev->pad);
        /* First pad of a session decides it; a second pad joins the one
         * already running rather than starting a new one. */
        if (cue_pads_held() == 1) {
            int playing = cue_deck_playing(ev);

            __atomic_store_n(&gate_g_latched, 0, __ATOMIC_RELAXED);
            __atomic_store_n(&gate_g_armed, g_gate_on && !playing,
                             __ATOMIC_RELAXED);
            if (g_gate_on)
                MDBG("gate: pad %d down with the deck %s -> %s\n", ev->pad,
                     playing ? "playing" : "paused",
                     playing ? "the deck's own hot cue" : "gated");
        }
        break;

    case CUE_PAD_PRESSED:
        /* The deck's press run reports which it was, and this is the only phase
         * where it is known. */
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
    /* AND THE CUE HAS TO STILL BE THERE.
     *
     * The deck's press status answers "was this pad already set", which it is
     * both when the press RECALLED the cue and when a held CALL/DELETE made it
     * ERASE the cue -- the same answer for opposite events. Asking for a
     * back-cue to a cue that no longer exists made the deck plant a fresh one at
     * the play head to have something to return to, so deleting a hot cue moved
     * it to the needle instead of removing it.
     *
     * ASKED HERE rather than in the press, where it reads as still set: the
     * erase lands on its own task, and the release is late enough to have seen
     * it. A cue that did not survive the press is not one to go back to. */
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

/* The hooks are always in and read g_gate_on on every event, so this toggles the
 * behaviour live. `changed` keeps the play-screen shortcut in step. */
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
