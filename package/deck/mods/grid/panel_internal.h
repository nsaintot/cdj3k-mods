/* SPDX-License-Identifier: MIT OR Apache-2.0 */
/*
 * mods/grid/panel_internal.h - what the grid panel files share.
 *
 * panel.c owns the hooks, the build and every piece of state below;
 * panel_button.c one button; panel_layout.c where they sit; panel_debug.c the
 * tree dump. Declarations only.
 */
#ifndef EP122_MOD_GRID_PANEL_INTERNAL_H
#define EP122_MOD_GRID_PANEL_INTERNAL_H

#include "core/mod_core.h"
#include "juce/draw.h"
#include "juce/juce.h"

/* The five buttons, in the order they are built and laid out. */
enum { GP_HALF, GP_DOUBLE, GP_ENLARGE, GP_REDUCE, GP_RESET };

/* The panel's geometry, in screen pixels.
 *
 * One common seen gap, GP_VGAP, between any two buttons, with three exceptions
 * from the design:
 *
 *   1. the deck's 1/2 pair is flush. The design draws those two as one
 *      rounded rect with a divider and the four BPM buttons as separate rects,
 *      so this does not carry over to ours.
 *   2. the BPM group's RESET stands GP_VRESET off its last button, off the plate.
 *   3. the two groups are at least GP_VGROUP apart.
 *
 *   [SNAP][SHIFT][1/2][1/2][RST]     [[ x1/2 ][ x2 ][ <| ][ |> ]]   [RST]
 *
 *   in bounds, on a 1280px panel:
 *   20 |<---- 583 ---->| 52 |<-- plate 506 -->| 9 |90| 20        = 1280
 *
 * Every button is the deck's height, 84, and both RESETs share the smaller box,
 * since any difference in a row of firmware artwork reads as a mistake. The
 * plate's margin therefore cannot come out of a button: GridAdjust is grown by
 * GP_PAD at each end and every button moved GP_PAD down it, back on the row the
 * firmware used. juce clips a child to its parent, so there is no other way. */
#define GP_BTN_W        116
#define GP_BTN_H        84
#define GP_PLATE_INSET  1
/* RESET is shorter than the four buttons it undoes, as on the 3000X. Both RESETs
 * use this size, so the deck's is repainted (paint only; the press still goes to
 * the firmware). The bounds themselves are reduced, not just the drawn plate: the
 * eye measures gaps to the artwork, and a small plate centred in a 116 slot left
 * an 18px gap to the deck's RESET against 8 elsewhere. */
#define GP_RESET_W      90
#define GP_RESET_H      50
#define GP_N            5
#define GP_VGAP         10             /* between any two neighbours           */
#define GP_VRESET       (2 * GP_VGAP)  /* the last button to that group's RESET */
#define GP_GROUP_L      GP_MARGIN

/* ---- button colours, from the design --------------------------------------
 *
 * Outline 2 px, inner 112x80, so 114x82 of plate inside a 116x84 component.
 * Inactive is #7D7D7D on #323232; touched is #FFF on #646464. These come from the
 * panel design, not sampling, and are authoritative.
 *
 * They are deck values, not mod_ui() roles: every use wraps them in
 * mod_colour_stock, so each theme maps them the same way it maps the deck's own
 * button sprites (image.c themes ARGB sprites within THEME_IMG_MAXDIM). Roles do
 * not fit: four of these (#646464, #101010, #262626, #4b4b4b) have no matching
 * role, and none of the role constraints (separate stem hues, a checker's fixed
 * ratio, the accent blue) apply to a strip of greys. ORIGINAL stays
 * bit-identical to the design. */
#define GP_BTN_EDGE     2

#define GP_COL_LINE     0xff7d7d7dU

#define GP_COL_FILL     0xff323232U

#define GP_COL_LINE_ON  0xffffffffU

#define GP_COL_FILL_ON  0xff646464U

/* The "nothing to undo" state, from the design (not sampled). */
#define GP_COL_LINE_OFF 0xff262626U

#define GP_COL_FILL_OFF 0xff101010U

#define GP_COL_TEXT_OFF 0xff4b4b4bU

