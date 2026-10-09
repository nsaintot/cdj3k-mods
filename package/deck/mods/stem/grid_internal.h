/* SPDX-License-Identifier: MIT OR Apache-2.0 */
/*
 * mods/stem/grid_internal.h - what the grid files share.
 *
 * grid.c owns the reply cache and the beats table it publishes, grid_read.c
 * reads a grid out of the deck's own objects, grid_edit.c moves and rebuilds
 * one. stem.h is the public interface.
 */
#ifndef EP122_MOD_STEM_GRID_INTERNAL_H
#define EP122_MOD_STEM_GRID_INTERNAL_H

#include "stem/stem.h"

struct grid_beats {
    int32_t  count;
    int      rate;                      /* what `pos` is counted at */
    uint64_t tid_lo, tid_hi;            /* which track, as the reply named it */
    int64_t  pos[];                     /* ascending, origin already added */
};

/* The deck's Content and beat-cell layout. */
/* Every meow::RefCountedObjEx carries this at +0x10 and its count at +0x0c.
 * The app checks it on every retain (assert "sig == RefCountedObjExSig"). */
#define OBJ_SIG_OFF         0x10
#define OBJ_SIG             0x52434f58      /* 'XOCR' */
/* The pointer chain described in the grid.c header. */
#define PWSI_SOURCE_OFF     0x28
#define SRC_HOLDER_OFF      0x68
#define HOLDER_CONTENT_OFF  0x28
#define HOLDER_RATE_OFF     0x40
#define HOLDER_ORIGIN_OFF   0x30
#define CONTENT_BEATS_OFF   0x28
#define CONTENT_COUNT_OFF   0x30
/* A Content holds a second array, the bars: an int32 per downbeat holding that
 * downbeat's beat index, with its own count.
 *
 * The deck writes each beat's place in its bar into the analysis file by
 * subtracting the nearest bar index at or below the beat. Rescaling the beats
 * without the bars makes that count run past 4 (5, 6, 7...) after the last old
 * bar.
 *
 * Grids are regular: bars[0] < 4 (the deck's own reader assumes the
 * first downbeat is inside the first bar) and a step of 4 after. */
#define CONTENT_BARS_OFF    0x38
#define CONTENT_BARCNT_OFF  0x40
#define BAR_STRIDE          4
#define BEATS_PER_BAR       4
/* One beat: an int64 position, then a double holding that beat's BPM.
 *
 * Read as an int32 at +8, the BPM looks like zero for a round tempo, since that
 * is the low half of a little-endian double (125.0 is 405f4000_00000000).
 *
 * The play-screen BPM comes from this field, not from the interval, so a
 * rescaled grid that leaves it alone reads 0.0 BPM on the deck. */
#define BEAT_STRIDE         0x10
#define BEAT_BPM_OFF        0x08
#define BEAT_PER_BAR        4
/* Where a cue slot keeps its PositionWithSourceInfo. Same offset preview.c
 * copies one from. */
#define SLOT_PWSI_OFF       0x28
/* Sanity bounds for a grid. One beat cannot give a length, and a huge count
 * means a wild pointer that happened to be readable. */
#define GRID_MIN_BEATS      2
#define GRID_MAX_BEATS      1000000
#define GRID_MIN_BPM        20.0
#define GRID_MAX_BPM        400.0
/* The highest CueKind to ask: the eight hot cues, the memory cue, and the
 * preview needle at 9, the last kind cue.h accounts for.
 *
 * All are asked because which one answers cannot be predicted: a track may
 * answer on kind 0 with every hot cue silent, or on kinds 1-3 with kind 0
 * silent, even for a kind whose cue is set. */
#define GRID_KIND_MAX       9
/* Reception::replyBeatGridRequest, slot 3. */
#define GRID_RECEPTION_SLOT 0x18
/* ---- the whole grid, for a track whose tempo moves ------------------------
 *
 * The average beat length suffices for a fixed tempo. For a tempo that moves,
 * a loop needs every beat; stem_beat_at turns a position into a fractional beat
 * index against this array.
 *
 * The array is copied, never pointed at: the deck's object has a lifetime we do
 * not control, and the audio thread would hold the pointer for a whole track.
 * A long track is a couple of thousand 8-byte beats, a few kilobytes.
 *
 * It is copied when the reply lands ([message]), the one moment the object is
 * certainly alive. The pad press ([deck]) takes it by a single atomic exchange;
 * whichever thread ends up holding an array nobody took frees it. */
#define GRID_BEATS_MAX      32768       /* ~4 h at 128 BPM; 256 kB of int64 */
#define GRID_BEATS_CHUNK    512         /* beats a mod_safe_read at a time    */
/* Replies are cached per track, not just the last one: the deck only replies
 * when it has to read a grid, so a track it has already read is silent when
 * reloaded (A, B, then A again: the second A gets no reply). With one slot the
 * cached grid would be the other track's and nothing would arm, leaving the
 * X-PAD and quantized mutes with no clock.
 *
 * Each slot holds scalars (samples per beat, their rate, beat 0) and our own
 * copy of the beat array, never a pointer into a deck object.
 *
 * Written by [message] alone, on the reply; also read from [deck], so each slot
 * publishes its id last and a reader re-checks it after taking the scalars. A
 * slot is reused only after GRID_REPLY_SLOTS other tracks. */
