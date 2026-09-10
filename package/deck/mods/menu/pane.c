// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * menu/pane.c - the right pane, borrowed from DJSettingRightPaneTableModel.
 *
 * Its model, geometry and radio are the deck's. Ours is the answer to how many
 * values a row has and which one is checked, so most of this overrides a count
 * or a label and hands the drawing back.
 */
#include "menu/internal.h"

/* Apply a right-pane value tap ourselves: the pane belongs to the mod row, so this
 * flips the feature instead of writing a stock DJ setting, then refreshes both the
 * centre value column and the radio. */
/* Apply a chosen value. `focus_pane` is for a touch tap, which never reaches the stock
 * focus-advance handler; on the rotary path the stock dispatch owns the focus level. */
void menu_apply_value(int row, int focus_pane, const char *src)
{
    uintptr_t list, rlist;
    int32_t focus = FOCUS_OPTION_PANE;

    const struct kit_row *r = menu_row(menu_g_setting_row);
    int *state = r ? r->state : NULL;

    if (!state) return;                                /* pane isn't ours */
    if (row < 0 || row >= menu_pane_rows(r)) return;    /* not a row the pane has */
    if (row != menu_row_index(r)) {
        *state = row;
        MDBG("%s -> %s (%s)\n", r->label, menu_row_value(r, *state), src);
        /* A committed value may change anything on screen -- a theme is resolved
         * at draw time -- so the whole view is repainted rather than only the two
         * lists below, which would leave the surrounding chrome trailing until
         * something else invalidated it. */
        if (menu_g_view)
            ((void (*)(void *))FN_REPAINT)((void *)menu_g_view);
        /* Before the save: a feature that rejects the value clears it here, and
         * the cleared value is then what is persisted. */
        if (r->changed) r->changed();
        mods_settings_save();          /* survive an app restart */
    }
    if (focus_pane)
        mod_safe_write(menu_g_view + VIEW_FOCUS_OFF, &focus, sizeof(focus));
    list  = menu_view_ptr(menu_g_view, VIEW_DJLIST_OFF);
    rlist = menu_view_ptr(menu_g_view, VIEW_RIGHT_LIST_OFF);
    if (list)  ((void (*)(void *))FN_REPAINT)((void *)list);   /* centre value */
    if (rlist) {
        /* The dot colour is written onto each row's component inside
         * refreshComponentForCell, so a plain repaint keeps the colours the rows already
         * hold and the dot stays on the previous value. Re-run it by nudging the SELECTION:
         * JUCE refreshes a row whenever its selected state changes (that is how isSelected
         * reaches the model), and it does exactly this itself during mouseDown -- so it is
         * safe from inside a click, unlike updateContent, which destroys and rebuilds the
         * rows while the click is still being dispatched. That re-entrancy is what made
         * touch work or not depending on timing. Guarded: not a user choice. */
        menu_g_rsel_guard = 1;
        ((selectrow_t)FN_SELECT_ROW)((void *)rlist, row ? 0 : 1, 1, 1);
        ((selectrow_t)FN_SELECT_ROW)((void *)rlist, row, 1, 1);
        menu_g_rsel_guard = 0;
        ((void (*)(void *))FN_REPAINT)((void *)rlist);
    }
}

/* A real tap on a value row. This -- not the selection change -- is the touch signal:
 * a tap makes the app re-select the PREVIOUS value row straight afterwards, so adopting
 * from selection applied the tap and then immediately undid it. */
void menu_rcellclicked(void *self, int row, int col, void *event)
{
    if (!menu_g_mod_mode) {
        ((cellclick_t)menu_g_orig_rcellclick)(self, row, col, event);
        return;
    }
    menu_apply_value(row, 1, "tap");    /* never call stock: it would write the DJ setting */
}

/* Build the mod row's value pane: borrow the OFF/ON option set for native strings and
 * styling, then disown it so a tap can't write the stock setting, and highlight the row
 * matching the gate state. Re-assertable, because several stock paths rebuild the pane
 * from the row index behind our back. */
