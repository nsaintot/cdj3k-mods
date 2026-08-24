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
 * on the beat interval, which stem_grid_edit_scale already does and no firmware
 * mode can -- see stem/grid.c. This file is the panel side: it shifts the deck's
 * five buttons left and builds the second group beside them, on a plate of its
 * own so the five read as one group acting on one number. Only ours gets a
 * plate: the deck's five are its own controls and its own artwork.
 *
 * ---- finding the panel -----------------------------------------------------
 *
 * gui::detailed_waveform::GridAdjust is MODEL-FIRST, like the play-screen rack:
 * its primary vtable is a ten-slot updater interface whose +0xd0 is a `return 0`
 * stub, and the juce::Component it also is lives at a further vtable. It is a
 * VIRTUAL base, so ep122_syms.spec cannot name that vtable with `base=` -- the
 * generator refuses with "0 vtables at offset-to-top 24" and is right to: for a
 * virtual base the RTTI records an index into the vtable, not an offset.
 *
 * So the panel is found by TYPEINFO through the component tree, which every
 * vtable in a class's group agrees on. See juce.h and [play-screen rack].
 *
 * And the Component vtable comes off the LIVE OBJECT rather than out of the
 * spec: the child pointer in the tree IS the Component subobject, so the word at
 * it is exactly the vtable we want, and the typeinfo behind it says whose. That
 * is a stronger identity check than an address, not a weaker one -- what is
 * skipped is only the ability to state it on the Mac.
 *
 * ---- geometry is READ, never assumed ---------------------------------------
 *
 * The existing buttons' bounds come out of the live tree, so the new group is
 * placed against what is actually there. Measured on 3.19, in panel coordinates:
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
 * same size and inset the same 1px, which is what makes the two groups line up
 * rather than merely sit near each other.
 */
#include "grid/panel_internal.h"
#include "core/ep122_syms.h"
#include "juce/draw.h"
#include "juce/juce.h"
#include "kit/mod.h"
#include "stem/stem.h"





/* THE SMALLER THING IS THE COMPONENT, not a plate drawn small inside a
 * full-size one. Measured with the plate centred in a 116 slot instead: the gap
 * to the deck's RESET read 18px against 8 everywhere else, because the eye
 * measures to the ARTWORK and the slot's spare 20px sat in front of it. With the
 * bounds themselves reduced, the common gap puts it where the common gap puts
 * everything. */









/* GLYPH COVERAGE IS CHECKED, NOT HOPED FOR. The deck carries three faces in
 * /usr/share/fonts/ttf and they do not agree, so the labels use only what all of
 * them draw, plus the two arrows the big two have:
 *
 *   U+00D7 x     the multiplication sign   -- in all three
 *   U+007C |     the grid bars             -- in all three
 *   U+25C0 / U+25B6  black triangles       -- UCGothic_J and TsukuGoCustomPro,
 *                                             NOT in UCGothicLatin (551 glyphs)
 *   U+25C4 / U+25BA  the "pointer" pair    -- in NONE of them, so not these
 *
 * Read out of the cmaps with fontTools rather than guessed at; a missing glyph
 * renders as a blank box, which reads as a broken button rather than as a font
 * problem. */
static const char *const gp_text[GP_N] = {
    "|||\xc3\x97" "1/2",                    /* |||x1/2 */
    "|||\xc3\x97" "2",                      /* |||x2   */
    "|||",                                  /* enlarge: arrows drawn, see below */
    "|||",                                  /* reduce                           */
    "RESET"
};

/* For the log only. The two arrow buttons carry the same text, so a line naming
 * the label cannot say which one was pressed -- which is exactly what a log of a
 * fine adjust is for. */
static const char *const gp_name[GP_N] = {
    "x1/2", "x2", "ENLARGE", "REDUCE", "RESET"
};

uintptr_t gp_g_panel;                  /* the GridAdjust component      */
static uintptr_t gp_g_parent;                 /* what it hangs off             */
int       gp_g_first;                  /* x of OUR first button, off the panel's own width */
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