#define GRID_REPLY_SLOTS 16
/* How far into a page source grid_probe_sid looks for its sourceId. */
#define GRID_SID_SCAN       0x200
/* Bytes of the TrackID that comes with the deck's beat-grid reply (see
 * grid_wrap_reply). */
#define GRID_TID_BYTES      24
/* The red zones as the deck writes them, one element wide on each side: 16
 * bytes around the 16-byte cells, 4 around the 4-byte bar indices. The chunk
 * header sizes exactly for guard + n*stride + guard. */
#define GRID_RED_LO         0xAF
#define GRID_RED_HI         0xEF
/* A rescale past this is a mis-tap, not a grid. */
#define GRID_EDIT_K_MIN     0.125
#define GRID_EDIT_K_MAX     8.0
/* Slot 0x50 of ITrackInfoRepositoryCache:
 *
 *   bool registerBeatGrid(const AsyncCommand::RequestID &, const TrackID &,
 *                         const appnd_trk_info::BeatGrid &,
 *                         ListenerReference<ICachedBeatGridRegistListener>,
 *                         AsyncCommand::Priority)
 *
 * Taken from the live object's vtable, which is first checked against the
 * class's (the same identity rule the panel uses for a Component). */
#define GRID_TIR_REGISTER   0x50
/* The BeatGrid value inside the holder (meow::UpdatableSharedObject<BeatGrid>)
 * at +0x28: 0x20 bytes of { Content *content; int64 origin; int64; int32 rate;
 * int16 offset; }. This matches the holder offsets above (content +0x28, origin
 * +0x30, rate +0x40) and the deck's own copy of the struct at every offset
 * register.
 *
 * Copied whole, so the fields we have no name for are preserved. */
#define GRID_VALUE_OFF      0x28
#define GRID_VALUE_BYTES    0x20
/* AsyncCommand::Priority, as the deck passes it for a grid write. */
#define GRID_SAVE_PRIO      2
/* meow::ObjectMap's key: a hash of the object's name.
 *
 *     h(s) = (s[0] + 0x89 * h(s+1)) % M,  h("") = 0
 *
 * Reimplemented because the app's version is an inlined constexpr helper. The
 * same 64-bit expression gives the same result, overflow included. A wrong hash
 * resolves to another object or none, and the vtable check in grid_cache
 * refuses both. */
#define GRID_MAP_MUL        0x89ULL
#define GRID_MAP_MOD        0x1de5d6e3f8868a2ULL
#define GRID_CACHE_NAME     "TrackInfoRepositoryCachePtr"

/* The loaded track's holder, and the original grid kept for a reset.
 * grid.c owns them; grid_edit.c restores from them. */
extern uintptr_t grid_g_holder;
extern uintptr_t grid_g_orig_content;
extern uintptr_t grid_g_orig_beats;
extern int32_t   grid_g_orig_count;
extern uintptr_t grid_g_orig_bars;
extern int32_t   grid_g_orig_barcnt;

int64_t grid_to_pool(int64_t raw, int pool_rate, int grid_rate);
double grid_beat_bpm(uintptr_t beats, int i);
int grid_downbeat(uintptr_t beats, int count);
int grid_from_holder(uintptr_t holder, double *spb_out, int64_t *beat0_out, int *rate_out, const char **why);
void grid_probe_sid(uintptr_t src);
double grid_scale(uintptr_t holder, int pool_rate, int64_t *beat0, const char **why);
struct grid_beats * grid_copy_beats(uintptr_t holder);
void grid_hex(uintptr_t at, int n, char *out);

/* Declared before the typedef that uses it: a struct first named inside a
 * prototype is scoped to that prototype, and the later definition would be a
 * different type. */
struct grid_mapped_ptr;

typedef int  (*grid_link_fn)(struct grid_mapped_ptr *);
typedef void (*grid_reqid_fn)(void *);
typedef int  (*grid_register_fn)(uintptr_t cache, const void *req,
                                 const void *tid, const void *grid,
                                 const void *listener, int prio);



/* meow::MappedObjPtr: the cached pointer and the id to fill it from. link()
 * leaves a non-null pointer alone, so it is kept between calls as the deck
 * does. */
struct grid_mapped_ptr {
    uintptr_t obj;
    uint64_t  id;
};
/* meow::ListenerReference, a tagged union. Tag 2 is a shared listener with its
 * interface at +8 and its owner at +0x10; tag 0 is empty, and the app's copy
 * helper then reads nothing else. No callback is needed, so it is passed empty;
 * the outcome is read from the deck's log and the file. */
struct grid_listener_ref {
    int32_t   tag;
    uintptr_t iface;
    uintptr_t owner;
};

/* The track the holder belongs to, cleared when a grid is edited
 * but not saved. */
extern uint64_t grid_g_holder_tid_lo, grid_g_holder_tid_hi;

#endif /* EP122_MOD_STEM_GRID_INTERNAL_H */
