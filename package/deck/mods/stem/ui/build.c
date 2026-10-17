// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * ui/build.c - constructing the button and the control row
 *
 * Part of the STEMS play-screen UI. The shared contract, and the reasoning
 * behind the design, is in ui.h.
 */
#include "stem/ui/ui.h"

/* Where each lane starts and how wide it is, for stems_lanes_place. */
static int g_lane_x[N_STEMS], g_lane_w, g_lane_y, g_row_w;
static int g_lanes_placed = -1;    /* the STEM MUTE PADS value the lanes are laid out for */

/* Lay the controls out, drawn where the design puts them, with touch areas that cover
 * the whole row: each blank gap is split at its middle between the ink on either side
 * (a key's plate, a fader's strip and cap), BYPASS reaches the left edge and the last
 * fader the right one. A finger anywhere along the row lands on the nearest control. */
void stems_lanes_place(void)
{
    /* l..r is where the control draws, il..ir its ink. */
    struct { uintptr_t comp; int stem; int l, r, il, ir; } c[1 + 2 * N_STEMS];
    int pads = g_stem_mute_pads ? 1 : 0;
    int key  = MUTE_W + MUTE_GAP;
    int fw   = pads ? LANE_PADS_W : g_lane_w - key;
    int i, n = 0;

    if (pads == g_lanes_placed || !g_lane_w || !stems_g_btn_bypass) return;
    g_lanes_placed = pads;

    /* The drawn rects, left to right. */
    c[n].comp = stems_g_btn_bypass; c[n].stem = -1;
    c[n].l = ROW_LEFT; c[n].r = ROW_LEFT + SW_CELL_W; n++;
    for (i = 0; i < N_STEMS; i++) {
        if (stems_g_caption[i]) {
            stems_set_visible(stems_g_caption[i], !pads);
            if (!pads) {
                c[n].comp = stems_g_caption[i]; c[n].stem = -1;
                c[n].l = g_lane_x[i]; c[n].r = g_lane_x[i] + MUTE_W; n++;
            }
        }
        c[n].comp = stems_g_wedge[i]; c[n].stem = i;
        /* Beside its key the fader ends the lane; with the keys folded away it is
         * LANE_PADS_W and centred, so its 100 % end stays clear of the next lane and of
         * the bezel. */
        c[n].l = g_lane_x[i] + (pads ? (g_lane_w - fw) / 2 : key);
        c[n].r = c[n].l + fw; n++;
    }
    for (i = 0; i < n; i++) {
        c[i].il = c[i].l;
        c[i].ir = c[i].r;
        if (c[i].comp == stems_g_btn_bypass) {      /* the switch's frame */
            c[i].il = c[i].l + (SW_CELL_W - SW_W) / 2;
            c[i].ir = c[i].il + SW_W;
        }
        if (c[i].stem >= 0) {
            int il, ir;

            stems_lane_ink(c[i].r - c[i].l, &il, &ir);
            c[i].il = c[i].l + il;
            c[i].ir = c[i].l + ir;
        }
    }

    for (i = 0; i < n; i++) {
        int hl = i == 0     ? 0       : (c[i - 1].ir + c[i].il + 1) / 2;
        int hr = i == n - 1 ? g_row_w : (c[i].ir + c[i + 1].il + 1) / 2;

        MDBG("stems: touch %d..%d for ink %d..%d\n", hl, hr, c[i].il, c[i].ir);
        if (c[i].stem >= 0) {
            stems_wedge_place(c[i].stem, hl, g_lane_y, hr - hl, LANE_H,
                              c[i].l - hl, c[i].r - c[i].l);
        } else if (c[i].comp) {
            ((bounds_t)FN_SET_BOUNDS)((void *)c[i].comp, hl, g_lane_y, hr - hl, LANE_H);
            stems_hit_set(c[i].comp, c[i].l - hl, c[i].r - c[i].l);
            stems_repaint(c[i].comp);
        }
    }
    MDBG("stems: MUTE keys %s, touch areas cover 0..%d\n",
         pads ? "folded away (the pads mute)" : "shown", g_row_w);
}

/* The control row takes the stock panel rect inside the waveform view, with its top
 * raised by ROW_RISE; the bottom stays, so the controls sit on the stock rect.
 *
 * The lift is applied only here: kit_band_rect() keeps the stock rect because the kit
 * identifies the app's own panels by comparing bounds against it. Our strip is
 * excluded from those scans by identity (kit_band_own). */
