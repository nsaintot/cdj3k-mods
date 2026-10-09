// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * browse/sort.c - the sort EDIT borrows and restores.
 *
 * See browse.h for why EDIT takes the sort. This file finds the live header,
 * saves the DJ's sort, forces `#` ascending, and restores it.
 *
 * ---- what a juce::TableHeaderComponent holds -------------------------------
 *
 * gui::TrackListHeader is one, with mouseDown as its only override, so this
 * layout is stock juce, as getSortColumnId and setSortColumnId use it:
 *
 *   header + 0xd8   ColumnInfo**   the columns, as pointers
 *   header + 0xe8   int            how many
 *   column + 0x00   juce::String   the name (reads back empty although the
 *                                  columns are named; see BS_POSITION_COL)
 *   column + 0x08   int            id
 *   column + 0x0c   int            propertyFlags
 *
 * The relevant juce flags: sortable 0x10, sortedForwards 0x20, sortedBackwards
 * 0x40. getSortColumnId returns the column carrying either sorted bit, which is
 * also how the direction is read here.
 */
#include "browse/browse.h"

#define HDR_COLUMNS_OFF   0xd8
#define HDR_NCOLUMN_OFF   0xe8
/* gui::TrackListHeader's own listeners: gui::TrackListHeaderListener, one
 * virtual taking the column id. Its mouseDown walks this pair. */
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

/* What was borrowed, and from which header. A separate `held` flag because a
 * list with no sort reports column 0, which must also be restored. */
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

/* Depth-first search for a visible component of that class. The browse view
 * has two gui::TrackListWidgets (full-width and beside a hierarchy) and shows
 * one, so the first of the class is often the wrong one. Depth-bounded. */
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

/* The track list on screen. Not cached, since which of the two lists shows
 * changes between presses. */
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
    /* The header is a visible child of the list; searching inside this list
     * cannot find the other list's header. */
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

/* `#` is column id 3. gui::TrackListHeader::TrackListHeader(
 * gui::TrackListType) names its seven columns as it adds them:
 *
 *   id 1 "PREVIEW"  id 2 ""  id 3 "#"  id 4 "TRACK"  id 5 ""  id 6 "BPM"  id 7 "KEY"
 *
 * with flags 1 on the first two and 0x11 (visible|sortable) on 3..7.
 *
 * A list with no positions (all tracks, an artist's tracks) has id 3's visible
 * bit cleared by gui::TrackListWidget's show/hide. The header
 * object is shared, so `#` being visible describes the current list.
 *
 * The deck also rewrites the sortable bit: gui::TrackListHeader::setSortEnabled
 * sets ids 3..7 to `visible ? 0x11 : 0x10`, so keeping the header
 * disabled requires polling. */
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

/* Tell the header's listeners a column was chosen; this is what re-sorts the
 * rows.
 *
 * setSortColumnId only moves juce's marker: it clears the sorted bits, sets
 * one, repaints and triggers juce's async update. The deck re-sorts by
 * re-querying the library: gui::TrackListWidget maps the column id to a sort
 * kind and passes it to TrackListDisplayFormatEventFacade::sort(), which fetches
 * the list again. Without the notify the header shows `#` while the rows keep
 * the old order.
 *
 * The deck's mouseDown marks first, then notifies, because the listener reads
 * the direction from the header. */
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

/* Sort the list by (column, direction) from any current state.
 *
 * A notify naming the current sort column flips the direction (the header's
 * double-tap behaviour); naming another column sorts it ascending. The marker
 * does not affect this: setting it to `forwards` before notifying does not
 * prevent the flip.
 *
 * So the number of notifies is computed, and the marker is set last to the
 * resulting state. */
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

/* The header's ascending/descending control, a plain TogglesImageButton
 * child. Clearing the sortable bits does not affect it, so it is hidden and
 * shown explicitly. */
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

/* Ends the mode once `#` is no longer on screen: a list with no `#` has no
 * position to move a row to. The header keeps its seven columns across view
 * changes and toggles their `visible` bit, so this is one flag read.
 *
 * Do not switch to watching for a gui::TrackListWidget change: that missed the
 * switch to the all-tracks view.
 *
 * Tests `visible`, not `sortable`, because this mode clears sortable. */
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

/* The temporary column for a refetch: any one other than `#`. TRACK (id 4) is
 * always added by the header's constructor. bs_notify ignores the cleared
 * sortable bits, as the header's mouseDown does. */
#define BS_ALT_COL  4

/* Make the deck fetch the list again after a reorder.
 *
 * A sort-kind change is a library message; a direction flip is not.
 * Re-naming the current sort column is answered on the UI thread from the rows
 * the widget holds, so nothing refreshes and the queued write gets no thread.
 * Naming a different column posts a real request.
 *
 * So it sorts away and back:
 *
 *   away   a message that runs the queued move. Its own answer is the old order
 *          (the write runs after it), and the drain drops the newly cached rows.
 *   back   a second message, with nothing cached, which reads the new order and
 *          leaves the list on `#` ascending.
 */
void browse_sort_refetch(void)
{
    if (!bs_g_held || !bs_g_header)
        return;
    bs_notify(bs_g_header, BS_ALT_COL);
    /* Move the marker too: bs_goto reads it to count notifies, and bs_notify
     * does not set it. Otherwise bs_goto sees `#` already selected and the list
     * stays sorted by name. */
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
        /* The header on screen must be the one the mode borrowed from. Going
         * back up to the playlist chooser shows a different track list (the
         * preview beside the hierarchy) while the original, now hidden, still
         * has its `#` column.
         *
         * Only `visible` is tested, because this mode clears `sortable`. */
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

    /* Move the sort before clearing the flags: bs_goto reads the header's
     * current column and direction, and an unsortable header still on the
     * DJ's sort could not be recovered. */
    bs_goto(header, BS_POSITION_COL, 1);
    bs_set_sortable(header, 0);
    bs_dir_button(header, 0);
    MDBG("browse: sort borrowed -- was column %d %s, now %d ascending\n",
         bs_g_was_col, bs_g_was_fwd ? "ascending" : "descending",
         BS_POSITION_COL);
    return 0;
}

/* `resort` says whether to restore the DJ's sort: yes when EDIT was turned off,
 * no when the mode expired because the list changed. One header serves every
 * browse list, so restoring then would hit the replacement list (the
 * all-tracks view would end on TRACK descending, because notifying its current
 * column flips it). The flags and the direction control are always restored;
 * the deck has already set the new list's sort. */
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
    /* A list that had no sort gets none back: setSortColumnId(0) clears every
     * column's sorted bit, and there is no column to notify. */
    if (bs_g_was_col)
        bs_goto(header, bs_g_was_col, bs_g_was_fwd);
    else
        ((void (*)(void *, int, int))FN_HDR_SET_SORTCOL)((void *)header, 0, 1);
    MDBG("browse: sort handed back -- column %d %s\n",
         bs_g_was_col, bs_g_was_fwd ? "ascending" : "descending");
}
