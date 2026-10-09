// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * browse/drag.c - carrying a row to a new place in the list.
 *
 * The gesture half of the reorder. See browse.h for the EDIT mode and the
 * offsets this reads.
 *
 * ---- moving the row ---------------------------------------------------------
 *
 * The row is moved by changing its wrapper's bounds. juce sets a child's
 * Graphics origin from its position, so everything the row draws (number,
 * title, key, waveform strip) moves with it. The translucency is a juce
 * transparency layer around the wrapper's paint (see below).
 *
 * The finger's position arrives relative to the row, which is moving, so all
 * measurements are in the parent's space: `current bounds y + event y`.
 *
 * ---- not chaining -----------------------------------------------------------
 *
 * The stock handlers scroll the list and move the selection. While a drag is
 * live they are not called, so the list does not scroll under the gesture;
 * nothing on the viewport needs to be set or restored.
 *
 * ---- failing open -----------------------------------------------------------
 *
 * Every hook falls through to stock unless a drag is live, and a drag only
 * starts on a row of the list EDIT is on. RowComp is shared by every touchable
 * table in the app, including DJ SETTINGS.
 */
#include "browse/browse.h"

static uintptr_t dg_g_row;          /* the RowComp under the finger  */
static uintptr_t dg_g_wrap;         /* its Row wrapper: what actually moves */
static uintptr_t dg_g_owner;        /* the list it came from         */
static int32_t   dg_g_orig[4];      /* where the WRAPPER sat before the drag */
static int       dg_g_from;         /* its row number, 0-based       */
static int       dg_g_insert;       /* where it would land, 0-based  */
static int       dg_g_press;        /* the finger's y at mouseDown, in the parent */
static int       dg_g_rows;         /* how many rows the model has   */
static int       dg_g_marked;       /* the mark has been drawn at least once */
static int       dg_g_settle;       /* ticks a release must survive to count */
static int       dg_g_lifted;       /* the row is off its slot, following a finger */
static uintptr_t dg_g_row_vptr;     /* what the two carried objects were, so a */
static uintptr_t dg_g_wrap_vptr;    /* deleted one is not written back into    */
static uintptr_t dg_g_view;         /* the scrolled content: the rows' parent  */
static uintptr_t dg_g_clip;         /* what shows a window onto it             */
static int       dg_g_screen;       /* the finger, in that window's space      */
static uintptr_t dg_g_stock_content_paint;
static uintptr_t dg_g_stock_down, dg_g_stock_drag, dg_g_stock_up;
static uintptr_t dg_g_stock_paint;
static uintptr_t dg_g_stock_wrap_paint, dg_g_stock_wrap_over;

/* ---- reading the row ----------------------------------------------------- */

static int dg_row_no(uintptr_t rc)
{
    int32_t v = -1;

    return mod_safe_read(rc + RC_ROW_OFF, &v, sizeof(v)) == 0 ? (int)v : -1;
}

#define DG_TI_WRAP  juce_class_of(ep122_sym(EP122_LISTBOX_ROW))

static uintptr_t dg_owner(uintptr_t rc)
{
    uintptr_t v = 0;

    mod_safe_read(rc + RC_OWNER_OFF, &v, sizeof(v));
    return v;
}

/* The model's row count, through the slot the deck reads it from. Used to
 * clamp the insertion point to the last row. */
static int dg_num_rows(uintptr_t owner)
{
    uintptr_t model = 0, vt = 0, fn = 0;

    if (mod_safe_read(owner + LB_MODEL_OFF, &model, sizeof(model)) != 0 || !model)
        return 0;
    if (mod_safe_read(model, &vt, sizeof(vt)) != 0 || !vt)
        return 0;
    if (mod_safe_read(vt + MODEL_NUMROWS, &fn, sizeof(fn)) != 0 || !fn)
        return 0;
    return (int)((int64_t (*)(void *))fn)((void *)model);
}

static int dg_event_y(void *event)
{
    float y = 0.0f;

    if (mod_safe_read((uintptr_t)event + ME_Y_OFF, &y, sizeof(y)) != 0)
        return 0;
    return (int)y;
}

