// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * menu/lists.cc - the settings list and the value pane.
 *
 * Both are juce::TableListBox, built the way UtilityView builds its own lists.
 * Each has a TableListBoxModel of ours whose vtable is a copy of
 * gui::DJSettingTableModel's with every slot that reads model fields replaced.
 * The paint copies the stock models (kit/ui.h).
 */
#include "menu/internal.h"
#include "juce/juce.hh"

#define FN_TLB_CTOR        ep122_sym(EP122_JUCE_TABLELISTBOX_CTOR)
#define FN_HDR_CTOR        ep122_sym(EP122_JUCE_TABLEHEADER_CTOR)
#define FN_HDR_ADDCOLUMN   ep122_sym(EP122_JUCE_TABLEHEADER_ADDCOLUMN)
#define FN_TLB_SETHEADER   ep122_sym(EP122_JUCE_TABLELISTBOX_SETHEADER)
#define FN_TLB_SETHEADERH  ep122_sym(EP122_JUCE_TABLELISTBOX_SETHEADERHEIGHT)
#define FN_LB_SETROWH      ep122_sym(EP122_JUCE_LISTBOX_SETROWHEIGHT)
#define FN_LB_UPDATE       ep122_sym(EP122_JUCE_LISTBOX_UPDATE)
#define FN_LB_SELECTROW    ep122_sym(EP122_JUCE_LISTBOX_SELECTROW)
#define FN_VP_SCROLLBARS   ep122_sym(EP122_JUCE_VIEWPORT_SETSCROLLBARS)
#define FN_COMP_INTERCEPTS ep122_sym(EP122_JUCE_COMP_SETINTERCEPTS)
#define FN_IMAGE_COPY      ep122_sym(EP122_JUCE_IMAGE_COPY)
#define FN_ILU_CTOR        ep122_sym(EP122_GUI_ILU_CTOR)
#define FN_ILU_SETUP       ep122_sym(EP122_GUI_ILU_SETUP)
#define DJSET_MODEL_VT     ep122_sym(EP122_DJSET_MODEL)
#define DJSET_RMODEL_VT    ep122_sym(EP122_DJSET_RMODEL)

#define TLB_ALLOC_SIZE     0x150
#define HDR_ALLOC_SIZE     0x138
#define ILU_ALLOC_SIZE     0xe0
#define LB_VIEWPORT_OFF    0xe0     /* juce::ListBox: Viewport* */

#define LB_COL_BG          0x1002800  /* juce::ListBox::backgroundColourId */

#define COLUMN_ID          1
#define COLUMN_MIN_W       30
#define COLUMN_FLAGS       0x1f

/* TableListBoxModel vtable: two head words, then slots 0x00..0x88. */
#define MODEL_VT_HEAD      2
#define MODEL_VT_SLOTS     18
#define MODEL_VT_WORDS     (MODEL_VT_HEAD + MODEL_VT_SLOTS)

enum {
    SLOT_DTOR1 = 0x00, SLOT_DTOR0 = 0x08, SLOT_NUMROWS = 0x10, SLOT_PAINTBG = 0x18,
    SLOT_PAINTCELL = 0x20, SLOT_REFRESH = 0x28, SLOT_CLICKED = 0x30,
    SLOT_SELCHANGED = 0x60, SLOT_GUI_FOCUSED = 0x88,
};

/* The size of gui::DJSettingTableModel, so a slot inherited from its vtable
 * reads zeroes rather than our neighbours. */
#define MODEL_SIZE         0x50

struct model { uintptr_t vptr; uint8_t fields[MODEL_SIZE - sizeof(uintptr_t)]; };

static uintptr_t g_list_vt[MODEL_VT_WORDS], g_pane_vt[MODEL_VT_WORDS];
static struct model g_list_model, g_pane_model;
static uintptr_t g_list, g_pane;
static int g_driving;                   /* our own selectRow is running */

/* Dot images, copied from the DJ right-pane model. */
alignas(16) static uint8_t g_dot_on[16], g_dot_off[16];
static int g_dots;

/* ================================================================== */
/* Rows                                                               */
/* ================================================================== */

const struct kit_row *menu_row(void)
{
    return kit_menu_row(menu_g_menu, menu_g_cat, menu_g_row);
}

static const char *row_value(const struct kit_row *r)
{
    int v;

    if (!r) return "";
    if (r->info) return r->info;
    if (r->text) return r->text[0] ? r->text : "NOT SET";
    v = *r->state;
    return (v >= 0 && v < (int)r->nvalues) ? r->values[v] : "";
}

/* Values the pane lists: a CHOICE row's own; none for TEXT and INFO. */
static int pane_rows(const struct kit_row *r)
{
    return (r && r->values) ? (int)r->nvalues : 0;
}

