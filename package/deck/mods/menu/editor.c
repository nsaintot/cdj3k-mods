// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * menu/editor.c - text rows: our own juce::TextEditor on the deck's keyboard.
 *
 * The keyboard is the view's. Its key listener is the view, whose handler
 * writes the stock HISTORY NAME setting, so we own that listener slot and edit
 * the row's own buffer instead. The editor is ours for the same reason: every
 * key the view handles is committed from whatever editor sits at view+0x778.
 */
#include "menu/internal.h"

#define FN_KBD_SHOW        ep122_sym(EP122_KBD_SHOW)    /* show(view): seed editor, show keyboard */
#define FN_KBD_HIDE        ep122_sym(EP122_KBD_HIDE)    /* hide(view) */
#define FN_KEY_TO_TEXT     ep122_sym(EP122_KBD_KEY_TO_TEXT)  /* String keyToText(int key), x8 return */
#define FN_EDITOR_CTOR     ep122_sym(EP122_JUCE_EDITOR_CTOR)     /* TextEditor(const String&, wchar) */
#define FN_EDITOR_SETTEXT  ep122_sym(EP122_JUCE_EDITOR_SETTEXT)  /* setText(const String&, bool) */
#define FN_EDITOR_SETFONT  ep122_sym(EP122_JUCE_EDITOR_SETFONT)
#define FN_EDITOR_JUSTIFY  ep122_sym(EP122_JUCE_EDITOR_JUSTIFY)
#define FN_EDITOR_FOCUS    ep122_sym(EP122_JUCE_COMP_GRABFOCUS)
#define ADDR_SKIN_FG       ep122_sym(EP122_SKIN_FG)     /* Colours::white */
#define ADDR_SKIN_BG       ep122_sym(EP122_SKIN_BG)     /* Colours::transparentBlack */

#define EDITOR_ALLOC_SIZE  0x2e8
#define EDITOR_FONT_H      32.0f        /* the view's own editor */
#define ED_JUSTIFY_RIGHT   0xa          /* right | top */
#define ED_COL_BG          0x1000200
#define ED_COL_TEXT        0x1000201
#define ED_COL_HIGHLIGHT   0x1000202
#define ED_COL_CARET       0x1000204
#define ED_COL_OUTLINE     0x1000205
#define ED_COL_FOCUSED     0x1000206
#define ED_COL_SHADOW      0x1000207

/* The stock editor sits on the third list row (SYSTEM > HISTORY NAME). */
#define KBD_STOCK_ROW      2
#define KBD_TOP_DEFAULT    444          /* the keyboard's top edge, view y */
#define KBD_KEY_BACKSPACE  0x4f
#define KBD_KEY_CLEAR      0x51

typedef void (*kbdkey_t)(void *listener, long key);
typedef str_ret_t (*key_to_text_t)(int key);

static uintptr_t g_orig_kbdkey;
static int g_kbd_ok;
static int g_up;
static const struct kit_row *g_row;
static uintptr_t g_editor;
static int g_editor_dy = -1;            /* editor y minus its row's top */
static int g_list_cut;                  /* the list is shortened for the keyboard */
static char g_orig[KIT_ROW_TEXT_MAX];   /* the value on open */

int menu_kbd_is_up(void)
{
    return g_up;
}

const struct kit_row *menu_kbd_row(void)
{
    return g_row;
}

static uintptr_t view_ptr(uintptr_t off)
{
    uintptr_t p = 0;

    if (!menu_g_view || mod_safe_read(menu_g_view + off, &p, sizeof(p)) != 0) return 0;
    return p;
}

/* The keyboard widget reaches juce::Component through a virtual base; the
 * offset to it is in the vtable at -0x18. */
static uintptr_t kbd_component(void)
{
    uintptr_t kbd = view_ptr(VIEW_KBD_OFF), vt = 0;
    int64_t off = 0;

    if (!kbd || mod_safe_read(kbd, &vt, sizeof(vt)) != 0 || !vt) return 0;
    if (mod_safe_read(vt - 0x18, &off, sizeof(off)) != 0) return 0;
    return kbd + off;
}

static void to_front(uintptr_t comp)
{
    if (comp) ((void (*)(void *, int))FN_COMP_TOFRONT)((void *)comp, 0);
}

