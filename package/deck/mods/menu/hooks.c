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
    /* In mod mode report only the mod rows: JUCE clamps the selection to them,
     * and the count change makes updateContent re-layout and repaint. The count
     * is what menu_list_fit made room for, so the list only scrolls while the
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
            /* While a text row is edited its editor draws the same text over the value
             * column a few pixels off, so the row leaves the value blank. */
            int editing = menu_kbd_is_up() && r->text && menu_kbd_row() == r;
            menu_draw_mod_row(self, g, w, h, r->label,
                         editing ? "" : menu_row_value(r, menu_row_index(r)), sel);
        }
    }
}

/* Focus-level input dispatch. Its level-1 branch rebuilds the right pane directly from
 * the row index (stock mapping), so ours is re-asserted afterwards. Leaving the right
 * pane adopts the value row the stock nav landed on, so the rotary can change values. */
static int64_t mod_input(void *view, long a2, long a3)
{
    int64_t ret;
    int32_t before = -1, focus = -1, saved, row_sel = -1, sel_row;
    uint32_t optset = OPTSET_NONE;
    uintptr_t rmodel, rlist;

    menu_note_view(view);
    /* While the popup is up, dismiss it and swallow the event so the same tick does not
     * also move the cursor or change a value. Hardware input reaches the view directly,
     * so the popup cannot block it. */
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

    /* Leaving the pane is the rotary confirm: stock would write the setting here, but ours
     * is disowned, so apply the highlighted value ourselves. The rotary's selection changes
     * cannot be used: they land while stock has briefly rebuilt the pane as the real
     * setting, which the guards ignore. */
    if (before == FOCUS_OPTION_PANE && focus != FOCUS_OPTION_PANE &&
        row_sel >= 0 && row_sel < menu_pane_rows(menu_row(menu_g_setting_row)))
        menu_apply_value(row_sel, 0, "rotary");

    /* Rebuild only when a stock path replaced our option list: we leave the id at
     * OPTSET_NONE, stock leaves 0..7. The rebuild's row-changed listener sets
     * view+0x388 = 1, stealing focus to the centre list and leaving the right pane's
     * selection unrendered, so the stock dispatch's focus is restored afterwards. */
    rmodel = menu_view_ptr((uintptr_t)view, VIEW_RIGHT_MODEL_OFF);
    if (rmodel && mod_safe_read(rmodel + RMODEL_OPTSET_OFF, &optset, sizeof(optset)) == 0 &&
        optset != OPTSET_NONE) {
        saved = focus;
        /* Keep the cursor where the rotary left it while browsing inside the pane;
         * otherwise start it on the current value. */
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
 * The deck has this helper only on RK3399; the Renesas build inlined all ten identical
 * siblings into their dispatchers, leaving no address to call. Every primitive is
 * resolved on both processors, so it is reimplemented here. `listref` and `modelref`
 * point at the fields, as in the deck's helper.
 *
 * delta == 0 selects row 0, matching the deck's helper; the one caller never passes 0. */
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
 * selection to row 0, and it can be tapped), bounce onto a real mod row. */
static void mod_selchanged(void *self, int row)
{
    int target;
    int settling = menu_g_mod_mode && !menu_g_bouncing && menu_g_view && menu_row(row);

    if (settling) {
        if (row != menu_g_setting_row) menu_kbd_close();   /* the previous row's editor, if any */
        menu_g_setting_row = row;
    }

    ((selchanged_t)menu_g_orig_selchanged)(self, row);

    if (!menu_g_mod_mode || menu_g_bouncing || !menu_g_view) return;

    if (settling) {
        const struct kit_row *r = menu_row(row);
        if (r->text) menu_kbd_open(r);
        return;
    }
    /* Not selectable: the title, or a blank filler row below the last setting. Put the
     * cursor back on the row it came from, as stock does in the SYSTEM pane, and as a
     * rotary tick past either end should. Do not land on the nearest live row instead:
     * the bounce re-enters here with menu_g_bouncing set, so the settings-row branch above
     * never runs and the row gets neither its keyboard nor its pane. The nearest-row
     * fallback is only for when the previous row has just been hidden. */
    target = menu_row(menu_g_setting_row) ? menu_g_setting_row
           : ((row < MOD_ROW_FIRST) ? MOD_ROW_FIRST : menu_row_last());
    if (target != menu_g_setting_row)
        menu_kbd_close();              /* the row the editor belonged to is gone */
    menu_g_bouncing = 1;
    menu_listnav((void *)(menu_g_view + VIEW_DJLIST_OFF),
                 (void *)(menu_g_view + VIEW_MODEL_REF_OFF), target - row);
    menu_g_bouncing = 0;
    menu_g_setting_row = target;
    /* Selecting the blank row rebuilt the right pane, so hide it again if an editor is
     * still up. */
    if (menu_kbd_is_up()) menu_pane_show(menu_g_view, 0);
    MDBG("row %d bounced -> row %d\n", row, target);
}

/* Dismiss-on-nav: any real category navigation, rotary or a sidebar tap, drops the
 * overlay. menu_g_mod_mode is cleared before the stock handler runs, so the
 * category-enter it triggers (DJ SETTING's setup -> updateContent ->
 * our getNumRows) already renders stock with no flash of the mod rows; the refresh after
 * covers a dismiss that stays on DJ SETTING. Arming is a Ver tap (mouseDown), which never
 * calls these. While menu_g_mod_mode==0 both hooks are pure pass-throughs.
 *
 * Rotary: this handler also runs for row scrolling inside the list, so it only dismisses
 * when the category changed. The rotary category write is synchronous, so comparing
 * before/after is reliable here. */
static int64_t mod_switch(void *view, long a2, long delta)
{
    int32_t before = -1, after = -1;
    int64_t ret;
    menu_note_view(view);
    /* Before the menu_g_mod_mode fast path: leaving UTILITY drops the overlay but the
     * popup is only hidden with its parent, so it would reappear over a stock page.
     * Dismissed, not swallowed: this hook also runs when entering a category, and
     * swallowing that would leave the view half set up.
     *
     * A rotary tick in UTILITY arrives here (view vtable+0x198), not at the input
     * dispatch (+0x190). */
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

/* Touch: the category write is async, so this dismisses without reading the
 * post-switch category. */
static int64_t mod_touchsel(void *view, long a2, long a3)    /* sidebar-tap category select (vtable+0x178) */
{
    menu_note_view(view);
    menu_kbd_close();                     /* first, so a warning it raises is dismissed too */
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
/* ================================================================== */

static void mod_mousedown(void *self, void *event)
{
    ((mousedown_t)menu_g_orig_mousedown)(self, event);   /* keep stock popup-dismiss behaviour */
    menu_note_view(self);                            /* self == the UTILITY view */
    /* Our popup intercepts clicks, so a tap rarely gets here while it is up; one
     * that does only dismisses it and is not a Ver hit. */
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
    /* Every primitive is resolved by name, so checking that it resolved is enough. */
    menu_g_render_ok = FN_FONT_BUILD && FN_DRAW_TEXT && FN_G_SETFONT && FN_G_SETCOL;
    if (!menu_g_render_ok) {
        MERR("modmenu: render primitives unavailable -> disabled\n");
        return -1;
    }

    /* Everything below is optional and checked separately, so a missing primitive
     * disables only the feature that needs it: without the StringArray pair or
     * setStrings, a custom pane reads OFF/ON. */
    menu_g_strarr_ok = FN_STRARR_CTOR && FN_STRARR_ADD;
    menu_g_setstr_ok = FN_RMODEL_SETSTRINGS != 0;
    menu_g_kbd_ok    = FN_KBD_SHOW && FN_KBD_HIDE && FN_EDITOR_SETTEXT;
    /* Without our own editor a text row stays read-only: typing into the view's would
     * rewrite the stock HISTORY NAME setting (see mod_editor_get). */
    menu_g_editor_ok = menu_g_kbd_ok && FN_EDITOR_CTOR && FN_EDITOR_SETFONT &&
                  FN_EDITOR_JUSTIFY && FN_SET_BOUNDS && FN_STR_DEFCTOR &&
                  FN_EDITOR_FOCUS && FN_KEY_TO_TEXT && FN_ADD_VISIBLE &&
                  mod_patch_vslot("kbdKey", EP122_UTILITY_AS_KBD_LISTENER, 0,
                                  (void *)menu_kbd_key, &menu_g_orig_kbdkey) == 0;
    if (!menu_g_setstr_ok) MDBG("modmenu: setStrings unavailable -> panes read OFF/ON\n");
    if (!menu_g_editor_ok) MDBG("modmenu: TextEditor unavailable -> text rows are read-only\n");

    /* The twelve hooks below are all or nothing: they form one overlay across a list
     * model, a view and a right-pane model, and a subset would leave DJ SETTING
     * half-modded. */
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
