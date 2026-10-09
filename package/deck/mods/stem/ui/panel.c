// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * ui/panel.c - the row and the waveform band it borrows
 *
 * Part of the STEMS play-screen UI. The shared contract, and the reasoning
 * behind the design, is in ui.h.
 */
#include "stem/ui/ui.h"

/* ---- the band ------------------------------------------------------------
 *
 * kit/band.c owns the strip and arbitrates who has it; this is the STEMS side.
 * The mode number must be unique across clients: the kit's watch reads it back
 * to tell "still ours" from "taken". */
#define STEMS_BAND_MODE 5

static void stems_band_closed(void)
{
    if (!stems_g_row_open) return;
    MDBG("stems: the band went elsewhere -> closing our row\n");
    stems_g_row_open = 0;
    stems_sync();
}

static void stems_reslot(int32_t x)
{
    if (stems_g_btn_stems)
        ((void (*)(void *, int, int, int, int))FN_SET_BOUNDS)
            ((void *)stems_g_btn_stems, x, KIT_BAND_SLOT_Y,
             KIT_BAND_SLOT_W, KIT_BAND_SLOT_H);
}

KIT_BAND(k_band_stems,
         .name = "stems", .mode = STEMS_BAND_MODE, .closed = stems_band_closed,
         .shown = &g_stems_on, .reslot = stems_reslot, .order = 0);

int32_t stems_slot_x(void)
{
    return kit_band_slot_x(&k_band_stems);
}


/* ================================================================== */
/* Interaction                                                        */
/* ================================================================== */

/* ENABLE STEMS in MOD SETTINGS is the master gate and can change while the play
 * screen is up, so visibility is re-asserted from the paint hook. Paint runs per
 * frame; the memo keeps the common case to one comparison. */
void stems_sync(void)
{
    static int last = -1;
    int state = (g_stems_on ? 1 : 0) | (stems_g_row_open ? 2 : 0);
    int on, lit, gate_moved;

    if (state == last) return;
    gate_moved = (last >= 0) && ((last & 1) != (state & 1));
    last = state;
    on  = (state & 1) != 0;
    lit = on && (state & 2) != 0;

    /* The gate changes which slots are in use, so the other client's button and
     * the track title move with it. */
    if (gate_moved) kit_band_slots_changed();
    stems_set_visible(stems_g_btn_stems, on);
    stems_set_visible(stems_g_row, lit);
    /* Closing the row ends any gesture, cooldown included. The STEMS button is
     * exempt: it is not in the row and its finger is still down; clearing its grab
     * would drop its touch highlight mid-press. Its own mouseUp releases it. */
    if (!lit && stems_g_grab != stems_g_btn_stems) { stems_g_grab = 0; stems_g_grab_last = 0; }
    stems_btn_state(lit ? BTN_ON : BTN_OFF);

    /* The master gate is the only route that shows or hides the row without
     * touching stems_g_row_open; every other route claims or releases the band
     * itself. Hiding the strip alone would leave the waveform compacted around
     * nothing.
     *
     * This is a backstop. Switching STEMS off drops the resident set, and the warn
     * poll (hooks.c) then closes the row and returns the band after
     * WARN_SETTLE_TICKS. This covers that interval and the case with no set
     * resident.
     *
     * Keyed on the gate changing, not on `lit`, so an ordinary open/close or a
     * stock quick-menu takeover does not release the band. */
    if (gate_moved && stems_g_row_open) {
        if (on) kit_band_take(&k_band_stems);
        else    kit_band_give(&k_band_stems);
    }
}

/* One-shot component-tree dump at debug log level, for comparing the stock
 * quick-menu panel's layout with ours. Bounded on depth and child count so a bad
 * pointer cannot cause an unbounded walk. */
#define STEMS_TREE_DEPTH   4
#define STEMS_TREE_MAXKIDS 64

void stems_dump_tree(uintptr_t comp, int depth)
{
    uintptr_t vt = 0, kids = 0, child;
    int32_t n = 0, b[4], i;
    uint8_t flags = 0;

    if (depth > STEMS_TREE_DEPTH || !comp) return;
    if (mod_safe_read(comp, &vt, sizeof(vt)) != 0) return;
    if (stems_bounds(comp, b) != 0) return;
    if (mod_safe_read(comp + COMP_NCHILD_OFF, &n, sizeof(n)) != 0) return;
    mod_safe_read(comp + COMP_FLAGS_OFF, &flags, 1);
    MDBG("tree %*s%#lx vt=%#lx {%d,%d,%d,%d} flags=%02x kids=%d\n",
         depth * 2, "", (unsigned long)comp, (unsigned long)vt,
         b[0], b[1], b[2], b[3], flags, n);
    if (n <= 0 || n > STEMS_TREE_MAXKIDS) return;
    if (mod_safe_read(comp + COMP_CHILDREN_OFF, &kids, sizeof(kids)) != 0 || !kids) return;
    for (i = 0; i < n; i++) {
        child = 0;
        if (mod_safe_read(kids + (uintptr_t)i * sizeof(uintptr_t), &child, sizeof(child)) == 0)
            stems_dump_tree(child, depth + 1);
    }
}

/* The row-open flag for the non-UI parts of the feature. The groove circuit and its
 * lamps are gated on it, so it is read from [deck] and from the display tick. A
 * plain aligned int, written only by the toggle below and by stems_band_closed. */
int stems_row_open(void)
{
    return stems_g_row_open;
}

void stems_toggle_row(void)
{
    if (!stems_g_row_open) {
        /* Take the band first and open only if it is granted (see kit_band_take).
         * With no track loaded the app refuses the mode. */
        if (kit_band_take(&k_band_stems) < 0) {
            MDBG("stems: no band to open into (no track loaded?)\n");
            return;
        }
        stems_g_row_open = 1;
    } else {
        stems_g_row_open = 0;
        kit_band_give(&k_band_stems);
    }
    stems_sync();
    /* Show any in-flight processing as soon as the row appears. */
    if (stems_g_row_open) {
        /* The poll stopped while the row was shut, so the bar's cache is stale. */
        stems_progress_forget();
        stems_progress_poll();
    }
    MDBG("stems: control row %s\n", stems_g_row_open ? "open" : "closed");
    if (MLOG_AT(MOD_LOG_DEBUG) && stems_g_row_open && kit_band_view()) {
        MDBG("tree --- OURS open ---\n");
        stems_dump_tree(kit_band_view(), 0);
    }
}