/* WHETHER THE DECK'S RESET HAS ANYTHING TO UNDO, which its own artwork used to
 * say and this has to say instead. It is NOT in juce::Component -- all five
 * stock buttons read flags 0x2, the lit SNAP GRID and the greyed SHIFT GRID
 * alike -- so it comes from the grid itself: every one of the firmware's grid
 * modes moves the same int16 OFFSET and nothing else, so a non-zero offset IS
 * "there is something to reset".
 *
 * Read, not observed. Watching the four buttons' presses instead needed a hook
 * on each of their mouseDown slots, and that CRASHED THE DECK the first time the
 * RESET was touched -- a button's own press path is not somewhere to stand. */
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

/* THE PANEL OWNS THE EDIT, NOT THE GRID. stem_grid_edit_scale always rescales
 * the grid the deck loaded, so a second call REPLACES the first rather than
 * compounding -- which is right for the machinery (x2 twice is x2, and RESET is
 * a restore rather than an inverse) and wrong for four buttons a DJ taps in a
 * row. Measured before this existed: Enlarge pressed twice moved the grid by one
 * millisecond, not two, and x2 followed by Enlarge landed on 125.7 rather than
 * 251.5, because the second press was computed from the tempo it was reading and
 * then applied to the original.
 *
 * So the panel keeps the WHOLE edit -- a multiplier and a count of millisecond
 * steps -- and states it as one k against the original every time. Which is also
 * exactly what the 3000X's own wording says the fine adjust does: "moves the
 * beatgrid by 1 msec BASED ON THE FIRST GRID". */
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
            /* Saved like any other edit: putting the grid back is a change to
             * the track as much as moving it was, and a RESET that only held
             * until the next load would be the worse surprise. */
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
         * task and returns, so the finger never waits on the media, and every
         * edit states the whole grid rather than a difference from the last. */
        stem_grid_edit_save();
    } else {
        /* The grid did not move, so neither does what the panel thinks it is. */
        gp_g_mult  = was_mult;
        gp_g_steps = was_steps;
        MDBG("gpanel: %s (k=%.6f) refused\n", gp_name[which], k);
    }
}

/* ================================================================== */
/* One button                                                         */
/* ================================================================== */

/* The plate for one control: 2px edge, filled, inset a pixel inside the bounds
 * so it lands where the deck's artwork lands. `h` is the plate's own height,
 * which is not the component's for a RESET. */


/* Enlarge points OUT (the beats spread), Reduce points IN. Only these two have
 * arrows; everything else says what it does in words. */
/* The plate, then the stock Label::paint for the lettering. That order and not
 * the other one: Label draws its background and its text in a single call, so a
 * plate laid down afterwards falls across the word. Its own background and
 * outline colours are left transparent for exactly this reason -- the two-pixel
 * edge the design asks for is not something Label::drawRect can do anyway. */
/* Press acts, release only un-lights. The deck's own grid buttons act on press
 * too, and a control that repeats while held would make [x2] unusable. */
/* ================================================================== */
/* The readout                                                        */
/* ================================================================== */

/* ================================================================== */
/* Layout                                                             */
/* ================================================================== */

/* The gap BEFORE button i, in either group. Zero at 3 because 2 and 3 are the
 * flush pair, and the plate's own margin plus GP_SEP at 4 because the RESET
 * stands off the plate rather than sitting on it. */
/* How far a button's artwork stops inside its bounds. The deck's are PNGs that
 * fill theirs; everything we draw is inset. */
/* Re-asserted whenever the panel is shown rather than on every paint: a panel
 * that is being re-laid out does it in resized(), and resized() runs on the way
 * back up. Doing it per paint would also mean a setBounds inside a paint, which
 * schedules the repaint that would call it again.
 *
 * The panel's OWN height is part of the layout: the plate needs a margin under
 * the buttons and the buttons are the deck's height, so the 84px strip is grown
 * by GP_PAD. It has the room -- the strip sits 8px above the bottom of the
 * waveform view. Guarded against re-entry because setBounds runs resized(). */
/* ================================================================== */
/* The panel                                                          */
/* ================================================================== */

