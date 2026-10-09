// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * xpad/ui.c - the quick-menu button, the band it opens, and the clocks.
 *
 * The shared contract is in xpad.h.
 *
 * The button and the strip are juce::Labels with a cloned vtable, as in the stem
 * row (see stem/ui/ui.h): a Label already paints a background, an outline and
 * centred text, and cloning its vtable lets single slots be replaced without
 * touching any stock Label.
 */
#include "xpad/xpad.h"
#include "kit/mod.h"
#include "kit/menu.h"

/* ---- what the parts share ------------------------------------------------ */

const char *const xpad_div_name[XP_BRICKS]  = { "1/16", "1/8", "1/4", "1/2", "1", "2" };
const float       xpad_div_beats[XP_BRICKS] = { 0.0625f, 0.125f, 0.25f, 0.5f, 1.0f, 2.0f };

struct xpad_touch xpad_g_touch = { .div = XP_DIV_NONE };
int       xpad_g_hold;
int       xpad_g_overdub;
int       xpad_g_vol = 80;

uintptr_t xpad_g_btn, xpad_g_strip, xpad_g_pad, xpad_g_pane;
int       xpad_g_open;
/* ENABLE X-PAD. Off by default, so a deck that never opts in keeps its band slot
 * and track title and behaves as stock. Like ENABLE STEMS: a runtime toggle,
 * re-applied from sync rather than read once. */
int       xpad_g_on;

static uintptr_t xpad_g_orig_anchor_paint, xpad_g_orig_tick;
static uintptr_t xpad_g_btn_vt[VT_CLONE_WORDS], xpad_g_btn_vptr;
/* A finger is on the band button. See xpad_btn_mousedown. */
static int xpad_g_btn_held;
/* Countdown of a refused press's blink; zero is at rest. See MOD_BLINK_* in
 * juce/draw.h. */
static int xpad_g_refuse;
static int       xpad_g_built, xpad_g_building, xpad_g_api_ok;
static int       xpad_g_dirty;
unsigned  xpad_g_ticks;

/* ---- the band client ----------------------------------------------------- */

/* Unique, past the app's four modes and the stem row's 5. The kit reads it back
 * from the app's mode register to tell "still ours" from "taken", so no two
 * clients may share a number. */
#define XPAD_BAND_MODE 6

/* Closing, by any route, stops the feature: voices silenced, gesture dropped,
 * bar freed. The three borrowed controls revert to MEMORY, CALL/DELETE and the
 * brake on their own, since they check xpad_open() on every gesture.
 *
 * HOLD is cleared too, so a latch cannot carry over to the next open. */
static void xpad_stop(void)
{
    xpad_silence();
    xpad_overdub_set(0);
    xpad_g_hold        = 0;
    xpad_g_touch.div   = XP_DIV_NONE;
    xpad_g_touch.semis = 0.0f;
    xpad_g_touch.held  = 0;
    xpad_g_touch.snapped = 0;
}

static void xpad_band_closed(void)
{
    if (!xpad_g_open) return;
    MDBG("xpad: the band went elsewhere -> closing\n");
    xpad_g_open = 0;
    xpad_stop();
    xpad_sync();
}

static void xpad_reslot(int32_t x)
{
    if (xpad_g_btn)
        ((void (*)(void *, int, int, int, int))FN_SET_BOUNDS)
            ((void *)xpad_g_btn, x, KIT_BAND_SLOT_Y,
             KIT_BAND_SLOT_W, KIT_BAND_SLOT_H);
}

KIT_BAND(k_band_xpad,
         .name = "xpad", .mode = XPAD_BAND_MODE, .closed = xpad_band_closed,
         .shown = &xpad_g_on, .reslot = xpad_reslot, .order = 1);

int xpad_open(void) { return xpad_g_open; }

/* ---- the button ---------------------------------------------------------- */

/* The deck's title-bar style, as on STEMS and the stock buttons beside it: a
 * stippled surface and a short bar flush with the bottom edge.
 *
 * Both are painted before Label::paint, which draws background and lettering
 * in one call; a stipple drawn afterwards would cover the word. So the Label's
 * own background stays transparent and the plate is painted here.
 *
 * The plate carries the state, not the lettering: open turns the surface to
 * the accent colour, as on STEMS. */
/* The plate reads mod_ui() on every paint, so it follows the theme. A
 * juce::Label's text colour is stored on the component at build time, so it
 * must be re-applied when the theme changes; otherwise the word keeps the old
 * theme's colour (e.g. SANDSTONE's near-black navy on ORIGINAL's dark plate,
 * invisible). The stems button re-applies its own in its refresh. */
