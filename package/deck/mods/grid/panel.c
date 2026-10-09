// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * mods/grid/panel.c - the BPM half of the grid-edit panel.
 *
 * The CDJ-3000X's grid panel has two groups. The 3000 has only the first:
 *
 *   position   SNAP GRID(CUE) | SHIFT GRID | 1/2 < | 1/2 > | RESET
 *   BPM        |||x1/2 | |||x2 | <|||> | >|||< | RESET      <- missing
 *
 * with the BPM readout above the second group. Those four are all one operation
 * on the beat interval, which stem_grid_edit_scale does and no firmware mode can
 * (see stem/grid_edit.c). This file is the panel side: it shifts the deck's five
 * buttons left and builds the second group beside them, on a plate of its own so
 * the five read as one group acting on one number. The deck's five get no plate.
 *
 * ---- finding the panel -----------------------------------------------------
 *
 * gui::detailed_waveform::GridAdjust is model-first, like the play-screen rack:
 * its primary vtable is a ten-slot updater interface whose +0xd0 is a `return 0`
 * stub, and its juce::Component vtable comes later. Component is a virtual base,
 * so ep122_syms.spec cannot name that vtable with `base=`: the generator refuses
 * with "0 vtables at offset-to-top 24", because for a virtual base the RTTI
 * records an index into the vtable, not an offset.
 *
 * So the panel is found by typeinfo through the component tree, which every
 * vtable in a class's group agrees on. See juce.h and the play-screen rack
 * (ep122_syms.spec).
 *
 * The Component vtable is read off the live object instead of the spec: the
 * child pointer in the tree is the Component subobject, so its first word is the
 * vtable, and the typeinfo behind it confirms the class. The only loss is that
 * it cannot be checked offline.
 *
 * ---- geometry is read from the live tree -----------------------------------
 *
 * The existing buttons' bounds come from the live tree, so the new group is
 * placed against what is actually there. The stock layout, in panel coordinates:
 *
 *   GridAdjust {0,293,1280,84}   -- the bottom strip of the waveform view
 *     SnapGridButton   {315,0,116,84}
 *     ShiftGridButton  {451,0,116,84}
 *     1/2 <            {587,0,116,84}
 *     1/2 >            {713,0,116,84}
 *     ResetButton      {849,0,116,84}
 *
 * A stock button is 116x84 and the plate the design gives is 114x82, so the
 * component carries a 1px margin the artwork does not use. Ours are built the
 * same size and inset the same 1px, so the two groups line up.
 */
#include "grid/panel_internal.h"
#include "core/ep122_syms.h"
#include "juce/draw.h"
#include "juce/juce.h"
#include "kit/mod.h"
#include "stem/stem.h"

/* Glyph coverage: the deck carries three faces in /usr/share/fonts/ttf
 * and they differ, so the labels use only what all of them draw, plus the two
 * arrows the two larger faces have:
 *
 *   U+00D7 x     the multiplication sign   -- in all three
 *   U+007C |     the grid bars             -- in all three
 *   U+25C0 / U+25B6  black triangles       -- UCGothic_J and TsukuGoCustomPro,
 *                                             not in UCGothicLatin (551 glyphs)
 *   U+25C4 / U+25BA  the "pointer" pair    -- in none of them
 *
 * Read from the cmaps with fontTools. A missing glyph renders as a blank box. */
static const char *const gp_text[GP_N] = {
    "|||\xc3\x97" "1/2",                    /* |||x1/2 */
    "|||\xc3\x97" "2",                      /* |||x2   */
    "|||",                                  /* enlarge: arrows drawn, see below */
    "|||",                                  /* reduce                           */
    "RESET"
};

/* For the log only: the two arrow buttons carry the same text. */
static const char *const gp_name[GP_N] = {
    "x1/2", "x2", "ENLARGE", "REDUCE", "RESET"
};

uintptr_t gp_g_panel;                  /* the GridAdjust component      */
static uintptr_t gp_g_parent;                 /* what it hangs off             */
int       gp_g_first;                  /* x of our first button, from the panel's width */
int32_t   gp_g_panel_b[4];             /* the strip as the deck laid it out */
int       gp_g_panel_h;                /* ...and the height we grow it to    */
int       gp_g_btn_y;                  /* where the buttons sit in the grown strip */
uintptr_t gp_g_stock[GP_N];            /* the deck's five, left group   */
uintptr_t gp_g_btn[GP_N];              /* ours, right group             */
static uintptr_t gp_g_band;                   /* the plate over the group  */
uintptr_t gp_g_readout;                /* the value inside it       */
static uintptr_t gp_g_vptr;
static uintptr_t gp_g_vt[VT_CLONE_WORDS];
static uintptr_t gp_g_orig_panel_paint;
static uintptr_t gp_g_orig_reset_paint;

