// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * wave.h - declarations shared by the waveform mod's translation units.
 *
 * Column format and band recombination: codec.c.
 *
 * Pulling the drums fader lowers each band by the share drums contributed to it. The
 * frequency axis is not remapped onto stems (a vocal is not confined to one band).
 *
 * Threads:
 *   reply hook   whatever thread the repository replies on. Publishes a pointer
 *                and nothing else.
 *   worker       ours, one per process. Owns every g_* below, the analysis, and
 *                all writes into the deck's arrays.
 *   stem job     calls wave_stems_track_ready/gone only, which raise flags.
 *
 * Drums never exists as a file: the deck plays the derived residual, so that is
 * what is analyzed (one subtraction per sample, no media write, no cache entry).
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


/* Called by the stem job when a set becomes playable and when it is torn down.
 * Both only raise a flag, so neither can stall the job. `path` is the track the
 * stems belong to; the analysis re-decodes it to derive drums. */
void wave_stems_track_ready(const char *path);
void wave_stems_track_gone(void);

/* ---- the 3-band column codec (codec.c) -------------------------------------
 * Bands are indexed low, mid, high throughout. Heights are the stored 7-bit
 * amplitudes, not dB. See codec.c for the format. */
#define MOD_WAVE_STRIDE 6
#define MOD_WAVE_BANDS  3

/* Builds the high-band conversion tables. Call once, before any scaling. */
void mod_wave_codec_init(void);

void mod_wave_decode(uint16_t hdr, const uint8_t *h, uint8_t *out);
void mod_wave_encode(const uint8_t *bands, uint16_t *hdr_out, uint8_t *h);

/* Rewrite `columns` columns, scaling by a ratio per column per band (each stem's
 * share of a band varies along the track). Low and mid scale linearly; high goes
 * through the high-band curve (codec.c). dst may alias src. */
void mod_wave_scale_ratios(const uint8_t *src, uint8_t *dst, size_t columns,
                           const float (*ratio)[MOD_WAVE_BANDS]);

/* The other two styles the deck keeps for the same track. Blue has no bands, so
 * it takes one broadband ratio; RGB carries band shares as colour plus an overall
 * height, so it takes both. Layouts are documented at the definitions (they are
 * not the file formats). */
void mod_wave_scale_blue(const uint8_t *src, uint8_t *dst, size_t columns,
                         const float *ratio_broad);
void mod_wave_scale_rgb(const uint8_t *src, uint8_t *dst, size_t columns,
                        const float (*ratio)[MOD_WAVE_BANDS],
                        const float *ratio_broad);

/* ---- the three styles ------------------------------------------------------
 *
 * The deck holds all three detailed waveforms for the loaded track, at the same
 * column count, and draws the selected style. Every latched style is scaled; the
 * analysis is shared, so the extra cost is one pass over a 1- or 2-byte array. */
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

/* How often a moving fader is sampled (250 ms lags visibly). */
#define APPLY_MS     33

/* The analyzer publishes a chunk of columns and pauses; an array caught
 * mid-fill is all zeros past the fill point and passes every structural check.
 * So the pristine copy waits for the bytes to stop moving.
 *
 * One poll per worker tick, never a loop of its own: a loop would block the worker
 * for three seconds per style (nine in total), leaving fader moves, bypass,
 * stems-off and track changes unserviced. Polling from the tick also lets the
 * three styles settle concurrently. */
#define POLL_TICKS   (250 / APPLY_MS)
#define STABLE_POLLS 12
#define CAPTURE_GIVEUP_POLLS (STABLE_POLLS * 4)

/* A fader move under this does not justify rewriting 73,000 columns. */
#define GAIN_EPSILON 0.01f

/* Only a fraction of the track is on screen: about 4,900 columns, at 3.95
 * columns per pixel.
 *
 * So a fader move paints the window around the playhead first and the rest once
 * the fader has been still for SETTLE_TICKS. Both halves are always painted, so
 * a wrong window only adds latency off screen; hence a generous margin instead
 * of tracking ZOOM. */