static void xpad_ink_sync(const struct theme_ui *ui)
{
    static unsigned seen;
    unsigned gen = mod_ui_gen();

    if (gen == seen)
        return;
    seen = gen;
    if (xpad_g_btn)
        juce_comp_colour(xpad_g_btn, LBL_COL_TEXT, ui->text_deck);
    if (xpad_g_strip)
        juce_comp_colour(xpad_g_strip, LBL_COL_TEXT, ui->text);
}

/* The bright half of a refusal blink. Derived from the countdown rather than
 * stored, so the paint always sees the current phase. */
static int xpad_refuse_hot(void)
{
    return xpad_g_refuse && !((xpad_g_refuse / MOD_BLINK_PERIOD) & 1);
}

/* One tick of the countdown. Repaints only when the half changes; nothing else
 * invalidates the button while the panel is shut. */
static void xpad_refuse_step(void)
{
    if (!xpad_g_refuse)
        return;
    if (--xpad_g_refuse % MOD_BLINK_PERIOD == 0)
        juce_comp_repaint(xpad_g_btn);
}

static void xpad_btn_paint(void *self, void *g)
{
    int32_t b[4];

    if (juce_comp_bounds((uintptr_t)self, b) == 0) {
        const struct theme_ui *ui = mod_ui();

        uint32_t lift = xpad_g_btn_held ? MOD_CHECKER_HOT_Q8 : 0;

        xpad_ink_sync(ui);
        mod_checker_plate(g, 0, 0, b[2], b[3],
                          xpad_refuse_hot() ? ui->refuse
                          : xpad_g_open     ? ui->accent
                                            : ui->surface, lift);
        /* The bar lifts with the plate; it is one colour, so lifted directly,
         * as on the stems button. */
        mod_btn_bar(g, 0, 0, b[2], b[3],
                    mod_colour_lift(xpad_g_open ? ui->bar_on : ui->bar, lift));
    }
    /* Bracketed: the Label's text colour is already themed, so the generic pass
     * must not transform it again. */
    mod_draw_enter();
    ((void (*)(void *, void *))LABEL_FN_PAINT)(self, g);
    mod_draw_leave();
}

void xpad_sync(void)
{
    static int last = -1;
    int state = (xpad_g_on ? 1 : 0) | (xpad_g_open ? 2 : 0);

    if (state == last) return;
    last = state;
    /* Disabling closes the panel fully: hiding only the strip would leave the
     * waveform compacted and the band held by an invisible client. */
    if (!xpad_g_on && xpad_g_open) {
        xpad_g_open = 0;
        xpad_stop();
        kit_band_give(&k_band_xpad);
    }
    juce_comp_set_visible(xpad_g_btn, xpad_g_on);
    juce_comp_set_visible(xpad_g_strip, xpad_g_on && xpad_g_open);
    juce_comp_repaint(xpad_g_btn);
    kit_band_slots_changed();
}

void xpad_toggle(void)
{
    if (!xpad_g_open) {
        /* No samples: the plate blinks and the panel stays shut. The first
         * flash shows immediately, because mousedown repaints after this
         * returns.
         *
         * Only opening is gated; an open panel must always be able to close,
         * whatever happened to the stick.
         *
         * deck.c's pad claim relies on this: an open panel always has at least
         * one live pad. */
        if (xpad_bank_count() == 0) {
            xpad_g_refuse = MOD_BLINK_TICKS;
            MDBG("xpad: no loops in %s -> refusing to open, blinking\n", XP_LOOP_DIR);
            return;
        }
        /* Take the band first and open only if that succeeds. The app refuses
         * the mode with no track loaded, and a strip drawn into a rect it never
         * laid out lands across the middle of the screen. */
        if (kit_band_take(&k_band_xpad) < 0) {
            MDBG("xpad: no band to open into (no track loaded?)\n");
            return;
        }
        xpad_g_open = 1;
    } else {
        xpad_g_open = 0;
        xpad_stop();
        kit_band_give(&k_band_xpad);
    }
    xpad_sync();
    MDBG("xpad: panel %s (%d bank%s)\n", xpad_g_open ? "open" : "closed",
         xpad_bank_count(), xpad_bank_count() == 1 ? "" : "s");
}

/* A held finger lifts the plate, like the other buttons in this band (BEAT LOOP
 * goes #323232 -> #616161 when held). The toggle happens on press, so the lift
 * applies to whichever plate state results. MOD_CHECKER_HOT_Q8 is the stock
 * buttons' own held fraction. */
