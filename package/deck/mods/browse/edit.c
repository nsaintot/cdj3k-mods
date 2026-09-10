// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * browse/edit.c - the EDIT toggle in the browse header.
 *
 * The gate in front of the reorder gesture. See browse.h for why the feature
 * needs a gate at all and where it attaches.
 *
 * ---- the plate ------------------------------------------------------------
 *
 * It is one of the header's buttons, not a decoration beside them: the deck's
 * PREVIEW and font-size plates are 114x90 four-pixel checkerboards on a 124
 * stride, and this takes the free slot to their left with the same surface from
 * the shared draw kit. Both numbers come off the neighbour at run time -- the
 * point of reading the layout is not to have written it down.
 *
 * Including the bottom lip: 56x3 flush with the bottom edge, #7d7d7d unlit and
 * #afafaf lit, which is mod_btn_bar unchanged. It is the same mark the waveform
 * title bar's quick menu wears, and a plate without one does not read as a
 * button on this deck.
 *
 * Lit is YELLOW rather than the deck's blue, through the `mode` role -- see the
 * note beside it in draw.h -- and the lettering and the mark go NEAR-BLACK with
 * it, through text_on_accent. Ink that stays white over a light chromatic fill
 * is the one combination on this deck that does not hold at arm's length.
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
/* The top of the tree the header hung from when the mode was entered. The deck
 * DETACHES a view rather than hiding it -- measured: stepping to the play screen
 * leaves gui::BrowseView with no parent at all, still visible, still holding its
 * list. So "is the browse screen up" is not a visibility question, and the walk
 * that looks for the list happily found it inside the orphaned subtree. What
 * changes is what the header's chain ends at. */
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
 * See browse.h. Under gui::PlayListView -- the PLAYLIST button's screen -- a
 * track list is a playlist by construction. */
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

/* The model's row count, through the slot the deck reads it from: the list's
 * visible row's owner is the box, and the box holds the model. */
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

/* The browse screen's answer: the collector's newest held track-list cache,
 * asked every BE_GATE_TICKS, and at once when the list on screen changes --
 * the widget is reused between an album and a playlist, so the row count is
 * the change that shows. Kept in be_g_pid so the log can quote it. */
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
 * One arrow: a shaft the full height, and a solid triangular head widening away
 * from the tip a row at a time. `up` only chooses which end the head grows
 * from; everything else is shared, which is what keeps the two arrows identical
 * rather than nearly identical.
 *
 * The two BASE CORNERS come back at half alpha instead of at full, which is the
 * whole of "rounded" at eleven pixels across: the tip is already blunt at three
 * pixels and the diagonals are already a staircase, so the corners are the only
 * hard thing in the shape. Alpha in the colour blends, so this is real coverage
 * rather than a lighter dot. */
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

/* {left, right, top} of each bar. The middle one reaches further left because
 * it has no arrow beside it -- that asymmetry is the icon, not a slip. */
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
    /* Near-black on the lit plate, which is what text_on_accent is for: white
     * on a light chromatic fill is the one combination on this deck that does
     * not hold at arm's length, and the yellow is a light fill. */
    ink = be_g_on ? ui->text_on_accent : ui->text;

    /* Unlit takes the theme's authored second grey; lit is chromatic, which
     * the duotone leaves alone, so a derived partner is right for it. */
    mod_draw_enter();
    if (be_g_on)
        mod_checker_lift(g, 0, 0, b[2], b[3], ui->mode, lift);
    else
        mod_checker_lift2(g, 0, 0, b[2], b[3], ui->surface, ui->surface2, lift);
    mod_btn_bar(g, 0, 0, b[2], b[3],
                mod_colour_lift(be_g_on ? ui->bar_on : ui->bar, lift));
    /* Our own lettering rather than the Label's, which can only centre in its
     * own bounds -- and this plate carries a word AND a mark. The Label holds no
     * text at all, which is also why Label::paint is not chained here: with a
     * transparent background and nothing to write it would draw nothing. */
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
    /* On the press, like the deck's own buttons: the lift below is what the
     * finger is for, and a toggle that waits for the release reads as lag. */
    be_g_held = 1;
    if (be_g_on) {
        be_g_on = 0;
        be_g_list = be_g_root = 0;
        browse_sort_give_back(1);
    } else {
        /* The mode does not come on if the sort could not be taken. A reorder
         * under someone else's sort would move the row the DJ is looking at to
         * a position they cannot see, so half of this feature is worse than
         * none of it. */
        be_g_on = browse_sort_take(be_g_bar) == 0;
        /* WHICH list, not just "a mode is on". RowComp is the row of every
         * touchable table in the app -- the browse sidebar and DJ SETTINGS
         * included -- so a gesture gated on the mode alone carries THOSE rows
         * too, and a tap on the sidebar becomes a drag that eats the tab. */
        be_g_list = be_g_on ? browse_track_list(be_g_bar) : 0;
        be_g_root = be_g_on ? juce_comp_root(be_g_bar) : 0;
    }
    be_repaint(be_g_btn);
    /* AND THE LIST, because the mode changes how its rows draw -- the selection
     * plate goes while EDIT is on -- and nothing else would invalidate them. The
     * rows are painted once and left alone until something touches them, so
     * without this the mode's first effect on a row is whenever it next happens
     * to repaint for an unrelated reason. */
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
 * The group is gui::BrowseDispSwitchButtonsWidget and the plates are ITS
 * children: PREVIEW at 0, the font-size button at 124, INFO at 267, each 114
 * wide except INFO. So the group's own x is where the right-hand run starts, the
 * first two children give the stride, and our slot is one stride before it.
 *
 * Only gui::TogglesImageButton exactly, which is a typeinfo compare and so does
 * not catch the back arrow -- that is a gui::TogglesImageButtonEx, a different
 * class deriving from it.
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

    /* The group's x, not the button's: the button's is relative to the group and
     * ours is relative to the header. */
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

