// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * browse/sort.c - the sort EDIT borrows, and gives back.
 *
 * See browse.h for why the mode owns the sort at all. This file is only the
 * mechanism: find the live header, read what the DJ had, force `#` ascending,
 * and put it back.
 *
 * ---- what a juce::TableHeaderComponent holds -------------------------------
 *
 * gui::TrackListHeader IS one, with mouseDown its single override, so all of
 * this is stock juce read out of getSortColumnId (0x1b7b0d0) and
 * setSortColumnId (0x1bad710) rather than guessed at:
 *
 *   header + 0xd8   ColumnInfo**   the columns, as pointers
 *   header + 0xe8   int            how many
 *   column + 0x00   juce::String   the name -- EMPTY on this skin, see below
 *   column + 0x08   int            id
 *   column + 0x0c   int            propertyFlags
 *
 * and the three flags that matter are juce's own: sortable 0x10, sortedForwards
 * 0x20, sortedBackwards 0x40. getSortColumnId is literally "the column carrying
 * either sorted bit", which is also how the direction is read here.
 */
#include "browse/browse.h"

#define HDR_COLUMNS_OFF   0xd8
#define HDR_NCOLUMN_OFF   0xe8
/* gui::TrackListHeader's OWN listeners -- gui::TrackListHeaderListener, one
 * virtual, taking the column id. Read out of its mouseDown (0x14e8558), which
 * walks exactly this pair. */
#define HDR_LISTENERS_OFF 0x150
#define HDR_NLISTENER_OFF 0x160
#define COL_ID_OFF        0x08
#define COL_FLAGS_OFF     0x0c
#define COL_VISIBLE       0x01
#define COL_SORTABLE      0x10
#define COL_FORWARDS      0x20
#define COL_BACKWARDS     0x40

#define BS_MAX_COLUMNS    16

#define BS_TI_LIST    juce_class_of(ep122_sym(EP122_TRACKLIST))
#define BS_TI_HEADER  juce_class_of(ep122_sym(EP122_TRACKLIST_HEADER))
#define BS_TI_BTN     juce_class_of(ep122_sym(EP122_BTN))

#define FN_HDR_GET_SORTCOL  ep122_sym(EP122_JUCE_HDR_GET_SORTCOL)
#define FN_HDR_SET_SORTCOL  ep122_sym(EP122_JUCE_HDR_SET_SORTCOL)

/* What was borrowed, and from whom. `held` rather than "is the id non-zero":
 * a list with no sort at all reports 0, and that is a state worth restoring
 * exactly as much as any other. */
static uintptr_t bs_g_header;
static uintptr_t bs_g_dirbtn;         /* the header's own ascending/descending control */
static int       bs_g_held;
static int       bs_g_col;            /* the `#` column the mode is holding */
static int       bs_g_was_col;
static int       bs_g_was_fwd;
static uint32_t  bs_g_was_sortable;   /* one bit per column, in column order */

/* ---- reaching the live header -------------------------------------------- */

static uintptr_t bs_column(uintptr_t header, int i)
{
    uintptr_t arr = 0, col = 0;

    if (mod_safe_read(header + HDR_COLUMNS_OFF, &arr, sizeof(arr)) != 0 || !arr)
        return 0;
    if (mod_safe_read(arr + (size_t)i * sizeof(col), &col, sizeof(col)) != 0)
        return 0;
    return col;
}

static int bs_ncolumn(uintptr_t header)
{
    int32_t n = 0;

    if (mod_safe_read(header + HDR_NCOLUMN_OFF, &n, sizeof(n)) != 0)
        return 0;
    return (n < 0 || n > BS_MAX_COLUMNS) ? 0 : (int)n;
}

static int bs_flags(uintptr_t col)
{
    int32_t f = 0;

    return mod_safe_read(col + COL_FLAGS_OFF, &f, sizeof(f)) == 0 ? (int)f : 0;
}

