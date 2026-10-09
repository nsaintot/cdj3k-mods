// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * browse/edit.c - the EDIT toggle in the browse header.
 *
 * Enables the reorder gesture. See browse.h for why it needs a mode and where
 * it attaches.
 *
 * ---- the plate ------------------------------------------------------------
 *
 * Styled as one of the header's buttons: the deck's PREVIEW and font-size
 * plates are 114x90 four-pixel checkerboards on a 124 stride, and this takes
 * the free slot to their left with the same surface from the shared draw kit.
 * Size and stride are read from the neighbour at run time.
 *
 * It has the same bottom lip: 56x3 flush with the bottom edge, #7d7d7d unlit
 * and #afafaf lit (mod_btn_bar), as on the waveform title bar's quick menu.
 *
 * Lit is yellow, not the deck's blue, through the `mode` role (see draw.h), and
 * the lettering and mark turn near-black through text_on_accent, since white
 * on a light chromatic fill is hard to read.
 */
#include "browse/browse.h"

/* ---- what we own --------------------------------------------------------- */

static uintptr_t be_g_bar;                  /* the header, once it has painted  */
static uintptr_t be_g_btn;                  /* our plate                        */
static uintptr_t be_g_peer;                 /* PREVIEW: what our visibility follows */
static uintptr_t be_g_vptr;                 /* the cloned juce::Label vtable    */
static uintptr_t be_g_vt[VT_CLONE_WORDS];
static uintptr_t be_g_orig_bar_paint;
static uintptr_t be_g_orig_mouseup;
static uintptr_t be_g_orig_tick;
static uintptr_t be_g_list;                 /* the track list the mode is on    */
/* The root of the header's tree when the mode was entered. The deck detaches a
 * view instead of hiding it: on the play screen gui::BrowseView has no parent
 * but is still visible and still holds its list. So whether the browse screen
 * is up is decided by the header's root, not by visibility. */
static uintptr_t be_g_root;
static int       be_g_on;                   /* the mode itself                  */
static int       be_g_held;                 /* a finger is on the plate         */
static int       be_g_shown = -1;           /* what we last told the plate      */

static void be_sync(void);

int browse_edit_on(void)
{
    return be_g_on;
}

uintptr_t browse_edit_list(void)
{
    return be_g_on ? be_g_list : 0;
}

/* ---- helpers ------------------------------------------------------------- */

#define BE_TI_BTN       juce_class_of(ep122_sym(EP122_BTN))
#define BE_TI_SWITCHES  juce_class_of(ep122_sym(EP122_BROWSE_SWITCHES))
#define BE_TICK_SLOT    0x10        /* juce::Timer::timerCallback */

static void be_repaint(uintptr_t comp)
{
    uintptr_t fn = ep122_sym(EP122_JUCE_COMP_REPAINT);

    if (comp && fn)
        ((void (*)(void *))fn)((void *)comp);
}

/* ---- is the list on screen a PLAYLIST? ------------------------------------
 *
 * See browse.h. Under gui::PlayListView (the PLAYLIST button's screen) every
 * track list is a playlist. */
static int be_under_playlist_view(uintptr_t list)
{
    uintptr_t ti = juce_class_of(ep122_sym(EP122_PLAYLIST_VIEW)), c = list;
    int i;

    if (!ti)
        return 0;
    for (i = 0; i < 8 && c; i++, c = juce_comp_parent(c))
        if (juce_comp_class(c) == ti)
            return 1;
    return 0;
}

/* The model's row count, via the visible row's owning list box and its
 * model. */
static int be_num_rows(uintptr_t list)
{
    uintptr_t rc = bs_find_visible_class(list, ep122_sym(EP122_ROWCOMP));
    uintptr_t owner = 0, model = 0, vt = 0, fn = 0;

    if (!rc || mod_safe_read(rc + RC_OWNER_OFF, &owner, sizeof(owner)) != 0 || !owner)
        return -1;
    if (mod_safe_read(owner + LB_MODEL_OFF, &model, sizeof(model)) != 0 || !model)
        return -1;
    if (mod_safe_read(model, &vt, sizeof(vt)) != 0 || !vt)
        return -1;
    if (mod_safe_read(vt + MODEL_NUMROWS, &fn, sizeof(fn)) != 0 || !fn)
        return -1;
    return (int)((int64_t (*)(void *))fn)((void *)model);
}

