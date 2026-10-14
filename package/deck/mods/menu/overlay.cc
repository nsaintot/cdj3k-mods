// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * menu/overlay.cc - the MOD SETTINGS overlay: container, background, focus.
 *
 * The container is a juce::Label with its paint replaced, child of the UTILITY
 * view, covering the view from below the header band to the bottom rule. Its
 * paint is the part of UtilityView::paint below y 90; the rail buttons, the
 * two lists and the page area sit on it.
 * It is built on the first open and kept; closing hides it.
 *
 * Focus follows the stock columns: rail, list, value pane. The rotary moves
 * within a column, a push goes one column right, BACK one column left and,
 * from the rail, closes the overlay. A category with a page has two columns:
 * the rail and the page.
 */
#include "menu/internal.h"
#include "juce/juce.hh"


uintptr_t     menu_g_view;
int           menu_g_open;
struct kit_menu *menu_g_menu;
int           menu_g_cat;
int           menu_g_row;
enum ov_focus menu_g_focus = FOCUS_RAIL;

static uintptr_t g_root;
static uintptr_t g_root_vt[VT_CLONE_WORDS];
static int       g_build_failed;

int menu_overlay_ok(void)
{
    return FN_ADD_VISIBLE && FN_SET_BOUNDS && FN_COMP_TOFRONT && FN_COMP_REPAINT &&
           FN_LABEL_CTOR && FN_STR_CTOR && FN_STR_DTOR && FN_FONT_BUILD && FN_FONT_DTOR &&
           MOD_FN_GFX_SETCOLOUR && MOD_FN_GFX_FILLRECT && MOD_FN_GFX_SETFONT &&
           MOD_FN_GFX_DRAWFITTEDTEXT &&
           ep122_sym(EP122_JUCE_TABLELISTBOX_CTOR) && ep122_sym(EP122_JUCE_TABLEHEADER_CTOR) &&
           ep122_sym(EP122_JUCE_TABLEHEADER_ADDCOLUMN) &&
           ep122_sym(EP122_JUCE_TABLELISTBOX_SETHEADER) &&
           ep122_sym(EP122_JUCE_TABLELISTBOX_SETHEADERHEIGHT) &&
           ep122_sym(EP122_JUCE_LISTBOX_SETROWHEIGHT) && ep122_sym(EP122_JUCE_LISTBOX_UPDATE) &&
           ep122_sym(EP122_JUCE_LISTBOX_SELECTROW) && ep122_sym(EP122_JUCE_LISTBOX_CURRENTROW) &&
           ep122_sym(EP122_JUCE_VIEWPORT_SETSCROLLBARS) &&
           ep122_sym(EP122_JUCE_COMP_SETINTERCEPTS) &&
           ep122_sym(EP122_JUCE_IMAGE_CTOR) && ep122_sym(EP122_JUCE_IMAGE_DTOR) &&
           ep122_sym(EP122_JUCE_IMAGE_COPY) && ep122_sym(EP122_JUCE_IMAGE_SWAP) &&
           ep122_sym(EP122_JUCE_GFX_CTOR_IMAGE) && ep122_sym(EP122_JUCE_GFX_DTOR) &&
           ep122_sym(EP122_JUCE_IDENTIFIER_CTOR) &&
           ep122_sym(EP122_MEOW_SLB_CTOR) && ep122_sym(EP122_MEOW_SLB_SETTOGGLE) &&
           ep122_sym(EP122_GUI_ILU_CTOR) && ep122_sym(EP122_GUI_ILU_SETUP) &&
           ep122_sym(EP122_DJSET_MODEL) && ep122_sym(EP122_DJSET_RMODEL);
}

/* ================================================================== */
/* Background                                                         */
/* ================================================================== */