/* Move the wrapper, which carries the row's position. */
static void dg_place(int y)
{
    uintptr_t fn = ep122_sym(EP122_JUCE_COMP_SETBOUNDS);

    if (fn && dg_g_wrap)
        ((void (*)(void *, int, int, int, int))fn)((void *)dg_g_wrap,
                                                   dg_g_orig[0], y,
                                                   dg_g_orig[2], dg_g_orig[3]);
}

/* ---- carrying a row past the edge of the window ---------------------------
 *
 * Without autoscroll a track could only be dropped on the ten visible rows.
 *
 * meow::TouchableViewport is not a juce::Viewport (it is a juce::Component that
 * is also a ComponentListener and a MultiTimer), so
 * juce::Viewport::setViewPosition does not apply. The scroll offset is the
 * content component's own y. For an 18-row playlist scrolled to row 6:
 *
 *   Row (wrapper)     {0,240,1188,48}      row 5 * 48, so wrapper y is content
 *   ViewedComponent   {0,-240,1188,864}    18 * 48 tall, offset by five rows
 *   juce::Component   {0,0,1188,480}       the window, ten rows
 *
 * The gesture measures travel in content coordinates, which do not change when
 * the list scrolls, so the row under the finger needs no correction.
 *
 * Moving the content is what the viewport itself does; it listens to its
 * content (the ComponentListener base), so the deck recycles the rows onto
 * their new positions. */
#define DG_EDGE_PX   44     /* how near an edge starts it: about one row */
#define DG_SCROLL_PX  9     /* per display tick, so ~7 rows a second */

/* Scroll `px` further down the list, or up when negative. Returns how far the
 * content actually moved, which is 0 at either end. */
static int dg_scroll_by(int px)
{
    uintptr_t fn = ep122_sym(EP122_JUCE_COMP_SETBOUNDS);
    int32_t v[4], c[4];
    int want, least;

    if (!fn || !dg_g_view || !dg_g_clip ||
        juce_comp_bounds(dg_g_view, v) != 0 ||
        juce_comp_bounds(dg_g_clip, c) != 0)
        return 0;
    /* Scrolling down moves the content up (negative y), at most to the last
     * window, and not at all when everything fits. */
    least = c[3] - v[3];
    if (least > 0)
        least = 0;
    want = v[1] - px;
    if (want > 0)
        want = 0;
    if (want < least)
        want = least;
    if (want == v[1])
        return 0;
    ((void (*)(void *, int, int, int, int))fn)((void *)dg_g_view,
                                               v[0], want, v[2], v[3]);
    return want - v[1];
}

/* Restore the carried row's own track after a scroll.
 *
 * The list hands out components as `components[row % N]`, so scrolling two rows
 * can reassign the carried component (e.g. the one carrying row 0 is redrawn as
 * row 12 of 18). The `#` column follows the RowComp's row number,
 * but the title is in a child filled by refreshComponentForRow, so the carried
 * row would show a different track.
 *
 * So the row is re-asserted through the deck's refresh, with the same three
 * arguments updateVisibleArea uses. Called right after each scroll step, the
 * only time the list reassigns: the content's move notifies the viewport
 * synchronously, so this runs before anything paints. */
static void dg_reclaim(void)
{
    uintptr_t owner = 0, model = 0, vt = 0, fn = 0, child = 0, back;
    int32_t row = -1;
    uint8_t sel = 0;

    if (!dg_g_wrap || dg_g_from < 0)
        return;
    if (mod_safe_read(dg_g_wrap + ROW_INDEX_OFF, &row, sizeof(row)) != 0 ||
        row == dg_g_from)
        return;
    if (mod_safe_read(dg_g_wrap + ROW_OWNER_OFF, &owner, sizeof(owner)) != 0 ||
        !owner ||
        mod_safe_read(owner + LB_ROW_MODEL, &model, sizeof(model)) != 0 ||
        !model ||
        mod_safe_read(model, &vt, sizeof(vt)) != 0 || !vt ||
        mod_safe_read(vt + MODEL_REFRESH, &fn, sizeof(fn)) != 0 || !fn ||
        mod_safe_read(dg_g_wrap + ROW_CHILD_OFF, &child, sizeof(child)) != 0)
        return;

    row = dg_g_from;
    mod_safe_write(dg_g_wrap + ROW_INDEX_OFF, &row, sizeof(row));
    mod_safe_write(dg_g_wrap + ROW_SEL_OFF, &sel, sizeof(sel));
    back = ((uintptr_t (*)(void *, int, int, void *))fn)((void *)model,
                                                         dg_g_from, 0,
                                                         (void *)child);
    /* Only the case where it fills and returns the given component is handled;
     * a new component would need adopting, which the list does itself on the
     * next scroll step. */
    if (back && back != child)
        MDBG("browse: the row refresh returned a different component -- the "
             "carried row keeps the one it had\n");
}

