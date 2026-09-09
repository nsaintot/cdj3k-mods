// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * xpad/pane.c - the readout beside the pad.
 *
 * FLOATING: a gutter and no outline. What says it is not a thing to press is
 * that it has no edge -- an outlined box beside a pad made of outlined boxes
 * reads as a seventh brick. Everything here is driven from the strip, from the
 * two borrowed buttons and from the borrowed knob, and none of it answers to a
 * finger.
 *
 * It carries both values, loop and pitch, because the finger is on the brick
 * that shows them and a hand covers the brick. Unlabelled -- a fraction and a
 * signed percentage explain themselves, and at this size a caption would cost
 * more than it says. The percentage is of the RANGE, not of the play rate; see
 * XP_PCT_FULL.
 */
#include "xpad/xpad.h"

/* A LAMP AND ITS LEGEND, which is how the RMX-1000 puts these two on a panel: an
 * illuminated button with its name printed beside it. Two plates that inverted
 * into each other read as two different design languages sitting together, which
 * is what these were before.
 *
 * The lamp carries the state and the word never changes shape -- only its
 * brightness, from the deck's dim lettering to its full white, which is the same
 * pair GATE CUE uses one rack below. The lamp's own frame is one pixel of `edge`,
 * no fill, until it lights.
 *
 * BOTH LIGHT THE SAME WAY. OVERDUB blinked here for a while, after the RMX's own
 * recording signal, and it was wrong on a screen: a lamp caught on the dim half
 * of its cycle beside a steady one does not read as "recording", it reads as a
 * control halfway through changing state. Two lamps side by side are compared
 * with each other, not watched over time. */
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

    /* Loop and pitch. Dashes rather than a zero when nothing is touched: the pad
     * is dormant, not set to unity, and those are different states. */
    if (!xpad_gesture_live()) {
        /* The dash is DRAWN, not spelled. juce::String(const char*) is
         * CharPointer_ASCII on this build, so a U+2014 literal arrives as the
         * three Latin-1 characters its UTF-8 encoding is made of -- which is
         * exactly what it rendered as. juce_string_utf8 exists for the cases
         * where the mark has to be a glyph; a horizontal bar does not. */
        mod_gfx_colour(g, ui->text_off);
        mod_gfx_fill(g, inner_x + 4, inner_y + row_h - XP_DASH_DROP,
                     XP_DASH_W, XP_DASH_H);
        mod_gfx_text(g, "0%", XP_FONT_VALUE, ui->text_off,
                     inner_x, inner_y, left_w, row_h, JUCE_JUSTIFY_BOT_R);
    } else {
        int p = xpad_pitch_pct(t->semis);

        /* BOTH VALUES STAY LIT. The gate is per step rather than a lock, so
         * neither axis is ever out of reach and dimming one would be describing
         * a mode that does not exist. */
        mod_gfx_text(g, xpad_div_name[t->div], XP_FONT_VALUE, ui->xpad,
                     inner_x, inner_y, left_w, row_h, JUCE_JUSTIFY_BOT_L);
        /* White at zero, the pad's colour once the bend is live -- the same distinction the
         * cursor's thickness makes on the pad, said a second way for the eye that
         * is on the numbers rather than on the shape. */
        snprintf(pct, sizeof(pct), "%+d%%", p);
        mod_gfx_text(g, p ? pct : "0%", XP_FONT_VALUE, p ? ui->xpad : ui->text,
                     inner_x, inner_y, left_w, row_h, JUCE_JUSTIFY_BOT_R);
    }

    /* VOL, off the VINYL SPEED ADJUST knob. A rail rather than a number: it is
     * set by feel on a knob the DJ is already touching, so what it has to show is
     * roughly where it sits, not what it reads. */
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

    /* OVERDUB ON TOP. It is the one of the two that changes what the next press
     * DOES rather than what the last gesture left behind, so it takes the line
     * the eye reaches first. */
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
