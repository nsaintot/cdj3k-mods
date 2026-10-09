// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * mods/grid/panel_layout.c - where the buttons sit, and the readout that follows them.
 */
#include "grid/panel_internal.h"
#include "core/ep122_syms.h"
#include "juce/draw.h"
#include "juce/juce.h"
#include "kit/mod.h"
#include "stem/stem.h"

void gp_readout_sync(void)
{
    double bpm = stem_grid_bpm();
    char buf[32];

    if (!gp_g_readout || bpm == gp_g_shown_bpm)
        return;
    gp_g_shown_bpm = bpm;
    if (bpm > 0.0)
        snprintf(buf, sizeof(buf), "%.1f", bpm);
    else
        snprintf(buf, sizeof(buf), "--.-");
    juce_label_text(gp_g_readout, buf);
}

static void gp_place(uintptr_t comp, int x, int y, int w, int h)
{
    int32_t b[4];

    if (!comp)
        return;
    if (juce_comp_bounds(comp, b) == 0 &&
        b[0] == x && b[1] == y && b[2] == w && b[3] == h)
        return;
    ((void (*)(void *, int, int, int, int))FN_SET_BOUNDS)((void *)comp, x, y, w, h);
}

/* How far a button's artwork stops inside its bounds. The deck's are PNGs that
 * fill theirs; everything we draw is inset. */
static int gp_inset(int i, int ours)
{
    return (ours || i == GP_RESET) ? GP_PLATE_INSET : 0;
}

/* Bounds gap before button i, from the seen gap the design asks for. Because
 * the deck's artwork fills its bounds and ours is inset, one bounds gap renders
 * differently depending on its neighbours (at a bounds gap of 12: 12px
 * between two of the deck's, 13px against a drawn RESET, 14px between two of
 * ours). So the GP_V* constants are seen distances and the insets in the way are
 * subtracted here. Zero at 3 for the deck's flush 1/2 pair; GP_VRESET at 4 for
 * our RESET, which stands off the plate. */
static int gp_gap_before(int i, int ours)
{
    if (i == 0)
        return 0;
    if (i == 3 && !ours)
        return 0;                              /* the deck's 1/2 pair, flush */
    if (i == 4 && ours)                        /* off the plate, and clear of it */
        return GP_VRESET - gp_inset(i - 1, ours) - gp_inset(i, ours);
    return GP_VGAP - gp_inset(i - 1, ours) - gp_inset(i, ours);
}

int gp_btn_x(int first, int i, int ours)
{
    int x = first, k;

    for (k = 1; k <= i; k++)
        x += GP_BTN_W + gp_gap_before(k, ours);
    return x;
}

/* Re-asserted whenever the panel is shown, not on every paint: a panel being
 * re-laid out does so in resized(), which runs on the way back up, and a
 * setBounds inside a paint schedules another paint.
 *
 * The panel's own height is part of the layout: the 84px strip is grown by
 * GP_PAD so the plate has a margin around deck-height buttons (the strip sits
 * 8px above the bottom of the waveform view). Guarded against re-entry because
 * setBounds runs resized(). */
void gp_layout(void)
{
    static int busy;
    int i;

    if (busy)
        return;
    busy = 1;
    gp_place(gp_g_panel, gp_g_panel_b[0], gp_g_panel_b[1],
             gp_g_panel_b[2], gp_g_panel_h);
    for (i = 0; i < GP_N; i++) {
        int w = (i == GP_RESET) ? GP_RESET_W : GP_BTN_W;
        int h = (i == GP_RESET) ? GP_RESET_H : GP_BTN_H;
        int y = gp_g_btn_y + (GP_BTN_H - h) / 2;

        gp_place(gp_g_stock[i], gp_btn_x(GP_GROUP_L, i, 0), y, w, h);
        gp_place(gp_g_btn[i],   gp_btn_x(gp_g_first, i, 1), y, w, h);
    }
    busy = 0;
}
