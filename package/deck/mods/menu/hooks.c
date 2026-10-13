// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * menu/hooks.c - the UtilityView slots, the Ver label and the install.
 *
 * While the overlay is closed, the hooks watch for a Ver tap and otherwise call
 * the stock handler. While it is open, the rotary, the rotary push and BACK
 * drive the overlay instead of the view.
 */
#include "menu/internal.h"
#include "kit/mod.h"

typedef int64_t (*view_evt_t)(void *view, long a, long b);
typedef int64_t (*view_btn_t)(void *view, uint64_t id, long type);
typedef void    (*mousedown_t)(void *self, void *event);

static uintptr_t g_orig_mousedown, g_orig_shown, g_orig_button, g_orig_push, g_orig_turn;

/* ================================================================== */
/* Ver label                                                          */
/* ================================================================== */

static void label_set_text(uintptr_t lbl, const char *text)
{
    uint8_t s[16] __attribute__((aligned(16)));

    ((str_ctor_t)FN_STR_CTOR)(s, text);
    ((void (*)(void *, const void *, int))FN_LABEL_SETTEXT)((void *)lbl, s, NOTIFY_NONE);
    ((str_dtor_t)FN_STR_DTOR)(s);
}

/* Append "-m" to "Ver.X.XX": the on-screen sign that the mods loaded. Only the
 * label's copy changes; the version the deck reports over the network does not. */
static void tag_ver(uintptr_t lbl)
{
    const size_t taglen = sizeof(VER_MOD_TAG) - 1;
    char cur[40], tagged[48];
    size_t len;

    if (juce_string_read(lbl + LABEL_LASTTEXT_OFF, cur, sizeof(cur)) != 0) return;
    len = strlen(cur);
    if (len == 0 || len + taglen >= sizeof(tagged)) return;
    if (len >= taglen && memcmp(cur + len - taglen, VER_MOD_TAG, taglen) == 0) return;

    memcpy(tagged, cur, len);
    memcpy(tagged + len, VER_MOD_TAG, taglen + 1);
    label_set_text(lbl, tagged);
}

/* The screen title ("UTILITY") is a child of the view with the Ver label's class,
 * in the left half of the header band. */
static uintptr_t g_title;
static int32_t   g_title_bounds[4];
static char      g_title_text[32];

static uintptr_t find_title(uintptr_t ver)
{
    uintptr_t cls = juce_comp_class(ver);
    int n = juce_comp_nchild(menu_g_view), i;

    for (i = 0; i < n; i++) {
        uintptr_t c = juce_comp_child(menu_g_view, i);
        int32_t b[4];

        if (!c || c == ver || juce_comp_class(c) != cls || juce_comp_bounds(c, b) != 0)
            continue;
        if (b[0] >= OV_W / 2 || b[1] >= OV_Y) continue;
        if (juce_string_read(c + LABEL_LASTTEXT_OFF, g_title_text, sizeof(g_title_text)) != 0)
            continue;
        memcpy(g_title_bounds, b, sizeof(b));
        return c;
    }
    return 0;
}

/* "MOD SETTINGS" while the overlay is open, in a box wide enough for it; the
 * stock text and box otherwise. */
static void style_title(uintptr_t ver)
{
    const char *want;
    char cur[32];
    int w;

    if (!g_title) g_title = find_title(ver);
    if (!g_title) return;
    want = menu_g_open ? MENU_TITLE : g_title_text;
    if (juce_string_read(g_title + LABEL_LASTTEXT_OFF, cur, sizeof(cur)) != 0 ||
        strcmp(cur, want) == 0)
        return;
    w = menu_g_open ? MENU_TITLE_W : g_title_bounds[2];
    ((void (*)(void *, int, int, int, int))FN_SET_BOUNDS)
        ((void *)g_title, g_title_bounds[0], g_title_bounds[1], w, g_title_bounds[3]);
    label_set_text(g_title, want);
}

/* The Ver label: brighter than the skin grey at rest so it reads as tappable,
 * the accent while the overlay is open. Also sets the screen title. */
void menu_style_ver(void)
{
    uintptr_t lbl = 0;
    uint32_t colour;

    if (!menu_g_view ||
        mod_safe_read(menu_g_view + VIEW_VERLBL_OFF, &lbl, sizeof(lbl)) != 0 || !lbl)
        return;
    colour = menu_g_open ? mod_ui()->accent : VER_COLOUR_IDLE;
    ((void (*)(void *, int, void *))FN_COMP_SETCOLOUR)((void *)lbl, LABEL_TEXT_COLOUR_ID, &colour);
    tag_ver(lbl);
    style_title(lbl);
}

/* Set when a second UtilityView appears after the overlay was built on the
 * first: the overlay's components belong to the old view, so it stays shut. */
static int g_stale;

/* Remember the view; it is also the parent of the kit's popup. */
static void note_view(void *view)
{
    if (!view || g_stale) return;
    if (menu_g_view != (uintptr_t)view) {
        if (menu_g_view && menu_overlay_built()) {
            MERR("modmenu: UtilityView replaced after the overlay was built -> disabled\n");
            g_stale = 1;
            menu_g_open = 0;            /* without touching the old components */
            return;
        }
        menu_g_view = (uintptr_t)view;
        kit_popup_set_parent(menu_g_view);
    }
    menu_style_ver();
}

