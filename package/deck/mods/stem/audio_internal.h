/* SPDX-License-Identifier: MIT OR Apache-2.0 */
/*
 * mods/stem/audio_internal.h - what the stem audio files share.
 *
 * audio.c owns the CascadedTimeStretchManager hooks and every piece of state
 * below; audio_mix.c the mix and the groove circuit; audio_rate.c the two
 * clocks; audio_probe.c the watch, the capture and the report.
 *
 * DECLARATIONS ONLY. A `static` definition here would give every translation
 * unit its own private copy and still link, which is how the hooks came to
 * write one probe ring while the report read another.
 */
#ifndef EP122_MOD_STEM_AUDIO_INTERNAL_H
#define EP122_MOD_STEM_AUDIO_INTERNAL_H

#include "stem/stem.h"
#include "stem/loop.h"   /* struct stem_loop, in struct gc_phase */

/* The stretcher's position: four words it passes and returns by value. */
typedef struct { uint64_t w[4]; } pcm_pos_t;

/* How fast the file runs against the track: its beat length over the track's.
 * 1.0 -- the file at its own tempo -- whenever either tempo is unknown, which
 * is the behaviour of a loop with no BPM in the config and of a track with no
 * analysis. The ratio is bounded because it multiplies a track position: a
 * grid that survived validation but disagrees wildly with the file's stated
 * tempo is a wrong config line, and playing that at forty times speed is worse
 * than playing it flat. */
#define GC_RATIO_MIN  0.25

#define GC_RATIO_MAX  4.0

struct gc_phase {
    struct stem_loop fl;
    double           ratio;        /* the fixed-tempo route */
    const int64_t   *beats;        /* NULL selects that route */
    int32_t          count;
    double           engaged;      /* beat index the loop was engaged at */
    double           file_spb;     /* the file's beat, in frames */
    /* Which interval the last frame landed in, carried across the block. Starts
     * at -1 and is only ever a hint -- see stem_beat_at. */
    int32_t          cursor;
};

/* The mute ramp: a quarter-beat of it, and whether it has been measured.
 * Defined in audio.c. */
extern int64_t stem_g_mute_quarter;
extern int stem_g_mute_quarter_ok;

/* Six sources the stretcher can pull from. operate() has its own row. */
#define N_PROBE 6

struct probe {
    const char *name;
    int         sym;
    uintptr_t   vt;
    uintptr_t   orig;        /* stock read(), for chaining */
    uint64_t    calls;       /* cumulative */
    uint64_t    frames;      /* cumulative; see stem_engine_frames */
    /* Rolling window, so a class is reported at a steady cadence whatever its rate.
     * frames/s is the number that matters: the class carrying playback must move
     * samples at the stream rate, and anything well below that is a loader or a
     * preview. Counting calls alone cannot tell those apart. */
    uint64_t    win_calls, win_frames, win_t0, win_maxblk;
};

typedef pcm_pos_t (*read_fn_t)(void *self, void *dst, const void *src,
                               int64_t len);

/* pcmbuf::Position, 32 bytes. Field names come from the binary's own assert
 * text ("source_pos.sourceId.isValid()", "0 <= source_pos.pos.value"):
 *   +0x00  constant tag written on every construction, 0x01e10c08
 *   +0x08  sourceId, 16 bytes
 *   +0x18  pos.value, int64 sample index
 *
 * read() TAKES TWO OF THEM, a range: the tag appears again at +0x20, and the
 * second position's sourceId is zero with its value at INT64_MAX -- `from` and
 * an unbounded `to`. Only the first is the play head. What it does NOT carry is
 * the source object, only its id, which is why grid.c reads the beat grid off a
 * cue slot instead of off the block being played. */
#define POS_SOURCEID_OFF    0x08

#define POS_POS_OFF         0x18

#define PROBE_STRETCH 1

#define PROBE_WIN_SEC 3

struct stem_op_stats {
    uint64_t calls, ticks, max_ticks, frames;
};

