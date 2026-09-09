// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * menu/editor.c - text rows: our own juce::TextEditor on the deck's keyboard.
 *
 * The keyboard is the deck's, the editor is not: typing into the view's own
 * editor would rewrite the stock HISTORY NAME setting, so we own the keyboard's
 * listener slot and commit into our own buffer. See menu_editor_get().
 */
#include "menu/internal.h"

/* ================================================================== */
/* Text rows: the software keyboard                                   */
/* ================================================================== */

static int g_kbd_up;                    /* our text row currently owns the keyboard */
static const struct kit_row *g_kbd_row; /* the row it is editing                   */

/* Both read by the paint hook, which must not draw a value the editor is already
 * drawing over. Accessors rather than externs: what is being edited is this
 * file's business, and everywhere else only needs to ask whether a given row is
 * the one. */
int menu_kbd_is_up(void)
{
    return g_kbd_up;
}

const struct kit_row *menu_kbd_row(void)
{
    return g_kbd_row;
}
static uintptr_t g_editor;        /* our juce::TextEditor, built once and kept   */
/* The value on open, to tell a real edit apart. Sized for any text row rather
 * than for one feature's field. */
static char g_kbd_orig[KIT_ROW_TEXT_MAX];

/* Build our editor on first use and keep it: it is parented to the UTILITY view, which
 * outlives every visit, and the constructor allocates a caret and a viewport that tearing
 * it down again would only have to unwind. It takes the view editor's own bounds, so
 * standing one in for the other moves nothing on screen. */
static uintptr_t mod_editor_get(void)
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
    if (!menu_g_editor_ok || !menu_g_view) return 0;

    stock = menu_view_ptr(menu_g_view, VIEW_EDITOR_OFF);
    if (!stock || mod_safe_read(stock + COMP_BOUNDS_OFF, b, sizeof(b)) != 0) return 0;
    if (b[2] <= 0 || b[3] <= 0) return 0;

    p = (uintptr_t)calloc(1, EDITOR_ALLOC_SIZE);
    if (!p) return 0;

    ((void (*)(void *))FN_STR_DEFCTOR)(name);       /* the component name -- unused, empty */
    ((void (*)(void *, void *, int))FN_EDITOR_CTOR)((void *)p, name, 0);
    ((str_dtor_t)FN_STR_DTOR)(name);

    ((void (*)(void *, int, int, int, int))FN_SET_BOUNDS)((void *)p, b[0], b[1], b[2], b[3]);

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
    juce_comp_set_visible(p, 0);        /* menu_kbd_open decides when it is on screen */
    g_editor = p;
    MDBG("editor: built %#lx at {%d,%d,%d,%d} fg=%08x bg=%08x\n",
         (unsigned long)p, b[0], b[1], b[2], b[3], fg, bg);
    return p;
}

/* Copy the characters of a juce::String. It is one pointer to its UTF-8 bytes (the
 * refcount/length header sits BEFORE them), so they are readable directly -- probed
 * rather than dereferenced, as everywhere else here. Returns the length written. */
size_t menu_str_copy(uintptr_t sp, char *out, size_t cap)
{
    size_t i;

    if (cap == 0) return 0;
    for (i = 0; sp && i < cap - 1; i++) {
        char c = 0;
        if (mod_safe_read(sp + i, &c, 1) != 0) break;
        if (c == '\0') break;
        if (c == '\n' || c == '\r') break;     /* never let one line become two on save */
        out[i] = c;
    }
    out[i] = '\0';
    return i;
}

/* Put our buffer on screen. Our editor is only ever written from `g_kbd_target`, which
 * is the row's own value, so the buffer stays the single source of truth. */
static void mod_editor_show_text(const char *text)
{
    uint8_t s[8] __attribute__((aligned(8)));

    if (!g_editor) return;
    ((str_ctor_t)FN_STR_CTOR)(s, text);
    ((void (*)(void *, void *, int))FN_EDITOR_SETTEXT)((void *)g_editor, s, 1);
    ((str_dtor_t)FN_STR_DTOR)(s);
}

/* The keyboard's listener callback. While our row owns the keyboard we do the edit the
 * stock body would have done, but against our own buffer and our own editor -- and we do
 * NOT chain, which is the whole point: the stock body ends by committing the text to
 * HISTORY NAME, and not running it is the only thing that stops that. Every other key,
 * and every stock use of this keyboard, falls straight through. */
void menu_kbd_key(void *self, long key)
{
    char *text;
    size_t cap, n;

    if (!g_kbd_up || !g_editor || !g_kbd_row) {
        ((kbdkey_t)menu_g_orig_kbdkey)(self, key);
        return;
    }

    text = g_kbd_row->text;
    cap  = g_kbd_row->text_cap;
    n    = strlen(text);
    if (key == KBD_KEY_BACKSPACE) {
        if (n) text[n - 1] = '\0';
    } else if (key == KBD_KEY_CLEAR) {
        text[0] = '\0';
    } else if (n < cap - 1) {
        str_ret_t r = ((key_to_text_t)FN_KEY_TO_TEXT)((int)key);
        uintptr_t sp = 0;
        memcpy(&sp, &r, sizeof(sp));
        menu_str_copy(sp, text + n, cap - n);
        ((str_dtor_t)FN_STR_DTOR)(&r);
    }

    mod_editor_show_text(text);
    ((void (*)(void *))FN_EDITOR_FOCUS)((void *)g_editor);
}

