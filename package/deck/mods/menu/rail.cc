// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * menu/rail.cc - the category rail.
 *
 * One meow::SkinLoadableButton per category, the class of the stock rail. The
 * stock buttons blit a PNG of their label; ours get the same picture drawn at
 * run time: the label in the deck's font on a transparent 101x70 ARGB image,
 * white when on and #969696 when off. The cell colour behind it is painted by
 * the overlay (overlay.cc).
 */
#include "menu/internal.h"
#include "juce/juce.hh"

#define FN_SLB_CTOR        ep122_sym(EP122_MEOW_SLB_CTOR)
#define FN_SLB_SETTOGGLE   ep122_sym(EP122_MEOW_SLB_SETTOGGLE)
#define FN_IDENT_CTOR      ep122_sym(EP122_JUCE_IDENTIFIER_CTOR)
#define FN_IMAGE_CTOR      ep122_sym(EP122_JUCE_IMAGE_CTOR)
#define FN_IMAGE_DTOR      ep122_sym(EP122_JUCE_IMAGE_DTOR)
#define FN_IMAGE_SWAP      ep122_sym(EP122_JUCE_IMAGE_SWAP)
#define FN_GFX_CTOR_IMAGE  ep122_sym(EP122_JUCE_GFX_CTOR_IMAGE)
#define FN_GFX_DTOR        ep122_sym(EP122_JUCE_GFX_DTOR)

#define SLB_ALLOC_SIZE     0x220
#define SLB_IGUIBTN_OFF    0xd0     /* what the listener receives          */
#define SLB_LISTENERS_OFF  0xe8     /* juce::Array<IGuiButton listener*>        */
#define ARRAY_ALLOC_OFF    0x8      /* int numAllocated                          */
#define ARRAY_USED_OFF     0x10     /* int numUsed                               */
#define SLB_IMG_ON_OFF     0x100
#define SLB_IMG_OFF_OFF    0x108

#define IMG_FORMAT_ARGB    2
#define GFX_BYTES          64       /* juce::Graphics, with room to spare */

/* The stock rail lettering: cap height 12 px, 20 px between lines, the block
 * centred on y 34. */
#define RAIL_FONT          14.6f
#define RAIL_LINE_H        20
#define RAIL_TEXT_MID      34
#define RAIL_LINES_MAX     2

static uintptr_t g_btn[KIT_MENU_MAX_CATS];
static int       g_nbtn;

/* ================================================================== */
/* Images                                                             */
/* ================================================================== */

/* Draw `label` ("\n" between lines) into a fresh image. The colour bypasses the
 * theme, so the image holds the same pixels as a stock PNG and is themed when
 * it is drawn. */
static void make_label_image(void *img, const char *label, uint32_t argb)
{
    alignas(16) uint8_t g[GFX_BYTES];
    char lines[RAIL_LINES_MAX][32];
    int n = 0, i;
    const char *p = label;

    while (n < RAIL_LINES_MAX && *p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);

        if (len >= sizeof(lines[0])) len = sizeof(lines[0]) - 1;
        memcpy(lines[n], p, len);
        lines[n][len] = '\0';
        n++;
        p = nl ? nl + 1 : p + strlen(p);
    }

    ep_call(void(void *, int, int, int, int))
        ::at(FN_IMAGE_CTOR, img, IMG_FORMAT_ARGB, RAIL_W, RAIL_H, 1);
    memset(g, 0, sizeof(g));
    ep_call(void(void *, void *))::at(FN_GFX_CTOR_IMAGE, g, img);
    for (i = 0; i < n; i++) {
        int y = RAIL_TEXT_MID - n * RAIL_LINE_H / 2 + i * RAIL_LINE_H;

        mod_gfx_colour(g, argb);
        kit_ui_text(g, lines[i], RAIL_FONT, 0, y, RAIL_W, RAIL_LINE_H, KIT_UI_CENTRED);
    }
    ep_call(void(void *))::at(FN_GFX_DTOR, g);
}

/* Swap a new image into the button field at `off`; the old image is released
 * with the temporary. */