/* Repaint the whole list: the gap, the row with the insertion mark and the
 * carried row do not share an immediate parent. */
static void dg_repaint(void)
{
    uintptr_t fn = ep122_sym(EP122_JUCE_COMP_REPAINT);

    if (fn && dg_g_owner)
        ((void (*)(void *))fn)((void *)dg_g_owner);
}

/* Log the row's ancestry once per drag: which components hold it, their bounds
 * and child counts. */
static void dg_chain(uintptr_t rc)
{
    char name[96];
    uintptr_t c = rc;
    int i;

    for (i = 0; i < 5 && c; i++) {
        int32_t b[4] = { 0, 0, 0, 0 };

        juce_comp_bounds(c, b);
        MDBG("browse:   %s%p %s {%d,%d,%d,%d} children=%d\n",
             i ? "  ^ " : "row ", (void *)c,
             juce_comp_class_name(c, name, sizeof(name)),
             (int)b[0], (int)b[1], (int)b[2], (int)b[3], juce_comp_nchild(c));
        c = juce_comp_parent(c);
    }
}

/* Move the component to the end of its parent's child array, which is juce's
 * z-order; otherwise a row dragged down is painted under the rows after it.
 *
 * Done by moving the pointer, as toFront does, on the message thread that owns
 * the array; re-adding the child would detach it mid-gesture with the mouse
 * grab on it. The order is never restored; rows in their slots do not
 * overlap. */
static void dg_to_front(uintptr_t rc)
{
    uintptr_t p = juce_comp_parent(rc), arr = 0, cur = 0;
    int32_t n = 0;
    int i, at = -1;

    if (!p || mod_safe_read(p + JUCE_CHILDREN_OFF, &arr, sizeof(arr)) != 0 || !arr)
        return;
    if (mod_safe_read(p + JUCE_NCHILD_OFF, &n, sizeof(n)) != 0 || n <= 1 || n > 256)
        return;
    for (i = 0; i < n; i++)
        if (mod_safe_read(arr + (size_t)i * sizeof(cur), &cur, sizeof(cur)) == 0 &&
            cur == rc) {
            at = i;
            break;
        }
    if (at < 0 || at == n - 1)
        return;
    for (i = at; i < n - 1; i++) {
        if (mod_safe_read(arr + (size_t)(i + 1) * sizeof(cur), &cur, sizeof(cur)) != 0)
            return;
        mod_safe_write(arr + (size_t)i * sizeof(cur), &cur, sizeof(cur));
    }
    mod_safe_write(arr + (size_t)(n - 1) * sizeof(rc), &rc, sizeof(rc));
}

/* ---- the gesture --------------------------------------------------------- */

/* Whether the two carried objects are still the ones grabbed.
 *
 * The list can rebuild under a drag (media eject, view change, a recycled
 * RowComp). Their vtable pointers are checked, as the EDIT plate checks its
 * own. This guards every write back to the row: writing to a freed component
 * could crash the deck. */
static int dg_alive(void)
{
    uintptr_t v = 0;

    return dg_g_row && dg_g_wrap &&
           mod_safe_read(dg_g_row, &v, sizeof(v)) == 0 && v == dg_g_row_vptr &&
           mod_safe_read(dg_g_wrap, &v, sizeof(v)) == 0 && v == dg_g_wrap_vptr;
}