/* ONE PLATE PER HEADER. There is more than one gui::BrowseTitleWidget: the
 * browse screen has one and the screen behind the deck's own PLAYLIST button has
 * another, both live at once under the ViewTransitionManager. Binding to the
 * first one that painted meant the second screen -- the one a DJ reaches with a
 * dedicated button, showing nothing but a playlist -- could never carry an EDIT
 * at all.
 *
 * Small and fixed: the app builds these once and keeps them, so a table this
 * size is the whole set rather than a cache. A header past the end simply goes
 * without, which is the same failure the single binding had, for the same cost. */
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
            /* Already ours. Follow it: this runs from the paint of whichever
             * header is on screen, so it is also how the module learns which
             * one that is. */
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
    /* Empty: the plate paints its own word. The Label is here to be a component
     * with a vtable we own -- somewhere to put a press and a paint. */
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
 * A VISIBLE TRACK LIST, and one the deck filled from djdbSongPlaylist.
 *
 * The first version mirrored PREVIEW, on the grounds that the deck hides that
 * whole button group where there are no tracks. It does -- but PREVIEW is also
 * hidden on the playlist screen reached directly rather than through BROWSE,
 * which is a track list, and is exactly where a DJ would look for this. The
 * widget itself is the honest question.
 *
 * The second half is the PLAYLIST-ONLY gate. An artist's or album's `#` is the
 * track's own album number out of its tags, so a reorder there would rewrite
 * metadata rather than move anything; be_on_playlist is 0 unless the list on
 * screen is served from a playlist's cache or sits on the PLAYLIST screen.
 *
 * Turning the mode OFF on the way out, not just hiding the plate: coming back to
 * a browse screen with an invisible EDIT still latched would leave the list in a
 * mode with nothing on screen saying so. */
static void be_sync(void)
{
    uintptr_t vptr = 0, list;
    int want;

    if (!be_g_btn || !be_g_peer)
        return;
    /* Our plate is a CHILD of the deck's header, so the deck deletes it if it
     * ever rebuilds the browse view -- and this runs 44 times a second off a
     * pointer we are holding. One read says whether the object is still ours;
     * anything else and the whole attachment is dropped, so the header's next
     * paint builds it again rather than this writing into freed memory. */
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
    /* A TRACK LIST WITH POSITIONS IN IT. `#` is the whole question: a list that
     * shows one has a stored order to move a row within, and a list that does
     * not -- all tracks, an artist's tracks, a search result -- has nothing a
     * reorder could mean. The deck clears the column's flags on those, so this
     * is one bit rather than a guess about which view is up. */
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

/* Every gui::BrowseTitleWidget's paint, which is how the module learns both that
 * a header exists and which one is on screen. There is more than one -- the
 * browse screen has its own and gui::PlayListView another, alive together -- so
 * this attaches per header rather than to the first that paints. */
static void be_bar_paint(void *self, void *g)
{
    if (be_g_orig_bar_paint)
        ((void (*)(void *, void *))be_g_orig_bar_paint)(self, g);
    be_attach((uintptr_t)self);
}

/* The deck's own 44 Hz display refresh. The header paint would be the cheaper
 * clock and is the wrong one: PREVIEW appearing is not a reason for the bar to
 * be invalidated, so following it from there leaves our plate a screen behind
 * whichever way it moved. */
static void be_tick(void *self)
{
    if (be_g_orig_tick)
        ((void (*)(void *))be_g_orig_tick)(self);
    be_sync();
    browse_drag_tick();
    /* THE MODE BELONGS TO ONE LIST, and ends when that list is not what is on
     * screen -- which covers walking back up to the playlist chooser, switching
     * category, and leaving the browse screen altogether. Two conditions rather
     * than one because they fail at different moments: the list changes as soon
     * as the view does, and `#` can go while the same list is still up. */
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
    /* THE GESTURE FIRST, and the plate only if it took. A plate that cannot
     * reorder is worse than none: pressing it still borrows the DJ's sort and
     * forces `#`, and hands back nothing for it. Failing here leaves the row
     * hooks in place and inert -- they all return to stock unless EDIT is on,
     * and without a plate it never can be. */
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
    /* REQUIRED. The tick is not decoration: it is what ends the mode when its
     * list leaves, what holds the header disabled against the deck putting the
     * sortable bits back, and what commits a drop once the release has settled.
     * Without it the sort stays clamped on whatever list follows and no drag
     * ever writes -- so the feature does not go up at all rather than going up
     * broken. */
    if (mod_patch_vslot("browseTick", EP122_DISPLAY_REFRESH, BE_TICK_SLOT,
                        (void *)be_tick, &be_g_orig_tick) != 0) {
        MDBG("browse: no display tick -> no EDIT (the mode could not end, the"
             " sort could not be held, and a drop could not commit)\n");
        return -1;
    }
    return 0;
}

/* ONE MOD FOR THE WHOLE FEATURE. The plate, the sort it borrows and the gesture
 * are three files but a single thing, and there is no useful state where some of
 * them are installed: see be_install. */
KIT_MOD(k_mod_browse_reorder,
        .name = "browse_reorder", .prio = 36, .install = be_install,
        .what = "drag a track to a new place in a playlist, behind an EDIT gate");