static uintptr_t gp_g_orig_panel_setvis;
uintptr_t gp_g_orig_mouseup;
int       gp_g_hot = -1;               /* which of ours has a finger on it */
double    gp_g_shown_bpm = -1.0;

/* ================================================================== */
/* Small helpers                                                      */
/* ================================================================== */

void gp_repaint(uintptr_t comp)
{
    uintptr_t fn = ep122_sym(EP122_JUCE_COMP_REPAINT);

    if (comp && fn)
        ((void (*)(void *))fn)((void *)comp);
}

/* ================================================================== */
/* The actions                                                        */
/* ================================================================== */

/* The panel owns the edit state. stem_grid_edit_scale always rescales the grid
 * the deck loaded, so a second call replaces the first instead of compounding
 * (x2 twice is x2, and RESET is a restore). So the panel keeps the whole edit, a
 * multiplier and a count of millisecond steps, and applies it as one k against
 * the original each time, matching the 3000X's "moves the beatgrid by 1 msec
 * based on the first grid". */
double    gp_g_mult = 1.0;
int       gp_g_steps;
static uintptr_t gp_g_edit_id;


void gp_action(int which)
{
    uintptr_t id = stem_grid_id();
    double base = stem_grid_orig_bpm();
    double beat, target, k;
    double was_mult = gp_g_mult;
    int    was_steps = gp_g_steps;

    /* Another grid means another track: an edit state carried across would apply
     * this track's millisecond steps to the next one's tempo. */
    if (id != gp_g_edit_id) {
        gp_g_edit_id = id;
        gp_g_mult    = 1.0;
        gp_g_steps   = 0;
        was_mult     = 1.0;
        was_steps    = 0;
    }

    if (which == GP_RESET) {
        gp_g_mult  = 1.0;
        gp_g_steps = 0;
        if (stem_grid_edit_reset() == 0) {
            MDBG("gpanel: RESET -> %.2f BPM\n", stem_grid_bpm());
            /* Saved like any other edit, so a RESET survives the next load. */
            stem_grid_edit_save();
        } else {
            MDBG("gpanel: RESET -> nothing to put back\n");
        }
        return;
    }
    if (!(base > 0.0)) {
        MDBG("gpanel: no grid -> %s does nothing\n", gp_name[which]);
        return;
    }

    switch (which) {
    case GP_DOUBLE:  if (gp_g_mult < GP_MULT_MAX) gp_g_mult *= 2.0; break;
    case GP_HALF:    if (gp_g_mult > GP_MULT_MIN) gp_g_mult *= 0.5; break;
    case GP_ENLARGE: if (gp_g_steps <  GP_STEPS_MAX) gp_g_steps++; break;
    case GP_REDUCE:  if (gp_g_steps > -GP_STEPS_MAX) gp_g_steps--; break;
    default: return;
    }

    /* The interval the edited grid should have: the original's, divided by the
     * multiplier, then moved by the millisecond steps. Enlarge spreads the beats
     * (interval grows, tempo falls); Reduce is the other way. */
    beat   = 60.0 / base;
    target = beat / gp_g_mult + (double)gp_g_steps * GP_MS;
    if (!(target > GP_MS)) {
        gp_g_mult  = was_mult;
        gp_g_steps = was_steps;
        return;
    }
    k = beat / target;
    if (stem_grid_edit_scale(k) == 0) {
        MDBG("gpanel: %s -> x%.4g %+d ms, k=%.6f -> %.2f BPM\n",
             gp_name[which], gp_g_mult, gp_g_steps, k, stem_grid_bpm());
        /* Per press, like the deck's own grid adjust: the register call posts a
         * task and returns, so the press never waits on the media. Each save
         * writes the whole grid, not a delta. */
        stem_grid_edit_save();
    } else {
        /* The grid did not move, so roll back the edit state. */
        gp_g_mult  = was_mult;
        gp_g_steps = was_steps;
        MDBG("gpanel: %s (k=%.6f) refused\n", gp_name[which], k);
    }
}

