// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * xpad/xpad.h - the contract between the parts of the X-PAD SAMPLER.
 *
 * A touch strip in the band under the waveform, after the RMX-1000's X-PAD.
 * X picks a loop length in beats, Y bends the pitch, the hot cues are the sample
 * banks, and three panel controls are borrowed for as long as the panel is open.
 *
 * THE PANEL IS THE MODE. Opening it takes the pads and the three controls;
 * closing it stops the sound and hands every one of them back. Nothing the DJ
 * has not opened is changed, which is what makes taking the DELETE and MEMORY
 * buttons acceptable at all.
 *
 * Layout, at the band's own 1280 width:
 *
 *  20                                    936   960                     1280
 *   +--------------------------------------+ | 1/4         +32%  [HOLD   ]
 *   | 1/16 | 1/8 | 1/4 | 1/2 |  1   |  2   | | VOL #####--       [OVERDUB]
 *   +--------------------------------------+ |
 *                    the pad             gutter       the readout, floating
 *
 * The pad is six equal bricks. A brick carries its own name in a notch cut out
 * of the fill, so the lettering never disappears under the value -- 43px of
 * unbroken fill down each gutter at any pitch, which is what makes the shape
 * readable from the back of a booth where the digits are not.
 *
 * Threads: everything in ui.c/strip.c/pane.c is [message]; audio.c is [audio]
 * and may not allocate, lock or log; bank.c scans and decodes on [worker].
 */
#ifndef EP122_MOD_XPAD_H
#define EP122_MOD_XPAD_H

#include "juce/juce.h"
#include "juce/draw.h"
#include "kit/band.h"
#include "lamp/lamp.h"

