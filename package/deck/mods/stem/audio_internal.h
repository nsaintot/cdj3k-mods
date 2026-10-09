/* SPDX-License-Identifier: MIT OR Apache-2.0 */
/*
 * mods/stem/audio_internal.h - what the stem audio files share.
 *
 * audio.c owns the CascadedTimeStretchManager hooks and every piece of state
 * below; audio_mix.c the mix and the groove circuit; audio_rate.c the two
 * clocks; audio_probe.c the probes, the track watch and the report.
 *
 * Declarations only. A `static` definition here would give every translation
 * unit its own private copy and still link, so the hooks and the report would
 * use different objects.
 */
#ifndef EP122_MOD_STEM_AUDIO_INTERNAL_H
#define EP122_MOD_STEM_AUDIO_INTERNAL_H

#include "stem/stem.h"
#include "stem/loop.h"   /* struct stem_loop, in struct gc_phase */

/* The stretcher's position: four words it passes and returns by value. */
typedef struct { uint64_t w[4]; } pcm_pos_t;

/* How fast the file runs against the track: its beat length over the track's.
 * 1.0 (the file at its own tempo) whenever either tempo is unknown: a loop with
 * no BPM in the config, or a track with no analysis. Bounded because it
 * multiplies a track position; a ratio outside the bounds means a wrong config
 * line, and the file plays at its own tempo instead. */
#define GC_RATIO_MIN  0.25

#define GC_RATIO_MAX  4.0

/* Everything the loop phase needs that does not change within a block. Two
 * routes: with the track's beat array the file index is beats elapsed times the
 * file's beat length, which holds however the tempo moves; without one it is
 * elapsed frames times a single ratio, which holds only at a steady tempo. No
 * array just means a track with no readable grid. */
struct gc_phase {
    struct stem_loop fl;
    double           ratio;        /* the fixed-tempo route */
    const int64_t   *beats;        /* NULL selects that route */
    int32_t          count;
    double           engaged;      /* beat index the loop was engaged at */
    double           file_spb;     /* the file's beat, in frames */
    /* Which interval the last frame landed in, carried across the block. Starts
     * at -1 and is only a hint; see stem_beat_at. */
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
    /* Rolling window, so a class is reported at a steady cadence whatever its
     * rate. frames/s tells the classes apart: the one carrying playback moves
     * samples at the stream rate; anything well below is a loader or a preview.
     * Call counts alone cannot distinguish them. */
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
 * read() takes two of them as a range: the tag appears again at +0x20, and the
 * second position's sourceId is zero with its value at INT64_MAX (`from` and an
 * unbounded `to`). Only the first is the play head. It carries the source's id
 * but not the source object, which is why grid.c reads the beat grid off a cue
 * slot instead of off the block being played. */
#define POS_SOURCEID_OFF    0x08

#define POS_POS_OFF         0x18

/* The stretcher's row in g_probe. stem_engine_frames() reads it by index, so a
 * renumbered table would silently measure the wrong class. */
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
    /* Post-mix peak over the report window, to confirm the gain reached the
     * samples. 93 blocks/s x 2048 floats is ~190 k comparisons per second
     * against 96 k frames/s of phase vocoder, so the scan is not strided.
     * Written by the audio thread, cleared by the message thread; a lost update
     * costs one window of resolution, so no lock. */
    float      peak;
    /* The Position the stretcher last read at. `pos` advances by readLength per
     * call, so its rate over a window is the pool's sample rate at 1.0x, which
     * every stem alignment depends on. The read rate only matches it while the
     * stretcher runs 1:1. `sid` changes on a track change. */
    int64_t    pos;
    uint64_t   sid_lo, sid_hi;
    /* Reads whose id still carried the pre-buffering marker in its top half.
     * Reported so a firmware that packs real data there shows up; the symptom
     * would be two tracks read as one. */
    uint64_t   sid_provisional;
    /* Bumped by the audio thread whenever the sourceId changes, i.e. whenever a
     * different track starts being read.
     *
     * One of the two reliable track-change signals; g_load below is the other.
     * setSource is not one: it fires twice at player construction and never
     * again, because a single PageBuffer serves the whole pool and tracks are
     * switched by the sourceId inside each Position. decode.c's `open` hook is
     * not one either: the preview player opens files too, so browsing would
     * launch separations for tracks nobody loaded.
     *
     * A counter so the reader cannot miss two changes in one window; relaxed
     * because the message thread acts on it and one window late costs nothing. */
    uint32_t   track_gen;
};

/* The deck's own "the track is in the pool": PcmBufferFunctionHandler::
 * onLoadResult's task, which carries the load's SourceId and Result. The reads
 * above only move once the pool is read, and a deck loaded and left at 0:00
 * with AUTO CUE off reads nothing; this fires for that load. Written by the
 * task's thread under a seqlock, read from the message thread. */
struct stem_load_state {
    volatile uint32_t gen;
    uint64_t   sid_lo, sid_hi;          /* the id of a load that succeeded */
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