struct stem_src_state {
    void      *obj;      /* the IReadable the stretcher pulls from */
    uintptr_t  vt;       /* its vtable, once patched */
    uintptr_t  orig;     /* that class's stock read() */
    uintptr_t  seen_vt;  /* a second class, if one turned up */
    uint64_t   sets;     /* setSource calls, for the report */
    uint64_t   hits;     /* reads that matched `obj` */
    uint64_t   misses;   /* reads through the patched class that did not */
    void      *last;     /* the object of the most recent miss */
    /* Post-mix peak over the report window, so "the gain reached the samples"
     * is a number rather than a claim. 93 blocks/s x 2048 floats is ~190 k
     * comparisons per second against 96 k frames/s of phase vocoder, so the
     * scan is not worth striding. Written by the audio thread, cleared by the
     * message thread; a lost update costs one window of resolution and is not
     * worth a lock on this path. */
    float      peak;
    /* The Position the stretcher last read at. `pos` advances by readLength per
     * call, so its rate over a window IS the pool's sample rate at 1.0x -- the
     * number every stem alignment depends on, and one that must be measured
     * rather than inferred from the read rate (which only coincides while the
     * stretcher passes through 1:1). `sid` is what changes on a track change. */
    int64_t    pos;
    uint64_t   sid_lo, sid_hi;
    /* Reads whose id still carried the pre-buffering marker in its top half.
     * Reported because it is how a firmware that packs something REAL up there
     * would announce itself -- the symptom would be two tracks read as one. */
    uint64_t   sid_provisional;
    /* Bumped by the audio thread whenever the sourceId changes, i.e. whenever a
     * different track starts being read.
     *
     * One of the two honest track-change signals, with g_load below the other.
     * setSource looked like the obvious one and is not: it fires twice at player
     * construction and never again, because there is a single PageBuffer for the
     * whole pool and tracks are switched by the sourceId inside each Position
     * rather than by installing a new source. decode.c's `open` hook is not it
     * either -- the preview player opens files too, so browsing would launch
     * separations for tracks nobody loaded.
     *
     * A counter rather than a flag so the reader cannot miss two changes in one
     * window, and relaxed because a change is acted on from the message thread
     * where being one window late costs nothing. */
    uint32_t   track_gen;
};

/* The deck's own "the track is in the pool": PcmBufferFunctionHandler::
 * onLoadResult's task, which carries the load's SourceId and Result. The reads
 * above only move once the pool is read, and a deck loaded and left at 0:00
 * with AUTO CUE off reads nothing -- this is what fires for that load. Written
 * by the task's thread under a seqlock, read from the message thread. */
struct stem_load_state {
    volatile uint32_t gen;
    uint64_t   sid_lo, sid_hi;
    int32_t    result;
};

struct stem_xp_gate { uint64_t hit, miss, saw; };

struct stem_xp_lag {
    int64_t  before, after, lo, hi;
    uint64_t n;
};

/* All defined in audio.c, which owns the hooks that write them. */
extern struct stem_op_stats g_op;
extern struct stem_src_state g_src;
extern struct stem_load_state g_load;
extern struct stem_xp_gate g_xp_gate;
extern struct stem_xp_lag g_xp_lag;
extern int g_engine_rate;
extern int g_pool_rate;
extern uintptr_t g_orig_operate;
extern struct probe g_probe[N_PROBE];
extern struct probe g_op_probe;

uint64_t stem_cntvct(void);
uint64_t stem_cntfrq(void);
int  snap_rate(uint64_t r);
void pool_rate_observe(uint64_t rate);
void probe_tick(struct probe *p, int64_t len);
pcm_pos_t probe_read_stretch(void *self, void *dst, const void *src, int64_t len);
void stem_mix(void *self, const void *src, void *dst, int64_t pos, int64_t len);

#endif /* EP122_MOD_STEM_AUDIO_INTERNAL_H */