#ifdef __cplusplus
extern "C" {
#endif


/* ---- the pad, in pixels --------------------------------------------------
 *
 * Horizontal is the design's own, at 1:1 against the 1280 the band is wide.
 * Vertical is DERIVED, because the band the app frees is 84px and the design
 * was drawn against 103: the ends of the pitch axis are what the strip's height
 * allows, not a number that can be written down once. XP_TRAVEL below is
 * computed from the strip, so a firmware with a different band still centres
 * unity and still spends every pixel it has. */
/* A LEFT MARGIN, because the deck's bezel is retracted over the glass and the
 * far corners of the screen are genuinely hard to reach. The pad is what a
 * finger lands on, so it comes in off the edge; the readout does not, and runs
 * to the right edge where nothing has to touch it. */
#define XP_MARGIN_L     20
#define XP_PAD_W        916
#define XP_GUTTER       24
#define XP_PAD_X        XP_MARGIN_L
#define XP_PANE_X       (XP_PAD_X + XP_PAD_W + XP_GUTTER)
#define XP_BRICKS       6
/* The name's notch, and therefore the fill's two gutters: 153 - 66 = 87, so 43
 * of solid fill down each side of the lettering at any value. */
#define XP_NOTCH_W      66
/* How tall the gap the fill leaves for the lettering is. Only as tall as the
 * name needs: above and below it the fill spans the whole brick, so a large
 * bend is a bolder mark than a small one rather than just a taller one. */
#define XP_NOTCH_H      40
#define XP_EDGE         2       /* the pad's own outline, and the brick dividers */
#define XP_EDGE_LIT     3       /* the active brick wears a heavier one          */

/* The unity cursor: a hairline in each gutter, thickening when the snap has hold
 * of the finger. That thickening is the ONLY signal that pitch is being held at
 * zero, which is what lets the snap cost no vertical travel at all. */
#define XP_CURSOR_H     3
#define XP_CURSOR_SNAP_H 6

/* The pitch axis runs to the brick's own lip and no further: at the ends of
 * travel the fill meets the outline, so full IS full. A clearance inside that
 * was tried and read as the control failing to reach its own end. */

/* The pitch axis is LINEAR IN SEMITONES, so an interval is the same distance
 * wherever it is taken from -- the way a keyboard is spaced.
 *
 * +-12, MEASURED. A chirp through the hardware comes back at ratio 1.9987
 * (+11.99 st) at the top of travel and 0.4926 (-12.26 st) at the bottom, so
 * the range is an octave either side of unity: endpoints of 2.0 and 0.5,
 * reached at the ends of travel and nowhere before them. */
#define XP_SEMITONES    12
/* What the readout says at the ends of travel. A SIGNED PERCENTAGE OF THE
 * RANGE, not of the play rate: a rate percentage cannot be symmetric, since an
 * octave up is +100% and an octave down is -50%, and -100% of a rate is a
 * stopped sample rather than an octave. This is symmetric because the axis is. */
#define XP_PCT_FULL     50
/* Back to exactly zero, in semitones rather than pixels now that the bend is
 * accumulated rather than measured from a fixed point. Small enough that it
 * costs no usable range and large enough to be reachable with a fingertip. */
#define XP_SNAP_ST      0.5f

/* ---- the gesture ---------------------------------------------------------
 *
 * PITCH IS ACCUMULATED, and the angle is read CONTINUOUSLY.
 *
 * Every move is routed by its own direction: travel that is more vertical than
 * horizontal adds to the bend, travel that is more horizontal picks the brick
 * under the finger. Neither is latched, so one touch can bend up, slide left to
 * another loop length carrying that bend with it, and bend back down again --
 * which is the gesture this control is for.
 *
 * That only works because the bend is a RUNNING TOTAL rather than a function of
 * where the finger is. Measuring it from the press would reset it the moment the
 * finger moved sideways and back; adding each vertical step to a total means
 * sideways travel simply contributes nothing and the bend survives it.
 *
 * A 45-degree split needs no tuning and leaves no direction meaning neither.
 * The horizontal branch additionally wants XP_STEP_PX before it will act, so a
 * finger resting on a brick boundary while bending cannot flicker between two;
 * the vertical branch needs no such floor, because a step of nothing adds
 * nothing.
 *
 * The DISPLAY stays absolute -- the cursor is the zero line and the fill grows
 * out of it -- because what the readout has to say is what the pitch IS, not
 * how far the finger has moved. */
#define XP_STEP_PX      2
/* And a ceiling on one step, because a delta this large is not a finger: it is a
 * second contact or a lift reported somewhere else, and it would otherwise slam
 * the bend to an end stop in a single event. Comfortably above the fastest real
 * swipe between two touch reports, comfortably below the pad's own width. */
#define XP_JUMP_PX      160

/* ---- the readout --------------------------------------------------------- */

#define XP_PANE_PAD_X   10
#define XP_PANE_PAD_Y   8
/* ---- the two state plates ------------------------------------------------
 *
 * A LAMP AND ITS LEGEND, which is how the RMX-1000 puts these two on a panel: an
 * illuminated button with its name printed under it. Here the name goes beside
 * the light instead, because a 132x31 row has width and no height to spare.
 *
 * The lamp carries the state and the word never changes weight -- a plate that
 * inverted, or grew a second colour, made two things that sit together read as
 * two different design languages. Both light the same way: a blinking one beside
 * a steady one reads as a control mid-change, not as a recorder. */
#define XP_FLAG_W       132     /* OVERDUB / HOLD, stacked                      */
#define XP_FLAG_GAP     8
/* One pixel, which is what juce::Label's own drawRect gives every outlined plate
 * on the rack. */
#define XP_FLAG_EDGE    1
#define XP_LAMP_PAD     4       /* the lamp's inset within the row              */
#define XP_LAMP_GAP     9       /* ...and the air between it and the word       */
#define XP_VOL_H        16
/* The dormant loop value, which is a dash rather than a number: the pad is not
 * set to anything, and "no loop" and "1/32" are different states. Sized and
 * placed to sit where the digits' own middle would be. */
#define XP_DASH_W       26
#define XP_DASH_H       3
#define XP_DASH_DROP    13

/* The quick-menu button's lettering. The same size the deck bakes into BEAT LOOP
 * and KEY SHIFT, and the same STEMS uses, because it sits in their row. */
#define XP_FONT_BTN     24.0f
#define XP_FONT_BRICK   34.0f   /* the six loop names                           */
#define XP_FONT_LIT     32.0f   /* the active one, in its notch                 */
/* The readout is READ, not aimed at, so it does not need the pad's type size --
 * and at the pad's size it competed with the strip it is describing. */
#define XP_FONT_VALUE   22.0f   /* loop and pitch, in the readout               */
/* GATE CUE's own size, since these two are the same kind of plate on the same
 * screen and sit a rack apart from it. */
#define XP_FONT_FLAG    15.0f   /* VOL, OVERDUB, HOLD                           */

/* The active brick's name blinks, off the display tick (a measured 47 Hz). Long
 * enough to read as a pulse rather than a flicker, and asymmetric so the lit
 * phase dominates: the name is information first and an indicator second. */
#define XP_BLINK_PERIOD 52
#define XP_BLINK_ON     32
/* The dim half of the blink, as a Q8 scale. Not off: the name is what says which
 * loop is armed, and a name that vanishes for a third of every cycle is worse to
 * read than one that merely dips. */
#define XP_BLINK_DIM_Q8 140

/* How often the mix's own window is printed, in display ticks -- about a second
 * at the measured 47 Hz. */
#define XPAD_STAT_TICKS 47

/* ---- the six loop lengths ------------------------------------------------
 *
 * The pad reads left to right shortest first, which is the RMX's own order and
 * puts the fast rolls under the hand that reaches for them.
 *
 * 1/16 to 2 BEATS. A 1/32 roll at any club tempo is a tone rather than a
 * rhythm -- 78 retriggers a second at 148 BPM, which is a buzz whose pitch the
 * pad does not control -- and the length that was missing at the other end is
 * the two-beat one, which is where a sample gets to be heard as itself.
 */
#define XP_DIV_NONE     (-1)
extern const char *const xpad_div_name[XP_BRICKS];
/* THE ROLL'S LENGTH IN BEATS, not a divisor: the pad runs past a whole beat at
 * the right-hand end and 2 has no integer divisor. */
extern const float       xpad_div_beats[XP_BRICKS];

/* ---- the banks -----------------------------------------------------------
 *
 * One per hot cue. Files in mods/loops/ on the stick, sorted by name, the first
 * eight taken -- NO CONFIG FILE, unlike GROOVE CIRCUIT, because a one-shot needs
 * nothing said about it: the circuit's loops have to be told their BPM to be
 * rate-matched against the track, and a sample is played at its own length and
 * repeated on the track's grid instead. Sorting by name is what makes the order
 * the DJ's: number the files and the pads follow. */
#define XP_BANKS        8
#define XP_LOOP_DIR     "mods/loops"
/* What one bank may hold. A sample is a hit or a bar, not a track; at the pool's
 * 96 kHz stereo s16 this is 3.8 MB a bank, 31 MB for all eight. Longer files
 * load truncated rather than refused, because a truncated sample still plays. */
#define XP_MAX_SECONDS  10

/* bank.c -- [worker] scans and decodes, and is the only writer. */
void        xpad_bank_poll(void);
int         xpad_bank_ready(int bank);          /* [any] a file is behind this pad */
int         xpad_bank_count(void);              /* [any] */
const char *xpad_bank_name(int bank);           /* [message] the file's own name */

struct xpad_bank_view {
    const int16_t *pcm;         /* interleaved stereo at the pool rate */
    int64_t        frames;
};
int  xpad_bank_acquire(int bank, struct xpad_bank_view *out);   /* [audio] */
void xpad_bank_release(void);

/* ---- the voices ----------------------------------------------------------
 *
 * A PAD IS A SELECTION AND A ONE-SHOT, never a held key. Pressing it makes that
 * bank the X-PAD's, and plays it once; the release means nothing and there is no
 * state to leave behind. What repeats a sample is the X-PAD -- a finger on the
 * strip rolls whatever is selected -- which is the RMX's own division of labour
 * and is why the pads need no hold at all.
 *
 * THE PRESS IS NOT QUANTIZED. A pad is a drum: what a finger asks for is the
 * sound now, and a hit held back to the next boundary is a hit the DJ did not
 * play. The grid belongs to the ROLL, which is a machine and is meant to be on
 * it -- and even there only from the second hit: a zone taking the finger sounds
 * on the spot, and only the repeats are gridded. The press still crosses to the
 * mix through a bit rather than a voice, because [deck] does not own one, but
 * the mix takes it on the very next block. */
/* THE DECK'S OWN QUANTIZE IS THE QUANTIZE, wherever the X-PAD snaps at all.
 * There is no second setting for it and there should not be: a DJ who has turned
 * QUANTIZE off has said what they want from every timed gesture on the deck.
 *
 * What still asks: the strip's automation, for its sampling rate, and the stem
 * row's mute. The pads and the roll's first hit do not.
 *
 * Answers the DIVISOR of a beat -- 1, 2, 4 or 8 -- or 0 when quantize is off. */
int xpad_quantize_div(void);

/* What the strip's automation samples at when quantize is OFF. It still needs a
 * rate: an automation recorder with no grid samples every block and fills thirty
 * events in a fifth of a second. A quarter beat is what a swept surface is worth
 * -- where a sweep crosses a brick is wherever the hand happened to be, not a
 * moment the DJ chose. */
#define XP_QUANTIZE_PAD 4

void xpad_fire(int bank);           /* [deck] the pad went down */
/* [audio] The bar's and the roll's trigger. Deliberately NOT xpad_fire -- see
 * the note at its definition for the two things the deck's entry point does that
 * neither of them must. */
void xpad_seq_fire(int bank);
/* ...and the same thing placed ON A BEAT inside the block being mixed, so a
 * boundary is played at the frame it falls on and not at the block's edge. */
void xpad_seq_fire_at(int bank, double beat);
void xpad_silence(void);            /* [message] the panel shut: everything off */
int  xpad_voice_lit(int bank);      /* [any] sounding, for the lamp */

/* Which bank the pads last picked, or -1. [deck] writes, [audio] and the lamps
 * read. */
extern int xpad_g_sel;

/* [any] The lamp for one pad: 0 when the X-PAD does not own it, else *out is
 * filled -- LAMP_DIM for a bank that is loaded and quiet, LAMP_LIT for the one
 * selected or sounding, and a dim white for a pad with no sample behind it.
 *
 * Asked by lamp/lamp.c ahead of the groove circuit's, because while the panel
 * is open the pads are sample slots and nothing else can have them. The law
 * that decides what a triple and a level mean is lamp.h; read it before
 * choosing a colour here. */
int xpad_pad_lamp(int pad, struct lamp *out);

/* What reached the output over one window. The peak is of OUR OWN contribution
 * rather than of the block, so a loud track cannot stand in for a sample that
 * never played, and `fired` is counted on the DECK where `fires` is counted in
 * the MIX -- which is what separates "the pad did nothing" from "the mix never
 * ran". */
struct xpad_mix_stat {
    unsigned blocks;    /* blocks our sum was added to */
    unsigned fires;     /* triggers the mix acted on   */
    unsigned fired;     /* triggers the deck asked for */
    float    peak;
    /* AND WHAT THE CLOCK DID, because "the roll plays one hit and stops" and
     * "the pad is silent" are the same line without it: a roll that cannot
     * cross a boundary has no clock, and which of the two routes it is on says
     * whether that is the grid or the tempo. */
    int      clock;     /* 0 none, 1 the flat route, 2 the beat array */
    double   beat;      /* where the head that mixes is */
    double   span;      /* what one block covered, in beats */
};
void xpad_mix_stat(struct xpad_mix_stat *out);  /* [message], clears the window */

/* The bend as a playback ratio, and the smoothing that reaches it.
 *
 * The ratio is exp2f(semitones / 12), evaluated once a block.
 *
 * THE TIME CONSTANT IS THE THING THAT PORTS, not a per-block pole: a smoothing
 * coefficient only means anything next to the block size and the rate it runs
 * at. Taking a 48-sample-block coefficient across to our rate, rather than the
 * 49.5 ms it works out to, made the first attempt's pitch follow the pad about
 * 2.5x too fast. */
#define XP_PITCH_TAU_MS 49.5f

/* THE CROSSFADE WINDOW, per direction, in milliseconds.
 *
 * The pitch engine is a pair of read heads wandering back through the sample and
 * wrapping a window at a time -- see xp_heads in audio.c. W is how far they
 * wander, and it is the one number the flash image does not give up: the window
 * constant in it belongs to the tempo-sweep driver, not to the X-PAD path.
 *
 * MEASURED, three ways, across a seven-position sweep rather than off the two
 * endpoints:
 *
 *   THE COMB. A steady sine comes back with an evenly spaced comb, and
 *   W = 2|1 - r| / f_comb lands on 31.34, 31.33, 31.37 ms at the three positions
 *   below unity -- a spread of 0.13% -- and 27.13, 26.98, 26.05 above it.
 *
 *   THE COPIES. A 4 ms burst comes back at full pitch as three copies W/(2r)
 *   apart. Measured 6.67 ms at r = 2, so W = 26.7 ms.
 *
 *   THE REPEAT. Bursts 600 ms apart come back with identical structure, which
 *   only happens when 600 ms is a whole number of half phases. 26.667 ms gives
 *   22.5 of them, and half a phase is exactly the head swap.
 *
 * THE FACTOR OF TWO IS THE TRAP. Two staggered heads wrap alternately, so the
 * comb spacing is 2|1 - r| / W and not |1 - r| / W: reading it the other way
 * halves the window, and then the copies come out 3.3 ms apart instead of 6.7.
 *
 * The asymmetry between the two sides is real -- it holds across the sweep on
 * both -- and has no mechanism in the recovered engine, so it is carried as two
 * numbers rather than derived from one. */
#define XP_XFADE_UP_MS      26.67f
#define XP_XFADE_DOWN_MS    31.35f

/* ---- OVERDUB -------------------------------------------------------------
 *
 * A FOUR-BEAT EVENT SEQUENCER, not an audio recorder. What is stored is which
 * pad fired and at what phase of the bar; replay re-triggers the voice. That is
 * forced by the RMX's own behaviour -- its mute and delete act per pad, which is
 * impossible against a mixed buffer -- and it is also what makes the loop free
 * of feedback, of gain stacking and of a capture buffer.
 *
 * The phase is measured against the TRACK's grid, so a hit stored at beat 2.7 of
 * the bar fires at every beat congruent to 2.7 for as long as OVERDUB is on:
 * there is no window to start, nothing to keep in step, and nothing to drift.
 * Turning OVERDUB off frees the lot. */
#define XP_SEQ_BEATS    4
/* What the bar holds, hits and gesture TOGETHER. When it is full the OLDEST goes
 * to make room: a recorder that stops recording is a recorder that quietly
 * ignores the DJ, and what they are playing now matters more than what they
 * played four beats ago.
 *
 * Thirty, which is deliberately small. A four-beat loop that a listener can hold
 * in their head is a handful of events; a hundred is an automation dump, and it
 * comes out as a smear rather than as a part. The consequence to know about is
 * that hits and gesture share the budget -- a long sweep will push earlier hits
 * out of the bar, because the rule is oldest-first and a sweep is younger. */
#define XP_SEQ_MAX      30
/* The smallest bend worth an event of its own. The gesture is sampled on the
 * quantize grid and only when it has actually moved, so a finger held still
 * costs the bar nothing and a sweep costs it one event per audible step. */
#define XP_AUTO_ST      0.75f

/* seq.c */
void xpad_seq_record(int bank, double beat);    /* [audio] a voice was fired */
/* [audio] The X-PAD's own gesture, as automation. `div` of XP_DIV_NONE is the
 * finger coming off, which has to be recorded too or the bar would hold the last
 * value it saw for ever. */
void xpad_seq_record_pad(int div, float semis, double beat);
/* [audio] What the bar is currently playing back on the strip, or 0 when it is
 * playing none. Live touch outranks it; see xp_gesture_read. */
int  xpad_seq_auto(int *div, float *semis);
void xpad_seq_clear(void);                      /* [any] */
int  xpad_seq_count(void);                      /* [any] events held */
void xpad_overdub_set(int on);                  /* [deck] DELETE, or the panel */
/* [audio] Fire whatever the bar says between two beat positions. `b0` and `b1`
 * are the track's fractional beat index at the ends of one block, ascending. */
void xpad_seq_play(double b0, double b1);

/* ---- state the parts share ----------------------------------------------- */

/* The gesture, as the strip leaves it and the readout draws it. `div` is a brick
 * index or XP_DIV_NONE when nothing is touched; `semis` is the bend, already
 * snapped; `held` says a finger is down, which is what the cursor thickens for. */
struct xpad_touch {
    int   div;
    float semis;        /* the running bend, -XP_SEMITONES .. +XP_SEMITONES */
    int   snapped;
    int   held;
};

extern struct xpad_touch xpad_g_touch;
extern int xpad_g_hold;         /* HOLD: the sound latches when the finger lifts */

/* IS THE GESTURE ACTING? A finger on the strip, or HOLD keeping the last one.
 *
 * ONE PREDICATE, asked by the mix and by both paints, because the alternative is
 * what this replaced: the strip cleared its own values on a lift and the audio
 * tested a different condition, so switching HOLD off left the brick lit and the
 * pitch on screen with nothing sounding. The VALUES are left where the finger
 * left them either way -- there is nothing to clear, and nothing to clear them
 * from a thread that does not own them. */
static inline int xpad_gesture_live(void)
{
    return (xpad_g_touch.held || xpad_g_hold) &&
           xpad_g_touch.div != XP_DIV_NONE;
}
extern int xpad_g_overdub;      /* OVERDUB: the 4-beat recorder is armed         */
extern int xpad_g_vol;          /* 0..100, from VINYL SPEED ADJUST               */

/* Built, and the panel's open state. */
extern uintptr_t xpad_g_btn;    /* the quick-menu button        */
extern uintptr_t xpad_g_strip;  /* the whole band: pad + pane   */
extern uintptr_t xpad_g_pad;    /* the touch surface            */
extern uintptr_t xpad_g_pane;   /* the readout                  */
extern int       xpad_g_open;
/* ENABLE X-PAD, the master gate. OFF by default. */
extern int       xpad_g_on;
extern unsigned  xpad_g_ticks;   /* display ticks, ~47 Hz */

/* ui.c */
int  xpad_open(void);
void xpad_toggle(void);
void xpad_sync(void);

/* [any] The readout is stale. Deliberately a FLAG rather than a repaint: the
 * three borrowed controls run on the deck's task thread, and juce owns its
 * components on the message thread. The display tick picks it up. */
void xpad_repaint(void);

/* strip.c */
uintptr_t xpad_build_pad(uintptr_t parent, int x, int y, int w, int h);
/* Where unity sits and how far the finger may travel, both derived from the
 * pad's own height so nothing here assumes the band's size. */
int  xpad_unity_y(int h);
int  xpad_travel(int h);

/* pane.c */
uintptr_t xpad_build_pane(uintptr_t parent, int x, int y, int w, int h);

/* The bend, as the readout says it: -XP_PCT_FULL .. +XP_PCT_FULL. */
int  xpad_pitch_pct(float semis);


#ifdef __cplusplus
}
#endif

#endif /* EP122_MOD_XPAD_H */