static int bs_id(uintptr_t col)
{
    int32_t v = 0;

    return mod_safe_read(col + COL_ID_OFF, &v, sizeof(v)) == 0 ? (int)v : 0;
}

/* Depth-first for a VISIBLE component of that class. The browse view carries two
 * gui::TrackListWidgets, the full-width one and the one beside a hierarchy, and
 * only ever shows one -- so "the first of that class" finds the wrong header
 * half the time. Bounded like every walk over the app's live tree. */
static uintptr_t bs_find_visible(uintptr_t comp, uintptr_t ti, int depth)
{
    int n, i;

    if (!comp || depth > 8 || !juce_comp_visible(comp))
        return 0;
    if (juce_comp_class(comp) == ti)
        return comp;
    n = juce_comp_nchild(comp);
    for (i = 0; i < n; i++) {
        uintptr_t hit = bs_find_visible(juce_comp_child(comp, i), ti, depth + 1);

        if (hit)
            return hit;
    }
    return 0;
}

uintptr_t bs_find_visible_class(uintptr_t comp, uintptr_t vt)
{
    uintptr_t ti = vt ? juce_class_of(vt) : 0;

    return ti ? bs_find_visible(comp, ti, 0) : 0;
}

/* The header of whichever track list is on screen. NOT cached across a take:
 * which of the two lists is showing is exactly what changes between one press
 * and the next. */
uintptr_t browse_track_list(uintptr_t bar)
{
    uintptr_t root = juce_comp_root(bar);

    return (root && BS_TI_LIST) ? bs_find_visible(root, BS_TI_LIST, 0) : 0;
}

static uintptr_t bs_header(uintptr_t bar)
{
    uintptr_t list = browse_track_list(bar);

    if (!list || !BS_TI_HEADER)
        return 0;
    /* The header is a child of the list and is visible with it, so the same
     * walk serves -- and it cannot pick up the other list's header, because the
     * search starts inside this one. */
    return bs_find_visible(list, BS_TI_HEADER, 0);
}

/* ---- reading and writing the sort ---------------------------------------- */

static int bs_sorted_forwards(uintptr_t header)
{
    int n = bs_ncolumn(header), i;

    for (i = 0; i < n; i++) {
        uintptr_t col = bs_column(header, i);
        int f = col ? bs_flags(col) : 0;

        if (f & (COL_FORWARDS | COL_BACKWARDS))
            return (f & COL_FORWARDS) != 0;
    }
    return 1;
}

/* `#` IS COLUMN ID 3. Not inferred -- read out of
 * gui::TrackListHeader::TrackListHeader(gui::TrackListType) (0x14e7648), which
 * names its seven columns as it adds them:
 *
 *   id 1 "PREVIEW"  id 2 ""  id 3 "#"  id 4 "TRACK"  id 5 ""  id 6 "BPM"  id 7 "KEY"
 *
 * with flags 1 on the first two and 0x11 -- visible|sortable -- on 3..7. So the
 * columns DO carry names; reading them live off ColumnInfo came back empty, and
 * that read was wrong rather than the columns being nameless.
 *
 * A list with no positions -- all tracks, an artist's tracks -- gets id 3's
 * visible bit cleared through gui::TrackListWidget's own show/hide (0x14f3020).
 * So `#` on screen is a fact about THIS list, not about the header object, which
 * is shared between them.
 *
 * The deck also owns the sortable bit: gui::TrackListHeader::setSortEnabled
 * (0x14e7050) walks ids 3..7 and rewrites each one's flags to `visible ? 0x11 :
 * 0x10` -- which is why holding the header disabled has to be a poll. */
#define BS_POSITION_COL   3

static void bs_dump_columns(uintptr_t header)
{
    int n = bs_ncolumn(header), i;

    for (i = 0; i < n; i++) {
        uintptr_t col = bs_column(header, i);

        if (col)
            MDBG("browse: column %d id %d flags %#x\n",
                 i, bs_id(col), bs_flags(col));
    }
}

