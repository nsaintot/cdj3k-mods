// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * wave.h - the contract between the waveform mod's translation units.
 *
 * Format, injection point and the measured band recombination:
 * docs/waveform-3band-re.md.
 *
 * Pull the drums fader and the low band drops by the share drums contributed. The
 * frequency axis is NOT remapped onto stem identity -- a vocal is not confined to
 * a band. The axis stays; only its input changes.
 *
 * Threads:
 *   reply hook   whatever thread the repository replies on. Publishes a pointer
 *                and nothing else.
 *   worker       ours, one per process. Owns every g_* below, the analysis, and
 *                all writes into the deck's arrays.
 *   stem job     calls wave_stems_track_ready/gone only, which raise flags.
 *
 * Drums never exists as a file: the deck plays the derived residual, so that is
 * what is analyzed -- one subtraction per sample, no media write, no cache entry.
 */
#ifndef EP122_MODS_WAVE_H
#define EP122_MODS_WAVE_H

#include "core/mod_core.h"
#include "stem/stem.h"

#include <math.h>
#include <pthread.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif


/* Told by the stem job when a set becomes playable, and when it is torn down.
 * Worker thread; both only raise a flag, so neither can stall the job. `path` is
 * the track the stems belong to -- the analysis re-decodes it to derive drums,
 * which never exists as a file. */
void wave_stems_track_ready(const char *path);
void wave_stems_track_gone(void);

/* ---- the 3-band column codec (wave_codec.c) --------------------------------
 * Bands are indexed low, mid, high throughout. Heights are the stored 7-bit
 * amplitudes, not dB. See wave_codec.c for the format. */
#define MOD_WAVE_STRIDE 6
#define MOD_WAVE_BANDS  3

/* Builds the high-band conversion tables. Call once, before any scaling. */
void mod_wave_codec_init(void);

void mod_wave_decode(uint16_t hdr, const uint8_t *h, uint8_t *out);
void mod_wave_encode(const uint8_t *bands, uint16_t *hdr_out, uint8_t *h);

/* Rewrite `columns` columns, scaling with a ratio PER COLUMN per band -- which
 * is what a fader move actually needs, since each stem's share of a band varies
 * along the track. Low and mid scale linearly; high goes through the measured
 * curve. dst may alias src, so a whole track can be scaled in place. */
void mod_wave_scale_ratios(const uint8_t *src, uint8_t *dst, size_t columns,
                           const float (*ratio)[MOD_WAVE_BANDS]);

/* The other two styles the deck keeps for the same track. Blue has no bands at
 * all, so it takes one broadband ratio; RGB carries the three band shares as
 * colour AND an overall height, so it takes both. Layouts are documented at the
 * definitions -- they are NOT the file formats. */
void mod_wave_scale_blue(const uint8_t *src, uint8_t *dst, size_t columns,
                         const float *ratio_broad);
void mod_wave_scale_rgb(const uint8_t *src, uint8_t *dst, size_t columns,
                        const float (*ratio)[MOD_WAVE_BANDS],
                        const float *ratio_broad);

/* ---- the three styles ------------------------------------------------------
 *
 * The deck holds ALL THREE detailed waveforms for the loaded track, at the same
 * column count, and draws whichever style the DJ selected. Rather than work out
 * which that is, every style that has been latched is scaled; the analysis is
 * shared, so the extra cost is one pass over a 1- or 2-byte array. */
enum { WS_STYLE_3BAND = 0, WS_STYLE_RGB, WS_STYLE_BLUE };

extern const char *const wave_k_style_name[3];
extern const int         wave_k_stride[3];      /* bytes per column, per style */

/* shareptr -> obj -> +0x28 -> content -> +0x28 -> columns, +0x30 -> u32 count.
 * The count is a plain count, not the end half of a begin/end pair. */
#define OBJ_CONTENT_OFF     0x28
#define CONTENT_COLUMNS_OFF 0x28
#define CONTENT_COUNT_OFF   0x30
#define MAX_COLUMNS         300000

/* 150 columns per second of track, exactly. */
#define COLUMNS_PER_SEC 150

/* How often a moving fader is sampled. 250 ms read as lag -- a quarter second
 * between the finger and the picture. */
#define APPLY_MS     33

/* The analyzer publishes a chunk of columns and pauses; an array caught
 * mid-fill is all zeros past the fill point and passes every structural check.
 * So the pristine copy waits for the bytes to stop moving.
 *
 * It waits ONE POLL PER WORKER TICK, never in a loop of its own. A loop parks
 * the worker for three seconds per style and nine across the three, and for all
 * of that time a fader move, a bypass, a stems-off and a track change go
 * unserviced -- which is precisely the dead zone after a track change that
 * reads as the waveform being stuck. Polling from the tick also lets the three
 * styles settle concurrently instead of one after another. */