/* The playlist id from the collector's newest held track-list cache, polled
 * every BE_GATE_TICKS and immediately when the list on screen changes. The
 * widget is reused between albums and playlists, so a change is detected by
 * its row count. Kept for the log. */
static uint32_t be_g_pid;

static void be_poll_playlist(uintptr_t list)
{
    static uintptr_t last_list;
    static int       last_rows, ticks;
    int rows = list ? be_num_rows(list) : -1;

    if (list != last_list || rows != last_rows) {
        last_list = list;
        last_rows = rows;
        ticks = 0;
    }
    if (ticks-- <= 0) {
        ticks = BE_GATE_TICKS - 1;
        be_g_pid = mod_djdb_playlist_shown();
    }
}

static int be_on_playlist(uintptr_t list)
{
    return be_under_playlist_view(list) || be_g_pid != 0;
}

/* ---- the mark -------------------------------------------------------------
 *
 * One arrow: a full-height shaft and a solid triangular head widening one pixel
 * per row from the tip. `up` only selects which end the head is at, so both
 * arrows are identical.
 *
 * The two base corners are drawn at half alpha to round them; at eleven pixels
 * across they are the only sharp part of the shape. The alpha blends. */
static void be_arrow(void *g, int x, int y, uint32_t col, int up)
{
    int r, last = BE_HEAD_ROWS - 1;
    int base_y = up ? y + last : y + BE_ARROW_H - 1 - last;

    mod_gfx_colour(g, col);
    mod_gfx_fill(g, x + BE_ARROW_X, y, BE_GLYPH_T, BE_ARROW_H);
    for (r = 0; r < BE_HEAD_ROWS; r++) {
        int ry = up ? y + r : y + BE_ARROW_H - 1 - r;
        int in = (r == last);      /* the widest row, held back by a pixel */

        mod_gfx_fill(g, x + BE_ARROW_X - r + in, ry,
                     BE_GLYPH_T + 2 * r - 2 * in, 1);
    }
    mod_gfx_colour(g, (col & 0x00ffffffu) | 0x80000000u);
    mod_gfx_fill(g, x + BE_ARROW_X - last, base_y, 1, 1);
    mod_gfx_fill(g, x + BE_ARROW_X + BE_GLYPH_T - 1 + last, base_y, 1, 1);
}

/* {left, right, top} of each bar. The middle one extends further left because
 * it has no arrow beside it; this is intentional. */
static const int8_t k_be_bars[3][3] = {
    { 20, 43,  2 },
    {  8, 43, 17 },
    { 20, 43, 32 },
};

static void be_glyph(void *g, int x, int y, uint32_t col)
{
    int i;

    mod_gfx_colour(g, col);
    for (i = 0; i < 3; i++)
        mod_gfx_fill(g, x + k_be_bars[i][0], y + k_be_bars[i][2],
                     k_be_bars[i][1] - k_be_bars[i][0] + 1, BE_GLYPH_T);
    be_arrow(g, x, y, col, 1);
    be_arrow(g, x, y + BE_ARROW_Y, col, 0);
}

/* ---- the plate ----------------------------------------------------------- */

static void be_paint(void *self, void *g)
{
    const struct theme_ui *ui = mod_ui();
    int32_t b[4];
    uint32_t lift, ink;

    if ((uintptr_t)self != be_g_btn || juce_comp_bounds((uintptr_t)self, b) != 0)
        return;
    lift = be_g_held ? MOD_CHECKER_HOT_Q8 : 0;
    /* Near-black (text_on_accent) on the lit yellow plate, for legibility. */
    ink = be_g_on ? ui->text_on_accent : ui->text;

    /* Unlit uses the theme's second grey; lit is chromatic, which the duotone
     * does not change, so a derived partner colour is used. */
    mod_draw_enter();
    if (be_g_on)
        mod_checker_lift(g, 0, 0, b[2], b[3], ui->mode, lift);
    else
        mod_checker_lift2(g, 0, 0, b[2], b[3], ui->surface, ui->surface2, lift);
    mod_btn_bar(g, 0, 0, b[2], b[3],
                mod_colour_lift(be_g_on ? ui->bar_on : ui->bar, lift));
    /* Our own lettering: the Label can only centre in its bounds, and this
     * plate has a word and a mark. The Label holds no text, so Label::paint is
     * not chained (it would draw nothing). */
    mod_gfx_text(g, BE_TEXT, BE_FONT, ink, 0, BE_LABEL_CY - b[3] / 2,
                 b[2], b[3], JUCE_JUSTIFY_CENTRED);
    be_glyph(g, (b[2] - BE_GLYPH_W) / 2, BE_GLYPH_Y, ink);
    mod_draw_leave();
}

