// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * menu/pane.c - the right pane, borrowed from DJSettingRightPaneTableModel.
 *
 * Its model, geometry and radio are the deck's. This file supplies how many
 * values a row has, their labels and which one is checked, and leaves the
 * drawing to stock.
 */
#include "menu/internal.h"

/* Apply a chosen value to the mod row's feature instead of a stock DJ setting, then
 * refresh the centre value column and the radio. `focus_pane` is for a touch tap,
 * which never reaches the stock focus-advance handler; on the rotary path the stock
 * dispatch owns the focus level. */
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
        /* A committed value may change anything on screen (a theme is resolved at
         * draw time), so the whole view is repainted, not just the two lists. */
        if (menu_g_view)
            ((void (*)(void *))FN_REPAINT)((void *)menu_g_view);
        /* Before the save: a feature that rejects the value clears it here, and
         * the cleared value is persisted. */
        if (r->changed) r->changed();
        mods_settings_save();          /* survive an app restart */
    }
    if (focus_pane)
        mod_safe_write(menu_g_view + VIEW_FOCUS_OFF, &focus, sizeof(focus));
    list  = menu_view_ptr(menu_g_view, VIEW_DJLIST_OFF);
    rlist = menu_view_ptr(menu_g_view, VIEW_RIGHT_LIST_OFF);
    if (list)  ((void (*)(void *))FN_REPAINT)((void *)list);   /* centre value */
    if (rlist) {
        /* The dot colour is written onto each row's component in
         * refreshComponentForCell, so a plain repaint leaves the dot on the previous
         * value. Nudging the selection re-runs it: JUCE refreshes a row whenever its
         * selected state changes, and does this itself during mouseDown, so it is safe
         * inside a click. Do not use updateContent here: it destroys and rebuilds the
         * rows while the click is still being dispatched, making touch timing-dependent.
         * Guarded, since this is not a user choice. */
        menu_g_rsel_guard = 1;
        ((selectrow_t)FN_SELECT_ROW)((void *)rlist, row ? 0 : 1, 1, 1);
        ((selectrow_t)FN_SELECT_ROW)((void *)rlist, row, 1, 1);
        menu_g_rsel_guard = 0;
        ((void (*)(void *))FN_REPAINT)((void *)rlist);
    }
}

/* A real tap on a value row. This, not the selection change, is the touch signal: after
 * a tap the app re-selects the previous value row, so adopting from the selection would
 * undo the tap. */
void menu_rcellclicked(void *self, int row, int col, void *event)
{
    if (!menu_g_mod_mode) {
        ((cellclick_t)menu_g_orig_rcellclick)(self, row, col, event);
        return;
    }
    menu_apply_value(row, 1, "tap");    /* never call stock: it would write the DJ setting */
}

/* Value of the model+0x10 field for the current row value. refreshComponentForCell picks
 * the dot's colour as (row == this field) ? model+0x18 : model+0x20 and paints it onto
 * the dot component. With the field at 0, row 0 draws filled and row 1 empty, so this is
 * the value's own row. */
int32_t menu_dot_field(void)
{
    /* The model's "checked" field is a row index, not a flag. */
    return (int32_t)menu_active_on();
}

/* Make the list tall enough for its row count. Stock sized it for the two-entry
 * OFF/ON set, so a longer list would scroll inside a two-row viewport.
 *
 * The row height is measured once from the stock two-row pane, and every build is
 * that unit times the row count. A two-value row gets exactly the stock height, so
 * nothing has to be restored. */
