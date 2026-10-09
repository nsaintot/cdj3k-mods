// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * xpad/strip.c - the pad: six bricks, and the bend inside the one under the
 * finger.
 *
 * X picks the loop length and Y bends the pitch, both from one touch, so a
 * sideways move must not change the pitch and a bend must not change the loop
 * length. Two mechanisms handle this, neither of them a mode:
 *
 *   The angle gate routes every step by its own direction, so sideways travel
 *   adds nothing to the bend and vertical travel does not move the brick. See
 *   xpad.h for why the bend must be a running total.
 *
 *   The snap holds pitch at exactly zero within half a semitone, so unity is
 *   easy to return to. It costs no vertical range, which is scarce: the band
 *   the app frees is 84 pixels and the whole range fits in half of that.
 *
 * The only indication of the snap is the cursor thickening from 3px to 6px,
 * which needs no extra room and is visible under a finger.
 *
 * The name sits in a notch so the fill and the lettering never overlap. The
 * fill is cut around the lettering (66px of a 153px brick), leaving 43px of
 * solid fill down each gutter at any value.
 */
#include "xpad/xpad.h"

/* ---- geometry ------------------------------------------------------------
 *
 * Derived from the pad's height, since the band is whatever the app frees and
 * the design was drawn for a taller strip. Unity is centred and the travel is
 * symmetric, because the axis is linear in semitones and both ends are the same
 * interval. */
int xpad_unity_y(int h) { return (h - XP_CURSOR_H) / 2; }
int xpad_travel(int h)  { return xpad_unity_y(h) - XP_EDGE_LIT; }

/* Brick edges, computed so the six tile the pad exactly: 916 is not divisible
 * by six. */
static int brick_x0(int i) { return i * XP_PAD_W / XP_BRICKS; }
static int brick_x1(int i) { return (i + 1) * XP_PAD_W / XP_BRICKS; }

static int brick_at(int x)
{
    int i;

    if (x < 0 || x >= XP_PAD_W) return XP_DIV_NONE;
    for (i = 0; i < XP_BRICKS; i++)
        if (x < brick_x1(i)) return i;
    return XP_BRICKS - 1;
}

int xpad_pitch_pct(float semis)
{
    float p = semis * (float)XP_PCT_FULL / (float)XP_SEMITONES;

    /* Rounded to nearest. Display only; the audio uses the float. */
    return (int)(p + (p < 0 ? -0.5f : 0.5f));
}

/* ---- paint --------------------------------------------------------------- */

/* The fill's two shapes, each clipped to a y range so the caller can pass the
 * brick's three bands directly. Above and below the name the fill spans the
 * whole brick; level with it, only the two gutters (43px each).
 *
 * Both are inset by the lit outline so the fill stays inside the active brick's
 * heavier edge. */
static void xpad_fill_wide(void *g, int x0, int w, int y0, int y1)
{
    if (y1 > y0)
        mod_gfx_fill(g, x0 + XP_EDGE_LIT, y0, w - 2 * XP_EDGE_LIT, y1 - y0);
}

static void xpad_fill_gutters(void *g, int x0, int w, int nx, int y0, int y1)
{
    if (y1 <= y0) return;
    mod_gfx_fill(g, x0 + XP_EDGE_LIT, y0, nx - x0 - XP_EDGE_LIT, y1 - y0);
    mod_gfx_fill(g, nx + XP_NOTCH_W, y0,
                 x0 + w - XP_EDGE_LIT - (nx + XP_NOTCH_W), y1 - y0);
}

