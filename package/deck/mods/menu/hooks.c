// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * menu/hooks.c - the overlay's vtable slots and the install.
 *
 * Each wrapper is a pass-through unless the overlay is armed. The install is all
 * or nothing across the twelve.
 */
#include "menu/internal.h"
#include "kit/mod.h"

/* ================================================================== */
/* Model hooks: pass-through unless the mod overlay is active          */
/* ================================================================== */

static int32_t mod_numrows(void *self)
{
    /* Collapse to just the mod rows in mod mode: JUCE then clamps the selection
     * to those rows (no plugging), and the count change is what makes
     * updateContent actually re-layout + repaint. The count is whatever
     * menu_list_fit made room for, so the list only scrolls while the
     * keyboard has cut it down (menu_list_fit_above_kbd). */
    if (menu_g_mod_mode) return menu_g_list_rows;
    return ((numrows_t)menu_g_orig_numrows)(self);
}

static void mod_paintcell(void *self, void *g, int row, int col, int w, int h, int sel)
{
    menu_g_model = (uintptr_t)self;                             /* self == the DJSettingTableModel */
    if (!menu_g_mod_mode) {                                     /* DJ SETTING: 100% stock */
        ((paintcell_t)menu_g_orig_paintcell)(self, g, row, col, w, h, sel);
        return;
    }
    if (row == MOD_ROW_TITLE) {                            /* overlay title */
        menu_draw_mod_header(self, g, w, h, "MOD SETTINGS");
    } else {
        const struct kit_row *r = menu_row(row);            /* one of the settings */
        if (r) {
            /* While a text row is being edited its editor sits over the value column and
             * draws the same characters, so the row must not also draw them -- the two
             * are right-aligned a few pixels apart and would show as one smeared string. */
            int editing = menu_kbd_is_up() && r->text && menu_kbd_row() == r;
            menu_draw_mod_row(self, g, w, h, r->label,
                         editing ? "" : menu_row_value(r, menu_row_index(r)), sel);
        }
    }
}

/* Focus-level input dispatch. Its level-1 branch rebuilds the right pane directly from
 * the row index (stock mapping), so re-assert ours afterwards. While the right pane holds
 * focus, adopt whatever value row the stock nav landed on first -- that is what makes the
 * rotary able to change the value, not just the touch taps. */
static int64_t mod_input(void *view, long a2, long a3)
{
    int64_t ret;
    int32_t before = -1, focus = -1, saved, row_sel = -1, sel_row;
    uint32_t optset = OPTSET_NONE;
    uintptr_t rmodel, rlist;

    menu_note_view(view);
    /* While the popup is up it owns the event: clear it and swallow, so the tick that
     * dismisses it does not also move the cursor or change a value. This is the path
     * the popup cannot block -- hardware input reaches the view directly. */
    if (kit_popup_is_up()) {
        kit_popup_dismiss();
        return 0;
    }
    mod_safe_read((uintptr_t)view + VIEW_FOCUS_OFF, &before, sizeof(before));
    menu_g_in_input = 1;                     /* anything the stock dispatch triggers is rotary */
    ret = ((switch_t)menu_g_orig_input)(view, a2, a3);
    menu_g_in_input = 0;
    if (!menu_g_mod_mode) return ret;

    mod_safe_read((uintptr_t)view + VIEW_FOCUS_OFF, &focus, sizeof(focus));
    rlist = menu_view_ptr((uintptr_t)view, VIEW_RIGHT_LIST_OFF);
    if (rlist) row_sel = ((int32_t (*)(void *, int))FN_CURRENT_ROW)((void *)rlist, 0);

    /* Leaving the pane is the rotary CONFIRM: stock would write the setting here, and ours
     * is disowned, so apply the highlighted value ourselves. The rotary's own selection
     * changes can't be used for this -- they land while stock has momentarily rebuilt the
     * pane as the real setting, which is exactly what the guards must ignore. */
    if (before == FOCUS_OPTION_PANE && focus != FOCUS_OPTION_PANE &&
        row_sel >= 0 && row_sel < menu_pane_rows(menu_row(menu_g_setting_row)))
        menu_apply_value(row_sel, 0, "rotary");

    /* Rebuild ONLY when a stock path actually replaced our option list: we always leave
     * the id at OPTSET_NONE, stock leaves 0..7. Re-asserting unconditionally also stole
     * focus back to the centre list every event (the row-changed listener sets
     * view+0x388 = 1), which left the right pane's selection unrendered -- so restore
     * whatever focus the stock dispatch had chosen. */
    rmodel = menu_view_ptr((uintptr_t)view, VIEW_RIGHT_MODEL_OFF);
    if (rmodel && mod_safe_read(rmodel + RMODEL_OPTSET_OFF, &optset, sizeof(optset)) == 0 &&
        optset != OPTSET_NONE) {
        saved = focus;
        /* Keep the cursor where the rotary left it while browsing inside the pane; on entry
         * (or anywhere else) start it on the value in force. */
        sel_row = (before == FOCUS_OPTION_PANE && focus == FOCUS_OPTION_PANE &&
                   (row_sel >= 0 &&
                    row_sel < menu_pane_rows(menu_row(menu_g_setting_row))))
                  ? row_sel : menu_active_on();
        menu_force_right_pane_sel((uintptr_t)view, sel_row);
        if (saved >= 0)
            mod_safe_write((uintptr_t)view + VIEW_FOCUS_OFF, &saved, sizeof(saved));
        MDBG("right pane restored (stock optset %u, focus %d, sel %d)\n", optset, saved, sel_row);
    }
    return ret;
}

