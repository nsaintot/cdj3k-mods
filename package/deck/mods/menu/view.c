// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * menu/view.c - the UTILITY view: repaint, the "Ver." affordance, category state.
 *
 * The overlay is reached by tapping the Ver. label and replaces the stock DJ
 * SETTING list in place instead of opening its own screen.
 */
#include "menu/internal.h"

/* ================================================================== */
/* Repaint + category helpers                                         */
/* ================================================================== */

/* updateContent() the DJ SETTING list (*(view+0x3f8)) so it re-queries
 * getNumRows and repaints under the current menu_g_mod_mode. First confirms the
 * list embeds our TableListBoxModel (captured in paintCell), so a wrong offset
 * cannot drive updateContent into a random object. */
void menu_refresh_djlist(void *view)
{
    uintptr_t list = 0, off, m;
    if (mod_safe_read((uintptr_t)view + VIEW_DJLIST_OFF, &list, sizeof(list)) != 0 || !list || !menu_g_model)
        return;
    for (off = 0; off <= LIST_MODEL_SCAN; off += 8) {
        m = 0;
        if (mod_safe_read(list + off, &m, sizeof(m)) == 0 && m == menu_g_model) {
            uintptr_t vt = 0;                              /* logs the ListBox vtable for the bg recolor */
            mod_safe_read(list, &vt, sizeof(vt));
            MDBG("djlist=%#lx vt=%#lx model@+%#lx\n",
                 (unsigned long)list, (unsigned long)vt, (unsigned long)off);
            /* Size first: updateContent lays the rows out against the bounds the
             * list has at that moment. */
            menu_list_fit(list, menu_g_mod_mode);
            ((void (*)(void *))FN_UPDATECONTENT)((void *)list);
            /* updateContent only re-lays out when the row count changes, which a
             * dismiss that stays on DJ SETTING does not, so repaint explicitly to
             * have the rows call paintCell again with the new mode. */
            ((void (*)(void *))FN_REPAINT)((void *)list);
            return;
        }
    }
}

/* Append "-m" to "Ver.X.XX" to show the shim is loaded.
 *
 * Idempotent by checking the current text for the suffix, so it survives the
 * label being rebuilt and never double-tags. (setText also returns early on an
 * unchanged lastTextValue, but that costs a juce::String per call, and this runs
 * from every view hook.) */
static void mod_tag_ver(uintptr_t lbl)
{
    const size_t taglen = sizeof(VER_MOD_TAG) - 1;
    char cur[40], tagged[48];
    uint8_t s[16] __attribute__((aligned(16)));
    size_t len;

    if (!FN_LABEL_SETTEXT || !FN_STR_CTOR || !FN_STR_DTOR) return;
    if (juce_string_read(lbl + LABEL_LASTTEXT_OFF, cur, sizeof(cur)) != 0) return;

    len = strlen(cur);
    if (len == 0 || len + taglen >= sizeof(tagged)) return;
    if (len >= taglen && memcmp(cur + len - taglen, VER_MOD_TAG, taglen) == 0)
        return;                              /* already ours */

    memcpy(tagged, cur, len);
    memcpy(tagged + len, VER_MOD_TAG, taglen + 1);
    ((str_ctor_t)FN_STR_CTOR)(s, tagged);
    ((void (*)(void *, const void *, int))FN_LABEL_SETTEXT)((void *)lbl, s, NOTIFY_NONE);
    ((str_dtor_t)FN_STR_DTOR)(s);

    /* Log the bounds: the label is right-justified with a skin-defined width, so
     * the suffix grows leftward and JUCE squeezes (drawFittedText, min scale 0.7)
     * before truncating. Taps span x 1127..1236, so the text is ~109 px
     * wide; a wider label has room. */
    {
        int32_t b[4] = { 0, 0, 0, 0 };
        mod_safe_read(lbl + COMP_BOUNDS_OFF, b, sizeof(b));
        MDBG("ver: \"%s\" -> \"%s\" (label %dx%d at (%d,%d))\n",
             cur, tagged, b[2], b[3], b[0], b[1]);
    }
}

/* Recolour the "Ver.X.XX" label so the hidden toggle is discoverable: brighter
 * than the skin grey at rest, accent while armed. setColour repaints by itself,
 * and a wrong colour id would do nothing rather than misdraw. */
void menu_style_ver(void *view)
{
    uintptr_t lbl = 0;
    uint32_t colour;
    if (!view) return;
    if (mod_safe_read((uintptr_t)view + VIEW_VERLBL_OFF, &lbl, sizeof(lbl)) != 0 || !lbl)
        return;
    colour = menu_g_mod_mode ? VER_COLOUR_ARMED : VER_COLOUR_IDLE;
    ((void (*)(void *, int, void *))FN_COMP_SETCOLOUR)((void *)lbl, LABEL_TEXT_COLOUR_ID, &colour);
    mod_tag_ver(lbl);
}

/* Remember the view (only these hooks receive it) and keep the Ver affordance on.
 * It is also the Component the kit's popup is drawn over, so any mod can raise a
 * message once UTILITY has been on screen. */
void menu_note_view(void *view)
{
    if (!view) return;
    menu_g_view = (uintptr_t)view;
    kit_popup_set_parent(menu_g_view);
    menu_style_ver(view);
}

/* Read one of the view's component pointers (list/model), 0 if unreadable. */
uintptr_t menu_view_ptr(uintptr_t view, uintptr_t off)
{
    uintptr_t p = 0;
    if (!view || mod_safe_read(view + off, &p, sizeof(p)) != 0) return 0;
    return p;
}

/* Ver-tap: flip the overlay flag and refresh the list to match. */
void menu_toggle_overlay(void *view)
{
    int32_t cat = 0;
    /* Close before the dismiss, so an address warning raised by the close is not
     * left over a stock page. */
    menu_kbd_close();                   /* the editor outlives the overlay too */
    kit_popup_dismiss();                /* never leave one stranded over a stock page */

    /* The rows are all-or-nothing (see kit_menu_problem). With a broken row list,
     * show the reason instead of arming onto an empty list. */
    if (!menu_g_mod_mode) {
        const char *why = kit_menu_problem();

        if (why) {
            const char *lines[2] = { "MOD SETTINGS unavailable", why };

            kit_popup_show(lines, 2);
            MDBG("Ver toggle refused: %s\n", why);
            return;
        }
    }
    menu_g_mod_mode = !menu_g_mod_mode;
    if (menu_g_mod_mode) {
        /* The overlay draws on the DJ SETTING list, so switch to that category first
         * (with the mode already set, so its updateContent renders our rows). */
        mod_safe_read((uintptr_t)view + VIEW_CAT_OFF, &cat, sizeof(cat));
        if (cat != 0) {
            ((void (*)(void *))FN_ENTER_DJSETTING)(view);
            MDBG("Ver toggle: cat %d -> DJ SETTING\n", cat);
        }
    }
    menu_refresh_djlist(view);
    menu_style_ver(view);
    if (menu_g_mod_mode) {
        /* Select the first real row explicitly. The stock row-0 selection stays in
         * place otherwise, and with an unchanged index JUCE never fires
         * selectedRowsChanged, so neither the title-row bounce nor the right-pane
         * rebuild would run. */
        uintptr_t list = menu_view_ptr((uintptr_t)view, VIEW_DJLIST_OFF);
        menu_g_setting_row = MOD_ROW_FIRST;
        if (list) ((selectrow_t)FN_SELECT_ROW)((void *)list, MOD_ROW_FIRST, 0, 1);
    }
    MDBG("Ver toggle -> mode=%d\n", menu_g_mod_mode);
}