static void be_mousedown(void *self, void *event)
{
    (void)event;
    if ((uintptr_t)self != be_g_btn)
        return;
    /* Toggle on press, like the deck's own buttons; waiting for release feels
     * laggy. */
    be_g_held = 1;
    if (be_g_on) {
        be_g_on = 0;
        be_g_list = be_g_root = 0;
        browse_sort_give_back(1);
    } else {
        /* The mode stays off if the sort could not be taken: under another
         * sort a moved row would land at a position the DJ cannot see. */
        be_g_on = browse_sort_take(be_g_bar) == 0;
        /* Record which list: RowComp is the row of every touchable table in
         * the app, including the browse sidebar and DJ SETTINGS, so the
         * gesture is limited to this list. */
        be_g_list = be_g_on ? browse_track_list(be_g_bar) : 0;
        be_g_root = be_g_on ? juce_comp_root(be_g_bar) : 0;
    }
    be_repaint(be_g_btn);
    /* Also repaint the list: EDIT hides the selection plate, and the rows would
     * otherwise not repaint until something else invalidated them. */
    be_repaint(be_g_list ? be_g_list : browse_track_list(be_g_bar));
    MDBG("browse: EDIT %s\n", be_g_on ? "on" : "off");
}

static void be_mouseup(void *self, void *event)
{
    if ((uintptr_t)self == be_g_btn && be_g_held) {
        be_g_held = 0;
        be_repaint(be_g_btn);
    }
    if (be_g_orig_mouseup)
        ((void (*)(void *, void *))be_g_orig_mouseup)(self, event);
}

static int be_vt_ready(void)
{
    const struct juce_vt_override ov[] = {
        { JUCE_VT_MOUSEDOWN, (void *)be_mousedown, NULL },
        { JUCE_VT_MOUSEUP,   (void *)be_mouseup,   &be_g_orig_mouseup },
        { JUCE_VT_PAINT,     (void *)be_paint,     NULL },
    };

    if (be_g_vptr)
        return 1;
    be_g_vptr = juce_label_vt_clone(be_g_vt, ov,
                                    (int)(sizeof(ov) / sizeof(ov[0])));
    return be_g_vptr != 0;
}

/* ---- placement ------------------------------------------------------------
 *
 * The group is gui::BrowseDispSwitchButtonsWidget and the plates are its
 * children: PREVIEW at 0, the font-size button at 124, INFO at 267, each 114
 * wide except INFO. The group's x is where the run starts, the first two
 * children give the stride, and our slot is one stride before it.
 *
 * Matches gui::TogglesImageButton exactly by typeinfo, which excludes the back
 * arrow (a gui::TogglesImageButtonEx, a derived class).
 */
static int be_slot(uintptr_t bar, int32_t out[4])
{
    uintptr_t ti = BE_TI_BTN;
    uintptr_t group = juce_comp_child_of_class(bar, BE_TI_SWITCHES);
    uintptr_t left = 0;
    int32_t gb[4], first[4] = { 0, 0, 0, 0 };
    int32_t next_x = 0;
    int n, i;

    if (!ti || !group || juce_comp_bounds(group, gb) != 0) {
        MDBG("browse: no switch-button group in the header -> no EDIT\n");
        return -1;
    }
    n = juce_comp_nchild(group);
    for (i = 0; i < n; i++) {
        uintptr_t c = juce_comp_child(group, i);
        int32_t cb[4];

        if (!c || juce_comp_class(c) != ti || juce_comp_bounds(c, cb) != 0)
            continue;
        if (!left || cb[0] < first[0]) {
            if (left)
                next_x = first[0];
            first[0] = cb[0]; first[1] = cb[1];
            first[2] = cb[2]; first[3] = cb[3];
            left = c;
        } else if (!next_x || cb[0] < next_x) {
            next_x = cb[0];
        }
    }
    if (!left) {
        MDBG("browse: the switch-button group holds no plate -> no EDIT\n");
        return -1;
    }

    /* Offset by the group's x: button positions are relative to the group, ours
     * to the header. */
    out[0] = gb[0] - (next_x ? next_x - first[0] : first[2] + BE_GAP);
    out[1] = gb[1] + first[1];
    out[2] = first[2];
    out[3] = first[3];
    if (out[0] < BE_MIN_X) {
        MDBG("browse: the slot left of x=%d is off the bar -> no EDIT\n",
             (int)gb[0]);
        return -1;
    }
    be_g_peer = left;
    return 0;
}