static void dg_end(void)
{
    if (dg_alive()) {
        dg_place(dg_g_orig[1]);
        dg_repaint();
    }
    dg_g_row = dg_g_wrap = dg_g_owner = 0;
    dg_g_view = dg_g_clip = 0;
    dg_g_row_vptr = dg_g_wrap_vptr = 0;
    dg_g_from = dg_g_insert = -1;
}

/* Defined below with the drag; a press needs them because a fast drag arrives
 * partly as presses. */
static int  dg_finger(uintptr_t rc, void *event);
static void dg_carry_to(int finger);

/* Log why a press did not start a drag, once per distinct reason, so a refused
 * press is distinguishable from a missing hook. */
static void dg_refuse(const char *why)
{
    static const char *last;

    if (why == last)
        return;
    last = why;
    MDBG("browse: a press on a row did not start a drag: %s\n", why);
}

/* Whether this row is inside the list EDIT is on. RowComp belongs to every
 * touchable table in the app, so without this the browse sidebar and DJ
 * SETTINGS would be draggable too (a tap on the sidebar's TRACK tab became a
 * drag). Depth-bounded; a row is five deep in its list. */
static int dg_in_edit_list(uintptr_t rc)
{
    uintptr_t list = browse_edit_list(), c = rc;
    int i;

    if (!list)
        return 0;
    for (i = 0; i < 12 && c; i++, c = juce_comp_parent(c))
        if (c == list)
            return 1;
    return 0;
}

static void dg_mousedown(void *self, void *event)
{
    uintptr_t rc = (uintptr_t)self, wrap;
    int32_t b[4], wb[4];

    if (!browse_edit_on()) {
        dg_refuse("EDIT is off");
        ((void (*)(void *, void *))dg_g_stock_down)(self, event);
        return;
    }
    /* A press while a row is still carried continues the same gesture. On a
     * fast drag the deck's touch layer treats a large jump between samples as
     * a lift and a new press; treating it as a new gesture would grab whatever
     * row is under the finger. It is handled as movement.
     *
     * Not chained to stock, so the list does not scroll or reselect. */
    if (dg_g_row) {
        int finger = dg_alive() ? dg_finger(rc, event) : DG_NO_FINGER;

        if (finger != DG_NO_FINGER && dg_in_edit_list(rc)) {
            if (!dg_g_lifted) {
                MDBG("browse: the finger came back -- row %d is still the one "
                     "being carried\n", dg_g_from);
                dg_g_lifted = 1;
                dg_to_front(dg_g_wrap);
            }
            /* dg_g_press is kept: travel is measured from the original
             * press, so an interrupted gesture resumes. */
            dg_g_settle = 0;
            dg_carry_to(finger);
            return;
        }
        MDBG("browse: a press somewhere else while row %d was carried -- "
             "putting it back\n", dg_g_from);
        dg_end();
    }
    if (!dg_in_edit_list(rc)) {
        dg_refuse("the row is not in the list EDIT is on");
        ((void (*)(void *, void *))dg_g_stock_down)(self, event);
        return;
    }
    if (juce_comp_bounds(rc, b) != 0 || b[3] <= 0 ||
        (dg_g_from = dg_row_no(rc)) < 0) {
        dg_refuse("the row has no bounds or no row number");
        ((void (*)(void *, void *))dg_g_stock_down)(self, event);
        return;
    }
    /* Check the wrapper's class, so a row built into something else does not
     * move an unrelated parent. */
    wrap = juce_comp_parent(rc);
    if (!wrap || juce_comp_class(wrap) != DG_TI_WRAP ||
        juce_comp_bounds(wrap, wb) != 0) {
        dg_refuse("its parent is not a TouchableListBox::Row");
        ((void (*)(void *, void *))dg_g_stock_down)(self, event);
        return;
    }
    dg_g_owner = dg_owner(rc);
    dg_g_rows = dg_num_rows(dg_g_owner);
    if (dg_g_rows <= 1 || dg_g_from >= dg_g_rows) {
        /* One row cannot be reordered, and a row number beyond the model's
         * count is not understood. Stock either way. */
        dg_refuse(dg_g_rows <= 1 ? "the model reports no rows"
                                 : "the model does not claim that row");
        dg_g_owner = 0;
        ((void (*)(void *, void *))dg_g_stock_down)(self, event);
        return;
    }

    dg_g_row = rc;
    dg_g_wrap = wrap;
    /* The rows' parent is the scrolled content, and its parent is the clipping
     * window. Read once here; if missing, the drag does not autoscroll. */
    dg_g_view = juce_comp_parent(wrap);
    dg_g_clip = dg_g_view ? juce_comp_parent(dg_g_view) : 0;
    dg_g_orig[0] = wb[0]; dg_g_orig[1] = wb[1];
    dg_g_orig[2] = wb[2]; dg_g_orig[3] = wb[3];
    dg_g_insert = dg_g_from;
    /* The finger in the list's space. The RowComp fills the wrapper at {0,0},
     * so add the wrapper's y to the event y. */
    dg_g_press = wb[1] + dg_event_y(event);
    (void)mod_safe_read(rc, &dg_g_row_vptr, sizeof(dg_g_row_vptr));
    (void)mod_safe_read(wrap, &dg_g_wrap_vptr, sizeof(dg_g_wrap_vptr));
    dg_g_lifted = 1;
    dg_to_front(wrap);
    dg_repaint();
    dg_chain(rc);
    MDBG("browse: carrying row %d of %d, wrapper at {%d,%d,%d,%d}, finger %d\n",
         dg_g_from, dg_g_rows, wb[0], wb[1], wb[2], wb[3], dg_g_press);
}

