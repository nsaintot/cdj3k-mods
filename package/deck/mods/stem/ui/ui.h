// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * ui.h - the contract between the parts of the STEMS play-screen UI.
 *
 * The quick-menu button and the stem control row. The settings half is in menu/
 * (ENABLE STEMS / STEM SERVER LOCATION / STEM SERVER ADDRESS). Nothing here touches audio.
 *
 * Approach (RE-verified against EP122-3.19)
 * -----------------------------------------
 * Built from the app's own juce classes: no artwork added, no stock function patched
 * in place. Two vtable slots on one small app class are repointed, only to find the
 * attachment point:
 *
 *   gui::WaveformViewTitleWidget is the 1280x90 bar carrying the artwork, the track
 *   labels and the three quick-menu buttons (BEAT LOOP x=898, KEY SHIFT x=1022,
 *   BEAT JUMP x=1146, stride 124). Ctor sub_15c3140. It inherits juce::Component
 *   VIRTUALLY, so its own vtable group is awkward to hook -- but it owns a plain
 *   Component child, `TouchAria` (vtable 0x2159178, ctor inside sub_15c3140 at
 *   0x15c4014), whose parent pointer IS the bar. Hooking TouchAria's paint slot
 *   yields that parent the first time the bar draws.
 *
 * Layout, measured from sub_15c3140:
 *   - the 4th quick-menu slot is 898 - 124 = x=774, 114x90 (the stock art size).
 *   - the control row is pinned to the BOTTOM of the bar's own parent (the waveform
 *     view), full width. Anchoring to the grandparent's height rather than the bar
 *     puts it under the waveform whether the bar is drawn above or below it.
 *
 * Widgets:
 *   - buttons are juce::Labels (ctor sub_1bb8330, 0x1d0 bytes, vtable 0x25150d0).
 *     A Label already paints backgroundColourId + text + outlineColourId, which is
 *     the whole of a flat button, so nothing here implements paint(). To make one
 *     clickable the Label vtable is CLONED into our own writable array with only
 *     mouseDown (+0x28) replaced; instances of ours point at the clone, so no stock
 *     Label is affected.
 *   - sliders are real juce::Sliders (ctor sub_1bf8f80, 0x180 bytes). The app links
 *     the whole class -- sub_15c8488 constructs one -- so dragging, range clamping
 *     and thumb rendering are stock code.
 *
 * The Component pointers below are shared because every part of this UI draws into
 * one bar and one row owned by the app. They are declared here and defined in the
 * file that owns them: state.c for the app's tree, wedge.c for the wedge's.
 *
 * File layout under mods/stem/ui/:
 *
 *   state.c    the shared Component pointers and flags, defined once
 *   widgets.c  juce primitives, and the Label vtable clone that makes a button
 *   wedge.c    the three level controls: paint, gesture, level maths
 *   panel.c    the row's visibility, and claiming/handing back the waveform band
 *   row.c      captions, mute, bypass, and the progress bar that replaces them
 *   layout.c   finding the stock quick-menu panel and snapshotting what moves
 *   build.c    constructing the button and the row against the anchor
 *   hooks.c    the anchor hooks, the polls they drive, and install
 *
 * Everything here runs on the juce MESSAGE thread, which is also the only repaint
 * tick -- see stem.h. Nothing in this directory may block.
 */
#ifndef EP122_MODS_STEM_UI_H
#define EP122_MODS_STEM_UI_H

#include "stem/stem.h"
#include "juce/juce.h"
#include "kit/band.h"