static uintptr_t stems_build_row(void)
{
    const int32_t *panel = kit_band_rect();
    /* The same boundaries that weight the fill. */
    static const int k_prog_bound[N_PROG_BOUND] = PROG_BOUND_LIST;
    int usable, x, i;
    int row_y   = panel[1] - ROW_RISE;
    int row_h   = panel[3] + ROW_RISE;
    int wedge_h = row_h - WEDGE_Y - ROW_PAD / 2;
    uintptr_t band, row;

    /* `band` is the strip covering the panel rect and is returned, since it is what
     * gets hidden. `row` is the parent for the contents (stems_g_controls). Keep them
     * separate, or the caller ends up hiding the contents instead of the strip. */
    band = stems_label(kit_band_view(), "", FONT_CAPTION, COL_ROW_BG, mod_ui()->text, 0,
                       panel[0], row_y, panel[2], row_h);
    if (!band) return 0;

    /* Two transparent full-size containers: exactly one is ever visible. */
    stems_g_controls = stems_label(band, "", FONT_CAPTION, 0x00000000u, mod_ui()->text, 0,
                             0, 0, panel[2], row_h);
    stems_g_progress = stems_label(band, "", FONT_CAPTION, 0x00000000u, mod_ui()->text, 0,
                             0, 0, panel[2], row_h);
    if (!stems_g_controls || !stems_g_progress) return band;

    /* The processing state reuses the control row's geometry: its caption sits where
     * the stem captions sit and the bar within the wedge band, so swapping states
     * moves nothing vertically.
     *
     * Caption, then track, then fill, so the fill draws over the track. */
    stems_g_prog_x = ROW_LEFT;
    stems_g_prog_w = panel[2] - ROW_LEFT - ROW_RIGHT;
    /* Centred in the wedge band, which follows the panel rect and ROW_RISE. */
    stems_g_prog_y = WEDGE_Y + (wedge_h - PROG_BAR_H) / 2;
    /* Stock roles, not mod_ui()'s: juce paints these Labels and every fill already
     * goes through the theme hook (see mod_ui_stock). Resolved values would be
     * resolved twice, e.g. #ffffff on #ffffff under WHITE. */
    stems_g_prog_text = stems_label(stems_g_progress, "", FONT_CAPTION, 0x00000000u,
                              mod_ui_stock()->text, 0,
                              stems_g_prog_x, ROW_PAD / 2, stems_g_prog_w, CAPTION_H);
    stems_g_prog_track = stems_label(stems_g_progress, "", FONT_CAPTION,
                               mod_ui_stock()->track, mod_ui_stock()->track, 0,
                               stems_g_prog_x, stems_g_prog_y, stems_g_prog_w, PROG_BAR_H);
    stems_g_prog_fill = stems_label(stems_g_progress, "", FONT_CAPTION,
                              mod_ui_stock()->accent, mod_ui_stock()->accent, 0,
                              stems_g_prog_x, stems_g_prog_y, 0, PROG_BAR_H);
    /* Built after the fill so they draw over it. */
    for (i = 0; i < N_PROG_BOUND; i++) {
        stems_g_prog_mark[i] = stems_label(stems_g_progress, "", FONT_CAPTION,
                                     mod_ui_stock()->mark, mod_ui_stock()->mark, 0,
                                     stems_g_prog_x + stems_prog_px(k_prog_bound[i]),
                                     stems_g_prog_y, PROG_MARK_W, PROG_BAR_H);
        stems_set_visible(stems_g_prog_mark[i], 0);
    }
    stems_set_visible(stems_g_progress, 0);

    row = stems_g_controls;   /* everything below hangs off the controls container */
    /* The controls sit on the band's own rect, below the row's raised top. */
    g_lane_y = row_h - panel[3];
    g_row_w  = panel[2];
    /* The STEMS switch (BYPASS's button). */
    stems_g_btn_bypass = stems_label(row, "", FONT_CAPTION, 0x00000000u, mod_ui()->text, 1,
                                 ROW_LEFT, g_lane_y, SW_CELL_W, LANE_H);
    if (stems_g_btn_bypass && stems_icon_vt_ready())
        *(uintptr_t *)stems_g_btn_bypass = stems_g_icon_vptr;

    /* One gap between neighbours, so N lanes leave N-1 gaps inside the free span. */
    x        = ROW_LEFT + SW_CELL_W - SW_OVERLAP + ROW_GAP;
    usable   = panel[2] - x - ROW_RIGHT;
    g_lane_w = (usable - (N_STEMS - 1) * ROW_GAP) / N_STEMS;
    if (g_lane_w < MUTE_W + MUTE_GAP + 4 * LANE_CAP_W) {
        MDBG("stems: only %dpx left for the lanes -> row too narrow, skipping them\n",
             usable);
        return band;
    }

    for (i = 0; i < N_STEMS; i++, x += g_lane_w + ROW_GAP) {
        g_lane_x[i] = x;
        /* The MUTE key; placed and shown by stems_lanes_place. */
        stems_g_caption[i] = stems_label(row, "", FONT_CAPTION, 0x00000000u,
                                         mod_ui()->text, 1, x, g_lane_y, MUTE_W, LANE_H);
        if (!stems_wedge(row, x, g_lane_y, g_lane_w, LANE_H, i))
            MDBG("stems: lane %s failed to build\n", k_stem_name[i]);
    }
    g_lanes_placed = -1;
    stems_lanes_place();
    MDBG("stems: row {%d,%d,%d,%d} (+%d over the panel rect) + STEMS switch %dpx + "
         "%d lanes of %dx%d\n",
         panel[0], row_y, panel[2], row_h, ROW_RISE,
         SW_CELL_W, N_STEMS, g_lane_w, LANE_H);
    return band;
}