/* Value of the model+0x10 field for the current gate state. refreshComponentForCell picks
 * the dot's colour as (row == this field) ? model+0x18 : model+0x20, and sub_14c01a8 paints
 * it onto the dot component. Confirmed by sampling the glyph centre in the framebuffer:
 * with the field at 0, row 0's centre was 007de1 (filled) and row 1's 191919 (empty). So
 * the matching row is the filled one, i.e. this is simply the value's own row.
 * (Do not sample the row background to judge this -- the selected row is blue, which
 * looks identical to a filled dot in a coarse pixel scan and inverted this twice.) */
int32_t menu_dot_field(void)
{
    /* The model's "checked" field is a ROW INDEX, not a flag -- it happened to
     * be 0/1 while every row was a two-value radio. */
    return (int32_t)menu_active_on();
}

/* Re-letter the borrowed option set for rows that name their values something other than
 * OFF/ON. The strings are the model's own, so this has to run after the stock rebuild has
 * installed them and be followed by an updateContent: the row components take their text
 * when they are built, so a plain repaint would keep showing OFF/ON. */
/* Make the list TALL enough for the rows it now claims.
 *
 * Telling the model it has five rows is not the same as giving the widget room
 * for them: stock sized this list when it built the two-entry OFF/ON set, so a
 * longer list rendered correctly inside a two-row viewport and scrolled.
 *
 * The row height is not guessed -- it is measured, once, from the height stock
 * itself chose for a two-row pane, and every later build is that unit times the
 * row count. A two-value row therefore lands back on exactly the height stock
 * gave it, so nothing has to be remembered or restored: the size is derived from
 * the row each time, which is the same reason the row COUNT is a hook and not a
 * stored field. */
void menu_pane_fit(uintptr_t rlist, int rows)
{
    static int32_t rowh;
    int32_t b[4];

    if (!rlist || rows < 1) return;
    if (mod_safe_read(rlist + COMP_BOUNDS_OFF, b, sizeof(b)) != 0) return;

    if (!rowh) {
        /* First sight is a stock-built pane, so its height is the unit we want.
         * Refuse a nonsense measurement rather than resizing off it. */
        if (b[3] <= 0 || (b[3] % MOD_PANE_ROWS_STOCK) != 0) {
            MDBG("right pane: list is %dx%d, not a clean %d rows -> not resizing\n",
                 b[2], b[3], MOD_PANE_ROWS_STOCK);
            rowh = -1;
        } else {
            rowh = b[3] / MOD_PANE_ROWS_STOCK;
            MDBG("right pane: list %dx%d at (%d,%d) -> row height %d\n",
                 b[2], b[3], b[0], b[1], rowh);
        }
    }
    if (rowh <= 0) return;

    {
        int32_t want = rowh * rows;

        if (b[1] + want > MOD_SCREEN_H)          /* never off the bottom */
            want = MOD_SCREEN_H - b[1];
        if (want == b[3]) return;
        ((void (*)(void *, int, int, int, int))FN_SET_BOUNDS)
            ((void *)rlist, b[0], b[1], b[2], want);
        MDBG("right pane: resized to %dx%d for %d rows\n", b[2], want, rows);
    }
}

/* Give the DJ SETTING list room for MOD_ROWS_VISIBLE rows while the overlay is
 * armed, and stock's own height back when it is not.
 *
 * Stock builds this list eight rows tall and leaves the panel beneath it blank,
 * so an overlay of more rows rendered inside that viewport and scrolled. As with
 * the right pane, the row height is measured from the stock-built list, once,
 * and the armed height is that unit times the row count. What the list is sized
 * for is published in menu_g_list_rows, which is what getNumRows reports, and
 * handed to the kit as its ceiling: a list that could not be grown stays at its
 * stock height, and the rows past what it shows are dropped with a warning
 * rather than left undrawn and unselectable. */
static int32_t g_list_rowh;      /* measured once from the stock-built list */
static int32_t g_list_stock_h;

