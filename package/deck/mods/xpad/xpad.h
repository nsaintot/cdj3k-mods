// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * xpad/xpad.h - the contract between the parts of the X-PAD SAMPLER.
 *
 * A touch strip in the band under the waveform, modelled on the RMX-1000's
 * X-PAD. X picks a loop length in beats, Y bends the pitch, the hot cues are the
 * sample banks, and three panel controls are borrowed while the panel is open.
 *
 * The panel is the mode. Opening it takes the pads and the three controls;
 * closing it stops the sound and hands all of them back. Nothing changes unless
 * the DJ opened the panel, which is why taking the DELETE and MEMORY buttons is
 * acceptable.
 *
 * Layout, at the band's own 1280 width:
 *
 *  20                                    936   960                     1280
 *   +--------------------------------------+ | 1/4         +32%  [OVERDUB]
 *   | 1/16 | 1/8 | 1/4 | 1/2 |  1   |  2   | | VOL #####--       [HOLD   ]
 *   +--------------------------------------+ |
 *                    the pad             gutter       the readout, floating
 *
 * The pad is six equal bricks. Each brick's name sits in a notch cut out of the
 * fill, so the value never covers it. That leaves 43px of solid fill down each
 * side at any pitch, so the shape stays readable from a distance.
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
 * Horizontal sizes are the design's own, 1:1 against the 1280px band. Vertical
 * sizes are derived: the band the app frees is 84px and the design was drawn
 * against 103. xpad_travel() computes the pitch travel from the strip's height,
 * so a firmware with a different band still centres unity and uses every pixel. */
/* Left margin: the bezel overlaps the glass and the far corners are hard to
 * reach. Only the pad is inset; the readout is not touched and runs to the right
 * edge. */
#define XP_MARGIN_L     20
#define XP_PAD_W        916
#define XP_GUTTER       24
#define XP_PAD_X        XP_MARGIN_L
#define XP_PANE_X       (XP_PAD_X + XP_PAD_W + XP_GUTTER)
#define XP_BRICKS       6
/* The name's notch, which sets the fill's two gutters: 153 - 66 = 87, so 43px of
 * solid fill each side of the lettering at any value. */
#define XP_NOTCH_W      66
/* Height of the notch, only as tall as the name needs. Above and below it the
 * fill spans the whole brick, so a larger bend draws a bolder mark. */
#define XP_NOTCH_H      40
#define XP_EDGE         2       /* the pad's own outline, and the brick dividers */
#define XP_EDGE_LIT     3       /* the active brick's outline                    */

/* The unity cursor: a hairline in each gutter that thickens while the snap holds
 * the pitch at zero. The thickening is the only indication of the snap, so the
 * snap costs no vertical travel. */
#define XP_CURSOR_H     3
#define XP_CURSOR_SNAP_H 6

/* The pitch axis runs to the brick's own lip: at the ends of travel the fill
 * meets the outline. Do not add an inner clearance; it reads as the control
 * failing to reach its end. */

/* The pitch axis is linear in semitones, so an interval is the same distance
 * anywhere on it, as on a keyboard.
 *
 * +-12: an octave either side of unity, with ratios 2.0 and 0.5 reached only
 * at the ends of travel. */
#define XP_SEMITONES    12
/* What the readout shows at the ends of travel: a signed percentage of the
 * range, not of the play rate. A rate percentage is asymmetric (an octave up is
 * +100%, an octave down is -50%); this one is symmetric like the axis. */
#define XP_PCT_FULL     50
/* Snap-to-zero window, in semitones since the bend is accumulated. Small enough
 * to cost no usable range, large enough to hit with a fingertip. */
#define XP_SNAP_ST      0.5f

/* ---- the gesture ---------------------------------------------------------
 *
 * Pitch is accumulated, and the direction is read on every move.
 *
 * A move that is more vertical than horizontal adds to the bend; one that is
 * more horizontal picks the brick under the finger. Neither is latched, so one
 * touch can bend up, slide to another loop length keeping the bend, and bend
 * back down.
 *
 * This requires the bend to be a running total of vertical steps, not a
 * function of the finger's position: sideways travel adds nothing, so the bend
 * survives it.
 *
 * The split is at 45 degrees. The horizontal branch also needs XP_STEP_PX of
 * travel before it acts, so a finger on a brick boundary cannot flicker between
 * two bricks while bending; the vertical branch needs no threshold.
 *
 * The display stays absolute: the cursor is the zero line and the fill grows
 * from it, showing the current pitch. */
