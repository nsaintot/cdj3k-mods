// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * menu/paint.c - row drawing, mirroring the stock paintCell field styling.
 * Font heights come off the live model, colours off the live theme.
 */
#include "menu/internal.h"

/* ================================================================== */
/* Row rendering (mirrors stock paintCell field styling)              */
/* ================================================================== */

/* Draw one text field: font height read off the model at `fonth_off`, colour as
 * juce ARGB with an opaque-white fallback. */
void menu_draw_field(void *self, void *g, uintptr_t fonth_off, uint32_t colour,
                       const char *text, int x, int w, int h, int justif)
{
    float fh = 24.0f;
    uint32_t bits = 0;
    if (mod_safe_read((uintptr_t)self + fonth_off, &bits, sizeof(bits)) == 0) {
        float v; memcpy(&v, &bits, sizeof(v));
        if (v >= 6.0f && v <= 200.0f) fh = v;
    }
    font_ret_t font = ((font_build_t)FN_FONT_BUILD)(fh);   /* s0=fh, x8=&font */
    ((void (*)(void *, void *))FN_G_SETFONT)(g, &font);
    ((void (*)(void *))FN_FONT_DTOR)(&font);

    if (colour == 0) colour = 0xffffffffu;                 /* fallback: opaque white */
    /* Through the draw kit, not setColour directly: setColour is the theme's setFill
     * hook and `colour` from mod_ui() is already theme-resolved, so a raw call applies
     * the palette twice (on SANDSTONE, #7b809d instead of #262944). mod_gfx_colour
     * brackets the call to avoid that. */
    mod_gfx_colour(g, colour);

    uint8_t s[16] __attribute__((aligned(16)));
    int j = justif;
    ((str_ctor_t)FN_STR_CTOR)(s, text);
    ((draw_text_t)FN_DRAW_TEXT)(g, s, x, 0, w, h, &j, 1);
    ((str_dtor_t)FN_STR_DTOR)(s);
}

void menu_draw_mod_row(void *self, void *g, int w, int h, const char *label,
                      const char *value, int sel)
{
    const struct theme_ui *ui = mod_ui();

    if (!menu_g_render_ok) return;
    menu_draw_field(self, g, MODEL_LBL_FONTH_OFF, ui->text_deck, label, 0xe, w, h, 1);
    /* On the selected row the value uses the label colour, as on the deck's own rows:
     * the dim #7d7d7d becomes #ffffff on both the focused blue and unfocused grey. */
    menu_draw_field(self, g, MODEL_VAL_FONTH_OFF, sel ? ui->text_deck : ui->text_value,
                    value, 0, w - 0xe, h, 2);
}

/* The overlay title row: "MOD SETTINGS" in the accent colour, so the list is
 * recognisable as the mod pane. */
void menu_draw_mod_header(void *self, void *g, int w, int h, const char *text)
{
    if (!menu_g_render_ok) return;
    menu_draw_field(self, g, MODEL_LBL_FONTH_OFF, MOD_HEADER_COLOUR, text, 0xe, w, h, 1);
    /* The build version, right-aligned in the value column; the only place a DJ
     * can see it without a shell. drawText ellipsises a long version instead of
     * overlapping the title. */
    menu_draw_field(self, g, MODEL_VAL_FONTH_OFF, MOD_HEADER_COLOUR,
               EP122_MOD_VERSION, 0, w - 0xe, h, 2);
}