#ifdef __cplusplus
extern "C" {
#endif


/* ---- juce::Component vtable slots ----
 * Pinned by diffing juce::ImageComponent (0x2514f30, overrides ONLY paint) and
 * juce::Label (0x25150d0) against plain juce::Component (0x2508760); mouseDown at
 * +0x28 and setVisible at +0x60 agree with ../../juce.h, which owns both. */
#define VT_SLOT_MOUSEDOWN   0x28
#define VT_SLOT_MOUSEDRAG   0x30
#define VT_SLOT_MOUSEUP     0x38
#define VT_SLOT_PAINT       0xd0

/* juce::Component: parent pointer and bounds, both cleared by the ctor's
 * memset(this+0x18, 0, 0x34). Bounds is {x,y,w,h} int32. The child list is the
 * juce::Array<Component*> that `addChildComponent` (sub_1ba6a40) grows. */
#define COMP_PARENT_OFF     0x18
#define COMP_BOUNDS_OFF     0x20
#define COMP_CHILDREN_OFF   0x40    /* Component** */
#define COMP_NCHILD_OFF     0x50    /* int, numUsed */
#define COMP_FLAGS_OFF      0xc0
#define COMP_FLAG_VISIBLE   0x02    /* the bit addChildComponent tests before re-showing */

/* ---- the anchor: gui::WaveformViewTitleWidget::TouchAria (vtable 0x2159178) ---- */
/* Its only overrides are mouseDown and mouseUp; paint is still the shared empty
 * `ret` stub, which is what makes the paint slot free to take. */
#define TA_VT               ep122_sym(EP122_TOUCHARIA)
#define FN_TA_MOUSEDOWN     ep122_sym(EP122_TOUCHARIA_MOUSEDOWN)
#define FN_EMPTY_STUB       ep122_sym(EP122_TOUCHARIA_PAINT)  /* `ret` -- every {} virtual, ICF-folded */

/* ---- the tick: gui::DisplayRefleshCycleTimer (vtable 0x20b7730) ----
 *
 * The title bar's paint is the wrong clock for anything that moves. It is a paint,
 * so it fires when the bar is INVALIDATED and not otherwise -- fine for building the
 * row (a repaint is exactly when the tree is there to attach to) and useless for a
 * progress bar, which has to advance while nothing on that bar has changed.
 *
 * This is the app's own display refresh: its callback drives waveformView->update,
 * widget->update and buffer->updateAll, on the message thread, for as long as the UI
 * is alive. Chaining it costs one slot and gives the row a real frame rate. */
#define DRC_VT              ep122_sym(EP122_DISPLAY_REFRESH)
#define DRC_SLOT_TIMERCB    0x10   /* juce::Timer::timerCallback */
#define FN_DRC_TIMERCB      ep122_sym(EP122_DISPLAY_REFRESH_TIMERCB)

/* ---- juce::Label (ctor sub_1bb8330; size pinned by sub_15c3140's four labels) ---- */
#define FN_LABEL_CTOR       ep122_sym(EP122_JUCE_LABEL_CTOR)  /* Label(const String& name, const String& text) */
#define FN_LABEL_SETFONT    ep122_sym(EP122_JUCE_LABEL_SETFONT)  /* Label::setFont(const Font&)                   */
#define FN_LABEL_JUSTIFY    ep122_sym(EP122_JUCE_LABEL_JUSTIFY)  /* Label::setJustificationType(Justification)    */
#define LABEL_VTABLE        ep122_sym(EP122_LABEL)
#define LABEL_FN_PAINT      ep122_sym(EP122_LABEL_PAINT)  /* what LABEL_VTABLE+0xd0 must hold (post-cond)  */
#define LABEL_ALLOC_SIZE    0x1d0
/* A Label's text is not a String member: it is a juce::Value, and Label listens to
 * its own. Setting the Value is therefore the whole of setText -- valueChanged runs
 * textWasChanged() and repaint() for us -- and it needs no address for setText itself.
 * The offset is read straight out of the ctor, which builds a var from the `text`
 * argument and constructs the Value at word 0x2a. */
#define LABEL_TEXTVALUE_OFF 0x150
#define FN_VAR_FROM_STR     ep122_sym(EP122_JUCE_VAR_CTOR_STRING)  /* juce::var::var(const String&)              */
#define FN_VALUE_SETVALUE   ep122_sym(EP122_JUCE_VALUE_SETVALUE)  /* juce::Value::setValue(const var&)          */
#define FN_VAR_DTOR         ep122_sym(EP122_JUCE_VAR_DTOR)  /* juce::var::~var()                          */
#define LBL_COL_BG          0x1000280    /* juce::Label::backgroundColourId */
#define LBL_COL_TEXT        0x1000281
#define LBL_COL_OUTLINE     0x1000282

/* juce::Justification, cross-checked against the four values sub_15c3140 uses
 * (9 topLeft / 0x21 centredLeft / 0x22 centredRight). */
#define JUSTIFY_CENTRED     0x24

/* ---- juce::Graphics: enough to paint a shape a Label cannot ----
 *
 * A Label fills rects and nothing else, so the wedge needs a paint of our own. Read
 * out of LookAndFeel_V2::drawLabel (sub_1b96b10), which is what Label::paint lands in:
 * `fillAll(colour)` is sub_1ad0a80 and `setColour` sub_1ad03f0.
 *
 * fillRect is the four-int overload, pinned by shape rather than by neighbourhood: it
 * builds a Rectangle<int> from its four arguments and calls the context's own fillRect
 * at vtable +0xa8 -- the same slot Graphics::fillAll uses, which is what fixes the
 * numbering. */
#define FN_GFX_SETCOLOUR    ep122_sym(EP122_JUCE_GFX_SETCOLOUR)  /* Graphics::setColour(const Colour&)         */
#define FN_GFX_FILLRECT     ep122_sym(EP122_JUCE_GFX_FILLRECT_INT)  /* Graphics::fillRect(int x,int y,int w,int h) */
/* Component::repaint(Rectangle<int>). The rect arrives as a POINTER in x1 -- checked in
 * the disassembly of setJustificationType's call site (`add x1, sp, #0x10`), not assumed
 * from the C++ signature, which would suggest x1/x2. */
#define FN_COMP_REPAINT     ep122_sym(EP122_JUCE_COMP_REPAINT_RECT)

/* ---- juce primitives specific to this UI; the shared ones are in ../../juce.h ---- */

/* ---- the progress bar: one number, one meaning ---------------------------------
 *
 * The bar is the WHOLE JOB. Each stage reports 0..100 within its own LEG -- the deck's
 * upload, the server's separation, the local decode -- and the map turns a leg-local
 * figure into a position on the bar. A leg that filled the bar and handed over would
 * either claim to be finished or jump backwards when the next one reported from its
 * own zero.
 *
 * THE CAPTION PRINTS THE MAPPED FIGURE, never the leg's own. Two scales on one widget
 * with nothing on screen to say which is which is what put "UPLOADING 90%" a fifth of
 * the way along the bar. One number, and it is the bar's.
 *
 * The boundaries below are the SINGLE definition of the division: they weight the fill
 * and they are where the delimiters are drawn, so a delimiter cannot sit anywhere the
 * job does not actually hand over. A new leg is one entry in the list, one case in
 * stems_prog_pos, and the delimiter appears with it.
 *
 * A cache hit has no upload and no server: LOADING is the whole job and takes the whole
 * bar with no delimiters. Which shape a run has is NOT knowable from the stage in hand
 * -- LOADING is the last leg of a separation and the only leg of a cache hit -- so it
 * rides in the snapshot, set by the thread that chose the path. See via_server in
 * ../stem.h.
 *
 * FOUR SEGMENTS, and each one is reached:
 *
 *   |  upload  |========  separating  ========|  prepare + fetch  |  load  |
 *   0         20                             80                 90      100
 *
 * The third segment used to hold the download alone, five points wide, and the sidecar
 * reported that leg as `part index / part count` -- 0 then 50 for a pair, never 100. So
 * the fill stopped two points past the second delimiter and the next thing drawn was
 * LOADING at the fourth: a segment the bar never crossed, with the server's own tail
 * (reconstructing, writing) crammed into the end of the separation instead.
 *
 * Now the segment holds what actually happens between the model finishing and the deck
 * having the files: the server packaging the stems, then the sidecar pulling them. They
 * share it across two INTERIOR splits, which carry no delimiter -- one segment, and the
 * DJ sees the caption change inside it rather than the bar hand over. */
#define PROG_BOUND_UPLOAD    20  /* the deck's PCM upload ends here       */
#define PROG_BOUND_SEPARATE  80  /* the model's own work ends here        */
/* RECONSTRUCTING and WRITING share the third segment with DOWNLOADING; no delimiter
 * between them. stemd sends no progress count for these two stages: 1% each, the
 * bar holds at the start of each until the next stage begins. Both use the caption
 * PREPARING (k_stage_name in row.c). */
#define PROG_BOUND_RECON     81  /* reconstruction ends                   */
#define PROG_BOUND_WRITE     82  /* the server has the files ready        */
#define PROG_BOUND_FETCH     90  /* the two stems have landed by here     */
#define PROG_BOUND_LIST     { PROG_BOUND_UPLOAD, PROG_BOUND_SEPARATE, PROG_BOUND_FETCH }
#define N_PROG_BOUND        3    /* one delimiter per SEGMENT boundary    */
#define PROG_MARK_W         2

#define PROG_BAR_H          14
/* Longest caption the bar can hold: a stage name, two spaces and either a percentage
 * or a queue depth. Sized here rather than at the snprintf so the buffer and the
 * strcmp cache cannot be given different lengths. */
#define PROG_CAPTION_MAX    64

/* The bar's Y is DERIVED in stems_build_row from the band the wedges occupy -- centred
 * in it, so the two states of the row read at the same height. That band comes from a
 * rect discovered at runtime, so the offset is computed where the rect is known: written
 * here as a constant it would go quietly stale the next time ROW_RISE or CAPTION_H
 * moved. */

/* No prologue guards live here any more. They existed to catch a hardcoded
 * address that a firmware revision had moved; every function above now arrives
 * from a masked signature match or out of a vtable the RTTI walk found, either
 * of which says more about the target than its first eight bytes ever did. */

/* juce::var comes back INDIRECTLY via x8, the same convention font_ret_t and
 * str_ret_t use -- see ../juce.h, which explains it once. This one stays here
 * because juce::var is only ever touched by the level sliders. */
typedef struct { uint8_t _pad[32]; } __attribute__((aligned(16))) var_ret_t;
typedef var_ret_t  (*value_get_t)(void *value);
typedef double     (*var_dbl_t)(void *v);
typedef void       (*bounds_t)(void *comp, int x, int y, int w, int h);
typedef void       (*setcol_t)(void *comp, int id, void *colour);
typedef void       (*mousedown_t)(void *self, void *event);
typedef void       (*paint_t)(void *self, void *g);
typedef void       (*timercb_t)(void *self);
typedef void       (*var_str_t)(void *out, const void *str);
typedef void       (*value_set_t)(void *value, const void *v);

/* ---- layout, in pixels ----
 * The button's size and where the free slots are belong to the title bar, not to
 * STEMS, and live in kit/band.h. WHICH slot is ours is not ours to say either:
 * it depends on which other clients are switched on, so it is asked of the kit. */
#define QM_SLOT_X        stems_slot_x()
#define QM_SLOT_Y        KIT_BAND_SLOT_Y
#define QM_SLOT_W        KIT_BAND_SLOT_W
#define QM_SLOT_H        KIT_BAND_SLOT_H

/* How far the row's top edge sits above the rect the stock panels use, to put the same
 * air above the row as below it. Measured with a loop set and the row open:
 *
 *   loop indicator ends   y 371      caption starts  y 383   ->  11 px above
 *   wedge ends            y 466      footer starts   y 475   ->   8 px below
 *
 * so three pixels of the upper margin are spare, and they go to the WEDGE: the band's
 * bottom does not move, so the caption keeps its height and its distance to the footer
 * while the wedge starts three higher.
 *
 * Three, and not the ten that PANEL_GAP would suggest. The band above is not empty --
 * the LOOP INDICATOR draws in it, and only while a loop is set, so a screen measured
 * without one reports the whole gap as free and the caption collides the moment a loop
 * appears. That was tried at ten and hit the icon. Any future change here has to be
 * measured against the states that FILL that band, not the resting one. */
#define ROW_RISE         3
#define ROW_PAD          0
/* The row's two outer margins, which are NOT the same number because they answer to
 * different things.
 *
 * LEFT is alignment: the BYPASS button lines up with the PLAYER box directly beneath
 * it, so the panel sits in the deck's own left column rather than floating a few pixels
 * off it. Measured off the stock box, not chosen.
 *
 * RIGHT is reach: the last wedge only reaches full at its right edge, so an 8px margin
 * meant setting VOCALS to 100% required a finger on the last few pixels of the panel --
 * against the bezel, where a touch is as likely to be swallowed as registered. */
#define ROW_LEFT         20
#define ROW_RIGHT        36
#define ROW_GAP          12
/* BYPASS is a square, not a word. It was 168px of lettering for a button that is
 * pressed once a set, next to three controls that are used constantly -- so it gives its
 * width back to them.
 *
 * Square LITERALLY, so its width is not a constant here: it follows the row's content
 * height (see stems_build_row), which keeps it square through any change to CAPTION_H or
 * to the band, instead of drifting into a rectangle the next time one of those moves.
 * That is also why it gets no bottom bar -- mod_btn_bar refuses a rect narrower than the
 * stock 56px mark, and correctly so: the bar belongs to title-bar buttons, not to a
 * control living in the panel. */
#define CAPTION_H        36
#define CAPTION_W        150   /* fits HARMONICS at FONT_CAPTION with room to be a plate */
/* juce::Label draws its outline as a single-pixel drawRect and offers no width for it.
 * The plate is a hold-to-mute target the size of a fingertip, and at arm's length a
 * hairline does not read as the edge of one -- so the paint override adds rings INSIDE
 * the one Label drew. This is a count of rings, not a pixel width, so three costs
 * nothing but the pixels. */
#define CAPTION_EDGE_W   2
/* The wedge takes everything under the caption. "Full height" is the point of the
 * shape: a triangle only reads as one if it has room to be tall at its wide end. */
#define WEDGE_Y          (CAPTION_H + ROW_PAD / 2)
#define FONT_QUICKMENU   24.0f  /* matches the baked BEAT LOOP / KEY SHIFT lettering */
#define FONT_BUTTON      28.0f
#define FONT_CAPTION     18.0f
/* The edit badge's bullet, SIZED AND PLACED BY THE MARK rather than by the line,
 * because a dot is a fraction of its em and nothing about the line box is where
 * the DJ sees it. Every number here was measured off the deck.
 *
 * SIZE. U+2022 is about a quarter of its em, and the bounds do not constrain it:
 * an 80 pt line in a 48 px box still drew the full 19 px mark, so juce::Label is
 * not fitting text to these bounds and the font alone decides the size. 36 pt
 * gives a mark of about 9 px -- a status light beside the lettering rather than
 * a second thing competing with it.
 *
 * PLACE. The mark sits at the CENTRE of the box, which is worth stating because
 * it is not true of every glyph: U+00B7 lands at seven tenths of the height,
 * being a low mark on a centred line, and centring its box put it on top of the
 * lettering. Placing by where the mark lands, rather than by where the box goes,
 * is what makes swapping the glyph a one-line change. */
#define FONT_BADGE       36.0f
#define BADGE_BOX        28            /* square, and far larger than the mark */
#define BADGE_MARK_X     (BADGE_BOX / 2)
#define BADGE_MARK_Y     (BADGE_BOX / 2)
/* Where the mark itself is wanted, in the button's own coordinates -- level with
 * the warn badge in the opposite corner, so the two read as a pair. */
#define BADGE_AT_X       14
#define BADGE_AT_Y       14

/* ---- colours ----
 *
 * Every colour our controls use is a ROLE now, resolved through mod_ui() so the theme in
 * force decides what it actually is; the values and the reasoning behind each live in
 * theme/roles.c. What is left here is the two that are not colours at all -- both are
 * fully transparent, and they mean "draw nothing", which no theme should ever override. */
/* Transparent, because that is what the stock quick-menu panels are: BEAT LOOP and
 * friends draw their controls straight onto the app's own black, with no strip behind
 * them. An opaque band read as a slab bolted under the waveform rather than as the
 * same kind of floating control. Nothing is lost by dropping it -- the band we borrow
 * the mode for is empty by construction, since QM_MODE_OURS is past the app's four and
 * no stock panel draws into it. */
#define COL_ROW_BG       0x00000000u
#define COL_OUTLINE      0x00000000u   /* Label draws an outline rect; keep it invisible */

/* ---- the wedge, as a row of steps ----
 *
 * Not a solid triangle with an outline. That shape had two faults and this one has
 * neither:
 *
 *   - a diagonal needs antialiasing, and we have no antialiased primitive. Everything in
 *     the stock UI is a prebaked PNG for exactly this reason. Vertical bars are
 *     axis-aligned, so they are pixel-exact with the fillRect we do have and there is
 *     nothing left to smooth.
 *   - a triangle's tip has no area, so the quiet end of the range had nothing to show and
 *     the outline ate what little there was. The shortest bar is a fixed height, so the
 *     bottom of the range is as legible as the top.
 *
 * The bar and gap are the deck's own, measured off the KEY SHIFT step strip rather than
 * chosen: 5px lit, 4px dark, and #323232 for a step that is not lit. */
#define WEDGE_BAR_W      5     /* the marked bars keep the stock width */
#define WEDGE_BAR_GAP    4
/* Everything else is a pixel narrower, so a mark is bolder as well as brighter and reads
 * at a glance without looking at the shade. The PITCH is untouched -- the slot is still
 * WEDGE_BAR_W + WEDGE_BAR_GAP and a thin bar simply leaves its extra pixel as gap -- so
 * the left edges stay on an exact grid, which is what the eye reads the ramp from. */
#define WEDGE_BAR_THIN   4
/* A FLOOR on the leftmost step, not its height -- the paint works back from a whole-pixel
 * rise, so the shortest bar comes out at this or a little above. It exists so the quiet
 * end of the range has something to show. */
#define WEDGE_MIN_H      8
/* Every so many bars is a SCALE MARK, so the ramp can be read as a position and not just
 * as a slope -- there is no number on this control and forty-odd identical bars give the
 * eye nothing to count from. With the bar count the panel width works out to, the marks
 * land on the ends and the two thirds between them. */
#define WEDGE_TICK       14
/* The warn badge. Amber rather than red: nothing is broken, there is just nothing
 * to separate with -- and red on this deck means an error the DJ has to act on. */
/* ...and what a REFUSED press looks like. The badge on its own is a state; the
 * button going red is the answer to the thing the DJ just did, so it is the press
 * that gets the loud colour and it lasts only as long as the flash. */
/* The flash itself is MOD_BLINK_* in ../../draw.h: one refusal gesture, shared with
 * the X-PAD button, so the two cannot answer a press at different speeds.
 *
 * Counted in display ticks, which the app's own refresh timer delivers -- printed once
 * at startup rather than assumed, because every duration here is expressed in them. */
#define WARN_SETTLE_TICKS  44     /* hold before the badge appears: ~1 s  */

#define STEM_LEVEL_MAX   100.0
/* N_STEMS lives in ../stem.h -- the audio side needs it for the level array too. */

/* ---- shared state, defined once in state.c -------------------------------
 *
 * Component pointers into the app's own tree, plus the flags that say what we
 * have done to it. Every one of these is read by more than one part of the UI;
 * anything used by a single file stays static in that file. */


/* The STEMS button's STATE, not its colour. Storing the resolved ARGB would be a
 * cache that goes stale the moment the theme changes, leaving one button wearing
 * the old palette; the state is what is actually true, so the colour is resolved
 * at paint from whatever theme is in force then. */
enum btn_state { BTN_OFF, BTN_ON, BTN_REFUSE };

/* Our clone of the juce::Label vtable: identical to stock except mouseDown. The
 * two words ahead of the slots (offset-to-top, typeinfo) are cloned too, so
 * anything reaching for RTTI through vptr[-1] still finds Label's. */
#define VT_CLONE_SLOTS 64

extern uintptr_t stems_g_orig_ta_paint, stems_g_orig_ta_mousedown;
extern uintptr_t stems_g_orig_drc_timer;
extern int       stems_g_dumped_stock;
extern int       stems_g_api_ok;         /* every juce primitive verified at install  */
extern int       stems_g_built;          /* the button + row exist and are attached   */
extern int       stems_g_building;       /* re-entry guard: building repaints         */
extern int       stems_g_row_open;       /* the STEMS button is lit / the row is up   */

extern uintptr_t stems_g_btn_stems;
extern enum btn_state stems_g_btn_state;
extern uintptr_t stems_g_warn;           /* the badge, hidden unless earned           */
extern int       stems_g_warn_blink;     /* budget AND phase: odd/even is the colour  */
extern int       stems_g_warn_up;
extern uintptr_t stems_g_edit;           /* the "not as the track came" mark           */
extern uint32_t  stems_g_edit_col;       /* the colour it holds, 0 while hidden        */
extern uintptr_t stems_g_row;            /* the control strip (a Label as a panel)    */
extern uintptr_t stems_g_btn_bypass;
extern uintptr_t stems_g_caption[N_STEMS];
extern uintptr_t stems_g_controls;       /* BYPASS + captions + rails + fills         */
extern uintptr_t stems_g_progress;       /* the processing bar                        */
extern uintptr_t stems_g_prog_fill, stems_g_prog_text, stems_g_prog_track;
extern uintptr_t stems_g_prog_mark[N_PROG_BOUND];
extern int32_t   stems_g_prog_x, stems_g_prog_y, stems_g_prog_w;
extern int       stems_g_bypass_on;
extern int       stems_g_processing;
extern const char *const k_stem_name[N_STEMS];

extern uintptr_t stems_g_label_vt[VT_CLONE_WORDS];
extern uintptr_t stems_g_label_vptr;
extern uintptr_t stems_g_label_mouseup;  /* stock juce::Label::mouseUp, chained by ours */

/* ---- the wedge's own state (wedge.c) -------------------------------------
 * The three controls and the single gesture they share. Read outside wedge.c
 * only to draw the captions that label them and to reset levels on a track
 * change; nothing else may write them. */
extern uintptr_t stems_g_wedge[N_STEMS];
extern int       stems_g_level[N_STEMS];   /* percent, 0..STEM_LEVEL_MAX */
extern int       stems_g_mute[N_STEMS];
extern uintptr_t stems_g_grab;             /* who owns the gesture right now */
extern uintptr_t stems_g_grab_last;        /* who owned it most recently     */
extern unsigned  stems_g_ticks;            /* display ticks; the gesture cooldown */
extern uintptr_t stems_g_icon_vptr;

/* ---- what each part offers the others ------------------------------------
 * Message thread throughout. Grouped by the file that defines them, because
 * "who owns this" is the question a reader arrives with. */

/* widgets.c */
void      stems_set_visible(uintptr_t comp, int visible);
int       stems_bounds(uintptr_t comp, int32_t out[4]);
void      stems_colour(uintptr_t comp, int id, uint32_t argb);
uint32_t  stems_lighter(uint32_t argb);
void      stems_text(uintptr_t label, const char *text);
int       stems_label_vt_ready(void);
uintptr_t stems_label(uintptr_t parent, const char *text, float font_h,
                      uint32_t bg, uint32_t fg, int clickable,
                      int x, int y, int w, int h);

/* wedge.c */
uintptr_t stems_wedge(uintptr_t parent, int x, int y, int w, int h, int stem);
int       stems_icon_vt_ready(void);
int       stems_grab_take(uintptr_t self);
void      stems_grab_release(void);
void      stems_repaint(uintptr_t comp);
uint32_t  stems_touch_lift(uintptr_t comp);
uint32_t  stems_btn_surface(void);
void      stems_btn_state(enum btn_state st);
void      stems_publish_gain(int i);
void      stems_level_set(int i, int pct);
int       stems_ready(void);

/* panel.c */
void      stems_sync(void);
void      stems_dump_tree(uintptr_t comp, int depth);
void      stems_toggle_row(void);
/* Where our quick-menu button goes, asked of the band kit: it depends on which
 * other clients are switched on. panel.c owns the band descriptor. */
int32_t   stems_slot_x(void);

/* row.c -- the three below are vtable slots, reached only through the clone
 * widgets.c builds, never called directly. */
void      stems_label_paint(void *self, void *g);
void      stems_label_mousedown(void *self, void *event);
void      stems_label_mouseup(void *self, void *event);
uint32_t  stems_bypass_colour(void);
void      stems_caption_sync(int i);
void      stems_mute_blink(void);
/* Show the snapshot on the bar, or NULL to hand the row back to the controls.
 *
 * A PURE FUNCTION OF `st`: everything drawn is derived from it on this call, and the
 * only thing remembered between calls is what is already on screen -- so a tick that
 * was never taken cannot leave the row describing a job that has gone. */
void      stems_processing_set(const struct stem_ui_state *st);
/* Forget what is on the bar. The poll runs only while the row is open, so the cache
 * describes a job that may have advanced, finished, or been replaced by another while
 * nothing was watching. */
void      stems_progress_forget(void);
/* The "the picture cannot follow the faders yet" mark, in wavewait.c. Built into the
 * controls container -- NOT into BYPASS, whose press it would otherwise take. */
uintptr_t stems_wavewait_build(uintptr_t parent, int x, int y, int w, int h);
void      stems_wavewait_poll(void);
/* Percent of the bar -> pixels along it. The fill and the delimiters go through this
 * one conversion or a delimiter drifts off the boundary it marks. */
int       stems_prog_px(int pct);

/* layout.c */

/* build.c */
void      stems_build(uintptr_t anchor);

/* hooks.c */
void      stems_progress_poll(void);
int       stems_available(void);


#ifdef __cplusplus
}
#endif

#endif /* EP122_MODS_STEM_UI_H */