/* Tell the header's listener a column was chosen -- which is the half that
 * actually REORDERS ANYTHING.
 *
 * setSortColumnId only moves juce's own marker: it clears the sorted bits, sets
 * one, repaints and triggers juce's async update. The deck does not re-sort a
 * juce table, it RE-ASKS THE LIBRARY -- gui::TrackListWidget maps the column id
 * to a sort kind and passes it down to
 * TrackListDisplayFormatEventFacade::sort(), which fetches the list again. So
 * without this the header said `#` while the rows stayed in the DJ's old order,
 * which is the one outcome worse than not touching the sort at all.
 *
 * Both halves in the same order the deck's own mouseDown does them: mark first,
 * then notify, because the listener reads the direction back off the header. */
static void bs_notify(uintptr_t header, int column_id)
{
    uintptr_t arr = 0;
    int32_t n = 0;
    int i;

    if (mod_safe_read(header + HDR_LISTENERS_OFF, &arr, sizeof(arr)) != 0 || !arr)
        return;
    if (mod_safe_read(header + HDR_NLISTENER_OFF, &n, sizeof(n)) != 0)
        return;
    if (n < 0 || n > 8)
        return;
    for (i = 0; i < n; i++) {
        uintptr_t l = 0, vt = 0, fn = 0;

        if (mod_safe_read(arr + (size_t)i * sizeof(l), &l, sizeof(l)) != 0 || !l)
            continue;
        if (mod_safe_read(l, &vt, sizeof(vt)) != 0 || !vt)
            continue;
        if (mod_safe_read(vt, &fn, sizeof(fn)) != 0 || !fn)
            continue;
        ((void (*)(void *, int))fn)((void *)l, column_id);
    }
}

/* Put the list on (column, direction), whatever it is on now.
 *
 * The deck's rule, measured: a notify naming the column it is ALREADY sorted by
 * FLIPS the direction; naming any other column sorts that one ascending. It is
 * the header's own double-tap behaviour, and it lives below the notify -- the
 * marker says nothing to it, which is why setting the marker to `forwards` and
 * then notifying came back descending.
 *
 * So the number of notifies is a small piece of arithmetic rather than a call,
 * and the marker is set at the END, to whatever we actually arrived at. */
static void bs_goto(uintptr_t header, int col, int forwards)
{
    int cur = (int)((int64_t (*)(void *))FN_HDR_GET_SORTCOL)((void *)header);
    int cur_fwd = bs_sorted_forwards(header);
    int n = 0;

    if (cur != col)
        n = forwards ? 1 : 2;      /* a fresh column lands ascending */
    else if (cur_fwd != forwards)
        n = 1;
    while (n-- > 0)
        bs_notify(header, col);
    ((void (*)(void *, int, int))FN_HDR_SET_SORTCOL)((void *)header, col, forwards);
}

static void bs_set_sortable(uintptr_t header, int on)
{
    int n = bs_ncolumn(header), i;

    for (i = 0; i < n && i < BS_MAX_COLUMNS; i++) {
        uintptr_t col = bs_column(header, i);
        int32_t f;

        if (!col)
            continue;
        f = bs_flags(col);
        if (on) {
            if (!(bs_g_was_sortable & (1u << i)))
                continue;               /* it was not sortable to begin with */
            f |= COL_SORTABLE;
        } else {
            if (f & COL_SORTABLE)
                bs_g_was_sortable |= 1u << i;
            f &= ~COL_SORTABLE;
        }
        mod_safe_write(col + COL_FLAGS_OFF, &f, sizeof(f));
    }
}

/* The header's own ascending/descending control, which is a plain
 * TogglesImageButton child. Clearing the columns' sortable bit stops the deck
 * DRAWING their arrows but says nothing about this one, so it is hidden by hand
 * and shown again with the sort. */