/* ================================================================== */
/* One button                                                         */
/* ================================================================== */

/* See panel_button.c. */

/* ================================================================== */
/* The readout                                                        */
/* ================================================================== */

/* ================================================================== */
/* Layout                                                             */
/* ================================================================== */

/* The readout and the layout: see panel_layout.c. */

/* ================================================================== */
/* The panel                                                          */
/* ================================================================== */

/* Chained after the stock paint and before the children (a juce parent paints,
 * then its children over it), so the plate is drawn here as a backdrop without
 * touching the buttons' z-order. */
static void gp_panel_paint(void *self, void *g)
{
    int32_t b[4];

    if (gp_g_orig_panel_paint)
        ((void (*)(void *, void *))gp_g_orig_panel_paint)(self, g);
    if (juce_comp_bounds((uintptr_t)self, b) != 0)
        return;

    /* Our group only; the deck's five keep their stock look. */
    mod_gfx_colour(g, mod_colour_stock(GP_COL_GROUP));
    mod_gfx_fill(g, gp_g_first - GP_PLATE_PAD, 0, GP_PLATE_W, b[3]);

    gp_readout_sync();
}

/* The readout lives outside the panel, so it follows the panel's visibility
 * here. A hook rather than a poll, so both change in the same frame and no BPM
 * box is left over the waveform. */
static void gp_panel_setvisible(void *self, int visible)
{
    if (gp_g_orig_panel_setvis)
        ((void (*)(void *, int))gp_g_orig_panel_setvis)(self, visible);
    if ((uintptr_t)self != gp_g_panel)
        return;
    if (gp_g_band)
        juce_comp_set_visible(gp_g_band, visible);
    if (visible) {
        gp_layout();
        gp_readout_sync();
    }
}

/* ================================================================== */
/* Build                                                              */
/* ================================================================== */

static int gp_hook_panel(uintptr_t panel)
{
    uintptr_t vt = 0, paint = 0, setvis = 0;

    if (mod_safe_read(panel, &vt, sizeof(vt)) != 0 || !vt)
        return -1;
    /* The caller matched the object's typeinfo, so this is the Component vtable
     * of gui::detailed_waveform::GridAdjust. expect_fn is what the slot already
     * holds; the identity check is the typeinfo, not a stored address. */
    if (mod_safe_read(vt + JUCE_VT_PAINT, &paint, sizeof(paint)) != 0 || !paint)
        return -1;
    if (mod_safe_read(vt + JUCE_VT_SETVISIBLE, &setvis, sizeof(setvis)) != 0 || !setvis)
        return -1;
    if (mod_patch_slot("gridPanelPaint", vt + JUCE_VT_PAINT, paint,
                       (void *)gp_panel_paint, &gp_g_orig_panel_paint) != 0)
        return -1;
    if (mod_patch_slot("gridPanelVisible", vt + JUCE_VT_SETVISIBLE, setvis,
                       (void *)gp_panel_setvisible, &gp_g_orig_panel_setvis) != 0)
        return -1;
    return 0;
}

/* The deck's RESET, by the same route as the panel: its Component vtable off the
 * live object, whose typeinfo says which class it is. */
static void gp_hook_reset(uintptr_t reset)
{
    uintptr_t vt = 0, paint = 0;

    if (juce_comp_class(reset) != GP_TI_RESET) {
        MDBG("gpanel: child 4 is not the ResetButton -> its artwork is left alone\n");
        return;
    }
    if (mod_safe_read(reset, &vt, sizeof(vt)) != 0 || !vt ||
        mod_safe_read(vt + JUCE_VT_PAINT, &paint, sizeof(paint)) != 0 || !paint)
        return;
    if (mod_patch_slot("gridResetPaint", vt + JUCE_VT_PAINT, paint,
                       (void *)gp_reset_paint, &gp_g_orig_reset_paint) != 0)
        MDBG("gpanel: could not take the RESET's paint -> two shapes of RESET\n");
}