#define XP_STEP_PX      2
/* Maximum single step. A larger delta is a second contact or a lift reported
 * elsewhere, and would slam the bend to an end stop in one event. Well above the
 * fastest real swipe between two touch reports, well below the pad's width. */
#define XP_JUMP_PX      160

/* ---- the readout --------------------------------------------------------- */

#define XP_PANE_PAD_X   10
#define XP_PANE_PAD_Y   8
/* ---- the two state plates ------------------------------------------------
 *
 * A lamp and its legend, as on the RMX-1000's illuminated buttons. The name sits
 * beside the light instead of under it, because a 132x31 row has width but no
 * height to spare.
 *
 * The lamp carries the state; the word never changes weight. Inverting the
 * plate or adding a second colour would make the two plates inconsistent. Both
 * light the same way, since a blinking one beside a steady one reads as a
 * control mid-change. */
#define XP_FLAG_W       132     /* OVERDUB / HOLD, stacked                      */
#define XP_FLAG_GAP     8
/* One pixel, matching juce::Label's drawRect on every outlined plate in the
 * rack. */
#define XP_FLAG_EDGE    1
#define XP_LAMP_PAD     4       /* the lamp's inset within the row              */
#define XP_LAMP_GAP     9       /* ...and the gap between it and the word       */
#define XP_VOL_H        16
/* The dormant loop value is drawn as a dash, since "no loop" and "1/32" are
 * different states. Sized and placed at the digits' vertical middle. */
#define XP_DASH_W       26
#define XP_DASH_H       3
#define XP_DASH_DROP    13

/* The quick-menu button's lettering: the size the deck bakes into BEAT LOOP and
 * KEY SHIFT, and the size STEMS uses, since it sits in their row. */
#define XP_FONT_BTN     24.0f
#define XP_FONT_BRICK   34.0f   /* the six loop names                           */
#define XP_FONT_LIT     32.0f   /* the active one, in its notch                 */
/* Smaller than the pad's type: the readout is only read, and at the pad's size
 * it competed with the strip. */
#define XP_FONT_VALUE   22.0f   /* loop and pitch, in the readout               */
/* GATE CUE's size, since these are the same kind of plate on the same screen. */
#define XP_FONT_FLAG    15.0f   /* VOL, OVERDUB, HOLD                           */

/* The active brick's name blinks off the display tick (about 47 Hz). The
 * period is long enough to read as a pulse, and the lit phase is longer than
 * the dim one. */
#define XP_BLINK_PERIOD 52
#define XP_BLINK_ON     32
/* The dim half of the blink, as a Q8 scale. Not zero, because the name says
 * which loop is armed and should stay readable. */
#define XP_BLINK_DIM_Q8 140

/* How often the mix's stats window is printed, in display ticks: about a second
 * at the 47 Hz display tick. */
#define XPAD_STAT_TICKS 47

/* ---- the six loop lengths ------------------------------------------------
 *
 * Left to right, shortest first, as on the RMX.
 *
 * 1/16 to 2 beats. A 1/32 roll at club tempo is a tone (78 retriggers a second
 * at 148 BPM) whose pitch the pad does not control, so there is no 1/32.
 */
#define XP_DIV_NONE     (-1)
extern const char *const xpad_div_name[XP_BRICKS];
/* The roll's length in beats, not a divisor: the longest is 2 beats, which has
 * no integer divisor. */
extern const float       xpad_div_beats[XP_BRICKS];

/* ---- the banks -----------------------------------------------------------
 *
 * One per hot cue. Files in mods/loops/ on the stick, sorted by name, first
 * eight taken. There is no config file, unlike GROOVE CIRCUIT: a one-shot plays
 * at its own length and is repeated on the track's grid, so it needs no BPM.
 * Numbering the files sets the pad order. */
#define XP_BANKS        8
#define XP_LOOP_DIR     "mods/loops"
/* Maximum length of one bank. At the pool's 96 kHz stereo s16 this is 3.8 MB a
 * bank, 31 MB for all eight. Longer files are truncated, not refused. */
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
 * A pad press selects that bank for the X-PAD and plays it once; the release
 * does nothing. Repeats come from the strip, which rolls the selected bank, as
 * on the RMX.
 *
 * The press is not quantized: it sounds immediately. Only the roll is on the
 * grid, and only from its second hit; the first hit sounds when the finger
 * lands. The press reaches the mix through a bit, since [deck] does not own a
 * voice, and the mix takes it on the next block. */