static void bs_dir_button(uintptr_t header, int visible)
{
    if (!bs_g_dirbtn)
        bs_g_dirbtn = juce_comp_child_of_class(header, BS_TI_BTN);
    if (bs_g_dirbtn)
        juce_comp_set_visible(bs_g_dirbtn, visible);
}

/* The column with that id, whatever position it is in. */
static uintptr_t bs_column_by_id(uintptr_t header, int id)
{
    int n = bs_ncolumn(header), i;

    for (i = 0; i < n; i++) {
        uintptr_t col = bs_column(header, i);

        if (col && bs_id(col) == id)
            return col;
    }
    return 0;
}

/* IS THERE STILL A `#` ON SCREEN? -- the mode's own expiry.
 *
 * It asks the question that decides the answer rather than one that correlates
 * with it: a reorder moves a row to a POSITION, and a list showing no `#` has
 * none to move it to. The header keeps its seven columns across a view change
 * and moves their `visible` bit, so this reads as one flag.
 *
 * The obvious alternative -- watch the gui::TrackListWidget change -- was built
 * first and did not fire: EDIT stayed lit over the all-tracks view with the
 * playlist's sort still clamped on. Whatever it was comparing, it was not
 * catching the switch, and this does.
 *
 * Tested on `visible` rather than on `sortable` because sortable is the bit this
 * mode itself takes away. */
int browse_sort_hold(void)
{
    uintptr_t col;

    if (!bs_g_held || !bs_g_header)
        return 0;
    col = bs_column_by_id(bs_g_header, bs_g_col);
    if (!col || !(bs_flags(col) & COL_VISIBLE))
        return -1;
    bs_set_sortable(bs_g_header, 0);
    if (bs_g_dirbtn && juce_comp_visible(bs_g_dirbtn))
        juce_comp_set_visible(bs_g_dirbtn, 0);
    return 0;
}

/* THE COLUMN TO GO AWAY TO. Any sortable one that is not `#`; TRACK is id 4 and
 * the header's constructor always adds it, so there is nothing to search for.
 * The mode has cleared the sortable bits by now, and bs_notify does not consult
 * them -- it calls the listener the way the header's own mouseDown does. */
#define BS_ALT_COL  4

/* MAKE THE DECK FETCH THE LIST AGAIN, for after a reorder.
 *
 * A SORT KIND CHANGE IS A LIBRARY MESSAGE; a direction flip is not. Measured
 * both ways: naming the column the list is already sorted by is answered inside
 * the UI thread from the rows the widget holds, and the library never hears of
 * it -- which is why the first attempt at this refreshed nothing and, worse,
 * left the write with no thread to land on. Naming a DIFFERENT column posts a
 * real request, and that is the whole difference.
 *
 * So it goes away and comes back, and the two trips do different jobs:
 *
 *   away   a message the queued move rides in on. Its own answer is the OLD
 *          order -- the write happens after the message it arrived with -- and
 *          the drain drops the freshly cached rows again on the way out.
 *   back   a second message, now with nothing cached, which reads the order the
 *          write left and puts the list on `#` ascending where the mode wants it.
 */
void browse_sort_refetch(void)
{
    if (!bs_g_held || !bs_g_header)
        return;
    bs_notify(bs_g_header, BS_ALT_COL);
    /* The marker has to be moved with it. bs_goto works out how many notifies
     * the trip back needs by READING the marker, and bs_notify deliberately does
     * not touch it -- so without this the header still claims `#`, bs_goto
     * concludes there is nowhere to go, and the list is left sorted by name. */
    ((void (*)(void *, int, int))FN_HDR_SET_SORTCOL)((void *)bs_g_header,
                                                    BS_ALT_COL, 1);
    bs_goto(bs_g_header, bs_g_col, 1);
    MDBG("browse: the list was fetched again -- it had been reordered under it\n");
}