/* Built once, parented to the view, with the stock editor's bounds and look. */
static uintptr_t editor_get(void)
{
    static const int k_fg[] = { ED_COL_TEXT, ED_COL_CARET };
    static const int k_bg[] = { ED_COL_BG, ED_COL_OUTLINE, ED_COL_HIGHLIGHT,
                                ED_COL_FOCUSED, ED_COL_SHADOW };
    uint8_t name[8] __attribute__((aligned(8)));
    uint32_t fg = 0, bg = 0;
    uintptr_t stock, p;
    font_ret_t font;
    int32_t b[4], justify;
    unsigned i;

    if (g_editor) return g_editor;
    if (!g_kbd_ok || !menu_g_view) return 0;

    stock = view_ptr(VIEW_EDITOR_OFF);
    if (!stock || juce_comp_bounds(stock, b) != 0 || b[2] <= 0 || b[3] <= 0) return 0;

    p = (uintptr_t)calloc(1, EDITOR_ALLOC_SIZE);
    if (!p) return 0;

    ((void (*)(void *))FN_STR_DEFCTOR)(name);
    ((void (*)(void *, void *, int))FN_EDITOR_CTOR)((void *)p, name, 0);
    ((str_dtor_t)FN_STR_DTOR)(name);

    ((void (*)(void *, int, int, int, int))FN_SET_BOUNDS)((void *)p, b[0], b[1], b[2], b[3]);
    g_editor_dy = b[1] - (OV_Y + LIST_Y + KBD_STOCK_ROW * ROW_H);

    font = ((font_build_t)FN_FONT_BUILD)(EDITOR_FONT_H);
    ((void (*)(void *, void *))FN_EDITOR_SETFONT)((void *)p, &font);
    ((void (*)(void *))FN_FONT_DTOR)(&font);

    justify = ED_JUSTIFY_RIGHT;
    ((void (*)(void *, void *))FN_EDITOR_JUSTIFY)((void *)p, &justify);

    mod_safe_read(ADDR_SKIN_FG, &fg, sizeof(fg));
    mod_safe_read(ADDR_SKIN_BG, &bg, sizeof(bg));
    for (i = 0; i < sizeof(k_fg) / sizeof(k_fg[0]); i++)
        ((void (*)(void *, int, void *))FN_COMP_SETCOLOUR)((void *)p, k_fg[i], &fg);
    for (i = 0; i < sizeof(k_bg) / sizeof(k_bg[0]); i++)
        ((void (*)(void *, int, void *))FN_COMP_SETCOLOUR)((void *)p, k_bg[i], &bg);

    ((addvis_t)FN_ADD_VISIBLE)((void *)menu_g_view, (void *)p, -1);
    juce_comp_set_visible(p, 0);
    g_editor = p;
    return p;
}

/* Copy a juce::String's characters, stopping at a line break. */
static size_t str_copy(uintptr_t sp, char *out, size_t cap)
{
    size_t i;

    if (cap == 0) return 0;
    for (i = 0; sp && i < cap - 1; i++) {
        char c = 0;

        if (mod_safe_read(sp + i, &c, 1) != 0 || c == '\0' || c == '\n' || c == '\r') break;
        out[i] = c;
    }
    out[i] = '\0';
    return i;
}

static void editor_show_text(const char *text)
{
    uint8_t s[8] __attribute__((aligned(8)));

    if (!g_editor) return;
    ((str_ctor_t)FN_STR_CTOR)(s, text);
    ((void (*)(void *, void *, int))FN_EDITOR_SETTEXT)((void *)g_editor, s, 1);
    ((str_dtor_t)FN_STR_DTOR)(s);
}

/* The keyboard's listener. While a text row is open we apply the key to the
 * row's buffer and do not chain: the stock handler would commit to HISTORY NAME. */
static void kbd_key(void *self, long key)
{
    char *text;
    size_t cap, n;

    if (!g_up || !g_editor || !g_row) {
        ((kbdkey_t)g_orig_kbdkey)(self, key);
        return;
    }
    text = g_row->text;
    cap  = g_row->text_cap;
    n    = strlen(text);
    if (key == KBD_KEY_BACKSPACE) {
        if (n) text[n - 1] = '\0';
    } else if (key == KBD_KEY_CLEAR) {
        text[0] = '\0';
    } else if (n < cap - 1) {
        str_ret_t r = ((key_to_text_t)FN_KEY_TO_TEXT)((int)key);
        uintptr_t sp = 0;

        memcpy(&sp, &r, sizeof(sp));
        str_copy(sp, text + n, cap - n);
        ((str_dtor_t)FN_STR_DTOR)(&r);
    }
    editor_show_text(text);
    ((void (*)(void *))FN_EDITOR_FOCUS)((void *)g_editor);
}