/* The deck's own QUANTIZE setting is the only one; the X-PAD has no separate
 * setting.
 *
 * Callers: the strip's automation (for its sampling rate) and the stem row's
 * mute. The pads and the roll's first hit do not ask.
 *
 * Returns the divisor of a beat (1, 2, 4 or 8), or 0 when quantize is off. */
int xpad_quantize_div(void);

/* The strip automation's sampling rate when quantize is off. Without a grid the
 * recorder would sample every block and fill thirty events in a fifth of a
 * second. A quarter beat is enough for a sweep. */
#define XP_QUANTIZE_PAD 4

void xpad_fire(int bank);           /* [deck] the pad went down */
/* [audio] The bar's and the roll's trigger. Not xpad_fire; see the note at the
 * definition. */
void xpad_seq_fire(int bank);
/* ...and the same, placed on a beat inside the block being mixed, so a boundary
 * plays at its own frame rather than at the block's edge. */
void xpad_seq_fire_at(int bank, double beat);
void xpad_silence(void);            /* [message] the panel shut: everything off */
int  xpad_voice_lit(int bank);      /* [any] sounding, for the lamp */

/* Which bank the pads last picked, or -1. [deck] writes, [audio] and the lamps
 * read. */
extern int xpad_g_sel;

/* [any] The lamp for one pad: 0 when the X-PAD does not own it, else *out is
 * filled -- LAMP_DIM for a loaded, quiet bank, LAMP_LIT for the one selected or
 * sounding, and a dim white for a pad with no sample.
 *
 * lamp/lamp.c asks this before the groove circuit, because while the panel is
 * open the pads belong to the X-PAD. lamp.h defines what a triple and a level
 * mean; read it before choosing a colour here. */
int xpad_pad_lamp(int pad, struct lamp *out);

/* What reached the output over one window. The peak is of our own contribution,
 * not the block, so a loud track cannot hide a sample that never played.
 * `fired` is counted on the deck and `fires` in the mix, which separates "the
 * pad did nothing" from "the mix never ran". */
struct xpad_mix_stat {
    unsigned blocks;    /* blocks our sum was added to */
    unsigned fires;     /* triggers the mix acted on   */
    unsigned fired;     /* triggers the deck asked for */
    float    peak;
    /* The clock state, to tell "the roll plays one hit and stops" (no clock to
     * cross a boundary) from "the pad is silent". The route says whether the
     * clock comes from the grid or the tempo. */
    int      clock;     /* 0 none, 1 the flat route, 2 the beat array */
    double   beat;      /* where the head that mixes is */
    double   span;      /* what one block covered, in beats */
};
void xpad_mix_stat(struct xpad_mix_stat *out);  /* [message], clears the window */

/* The bend as a playback ratio, and the smoothing toward it.
 *
 * The ratio is exp2f(semitones / 12), evaluated once a block.
 *
 * The constant is a time, not a per-block pole: a smoothing coefficient depends
 * on block size and rate. A coefficient taken from 48-sample blocks makes the
 * pitch follow the pad about 2.5x too fast here. */
#define XP_PITCH_TAU_MS 49.5f

/* The crossfade window W, per direction, in milliseconds.
 *
 * The pitch engine is a pair of read heads moving back through the sample and
 * wrapping one window at a time (see xp_heads in audio.c). W is how far they
 * move. It is not the tempo-sweep driver's window constant.
 *
 * Three properties pin W:
 *
 *   The comb. A steady sine comes back with an evenly spaced comb, and
 *   W = 2|1 - r| / f_comb gives about 31.35 ms below unity and 26-27 ms
 *   above it.
 *
 *   The copies. A short burst comes back at full pitch as three copies W/(2r)
 *   apart: 6.67 ms at r = 2, so W = 26.7 ms.
 *
 *   The repeat. Bursts 600 ms apart come back with identical structure, which
 *   requires 600 ms to be a whole number of half phases. 26.667 ms gives 22.5
 *   of them, and half a phase is exactly the head swap.
 *
 * Note the factor of two: two staggered heads wrap alternately, so the comb
 * spacing is 2|1 - r| / W, not |1 - r| / W. Using the latter halves the window
 * and puts the copies 3.3 ms apart instead of 6.7.
 *
 * The two sides differ with no known mechanism, so they are two constants. */
#define XP_XFADE_UP_MS      26.67f
#define XP_XFADE_DOWN_MS    31.35f

/* ---- OVERDUB -------------------------------------------------------------
 *
 * A four-beat event sequencer, not an audio recorder. It stores which pad fired
 * and at what phase of the bar; replay re-triggers the voice. The RMX's per-pad
 * mute and delete require this, and it also avoids feedback, gain stacking and
 * a capture buffer.
 *
 * The phase is measured against the track's grid, so a hit stored at beat 2.7
 * fires at every beat congruent to 2.7 while OVERDUB is on, with no window to
 * start and nothing to drift. Turning OVERDUB off frees everything. */