static void xpad_btn_mousedown(void *self, void *event)
{
    (void)event;
    if ((uintptr_t)self != xpad_g_btn) return;
    xpad_g_btn_held = 1;
    xpad_toggle();
    /* Repaint regardless of the toggle: a refused press changes no panel
     * state, so xpad_sync would not repaint. */
    juce_comp_repaint(xpad_g_btn);
}

static void xpad_btn_mouseup(void *self, void *event)
{
    (void)event;
    if ((uintptr_t)self != xpad_g_btn) return;
    xpad_g_btn_held = 0;
    juce_comp_repaint(xpad_g_btn);
}

/* ---- build --------------------------------------------------------------- */

static void xpad_build(uintptr_t anchor)
{
    const int32_t *panel;
    int y, h;

    if (xpad_g_built || xpad_g_building || !xpad_g_api_ok) return;
    if (kit_band_attach(anchor) != 0) return;
    if (!xpad_g_btn_vptr) {
        static const struct juce_vt_override ov[] = {
            { JUCE_VT_MOUSEDOWN, (void *)xpad_btn_mousedown, 0 },
            { JUCE_VT_MOUSEUP,   (void *)xpad_btn_mouseup,   0 },
            { JUCE_VT_PAINT,     (void *)xpad_btn_paint,     0 },
        };

        xpad_g_btn_vptr = juce_label_vt_clone(xpad_g_btn_vt, ov,
                                              (int)(sizeof(ov) / sizeof(ov[0])));
        if (!xpad_g_btn_vptr) return;
    }
    xpad_g_building = 1;
    panel = kit_band_rect();
    /* The same three pixels the stem row takes. The loop indicator draws above
     * the panel rect, so only 3px are free, not the 10 PANEL_GAP suggests. */
    y = panel[1] - 3;
    h = panel[3] + 3;

    if (!xpad_g_btn) {
        xpad_g_btn = juce_label(kit_band_bar(), "X-PAD", XP_FONT_BTN, 0x00000000u,
                                mod_ui()->text_deck, xpad_g_btn_vptr,
                                kit_band_slot_x(&k_band_xpad), KIT_BAND_SLOT_Y,
                                KIT_BAND_SLOT_W, KIT_BAND_SLOT_H);
        /* No outline (Label's one-pixel drawRect), matching the stock
         * buttons. */
        juce_comp_colour(xpad_g_btn, LBL_COL_OUTLINE, 0x00000000u);
        kit_band_own(xpad_g_btn);
    }
    if (!xpad_g_strip) {
        xpad_g_strip = juce_label(kit_band_view(), "", XP_FONT_FLAG, 0x00000000u,
                                  mod_ui()->text, 0, panel[0], y, panel[2], h);
        kit_band_own(xpad_g_strip);
        if (xpad_g_strip) {
            xpad_g_pad  = xpad_build_pad(xpad_g_strip, XP_PAD_X, 0, XP_PAD_W, h);
            xpad_g_pane = xpad_build_pane(xpad_g_strip, XP_PANE_X, 0,
                                          panel[2] - XP_PANE_X, h);
        }
    }
    xpad_g_building = 0;

    xpad_sync();
    if (!xpad_g_btn || !xpad_g_strip || !xpad_g_pad || !xpad_g_pane) {
        MDBG("xpad: build incomplete (btn=%#lx strip=%#lx pad=%#lx pane=%#lx)\n",
             (unsigned long)xpad_g_btn, (unsigned long)xpad_g_strip,
             (unsigned long)xpad_g_pad, (unsigned long)xpad_g_pane);
        return;
    }
    juce_comp_set_visible(xpad_g_strip, 0);
    xpad_g_built = 1;
    MDBG("xpad: built -- strip {%d,%d,%d,%d}, unity y=%d travel %d\n",
         panel[0], y, panel[2], h, xpad_unity_y(h), xpad_travel(h));
}

/* ---- the clocks ---------------------------------------------------------- */

/* The title bar paints whenever the loaded track or its labels change, so its
 * paint triggers the build with no user action, after the parent chain is
 * wired. Chained: the stem row hooks the same slot. */
static void xpad_anchor_paint(void *self, void *g)
{
    if (xpad_g_orig_anchor_paint)
        ((void (*)(void *, void *))xpad_g_orig_anchor_paint)(self, g);
    xpad_build((uintptr_t)self);
}

void xpad_repaint(void)
{
    __atomic_store_n(&xpad_g_dirty, 1, __ATOMIC_RELEASE);
}