/* ================================================================== */
/* Settings list model                                                */
/* ================================================================== */

static void model_nop(void *) {}

static int32_t list_numrows(void *)
{
    return kit_menu_count(menu_g_menu, menu_g_cat);
}

static enum kit_ui_row list_look(int sel)
{
    return !sel ? KIT_UI_ROW_NORMAL : menu_g_focus == FOCUS_LIST ? KIT_UI_ROW_FOCUSED
                                                                 : KIT_UI_ROW_CURSOR;
}

static void list_paintbg(void *, void *g, int, int w, int h, int sel)
{
    kit_ui_row_bg(g, w, h, list_look(sel));
}

static void list_paintcell(void *, void *g, int row, int, int w, int h, int sel)
{
    const struct kit_row *r = kit_menu_row(menu_g_menu, menu_g_cat, row);
    int editing;

    if (!r) return;
    /* The value is drawn by the editor while the keyboard is up on this row. */
    editing = menu_kbd_is_up() && menu_kbd_row() == r;

    kit_ui_setting(g, w, h, r->label, editing ? "" : row_value(r), list_look(sel));
}

static void click_nop(void *, int, int, void *) {}

/* A tap on the text row under the cursor reopens its keyboard; a tap on any
 * other row goes through selectedRowsChanged. */
static void list_cell_clicked(void *, int row, int, void *)
{
    const struct kit_row *r = kit_menu_row(menu_g_menu, menu_g_cat, row);

    if (r && r->text && row == menu_g_row && !menu_kbd_is_up()) menu_kbd_open(r);
}

static void list_selchanged(void *, int row)
{
    const struct kit_row *r;

    if (g_driving) return;
    r = kit_menu_row(menu_g_menu, menu_g_cat, row);
    if (!r) {
        /* Below the last row: keep the cursor where it was. */
        menu_list_select(menu_g_row);
        return;
    }
    if (row != menu_g_row) menu_kbd_close();
    menu_g_row = row;
    menu_set_focus(FOCUS_LIST);
    menu_pane_reload();
    if (r->text) menu_kbd_open(r);
}

static int list_focused(void *)
{
    return menu_g_focus == FOCUS_LIST;
}

/* ================================================================== */
/* Value pane model                                                   */
/* ================================================================== */

static int32_t pane_numrows(void *)
{
    return pane_rows(menu_row());
}

/* Unselected rows alternate #191919 / black; the cursor row is blue only while
 * the pane has focus. */
static void pane_paintbg(void *, void *g, int row, int w, int h, int sel)
{
    if (sel && menu_g_focus == FOCUS_PANE) {
        kit_ui_row_bg(g, w, h, KIT_UI_ROW_FOCUSED);
        return;
    }
    kit_ui_colour(g, (row & 1) ? KIT_UI_BLACK : KIT_UI_PANEL);
    kit_ui_rect(g, 0, 0, w, h);
    kit_ui_colour(g, KIT_UI_BLACK);
    kit_ui_rect(g, 0, h - 2, w, 2);
}

static void pane_paintcell(void *, void *, int, int, int, int, int) {}

static void ilu_setup(uintptr_t c, const struct kit_row *r, int row, int sel)
{
    uint32_t white = KIT_UI_WHITE;
    int n = pane_rows(r);
    void *dot = (*r->state == row) ? g_dot_on : g_dot_off;
    juce::String text(r->values[row], juce::from_utf8);

    ep_call(void(void *, void *, void *, int, int, int, int, int, const uint32_t *, int, int))
        ::at(FN_ILU_SETUP, (void *)c, text.addr(), dot, row, n, sel,
             PANE_W, ROW_H, &white, 0, -1);
}

/* Rows are the stock radio rows, gui::ImagedLabelUtility. */
static void *pane_refresh(void *, int row, int, int sel, void *existing)
{
    const struct kit_row *r = menu_row();
    uintptr_t c = (uintptr_t)existing;

    if (!c) {
        c = (uintptr_t)calloc(1, ILU_ALLOC_SIZE);
        if (!c) return NULL;
        ep_call(void(void *))::at(FN_ILU_CTOR, (void *)c);
        ep_call(void(void *, int, int))::at(FN_COMP_INTERCEPTS, (void *)c, 0, 1);
    }
    if (r && g_dots && row >= 0 && row < pane_rows(r))
        ilu_setup(c, r, row, sel);
    return (void *)c;
}

/* A tap on a value applies it. */
static void pane_selchanged(void *, int row)
{
    if (g_driving || row < 0 || row >= pane_rows(menu_row())) return;
    menu_set_focus(FOCUS_PANE);
    menu_commit(row);
}