/* The finger in the list's space, from an event delivered to any row (a fast
 * drag does not always stay on one). A RowComp fills its wrapper at {0,0}, so
 * this is the wrapper's y plus the event's y. */
static int dg_finger(uintptr_t rc, void *event)
{
    uintptr_t wrap = juce_comp_parent(rc);
    int32_t wb[4];

    if (!wrap || juce_comp_bounds(wrap, wb) != 0)
        return DG_NO_FINGER;
    return wb[1] + dg_event_y(event);
}

/* Carry the row to a finger position. Shared by mouseDrag and mouseDown, since
 * a fast drag arrives partly as presses. */
static void dg_carry_to(int finger)
{
    int y, dy, insert, h = dg_g_orig[3];
    int32_t v[4];

    /* The finger's position in the window, for the autoscroll tick, which has
     * no event to read. `finger` is in content coordinates. */
    if (dg_g_view && juce_comp_bounds(dg_g_view, v) == 0)
        dg_g_screen = finger + v[1];

    dy = finger - dg_g_press;

    /* Use travel, not absolute position: a row's y is not `index * height`
     * (row 1 of this list reports y=0). Rounded to the nearest row, so the
     * mark moves after half a row of travel. */
    insert = dg_g_from + (dy >= 0 ? (dy + h / 2) / h : -((-dy + h / 2) / h));
    if (insert < 0)
        insert = 0;
    if (insert > dg_g_rows - 1)
        insert = dg_g_rows - 1;

    /* Clamp to the existing slots. The content component is exactly as tall
     * as the rows, and juce clips children to it, so a row carried past the
     * last slot would be cut off and vanish.
     *
     * Clamped by travel, like the insertion index, so every position the row
     * can reach is one the mark can show. */
    if (dy < -dg_g_from * h)
        dy = -dg_g_from * h;
    if (dy > (dg_g_rows - 1 - dg_g_from) * h)
        dy = (dg_g_rows - 1 - dg_g_from) * h;
    y = dg_g_orig[1] + dy;

    dg_place(y);
    if (insert != dg_g_insert)
        MDBG("browse: over row %d (moved %d of %d px rows)\n", insert, dy, h);
    dg_g_insert = insert;
    dg_repaint();
}

/* Accepts drags on any row of the edit list, not only the carried one: after a
 * synthesised release-and-press the drags arrive at the RowComp under the
 * finger, and sending them to stock would scroll the list. */