/* One plate per header. There are two gui::BrowseTitleWidgets, the browse
 * screen's and the PLAYLIST button screen's, both live under the
 * ViewTransitionManager.
 *
 * Fixed size: the app builds these once and keeps them. A header beyond
 * BE_MAX_BARS gets no plate. */
static struct be_plate {
    uintptr_t bar, btn, peer;
} be_g_plate[BE_MAX_BARS];

static struct be_plate *be_find_plate(uintptr_t btn)
{
    int i;

    for (i = 0; i < BE_MAX_BARS; i++)
        if (be_g_plate[i].btn && be_g_plate[i].btn == btn)
            return &be_g_plate[i];
    return NULL;
}

static void be_attach(uintptr_t bar)
{
    int32_t slot[4];
    int i, free_at = -1;

    if (!bar)
        return;
    for (i = 0; i < BE_MAX_BARS; i++) {
        if (be_g_plate[i].bar == bar) {
            /* Already attached. This runs from the paint of the header on
             * screen, so it also tracks which one that is. */
            be_g_bar = bar;
            be_g_btn = be_g_plate[i].btn;
            be_g_peer = be_g_plate[i].peer;
            return;
        }
        if (!be_g_plate[i].bar && free_at < 0)
            free_at = i;
    }
    if (free_at < 0)
        return;
    if (!be_vt_ready() || be_slot(bar, slot) != 0)
        return;

    be_g_bar = bar;
    /* Empty: the plate paints its own word. The Label only provides a
     * component with a vtable we own for press and paint. */
    be_g_btn = juce_label(bar, "", BE_FONT, 0x00000000u, mod_ui()->text,
                          be_g_vptr, slot[0], slot[1], slot[2], slot[3]);
    if (!be_g_btn) {
        MDBG("browse: the EDIT plate would not build\n");
        return;
    }
    be_g_plate[free_at].bar = bar;
    be_g_plate[free_at].btn = be_g_btn;
    be_g_plate[free_at].peer = be_g_peer;
    be_g_shown = -1;
    be_sync();
    MDBG("browse: EDIT #%d at {%d,%d,%d,%d} on header %p, peer %p\n",
         free_at, (int)slot[0], (int)slot[1], (int)slot[2], (int)slot[3],
         (void *)bar, (void *)be_g_peer);
}

/* ---- when it is on screen at all ------------------------------------------
 *
 * Shown over a visible track list that the deck filled from djdbSongPlaylist.
 *
 * Do not key visibility on PREVIEW: it is also hidden on the PLAYLIST button's
 * screen, which is a track list.
 *
 * Playlist-only: an artist's or album's `#` is the track's album number from
 * its tags. be_on_playlist is 0 unless the list is served from a playlist's
 * cache or sits on the PLAYLIST screen.
 *
 * be_tick turns the mode off when leaving, not just the plate, so EDIT is never
 * latched while invisible. */
static void be_sync(void)
{
    uintptr_t vptr = 0, list;
    int want;

    if (!be_g_btn || !be_g_peer)
        return;
    /* Our plate is a child of the deck's header, so the deck deletes it if it
     * rebuilds the browse view, and this runs at 44 Hz on a held pointer. If the
     * vptr no longer matches, drop the attachment so the header's next paint
     * rebuilds it, instead of writing into freed memory. */
    if (mod_safe_read(be_g_btn, &vptr, sizeof(vptr)) != 0 || vptr != be_g_vptr) {
        struct be_plate *p = be_find_plate(be_g_btn);

        MDBG("browse: the header took our EDIT plate with it -- rebuilding\n");
        if (p)
            p->bar = p->btn = p->peer = 0;
        be_g_btn = be_g_peer = be_g_bar = be_g_list = 0;
        be_g_shown = -1;
        be_g_on = be_g_held = 0;
        return;
    }
    /* A track list with a `#` column has a stored order. Lists without one
     * (all tracks, an artist's tracks, a search result) have the column's
     * flags cleared by the deck, so this is one bit. */
    list = browse_track_list(be_g_bar);
    be_poll_playlist(list);
    want = list != 0 && browse_sort_has_position(be_g_bar) && be_on_playlist(list);
    if (want == be_g_shown)
        return;
    MDBG("browse: EDIT %s -- list %p, playlist %u%s\n",
         want ? "shown" : "hidden", (void *)list, (unsigned)be_g_pid,
         list && be_under_playlist_view(list) ? " (PLAYLIST screen)" : "");
    be_g_shown = want;
    juce_comp_set_visible(be_g_btn, want);
}