int menu_list_row_h(void)
{
    return g_list_rowh > 0 ? g_list_rowh : MOD_ROW_H;
}

void menu_list_fit(uintptr_t list, int armed)
{
    int32_t b[4], want;
    int rows;

    if (!list || mod_safe_read(list + COMP_BOUNDS_OFF, b, sizeof(b)) != 0) return;

    if (!g_list_rowh) {
        if (b[3] <= 0)
            return;                 /* not laid out yet: measure next time */
        if ((b[3] % MOD_LIST_ROWS_STOCK) != 0) {
            MDBG("djlist: %dx%d is not a clean %d rows -> not resizing\n",
                 b[2], b[3], MOD_LIST_ROWS_STOCK);
            g_list_rowh = -1;
        } else {
            g_list_rowh = b[3] / MOD_LIST_ROWS_STOCK;
            g_list_stock_h = b[3];
            MDBG("djlist: %dx%d at (%d,%d) -> row height %d\n",
                 b[2], b[3], b[0], b[1], g_list_rowh);
        }
    }
    if (g_list_rowh <= 0) return;

    rows = armed ? MOD_ROWS_VISIBLE : MOD_LIST_ROWS_STOCK;
    want = armed ? g_list_rowh * rows : g_list_stock_h;
    /* The slack counts: the grow below passes through want + slack. */
    if (b[1] + want + MOD_LIST_GROW_SLACK > MOD_LIST_BOTTOM) {
        MDBG("djlist: %d rows would end at %d, past %d -> staying stock-sized\n",
             rows, b[1] + want, MOD_LIST_BOTTOM);
        rows = MOD_LIST_ROWS_STOCK;
        want = g_list_stock_h;
    }
    menu_g_list_rows = rows;
    kit_menu_set_shown(rows - MOD_ROW_FIRST);
    if (want == b[3]) return;
    /* Growing back from the keyboard's cut goes past the target first. The cut
     * list is scrolled and overflowing, so both of JUCE's scrollbars are up, and
     * each keeps the other alive: the horizontal one takes the height that makes
     * the content overflow vertically, the vertical one takes the width that
     * makes it overflow horizontally, and the content stays scrolled by one bar.
     * A viewport taller than the content has no use for either, and once they
     * are gone the content is back at the top and fits the real size exactly. */
    if (want > b[3])
        ((void (*)(void *, int, int, int, int))FN_SET_BOUNDS)
            ((void *)list, b[0], b[1], b[2], want + MOD_LIST_GROW_SLACK);
    ((void (*)(void *, int, int, int, int))FN_SET_BOUNDS)
        ((void *)list, b[0], b[1], b[2], want);
    MDBG("djlist: resized to %dx%d for %d rows\n", b[2], want, rows);
}

/* Bring `row` out from under the software keyboard: the list is cut down to the
 * rows above it and scrolled so `row` is the last one showing. Returns the
 * on-screen row the editor belongs on, which is `row` itself when it was already
 * clear of the keyboard. menu_list_fit grows the list back when the keyboard
 * closes, and JUCE returns it to the top by itself: every row fits again, so
 * there is nothing left to scroll.
 *
 * selectRow only scrolls when the selection changes, and `row` is the selection
 * that opened the keyboard, so it is stepped off and back on. The bounce guard is
 * up for both steps: the stock handler still runs (and rebuilds the right pane,
 * which the caller hides again), the overlay's own does not. */
int menu_list_fit_above_kbd(uintptr_t list, int row)
{
    int32_t b[4];
    int above;

    if (!list || g_list_rowh <= 0 ||
        mod_safe_read(list + COMP_BOUNDS_OFF, b, sizeof(b)) != 0)
        return row;
    above = (MOD_KBD_TOP - b[1]) / g_list_rowh;
    if (above < 2 || row < above) return row;

    ((void (*)(void *, int, int, int, int))FN_SET_BOUNDS)
        ((void *)list, b[0], b[1], b[2], above * g_list_rowh);
    menu_g_bouncing = 1;
    ((selectrow_t)FN_SELECT_ROW)((void *)list, row - 1, 1, 1);
    ((selectrow_t)FN_SELECT_ROW)((void *)list, row, 0, 1);
    menu_g_bouncing = 0;
    MDBG("djlist: cut to %d rows for the keyboard, row %d shown at %d\n",
         above, row, above - 1);
    return above - 1;
}