void menu_pane_fit(uintptr_t rlist, int rows)
{
    static int32_t rowh;
    int32_t b[4];

    if (!rlist || rows < 1) return;
    if (mod_safe_read(rlist + COMP_BOUNDS_OFF, b, sizeof(b)) != 0) return;

    if (!rowh) {
        /* The first pane seen is stock-built, so its height gives the unit.
         * Do not resize off an implausible measurement. */
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
 * armed, and its stock height when it is not.
 *
 * Stock builds this list eight rows tall with the panel beneath it blank. As with
 * the right pane, the row height is measured once from the stock-built list and
 * the armed height is that unit times the row count. The resulting row count is
 * stored in menu_g_list_rows (what getNumRows reports) and passed to the kit as
 * its limit: a list that could not be grown stays stock height, and rows past it
 * are dropped with a warning. */
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
    /* Growing back from the keyboard's cut overshoots the target first. The cut
     * list is scrolled and overflowing, so both JUCE scrollbars are up and each
     * keeps the other alive (each takes space that makes the content overflow in
     * the other direction). A viewport taller than the content drops both, and the
     * content returns to the top and fits the real size. */
    if (want > b[3])
        ((void (*)(void *, int, int, int, int))FN_SET_BOUNDS)
            ((void *)list, b[0], b[1], b[2], want + MOD_LIST_GROW_SLACK);
    ((void (*)(void *, int, int, int, int))FN_SET_BOUNDS)
        ((void *)list, b[0], b[1], b[2], want);
    MDBG("djlist: resized to %dx%d for %d rows\n", b[2], want, rows);
}

/* Bring `row` out from under the software keyboard: the list is cut down to the
 * rows above it and scrolled so `row` is the last one showing. Returns the
 * on-screen row the editor belongs on (`row` itself if already clear of the
 * keyboard). menu_list_fit grows the list back when the keyboard closes, and
 * JUCE scrolls it back to the top since every row fits again.
 *
 * selectRow only scrolls when the selection changes, and `row` is already
 * selected, so it is stepped off and back on. The bounce guard is up for both
 * steps: the stock handler still runs (and rebuilds the right pane, which the
 * caller hides again), the overlay's does not. */
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

/* Relabel the borrowed option set for rows whose values are not OFF/ON. The strings
 * are the model's own, so this runs after the stock rebuild installed them and is
 * followed by updateContent: row components take their text when built, so a
 * repaint alone would keep showing OFF/ON. */
void menu_pane_labels(uintptr_t view, uintptr_t rmodel, const struct kit_row *r)
{
    uint8_t sa[JUCE_STRARRAY_BYTES] __attribute__((aligned(8))) = { 0 };
    const char *labels[MOD_ROW_VALUES_MAX];
    uintptr_t rlist;
    int n, i;

    if (!menu_g_strarr_ok || !menu_g_setstr_ok || !rmodel || !r) return;
    /* The borrowed option set is the deck's own OFF/ON array, so a row using
     * kit_off_on needs no relabelling. */
    if (r->values == kit_off_on) return;

    /* Exactly as many strings as menu_rnumrows reports; both use menu_pane_rows. */
    n = menu_pane_rows(r);
    for (i = 0; i < n; i++)
        labels[i] = r->text ? "" : menu_row_value(r, i);
    juce_strarray_set(sa, labels, n);
    /* Swaps, so `sa` comes back holding the model's old strings and destroying it
     * frees them, as stock does with its throwaway copy. */
    ((void (*)(void *, void *))FN_RMODEL_SETSTRINGS)((void *)rmodel, sa);
    ((void (*)(void *))FN_STRARR_DTOR)(sa);

    rlist = menu_view_ptr(view, VIEW_RIGHT_LIST_OFF);
    menu_pane_fit(rlist, n);
    if (rlist) ((void (*)(void *))FN_UPDATECONTENT)((void *)rlist);
    MDBG("right pane: %d values, \"%s\"..\"%s\"\n", n, labels[0], labels[n - 1]);
}

/* Build the mod row's value pane: borrow the OFF/ON option set for native strings and
 * styling, disown it so a tap cannot write the stock setting, and relabel it. Several
 * stock paths rebuild the pane from the row index, so this is re-asserted.
 * `sel_row` is the row to leave the cursor on: normally the current value, but while the
 * user is browsing the pane with the rotary it stays where they moved it. */
void menu_force_right_pane_sel(uintptr_t view, int sel_row)
{
    uintptr_t rmodel, rlist;
    uint32_t none = OPTSET_NONE;

    /* Guarded throughout: the stock rebuild selects the borrowed set's current value,
     * and our selectRow follows; neither is a user choice. */
    menu_g_rsel_guard = 1;
    ((rowchanged_t)menu_g_orig_rowchanged)((void *)(view + LISTENER_VIEW_DELTA), OPTSET_OFF_ON);
    rmodel = menu_view_ptr(view, VIEW_RIGHT_MODEL_OFF);
    if (rmodel) {
        int32_t dot = menu_dot_field();

        /* Once the option set is disowned, menu_rnumrows answers with the row's
         * length instead of the borrowed set's, so nothing may query the model for
         * a row before the setStrings in menu_pane_labels below. The writes in
         * between trigger no callbacks; keep it that way. */
        mod_safe_write(rmodel + RMODEL_OPTSET_OFF, &none, sizeof(none));
        /* The rebuild seeds this from the borrowed set's value, so point it back at
         * ours; menu_rrefresh also re-stamps it per row. */
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

/* getNumRows on the right model.
 *
 * The stock body is `ldr w0,[x0,#0x110]`: a field seeded from the option set the pane
 * was built from, two for the borrowed OFF/ON set. Do not write that field for longer
 * rows: it outlives the row, so moving onto a two-value row leaves the model claiming
 * five rows over a two-entry array, which crashes the deck.
 *
 * Active only while the overlay is up and the model carries our disowned option-set
 * id, so a stock rebuild in progress, and everything after we release the pane, gets
 * the stock field. */
int32_t menu_rnumrows(void *self)
{
    uint32_t optset = 0;

    if (menu_g_mod_mode &&
        mod_safe_read((uintptr_t)self + RMODEL_OPTSET_OFF, &optset,
                      sizeof(optset)) == 0 && optset == OPTSET_NONE)
        return (int32_t)menu_pane_rows(menu_row(menu_g_setting_row));
    return ((int32_t (*)(void *))menu_g_orig_rnumrows)(self);
}

/* Per-row refresh: stamp model+0x10 just before the row is built, so the dot follows
 * our value whatever restamped the field. */
void *menu_rrefresh(void *self, int row, int col, int isSelected, void *existing)
{
    if (menu_g_mod_mode) {
        int32_t dot = menu_dot_field();
        mod_safe_write((uintptr_t)self + RMODEL_CHECKED_OFF, &dot, sizeof(dot));
    }
    return ((rrefresh_t)menu_g_orig_rrefresh)(self, row, col, isSelected, existing);
}

/* Selection changes are only adopted for the rotary. A touch tap is handled by
 * cellClicked above, because the app re-selects the previous value row after a tap.
 * Selection changes outside the input dispatch come from the app itself. */
void menu_rselchanged(void *self, int row)
{
    uint32_t optset = 0;

    ((selchanged_t)menu_g_orig_rselchanged)(self, row);
    if (!menu_g_mod_mode || menu_g_rsel_guard || !menu_g_in_input) return;
    /* Only while the pane is ours: during a stock rebuild this field holds a real
     * option-set id, and its row-0 selection must not count as a choice. */
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
    /* The pane always follows menu_g_setting_row, whatever row the notification names.
     * Ignoring `row` keeps a touch tap on the title or a filler row from blanking the
     * pane: JUCE fires cellClicked (which notifies this listener with the clicked row)
     * after the bounce has already moved the selection. */
    (void)row;

    menu_force_right_pane(view);
    {
        const struct kit_row *r = menu_row(menu_g_setting_row);
        MDBG("right pane: %s %s/%s (sel=%d)\n", menu_row_label(menu_g_setting_row),
             menu_row_value(r, 0), menu_row_value(r, menu_row_nvalues(r) - 1),
             menu_active_on());
    }
}