#define POLL_TICKS   (250 / APPLY_MS)
#define STABLE_POLLS 12
#define CAPTURE_GIVEUP_POLLS (STABLE_POLLS * 4)

/* A fader move under this does not justify rewriting 73,000 columns. */
#define GAIN_EPSILON 0.01f

/* Only a fraction of the track is on screen. Measured by marking known columns
 * and reading the marker's x position off a screenshot: columns ~3,990-8,860
 * were visible, 4,870 of 72,864 (6.7 %), at 3.95 columns per pixel.
 *
 * So a fader move paints the window around the playhead FIRST and the rest once
 * the fader has been still for SETTLE_TICKS. Correctness never depends on the
 * window being right -- both halves are always painted -- so a wrong guess costs
 * only latency on columns nobody is looking at. That is what lets this carry a
 * generous margin instead of tracking ZOOM, which moves both the span and the
 * playhead's place within it. */
#define VISIBLE_BEHIND 6000
#define VISIBLE_AHEAD  14000
#define SETTLE_TICKS   6

/* How long a transient "not yet" on the analysis is left before asking again.
 * The two that actually happen are a pool rate not yet observed -- a track
 * loaded and left paused -- and stems being swapped as the request lands. */
#define ANALYSIS_RETRY_TICKS (1000 / APPLY_MS)

/* ---- WHICH track a reply is for --------------------------------------------
 *
 * The reply carries it. `replyDetailedWaveformRequest_3Band(const RequestID&,
 * const trackid::TrackID&, ...)` -- and a TrackID is sixteen bytes, two 64-bit
 * halves, which is what the handler copies straight out of it.
 *
 * That value is THE SAME ONE the page pool uses as its sourceId, so the track
 * the deck is playing can be compared against the track a reply is about.
 * Confirmed live: reply trackid b:1020200000001 against playing sid
 * b:1020200000001, all three styles.
 *
 * This matters because replies arrive for waveforms the deck view is not
 * showing. Measured before the check existed: Rabbit Hole loaded and playing,
 * bound to High Priestess's 63307-column array, this track's ratios painted
 * onto the previous track's picture. No amount of pointer or content comparison
 * can tell those apart -- only the id can.
 *
 * ORDERING. A reply can land BEFORE the sourceId is known: the first 3-band
 * reply of a load reads "playing 0:0" because no audio block has been read yet.
 * So the id is latched with the object and checked again from the worker once
 * the sourceId turns up, rather than being a gate at the door only. */
struct wave_trackid { uint64_t lo, hi; };

/* Seqlock, because the pair is written by the reply thread and read by the
 * worker, and a torn read here would throw away a good copy for no reason. Odd
 * generation means a write is in progress. */
struct wave_tid_slot {
    volatile uint32_t gen;
    struct wave_trackid id;
};
extern struct wave_tid_slot wave_g_tid[3];

int wave_latched_tid(int style, struct wave_trackid *out);   /* 0 if not settled */

/* The object last replied for a given track, or 0 if that track has never been
 * replied for.
 *
 * The deck keys a waveform Reception on the browse REQUEST, not on the track:
 * load a track from an EARLIER index in the same list and it reuses the object
 * it already holds and never replies at all, so the latch stays on the track we
 * came from and the capture can never bind -- no copy, no analysis, no paint,
 * for as long as that track is loaded. Measured against the same track reached
 * through a different menu, which replies immediately and works, which is what
 * says the object belongs to the request and not to the track.
 *
 * The waveform itself is fine in that state and on screen; only our handle to it
 * is missing. This gives it back. */
uintptr_t wave_obj_for_tid(int style, uint64_t lo, uint64_t hi);

/* ---- telling one track's waveform from the next ----------------------------
 *
 * NOT by pointer. The deck hands out the same object, the same column array and
 * the same count for every track it loads: 63307 columns at 0xffff780b2696 was
 * measured for two different tracks six minutes apart, and a shorter track that
 * follows a longer one inherits the longer one's count. The count is the
 * array's capacity, not the track's length.
 *
 * So identity is CONTENT. A handful of spans are compared against what we last
 * wrote, and a mismatch means the deck refilled the array -- a new track, or
 * the analyzer publishing more of this one. Sampled rather than compared whole
 * because at 30 Hz across three styles a full memcmp of 655 KB buys nothing
 * that sixteen probes do not.
 *
 * Two differing probes are required rather than one, so a single localised
 * write does not throw away a good copy; a different track differs almost
 * everywhere. */