int browse_sort_has_position(uintptr_t bar)
{
    uintptr_t header = bs_header(bar), col;

    if (!header)
        return 0;
    if (bs_g_held) {
        /* The header ON SCREEN has to be the one the mode borrowed from. It is
         * not enough to ask whether SOME list still has a `#`: walking back up
         * to the playlist chooser puts a different track list on screen -- the
         * preview beside the hierarchy -- while the one EDIT was turned on over
         * still exists, hidden, still carrying its column. Answering from that
         * left the plate lit over a list it would refuse to reorder.
         *
         * Only `visible` is tested here, because `sortable` is the bit the mode
         * itself takes away. */
        if (header != bs_g_header)
            return 0;
        col = bs_column_by_id(header, bs_g_col);
        return col && (bs_flags(col) & COL_VISIBLE) != 0;
    }
    col = bs_column_by_id(header, BS_POSITION_COL);
    return col && (bs_flags(col) & (COL_VISIBLE | COL_SORTABLE)) ==
                  (COL_VISIBLE | COL_SORTABLE);
}

int browse_sort_take(uintptr_t bar)
{
    uintptr_t header = bs_header(bar);

    if (bs_g_held)
        return 0;
    if (!header || !FN_HDR_GET_SORTCOL || !FN_HDR_SET_SORTCOL) {
        MDBG("browse: no live track-list header -> the sort is left alone\n");
        return -1;
    }
    bs_dump_columns(header);
    if (!browse_sort_has_position(bar)) {
        MDBG("browse: this list has no `#` -> the sort is left alone\n");
        return -1;
    }

    bs_g_was_col = (int)((int64_t (*)(void *))FN_HDR_GET_SORTCOL)((void *)header);
    bs_g_was_fwd = bs_sorted_forwards(header);
    bs_g_was_sortable = 0;
    bs_g_header = header;
    bs_g_col = BS_POSITION_COL;
    bs_g_held = 1;

    /* Order matters: move the sort FIRST, then take the flags away. bs_goto
     * needs to read the header's current column and direction, and a header
     * left un-sortable while the sort is still the DJ's would be a state
     * neither of us could get out of. */
    bs_goto(header, BS_POSITION_COL, 1);
    bs_set_sortable(header, 0);
    bs_dir_button(header, 0);
    MDBG("browse: sort borrowed -- was column %d %s, now %d ascending\n",
         bs_g_was_col, bs_g_was_fwd ? "ascending" : "descending",
         BS_POSITION_COL);
    return 0;
}

/* `resort` says whether the DJ's sort is still THEIRS to get back.
 *
 * It is when they pressed EDIT off, and it is not when the mode expired because
 * the list changed: one header serves every browse list, so re-imposing the
 * playlist's sort then lands on whatever list replaced it -- measured, leaving
 * the all-tracks view on TRACK descending because a notify naming a column that
 * list sorts by anyway flips it. The flags and the direction control always come
 * back; only the sort itself is conditional, and the deck has already set the
 * new list's own. */
void browse_sort_give_back(int resort)
{
    uintptr_t header = bs_g_header;

    if (!bs_g_held)
        return;
    bs_g_held = 0;
    bs_g_header = 0;
    if (!header)
        return;
    bs_set_sortable(header, 1);
    bs_dir_button(header, 1);
    bs_g_dirbtn = 0;
    if (!resort) {
        MDBG("browse: sort released, not restored -- it belongs to a list that "
             "is no longer on screen\n");
        return;
    }
    /* A list that had no sort of its own gets none back: setSortColumnId(0)
     * clears every column's sorted bit, which is exactly that state, and there
     * is no column to notify about. */
    if (bs_g_was_col)
        bs_goto(header, bs_g_was_col, bs_g_was_fwd);
    else
        ((void (*)(void *, int, int))FN_HDR_SET_SORTCOL)((void *)header, 0, 1);
    MDBG("browse: sort handed back -- column %d %s\n",
         bs_g_was_col, bs_g_was_fwd ? "ascending" : "descending");
}
