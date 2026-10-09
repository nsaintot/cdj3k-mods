// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * draw.cc - the shared drawing kit. draw.h documents the constants and each
 * function.
 */
#include "juce/draw.h"
#include "juce/juce.hh"

/* Message thread only, so a plain int suffices. A counter, not a flag, because
 * a bracketed stock paint can call back into the kit. */
static int g_drawing;

/* 1 while the current theme puts the UI on a light ground. See
 * MOD_CHECKER_ALT_Q8. */
static int g_light_ground;

void mod_draw_ground(int light) { g_light_ground = !!light; }

void mod_draw_enter(void) { g_drawing++; }
void mod_draw_leave(void) { if (g_drawing > 0) g_drawing--; }
int  mod_drawing(void)    { return g_drawing > 0; }

void mod_gfx_colour(void *g, uint32_t argb)
{
    /* juce::Colour is a bare ARGB word, taken by reference. */
    uint32_t c = argb;

    /* setColour reaches setFill synchronously, so the bracket spans one call. */
    mod_draw_enter();
    ep_call(void(void *, void *))::at(MOD_FN_GFX_SETCOLOUR, g, &c);
    mod_draw_leave();
}

void mod_gfx_fill(void *g, int x, int y, int w, int h)
{
    ep_call(void(void *, int, int, int, int))::at(MOD_FN_GFX_FILLRECT, g, x, y, w, h);
}

void mod_gfx_text(void *g, const char *text, float font_h, uint32_t argb,
                  int x, int y, int w, int h, int justification)
{
    int j = justification;

    if (!MOD_FN_GFX_SETFONT || !MOD_FN_GFX_DRAWTEXT || !FN_FONT_BUILD ||
        !FN_STR_CTOR)
        return;

    {
        juce::Font font(font_h);
        ep_call(void(void *, void *))::at(MOD_FN_GFX_SETFONT, g, font);
    }

    mod_gfx_colour(g, argb);

    juce::String s(text);
    /* The trailing 1 is juce's useEllipsesIfTooBig: text that does not fit is
     * truncated with an ellipsis instead of squashed. */
    ep_call(void(void *, void *, int, int, int, int, int *, int))
        ::at(MOD_FN_GFX_DRAWTEXT, g, s, x, y, w, h, &j, 1);
}

uint32_t mod_colour_scale(uint32_t argb, uint32_t q8)
{
    uint32_t r = (argb >> 16) & 0xffu, g = (argb >> 8) & 0xffu, b = argb & 0xffu;

    r = (r * q8 + 128u) >> 8;
    g = (g * q8 + 128u) >> 8;
    b = (b * q8 + 128u) >> 8;
    return (argb & 0xff000000u) | (r << 16) | (g << 8) | b;
}

uint32_t mod_colour_lift(uint32_t argb, uint32_t q8)
{
    uint32_t r = (argb >> 16) & 0xffu, g = (argb >> 8) & 0xffu, b = argb & 0xffu;

    /* Each channel moves the same fraction of its own headroom, which keeps
     * the hue. */
    r += ((0xffu - r) * q8 + 128u) >> 8;
    g += ((0xffu - g) * q8 + 128u) >> 8;
    b += ((0xffu - b) * q8 + 128u) >> 8;
    return (argb & 0xff000000u) | (r << 16) | (g << 8) | b;
}

void mod_checker_pair(void *g, int x, int y, int w, int h,
                      uint32_t light, uint32_t dark)
{
    int cy, cx;

    /* w and h come from a component's bounds read out of app memory, so they
     * are untrusted. Out of range, the loop below would issue hundreds of
     * thousands of fills and hang the message thread. Nothing on this panel is
     * larger than the screen. */
    if (w <= 0 || h <= 0 || w > MOD_DRAW_MAX || h > MOD_DRAW_MAX) return;
    mod_gfx_colour(g, light);
    mod_gfx_fill(g, x, y, w, h);
    mod_gfx_colour(g, dark);
    /* Even rows start one cell in, so the rect's top-left cell is light. That
     * is the deck's phase: PREVIEW at x=894 and the font-size button at x=1018
     * both start light, and 124 px is an odd number of 4 px cells, so the
     * pattern is phased per component, not to the screen.
     *
     * Cells at the right or bottom edge are clipped, not skipped, so the
     * pattern reaches the border when the size is not a multiple of the cell. */
    for (cy = 0; cy * MOD_CHECKER_CELL < h; cy++) {
        int cyy = cy * MOD_CHECKER_CELL;
        int ch  = h - cyy < MOD_CHECKER_CELL ? h - cyy : MOD_CHECKER_CELL;

        for (cx = !(cy & 1); cx * MOD_CHECKER_CELL < w; cx += 2) {
            int cxx = cx * MOD_CHECKER_CELL;
            int cw  = w - cxx < MOD_CHECKER_CELL ? w - cxx : MOD_CHECKER_CELL;

            mod_gfx_fill(g, x + cxx, y + cyy, cw, ch);
        }
    }
}

uint32_t mod_checker_alt(uint32_t base)
{
    /* Down from the surface on a dark ground, up on a light one: inverting the
     * stock pair swaps which of the two is lighter. draw.h has the levels. */
    return g_light_ground ? mod_colour_lift(base, MOD_CHECKER_ALT_LIGHT_Q8)
                          : mod_colour_scale(base, MOD_CHECKER_ALT_Q8);
}

void mod_checker_lift2(void *g, int x, int y, int w, int h,
                       uint32_t base, uint32_t alt, uint32_t q8)
{
    /* Lift both after pairing them; draw.h explains why. */
    mod_checker_pair(g, x, y, w, h,
                     mod_colour_lift(base, q8), mod_colour_lift(alt, q8));
}

void mod_checker_lift(void *g, int x, int y, int w, int h,
                      uint32_t light, uint32_t q8)
{
    mod_checker_lift2(g, x, y, w, h, light, mod_checker_alt(light), q8);
}

void mod_checker(void *g, int x, int y, int w, int h, uint32_t light)
{
    mod_checker_lift(g, x, y, w, h, light, 0);
}

void mod_btn_bar(void *g, int x, int y, int w, int h, uint32_t col)
{
    if (w < MOD_BTN_BAR_W || h < MOD_BTN_BAR_H) return;
    mod_gfx_colour(g, col);
    mod_gfx_fill(g, x + (w - MOD_BTN_BAR_W) / 2, y + h - MOD_BTN_BAR_H,
                 MOD_BTN_BAR_W, MOD_BTN_BAR_H);
}

void mod_checker_plate(void *g, int x, int y, int w, int h,
                       uint32_t base, uint32_t q8)
{
    const struct theme_ui *ui = mod_ui();
    uint32_t alt = base == ui->surface ? ui->surface2
                 : base == ui->accent  ? ui->accent2
                                       : mod_checker_alt(base);

    mod_checker_lift2(g, x, y, w, h, base, alt, q8);
}