int menu_kbd_install(void)
{
    g_kbd_ok = FN_KBD_SHOW && FN_KBD_HIDE && FN_KEY_TO_TEXT && FN_EDITOR_CTOR &&
               FN_EDITOR_SETTEXT && FN_EDITOR_SETFONT && FN_EDITOR_JUSTIFY &&
               FN_EDITOR_FOCUS && FN_COMP_TOFRONT && FN_STR_DEFCTOR &&
               mod_patch_vslot("kbdKey", EP122_UTILITY_AS_KBD_LISTENER, 0,
                               (void *)kbd_key, &g_orig_kbdkey) == 0;
    if (!g_kbd_ok) MDBG("modmenu: keyboard unavailable -> text rows are read-only\n");
    return g_kbd_ok;
}

/* Open the keyboard on `r`'s buffer, with our editor over the row's value. */
void menu_kbd_open(const struct kit_row *r)
{
    uintptr_t ed;
    int y;

    if (g_up || !r || !r->text) return;
    ed = editor_get();
    if (!ed) {
        MDBG("modmenu: no editor for %s\n", r->label);
        return;
    }

    ((void (*)(void *))FN_KBD_SHOW)((void *)menu_g_view);
    juce_comp_set_visible(view_ptr(VIEW_EDITOR_OFF), 0);

    /* A row the keyboard would cover is scrolled above it by shortening the
     * list. Only then: the resize rebuilds the list's rows, and a tap that
     * opened the keyboard is still inside one of them. */
    {
        int32_t kb[4];
        uintptr_t kbd = kbd_component();
        int top = kbd && juce_comp_bounds(kbd, kb) == 0 && kb[1] > OV_Y ? kb[1] : KBD_TOP_DEFAULT;

        y = menu_list_row_y(menu_g_row);
        if (y < 0 || y + ROW_H > top) {
            menu_list_fit(top);
            g_list_cut = 1;
            y = menu_list_row_y(menu_g_row);
        }
    }
    if (y < 0) {
        MDBG("modmenu: %s is not on screen -> no keyboard\n", r->label);
        if (g_list_cut) menu_list_fit(-1);
        g_list_cut = 0;
        ((void (*)(void *))FN_KBD_HIDE)((void *)menu_g_view);
        return;
    }

    g_up = 1;
    g_row = r;
    editor_show_text(r->text);
    {
        int32_t b[4];

        if (juce_comp_bounds(ed, b) == 0)
            ((void (*)(void *, int, int, int, int))FN_SET_BOUNDS)
                ((void *)ed, b[0], y + g_editor_dy, b[2], b[3]);
    }
    juce_comp_set_visible(ed, 1);
    /* The overlay is on top of the view's children; raise these above it. */
    to_front(ed);
    to_front(kbd_component());
    ((void (*)(void *))FN_EDITOR_FOCUS)((void *)ed);
    menu_lists_repaint();

    strncpy(g_orig, r->text, sizeof(g_orig) - 1);
    g_orig[sizeof(g_orig) - 1] = '\0';
    MDBG("modmenu: keyboard on %s = \"%s\"\n", r->label, r->text);
}

/* Close it and persist the value if it changed. Safe to call at any time. */
void menu_kbd_close(void)
{
    const struct kit_row *r = g_row;

    if (!g_up) return;
    g_up = 0;
    g_row = NULL;

    /* Hide the keyboard before calling changed(): a popup it raises would sit
     * behind the keyboard. */
    juce_comp_set_visible(g_editor, 0);
    ((void (*)(void *))FN_KBD_HIDE)((void *)menu_g_view);
    if (g_list_cut) menu_list_fit(-1);
    g_list_cut = 0;
    menu_lists_repaint();

    if (!r || strcmp(g_orig, r->text) == 0) return;
    if (r->changed) r->changed();
    mods_settings_save();
    menu_lists_repaint();
    MDBG("modmenu: %s = \"%s\"\n", r->label, r->text);
}