static void set_image(uintptr_t btn, unsigned off, const char *label, uint32_t argb)
{
    alignas(16) uint8_t img[16] = { 0 };

    make_label_image(img, label, argb);
    ep_call(void(void *, void *))::at(FN_IMAGE_SWAP, (void *)(btn + off), img);
    ep_call(void(void *))::at(FN_IMAGE_DTOR, img);
}

/* ================================================================== */
/* Press                                                              */
/* ================================================================== */

/* Append to a button's listener array with realloc, so the app's free()
 * releases it. Do not resolve the app's add function: it has near-identical
 * copies, and none of its callers reaches it within the resolver's 64-call
 * scope. */
static int add_listener(uintptr_t btn, const void *listener)
{
    uintptr_t arr = btn + SLB_LISTENERS_OFF;
    int32_t used = *(int32_t *)(arr + ARRAY_USED_OFF);
    int32_t alloc = *(int32_t *)(arr + ARRAY_ALLOC_OFF);
    const void **data = *(const void ***)arr;

    if (used >= alloc) {
        int32_t n = (used + 1 + (used + 1) / 2 + 8) & ~7;
        const void **p = (const void **)realloc(data, (size_t)n * sizeof(*p));

        if (!p) return -1;
        data = p;
        *(const void ***)arr = data;
        *(int32_t *)(arr + ARRAY_ALLOC_OFF) = n;
    }
    data[used] = listener;
    *(int32_t *)(arr + ARRAY_USED_OFF) = used + 1;
    return 0;
}

/* IGuiButton listener: slot +0x10 on press (touch-down), +0x18 on release. */
static void rail_nop(void *, void *) {}

static void rail_pressed(void *, void *iguibtn)
{
    uintptr_t btn = (uintptr_t)iguibtn - SLB_IGUIBTN_OFF;
    int i;

    for (i = 0; i < g_nbtn; i++) {
        if (g_btn[i] != btn) continue;
        menu_set_focus(FOCUS_RAIL);
        menu_select_cat(i);
        return;
    }
}

static void *const k_listener_vt[] = {
    (void *)rail_nop, (void *)rail_nop, (void *)rail_pressed, (void *)rail_nop,
};
static const void *g_listener = k_listener_vt;

/* ================================================================== */
/* Build and state                                                    */
/* ================================================================== */

/* One button per registered category, in rail order. Built on the first open,
 * when every feature has registered. */
int menu_rail_build(uintptr_t parent)
{
    alignas(16) static uint8_t ident[KIT_MENU_MAX_CATS][16];
    int n = kit_menu_ncats(menu_g_menu), i;

    for (i = 0; i < n && i < KIT_MENU_MAX_CATS; i++) {
        const struct kit_cat *cat = kit_menu_cat(menu_g_menu, i);
        uintptr_t b = (uintptr_t)calloc(1, SLB_ALLOC_SIZE);
        char key[48];

        if (!b) return -1;
        snprintf(key, sizeof(key), "img_btnMod_%s", cat->id);
        ep_call(void(void *, const char *))::at(FN_IDENT_CTOR, ident[i], key);
        ep_call(void(void *, void *, int, void *))::at(FN_SLB_CTOR, (void *)b, ident[i], 1, NULL);
        if (add_listener(b, &g_listener) != 0) return -1;

        set_image(b, SLB_IMG_ON_OFF, cat->label, KIT_UI_WHITE);
        set_image(b, SLB_IMG_OFF_OFF, cat->label, COL_RAIL_OFF);

        ep_call(void(void *, int, int, int, int))
            ::at(FN_SET_BOUNDS, (void *)b, RAIL_X, RAIL_Y + i * RAIL_PITCH, RAIL_W, RAIL_H);
        ep_call(void(void *, void *, int))::at(FN_ADD_VISIBLE, (void *)parent, (void *)b, -1);
        g_btn[i] = b;
        g_nbtn = i + 1;
    }
    return 0;
}

/* The selected category shows its "on" image. */
void menu_rail_show(int c)
{
    int i;

    for (i = 0; i < g_nbtn; i++)
        ep_call(void(void *, int))::at(FN_SLB_SETTOGGLE, (void *)g_btn[i], i == c);
}