void menu_pane_labels(uintptr_t view, uintptr_t rmodel, const struct kit_row *r)
{
    uint8_t sa[JUCE_STRARRAY_BYTES] __attribute__((aligned(8))) = { 0 };
    const char *labels[MOD_ROW_VALUES_MAX];
    uintptr_t rlist;
    int n, i;

    if (!menu_g_strarr_ok || !menu_g_setstr_ok || !rmodel || !r) return;
    /* The borrowed option set IS the deck's own OFF/ON array, so a row using
     * kit_off_on already reads correctly: re-lettering it would hand the model
     * two identical strings and rebuild the rows for nothing. */
    if (r->values == kit_off_on) return;

    /* Exactly as many strings as menu_rnumrows will claim. Both read the same
     * function, so the array and the count are the same number by construction;
     * an earlier version wrote the count into the model instead and the two
     * drifted apart the moment the cursor moved to a shorter row. */
    n = menu_pane_rows(r);
    for (i = 0; i < n; i++)
        labels[i] = r->text ? "" : menu_row_value(r, i);
    juce_strarray_set(sa, labels, n);
    /* Swaps, so `sa` comes back holding the strings the model had and destroying it
     * frees them -- the same hand-off stock does with its throwaway copy. */
    ((void (*)(void *, void *))FN_RMODEL_SETSTRINGS)((void *)rmodel, sa);
    ((void (*)(void *))FN_STRARR_DTOR)(sa);

    rlist = menu_view_ptr(view, VIEW_RIGHT_LIST_OFF);
    menu_pane_fit(rlist, n);
    if (rlist) ((void (*)(void *))FN_UPDATECONTENT)((void *)rlist);
    MDBG("right pane: %d values, \"%s\"..\"%s\"\n", n, labels[0], labels[n - 1]);
}

/* `sel_row` is the row to leave the cursor on: normally the value in force, but while the
 * user is browsing the pane with the rotary it must stay where they moved it. */
void menu_force_right_pane_sel(uintptr_t view, int sel_row)
{
    uintptr_t rmodel, rlist;
    uint32_t none = OPTSET_NONE;

    /* Guarded end-to-end: the stock rebuild selects the borrowed set's current value
     * internally, and our own selectRow follows -- neither is a user choice. */
    menu_g_rsel_guard = 1;
    ((rowchanged_t)menu_g_orig_rowchanged)((void *)(view + LISTENER_VIEW_DELTA), OPTSET_OFF_ON);
    rmodel = menu_view_ptr(view, VIEW_RIGHT_MODEL_OFF);
    if (rmodel) {
        int32_t dot = menu_dot_field();

        /* Disowning the option set is also the moment menu_rnumrows starts
         * answering with the ROW's length instead of the borrowed set's, so
         * nothing may ask the model for a row between here and the setStrings
         * in menu_pane_labels below. Nothing does -- the writes in between are
         * plain field pokes with no callback -- but keep it that way. */
        mod_safe_write(rmodel + RMODEL_OPTSET_OFF, &none, sizeof(none));
        /* The rebuild seeds this from the borrowed set's own value, so point it back at
         * ours; menu_rrefresh re-stamps it per row anyway. */
        mod_safe_write(rmodel + RMODEL_CHECKED_OFF, &dot, sizeof(dot));
    }
    menu_pane_labels(view, rmodel, menu_row(menu_g_setting_row));
    rlist = menu_view_ptr(view, VIEW_RIGHT_LIST_OFF);
    if (rlist) ((selectrow_t)FN_SELECT_ROW)((void *)rlist, sel_row, 0, 1);
    menu_g_rsel_guard = 0;
}