static int pane_focused(void *)
{
    return menu_g_focus == FOCUS_PANE;
}

/* ================================================================== */
/* Build                                                              */
/* ================================================================== */

struct slot_fn { unsigned slot; void *fn; };

static uintptr_t model_vt(uintptr_t *out, const struct slot_fn *ov, int n)
{
    int i;

    if (mod_safe_read(DJSET_MODEL_VT - MODEL_VT_HEAD * sizeof(uintptr_t), out,
                      MODEL_VT_WORDS * sizeof(uintptr_t)) != 0)
        return 0;
    for (i = 0; i < n; i++)
        out[MODEL_VT_HEAD + ov[i].slot / sizeof(uintptr_t)] = (uintptr_t)ov[i].fn;
    return (uintptr_t)&out[MODEL_VT_HEAD];
}

static uintptr_t make_table(uintptr_t parent, struct model *m, const char *name,
                            int x, int y, int w, int h)
{
    uintptr_t tlb, hdr;

    tlb = (uintptr_t)calloc(1, TLB_ALLOC_SIZE);
    hdr = (uintptr_t)calloc(1, HDR_ALLOC_SIZE);
    if (!tlb || !hdr) {
        free((void *)tlb);
        free((void *)hdr);
        return 0;
    }
    {
        juce::String n(name);
        ep_call(void(void *, void *, void *))::at(FN_TLB_CTOR, (void *)tlb, n.addr(), m);
    }
    ep_call(void(void *))::at(FN_HDR_CTOR, (void *)hdr);
    {
        juce::String col(name);
        ep_call(void(void *, void *, int, int, int, int, int, int))
            ::at(FN_HDR_ADDCOLUMN, (void *)hdr, col.addr(), COLUMN_ID, w,
                 COLUMN_MIN_W, -1, COLUMN_FLAGS, -1);
    }
    ep_call(void(void *, void *))::at(FN_TLB_SETHEADER, (void *)tlb, (void *)hdr);
    ep_call(void(void *, int))::at(FN_TLB_SETHEADERH, (void *)tlb, 0);
    ep_call(void(void *, int))::at(FN_LB_SETROWH, (void *)tlb, ROW_H);
    ep_call(void(void *, int, int, int, int))::at(FN_SET_BOUNDS, (void *)tlb, x, y, w, h);
    /* Transparent, so the overlay's stripes show below the last row. */
    juce_comp_colour(tlb, LB_COL_BG, 0);
    /* Scrolls, but without a scrollbar, like the stock lists. */
    ep_call(void(void *, int, int, int, int))
        ::at(FN_VP_SCROLLBARS, *(void **)(tlb + LB_VIEWPORT_OFF), 0, 0, 1, 0);
    ep_call(void(void *, void *, int))::at(FN_ADD_VISIBLE, (void *)parent, (void *)tlb, -1);
    return tlb;
}

static void load_dots(void)
{
    uintptr_t rmodel = 0, vt = 0;

    if (g_dots) return;
    if (mod_safe_read(menu_g_view + VIEW_RMODEL_OFF, &rmodel, sizeof(rmodel)) != 0 || !rmodel ||
        mod_safe_read(rmodel, &vt, sizeof(vt)) != 0 || vt != DJSET_RMODEL_VT) {
        MDBG("modmenu: view+0x3c8 is not the DJ right-pane model -> no dots\n");
        return;
    }
    ep_call(void(void *, void *))::at(FN_IMAGE_COPY, g_dot_on, (void *)(rmodel + RMODEL_DOT_ON_OFF));
    ep_call(void(void *, void *))::at(FN_IMAGE_COPY, g_dot_off, (void *)(rmodel + RMODEL_DOT_OFF_OFF));
    g_dots = 1;
}