/* `anchor` is a TouchAria, whose parent is the title bar's juce::Component base.
 * kit/band.c walks up from it to the waveform view and the stock panel rect;
 * nothing is built until that succeeds. */
void stems_build(uintptr_t anchor)
{
    uintptr_t bar;

    if (kit_band_attach(anchor) != 0) return;
    bar = kit_band_bar();
    if (!stems_label_vt_ready()) return;

    /* Each half is built at most once: paint fires per frame, so an unguarded retry
     * after a partial build would leak a widget per frame. */
    stems_g_building = 1;
    if (!stems_g_btn_stems) {
        stems_g_btn_stems = stems_label(bar, "STEMS", FONT_QUICKMENU, 0x00000000u, mod_ui()->text_deck, 1,
                                  QM_SLOT_X, QM_SLOT_Y, QM_SLOT_W, QM_SLOT_H);
        /* The badge is a child of the button, so it moves and hides with it. Not
         * clickable: it keeps stock Label's mouseDown (the empty stub) and the press
         * falls through to STEMS. */
        stems_g_warn = stems_label(stems_g_btn_stems, "!", FONT_CAPTION, 0x00000000u, mod_ui()->warn, 0,
                             QM_SLOT_W - 22, 4, 18, 20);
        stems_set_visible(stems_g_warn, 0);
        /* The edit mark: the opposite corner, same kind of non-clickable child.
         * The warn badge says the deck cannot separate this track; this one says
         * the stems are changing it.
         *
         * U+2022 BULLET as UTF-8 bytes, built via juce_string_utf8; the plain
         * String constructor is ASCII-per-byte and would produce three Latin-1
         * characters.
         *
         * Chosen for the font the deck actually resolves: U+00B7 MIDDLE DOT is a
         * circle in the UCGothic faces but a square in TsukuGo, FZLTZH and GB18030,
         * and this deck draws the square. U+2022 is round in every face that has it. */
        stems_g_edit = stems_label(stems_g_btn_stems, "\xe2\x80\xa2", FONT_BADGE, 0x00000000u, mod_ui()->text, 0,
                             BADGE_AT_X - BADGE_MARK_X, BADGE_AT_Y - BADGE_MARK_Y,
                             BADGE_BOX, BADGE_BOX);
        stems_set_visible(stems_g_edit, 0);
        kit_band_own(stems_g_btn_stems);
    }
    if (!stems_g_row) {
        stems_g_row = stems_build_row();
        kit_band_own(stems_g_row);
    }
    stems_g_building = 0;

    /* Sync before the completeness check: addAndMakeVisible shows each piece as it is
     * attached, so a half-built row would otherwise stay on screen with no way to close
     * it. */
    stems_sync();
    if (!stems_g_btn_stems || !stems_g_row) {
        MDBG("stems: build incomplete (button=%#lx row=%#lx)\n",
             (unsigned long)stems_g_btn_stems, (unsigned long)stems_g_row);
        return;
    }
    if (!kit_band_stock_up())
        kit_band_snapshot();
    stems_g_built = 1;
    MDBG("stems: attached to bar %#lx\n", (unsigned long)bar);
}