#define XP_SEQ_BEATS    4
/* Capacity of the bar, hits and gesture together. When full, the oldest event
 * is dropped to make room.
 *
 * Kept small on purpose: a usable four-beat loop is a handful of events, and a
 * hundred sounds like a smear. Hits and gesture share the budget, so a long
 * sweep pushes earlier hits out. */
#define XP_SEQ_MAX      30
/* The smallest bend recorded as its own event. The gesture is sampled on the
 * quantize grid and only when it has moved, so a still finger records nothing
 * and a sweep records one event per audible step. */
#define XP_AUTO_ST      0.75f

/* seq.c */
void xpad_seq_record(int bank, double beat);    /* [audio] a voice was fired */
/* [audio] The X-PAD's own gesture, as automation. A `div` of XP_DIV_NONE is the
 * finger lifting, which must be recorded or the bar would hold the last value
 * forever. */
void xpad_seq_record_pad(int div, float semis, double beat);
/* [audio] What the bar is currently playing back on the strip, or 0 for
 * nothing. Live touch takes priority; see xp_gesture_read. */
int  xpad_seq_auto(int *div, float *semis);
void xpad_seq_clear(void);                      /* [any] */
int  xpad_seq_count(void);                      /* [any] events held */
void xpad_overdub_set(int on);                  /* [deck] DELETE, or the panel */
/* [audio] Fire whatever the bar holds between two beat positions. `b0` and `b1`
 * are the track's fractional beat index at the ends of one block, ascending. */
void xpad_seq_play(double b0, double b1);

/* ---- state the parts share ----------------------------------------------- */

/* The gesture, as the strip writes it and the readout draws it. `div` is a brick
 * index or XP_DIV_NONE when nothing is touched; `semis` is the bend, already
 * snapped; `held` says a finger is down, which thickens the cursor. */
struct xpad_touch {
    int   div;
    float semis;        /* the running bend, -XP_SEMITONES .. +XP_SEMITONES */
    int   snapped;
    int   held;
};

extern struct xpad_touch xpad_g_touch;
extern int xpad_g_hold;         /* HOLD: the sound latches when the finger lifts */

/* Is the gesture active? A finger on the strip, or HOLD keeping the last one.
 *
 * The mix and both paints must use this one predicate so they agree on what is
 * sounding. The values themselves are cleared only on the message thread (on a
 * lift, or by the display tick after HOLD goes off). */
static inline int xpad_gesture_live(void)
{
    return (xpad_g_touch.held || xpad_g_hold) &&
           xpad_g_touch.div != XP_DIV_NONE;
}
extern int xpad_g_overdub;      /* OVERDUB: the 4-beat recorder is armed         */
extern int xpad_g_vol;          /* 0..100, from VINYL SPEED ADJUST               */

/* Built components, and the panel's open state. */
extern uintptr_t xpad_g_btn;    /* the quick-menu button        */
extern uintptr_t xpad_g_strip;  /* the whole band: pad + pane   */
extern uintptr_t xpad_g_pad;    /* the touch surface            */
extern uintptr_t xpad_g_pane;   /* the readout                  */
extern int       xpad_g_open;
/* ENABLE X-PAD, the master gate. Off by default. */
extern int       xpad_g_on;
extern unsigned  xpad_g_ticks;   /* display ticks, ~47 Hz */

/* ui.c */
int  xpad_open(void);
void xpad_toggle(void);
void xpad_sync(void);

/* [any] Marks the readout stale; the display tick repaints it. A flag rather
 * than a repaint, because the three borrowed controls run on the deck's task
 * thread and juce owns its components on the message thread. */
void xpad_repaint(void);

/* strip.c */
uintptr_t xpad_build_pad(uintptr_t parent, int x, int y, int w, int h);
/* Where unity sits and how far the finger may travel, both derived from the
 * pad's height so nothing assumes the band's size. */
int  xpad_unity_y(int h);
int  xpad_travel(int h);

/* pane.c */
uintptr_t xpad_build_pane(uintptr_t parent, int x, int y, int w, int h);

/* The bend, as the readout shows it: -XP_PCT_FULL .. +XP_PCT_FULL. */
int  xpad_pitch_pct(float semis);


#ifdef __cplusplus
}
#endif

#endif /* EP122_MOD_XPAD_H */
