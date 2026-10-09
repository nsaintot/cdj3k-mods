// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * ui/build.c - constructing the button and the control row
 *
 * Part of the STEMS play-screen UI. The shared contract, and the reasoning
 * behind the design, is in ui.h.
 */
#include "stem/ui/ui.h"

/* The control row takes the stock panel rect inside the waveform view, with its top
 * raised by ROW_RISE; the bottom stays, so the extra height goes to the wedges.
 *
 * The lift is applied only here: kit_band_rect() keeps the stock rect because the kit
 * identifies the app's own panels by comparing bounds against it. Our strip is
 * excluded from those scans by identity (kit_band_own). */
static uintptr_t stems_build_row(void)
{
    const int32_t *panel = kit_band_rect();
    /* The same boundaries that weight the fill. */
    static const int k_prog_bound[N_PROG_BOUND] = PROG_BOUND_LIST;
    int usable, wedge_w, x, i;
    int row_y   = panel[1] - ROW_RISE;
    int row_h   = panel[3] + ROW_RISE;
    int wedge_h = row_h - WEDGE_Y - ROW_PAD / 2;
    int btn_w   = wedge_h;                    /* BYPASS is square; see ui.h */
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
    /* BYPASS: a square icon button (see ui.h). */
    stems_g_btn_bypass = stems_label(row, "", FONT_CAPTION, mod_ui()->surface, mod_ui()->text, 1,
                                 ROW_LEFT, WEDGE_Y, btn_w, wedge_h);
    if (stems_g_btn_bypass && stems_icon_vt_ready())
        *(uintptr_t *)stems_g_btn_bypass = stems_g_icon_vptr;

    /* The wavewait mark goes in the free caption cell above BYPASS. */
    stems_wavewait_build(row, ROW_LEFT, ROW_PAD / 2, btn_w, CAPTION_H);

    /* One gap between neighbours, so N wedges leave N-1 gaps inside the free span. */
    x        = ROW_LEFT + btn_w + ROW_GAP;
    usable   = panel[2] - x - ROW_RIGHT;
    wedge_w  = (usable - (N_STEMS - 1) * ROW_GAP) / N_STEMS;
    if (wedge_w < 40) {
        MDBG("stems: only %dpx left for the wedges -> row too narrow, skipping them\n",
             usable);
        return band;
    }

    for (i = 0; i < N_STEMS; i++, x += wedge_w + ROW_GAP) {
        /* Narrower than the wedge and centred over it, so it reads as a button. */
        stems_g_caption[i] = stems_label(row, k_stem_name[i], FONT_CAPTION, mod_ui()->surface,
                                   mod_ui()->text_dim, 1, x + (wedge_w - CAPTION_W) / 2,
                                   ROW_PAD / 2, CAPTION_W, CAPTION_H);
        stems_caption_sync(i);
        if (!stems_wedge(row, x, WEDGE_Y, wedge_w, wedge_h, i))
            MDBG("stems: wedge %s failed to build\n", k_stem_name[i]);
    }
    MDBG("stems: row {%d,%d,%d,%d} (+%d over the panel rect) + BYPASS %dpx + "
         "%d wedges of %dx%d\n",
         panel[0], row_y, panel[2], row_h, ROW_RISE,
         btn_w, N_STEMS, wedge_w, wedge_h);
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