/* ================================================================== */
/* View hooks                                                         */
/* ================================================================== */

/* vt+0x28. The stock handler only dismisses a popup; a tap on the Ver label
 * opens or closes the overlay. */
static void hook_mousedown(void *view, void *event)
{
    float pos[2] = { -1.0f, -1.0f };
    int x, y;

    ((mousedown_t)g_orig_mousedown)(view, event);
    note_view(view);
    if (kit_popup_is_up()) {
        kit_popup_dismiss();
        return;
    }
    if (!event || mod_safe_read((uintptr_t)event + MEVENT_POS_OFF, pos, sizeof(pos)) != 0)
        return;
    x = (int)pos[0];
    y = (int)pos[1];
    if (g_stale || x < VER_HIT_X_MIN || y < 0 || y > VER_HIT_Y_MAX) return;
    if (menu_g_open) menu_close();
    else menu_open();
}

/* vt+0x178: the view is shown (1) or hidden (0). Leaving UTILITY closes the
 * overlay. */
static int64_t hook_shown(void *view, long shown, long c)
{
    note_view(view);
    kit_popup_dismiss();
    if (!shown) menu_close();
    return ((view_evt_t)g_orig_shown)(view, shown, c);
}

/* vt+0x188: buttons. BACK moves one column left; from the rail it closes the
 * overlay. A button sends more than one event; only the BACK type acts. */
static int64_t hook_button(void *view, uint64_t id, long type)
{
    note_view(view);
    if (!menu_g_open || id != BTN_ID_BACK)
        return ((view_btn_t)g_orig_button)(view, id, type);
    if (type != BTN_TYPE_BACK) return 1;
    if (kit_popup_is_up()) kit_popup_dismiss();
    else menu_back();
    return 1;
}

/* vt+0x190: rotary push, one event on press and one on release. Only the press
 * acts: the release would dismiss a popup the press has just raised. */
static int64_t hook_push(void *view, long a, long pressed)
{
    note_view(view);
    if (!menu_g_open) return ((view_evt_t)g_orig_push)(view, a, pressed);
    if (!pressed) return 1;
    if (kit_popup_is_up()) kit_popup_dismiss();
    else menu_push();
    return 1;
}

/* vt+0x198: rotary turn. A turn while a popup is up only dismisses it. */
static int64_t hook_turn(void *view, long a, long delta)
{
    note_view(view);
    if (!menu_g_open) {
        kit_popup_dismiss();
        return ((view_evt_t)g_orig_turn)(view, a, delta);
    }
    if (kit_popup_is_up()) kit_popup_dismiss();
    else menu_turn((int)delta);
    return 1;
}

/* ================================================================== */
/* ABOUT rows                                                         */
/* ================================================================== */

static const struct kit_cat k_cat_about = { "about", "ABOUT", KIT_ORDER_ABOUT };

static const struct kit_row k_about[] = {
    { .label = "VERSION", .idx = 0, .info = EP122_MOD_VERSION },
    { .label = "BUILD",   .idx = 1, .info = EP122_MOD_BUILD },
};

static void about_menu(struct kit_menu *menu)
{
    kit_menu_rows(menu, &k_cat_about, k_about, (int)(sizeof(k_about) / sizeof(k_about[0])));
}

/* ================================================================== */
/* Install                                                            */
/* ================================================================== */

static int menu_install(void)
{
    const int hooks_wanted = 5;
    int ok = 0;

    if (!menu_overlay_ok() || !FN_COMP_SETCOLOUR || !FN_LABEL_SETTEXT) {
        MERR("modmenu: primitives unavailable -> disabled\n");
        return -1;
    }
    menu_kbd_install();

    /* All or nothing: a subset would send some inputs to the view behind the overlay. */
    ok += mod_patch_vslot("mouseDown", EP122_UTILITY_VIEW, 0x28,
                          (void *)hook_mousedown, &g_orig_mousedown) == 0;
    ok += mod_patch_vslot("shown", EP122_UTILITY_VIEW, 0x178,
                          (void *)hook_shown, &g_orig_shown) == 0;
    ok += mod_patch_vslot("button", EP122_UTILITY_VIEW, 0x188,
                          (void *)hook_button, &g_orig_button) == 0;
    ok += mod_patch_vslot("push", EP122_UTILITY_VIEW, 0x190,
                          (void *)hook_push, &g_orig_push) == 0;
    ok += mod_patch_vslot("turn", EP122_UTILITY_VIEW, 0x198,
                          (void *)hook_turn, &g_orig_turn) == 0;
    if (ok != hooks_wanted) {
        MERR("modmenu: %d/%d hooks -> refused\n", ok, hooks_wanted);
        return -1;
    }
    MDBG("modmenu: installed\n");
    return 0;
}

KIT_MOD(k_mod_menu,
        .name = "menu", .prio = 20, .install = menu_install, .menu = about_menu,
        .what = "MOD SETTINGS overlay on the UTILITY screen");