/* UtilityView::paint below the header band, in overlay coordinates. */
static void root_paint(void *, void *g)
{
    int i, sel = menu_g_cat;

    kit_ui_colour(g, KIT_UI_PANEL);
    kit_ui_rect(g, 0, 0, OV_W, OV_H);
    kit_ui_colour(g, KIT_UI_BLACK);
    kit_ui_rect(g, 0, 2, OV_W - 1, 536);

    for (i = 0; i < RAIL_CELLS; i++) {
        uint32_t c = i != sel ? KIT_UI_PANEL
                   : menu_g_focus == FOCUS_RAIL ? KIT_UI_FOCUS : KIT_UI_SELECTED;

        kit_ui_colour(g, c);
        kit_ui_rect(g, RAIL_X, RAIL_Y + i * RAIL_PITCH, RAIL_W, RAIL_H);
    }
    kit_ui_colour(g, KIT_UI_PANEL);
    kit_ui_rect(g, RAIL_X, 507, RAIL_W, 29);

    for (i = 0; i < LIST_ROWS_SHOWN; i++)
        kit_ui_rect(g, LIST_X, LIST_Y + i * ROW_H, LIST_W, ROW_H - 2);
    kit_ui_rect(g, LIST_X, 504, LIST_W, 32);

    for (i = 0; i < 5; i++)
        kit_ui_rect(g, PANE_X, PANE_Y + 2 * i * ROW_H, PANE_W, ROW_H - 2);
    kit_ui_rect(g, PANE_X, 504, PANE_W, 32);

    kit_ui_colour(g, KIT_UI_RULE);
    kit_ui_rect(g, 0, 537, OV_W - 1, 2);
}

static int build(void)
{
    static const struct juce_vt_override ov[] = {
        { JUCE_VT_PAINT, (void *)root_paint, 0 },
    };
    uintptr_t vptr;

    if (g_root) return 0;
    if (g_build_failed || !menu_g_view || !menu_overlay_ok()) return -1;
    vptr = juce_label_vt_clone(g_root_vt, ov, 1);
    if (vptr)
        g_root = juce_label(menu_g_view, "", KIT_UI_FONT_LABEL, 0, 0, vptr, OV_X, OV_Y, OV_W, OV_H);
    if (!g_root || menu_rail_build(g_root) != 0 || menu_lists_build(g_root) != 0 ||
        menu_page_build(g_root) != 0) {
        MERR("modmenu: overlay build failed\n");
        if (g_root) juce_comp_set_visible(g_root, 0);
        g_build_failed = 1;
        return -1;
    }
    kit_menu_set_repaint(menu_g_menu, menu_page_repaint);
    MDBG("modmenu: overlay built at %#lx\n", (unsigned long)g_root);
    return 0;
}

int menu_overlay_built(void)
{
    return g_root != 0;
}

static void repaint_all(void)
{
    if (g_root) ep_call(void(void *))::at(FN_COMP_REPAINT, (void *)g_root);
    menu_lists_repaint();
}

/* ================================================================== */
/* Open / close                                                       */
/* ================================================================== */

/* The content of the category shown: a page, or the list and the pane. */
static const struct kit_page *cur_page(void)
{
    return kit_menu_cat_page(menu_g_menu, menu_g_cat);
}

static void show_content(void)
{
    const struct kit_page *p = cur_page();

    menu_lists_show(p == NULL);
    menu_page_show(p);
    if (!p) {
        menu_list_reload();
        menu_pane_reload();
    }
}

void menu_open(void)
{
    const char *why;

    menu_g_menu = kit_menu_instance();
    why = kit_menu_problem(menu_g_menu);

    if (menu_g_open) return;
    kit_popup_dismiss();
    if (why) {
        const char *lines[2] = { "MOD SETTINGS unavailable", why };

        kit_popup_show(lines, 2);
        MDBG("modmenu: refused: %s\n", why);
        return;
    }
    if (build() != 0) return;

    menu_g_open = 1;
    menu_g_focus = FOCUS_RAIL;
    menu_rail_show(menu_g_cat);
    juce_comp_set_visible(g_root, 1);
    ep_call(void(void *, int))::at(FN_COMP_TOFRONT, (void *)g_root, 0);
    show_content();
    menu_style_ver();
    repaint_all();
    MDBG("modmenu: open, category %d\n", menu_g_cat);
}

void menu_close(void)
{
    if (!menu_g_open) return;
    menu_kbd_close();
    kit_popup_dismiss();
    menu_g_open = 0;
    menu_page_show(NULL);
    juce_comp_set_visible(g_root, 0);
    menu_style_ver();
    MDBG("modmenu: closed\n");
}

/* ================================================================== */
/* Focus and navigation                                               */
/* ================================================================== */

