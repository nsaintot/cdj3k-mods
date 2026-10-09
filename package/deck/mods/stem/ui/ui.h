// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * ui.h - the contract between the parts of the STEMS play-screen UI.
 *
 * The quick-menu button and the stem control row. The settings are in menu/
 * (ENABLE STEMS / STEM SERVER LOCATION / STEM SERVER ADDRESS). Nothing here touches audio.
 *
 * Approach
 * --------
 * Built from the app's own juce classes: no added artwork, no stock function patched
 * in place. Two vtable slots on one small app class are repointed to find the
 * attachment point:
 *
 *   gui::WaveformViewTitleWidget is the 1280x90 bar carrying the artwork, the track
 *   labels and the three quick-menu buttons (BEAT LOOP x=898, KEY SHIFT x=1022,
 *   BEAT JUMP x=1146, stride 124). It inherits juce::Component virtually, so its
 *   own vtable group is awkward to hook. It owns a plain Component child,
 *   `TouchAria` (built by the bar's ctor), whose parent pointer is the bar. Hooking TouchAria's paint slot
 *   yields the bar the first time it draws.
 *
 * Layout, from the bar's ctor:
 *   - the 4th quick-menu slot is 898 - 124 = x=774, 114x90 (the stock art size).
 *   - the control row is pinned to the bottom of the bar's parent (the waveform
 *     view), full width, so it sits under the waveform whether the bar is drawn
 *     above or below it.
 *
 * Widgets:
 *   - buttons are juce::Labels (0x1d0 bytes).
 *     A Label already paints backgroundColourId + text + outlineColourId, which is
 *     a flat button. To make one clickable the Label vtable is cloned into a
 *     writable array with mouseDown (+0x28) replaced; only our instances point at
 *     the clone.
 *   - the level controls (wedges) are Labels with a second cloned vtable that
 *     also replaces paint and the mouse slots; see wedge.c.
 *
 * The Component pointers below are shared by every part of this UI. They are
 * declared here and defined in state.c (the app's tree) or wedge.c (the wedges).
 *
 * File layout under mods/stem/ui/:
 *
 *   state.c    the shared Component pointers and flags, defined once
 *   widgets.c  juce primitives, and the Label vtable clone that makes a button
 *   wedge.c    the three level controls: paint, gesture, level maths
 *   panel.c    the row's visibility, and claiming/handing back the waveform band
 *   row.c      captions, mute, bypass, and the progress bar that replaces them
 *   wavewait.c the mark shown while the waveform cannot follow the levels yet
 *   build.c    constructing the button and the row against the anchor
 *   hooks.c    the anchor hooks, the polls they drive, and install
 *
 * Finding the stock quick-menu panel and snapshotting it is in kit/band.c.
 *
 * Everything here runs on the juce message thread, which is also the only repaint
 * tick (see stem.h). Nothing in this directory may block.
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
 * mouseDown at +0x28 and setVisible at +0x60 agree with ../../juce/juce.h, which
 * owns both. */
#define VT_SLOT_MOUSEDOWN   0x28
#define VT_SLOT_MOUSEDRAG   0x30
#define VT_SLOT_MOUSEUP     0x38
#define VT_SLOT_PAINT       0xd0

/* juce::Component: parent pointer and bounds, both cleared by the ctor's
 * memset(this+0x18, 0, 0x34). Bounds is {x,y,w,h} int32. The child list is the
 * juce::Array<Component*> that `addChildComponent` grows. */
#define COMP_PARENT_OFF     0x18
#define COMP_BOUNDS_OFF     0x20
#define COMP_CHILDREN_OFF   0x40    /* Component** */
#define COMP_NCHILD_OFF     0x50    /* int, numUsed */
#define COMP_FLAGS_OFF      0xc0
#define COMP_FLAG_VISIBLE   0x02    /* the bit addChildComponent tests before re-showing */

/* ---- the anchor: gui::WaveformViewTitleWidget::TouchAria ---- */
/* Its only overrides are mouseDown and mouseUp; paint is the shared empty `ret`
 * stub, so the paint slot is free to take. */
#define TA_VT               ep122_sym(EP122_TOUCHARIA)
#define FN_TA_MOUSEDOWN     ep122_sym(EP122_TOUCHARIA_MOUSEDOWN)
#define FN_EMPTY_STUB       ep122_sym(EP122_TOUCHARIA_PAINT)  /* `ret` -- every {} virtual, ICF-folded */

/* ---- the tick: gui::DisplayRefleshCycleTimer ----
 *
 * The title bar's paint only fires when the bar is invalidated: enough for building
 * the row, not for a progress bar that must advance while the bar is unchanged.
 *
 * This is the app's own display refresh: its callback drives waveformView->update,
 * widget->update and buffer->updateAll on the message thread for as long as the UI
 * is alive. Chaining it costs one slot and gives the row a steady tick. */
#define DRC_VT              ep122_sym(EP122_DISPLAY_REFRESH)
#define DRC_SLOT_TIMERCB    0x10   /* juce::Timer::timerCallback */
#define FN_DRC_TIMERCB      ep122_sym(EP122_DISPLAY_REFRESH_TIMERCB)

/* ---- juce::Label ---- */
#define FN_LABEL_CTOR       ep122_sym(EP122_JUCE_LABEL_CTOR)  /* Label(const String& name, const String& text) */
#define FN_LABEL_SETFONT    ep122_sym(EP122_JUCE_LABEL_SETFONT)  /* Label::setFont(const Font&)                   */
#define FN_LABEL_JUSTIFY    ep122_sym(EP122_JUCE_LABEL_JUSTIFY)  /* Label::setJustificationType(Justification)    */
#define LABEL_VTABLE        ep122_sym(EP122_LABEL)
#define LABEL_FN_PAINT      ep122_sym(EP122_LABEL_PAINT)  /* what LABEL_VTABLE+0xd0 must hold (post-cond)  */
#define LABEL_ALLOC_SIZE    0x1d0
/* A Label's text is a juce::Value that the Label listens to, so setting the Value
 * is all of setText: valueChanged runs textWasChanged() and repaint(). The offset
 * comes from the ctor, which builds a var from the `text` argument and constructs
 * the Value at word 0x2a. */
#define LABEL_TEXTVALUE_OFF 0x150
#define FN_VAR_FROM_STR     ep122_sym(EP122_JUCE_VAR_CTOR_STRING)  /* juce::var::var(const String&)              */
#define FN_VALUE_SETVALUE   ep122_sym(EP122_JUCE_VALUE_SETVALUE)  /* juce::Value::setValue(const var&)          */
#define FN_VAR_DTOR         ep122_sym(EP122_JUCE_VAR_DTOR)  /* juce::var::~var()                          */
#define LBL_COL_BG          0x1000280    /* juce::Label::backgroundColourId */
#define LBL_COL_TEXT        0x1000281
#define LBL_COL_OUTLINE     0x1000282

/* juce::Justification (9 topLeft / 0x21 centredLeft / 0x22 centredRight). */
#define JUSTIFY_CENTRED     0x24

/* ---- juce::Graphics: enough to paint a shape a Label cannot ----
 *
 * A Label only fills rects, so the wedge has its own paint, using the same
 * Graphics calls as LookAndFeel_V2::drawLabel.
 *
 * fillRect is the four-int overload: it builds a Rectangle<int> from its four
 * arguments and calls the context's fillRect at vtable +0xa8, the same slot
 * Graphics::fillAll uses. */
#define FN_GFX_SETCOLOUR    ep122_sym(EP122_JUCE_GFX_SETCOLOUR)  /* Graphics::setColour(const Colour&)         */
#define FN_GFX_FILLRECT     ep122_sym(EP122_JUCE_GFX_FILLRECT_INT)  /* Graphics::fillRect(int x,int y,int w,int h) */
/* Component::repaint(Rectangle<int>). The rect is passed as a pointer in x1, not
 * in x1/x2. */
#define FN_COMP_REPAINT     ep122_sym(EP122_JUCE_COMP_REPAINT_RECT)

/* ---- juce primitives specific to this UI; the shared ones are in ../../juce/juce.h ---- */

/* ---- the progress bar ------------------------------------------------------------
 *
 * The bar is the whole job. Each stage reports 0..100 within its own leg (the deck's
 * upload, the server's separation, the local decode) and the map turns that into a
 * position on the bar. The caption prints the mapped figure, never the leg's own.
 *
 * The boundaries below both weight the fill and place the delimiters. A new leg is
 * one entry in the list and one case in stems_prog_pos.
 *
 * A cache hit has no upload and no server: LOADING takes the whole bar with no
 * delimiters. LOADING is the last leg of a separation and the only leg of a cache
 * hit, so the run's shape comes from the snapshot, set by the thread that chose the
 * path (via_server in ../stem.h).
 *
 * Four segments:
 *
 *   |  upload  |========  separating  ========|  prepare + fetch  |  load  |
 *   0         20                             80                 90      100
 *
 * The third segment covers the server packaging the stems, then the sidecar fetching
 * them. It has two interior splits with no delimiter; only the caption changes. */
#define PROG_BOUND_UPLOAD    20  /* the deck's PCM upload ends here       */
#define PROG_BOUND_SEPARATE  80  /* the model's own work ends here        */
/* RECONSTRUCTING and WRITING share the third segment with DOWNLOADING. stemd sends
 * no progress count for these two stages: 1% each, and the bar holds at the start of
 * each until the next stage begins. Both use the caption PREPARING (k_stage_name in
 * row.c). */
#define PROG_BOUND_RECON     81  /* reconstruction ends                   */
#define PROG_BOUND_WRITE     82  /* the server has the files ready        */
#define PROG_BOUND_FETCH     90  /* the two stems have landed by here     */
#define PROG_BOUND_LIST     { PROG_BOUND_UPLOAD, PROG_BOUND_SEPARATE, PROG_BOUND_FETCH }
#define N_PROG_BOUND        3    /* one delimiter per SEGMENT boundary    */
#define PROG_MARK_W         2

#define PROG_BAR_H          14
/* Longest caption the bar can hold: a stage name, two spaces and either a percentage
 * or a queue depth. Shared by the buffer and the strcmp cache. */
#define PROG_CAPTION_MAX    64

/* The bar's Y is computed in stems_build_row, centred in the band the wedges occupy,
 * because that band comes from a rect discovered at runtime. */

/* juce::var is returned indirectly via x8, like font_ret_t and str_ret_t (see
 * ../../juce/juce.h). */
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
 * The button size and slot positions belong to the title bar and live in
 * kit/band.h. Which slot is ours depends on which other clients are enabled, so
 * the kit decides. */
#define QM_SLOT_X        stems_slot_x()
#define QM_SLOT_Y        KIT_BAND_SLOT_Y
#define QM_SLOT_W        KIT_BAND_SLOT_W
#define QM_SLOT_H        KIT_BAND_SLOT_H

/* How far the row's top edge sits above the stock panel rect, to balance the margins.
 * With a loop set and the row open:
 *
 *   loop indicator ends   y 371      caption starts  y 383   ->  11 px above
 *   wedge ends            y 466      footer starts   y 475   ->   8 px below
 *
 * The band's bottom does not move, so the three extra pixels go to the wedge height.
 *
 * Not the ten PANEL_GAP would suggest: the loop indicator draws in the band above,
 * only while a loop is set, and ten collides with it. Check any change with a loop
 * set. */
#define ROW_RISE         3
#define ROW_PAD          0
/* The row's outer margins.
 *
 * LEFT aligns the BYPASS button with the stock PLAYER box beneath it.
 *
 * RIGHT keeps the last wedge's full-level edge away from the bezel, where touches are
 * often lost; at 8px, setting VOCALS to 100% was unreliable. */
#define ROW_LEFT         20
#define ROW_RIGHT        36
#define ROW_GAP          12
/* BYPASS is a square icon button, so the wedges get the width. Its width is not a
 * constant: it follows the row's content height (see stems_build_row). It gets no
 * bottom bar because mod_btn_bar refuses a rect narrower than the stock 56px mark;
 * the bar is for title-bar buttons. */
#define CAPTION_H        36
#define CAPTION_W        150   /* fits HARMONICS at FONT_CAPTION with room to be a plate */
/* juce::Label draws a single-pixel outline with no width setting. The caption is a
 * hold-to-mute target, so the paint override adds rings inside Label's outline to
 * make its edge visible. This is a count of rings, not a pixel width. */
#define CAPTION_EDGE_W   2
/* The wedge takes the full height under the caption. */
#define WEDGE_Y          (CAPTION_H + ROW_PAD / 2)
#define FONT_QUICKMENU   24.0f  /* matches the baked BEAT LOOP / KEY SHIFT lettering */
#define FONT_BUTTON      28.0f
#define FONT_CAPTION     18.0f
/* The edit badge's bullet, sized and placed by the visible mark rather than the
 * line box.
 *
 * Size: U+2022 is about a quarter of its em, and juce::Label does not fit text to
 * its bounds, so the font alone sets the size. 36 pt gives a mark of about 9 px.
 *
 * Place: U+2022 lands at the centre of its box. Not every glyph does: U+00B7 lands
 * at seven tenths of the height. Placing by where the mark lands makes swapping the
 * glyph a one-line change. */
#define FONT_BADGE       36.0f
#define BADGE_BOX        28            /* square, and far larger than the mark */
#define BADGE_MARK_X     (BADGE_BOX / 2)
#define BADGE_MARK_Y     (BADGE_BOX / 2)
/* Where the mark goes, in the button's coordinates: level with the warn badge in
 * the opposite corner. */
#define BADGE_AT_X       14
#define BADGE_AT_Y       14

/* ---- colours ----
 *
 * Every colour our controls use is a role resolved through mod_ui(), so the theme in
 * force decides it; values live in theme/roles.c. The two below are fully transparent
 * ("draw nothing") and no theme overrides them. */
/* Transparent, like the stock quick-menu panels, which draw straight onto the app's
 * black. The borrowed band is empty: QM_MODE_OURS is past the app's four modes, so no
 * stock panel draws into it. */
#define COL_ROW_BG       0x00000000u
#define COL_OUTLINE      0x00000000u   /* Label draws an outline rect; keep it invisible */

/* ---- the wedge, as a row of steps ----
 *
 * Vertical bars rising left to right, not a solid triangle:
 *
 *   - a diagonal needs antialiasing and there is no antialiased primitive; axis-aligned
 *     bars are pixel-exact with fillRect.
 *   - a triangle's tip has no area; the shortest bar has a minimum height, so the low
 *     end of the range stays visible.
 *
 * Bar and gap match the stock KEY SHIFT step strip: 5px lit, 4px dark, and
 * #323232 for an unlit step. */
#define WEDGE_BAR_W      5     /* the marked bars keep the stock width */
#define WEDGE_BAR_GAP    4
/* Unmarked bars are a pixel narrower, so marks stand out by width as well as shade.
 * The pitch stays WEDGE_BAR_W + WEDGE_BAR_GAP; a thin bar leaves the extra pixel as
 * gap, so left edges stay on a regular grid. */
#define WEDGE_BAR_THIN   4
/* Minimum height of the leftmost bar. The paint derives a whole-pixel rise per bar, so
 * the shortest bar ends up at this height or slightly above. */
#define WEDGE_MIN_H      8
/* Every WEDGE_TICK-th bar is a scale mark, since the control shows no number. At the
 * current panel width the marks land on the ends and at the thirds. */
#define WEDGE_TICK       14
/* The warn badge is amber, not red: nothing is broken, there is just no usable
 * server. Red on this deck means an error the DJ has to act on.
 * A refused press turns the button red for the duration of the flash only. */
/* The flash is MOD_BLINK_* in ../../juce/draw.h, shared with the X-PAD button.
 *
 * Durations are in display ticks from the app's refresh timer; the tick rate is
 * logged once at startup. */
#define WARN_SETTLE_TICKS  44     /* hold before the badge appears: ~1 s  */

#define STEM_LEVEL_MAX   100.0
/* N_STEMS lives in ../stem.h -- the audio side needs it for the level array too. */

/* ---- shared state, defined once in state.c -------------------------------
 *
 * Component pointers into the app's tree and the flags that track what we did to
 * it. Only state used by more than one file is here. */


/* The STEMS button's state, not its colour: the colour is resolved at paint from the
 * theme in force, so a theme change cannot leave it on the old palette. */
enum btn_state { BTN_OFF, BTN_ON, BTN_REFUSE };

/* Our clone of the juce::Label vtable: stock except mouseDown, mouseUp and paint.
 * The two words ahead of the slots (offset-to-top, typeinfo) are cloned too, so
 * RTTI through vptr[-1] still finds Label's. */
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
extern uintptr_t stems_g_controls;       /* BYPASS + captions + wedges                */
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
 * The three controls and the single gesture they share. Outside wedge.c these
 * are read to draw the captions and written only to reset levels on a track
 * change. */
extern uintptr_t stems_g_wedge[N_STEMS];
extern int       stems_g_level[N_STEMS];   /* percent, 0..STEM_LEVEL_MAX */
extern int       stems_g_mute[N_STEMS];
extern uintptr_t stems_g_grab;             /* who owns the gesture right now */
extern uintptr_t stems_g_grab_last;        /* who owned it most recently     */
extern unsigned  stems_g_ticks;            /* display ticks; the gesture cooldown */
extern uintptr_t stems_g_icon_vptr;

/* ---- what each part offers the others ------------------------------------
 * Message thread throughout. Grouped by defining file. */

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
/* Our quick-menu button's x, from the band kit (depends on which other clients are
 * enabled). panel.c owns the band descriptor. */
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
 * Everything drawn is derived from `st` on this call; the only state kept between
 * calls is a cache of what is on screen. */
void      stems_processing_set(const struct stem_ui_state *st);
/* Forget what is on the bar. The poll runs only while the row is open, so on reopen
 * the cache may describe a job that has since advanced, finished or been replaced. */
void      stems_progress_forget(void);
/* The "waveform cannot follow the levels yet" mark, in wavewait.c. Built into the
 * controls container, not into BYPASS, whose press it would otherwise take. */
uintptr_t stems_wavewait_build(uintptr_t parent, int x, int y, int w, int h);
void      stems_wavewait_poll(void);
/* Percent of the bar -> pixels along it. The fill and the delimiters must both use
 * this so they line up. */
int       stems_prog_px(int pct);

/* build.c */
void      stems_build(uintptr_t anchor);

/* hooks.c */
void      stems_progress_poll(void);
int       stems_available(void);


#ifdef __cplusplus
}
#endif

#endif /* EP122_MODS_STEM_UI_H */