static void gp_build(uintptr_t panel)
{
    static const struct juce_vt_override ov[] = {
        { JUCE_VT_MOUSEDOWN, (void *)gp_label_mousedown, NULL },
        { JUCE_VT_MOUSEUP,   (void *)gp_label_mouseup,   &gp_g_orig_mouseup },
        { JUCE_VT_PAINT,     (void *)gp_label_paint,     NULL },
    };
    int32_t pb[4];
    int i, n;

    n = juce_comp_nchild(panel);
    if (n != GP_N) {
        MDBG("gpanel: GridAdjust has %d children, not %d -- left alone\n", n, GP_N);
        return;
    }
    for (i = 0; i < GP_N; i++) {
        gp_g_stock[i] = juce_comp_child(panel, i);
        if (!gp_g_stock[i]) {
            MDBG("gpanel: child %d is not readable -- left alone\n", i);
            return;
        }
    }
    if (juce_comp_bounds(panel, pb) != 0)
        return;

    gp_hook_reset(gp_g_stock[GP_RESET]);

    /* Placed against the panel's own width, not a hardcoded 1280. If the two
     * groups would overlap, nothing is built and the deck's five buttons stay
     * where the firmware put them. */
    gp_g_first = pb[2] - GP_MARGIN - GP_OURS_W;
    if (gp_g_first - GP_PLATE_PAD - GP_VGROUP < GP_GROUP_L + GP_LEFT_W) {
        MDBG("gpanel: a %d px strip does not hold both groups -> no BPM group\n",
             (int)pb[2]);
        return;
    }

    /* The strip is grown both ways so the plate has a border above and below
     * deck-height buttons. The buttons then sit GP_PAD down the taller strip,
     * back on the row the firmware used.
     *
     * The height is clamped to the parent's measured height: the waveform view
     * has 8px under the strip and the border wants 9, so the bottom border loses
     * one pixel. With no room at all the strip is unchanged and the plate ends at
     * the buttons. */
    gp_g_panel_b[0] = pb[0];
    gp_g_panel_b[1] = pb[1];
    gp_g_panel_b[2] = pb[2];
    gp_g_panel_h    = pb[3];
    gp_g_btn_y      = 0;
    {
        int32_t par[4];
        int room = juce_comp_bounds(juce_comp_parent(panel), par) == 0;

        if (pb[1] >= GP_PAD && room && par[3] > pb[1] + GP_BTN_H) {
            gp_g_panel_b[1] = pb[1] - GP_PAD;
            gp_g_btn_y      = GP_PAD;
            gp_g_panel_h    = GP_PANEL_H;
            if (gp_g_panel_b[1] + gp_g_panel_h > par[3])
                gp_g_panel_h = par[3] - gp_g_panel_b[1];
        } else {
            MDBG("gpanel: no room around the strip -> plate ends at the buttons\n");
        }
    }

    gp_g_vptr = juce_label_vt_clone(gp_g_vt, ov, (int)(sizeof(ov) / sizeof(ov[0])));
    if (!gp_g_vptr) {
        MDBG("gpanel: no Label vtable clone -> no BPM group\n");
        return;
    }
    for (i = 0; i < GP_N; i++) {
        /* Our RESET uses the deck RESET's font so the two match. */
        gp_g_btn[i] = juce_label(panel, gp_text[i],
                                 (i == GP_RESET) ? GP_RESET_FONT : GP_FONT,
                                 0x00000000u, GP_COL_TEXT, gp_g_vptr,
                                 gp_btn_x(gp_g_first, i, 1), gp_g_btn_y,
                                 GP_BTN_W, GP_BTN_H);   /* gp_layout sizes it */
        if (!gp_g_btn[i]) {
            MDBG("gpanel: button %d would not build -> no BPM group\n", i);
            return;
        }
        juce_comp_colour(gp_g_btn[i], LBL_COL_OUTLINE, 0x00000000u);
    }

    /* Above the strip, so it is a child of the strip's parent. Skipped if there
     * is no room; the buttons work without it. */
    gp_g_parent = juce_comp_parent(panel);
    if (gp_g_parent && gp_g_panel_b[1] >= GP_BAND_H) {
        gp_g_band = juce_label(gp_g_parent, "", GP_RO_FONT,
                               GP_COL_GROUP, GP_COL_TEXT, 0,
                               GP_BAND_X(gp_g_first), gp_g_panel_b[1] - GP_BAND_H,
                               GP_BAND_W, GP_BAND_H);
        if (gp_g_band) {
            /* Children of the band, so they move and hide with it and need no
             * background of their own. */
            int ux = (GP_BAND_W - GP_UNIT_W) / 2;
            uintptr_t cap;

            juce_comp_colour(gp_g_band, LBL_COL_OUTLINE, 0x00000000u);
            cap = juce_label(gp_g_band, "BPM", GP_CAP_FONT, 0x00000000u,
                             GP_COL_CAP, 0, ux, 0, GP_CAP_W,
                             GP_RO_H + GP_RO_DESC);
            if (cap) {
                juce_comp_colour(cap, LBL_COL_OUTLINE, 0x00000000u);
                juce_label_justify(cap, JUCE_JUSTIFY_BOT_R);
            }
            gp_g_readout = juce_label(gp_g_band, "--.-", GP_RO_FONT,
                                      0x00000000u, GP_COL_TEXT, 0,
                                      ux + GP_CAP_W, 0, GP_VAL_W,
                                      GP_RO_H + GP_RO_DESC);
            if (gp_g_readout) {
                juce_comp_colour(gp_g_readout, LBL_COL_OUTLINE, 0x00000000u);
                juce_label_justify(gp_g_readout, JUCE_JUSTIFY_BOT_L);
            }
            juce_comp_set_visible(gp_g_band, juce_comp_visible(panel));
        }
    } else {
        MDBG("gpanel: only %d px above the strip -> no BPM readout\n",
             gp_g_parent ? (int)pb[1] : -1);
    }

    if (gp_hook_panel(panel) != 0) {
        MERR("gpanel: could not hook the panel's paint -> no group plates\n");
        return;
    }
    gp_layout();
    MDBG("gpanel: BPM group built -- left group at %d, ours at %d, band %s\n",
         GP_GROUP_L, gp_g_first, gp_g_band ? "yes" : "no");
}