#define ARRAY_PROBES      16
#define ARRAY_PROBE_BYTES 256
#define ARRAY_PROBE_DIFFS 2

/* Where a latched reply currently points. Re-resolved every tick: it is three
 * reads, and holding a stale `columns` across a track change means writing
 * megabytes through /proc/self/mem into whatever the allocator has since put
 * there. */
struct wave_array {
    uintptr_t obj, content, columns;
    uint32_t  count;
};

/* ---- per-style state (owned by the worker) --------------------------------- */

struct style_state {
    uint8_t  *pristine;      /* the deck's own columns, untouched */
    uint8_t  *scratch;       /* rewritten columns, before the poke */
    uint8_t  *written;       /* what the deck currently holds, as we left it */
    uint32_t  ncols;
    uintptr_t columns;       /* the live array we write to */
    uintptr_t from_obj;      /* which reply the copy came from */
    uintptr_t from_content;  /* and which Content inside it */
    struct wave_trackid from_tid;   /* and which track that reply was about */
    int       stride;
    int       modified;      /* is the deck showing our version? */

    /* A copy being taken, one poll per tick until the bytes stop moving. */
    uint8_t  *probe;
    uintptr_t probe_obj;     /* the reply it started on, and stays on */
    uintptr_t probe_columns;
    uint32_t  probe_ncols;
    uint32_t  probe_digest;
    int       probe_polls;
    int       probe_stable;
    int       probe_blank;   /* consecutive polls that found nothing written */
    int       probe_wait;    /* ticks still to skip before the next poll */
};
extern struct style_state wave_g_st[3];

/* One latched object PER STYLE, not one shared slot. The deck asks for more
 * than the deck view's own style -- browse previews come back through the other
 * Receptions -- so a single slot gets overwritten by a waveform nobody is
 * scaling, which both blocks the capture and throws away a good pristine copy.
 * That regression looked exactly like "the fader stopped working". */
extern volatile uintptr_t wave_g_obj_style[3];

/* ---- the analysis, shared by every style ----------------------------------- */

extern float   *wave_g_power;                  /* [stem][band][col] band powers */
extern float  (*wave_g_ratio)[MOD_WAVE_BANDS]; /* per column, per band */
extern float   *wave_g_ratio_broad;            /* per column, all bands together */
extern uint32_t wave_g_ncols;                  /* the analysis's column count */
extern float    wave_g_applied[N_STEMS];       /* gains the display currently shows */
extern int      wave_g_have_analysis;
extern volatile int wave_g_track_gone;         /* teardown: forget everything */

/* Columns outside the last painted window, still showing the previous gains. */
extern uint32_t wave_g_tail_lo, wave_g_tail_hi;
extern int      wave_g_tail_dirty;

/* ---- across the files ------------------------------------------------------
 *
 * EVERY cross-file symbol is prefixed `wave_`, and that is not a style choice.
 * The shim is LD_PRELOADed, so any global symbol it exports INTERPOSES the same
 * name in EP122 or in the libraries it loads. Splitting this mod into files
 * briefly exported `apply`, `paint`, `restore` and `write_style` as plain
 * globals, and the deck crashed on the next track load. Anything that is not
 * static needs a prefix that cannot collide. */

uintptr_t wave_deref(uintptr_t at);       /* a pointer, or 0 if unreadable */

/* wave_paint.c */
int  wave_resolve_obj(uintptr_t obj, struct wave_array *out);  /* 0 if it will not */
int  wave_resolve(int style, struct wave_array *out);   /* the BOUND object's array */
int  wave_stale(int style);              /* the copy no longer describes the array */
void wave_invalidate(int style);         /* drop the copy; the next tick re-takes it */
int  wave_capture_step(int style);       /* one poll; 1 when the copy is complete */
void wave_write_style(int style, const uint8_t *src);
void wave_write_style_range(int style, uint32_t lo, uint32_t hi);
void wave_restore(void);
void wave_ratios_for(const float *g, uint32_t lo, uint32_t hi);
void wave_paint(uint32_t lo, uint32_t hi);
void wave_paint_tail(void);
void wave_apply(const float *g);
int  wave_gains_moved(const float *g);

/* wave_analyze.c
 *
 *    1  analyzed, or loaded from the band cache
 *    0  not yet -- nothing is wrong, ask again shortly
 *   -1  not possible for this track
 *
 * The three are distinct because collapsing them is what left the waveform at
 * full height with the stems already playing: a request consumed on a rate that
 * had not been observed yet is never asked again. */
int  wave_run_analysis(const char *path);


#ifdef __cplusplus
}
#endif

#endif /* EP122_MODS_WAVE_H */