/* Move the DJ SETTING list selection by `delta`, clamped to the model's row count.
 *
 * The deck has this as a helper of its own, but only on RK3399: it is one of ten
 * identical siblings, and the Renesas build's compiler inlined all ten into their
 * dispatchers, leaving no address to call. The body is four steps and every
 * primitive is resolved on both processors, so we do it here instead. `listref`
 * and `modelref` point AT the fields, matching what the deck's helper takes.
 *
 * delta == 0 selecting row 0 is what the deck's own helper does, kept so the two
 * behave the same; the one caller never passes 0. */
static void menu_listnav(void *listref, void *modelref, long delta)
{
    void  *list  = *(void **)listref;
    void  *model = *(void **)modelref;
    void **vt;
    int    cur, rows, row;

    if (!list || !model) return;
    vt   = *(void ***)model;
    cur  = ((int32_t (*)(void *, int))FN_CURRENT_ROW)(list, 0);
    rows = ((int32_t (*)(void *))vt[MODEL_GETNUMROWS_SLOT])(model);

    if (delta > 0) {
        row = cur + (int)delta;
        if (row > rows - 1) row = rows - 1;
    } else if (delta != 0) {
        row = cur + (int)delta;
        if (row < 0) row = 0;
    } else {
        row = 0;
    }
    ((selectrow_t)FN_SELECT_ROW)(list, row, 0, 1);
}

/* The title row is a label, not a setting: if it gets selected (arming clamps the
 * selection to row 0, and it can also be tapped), bounce one row down onto the
 * first real mod row. */
static void mod_selchanged(void *self, int row)
{
    int target;

    ((selchanged_t)menu_g_orig_selchanged)(self, row);
    if (!menu_g_mod_mode || menu_g_bouncing || !menu_g_view) return;

    if (menu_row(row)) {               /* a real settings row: it now owns the right pane */
        const struct kit_row *r;
        if (row != menu_g_setting_row) menu_kbd_close();   /* the previous row's editor, if any */
        menu_g_setting_row = row;
        r = menu_row(row);
        if (r->text) menu_kbd_open(r);
        return;
    }
    /* Not selectable: the title, or one of the blank filler rows below the last setting.
     * Put the cursor back on the row it came from, which is what stock does -- verified in
     * the SYSTEM pane, where tapping a blank row leaves the selection and the right pane
     * exactly as they were -- and equally what a rotary tick past either end should do.
     * Landing on the nearest live row instead half-selects it: the bounce re-enters here
     * with menu_g_bouncing set, so the settings-row branch above never runs and the row gets
     * neither its keyboard nor its pane. That fallback is only for a cursor with nowhere
     * to go back to, i.e. when a gate has just closed under it. */
    target = menu_row(menu_g_setting_row) ? menu_g_setting_row
           : ((row < MOD_ROW_FIRST) ? MOD_ROW_FIRST : menu_row_last());
    if (target != menu_g_setting_row)
        menu_kbd_close();              /* the row the editor belonged to is gone */
    menu_g_bouncing = 1;
    menu_listnav((void *)(menu_g_view + VIEW_DJLIST_OFF),
                 (void *)(menu_g_view + VIEW_MODEL_REF_OFF), target - row);
    menu_g_bouncing = 0;
    menu_g_setting_row = target;
    /* Selecting the blank row rebuilt the right pane on the way through, so an editor
     * that is still up needs it hidden again. */
    if (menu_kbd_is_up()) menu_pane_show(menu_g_view, 0);
    MDBG("row %d bounced -> row %d\n", row, target);
}