void menu_force_right_pane(uintptr_t view)
{
    menu_force_right_pane_sel(view, menu_active_on());
}

/* Per-row refresh: stamp the field just before the row is built, so the dot is always
 * drawn from the selected row's flag no matter what restamped model+0x10 behind us. */
/* getNumRows on the right model.
 *
 * The stock body is `ldr w0,[x0,#0x110]` -- the count is a field seeded from
 * whichever option set the pane was built from, and the borrowed OFF/ON set is
 * two long. A row with more values than that has to say so, and saying so by
 * WRITING that field is what crashed the deck: the value outlives the row that
 * wanted it, so moving the cursor onto a two-value row left the model claiming
 * five rows over an array holding two.
 *
 * Answering the call instead cannot drift. It is scoped exactly like every other
 * hook here -- only while the overlay is up AND the model still carries our
 * disowned option-set id -- so a stock rebuild in progress, and everything after
 * we let go of the pane, gets the stock field back with nothing to undo. */
int32_t menu_rnumrows(void *self)
{
    uint32_t optset = 0;

    if (menu_g_mod_mode &&
        mod_safe_read((uintptr_t)self + RMODEL_OPTSET_OFF, &optset,
                      sizeof(optset)) == 0 && optset == OPTSET_NONE)
        return (int32_t)menu_pane_rows(menu_row(menu_g_setting_row));
    return ((int32_t (*)(void *))menu_g_orig_rnumrows)(self);
}

void *menu_rrefresh(void *self, int row, int col, int isSelected, void *existing)
{
    if (menu_g_mod_mode) {
        int32_t dot = menu_dot_field();
        mod_safe_write((uintptr_t)self + RMODEL_CHECKED_OFF, &dot, sizeof(dot));
    }
    return ((rrefresh_t)menu_g_orig_rrefresh)(self, row, col, isSelected, existing);
}

/* Selection changes are only trusted for the ROTARY. A touch tap is owned by cellClicked
 * above, because a tap is followed by the app re-selecting the previous value row -- and
 * adopting that second, app-driven change undid the tap (the value appeared not to
 * persist). Anything arriving outside the input dispatch is the app talking to itself. */
void menu_rselchanged(void *self, int row)
{
    uint32_t optset = 0;

    ((selchanged_t)menu_g_orig_rselchanged)(self, row);
    if (!menu_g_mod_mode || menu_g_rsel_guard || !menu_g_in_input) return;
    /* And only while the pane is ours: during a stock rebuild this field holds a real
     * option-set id, and its row-0 selection would otherwise read as a choice. */
    if (mod_safe_read((uintptr_t)self + RMODEL_OPTSET_OFF, &optset, sizeof(optset)) != 0 ||
        optset != OPTSET_NONE)
        return;
    menu_apply_value(row, 0, "rotary");
}

/* Re-point the right pane while the overlay is armed, so the value list belongs to
 * the mod row instead of the stock DJ setting that happens to share its index. */
void menu_rowchanged(void *self, int row)
{
    uintptr_t view = (uintptr_t)self - LISTENER_VIEW_DELTA;

    if (!menu_g_mod_mode) {                       /* stock DJ SETTING: untouched */
        ((rowchanged_t)menu_g_orig_rowchanged)(self, row);
        return;
    }
    /* Whatever row the notification names, the selection is bounced onto GATE CUE, so
     * its pane is the only correct one. Ignoring `row` also fixes a TOUCH tap on the
     * title or a filler row blanking the pane: JUCE fires cellClicked (which notifies
     * this same listener with the CLICKED row) after the bounce has already moved on. */
    (void)row;

    menu_force_right_pane(view);
    {
        const struct kit_row *r = menu_row(menu_g_setting_row);
        MDBG("right pane: %s %s/%s (sel=%d)\n", menu_row_label(menu_g_setting_row),
             menu_row_value(r, 0), menu_row_value(r, menu_row_nvalues(r) - 1),
             menu_active_on());
    }
}