void menu_set_focus(enum ov_focus f)
{
    if (menu_g_focus == f) return;
    menu_g_focus = f;
    if (f == FOCUS_PANE && !cur_page()) {
        const struct kit_row *r = menu_row();

        menu_pane_select(r && r->state ? *r->state : 0);
    }
    repaint_all();
}

void menu_select_cat(int c)
{
    if (c < 0 || c >= kit_menu_ncats(menu_g_menu)) return;
    if (c != menu_g_cat) {
        menu_kbd_close();
        menu_g_cat = c;
        menu_g_row = 0;
    }
    menu_rail_show(c);
    show_content();
    repaint_all();
}

void menu_list_changed(void)
{
    if (!menu_g_open || cur_page()) return;
    menu_list_reload();
    menu_pane_reload();
}

void menu_turn(int delta)
{
    const struct kit_row *r;
    int n, v;

    if (!delta) return;
    if (menu_g_focus != FOCUS_RAIL && cur_page()) {
        if (cur_page()->turn) cur_page()->turn(cur_page()->ctx, delta);
        return;
    }
    switch (menu_g_focus) {
    case FOCUS_RAIL:
        v = menu_g_cat + delta;
        if (v < 0) v = 0;
        if (v >= kit_menu_ncats(menu_g_menu)) v = kit_menu_ncats(menu_g_menu) - 1;
        menu_select_cat(v);
        break;
    case FOCUS_LIST:
        n = kit_menu_count(menu_g_menu, menu_g_cat);
        v = menu_g_row + delta;
        if (v < 0) v = 0;
        if (v >= n) v = n - 1;
        if (v == menu_g_row) break;
        menu_kbd_close();
        menu_g_row = v;
        menu_list_select(v);
        menu_pane_reload();
        r = menu_row();
        if (r && r->text) menu_kbd_open(r);
        break;
    case FOCUS_PANE:
        r = menu_row();
        n = (r && r->values) ? (int)r->nvalues : 0;
        v = menu_pane_cursor() + delta;
        if (v < 0) v = 0;
        if (v >= n) v = n - 1;
        if (v >= 0) menu_pane_select(v);
        break;
    }
}

void menu_push(void)
{
    const struct kit_row *r = menu_row();
    const struct kit_page *p = cur_page();

    if (p) {
        if (menu_g_focus == FOCUS_RAIL) menu_set_focus(FOCUS_LIST);
        else if (p->push) p->push(p->ctx);
        return;
    }
    switch (menu_g_focus) {
    case FOCUS_RAIL:
        if (kit_menu_count(menu_g_menu, menu_g_cat) > 0) menu_set_focus(FOCUS_LIST);
        break;
    case FOCUS_LIST:
        if (r && r->values) menu_set_focus(FOCUS_PANE);
        else if (r && r->text && !menu_kbd_is_up()) menu_kbd_open(r);
        break;
    case FOCUS_PANE:
        menu_commit(menu_pane_cursor());
        menu_set_focus(FOCUS_LIST);
        break;
    }
}

void menu_back(void)
{
    const struct kit_page *p = cur_page();

    if (menu_kbd_is_up()) {
        menu_kbd_close();
        return;
    }
    if (p && menu_g_focus != FOCUS_RAIL) {
        if (!p->back || !p->back(p->ctx)) menu_set_focus(FOCUS_RAIL);
        return;
    }
    switch (menu_g_focus) {
    case FOCUS_RAIL: menu_close(); break;
    case FOCUS_LIST: menu_set_focus(FOCUS_RAIL); break;
    case FOCUS_PANE: menu_set_focus(FOCUS_LIST); break;
    }
}

/* Apply a value to the row under the cursor, let its feature react, persist. */
void menu_commit(int value)
{
    const struct kit_row *r = menu_row();

    if (!r || !r->state || value < 0 || value >= (int)r->nvalues) return;
    if (*r->state != value) {
        *r->state = value;
        MDBG("modmenu: %s -> %s\n", r->label, r->values[value]);
        /* changed() may reject the value; the corrected one is saved. */
        if (r->changed) r->changed();
        mods_settings_save();
        /* A value can change anything on screen, such as the theme. */
        ep_call(void(void *))::at(FN_COMP_REPAINT, (void *)menu_g_view);
    }
    menu_list_changed();
    repaint_all();
}