/* Dismiss-on-nav: any real category navigation -- rotary (sub_1447b10) or a
 * sidebar tap (sub_1448088) -- drops the overlay. We clear menu_g_mod_mode BEFORE the
 * stock handler runs, so the category-enter it triggers (DJ SETTING = sub_14361e0
 * -> updateContent -> our getNumRows) already renders stock with no GATE CUE
 * flash, then refresh so a dismiss that stays on DJ SETTING also drops the
 * collapsed row. This does NOT read the post-switch category, so it is robust
 * against the async touch category-write (the switch may land after the handler
 * returns). Arming stays a Ver-tap (mouseDown), which never calls these. While
 * menu_g_mod_mode==0 both hooks are byte-for-byte pass-throughs -- inert at startup
 * and during all normal use. */
/* Rotary. Unlike the touch handler this one also runs for plain row scrolling inside
 * the list, so it must only dismiss when the CATEGORY really changed -- otherwise every
 * detent would drop the overlay. The rotary category write is synchronous (unlike
 * touch), so comparing before/after is reliable here. */
static int64_t mod_switch(void *view, long a2, long delta)
{
    int32_t before = -1, after = -1;
    int64_t ret;
    menu_note_view(view);
    /* Ahead of the menu_g_mod_mode fast path: leaving UTILITY drops the overlay but the
     * popup is only hidden with its parent, so coming back would show it again over
     * a stock page. Dismissed rather than swallowed -- this hook also runs as part of
     * entering a category, and eating that would leave the view half set up.
     *
     * A rotary tick in UTILITY arrives HERE (view vtable+0x198), not at the input
     * dispatch (+0x190), so this is the slot a popup dismissal has to sit in. */
    kit_popup_dismiss();
    if (!menu_g_mod_mode) {
        menu_kbd_close();
        return ((switch_t)menu_g_orig_switch)(view, a2, delta);
    }
    mod_safe_read((uintptr_t)view + VIEW_CAT_OFF, &before, sizeof(before));
    ret = ((switch_t)menu_g_orig_switch)(view, a2, delta);
    mod_safe_read((uintptr_t)view + VIEW_CAT_OFF, &after, sizeof(after));
    if (after != before) {
        menu_g_mod_mode = 0;
        menu_refresh_djlist(view);
        menu_style_ver(view);
        MDBG("switch: overlay dismissed (cat %d->%d)\n", before, after);
    }
    return ret;
}

static int64_t mod_touchsel(void *view, long a2, long a3)    /* sidebar-tap category select (vtable+0x178) */
{
    menu_note_view(view);
    menu_kbd_close();                     /* first, so a warning it raises goes with the rest */
    kit_popup_dismiss();                  /* same reasoning as mod_switch */
    if (!menu_g_mod_mode)
        return ((switch_t)menu_g_orig_touchsel)(view, a2, a3);
    menu_g_mod_mode = 0;
    int64_t ret = ((switch_t)menu_g_orig_touchsel)(view, a2, a3);
    menu_refresh_djlist(view);
    menu_style_ver(view);
    MDBG("touchsel: overlay dismissed\n");
    return ret;
}

/* ================================================================== */
/* view mouseDown hook (vtable+0x28) : the "Ver." touch entry          */
/* CALIBRATION build: dump the MouseEvent head and toggle on any tap.  */
/* ================================================================== */

static void mod_mousedown(void *self, void *event)
{
    ((mousedown_t)menu_g_orig_mousedown)(self, event);   /* keep stock popup-dismiss behaviour */
    menu_note_view(self);                            /* self == the UTILITY view */
    /* Our popup intercepts clicks, so a tap normally never gets this far; this only
     * matters for whatever slips past it, and must not also count as a Ver hit. */
    if (kit_popup_is_up()) {
        kit_popup_dismiss();
        return;
    }
    if (!event) return;

    float pos[2] = { -1.0f, -1.0f };
    if (mod_safe_read((uintptr_t)event + MEVENT_POS_OFF, pos, sizeof(pos)) != 0) return;
    int x = (int)pos[0], y = (int)pos[1];

    if (x >= VER_HIT_X_MIN && y >= 0 && y <= VER_HIT_Y_MAX) {
        MDBG("Ver hit: pos=(%d,%d)\n", x, y);
        menu_toggle_overlay(self);
    }
}

/* ================================================================== */
/* Install                                                            */
/* ================================================================== */