static void dg_mousedrag(void *self, void *event)
{
    uintptr_t rc = (uintptr_t)self;
    int finger;

    if (!dg_g_row || !dg_alive() || !dg_in_edit_list(rc)) {
        ((void (*)(void *, void *))dg_g_stock_drag)(self, event);
        return;
    }
    /* The list recycles its dozen RowComps while scrolling, which is normal
     * during an autoscrolled drag. The drag's state (start row and insertion
     * point, in content coordinates) is held here, not in the component, and
     * dg_reclaim restores what the component paints. dg_alive guards against
     * a deleted component. */
    dg_g_settle = 0;
    finger = dg_finger(rc, event);
    if (finger != DG_NO_FINGER)
        dg_carry_to(finger);
}

/* The drop, once the release has settled. Called from the tick, not mouseUp;
 * see dg_mouseup. */
static void dg_commit(void)
{
    uint32_t pid;
    int from = dg_g_from, to = dg_g_insert;

    dg_end();
    if (!dg_g_marked)
        MDBG("browse: the insertion mark never drew -- the rows are not being "
             "repainted, or the row number is not where it is read from\n");
    dg_g_marked = 0;
    if (from == to) {
        MDBG("browse: row %d put back where it was\n", from);
        return;
    }
    /* 1-based, as shown in the `#` column and stored in
     * DJDBSONGPLAYLIST.TRACKNO.
     *
     * Async because the message thread has no djdb context; the write runs on
     * the next library message, like a held tempo. The playlist id comes from
     * the cache this list is served from. */
    pid = mod_djdb_playlist_now();
    if (!pid) {
        /* Refused: guessing could reorder a playlist that is not on screen. */
        MWARN("browse: #%d -> #%d NOT written -- no playlist is known for this "
             "list\n", from + 1, to + 1);
        return;
    }
    MDBG("browse: MOVE #%d -> #%d in playlist %u\n", from + 1, to + 1,
         (unsigned)pid);
    /* dg_g_rows is the row count on screen, an independent check on `pid`.
     * See db.h. */
    if (mod_djdb_move_track_async(pid, from + 1, to + 1, dg_g_rows) != 0)
        return;
    /* Drop the deck's cached rows so the next fill reads the new order, then
     * request the list again, which also gives the write its thread; see
     * browse_sort_refetch. */
    mod_djdb_drop_list_cache(pid);
    browse_sort_refetch();
}

/* A release only counts once it has lasted DG_SETTLE_TICKS (44 Hz ticks). On
 * a fast move the deck's touch layer emits a release and a press although the
 * panel reports one unbroken touch. Taken literally, the drag would end and
 * grab whatever row the finger had reached. A press inside the window cancels
 * the release and the drag continues. */
/* The deck drops the touch on a fast move and re-acquires it up to
 * 14 ticks (~320 ms) later; its release in between looks like a real lift.
 *
 * 20 gives margin. The cost: a deliberate lift and new press within ~450 ms on
 * the same list resumes the old drag instead of starting a new one. */
#define DG_SETTLE_TICKS 20

static void dg_mouseup(void *self, void *event)
{
    if (!dg_g_row || !dg_in_edit_list((uintptr_t)self)) {
        ((void (*)(void *, void *))dg_g_stock_up)(self, event);
        return;
    }
    /* The row returns immediately; only the commit waits for the settle, so
     * normal drops do not hang. A press inside the window lifts the same row
     * again and the gesture continues. */
    dg_place(dg_g_orig[1]);
    dg_repaint();
    dg_g_lifted = 0;
    dg_g_settle = DG_SETTLE_TICKS;
}

/* ---- what the drag looks like -------------------------------------------- */

/* No selection plate while EDIT is on: in EDIT the blue plate would look like
 * it marks the row being moved.
 *
 * Hidden in paint, not by deselecting: the deck uses the selection for LOAD and
 * for where the rotary resumes. The byte is cleared only for the duration of
 * the stock paint. */
