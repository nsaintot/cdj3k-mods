// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * stem.h - the contract between the stem modules.
 *
 * Each declaration below is tagged with the thread that may call it; the tags
 * are defined in docs/mods.md (Threads).
 *
 *   ui/        [message]  the play-screen UI, split by component; see ui/ui.h
 *   audio.c    [audio]    the mix point; reads gains and readiness only
 *   decode.c   [filler]   reader open() capture; the 44.1 kHz decode chain
 *   job.c      [worker]   job state machine; sole writer of the UI snapshot
 *   ipc.c      [worker]   framing to the sidecar
 *   store.c    [worker]   stem files, open-then-unlink, page-pool registration
 *
 * A function is called on its module's thread unless its own comment says
 * otherwise. A new function that fits none of these is a design question, not
 * a reason to add a lock.
 */
#ifndef EP122_MODS_STEM_H
#define EP122_MODS_STEM_H

#include "core/mod_core.h"
#include "lamp/lamp.h"
#include "juce/draw.h"
#include "theme/theme.h"
#include "stem_proto.h"

#ifdef __cplusplus
extern "C" {
#endif


/* ---- settings (persisted; see mod_settings.h) ----------------------------- */

/* ENABLE STEMS. Off by default; the quick-menu button and every stem setting
 * below depend on it. */
extern int g_stems_on;

/* 0 = AUTO (discover on the LAN), 1 = MANUAL (use the address below). A bool so
 * the stock two-value radio works; only the labels differ from OFF/ON. */
extern int g_stem_manual;

/* Host name or address, typed on the software keyboard. Empty until set; the
 * value column ellipsises what does not fit. */
#define STEM_ADDR_MAX 64
extern char g_stem_addr[STEM_ADDR_MAX];

/* The server's identity for its separations (backend, model, preset and stemd
 * version) as one opaque string. It scopes the on-media cache so two models
 * coexist on a stick.
 *
 * Persisted because a cache lookup needs it before any server is reachable: a
 * deck that has seen a server once plays stems from the stick without one.
 * Empty until a server identifies itself, which disables the cache.
 *
 * STEM_SEP_ID_LEN is from stem_proto.h; it crosses the socket in stem_status. */
#define STEM_SEP_ID_MAX STEM_SEP_ID_LEN
extern char g_stem_sep_id[STEM_SEP_ID_MAX];

/* Tell the worker one of the two settings above changed. The sidecar learns
 * AUTO/MANUAL and the address only from HELLO, and re-runs discovery on each
 * one, so a change has to be pushed.
 *
 * Raises a flag; the worker does the round trip, so a settings screen never
 * blocks on the network. Cheap enough to call for a value the sidecar ignores.
 *
 * Also handles ENABLE STEMS being switched on: the loaded track is requested
 * and the DJ gets the notice about needing a server. */
void mods_stem_settings_changed(void);   /* [message] */

/* Stems the model produces: level sliders on the row, bands the waveform
 * analysis derives. */
#define N_STEMS 3

/* The levels as the audio thread sees them: plain normalised floats, 1.0 =
 * unity. The UI mirrors every slider change into these because the slider
 * value lives in a juce::Value, and reading it (Value::getValue -> juce::var ->
 * ~var) allocates, which causes dropouts on the audio thread.
 *
 * Defaults are unity and g_stem_bypass is clear, so a deck with STEMS enabled
 * but untouched passes audio through bit-identically.
 *
 * Relaxed atomics, not volatile: volatile has no memory model in C11 and does
 * not rule out a torn read. Relaxed is enough because each level is
 * independent; a 5 ms stale level is inaudible. On aarch64 both compile to a
 * single LDR/STR.
 *
 * Always go through the accessors. */
extern float g_stem_gain[N_STEMS];
extern int   g_stem_bypass;   /* BYPASS: stems out of circuit */

/* ---- the held MUTE, quantized --------------------------------------------
 *
 * The caption plate (word, blink, wedge) responds to the press in the same
 * frame; the audio waits for the next quarter beat.
 *
 * `want` is the intent, written by [message]; `live` is what the mix applies,
 * committed by [audio] on the boundary. `unmuted` is the fader's own gain, kept
 * separately so the commit can restore it without reading the row's UI state
 * from the audio thread.
 *
 * Gated on the deck's own QUANTIZE, as the sampler is: with it off the commit
 * is immediate. */
#define STEM_MUTE_QUANT  4        /* quarter beat */

extern int   g_stem_mute_want[N_STEMS];
extern int   g_stem_mute_live[N_STEMS];
extern float g_stem_gain_unmuted[N_STEMS];

/* [audio] Once per block, from the mix. Three integer compares when nothing is
 * pending. */
void stem_mute_commit(void);
/* [any] How many mutes the mix has applied. The press and the sound happen at
 * different moments, so a caption change does not prove the audio changed.
 * [audio] adds, [message] reads. */
extern unsigned g_stem_mute_commits;

/* [audio] read, [message] write. The generic __atomic_load/__atomic_store, not
 * the _n forms, which take only integer and pointer types. */
static inline float stem_gain_get(int i)
{
    float v;

    __atomic_load(&g_stem_gain[i], &v, __ATOMIC_RELAXED);
    return v;
}

static inline void stem_gain_set(int i, float v)
{
    __atomic_store(&g_stem_gain[i], &v, __ATOMIC_RELAXED);
}

static inline int stem_bypass_get(void)
{
    return __atomic_load_n(&g_stem_bypass, __ATOMIC_RELAXED);
}

static inline void stem_bypass_set(int v)
{
    __atomic_store_n(&g_stem_bypass, v, __ATOMIC_RELAXED);
}

/* ---- worker -> audio ------------------------------------------------------
 *
 * Together with g_stem_gain and g_stem_bypass (above, since the UI writes them
 * too), the whole surface the realtime thread sees.
 *
 * Cleared before any teardown begins and set only after a stem set is fully in
 * place, so the audio thread never sees a half-built or half-torn-down set. It
 * reads this once per block and takes the stock path when clear. */
extern volatile int g_stem_ready;

/* ---- worker -> message ---------------------------------------------------- */

/* What the UI needs to draw, written only by job.c.
 *
 * `gen` is bumped before and after each write, so a reader that sees the same
 * even value on both sides of its copy has a consistent copy. No lock: the
 * message thread must not block, and a stale frame is invisible. */
struct stem_ui_state {
    volatile uint32_t gen;
    int stage;            /* enum stem_stage */
    /* 0..100 within the current leg, not across the job: the upload, the server
     * separation and the decode each count their own. The row weights it onto
     * the bar; see the map in ui/ui.h. */
    int percent;
    int queue_position;   /* jobs ahead of us; meaningful when QUEUED */
    /* 1 when this run goes through the separator, 0 when it came off the media.
     * Published because the stage does not carry it (LOADING is the last leg of
     * a separation and the only leg of a cache hit), and the row may not have
     * been open when the run began. */
    int via_server;
    int reachable;        /* a server answered a health probe */
    int compatible;       /* server topology we can actually play */
    /* Whether `reachable`/`compatible` mean anything yet. Before the first
     * STATUS they are zero, which looks like "no server"; without this a
     * warning would show at boot before anything was asked. */
    int status_seen;
};

/* Copy the current state consistently. Returns 0 if no writer has published
 * anything yet, in which case *out is left zeroed. */
int stem_ui_read(struct stem_ui_state *out);   /* [message] */

/* ---- job.c: the state machine (worker thread) ----------------------------- */

/* Start work for the track decode.c most recently saw opened. Idempotent per
 * track: a second call for a track already in flight or done is a no-op. Only
 * sets a request flag, so it is safe from the message thread. */
void stem_job_request(void);   /* [message] */

/* Ask the sidecar to look for a server now (an mDNS browse when its current
 * address does not answer) instead of waiting for the 30 s status refresh.
 * Sets a flag the worker reads between jobs and never touches the socket, so it
 * is safe from the message thread and free while a separation is running. */
void stem_job_probe_now(void);   /* [message] */

/* Name the track the next request is for. Called from the track watch, where
 * the sourceId is known; the worker cannot tell a re-served track from a newly
 * opened one. */
void stem_job_set_track(const char *path);   /* [message] */

/* The loaded track has gone: drop the stems held for it.
 *
 * Does not cancel a separation in flight. A separation belongs to a track, not
 * to the transport, and one the DJ has switched away from still finishes into
 * the media cache. Only another track needing the server abandons one. */
void stem_track_gone(void);   /* [message] */

/* Publish a stage the worker itself is in, for phases no sidecar reports: the
 * length probe and the multi-second decode of the pair onto the pool timeline.
 * Worker thread only: the snapshot has exactly one writer, and store.c's second
 * decode thread must not call this.
 *
 * Lives here because job.c owns the snapshot; see the seqlock note at the top
 * of job.c for why a second writer would be a bug. */
void stem_progress_set(int stage, int percent);   /* [worker] */

/* Called from the repaint tick. Reads the snapshot and updates the widgets,
 * nothing else. It does not touch the sidecar socket: the worker owns that fd,
 * so this call cannot block. */
void stem_job_poll(void);   /* [message] */

/* ---- ipc.c: framing to the sidecar ----------------------------------------
 *
 * Worker thread only, all of it. The socket is blocking on purpose: a large PCM
 * send throttled by the sidecar's drain is the backpressure that keeps a whole
 * track from ever being resident. Safe only because no other thread waits on
 * this fd. */

/* Connect if not connected, sending HELLO on success. Retries are rate-limited
 * internally, so a dead sidecar costs one failed attempt per interval.
 *
 *   1  a new connection, so a HELLO just went out
 *   0  the existing connection was reused
 *  -1  no usable socket
 *
 * Callers depend on 1 vs 0. HELLO makes the sidecar probe the server (during
 * which it stops reading this socket for about a second) and answer with STATUS.
 * So a caller must wait for STATUS before pushing PCM on a new connection, and
 * must not wait on a reused one, where no STATUS comes. */
int  stem_ipc_ensure(void);
void stem_ipc_close(void);

/* Send a HELLO on an existing connection. The sidecar re-runs discovery and
 * reports STATUS, so this refreshes reachability without dropping the socket.
 *
 * Only safe while nothing is in flight: the sidecar stops reading this socket
 * for about a second while it probes, and a concurrent PCM push fails. The
 * worker's idle branch is the one place that holds. */
int  stem_ipc_hello(void);

/* Send one frame; `payload` may be NULL when len is 0. Blocks until written.
 * Returns 0, or -1 on a broken socket (the caller closes and reports it). */
int  stem_ipc_send(uint32_t type, const void *payload, uint32_t len);

/* Receive one frame. Returns 1 when the out-parameters were filled, 0 when
 * nothing was ready within `timeout_ms`, -1 on a broken socket. */
int  stem_ipc_recv(uint32_t *type, void *buf, uint32_t cap, uint32_t *len,
                   int timeout_ms);

/* ---- decode.c: the PCM source (worker thread) ----------------------------- */

/* The file behind a sourceId, bound on first sight to whatever the deck last
 * opened. The only reliable answer to "what is playing", and the only one safe
 * to upload or to key the cache on.
 *
 * Message thread: called from the track watch, where the sid is known and
 * string work is allowed. Returns NULL before any track has been opened. */
const char *stem_decode_path_for_sid(uint64_t lo, uint64_t hi);

/* The rate the separation model is trained at, and the only rate the server
 * accepts. Playback uses stem_pool_rate() instead; see audio.c. */
#define STEM_UPLOAD_RATE 44100

/* Decode the whole track at `rate`, handing each chunk to `sink`. The sink
 * returns 0 to continue or non-zero to abort (this is how cancellation gets in
 * without a signal). Returns the number of frames delivered, or -1 on error.
 *
 * With `sink == NULL` it builds the chain, reads the converter's output length
 * and tears down without decoding, so the caller gets the frame count for
 * Content-Length without a second full pass.
 *
 * The count is the decoder's, not the file's: the deck pads its decode, and
 * stems built against the file's count are misaligned with the deck's
 * timeline.
 *
 * Worker thread only: it opens files and runs a decoder. */
typedef int (*stem_pcm_sink_fn)(const float *interleaved, int64_t frames,
                                void *user);
int64_t stem_decode_pull(const char *path, int rate, stem_pcm_sink_fn sink,
                         void *user);

void mod_stem_decode_report(void);

/* How long the deck's own track loader has been quiet, in ms.
 *
 * Every reader the factory builds goes through our open hook, and our own stem
 * readers are excluded, so this measures the deck opening files for the track
 * it is loading: the activity a stem decode must not race. The stretcher rate
 * cannot answer this; it stays healthy right through a load. */
#define STEM_DECK_NEVER_OPENED  ((uint64_t)-1)
uint64_t stem_decode_deck_quiet_ms(void);

/* Block until the loader has been quiet for `quiet_ms`, or `max_ms` passes.
 * Returns 1 when it went quiet, 0 on timeout or when the DJ moved on.
 *
 * The caller picks the window. A cache hit asks for the shortest quiet that
 * still means the loader stopped, since the wait is all the latency the DJ
 * sees. A separation is about to spend tens of seconds on the server anyway, so
 * it asks for a window long enough that no load can still be running. */
int stem_decode_wait_deck_quiet(unsigned quiet_ms, unsigned max_ms);

/* ---- store.c: stem files and the page pool (worker thread) ---------------- */

/* Decode both stem files onto the pool's timeline and publish them for the
 * audio thread. Worker thread: it runs the deck's decoder twice and allocates
 * hundreds of MB.
 *
 * The caller must keep the failures apart. RETRY on a cache hit means a valid
 * entry the deck was not ready to load; treating it as BAD throws away the pair
 * and starts a 171 MB upload for a track already on the stick. */
#define STEM_PUBLISH_OK      0
#define STEM_PUBLISH_RETRY (-1)   /* not now -- no pool rate, no memory */
#define STEM_PUBLISH_BAD   (-2)   /* these files will not decode */
#define STEM_PUBLISH_ABORT (-3)   /* the track moved on; nothing is wrong */

int  stem_store_publish(const char *harmonics_path, float harmonics_gain,
                       const char *vocals_path, float vocals_gain);

/* Is the running publish still for the track the DJ has loaded?
 *
 * A publish is seconds of decode per stem plus two waits. The store checks this
 * in both decode threads and both waits and gives up as soon as it is false,
 * so a newly loaded track does not wait behind the previous one. That is ABORT,
 * not BAD: the files are fine and the cache entry must not be condemned.
 *
 * Returns 1 for any publish the loader did not start: the check compares the
 * loader's generation, which means nothing for a publish from elsewhere. */
int  stem_job_load_wanted(void);

/* Drop every stem held for the current track and wait until no audio thread is
 * still reading them. The caller clears g_stem_ready first. */
void stem_store_release_all(void);

/* What the realtime side sees: two flat s16 buffers on the pool's timeline and
 * the number of frames both of them cover.
 *
 * REALTIME. acquire() returns 1 when the buffers are live and must be paired
 * with release() before the read returns; that pairing is what stops the worker
 * freeing memory mid-mix. */
struct stem_view {
    const int16_t *harmonics;
    const int16_t *vocals;
    /* 1.0/gain for each part. The stored samples are the server's normalised
     * ones: a separated stem can peak past full scale, so the restored value
     * does not fit an int16, and clipping it would corrupt the derived drums
     * part. The restoration is folded into the mix coefficient instead (one
     * multiply per block). */
    float          h_scale;
    float          v_scale;
    int64_t        frames;
};
int  stem_store_acquire(struct stem_view *out);
void stem_store_release(void);

/* True once both shipped parts are registered and playable. */
int  stem_store_complete(void);

/* The rate the resident stems were decoded at, 0 if none are. The pool
 * measurement is checked against this, not against whichever source would
 * answer now. Any thread. */
int  stem_store_rate(void);

/* ---- cache.c: stems on the DJ's own media (worker thread) -----------------
 *
 * Not for latency (stemd caches its own output): this lets STEMS work with no
 * server on the network at all.
 *
 * Every path here is absolute and on the stick, so both buffers have to hold a
 * mount point, the layout and a hashed key with room to spare. */
#define STEM_CACHE_PATH_MAX 384

struct stem_cache_entry {
    char    harmonics_path[STEM_CACHE_PATH_MAX];
    char    vocals_path[STEM_CACHE_PATH_MAX];
    float   harmonics_gain;
    float   vocals_gain;
};

/* Is there a cached pair for this track? `frames` is the decoder's count, from
 * the probe in stem_decode_pull. It is part of the key because stems are
 * aligned to EP122's padded decode, and a firmware that pads differently must
 * miss rather than load a misaligned pair.
 *
 * Returns 0 and fills `out` on a hit. A miss (no media, no server ever seen,
 * not separated yet) is not an error. */
int stem_cache_lookup(const char *track_path, int64_t frames,
                      struct stem_cache_entry *out);

/* Copy a freshly separated pair from tmpfs onto the media. Returns 0 when the
 * entry is committed. Failure is survivable: the stems are already resident
 * and will play; only the next load pays again. */
int stem_cache_store(const char *track_path, int64_t frames,
                     const char *harmonics_src, float harmonics_gain,
                     const char *vocals_src, float vocals_gain);

/* ---- audio.c ---- */
void mod_stem_audio_report(void);

/* The page pool's sample rate, the rate every stem must be decoded at to share
 * the deck's timeline. Never assumed: it follows the engine's output setting,
 * and this deck runs 96 kHz while its library is 44.1.
 *
 * Two sources, cross-checked: how fast pcmbuf::Position advances at the mix
 * point (needs playback), and the stretcher's engine rate (does not). Returns 0
 * only when neither has an answer yet; callers must treat 0 as "not yet", not
 * as a rate. Safe from any thread. */
int  stem_pool_rate(void);

/* Frames the deck's time stretcher has been asked for, cumulatively, or 0 if
 * nothing is counting them. The rate of change shows whether the deck has its
 * CPU back after a track load, which is the precondition for starting a decode
 * and works on a paused deck. See the definition. */
uint64_t stem_engine_frames(void);

/* Measure the engine's rate now, blocking for a quarter of a second. Worker
 * thread only. Returns 0 when nothing is counting. Use this rather than waiting
 * for stem_pool_rate() to become non-zero: the passive source only updates
 * while the play screen paints, and a track is loaded from the browser. */
int stem_engine_rate_measure(void);

/* The playhead, in pool-rate samples, or -1 if nothing has been read yet. Any
 * thread. Divided by stem_pool_rate() it is the position in seconds; times 150
 * it is the waveform column under the needle. */
int64_t stem_source_pos(void);

/* The playing track's id, read off pcmbuf::Position. Returns 0 and leaves the
 * pair meaningless until a block has been read. Any thread.
 *
 * The same 128-bit value the track-info replies carry as `trackid::TrackID`;
 * wave/stems.c uses it to tell the deck view's waveform from a browse
 * preview's. */
int stem_source_id(uint64_t *lo, uint64_t *hi);

/* ---- gc.c: GROOVE CIRCUIT, a file in place of a stem ----------------------
 *
 * Eight slots on the DJ's own stick, one per pad:
 *
 *     mods/gc/slot1-d.wav    pad A replaces the DRUMS with this file
 *     mods/gc/slot4-h.wav    pad D replaces the HARMONICS
 *     mods/gc/slot8-v.wav    pad H replaces the VOCALS
 *
 * A pad with a file belongs to the groove circuit; a pad without one is a
 * normal hot cue. One replacement at a time across the whole feature.
 *
 * A file rather than a region of the track: a region can only be a stem the
 * deck already has, which excludes the drums (the residual `mix - harmonics -
 * vocals` exists only at the play head) and anything not in the track. */

/* The first mounted volume under the USB base, which is the first bank. 0 when
 * nothing is mounted. Lives in cache.c, which owns the mount test. [worker] */
int stem_media_first_root(char *out, size_t cap);

/* Rescan the stick if the volume or the pool rate has changed. Cheap otherwise;
 * called from the worker's idle branch. [worker] */
void mod_stem_gc_poll(void);

/* ---- grid.c -------------------------------------------------------------- */

/* Pool-rate samples per beat for the loaded track, or 0 when unknown (no cue to
 * read it from, or a track with no analysis). Used to rate-match a loop; see
 * grid.c. Any thread. */
double stem_grid_spb(void);

/* Where the grid's first downbeat sits, in pool-rate samples. Meaningless while
 * stem_grid_spb() is 0. Anchoring a loop here rather than at the play head puts
 * its downbeat on the track's. [any] */
int64_t stem_grid_beat0(void);

/* Read the grid off whichever cue the deck has, and publish it. A cue slot is
 * where the source object that owns the beat grid is reachable, and a pad press
 * is when one is in hand, so this is called as a slot is armed and answers for
 * the track playing at that moment. [deck] */
struct cue_event;
int stem_grid_take(const struct cue_event *ev);

/* [message] Arm the grid from the deck's own reply, off the display clock, so a
 * track with no pad pressed yet still has one. Cheap once it has taken. */
void stem_grid_tick(void);

/* Forget it, so a stale tempo cannot be applied to the next track. [any] */
void stem_grid_forget(void);

/* The whole grid: every beat of the loaded track, ascending, in pool-rate
 * samples.
 *
 * stem_grid_spb() is the average beat, exact for a fixed-tempo track but off by
 * however much the tempo moves on one that drifts, so a loop phased off it
 * slides further out of place every bar. The array keeps a loop on the track's
 * beats whatever the tempo does. The average stays for tracks with no readable
 * array, and because the file's own tempo is a single number either way.
 *
 * REALTIME. acquire() returning 1 must be paired with release() before the read
 * returns; that pairing stops the next track's grid freeing this one mid-mix.
 * 0 means there is no array and the fixed-tempo route applies; not a failure. */
struct stem_grid_view {
    const int64_t *beats;
    int32_t        count;
};
int  stem_grid_beats_acquire(struct stem_grid_view *out);
void stem_grid_beats_release(void);

/* ---- editing the deck's own grid -----------------------------------------
 *
 * Rescale the loaded track's beat grid by `k`, a BPM multiplier: 2.0 doubles
 * the number of beats (the 3000X's [x2]), 0.5 halves it, and a value near 1 is
 * the fine [Enlarge]/[Reduce] stretch. Returns 0 when the grid moved.
 *
 * The shape is preserved: new beat j goes at the original grid's position for
 * fractional beat j/k, interpolated, so a hand-gridded or drifting track keeps
 * its drift and only its tempo scales.
 *
 * `stem_grid_edit_reset` restores what the deck loaded (the panel's RESET). The
 * original array is kept and never written, so a reset does not depend on the
 * arithmetic being invertible.
 *
 * [message]: grid/panel.c is the only caller, so there is exactly one writer;
 * see the definition for why this is safe under live readers. */
int  stem_grid_edit_scale(double k);
int  stem_grid_edit_reset(void);

/* Make the edited grid the track's, through the deck's own register path. That
 * ends in the DB server writing the Quantize atom of the track's analysis file,
 * so the edit survives the cache entry, a reload and ejecting the media.
 * Returns 0 when the request was accepted; the write lands on the repository's
 * thread shortly after.
 *
 * Refuses a grid whose TrackID is unknown: a holder reached through a cue slot
 * can be the previous track's, and writing this track's beats under that id
 * would silently corrupt it.
 *
 * The browser's tempo is a different back end and does not follow; see the
 * definition. [message] */
int  stem_grid_edit_save(void);

/* The grid's current BPM, for the panel's readout, or 0 when there is none. */
double stem_grid_bpm(void);

/* The tempo the deck loaded, before any edit. Edits are measured against it,
 * since stem_grid_edit_scale always rescales the original and so replaces the
 * previous edit rather than compounding it.
 *
 * `stem_grid_id` is a token for which grid that is, only ever compared: it
 * changes when the deck loads another grid, which is when a panel must drop its
 * edit state. 0 when there is no grid. */
double    stem_grid_orig_bpm(void);
uintptr_t stem_grid_id(void);

/* The offset the deck's own grid-adjust modes move; non-zero means its RESET
 * has something to undo. See the definition. */
int64_t   stem_grid_offset(void);

/* One slot as the mix sees it: the file, at the pool rate, interleaved stereo.
 * REALTIME: acquire() returning 1 must be paired with release() before the
 * read returns, which stops a rescan freeing it mid-mix.
 *
 * `span` is the loop, `frames` the buffer. A decoder pads, and looping over the
 * whole buffer would put that padding at the end of every bar, so a slot whose
 * config line states a BPM loops over whole beats and never reaches the
 * padding. Without a stated BPM the two are equal. */
struct gc_view {
    const int16_t *pcm;
    int64_t        frames;
    int64_t        span;
    double         spb;       /* file samples a beat, 0 when no BPM was stated */
    int            part;      /* STEM_PART_* this one stands in for */
};
int  gc_acquire(int slot, struct gc_view *out);
void gc_release(void);

/* Which stem slot `slot` replaces, or -1 when it holds no file. Any thread; the
 * lamps ask it for a colour and the pads for whether to claim a press. */
int gc_slot_part(int slot);

/* The groove circuit's lamp for one pad, for lamp/lamp.c: 0 when the pad is not
 * a runnable slot, else *out is the stem the slot replaces, steady while loaded
 * and blinking while it is the active replacement. */
int gc_pad_lamp(int pad, struct lamp *out);

/* The armed slot, or -1. `gc_active` also returns the position the loop's phase
 * is measured from: the grid's first downbeat, or the play head on a track with
 * no grid. The index into the file is (track pos - engage) at the loop's own
 * tempo, wrapped over its length, so there is no cursor to keep and nothing to
 * drift. [deck] arms and disarms, [audio] and [message] read. */
int  gc_active_slot(void);
int  gc_active(int64_t *engage);

/* STEM_PART_* of the running replacement, or -1. Not gated by the stems row;
 * see the definition, and the badge in stem/ui/ that is its only caller. */
int  gc_active_part(void);
void gc_arm(int slot, int64_t engage);
void gc_disarm(void);

/* ---- ui.c / widgets.c / layout.c (message thread) ------------------------- */

/* Whether the STEMS control row is up. The groove circuit is gated on it, so a
 * pad with a slot file is an ordinary hot cue until the DJ opens the row. [any] */
int stems_row_open(void);

/* Return every stem level to full mix. Message thread only: it moves Sliders.
 * Called on a track change so a level set for the previous track does not
 * apply to the next one; see the note at the definition. */
void mod_stems_reset_levels(void);


#ifdef __cplusplus
}
#endif

#endif /* EP122_MODS_STEM_H */