static int menu_install(void)
{
    /* Every primitive is resolved by name, so "did it resolve" is the whole check:
     * a masked signature match says more than a four-byte prologue guard. */
    menu_g_render_ok = FN_FONT_BUILD && FN_DRAW_TEXT && FN_G_SETFONT && FN_G_SETCOL;
    if (!menu_g_render_ok) {
        MERR("modmenu: render primitives unavailable -> disabled\n");
        return -1;
    }

    /* Everything below is optional and checked separately, so one missing primitive
     * costs only the feature that needs it: without the StringArray pair there is no
     * re-lettered pane, and without setStrings a custom pane just reads OFF/ON. */
    menu_g_strarr_ok = FN_STRARR_CTOR && FN_STRARR_ADD;
    menu_g_setstr_ok = FN_RMODEL_SETSTRINGS != 0;
    menu_g_kbd_ok    = FN_KBD_SHOW && FN_KBD_HIDE && FN_EDITOR_SETTEXT;
    /* Without an editor of our own a text row must stay read-only: typing into the view's
     * would rewrite the stock HISTORY NAME setting (see mod_editor_get). */
    menu_g_editor_ok = menu_g_kbd_ok && FN_EDITOR_CTOR && FN_EDITOR_SETFONT &&
                  FN_EDITOR_JUSTIFY && FN_SET_BOUNDS && FN_STR_DEFCTOR &&
                  FN_EDITOR_FOCUS && FN_KEY_TO_TEXT && FN_ADD_VISIBLE &&
                  mod_patch_vslot("kbdKey", EP122_UTILITY_AS_KBD_LISTENER, 0,
                                  (void *)menu_kbd_key, &menu_g_orig_kbdkey) == 0;
    if (!menu_g_setstr_ok) MDBG("modmenu: setStrings unavailable -> panes read OFF/ON\n");
    if (!menu_g_editor_ok) MDBG("modmenu: TextEditor unavailable -> text rows are read-only\n");

    /* The twelve below are all or nothing, and MUST stay that way: they are one
     * overlay spread over a list model, a view and a right-pane model, and a
     * subset of them is a DJ SETTING screen that half-answers to a mod. The
     * optional primitives above degrade a feature; these decide whether there is
     * a menu at all. */
    const int hooks_wanted = 12;
    int ok = 0;
    ok += (mod_patch_vslot("paintCell", EP122_DJSET_MODEL, 0x20,
                           (void *)mod_paintcell, &menu_g_orig_paintcell) == 0);
    ok += (mod_patch_vslot("getNumRows", EP122_DJSET_MODEL, 0x10,
                           (void *)mod_numrows, &menu_g_orig_numrows) == 0);
    ok += (mod_patch_vslot("mouseDown", EP122_UTILITY_VIEW, 0x28,
                           (void *)mod_mousedown, &menu_g_orig_mousedown) == 0);
    ok += (mod_patch_vslot("switch", EP122_UTILITY_VIEW, 0x198,
                           (void *)mod_switch, &menu_g_orig_switch) == 0);
    ok += (mod_patch_vslot("touchsel", EP122_UTILITY_VIEW, 0x178,
                           (void *)mod_touchsel, &menu_g_orig_touchsel) == 0);
    ok += (mod_patch_vslot("selChanged", EP122_DJSET_MODEL, 0x60,
                           (void *)mod_selchanged, &menu_g_orig_selchanged) == 0);
    ok += (mod_patch_vslot("rowChanged", EP122_UTILITY_AS_MODEL_LISTENER, 0,
                           (void *)menu_rowchanged, &menu_g_orig_rowchanged) == 0);
    ok += (mod_patch_vslot("rCellClick", EP122_DJSET_RMODEL, 0x30,
                           (void *)menu_rcellclicked, &menu_g_orig_rcellclick) == 0);
    ok += (mod_patch_vslot("input", EP122_UTILITY_VIEW, 0x190,
                           (void *)mod_input, &menu_g_orig_input) == 0);
    ok += (mod_patch_vslot("rSelChanged", EP122_DJSET_RMODEL, 0x60,
                           (void *)menu_rselchanged, &menu_g_orig_rselchanged) == 0);
    ok += (mod_patch_vslot("rRefresh", EP122_DJSET_RMODEL, 0x28,
                           (void *)menu_rrefresh, &menu_g_orig_rrefresh) == 0);
    ok += (mod_patch_vslot("rNumRows", EP122_DJSET_RMODEL, 0x10,
                           (void *)menu_rnumrows, &menu_g_orig_rnumrows) == 0);
    if (ok != hooks_wanted) {
        MERR("modmenu: %d/%d hooks -> refused\n", ok, hooks_wanted);
        return -1;
    }
    MDBG("modmenu: installed %d/%d hooks (overlay inert until Ver-tap)\n",
         ok, hooks_wanted);
    return 0;
}

KIT_MOD(k_mod_menu,
        .name = "menu", .prio = 20, .install = menu_install,
        .what = "MOD SETTINGS overlay on the DJ SETTING list");