static void dg_paint(void *self, void *g)
{
    uintptr_t rc = (uintptr_t)self;
    int32_t b[4];
    uint8_t was = 0, off = 0;
    int hidden = 0;

    if (browse_edit_on() && dg_in_edit_list(rc) &&
        mod_safe_read(rc + RC_SELECTED_OFF, &was, sizeof(was)) == 0 && was) {
        mod_safe_write(rc + RC_SELECTED_OFF, &off, sizeof(off));
        hidden = 1;
    }
    ((void (*)(void *, void *))dg_g_stock_paint)(self, g);
    if (hidden)
        mod_safe_write(rc + RC_SELECTED_OFF, &was, sizeof(was));
    if (!dg_g_lifted || juce_comp_bounds(rc, b) != 0)
        return;

    mod_draw_enter();
    if (rc != dg_g_row &&
        dg_row_no(rc) == dg_g_insert && dg_g_insert != dg_g_from) {
        dg_g_marked = 1;
        /* The edge depends on direction. A drag down to row k lands after k
         * (#1 to #8 puts the row after the 8th), so the mark is on k's bottom
         * edge; a drag up lands before k, so it is on the top edge. This is
         * the only way to show "last".
         *
         * Drawn by the row, so it stays aligned while the rows scroll.
         *
         * In the list's selected-row green, through mod_colour_stock so it
         * stays visible under the WHITE theme. */
        mod_gfx_colour(g, mod_colour_stock(DG_MARK_COL));
        mod_gfx_fill(g, 0, dg_g_insert > dg_g_from ? b[3] - DG_LINE_H : 0,
                     b[2], DG_LINE_H);
    }
    mod_draw_leave();
}

/* ---- the carried row's translucency ---------------------------------------
 *
 * A juce transparency layer, not a dark wash: the row is rendered into its own
 * buffer and blended over the screen, so the rows it crosses show through.
 *
 * Opened in the wrapper's paint (before its children) and closed in its
 * paintOverChildren (after), so the layer covers the RowComp and its two
 * children, the waveform strip and the artwork dot.
 *
 * The two slots are juce::LowLevelGraphicsContext's +0x80 and +0x88, from the
 * software renderer's vtable: one allocates a buffer and stores the opacity,
 * the other pops it and blends the buffer back at that opacity.
 * juce::Graphics' first member is the context. */
#define GFX_VT_LAYER_BEGIN  0x80
#define GFX_VT_LAYER_END    0x88

static uintptr_t dg_ctx_slot(void *g, unsigned off, uintptr_t *ctx_out)
{
    uintptr_t ctx = 0, vt = 0, fn = 0;

    if (mod_safe_read((uintptr_t)g, &ctx, sizeof(ctx)) != 0 || !ctx)
        return 0;
    if (mod_safe_read(ctx, &vt, sizeof(vt)) != 0 || !vt)
        return 0;
    if (mod_safe_read(vt + off, &fn, sizeof(fn)) != 0 || !fn)
        return 0;
    *ctx_out = ctx;
    return fn;
}

/* Set when a layer was opened, so paintOverChildren only closes one that
 * exists. */
static int dg_g_layered;

static void dg_wrap_paint(void *self, void *g)
{
    uintptr_t ctx = 0, fn;

    ((void (*)(void *, void *))dg_g_stock_wrap_paint)(self, g);
    dg_g_layered = 0;
    if (!dg_g_lifted || (uintptr_t)self != dg_g_wrap)
        return;
    fn = dg_ctx_slot(g, GFX_VT_LAYER_BEGIN, &ctx);
    if (!fn)
        return;
    ((void (*)(void *, float))fn)((void *)ctx, DG_GHOST_ALPHA);
    dg_g_layered = 1;
}

static void dg_wrap_paint_over(void *self, void *g)
{
    uintptr_t ctx = 0, fn;

    if ((uintptr_t)self == dg_g_wrap && dg_g_layered) {
        dg_g_layered = 0;
        fn = dg_ctx_slot(g, GFX_VT_LAYER_END, &ctx);
        if (fn)
            ((void (*)(void *))fn)((void *)ctx);
    }
    ((void (*)(void *, void *))dg_g_stock_wrap_over)(self, g);
}

/* The gap. Drawn by the content component, whose paint runs before its
 * children, so it only shows where the carried row used to be. */
static void dg_content_paint(void *self, void *g)
{
    ((void (*)(void *, void *))dg_g_stock_content_paint)(self, g);
    if (!dg_g_lifted || (uintptr_t)self != juce_comp_parent(dg_g_wrap))
        return;
    /* A deck colour passed through the theme transform (mod_colour_stock), not
     * a theme role, so it matches the list under WHITE too. */
    mod_gfx_colour(g, mod_colour_stock(DG_HOLE_COL));
    mod_gfx_fill(g, dg_g_orig[0], dg_g_orig[1], dg_g_orig[2], dg_g_orig[3]);
}