/* Chained AFTER the stock paint and BEFORE the children, which is the whole
 * reason the plates are drawn here rather than as components: a juce parent
 * paints, then its children paint over it, so this is the one place a backdrop
 * for somebody else's buttons can go without touching their z-order. */
static void gp_panel_paint(void *self, void *g)
{
    int32_t b[4];

    if (gp_g_orig_panel_paint)
        ((void (*)(void *, void *))gp_g_orig_panel_paint)(self, g);
    if (juce_comp_bounds((uintptr_t)self, b) != 0)
        return;

    /* OURS ONLY. The deck's five are its own controls and its own artwork; a
     * plate behind them is this mod redecorating the firmware's half of the
     * panel, which is not what the group is for -- the plate is what says these
     * five belong together and act on one number. */
    mod_gfx_colour(g, mod_colour_stock(GP_COL_GROUP));
    mod_gfx_fill(g, gp_g_first - GP_PLATE_PAD, 0, GP_PLATE_W, b[3]);

    gp_readout_sync();
}

/* The readout lives outside the panel, so it has to be told when the panel comes
 * and goes. This is that wire, and it is a hook rather than a poll because the
 * two have to move in the same frame -- a BPM box left behind over the waveform
 * after the panel closes is worse than no box. */
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
    /* The object said which class it is before we got here, so this is the
     * Component vtable of gui::detailed_waveform::GridAdjust and nothing else.
     * expect_fn is what the slot already holds: the identity argument is the
     * typeinfo above, not a remembered address. */
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

    /* Placed against the panel's own width rather than against 1280: the whole
     * point of reading the geometry is not to have written it down. Two groups
     * that would overlap is the one case where doing nothing is right -- the
     * deck's five buttons stay where the firmware put them. */
    gp_g_first = pb[2] - GP_MARGIN - GP_OURS_W;
    if (gp_g_first - GP_PLATE_PAD - GP_VGROUP < GP_GROUP_L + GP_LEFT_W) {
        MDBG("gpanel: a %d px strip does not hold both groups -> no BPM group\n",
             (int)pb[2]);
        return;
    }

    /* The strip is grown BOTH WAYS so the plate has its border above and below
     * buttons that are the deck's own height -- the border cannot come out of a
     * button without making ours shorter than the five beside them. The buttons
     * then sit GP_PAD down the taller strip, which puts them back on exactly the
     * row the firmware had them on.
     *
     * Grown against the parent's MEASURED height, and CLAMPED to it rather than
     * refused: the waveform view has 8px under the strip and the border wants 9,
     * so asking for the full height and taking what is there costs one pixel off
     * the bottom border and keeps the other three. A parent with no room at all
     * keeps the strip as it was and the plate ends at the buttons. */
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
        /* The RESET takes the RESET's font, so the two of them are the same
         * control drawn twice rather than two controls that look alike. */
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

    /* Above the strip, so it belongs to the strip's parent. A parent that has no
     * room for it simply does not get one -- the four buttons are the feature and
     * the readout is what tells you what they did. */
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

/* THE PANEL IS OPENED BY HOLDING THE ROTARY -- it is a hardware gesture, which
 * is what gui::IHuiControllable on GridAdjust means, and the ZOOM/GRID indicator
 * beside the waveform is a readout of that mode rather than a touch target
 * (taps along it land on nothing, measured).
 *
 *     printf 'press ROTARY_PRESS\nsleep 1600\nrelease ROTARY_PRESS\n' | emuctl.py -
 *
 * THE ANCHOR is the waveform title bar's paint, the same one the stem row and
 * the cue shortcut already chain from -- it fires when that bar is invalidated,
 * its `self` is a live component, and the parent chain is wired by the time
 * anything is drawn. It is NOT a clock: nothing here happens until something
 * repaints the bar. */
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
        /* Said ONCE. Silence here would be indistinguishable from an anchor
         * that never fired, and those two want completely different fixes --
         * one is a gesture to find, the other is a hook to fix. */
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
    /* Chained, not owned: the shortcut and the stem row are already on this
     * slot and each calls the one it displaced. Priority orders the chain. */
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