static void xpad_pad_paint(void *self, void *g)
{
    const struct theme_ui *ui = mod_ui();
    const struct xpad_touch *t = &xpad_g_touch;
    int32_t b[4];
    int h, unity, travel, i, lit_on, live;

    if (juce_comp_bounds((uintptr_t)self, b) != 0) return;
    /* Draw what is acting, not what was last touched: switching HOLD off must
     * leave the strip dormant, not lit over silence. */
    live   = xpad_gesture_live();
    h      = b[3];
    unity  = xpad_unity_y(h);
    travel = xpad_travel(h);

    /* The pad's own outline, and the five dividers between the six bricks. */
    mod_gfx_colour(g, ui->edge);
    mod_gfx_fill(g, 0, 0, XP_PAD_W, XP_EDGE);
    mod_gfx_fill(g, 0, h - XP_EDGE, XP_PAD_W, XP_EDGE);
    mod_gfx_fill(g, 0, 0, XP_EDGE, h);
    mod_gfx_fill(g, XP_PAD_W - XP_EDGE, 0, XP_EDGE, h);
    for (i = 1; i < XP_BRICKS; i++)
        mod_gfx_fill(g, brick_x0(i) - XP_EDGE / 2, 0, XP_EDGE, h);

    /* The six names, all dimmed while one is live so the lit one stands out. */
    for (i = 0; i < XP_BRICKS; i++) {
        int x0 = brick_x0(i), w = brick_x1(i) - x0;

        if (live && i == t->div) continue;   /* drawn below, in its notch */
        mod_gfx_text(g, xpad_div_name[i], XP_FONT_BRICK,
                     live ? ui->text_off : ui->text,
                     x0, 0, w, h, JUCE_JUSTIFY_CENTRED);
    }
    if (!live)
        return;

    {
        int x0 = brick_x0(t->div), w = brick_x1(t->div) - x0;
        int nx = x0 + (w - XP_NOTCH_W) / 2;
        int cur_h = t->snapped ? XP_CURSOR_SNAP_H : XP_CURSOR_H;
        int px = (int)(t->semis * (float)travel / (float)XP_SEMITONES + (t->semis < 0 ? -0.5f : 0.5f));

        /* Heavier outline on the active brick, visible even at unity where
         * there is no fill. */
        mod_gfx_colour(g, ui->text_dim);
        mod_gfx_fill(g, x0, 0, w, XP_EDGE_LIT);
        mod_gfx_fill(g, x0, h - XP_EDGE_LIT, w, XP_EDGE_LIT);
        mod_gfx_fill(g, x0, 0, XP_EDGE_LIT, h);
        mod_gfx_fill(g, x0 + w - XP_EDGE_LIT, 0, XP_EDGE_LIT, h);

        /* The bend, growing from unity: up for positive, down for negative.
         * In the pad's red (mod_ui()->xpad), not the accent: on this deck blue
         * means "on, selected".
         *
         * The fill stops at the lettering instead of being masked over it, so
         * no background colour is needed; the background belongs to the app
         * and a theme may change it. */
        if (px) {
            int y0 = px > 0 ? unity - px : unity + XP_CURSOR_H;
            int y1 = px > 0 ? unity      : unity + XP_CURSOR_H - px;
            int ny0 = (h - XP_NOTCH_H) / 2, ny1 = ny0 + XP_NOTCH_H;

            mod_gfx_colour(g, ui->xpad);
            xpad_fill_wide(g, x0, w, y0, y1 < ny0 ? y1 : ny0);
            xpad_fill_gutters(g, x0, w, nx, y0 > ny0 ? y0 : ny0,
                                            y1 < ny1 ? y1 : ny1);
            xpad_fill_wide(g, x0, w, y0 > ny1 ? y0 : ny1, y1);
        }

        /* The name, in its notch. It blinks while the brick is live, with the
         * lit phase longer than the dim one. */
        lit_on = (int)(xpad_g_ticks % XP_BLINK_PERIOD) < XP_BLINK_ON;
        mod_gfx_text(g, xpad_div_name[t->div], XP_FONT_LIT,
                     lit_on ? ui->xpad : mod_colour_scale(ui->xpad, XP_BLINK_DIM_Q8),
                     nx, 0, XP_NOTCH_W, h, JUCE_JUSTIFY_CENTRED);

        /* The unity hairline, in the two gutters only, never across the
         * name. */
        mod_gfx_colour(g, ui->text);
        mod_gfx_fill(g, x0 + XP_EDGE_LIT, unity, nx - x0 - XP_EDGE_LIT, cur_h);
        mod_gfx_fill(g, nx + XP_NOTCH_W, unity,
                     x0 + w - XP_EDGE_LIT - (nx + XP_NOTCH_W), cur_h);
    }
}

/* ---- the gesture --------------------------------------------------------- */

/* juce::MouseEvent carries the position as two floats at +0x00, as read by the
 * MOD SETTINGS overlay (see menu/). */
#define ME_X 0x00
#define ME_Y 0x04

/* Where the finger was on the previous event. Each step is measured from here,
 * not from the press, so sideways travel adds nothing to the bend. */
static float xpad_g_last_x, xpad_g_last_y;

static int xpad_event_pos(void *event, float pos[2])
{
    return mod_safe_read((uintptr_t)event + ME_X, pos, 2 * sizeof(float));
}

static void xpad_settle(void)
{
    if (xpad_g_touch.semis >  (float)XP_SEMITONES) xpad_g_touch.semis =  (float)XP_SEMITONES;
    if (xpad_g_touch.semis < -(float)XP_SEMITONES) xpad_g_touch.semis = -(float)XP_SEMITONES;
    xpad_g_touch.snapped = xpad_g_touch.semis >  -XP_SNAP_ST &&
                           xpad_g_touch.semis <   XP_SNAP_ST;
    if (xpad_g_touch.snapped) xpad_g_touch.semis = 0.0f;
}