/* ---- the anchor ---------------------------------------------------------- */

/* Every gui::BrowseTitleWidget's paint: tells the module that a header exists
 * and which one is on screen. The browse screen and gui::PlayListView each have
 * one, so this attaches per header. */
static void be_bar_paint(void *self, void *g)
{
    if (be_g_orig_bar_paint)
        ((void (*)(void *, void *))be_g_orig_bar_paint)(self, g);
    be_attach((uintptr_t)self);
}

/* The deck's 44 Hz display refresh. The header paint is not used as the clock:
 * the bar is not repainted when PREVIEW appears or disappears, so the plate
 * would lag a screen behind. */
static void be_tick(void *self)
{
    if (be_g_orig_tick)
        ((void (*)(void *))be_g_orig_tick)(self);
    be_sync();
    browse_drag_tick();
    /* The mode belongs to one list and ends when that list leaves the screen:
     * going back to the playlist chooser, switching category, or leaving the
     * browse screen. The list check and the `#` check are both needed: the list
     * changes with the view, while `#` can disappear with the same list up. */
    if (be_g_on) {
        uintptr_t now = browse_track_list(be_g_bar);
        const char *why = NULL;

        if (juce_comp_root(be_g_bar) != be_g_root)
            why = "the browse screen is not the one showing";
        else if (now != be_g_list)
            why = "the list it was turned on over is not on screen";
        else if (browse_sort_hold() != 0)
            why = "the `#` column left the header";
        if (why) {
            MDBG("browse: EDIT off -- %s\n", why);
            be_g_on = 0;
            be_g_list = be_g_root = 0;
            browse_sort_give_back(0);
            be_repaint(be_g_btn);
            be_repaint(now);
        }
    }
}

static int be_install(void)
{
    /* Install the gesture first, and the plate only if it succeeded; a plate
     * that cannot reorder would still take the DJ's sort. If a later step fails
     * the row hooks stay installed but inert, since EDIT can never turn on. */
    if (browse_drag_install() != 0)
        return -1;
    if (!FN_LABEL_CTOR || !FN_ADD_VISIBLE || !FN_SET_BOUNDS || !FN_FONT_BUILD ||
        !FN_LABEL_SETFONT || !FN_LABEL_JUSTIFY || !FN_COMP_SETCOLOUR ||
        !MOD_FN_GFX_SETCOLOUR || !MOD_FN_GFX_FILLRECT ||
        !MOD_FN_GFX_SETFONT || !MOD_FN_GFX_DRAWTEXT) {
        MDBG("browse: juce primitives did not resolve -> no EDIT\n");
        return -1;
    }
    if (mod_patch_vslot("browseTitlePaint", EP122_BROWSE_TITLE, JUCE_VT_PAINT,
                        (void *)be_bar_paint, &be_g_orig_bar_paint) != 0) {
        MDBG("browse: no header anchor -> no EDIT\n");
        return -1;
    }
    /* Required: the tick ends the mode when its list leaves, keeps the header's
     * sortable bits cleared, and commits a drop once the release has settled. */
    if (mod_patch_vslot("browseTick", EP122_DISPLAY_REFRESH, BE_TICK_SLOT,
                        (void *)be_tick, &be_g_orig_tick) != 0) {
        MDBG("browse: no display tick -> no EDIT (the mode could not end, the"
             " sort could not be held, and a drop could not commit)\n");
        return -1;
    }
    return 0;
}

/* One mod for the whole feature: the plate, the sort and the gesture are three
 * files but install together (see be_install). */
KIT_MOD(k_mod_browse_reorder,
        .name = "browse_reorder", .prio = 36, .install = be_install,
        .what = "drag a track to a new place in a playlist, behind an EDIT gate");