#define GP_COL_TEXT     0xffffffffU

#define GP_COL_GROUP    0xff1a1a1aU   /* the plate a group sits on */

#define GP_FONT         20.0f

/* The band over the group holds a small dim "BPM" and a readable value. A Label
 * has one font, so it is three components: the band backdrop, and the word and
 * the number inside it, placed as one unit centred on the band so "BPM" sits
 * against its number.
 *
 * Both boxes are oversized: juce::Label narrows text to its minimum horizontal
 * scale before clipping, so a slightly short box looks like a condensed typeface.
 * "126.0" at this size is ~60px and Label insets 5px each side, so 76 already
 * squashed it. The boxes abut, so the space between word and number is fixed.
 *
 * Both use the band's full height: juce centres text in its box, so boxes of
 * different heights would put the word above the number. */
#define GP_RO_FONT      20.0f

#define GP_CAP_FONT     14.0f

#define GP_COL_CAP      0xff7d7d7dU

#define GP_CAP_W        50

#define GP_VAL_W        100

#define GP_UNIT_W       (GP_CAP_W + GP_VAL_W)

/* ---- the arrows are drawn, not typed --------------------------------------
 *
 * U+25C0/U+25B6 exist in two of the deck's three faces, but at the bars' size
 * they are much heavier than the bars, and a Label has one font. So the label
 * is the bars alone and the triangles are drawn separately, as one filled rect
 * per column (seven fills per arrow). */
#define GP_ARROW_W      7     /* columns from base to apex */

#define GP_ARROW_HALF   6     /* half the base height      */

/* Offset from the button's centre, so the arrows sit against the bars: ||| at
 * this size is 13px wide (edges at +-7), leaving ~4px either side. */
#define GP_ARROW_OFF    14

#define GP_RESET_FONT   20.0f

#define GP_N_PLATE      4     /* buttons on the plate; RESET stands off it */

#define GP_MARGIN       20    /* screen edge to the outermost button          */

#define GP_VPLATE       GP_VGAP        /* the plate's border round its four    */

#define GP_VGROUP       (4 * GP_VGAP)  /* the deck's RESET to that plate       */

#define GP_OGAP         (GP_VGAP - 2 * GP_PLATE_INSET)  /* ours, in bounds     */

#define GP_PAD          GP_PLATE_PAD

/* The plate hugs its four buttons. Its padding is also the vertical margin, so
 * the BPM band above sits this close to the buttons. */
#define GP_PLATE_PAD    (GP_VPLATE - GP_PLATE_INSET)

#define GP_LEFT_W       (GP_N_PLATE * GP_BTN_W + GP_RESET_W + 2 * GP_VGAP + \
                         (GP_VGAP - GP_PLATE_INSET))

#define GP_OURS_W       (GP_N_PLATE * GP_BTN_W + 3 * GP_OGAP + \
                         GP_VRESET - 2 * GP_PLATE_INSET + GP_RESET_W)

#define GP_PLATE_W      (GP_N_PLATE * GP_BTN_W + 3 * GP_OGAP + 2 * GP_PLATE_PAD)

#define GP_PANEL_H      (GP_BTN_H + 2 * GP_PAD)

/* The band above the buttons is narrower than the plate: its edges are the
 * middle of the gaps after the first and the third button, so both continue an
 * existing line. It must stay left of x 1180, where the waveform's ZOOM/GRID
 * indicator sits. */
#define GP_BAND_X(f)    ((f) + GP_BTN_W + GP_OGAP / 2)

#define GP_BAND_W       (2 * (GP_BTN_W + GP_OGAP))

/* The BPM readout sits above the group, outside the 84px strip, so it is a child
 * of the panel's parent (juce clips a child to its parent's bounds).
 *
 * It has the group's plate colour and sits flush on the strip, so the two read as
 * one block. The panel cannot draw outside its own bounds and the parent's paint
 * is under the waveform's, so this takes a separate component. */
#define GP_RO_H         22