static void xpad_pad_down(void *self, void *event)
{
    float pos[2];
    int   latched;

    if (xpad_event_pos(event, pos) != 0) return;
    xpad_g_last_x = pos[0];
    xpad_g_last_y = pos[1];

    /* A press picks the brick under it and starts the bend at zero, wherever it
     * lands.
     *
     * Unless HOLD is latching a bend: then the press continues from it, since
     * zeroing would audibly drop the held pitch. Read before the brick is
     * picked, because `div` indicates the latch and is about to be
     * overwritten. */
    latched = xpad_g_hold && xpad_g_touch.div != XP_DIV_NONE;

    xpad_g_touch.div = brick_at((int)pos[0]);
    if (!latched) {
        xpad_g_touch.semis   = 0.0f;
        xpad_g_touch.snapped = 1;
    }
    xpad_g_touch.held = 1;

    juce_comp_repaint((uintptr_t)self);
    juce_comp_repaint(xpad_g_pane);
}

static void xpad_pad_drag(void *self, void *event)
{
    int32_t b[4];
    float pos[2], dx, dy, adx, ady;
    int travel;

    if (juce_comp_bounds((uintptr_t)self, b) != 0) return;
    if (xpad_event_pos(event, pos) != 0) return;
    travel = xpad_travel(b[3]);
    if (travel <= 0) return;

    dx  = pos[0] - xpad_g_last_x;
    dy  = pos[1] - xpad_g_last_y;
    adx = dx < 0 ? -dx : dx;
    ady = dy < 0 ? -dy : dy;

    /* A step this large is a second contact or a lift reported at the origin,
     * which would jump the readout from a still touch. Re-anchor and change
     * nothing. */
    if (adx > (float)XP_JUMP_PX || ady > (float)XP_JUMP_PX) {
        xpad_g_last_x = pos[0];
        xpad_g_last_y = pos[1];
        return;
    }

    /* Route this step by its own direction. Nothing is latched, so one touch
     * can bend, cross to another brick keeping the bend, and bend again. */
    if (adx > ady) {
        if (adx >= (float)XP_STEP_PX) {
            int d = brick_at((int)pos[0]);

            if (d != XP_DIV_NONE) xpad_g_touch.div = d;
            xpad_g_last_x = pos[0];
            xpad_g_last_y = pos[1];
        }
        /* Below the threshold the anchor is not moved, so a slow crawl
         * accumulates until it crosses. */
    } else if (ady > 0.0f) {
        /* Up is positive, matching the fill and the readout. */
        xpad_g_touch.semis += -dy * (float)XP_SEMITONES / (float)travel;
        xpad_settle();
        xpad_g_last_x = pos[0];
        xpad_g_last_y = pos[1];
    }

    juce_comp_repaint((uintptr_t)self);
    juce_comp_repaint(xpad_g_pane);
}

/* Lifting ends the gesture unless HOLD is lit, which latches the last loop and
 * bend; the strip keeps showing them while they sound. */
static void xpad_pad_up(void *self, void *event)
{
    (void)event;
    xpad_g_touch.held = 0;
    if (!xpad_g_hold) {
        xpad_g_touch.div     = XP_DIV_NONE;
        xpad_g_touch.semis   = 0.0f;
        xpad_g_touch.snapped = 0;
    }
    juce_comp_repaint((uintptr_t)self);
    juce_comp_repaint(xpad_g_pane);
}

uintptr_t xpad_build_pad(uintptr_t parent, int x, int y, int w, int h)
{
    static uintptr_t vt[VT_CLONE_WORDS], vptr;

    if (!vptr) {
        static const struct juce_vt_override ov[] = {
            { JUCE_VT_PAINT,     (void *)xpad_pad_paint, 0 },
            { JUCE_VT_MOUSEDOWN, (void *)xpad_pad_down,  0 },
            { JUCE_VT_MOUSEDRAG, (void *)xpad_pad_drag,  0 },
            { JUCE_VT_MOUSEUP,   (void *)xpad_pad_up,    0 },
        };

        vptr = juce_label_vt_clone(vt, ov, (int)(sizeof(ov) / sizeof(ov[0])));
        if (!vptr) return 0;
    }
    return juce_label(parent, "", XP_FONT_BRICK, 0x00000000u, mod_ui()->text,
                      vptr, x, y, w, h);
}