int menu_lists_build(uintptr_t parent)
{
    static const struct slot_fn list_ov[] = {
        { SLOT_DTOR1, (void *)model_nop },        { SLOT_DTOR0, (void *)model_nop },
        { SLOT_NUMROWS, (void *)list_numrows },   { SLOT_PAINTBG, (void *)list_paintbg },
        { SLOT_PAINTCELL, (void *)list_paintcell }, { SLOT_CLICKED, (void *)list_cell_clicked },
        { SLOT_SELCHANGED, (void *)list_selchanged }, { SLOT_GUI_FOCUSED, (void *)list_focused },
    };
    static const struct slot_fn pane_ov[] = {
        { SLOT_DTOR1, (void *)model_nop },        { SLOT_DTOR0, (void *)model_nop },
        { SLOT_NUMROWS, (void *)pane_numrows },   { SLOT_PAINTBG, (void *)pane_paintbg },
        { SLOT_PAINTCELL, (void *)pane_paintcell }, { SLOT_REFRESH, (void *)pane_refresh },
        { SLOT_CLICKED, (void *)click_nop },   { SLOT_SELCHANGED, (void *)pane_selchanged },
        { SLOT_GUI_FOCUSED, (void *)pane_focused },
    };

    g_list_model.vptr = model_vt(g_list_vt, list_ov, (int)(sizeof(list_ov) / sizeof(list_ov[0])));
    g_pane_model.vptr = model_vt(g_pane_vt, pane_ov, (int)(sizeof(pane_ov) / sizeof(pane_ov[0])));
    if (!g_list_model.vptr || !g_pane_model.vptr) return -1;

    load_dots();
    g_list = make_table(parent, &g_list_model, "ModSettingTable", LIST_X, LIST_Y, LIST_W, LIST_H);
    g_pane = make_table(parent, &g_pane_model, "ModSettingRightPaneTable",
                        PANE_X, PANE_Y, PANE_W, PANE_H);
    return (g_list && g_pane) ? 0 : -1;
}

/* ================================================================== */
/* Driving the lists                                                  */
/* ================================================================== */

static void select_row(uintptr_t tlb, int row)
{
    if (!tlb) return;
    g_driving++;
    ep_call(void(void *, int, int, int))::at(FN_LB_SELECTROW, (void *)tlb, row, 0, 1);
    g_driving--;
}

static void update(uintptr_t tlb)
{
    if (!tlb) return;
    ep_call(void(void *))::at(FN_LB_UPDATE, (void *)tlb);
    ep_call(void(void *))::at(FN_COMP_REPAINT, (void *)tlb);
}

void menu_list_reload(void)
{
    int n = kit_menu_count(menu_g_menu, menu_g_cat);

    if (menu_g_row >= n) menu_g_row = n - 1;
    if (menu_g_row < 0) menu_g_row = 0;
    update(g_list);
    select_row(g_list, menu_g_row);
}

void menu_list_select(int row)
{
    select_row(g_list, row);
}

void menu_pane_reload(void)
{
    const struct kit_row *r = menu_row();

    load_dots();
    update(g_pane);
    select_row(g_pane, (r && r->state && pane_rows(r)) ? *r->state : -1);
}

void menu_pane_select(int idx)
{
    select_row(g_pane, idx);
}

int menu_pane_cursor(void)
{
    if (!g_pane) return -1;
    return ep_call(int32_t(void *, int))::at(ep122_sym(EP122_JUCE_LISTBOX_CURRENTROW),
                                             (void *)g_pane, 0);
}

void menu_lists_show(int visible)
{
    juce_comp_set_visible(g_list, visible);
    juce_comp_set_visible(g_pane, visible);
}

void menu_lists_repaint(void)
{
    if (g_list) ep_call(void(void *))::at(FN_COMP_REPAINT, (void *)g_list);
    if (g_pane) ep_call(void(void *))::at(FN_COMP_REPAINT, (void *)g_pane);
}

/* Shorten the list to whole rows above `bottom` (view y) and scroll the cursor
 * row into it; -1 gives the list its full height back. */
void menu_list_fit(int bottom)
{
    int h = LIST_H;

    if (!g_list) return;
    if (bottom >= 0) {
        h = (bottom - (OV_Y + LIST_Y)) / ROW_H * ROW_H;
        if (h < ROW_H) h = ROW_H;
        if (h > LIST_H) h = LIST_H;
    }
    ep_call(void(void *, int, int, int, int))::at(FN_SET_BOUNDS, (void *)g_list, LIST_X, LIST_Y, LIST_W, h);
    update(g_list);
    /* selectRow scrolls only when the selection changes. */
    select_row(g_list, menu_g_row ? menu_g_row - 1 : menu_g_row + 1);
    select_row(g_list, menu_g_row);
}

/* The viewport's first child holds the viewed component, whose y is minus the
 * scroll offset. */
int menu_list_row_y(int row)
{
    int32_t b[4], lb[4];
    uintptr_t vp, holder, content;
    int y;

    if (!g_list) return -1;
    vp = *(uintptr_t *)(g_list + LB_VIEWPORT_OFF);
    holder = vp ? juce_comp_child(vp, 0) : 0;
    content = holder ? juce_comp_child(holder, 0) : 0;
    if (!content || juce_comp_bounds(content, b) != 0) return -1;
    if (juce_comp_bounds(g_list, lb) != 0) return -1;
    y = row * ROW_H + b[1];
    if (y < 0 || y + ROW_H > lb[3]) return -1;
    return OV_Y + LIST_Y + y;
}