/* browse_drag_tick has no idle timeout: juce guarantees exactly one mouseUp, so
 * a drag with no events is a finger held still. It only checks that the row
 * still exists, since the list can rebuild under a drag. */
/* Autoscroll runs on the tick because a finger held at an edge sends no drag
 * events. The carried row is then re-placed from the finger's unchanged window
 * position, which is now over different content. */
static void dg_autoscroll(void)
{
    int32_t c[4], v[4];
    int px = 0;

    if (!dg_g_lifted || !dg_g_clip || juce_comp_bounds(dg_g_clip, c) != 0)
        return;
    if (dg_g_screen < DG_EDGE_PX)
        px = -DG_SCROLL_PX;
    else if (dg_g_screen > c[3] - DG_EDGE_PX)
        px = DG_SCROLL_PX;
    if (!px || !dg_scroll_by(px) || juce_comp_bounds(dg_g_view, v) != 0)
        return;
    dg_reclaim();
    dg_carry_to(dg_g_screen - v[1]);
}

void browse_drag_tick(void)
{
    if (dg_g_row && dg_g_lifted && dg_alive())
        dg_autoscroll();
    if (dg_g_row && dg_g_settle && dg_alive() && --dg_g_settle == 0) {
        dg_commit();
        return;
    }
    if (!dg_g_row || dg_alive())
        return;
    dg_g_settle = 0;
    MDBG("browse: the list rebuilt under the drag -- letting row %d go\n",
         dg_g_from);
    dg_g_row = dg_g_wrap = dg_g_owner = 0;
    dg_g_view = dg_g_clip = 0;
    dg_g_row_vptr = dg_g_wrap_vptr = 0;
    dg_g_from = dg_g_insert = -1;
}

/* ---- install ------------------------------------------------------------- */

int browse_drag_install(void)
{
    if (!ep122_sym(EP122_JUCE_COMP_SETBOUNDS) ||
        !ep122_sym(EP122_JUCE_COMP_REPAINT) ||
        !MOD_FN_GFX_SETCOLOUR || !MOD_FN_GFX_FILLRECT) {
        MDBG("browse: juce primitives did not resolve -> no reorder gesture\n");
        return -1;
    }
    /* All or none: a mouseDown hook without its mouseUp would leave a row
     * stranded mid-list. */
    if (mod_patch_vslot("rowDown", EP122_ROWCOMP, JUCE_VT_MOUSEDOWN,
                        (void *)dg_mousedown, &dg_g_stock_down) != 0 ||
        mod_patch_vslot("rowDrag", EP122_ROWCOMP, JUCE_VT_MOUSEDRAG,
                        (void *)dg_mousedrag, &dg_g_stock_drag) != 0 ||
        mod_patch_vslot("rowUp", EP122_ROWCOMP, JUCE_VT_MOUSEUP,
                        (void *)dg_mouseup, &dg_g_stock_up) != 0 ||
        mod_patch_vslot("rowPaint", EP122_ROWCOMP, JUCE_VT_PAINT,
                        (void *)dg_paint, &dg_g_stock_paint) != 0 ||
        mod_patch_vslot("rowWrapPaint", EP122_LISTBOX_ROW, JUCE_VT_PAINT,
                        (void *)dg_wrap_paint, &dg_g_stock_wrap_paint) != 0 ||
        mod_patch_vslot("rowWrapOver", EP122_LISTBOX_ROW, JUCE_VT_PAINTOVER,
                        (void *)dg_wrap_paint_over, &dg_g_stock_wrap_over) != 0 ||
        mod_patch_vslot("rowContent", EP122_VIEWED_COMP, JUCE_VT_PAINT,
                        (void *)dg_content_paint,
                        &dg_g_stock_content_paint) != 0) {
        MDBG("browse: could not take the row's touch -> no reorder gesture\n");
        return -1;
    }
    dg_g_from = dg_g_insert = -1;
    return 0;
}
