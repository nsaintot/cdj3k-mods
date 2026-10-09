// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * xpad/pane.c - the readout beside the pad.
 *
 * Floating: a gutter and no outline, so it does not look like a seventh brick.
 * It is driven by the strip, the two borrowed buttons and the borrowed knob, and
 * does not respond to touch.
 *
 * It shows both values, loop and pitch, because the hand covers the brick that
 * shows them. Unlabelled: a fraction and a signed percentage are
 * self-explanatory. The percentage is of the range, not the play rate; see
 * XP_PCT_FULL.
 */
#include "xpad/xpad.h"

/* A lamp and its legend, like the RMX-1000's illuminated buttons with the name
 * beside them.
 *
 * The lamp carries the state; the word changes only in brightness, from the
 * deck's dim lettering to full white. Unlit, the lamp is a one-pixel `edge`
 * frame with no fill.
 *
 * Both lamps light the same way. Do not make OVERDUB blink: next to a steady
 * lamp it reads as a control mid-change, not as recording. */
static void xpad_flag(void *g, const char *text, int lit,
                      int x, int y, int w, int h)
{
    const struct theme_ui *ui = mod_ui();
    int lamp = h - 2 * XP_LAMP_PAD;
    int lx = x, ly = y + XP_LAMP_PAD;

    if (lit) {
        mod_gfx_colour(g, ui->xpad_on);
        mod_gfx_fill(g, lx, ly, lamp, lamp);
    } else {
        mod_gfx_colour(g, ui->edge);
        mod_gfx_fill(g, lx, ly, lamp, XP_FLAG_EDGE);
        mod_gfx_fill(g, lx, ly + lamp - XP_FLAG_EDGE, lamp, XP_FLAG_EDGE);
        mod_gfx_fill(g, lx, ly, XP_FLAG_EDGE, lamp);
        mod_gfx_fill(g, lx + lamp - XP_FLAG_EDGE, ly, XP_FLAG_EDGE, lamp);
    }
    mod_gfx_text(g, text, XP_FONT_FLAG, lit ? ui->text : ui->text_dim,
                 lx + lamp + XP_LAMP_GAP, y, w - lamp - XP_LAMP_GAP, h,
                 JUCE_JUSTIFY_MID_L);
}

static void xpad_pane_paint(void *self, void *g)
{
    const struct theme_ui *ui = mod_ui();
    const struct xpad_touch *t = &xpad_g_touch;
    int32_t b[4];
    int w, h, inner_x, inner_y, inner_w, inner_h, left_w, row_h, y2, fill;
    char pct[16];

    if (juce_comp_bounds((uintptr_t)self, b) != 0) return;
    w = b[2];
    h = b[3];

    inner_x = XP_PANE_PAD_X;
    inner_y = XP_PANE_PAD_Y;
    inner_w = w - 2 * XP_PANE_PAD_X;
    inner_h = h - 2 * XP_PANE_PAD_Y;
    left_w  = inner_w - XP_FLAG_W - XP_FLAG_GAP;
    row_h   = (inner_h - XP_FLAG_GAP) / 2;
    y2      = inner_y + row_h + XP_FLAG_GAP;

    /* Loop and pitch. A dash for the loop when nothing is touched: the pad is
     * dormant, which differs from any loop value. */
    if (!xpad_gesture_live()) {
        /* The dash is drawn, not a glyph. juce::String(const char*) is
         * CharPointer_ASCII on this build, so a U+2014 literal renders as three
         * Latin-1 characters. juce_string_utf8 is for marks that must be
         * glyphs. */
        mod_gfx_colour(g, ui->text_off);
        mod_gfx_fill(g, inner_x + 4, inner_y + row_h - XP_DASH_DROP,
                     XP_DASH_W, XP_DASH_H);
        mod_gfx_text(g, "0%", XP_FONT_VALUE, ui->text_off,
                     inner_x, inner_y, left_w, row_h, JUCE_JUSTIFY_BOT_R);
    } else {
        int p = xpad_pitch_pct(t->semis);

        /* Both values stay lit: the axis split is per step, not a lock, so
         * both are always live. */
        mod_gfx_text(g, xpad_div_name[t->div], XP_FONT_VALUE, ui->xpad,
                     inner_x, inner_y, left_w, row_h, JUCE_JUSTIFY_BOT_L);
        /* White at zero, the pad's colour once bent; the same distinction the
         * cursor's thickness makes on the pad. */
        snprintf(pct, sizeof(pct), "%+d%%", p);
        mod_gfx_text(g, p ? pct : "0%", XP_FONT_VALUE, p ? ui->xpad : ui->text,
                     inner_x, inner_y, left_w, row_h, JUCE_JUSTIFY_BOT_R);
    }

    /* VOL, from the VINYL SPEED ADJUST knob. A rail rather than a number, since
     * the level is set by feel. */
    mod_gfx_text(g, "VOL", XP_FONT_FLAG, ui->text_dim,
                 inner_x, y2, 48, row_h, JUCE_JUSTIFY_CENTRED);
    {
        int rx = inner_x + 56, rw = left_w - 56;
        int ry = y2 + (row_h - XP_VOL_H) / 2;

        fill = rw * (xpad_g_vol < 0 ? 0 : xpad_g_vol > 100 ? 100 : xpad_g_vol) / 100;
        mod_gfx_colour(g, ui->track);
        mod_gfx_fill(g, rx, ry, rw, XP_VOL_H);
        mod_gfx_colour(g, xpad_g_open ? ui->text : ui->text_off);
        mod_gfx_fill(g, rx, ry, fill, XP_VOL_H);
    }

    /* OVERDUB on top: it changes what the next press does, so it gets the
     * first line. */
    xpad_flag(g, "OVERDUB", xpad_g_overdub,
              inner_x + left_w + XP_FLAG_GAP, inner_y, XP_FLAG_W, row_h);
    xpad_flag(g, "HOLD",    xpad_g_hold,
              inner_x + left_w + XP_FLAG_GAP, y2, XP_FLAG_W, row_h);
}

uintptr_t xpad_build_pane(uintptr_t parent, int x, int y, int w, int h)
{
    static uintptr_t vt[VT_CLONE_WORDS], vptr;

    if (!vptr) {
        static const struct juce_vt_override ov[] = {
            { JUCE_VT_PAINT, (void *)xpad_pane_paint, 0 },
        };

        vptr = juce_label_vt_clone(vt, ov, (int)(sizeof(ov) / sizeof(ov[0])));
        if (!vptr) return 0;
    }
    return juce_label(parent, "", XP_FONT_FLAG, 0x00000000u, mod_ui()->text,
                      vptr, x, y, w, h);
}