/* Park the editor on `row` (KBD_STOCK_ROW restores the stock spot). The stock y is
 * captured on first sight, so repeated opens shift from that baseline instead of
 * creeping, and nothing needs to know where the SYSTEM pane happens to put it. */
static void mod_kbd_move_editor(uintptr_t editor, int row)
{
    static int32_t stock_y = -1;
    int32_t b[4];

    if (!editor || mod_safe_read(editor + COMP_BOUNDS_OFF, b, sizeof(b)) != 0) return;
    if (b[2] <= 0 || b[3] <= 0 || b[3] > 4 * MOD_ROW_H) return;   /* not a row-sized editor */
    if (stock_y < 0) stock_y = b[1];
    ((void (*)(void *, int, int))FN_SET_TOPLEFT)
        ((void *)editor, b[0], stock_y + (row - KBD_STOCK_ROW) * MOD_ROW_H);
}

/* Clear the right pane for the duration of the edit, the way the stock text setting does:
 * with the keyboard up there is no value to choose, so leaving two radio buttons there
 * reads as a live control. Stock gets this by handing its own pane ten empty strings and
 * setting a byte at model+0x48 -- but that is the SYSTEM pane's model (view+0x3e8) and a
 * different class from the one our overlay drives (view+0x3c8), so rather than poke an
 * offset into a layout we have not established, take the list off screen. Same flat
 * result, and nothing of the model's is touched. */
void menu_pane_show(uintptr_t view, int visible)
{
    juce_comp_set_visible(menu_view_ptr(view, VIEW_RIGHT_LIST_OFF), visible);
}

/* Open the keyboard on `r`'s buffer. The stock show runs untouched -- it seeds and reveals
 * the view's OWN editor and raises the keyboard -- and we then simply take that editor's
 * place on screen. The view's editor keeps the real HISTORY NAME the whole time; nothing
 * of the view's is written, because the keys come to us instead (menu_kbd_key). */
void menu_kbd_open(const struct kit_row *r)
{
    uintptr_t ours;

    if (!menu_g_kbd_ok || !menu_g_view || !r || !r->text || g_kbd_up) return;

    ours = mod_editor_get();
    if (!ours) {
        MDBG("keyboard: no editor of our own -> %s stays read-only\n", r->label);
        return;
    }

    ((void (*)(void *))FN_KBD_SHOW)((void *)menu_g_view);
    juce_comp_set_visible(menu_view_ptr(menu_g_view, VIEW_EDITOR_OFF), 0);
    menu_pane_show(menu_g_view, 0);

    g_kbd_up = 1;
    g_kbd_row = r;
    mod_editor_show_text(r->text);
    {
        /* A row under the keyboard is scrolled up first; the editor then sits on
         * the slot it lands in. The scroll rebuilds the right pane, hence the
         * second hide. */
        int shown = menu_list_fit_above_kbd(
            menu_view_ptr(menu_g_view, VIEW_DJLIST_OFF), menu_g_setting_row);

        menu_pane_show(menu_g_view, 0);
        mod_kbd_move_editor(ours, shown);
    }
    juce_comp_set_visible(ours, 1);
    ((void (*)(void *))FN_EDITOR_FOCUS)((void *)ours);

    strncpy(g_kbd_orig, r->text, sizeof(g_kbd_orig) - 1);
    g_kbd_orig[sizeof(g_kbd_orig) - 1] = '\0';
    MDBG("keyboard: opened on %s = \"%s\" (editor %#lx)\n",
         r->label, r->text, (unsigned long)ours);
}

/* Close it. The row's value is already current -- each key edits it in place -- so the
 * only thing left is to persist it if it moved. Called from every path that navigates
 * away, so it must be idempotent and must not care why it was called. */
void menu_kbd_close(void)
{
    const struct kit_row *r = g_kbd_row;

    if (!g_kbd_up) return;
    g_kbd_up = 0;
    g_kbd_row = NULL;

    /* The keyboard comes down BEFORE the commit: a feature's changed() may put a
     * message on the glass, and a popup raised while the keyboard is up appears
     * behind it. The stock hide only knows about the view's own editor, so ours
     * comes off separately. */
    juce_comp_set_visible(g_editor, 0);
    menu_pane_show(menu_g_view, 1);
    ((void (*)(void *))FN_KBD_HIDE)((void *)menu_g_view);
    /* Back to full height if the open cut it down for the keyboard. The whole
     * refresh, not just the resize: a list that was scrolled and overflowing
     * keeps its scroll indicators and offset until updateContent re-lays it out. */
    menu_refresh_djlist((void *)menu_g_view);

    if (!r || strcmp(g_kbd_orig, r->text) == 0) {
        MDBG("keyboard: closed, value unchanged\n");
        return;
    }
    /* changed() first: it is where a feature refuses what was typed, so the value
     * that lands on disk is the corrected one and never the bad one. */
    if (r->changed) r->changed();
    mods_settings_save();
    MDBG("keyboard: committed \"%s\"\n", r->text);
}