/* The text box hangs GP_RO_DESC below the band and is bottom-justified, so the
 * glyphs, not juce's empty descender space, sit at the band's bottom edge (6px
 * of descent plus the plate's 10px border would put the value 16px off the
 * buttons). The overhang must stay under the font's descent because the band
 * clips its children: at 6 the digits lose their bottom 2px. At 3 the glyphs
 * stop one pixel inside the band, a border's width off the buttons. */
#define GP_RO_DESC      3

#define GP_BAND_H       GP_RO_H   /* the lettering, and no padding of its own */

/* ---- identity ------------------------------------------------------------
 *
 * A class identity is its typeinfo, not its vtable. ep122_sym gives the vtable's
 * address point; juce_comp_class gives the typeinfo one word before whichever
 * vtable an object points at. Comparing those two never matches, which looks
 * like "the panel is not built yet". Every vtable in a class's group shares the
 * typeinfo, which is why this works for a virtual base. */
#define GP_TI_PANEL   juce_class_of(ep122_sym(EP122_GRIDPANEL))

#define GP_TI_SNAP    juce_class_of(ep122_sym(EP122_GRIDBTN_SNAP))

#define GP_TI_SHIFT   juce_class_of(ep122_sym(EP122_GRIDBTN_SHIFT))

#define GP_TI_RESET   juce_class_of(ep122_sym(EP122_GRIDBTN_RESET))

/* ---- what the buttons do ------------------------------------------------
 *
 * Each of the five is one call into stem_grid_edit_*. From the 3000X manual:
 * [x2]/[x1/2] "doubles or halves the number of beats", and [Enlarge]/[Reduce]
 * "moves the beatgrid by 1 msec based on the first grid": an interval change
 * anchored at beat one, not an offset shift, which the deck's own
 * gridAdjustChengeReq cannot express. */
#define GP_MS           0.001         /* what Enlarge/Reduce move the interval by */

/* Far past any usable tempo either way and within what the grid code accepts,
 * so repeated presses stop at a limit rather than at a refusal. */
#define GP_MULT_MAX     8.0

#define GP_MULT_MIN     0.125

#define GP_STEPS_MAX    200

/* Whether the deck's RESET has anything to undo, from the deck's own model: the
 * byte at -0x28 from the Component subobject is 0 when there is something to
 * reset and 2 when not. Not juce's enabled flag: all five stock buttons read
 * flags 0x2, lit SNAP GRID and greyed SHIFT GRID alike.
 *
 * The stock button's brief held look (often invisible at 60fps) is not
 * reproduced: its paint goes through the generic widget-image path and the object
 * keeps no held state. Reading it would mean hooking the
 * button's press path, which crashed the deck; do not retry. Our own RESET has a
 * held look because it is a Label with our vtable and gets its own mouseDown.
 *
 * Read, never written. */
#define GP_ACTIVE_OFF   0x28          /* before the Component subobject */

#define GP_ACTIVE_VAL   0



/* All defined in panel.c, which builds the panel that owns them. */
extern uintptr_t gp_g_panel;
extern int32_t   gp_g_panel_b[4];
extern int       gp_g_panel_h;
extern uintptr_t gp_g_btn[GP_N];
extern int       gp_g_btn_y;
extern int       gp_g_first;
extern uintptr_t gp_g_readout;
extern double    gp_g_shown_bpm;
extern uintptr_t gp_g_stock[GP_N];

/* The pieces panel.c wires together when it builds. */
void gp_label_mousedown(void *self, void *event);
void gp_label_mouseup(void *self, void *event);
void gp_label_paint(void *self, void *g);
void gp_reset_paint(void *self, void *g);
int gp_btn_x(int first, int i, int ours);
void gp_layout(void);
void gp_readout_sync(void);
void gp_dump(uintptr_t comp, int depth);

/* Button states: OFF is nothing to undo, ON is a finger on it, IDLE otherwise. */
enum { GP_STATE_ON = -1, GP_STATE_IDLE = 0, GP_STATE_OFF = 1 };

extern int       gp_g_hot;
extern double    gp_g_mult;
extern uintptr_t gp_g_orig_mouseup;
extern int       gp_g_steps;

void gp_action(int which);
void gp_repaint(uintptr_t comp);

#endif /* EP122_MOD_GRID_PANEL_INTERNAL_H */