static void xpad_tick(void *self)
{
    if (xpad_g_orig_tick)
        ((void (*)(void *))xpad_g_orig_tick)(self);
    xpad_g_ticks++;
    if (!xpad_g_built) return;
    /* Before the open check: a refusal happens with the panel shut. */
    xpad_refuse_step();
    if (!xpad_g_open) return;

    /* HOLD going off drops the latched gesture. Done here, not in the MEMORY
     * handler, because this thread owns xpad_g_touch (MEMORY runs on the deck's
     * task thread), and this catches every route that clears HOLD.
     *
     * The values must be cleared, not just made inert: otherwise pressing
     * MEMORY again with no finger down would restore the old brick. */
    if (!xpad_g_hold && !xpad_g_touch.held &&
        xpad_g_touch.div != XP_DIV_NONE) {
        xpad_g_touch.div     = XP_DIV_NONE;
        xpad_g_touch.semis   = 0.0f;
        xpad_g_touch.snapped = 0;
        juce_comp_repaint(xpad_g_pad);
        juce_comp_repaint(xpad_g_pane);
    }
    /* The active brick's name blinks, and nothing else invalidates the strip
     * while a finger rests on it. */
    if (xpad_gesture_live())
        juce_comp_repaint(xpad_g_pad);
    /* The readout, when a borrowed control changed it. Repainted here because
     * those controls run on the deck's task thread. */
    if (__atomic_exchange_n(&xpad_g_dirty, 0, __ATOMIC_ACQUIRE)) {
        juce_comp_repaint(xpad_g_pane);
        /* The pad too: MEMORY releasing the latch changes the strip. */
        juce_comp_repaint(xpad_g_pad);
    }

    /* The mix's stats for the last second, logged here because the mix may not
     * log. Silent when nothing happened. */
    if ((xpad_g_ticks % XPAD_STAT_TICKS) == 0) {
        struct xpad_mix_stat st;

        xpad_mix_stat(&st);
        if (st.blocks || st.fired || xpad_seq_count())
            MDBG("xpad: mix %u blocks, deck fired %u, mix fired %u, peak %.3f,"
                 " vol %d%%, bar %d, clock %s beat %.3f span %.5f\n",
                 st.blocks, st.fired, st.fires, (double)st.peak, xpad_g_vol,
                 xpad_seq_count(),
                 st.clock == 2 ? "grid" : st.clock == 1 ? "tempo" : "NONE",
                 st.beat, st.span);
    }
}

/* ---- install ------------------------------------------------------------- */

static void xpad_gate_changed(void)
{
    /* Nothing to tear down here: sync owns the close, and the tick calls it. */
    MDBG("xpad: ENABLE X-PAD %s\n", xpad_g_on ? "on" : "off");
    xpad_sync();
}

static const struct kit_row k_rows[] = {
    KIT_ROW_BOOL("ENABLE X-PAD", &xpad_g_on,
                 .idx = KIT_IDX_XPAD, .changed = xpad_gate_changed),
};

static int xpad_ui_install(void)
{
    xpad_g_api_ok = FN_ADD_VISIBLE && FN_SET_BOUNDS && FN_LABEL_CTOR &&
                    FN_LABEL_SETFONT && FN_LABEL_JUSTIFY &&
                    MOD_FN_GFX_SETCOLOUR && MOD_FN_GFX_FILLRECT &&
                    MOD_FN_GFX_SETFONT && MOD_FN_GFX_DRAWTEXT;
    if (!xpad_g_api_ok) {
        MDBG("xpad: juce primitives did not resolve -> feature off\n");
        return -1;
    }
    if (mod_patch_vslot("xpadAnchor", EP122_TOUCHARIA, JUCE_VT_PAINT,
                        (void *)xpad_anchor_paint, &xpad_g_orig_anchor_paint) != 0) {
        MDBG("xpad: no anchor -> feature off\n");
        xpad_g_api_ok = 0;
        return -1;
    }
    if (mod_patch_vslot("xpadTick", EP122_DISPLAY_REFRESH, 0x10,
                        (void *)xpad_tick, &xpad_g_orig_tick) != 0)
        MDBG("xpad: no display tick -> the lit brick will not blink\n");
    kit_menu_add(k_rows, (int)(sizeof(k_rows) / sizeof(k_rows[0])));
    return 0;
}

KIT_MOD(k_mod_xpad_ui,
        .name = "xpad_ui", .prio = 35, .install = xpad_ui_install,
        .what = "X-PAD SAMPLER quick-menu button + touch strip");