/* ================================================================== */
/* Bring-up                                                           */
/* ================================================================== */

/* The panel is opened by holding the rotary, a hardware gesture (hence
 * gui::IHuiControllable on GridAdjust). The ZOOM/GRID indicator beside the
 * waveform only shows that mode; taps on it do nothing.
 *
 * The anchor is the waveform title bar's paint, which the stem row and the cue
 * shortcut also chain from: its `self` is a live component and the parent chain
 * is wired by the time anything is drawn. It is not a clock: nothing happens
 * until something repaints the bar. */
typedef void (*gp_paint_t)(void *self, void *g);
static uintptr_t gp_g_orig_paint;

static void gp_find(uintptr_t any_component);

static void gp_ta_paint(void *self, void *g)
{
    if (gp_g_orig_paint)
        ((gp_paint_t)gp_g_orig_paint)(self, g);
    gp_find((uintptr_t)self);
}

static void gp_find(uintptr_t any_component)
{
    uintptr_t root, panel;

    if (gp_g_panel || !any_component)
        return;
    root = juce_comp_root(any_component);
    if (!root)
        return;
    panel = juce_comp_find_class(root, GP_TI_PANEL);
    if (!panel) {
        /* Logged once, to distinguish "panel not open" from an anchor that
         * never fired. */
        static int said;

        if (!said) {
            said = 1;
            MDBG("gpanel: anchor fired, root %p, %d children -- no GridAdjust."
                 " The tree as it stands:\n",
                 (void *)root, juce_comp_nchild(root));
            gp_dump(root, 0);
        }
        return;
    }

    gp_g_panel = panel;
    MDBG("gpanel: found GridAdjust at %p -- the panel as it really is:\n",
         (void *)panel);
    gp_dump(panel, 0);
    gp_build(panel);
}

static int gp_install(void)
{
    if (!ep122_sym(EP122_GRIDPANEL)) {
        MDBG("gpanel: no GridAdjust class -> no grid panel\n");
        return -1;
    }
    if (!FN_LABEL_CTOR || !FN_ADD_VISIBLE || !FN_SET_BOUNDS || !FN_FONT_BUILD ||
        !FN_LABEL_SETFONT || !FN_LABEL_JUSTIFY || !FN_COMP_SETCOLOUR) {
        MDBG("gpanel: juce primitives did not resolve -> no BPM group\n");
        return -1;
    }
    /* Chained: the shortcut and the stem row also hook this slot, each calling
     * the one it displaced. Priority orders the chain. */
    if (mod_patch_vslot("gridPanelAnchor", EP122_TOUCHARIA, JUCE_VT_PAINT,
                        (void *)gp_ta_paint, &gp_g_orig_paint) != 0) {
        MDBG("gpanel: no paint anchor -> cannot find the panel\n");
        return -1;
    }
    return 0;
}

KIT_MOD(k_mod_grid_panel,
        .name = "grid_panel", .prio = 35, .install = gp_install,
        .what = "grid panel: the BPM group the 3000X has and this deck does not");