#define VISIBLE_BEHIND 6000
#define VISIBLE_AHEAD  14000
#define SETTLE_TICKS   6

/* Delay before retrying after a transient "not yet" from the analysis: a pool
 * rate not yet observed (track loaded and left paused), or stems being swapped
 * as the request lands. */
#define ANALYSIS_RETRY_TICKS (1000 / APPLY_MS)

/* ---- which track a reply is for -------------------------------------------
 *
 * The reply carries it: `replyDetailedWaveformRequest_3Band(const RequestID&,
 * const trackid::TrackID&, ...)`, where a TrackID is sixteen bytes (two 64-bit
 * halves) that the handler copies out.
 *
 * It is the same value the page pool uses as its sourceId, so a reply can be
 * matched against the playing track. Replies also arrive for
 * waveforms the deck view is not showing, and only the id tells them apart: pointer
 * and content comparison cannot.
 *
 * A reply can land before the sourceId is known (the first 3-band reply of a load
 * reads "playing 0:0" because no audio block has been read yet), so the id is
 * latched with the object and re-checked by the worker once the sourceId appears. */
struct wave_trackid { uint64_t lo, hi; };

/* Seqlock: written by the reply thread, read by the worker; a torn read would
 * discard a good copy. Odd generation means a write is in progress. */
struct wave_tid_slot {
    volatile uint32_t gen;
    struct wave_trackid id;
};
extern struct wave_tid_slot wave_g_tid[3];

int wave_latched_tid(int style, struct wave_trackid *out);   /* 0 if not settled */

/* The object last replied for a given track, or 0 if that track has never been
 * replied for.
 *
 * The deck keys a waveform Reception on the browse request, not the track: loading
 * a track from an earlier index in the same list reuses the object it already holds
 * and sends no reply, so the latch would stay on the previous track and the capture
 * never bind. This recovers the handle. */
uintptr_t wave_obj_for_tid(int style, uint64_t lo, uint64_t hi);

/* ---- telling one track's waveform from the next ----------------------------
 *
 * Not by pointer: the deck reuses the same object, column array and count across
 * tracks, and a shorter track after a longer one inherits its count. The count is the array's
 * capacity, not the track's length.
 *
 * So identity is content. Sixteen spans are compared against what we last wrote; a
 * mismatch means the deck refilled the array (a new track, or more analyzer output).
 * Sampled because a full 655 KB memcmp at 30 Hz across three styles gains nothing.
 *
 * Two differing probes are required, so a single localised write does not discard a
 * good copy; a different track differs almost everywhere. */
#define ARRAY_PROBES      16
#define ARRAY_PROBE_BYTES 256
#define ARRAY_PROBE_DIFFS 2

/* Where a latched reply currently points. Re-resolved every tick (three reads):
 * a stale `columns` across a track change would write megabytes through
 * /proc/self/mem into reallocated memory. */
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

/* One latched object per style. Browse previews come back through the other
 * styles' Receptions, so a single shared slot would be overwritten by a waveform
 * nobody is scaling, blocking the capture and discarding a good pristine copy. */
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
 * Every cross-file symbol must be prefixed `wave_`. The shim is LD_PRELOADed, so
 * any global it exports interposes the same name in EP122 or its libraries
 * (unprefixed `apply`, `paint`, `restore` crash the deck on track load). */

uintptr_t wave_deref(uintptr_t at);       /* a pointer, or 0 if unreadable */

/* paint.c */
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

/* analyze.c
 *
 *    1  analyzed, or loaded from the band cache
 *    0  not yet; ask again shortly
 *   -1  not possible for this track
 *
 * 0 and -1 must stay distinct: a request dropped because the rate was not yet
 * observed would never be retried, leaving the waveform at full height. */
int  wave_run_analysis(const char *path);


#ifdef __cplusplus
}
#endif

#endif /* EP122_MODS_WAVE_H */
