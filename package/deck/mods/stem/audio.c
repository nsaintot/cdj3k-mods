// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * stem_audio.c - the audio side of the STEMS mod.
 *
 * stem/ui/ owns the play-screen UI (quick-menu button, BYPASS toggle, three level
 * sliders, progress row). This file owns where that UI's levels meet PCM.
 *
 * The deck's audio path is:
 *
 *   file -> audio_format::AudioReaderFactory -> FileReadFlac/Mp3/Aac/...
 *        -> pcmbuf page pool  (~400 MB, 16 KB cells, ~5 track slots)
 *        -> pcmbuf::IReadable::read()                     <-- THE MIX POINT
 *        -> time_domain::CascadedTimeStretchManager::operate()
 *        -> time_domain::ReadableTimeStretchAdapter::read()
 *        -> dj_player::DjPlayerAudioProcessor::processBlockInternal()
 *        -> meow::AudioSystem::IOAdapter -> juce -> ALSA
 *
 * The mix goes at the source read, BELOW the stretcher, and the reason is cost:
 * summing above operate() would need one CascadedTimeStretchManager per stem --
 * three phase vocoders where the deck ships one, each with 2.9 MB of state --
 * whereas summing below it costs two multiply-adds and leaves the single stock
 * stretcher to process the sum. No CPU estimate is load-bearing that way, which
 * matters because this SoC is an RK3399 A72 and the emulator cannot predict it.
 *
 * That point exists and is reachable, which is not obvious from operate() alone:
 *
 *   ReadableTimeStretchAdapter::read      forwards to the manager, nothing else
 *   CascadedTimeStretchManager::operate   applies parameter tasks, then hands the
 *                                         caller's buffer to one of two engines
 *   TimeStretchScratch (this+0x30)        key-shift path, wraps KeyControlFGPR
 *   TimeStretchScan    (this+0x38)        plain path
 *   engine+0x50                           pcmbuf::IReadable* -- BOTH engines pull
 *                                         the block they are about to stretch
 *                                         from here, via read() at slot +0x10
 *
 * and CascadedTimeStretchManager::setSource (vtable +0x38) is what installs that
 * pointer into both engines. So hooking setSource yields the exact object the
 * stretcher reads from, whatever its class, and patching that class's read slot
 * puts us in front of it.
 *
 * Timing is what forces the mix this late rather than at the page pool: the pool
 * holds roughly 80 s behind the needle and 130 s ahead, so a gain applied there
 * would not become audible for minutes. At the source read it lands within one
 * block. Everything that makes a CDJ a CDJ -- timestretch, pitch, jog, loops,
 * hot cues, slip -- sits downstream, so three stems summed here stay
 * sample-aligned with no second transport to keep in sync.
 */
#include "stem/audio_internal.h"

#include <math.h>
#include "xpad/ext.h"
#include "stem/loop.h"
#include "kit/mod.h"
/* The waveform is told when a stem set goes away, so the bars stop following
 * faders that no longer drive anything. */
#include "wave/wave.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

#ifndef SYS_gettid
#define SYS_gettid 178
#endif

/* ---- pcmbuf::IReadable (EP122-3.19) --------------------------------------
 *
 * read() is slot 2 (+0x10) of the PRIMARY vtable in all three implementations
 * -- IReadable is the primary base, read() the first virtual after the two
 * destructor slots. Same offset everywhere, so the hook does not care which
 * class turns out to be in play.
 *
 *   pcmbuf::Position read(meow::Float2 *dst, const pcmbuf::Position &src,
 *                         meow::actual_time::Sample readLength)
 *
 * x0=this, x1=dst, x2=&src, x3=readLength, x8=sret. */
#define VT_SLOT_READ        0x10

/* Every implementation of IReadable, found by scanning for type_info objects
 * that list IReadable's among their bases -- not by guessing class names, which
 * is how the first pass missed half of them.
 *
 * The layering is roughly PageBuffer -> ReadableTimeStretchAdapter -> player,
 * so the adapter is what the deck player pulls from and PageBuffer is the paged
 * store underneath it. The mix belongs BELOW the stretcher: summing above it
 * would need one CascadedTimeStretchManager per stem instead of one for the
 * sum. RealtimeSRCBuffer belongs to the preview player, not the deck -- it is
 * probed only to keep it clearly distinguished from the play path. */

/* time_domain::CascadedTimeStretchManager
 *
 * operate(const pcmbuf::Position&, meow::Float2*, actual_time::Sample) is the
 * stretch itself; it is hooked for cost only. Note the argument order differs
 * from read(): position FIRST, then the buffer.
 *   x0=this, x1=const Position*, x2=meow::Float2* dst, x3=len, x8=sret.
 *
 * setSource(pcmbuf::IReadable*) is what we actually need. It is called once per
 * track load from ReadableTimeStretchAdapter's constructor, which resolves the
 * readable out of a meow::MappedObjPtr<pcmbuf::IReadable,0> registry by id, and
 * it writes that pointer into both engines at +0x50 before resetting the
 * KeyControlFGPR. Hooking it hands us the stretcher's input object by identity,
 * so we neither guess the class nor have to find the registry.
 *   x0=this, x1=pcmbuf::IReadable*. */
#define VT_SLOT_OPERATE     0x98
#define VT_SLOT_SETSOURCE   0x38

#define POS_BYTES           0x20

/* >16 bytes, so the AAPCS returns it indirectly through x8 -- which is exactly
 * what the stock function does. Declaring the return type this way is what
 * makes GCC emit the `add x8, ...` for us; the same trick the UI mods use for
 * juce::Font and juce::String. */

typedef pcm_pos_t (*operate_fn_t)(void *self, const void *pos, void *dst,
                                  int64_t len);
uintptr_t g_orig_operate;

/* Cost of one operate() call, so we can answer "does 3x fit in a block?" before
 * building anything that depends on the answer. CNTVCT_EL0 rather than
 * clock_gettime: the shim already hooks clock_gettime, so calling it from the audio
 * thread would re-enter our own code and measure that too. The counter is a plain
 * register read, a few ns, and CNTFRQ_EL0 gives its rate (24 MHz on RK3399 => ~42 ns
 * resolution, fine against a call expected in the tens of microseconds). */
uint64_t stem_cntvct(void)
{
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

uint64_t stem_cntfrq(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

struct stem_op_stats g_op;

/* What setSource handed the stretcher, and the read slot we patched to get in
 * front of it.
 *
 * `obj` is written on the track-load thread and read on the audio thread, and
 * it is a single aligned pointer, so relaxed atomics are enough: the audio
 * thread either sees the old source or the new one, and both are valid objects
 * whose read() it is entitled to call. What it must never see is a torn value,
 * which an aligned 8-byte store cannot produce.
 *
 * `vt` remembers which vtable we patched. If a second, different class ever
 * appears -- a second player, a different decode path -- we do NOT patch it:
 * one wrapper cannot chain to two different stock implementations, and the
 * report says so rather than silently mixing one source and not the other. */
struct stem_src_state g_src;

static uintptr_t g_orig_setsource;

/* Whether the post-stretch mix point found the play path. Counted rather than
 * logged, because it is decided on the audio thread; reported beside the probe
 * rates, since "the sampler is silent" and "the gate never matched" are
 * otherwise the same observation. */
struct stem_xp_gate g_xp_gate;

/* HOW FAR THE SOURCE READ RUNS AHEAD OF THE BLOCK BEING WRITTEN.
 *
 * stem_source_pos() is where the stretcher last READ; `dst` here is what it just
 * WROTE, and a time stretcher buffers between the two. The Position handed to
 * this read is this block's own place in the track, so the difference is the
 * error the sampler's clock carries -- measured, not assumed. Signed: positive
 * means the read position is ahead of the audio in the buffer. */
struct stem_xp_lag g_xp_lag;


/* The stretcher's last read position, in pool-rate samples, or -1 before
 * anything has been read. Any thread: a torn read costs one stale window and
 * nothing else, which is why it is not worth a lock on a path the audio thread
 * touches every block.
 *
 * This is the playhead. The waveform mod uses it to work out which columns are
 * actually on screen, so that a fader move can repaint what the DJ is looking at
 * before it repaints the rest of the track. */
/* The playing track's id, as the page pool names it. 0 before any block has
 * been read, which is a "do not know" rather than an id.
 *
 * Relaxed loads of a pair that the audio thread writes as two stores: a torn
 * read is possible in principle and costs one comparison, and the caller polls.
 * A lock here would be on the realtime path for the benefit of a repaint. */
/* ---- the pool's rate ------------------------------------------------------ */


int g_pool_rate;   /* 0 until measured; written by the message thread */

/* Snap a frames-per-second figure to a rate the engine actually uses, or 0 if it
 * is not within 1 % of any of them. Refusing is the point: a rate we invented
 * would put every stem out of alignment in a way that sounds like a bad
 * separation rather than like a bug. */
/* The pool rate as the TIME STRETCHER sees it. Written by the message thread
 * from the report window below, 0 until one has completed.
 *
 * This is the rate to answer with before anything has played, and the reason is
 * where the stretcher sits: it is pulled on the ENGINE's timeline, which is the
 * pool's, and something further down resamples to whatever the DAC wants.
 * Measured across two decks with deliberately different audio configurations:
 *
 *   deck        DAC (hw_params)   stretcher     pool (measured)
 *   ----        ---------------   ---------     ---------------
 *   audio on    96000             96026         96025
 *   audio off   48000             95964         95974
 *
 * The second row is why /proc/asound is not the source. hw_params knows the
 * DAC's rate, which is a different number from the pool's the moment anything
 * sits between them -- and answering 48000 there decoded a whole stem set at
 * half rate, played it misaligned, and threw it away when the measurement
 * landed six seconds later. Two progress bars for one track.
 *
 * Unlike the position measurement this needs no playback: the stretcher is
 * clocked by the output device, so it runs on a paused deck (96011 frames/s,
 * measured). It is also tempo-independent -- what varies with tempo is how much
 * the stretcher CONSUMES from the pool, not the block it is asked to produce. */
int g_engine_rate;


/* Measure the engine rate NOW, on a thread that may block.
 *
 * The report below publishes this for free once per window, but only while the
 * play screen is painting -- and a track is loaded from the BROWSE screen, which
 * is not painting it. Measured on the deck: the load ran at 12:44:22 and the
 * report did not produce a figure until 12:44:27, five seconds after the decode
 * it was needed for had already committed to the wrong number.
 *
 * So the worker asks for it at the moment it matters instead of hoping one is
 * lying around. Two samples a quarter-second apart is enough at 96 kHz -- 24000
 * frames against a 1 % tolerance -- and the worker is the one thread where a
 * quarter of a second is free. Returns 0 if nothing is counting, which is a
 * "do not know" and never a rate.
 *
 * WORKER THREAD ONLY. */
/* Called from the report with one window's worth of Position advance. Only
 * accepts a rate seen twice in a row: a single window can straddle a seek, a
 * pause or a tempo change, any of which would read as a different rate. */
/* meow::Float2 is a stereo float32 pair: read() indexes its backing store with
 * `base + (offset << 3)`, and processBlockInternal takes a
 * juce::AudioSampleBuffer (float). 8 bytes per frame. */
#define FLOAT2_BYTES        8

/* `sym` names the class; `vt` is filled in at install from the resolver and is
 * cached because the wrappers compare a live object's vptr against it on the
 * audio thread. There is no expected function and no prologue guard: the stock
 * read comes out of the slot, so there is nothing left for either to prove. */

struct probe g_probe[N_PROBE] = {
    { "PageBuffer",     EP122_PCM_PAGEBUF, 0, 0 },
    { "TimeStretch",    EP122_PCM_STRETCH, 0, 0 },
    { "ThruSampleRate", EP122_PCM_THRU,    0, 0 },
    { "SeqBuffer",      EP122_PCM_SEQ,     0, 0 },
    { "SimpleBuffer",   EP122_PCM_SIMPLE,  0, 0 },
    { "PreviewSRC",     EP122_PCM_PREVIEW, 0, 0 },
};

/* The stretcher's row. Named because stem_engine_frames() reads it by index and
 * a silently renumbered table would measure the wrong class rather than fail. */

/* operate() is hooked separately (different signature, different vtable) but shares
 * the same counters so it appears in the same rate table. */
struct probe g_op_probe = { "operate", EP122_TSMGR, 0, 0 };

/* Accumulate one call. NOTHING ELSE.
 *
 * This runs on the audio thread and on the track loader thread, and on those threads
 * MDBG is not merely expensive, it is dangerous: it is fprintf+fflush to a stderr that
 * journald drains, so under backpressure the write BLOCKS. A blocked loader thread is
 * exactly dj_player::AsyncLoadFunctionHandler::waitForAsyncProcessing timing out --
 * the deck sits at "Not Loaded" with the hot cues blinking and nothing has crashed.
 * mod_safe_read is out for the same reason (a pread syscall per call).
 *
 * So the realtime side only ever bumps counters, and mod_stem_audio_report() does the
 * printing from the message thread. */
/* Frames the time stretcher has been asked for, cumulatively. 0 if the probe
 * that counts them is not armed.
 *
 * This is the deck's own liveness, and it is the one signal that answers "has
 * the deck finished with the track" WITHOUT needing playback. The stretcher's
 * output read is clocked by ALSA, not by the transport, so it runs at the output
 * rate whether the deck is playing or paused -- measured on this deck, 96026
 * frames/s playing and 96011 frames/s paused -- and collapses to a quarter of
 * that while a track load has the CPU (23364 frames/s, in the same window the
 * loader was filling the pool). Nothing else distinguishes those two states from
 * outside: the pool stops being read entirely on a paused deck, which is why the
 * rate cannot be measured there and why sitting at the cue point left the stems
 * waiting for a playback that never came.
 *
 * Any thread: a torn 64-bit read costs one sample of a series the caller takes
 * several of. */
/* Track change -> drop the old stems and ask for new ones.
 *
 * Cancel first and unconditionally: whatever is held belongs to a track that is
 * no longer playing, and leaving it would mix one track's stems into another.
 * The request is gated on STEMS being on, so a deck that never opted in uploads
 * nothing. Both calls only set a flag for the worker, which is what makes them
 * safe to make from the message thread.
 *
 * This runs on EVERY paint, outside the reporting window below, and that is the
 * point: sitting inside the window put up to PROBE_WIN_SEC between loading a
 * track and the first byte going out, which is most of the 4-5 s the upload
 * appeared to take. The cost of checking is a relaxed load and a compare.
 *
 * What remains is the worker's usleep (<=100 ms) and the length probe (~0.5-2 s
 * for an 8-minute track), so a fast track change still cannot be instant. */
/* ONE THING: the sourceId, resolved to a path and acted on HERE, on the message
 * thread -- the sequence below moves Sliders. Two ways of learning it.
 *
 * The id is the only thing that names the track. It comes off the page pool's
 * reads, and a deck parked at the cue point still pre-buffers, so those reads
 * carry the track's id with a countdown in the top half -- masking that half
 * rather than rejecting it is what makes AUTO CUE work (see the sourceId
 * comment in the page-read hook). A deck loaded and left at 0:00 with AUTO CUE
 * off reads nothing at all, and for that the id is taken from the deck's own
 * load result, which names the same track whether or not it will ever be
 * played. Both feed one act, deduplicated on the path.
 *
 * Deduplicated on the PATH, because the same track arriving twice is one track. */
/* Called from the play-screen paint hook, i.e. the juce message thread, where
 * blocking on stderr costs a dropped frame rather than an audio stall. */

/* Apply the stem levels to the block the stock read() just produced.
 *
 * Only two parts are stored; drums is the residual:
 *
 *     out = d*mix + (h-d)*H + (v-d)*V
 *
 * which is why the derived part costs nothing and why s16 stems reconstruct
 * exactly -- the quantisation enters twice with opposite signs and cancels. At
 * unity (d=h=v=1) the expression collapses to `mix`, and the early-out below
 * makes that literal rather than arithmetic, so BYPASS is bit-exact.
 *
 * `pos` is the pool's own sample index and our buffers were decoded onto that
 * same timeline, so frame k of the block is frame pos+k of each stem -- no
 * mapping, no interpolation, no accumulated error. Outside the stems' coverage
 * the block is left alone: we have no separation there, and silencing it would
 * turn a short stem into a dropout.
 *
 * Audio-thread rules: no allocation, no I/O, no locks, bounded loop. `len`
 * frames are always valid because the stock read() zero-fills any tail it could
 * not satisfy. */
/* The active slot's file as a loop over the TRACK's timeline. The arithmetic is
 * loop.c, which is pure and has its own tests; this only says which loop. */

/* Everything the phase needs that does not change within a block.
 *
 * TWO ROUTES, one call. With the track's beat array in hand the file index is
 * BEATS ELAPSED times the file's beat length, which is right however the track's
 * tempo moves; without one it is elapsed frames times a single ratio, which is
 * right only while the tempo holds still. The array is what a drifting or
 * hand-gridded track has and a single number cannot express, and its absence is
 * a track with no readable grid rather than an error. */

/* The file's stereo frame at fractional index `u`, linearly interpolated and
 * scaled out of s16. `u` is strictly below the span, so the successor is either
 * the next frame or the first one -- the wrap is where the loop joins, and
 * interpolating across it is what makes a rate-matched loop seamless.
 *
 * The s16 scale rides on the interpolation weights, so it costs two multiplies
 * a frame rather than two a sample. */
/* Move the held MUTE onto the beat.
 *
 * The boundary comes from the X-PAD's clock, which is the only one on the audio
 * path -- it runs off the track's grid while the play head moves and off the
 * track's tempo when it does not, so a mute lands on the beat on a parked deck
 * too.
 *
 * ON THE QUARTER IT LANDS IN, not on a boundary falling inside this call. This
 * runs pre-stretch and the clock is advanced post-stretch, so the two are not in
 * step and this is not even called once per block -- the stretcher pulls its
 * source in bursts. Asking "did a boundary fall in the last span" from here
 * waits for the two to coincide, which they need never do, and the mute then
 * simply never lands. Remembering which quarter was last seen and acting when it
 * changes needs no coincidence: the first call after the boundary does it,
 * whenever that call happens.
 *
 * With the deck's QUANTIZE off, or with no clock at all, the commit is immediate
 * -- a mute that never lands is worse than one that lands early. */
int64_t stem_g_mute_quarter;
int stem_g_mute_quarter_ok;

/* One thin wrapper per class so the hot path needs no lookup to know which
 * vtable it came through. These are all observation only -- the stock call is
 * made with the arguments untouched and its result returned verbatim. The mix
 * happens in stem_source_read, on whichever class setSource turns out to name,
 * which may or may not be one of these. */
#define PROBE_WRAPPER(idx, sym)                                             \
    static pcm_pos_t sym(void *self, void *dst, const void *src, int64_t len) \
    {                                                                       \
        struct probe *p = &g_probe[idx];                                    \
        probe_tick(p, len);                                                 \
        return ((read_fn_t)p->orig)(self, dst, src, len);                   \
    }

/* ReadableTimeStretchAdapter::read (sub_aea7f8) is a three-line forwarder:
 *
 *     x4 = *(this + 0x18);                    // the CascadedTimeStretchManager
 *     (*(*x4 + 0x98))(x4, src_pos, dst);      // operate(), NOT IReadable::read
 *     return sret;
 *
 * so it is the stretcher's OUTPUT and observation-only, like the rest. It is
 * kept in the table because it is the class that carries steady playback --
 * constant 64-frame blocks matching the ALSA period -- which is what makes the
 * rate report able to tell the play path from a loader or the preview player. */
/* Is this read the loaded track's?
 *
 * BY THE SOURCE ID IN THE POSITION, not by the object. Every read through this
 * class carries the Position it is reading at, and its sourceId is the track --
 * so this asks the only question that matters and asks it of the data rather
 * than of a layout. The adapter's own +0x18 is a CascadedTimeStretchManager and
 * looks like the answer; measured, it is a DIFFERENT manager from the one
 * setSource is called on (stable 0x559dd790 against 0x57c7cc70 across a whole
 * session), so comparing them matched nothing at ~1650 reads a second.
 *
 * The id is masked the same way stem_source_read masks it: the top half of `hi`
 * counts down while a track is still arriving and is not identity. */
/* ---- capturing the play path ---------------------------------------------
 *
 * WHAT CAME OUT, on both sides of the sampler: `dst` before the sum is the track
 * alone, `dst` after it is the track with the shots in. Subtracting one from the
 * other outside gives our own contribution with its onsets exactly where they
 * were heard, against a copy of the track from the same buffer -- which is the
 * only way to ask "did that hit land on the beat" without asking the clock that
 * placed it.
 *
 * Mono, because an onset is an onset in either channel. Armed by a file so the
 * capture is a deliberate act; [audio] fills, [message] writes. */


/* Cost of one stretch, so the shape of the design can be checked against the
 * block period. A block is `len` frames; at 96 kHz 64 frames is ~666 us. Purely
 * observational -- the samples are not touched here, they are touched at the
 * source read below, which is one stretcher earlier. */
static pcm_pos_t stem_operate(void *self, const void *pos, void *dst, int64_t len)
{
    uint64_t t0 = stem_cntvct(), dt;
    pcm_pos_t r = ((operate_fn_t)g_orig_operate)(self, pos, dst, len);

    dt = stem_cntvct() - t0;
    g_op.calls++;
    g_op.ticks += dt;
    g_op.frames += (uint64_t)(len > 0 ? len : 0);
    if (dt > g_op.max_ticks) g_op.max_ticks = dt;
    probe_tick(&g_op_probe, len);
    return r;
}

/* ---- the mix point ------------------------------------------------------- */

/* Sum the stems into the block the stock read() just produced.
 *
 * Audio-thread rules: no allocation, no I/O, no locks, bounded loop. This runs
 * pre-stretch, so `len` here is the stretcher's input length, not the ALSA
 * period -- it varies with tempo and is not something to assume.
 *
 * With STEMS off, or before any stem is installed, the block is returned
 * untouched: not "multiplied by 1.0", untouched, so BYPASS is bit-exact
 * rather than merely inaudible. */
static pcm_pos_t stem_source_read(void *self, void *dst, const void *src,
                                  int64_t len)
{
    pcm_pos_t r = ((read_fn_t)g_src.orig)(self, dst, src, len);

    if (self != __atomic_load_n(&g_src.obj, __ATOMIC_RELAXED)) {
        /* The pool holds one of these per track slot, so reads through this
         * class that are not the stretcher's own are expected -- the loader
         * uses them too. Recording the last one is what distinguishes "the
         * player reads a different instance" from "we recorded the wrong
         * object", which the pointers alone cannot. */
        g_src.misses++;
        g_src.last = self;
        return r;
    }

    g_src.hits++;
    if (src) {
        uint64_t lo, hi;

        memcpy(&g_src.pos, (const char *)src + POS_POS_OFF, sizeof(g_src.pos));
        memcpy(&lo, (const char *)src + POS_SOURCEID_OFF, sizeof(lo));
        memcpy(&hi, (const char *)src + POS_SOURCEID_OFF + 8, sizeof(hi));
        /* THE TOP 32 BITS OF `hi` ARE NOT IDENTITY.
         *
         * Measured across a session, the pool served exactly four ids for two
         * tracks:
         *
         *     b:1010200000001              754 reads
         *     8:1010200000001               75 reads
         *     ffff00000008:1010200000001     6 reads
         *     fffe00000008:1010200000001     4 reads
         *
         * The last two are the same track as the second. `hi`'s low half is the
         * track, and its top half is 0 once the track is settled and counts down
         * from -1 while it is still arriving -- held that way for eight to
         * fourteen seconds, with `pos` pinned at 4096 and no rate, which is the
         * deck pre-buffering rather than playing.
         *
         * Comparing the whole 64 bits therefore sees ONE track as two, and fires
         * a change twice: the job starts, is cancelled by the settled id seconds
         * later, and starts again -- one wasted upload, or two full decodes of
         * the same cached pair. It also tells the waveform its copy belongs to
         * another track, throwing away a pristine copy to re-take it.
         *
         * Masked rather than ignored, and that distinction is the whole point.
         * These pre-buffering reads are the ONLY ones a paused deck ever makes,
         * so they are what makes stems load on a track parked at the cue point.
         * Dropping them for not being final takes that away; masking keeps the
         * change firing at the earliest honest moment and stops the second one.
         *
         * Counted, not logged: this is the audio thread. */
        if (hi >> 32) {
            g_src.sid_provisional++;
            hi &= 0xffffffffull;
        }
        if (lo != g_src.sid_lo || hi != g_src.sid_hi) {
            g_src.sid_lo = lo;
            g_src.sid_hi = hi;
            __atomic_store_n(&g_src.track_gen, g_src.track_gen + 1,
                             __ATOMIC_RELAXED);
        }
    }

    if (g_stems_on && !stem_bypass_get())
        stem_mix(self, src, dst, g_src.pos, len);

    {
        const float *s = (const float *)dst;
        int64_t n = len * 2, i;
        float pk = g_src.peak;

        for (i = 0; i < n; i++) {
            float a = s[i] < 0.0f ? -s[i] : s[i];
            if (a > pk) pk = a;
        }
        g_src.peak = pk;
    }
    return r;
}

/* CascadedTimeStretchManager::setSource. Runs on the track-load thread.
 *
 * The stock call goes first so the engines and the KeyControlFGPR are already
 * consistent before the audio thread can observe our pointer; then the class's
 * read slot is patched on first sight. Patching here rather than at install
 * time is what makes the class discovery unnecessary -- the address comes from
 * the object itself, so mod_patch_slot's expect_fn check is exact and the
 * prologue guard has nothing left to prove. */
static void stem_set_source(void *self, void *readable)
{
    uintptr_t vt = 0, fn = 0;

    ((void (*)(void *, void *))g_orig_setsource)(self, readable);
    g_src.sets++;

    if (!readable) {
        __atomic_store_n(&g_src.obj, (void *)0, __ATOMIC_RELAXED);
        return;
    }

    memcpy(&vt, readable, sizeof(vt));
    if (g_src.vt && vt != g_src.vt) {
        g_src.seen_vt = vt;               /* reported, not patched */
        return;
    }

    if (!g_src.vt) {
        /* The class discovered here may already be in the probe table -- it is,
         * today: PageBuffer. Then `fn` is that probe's wrapper rather than the
         * stock read, and we chain through it. That is correct and the log says
         * so ("stock" reads as a shim address), but it means the probe table is
         * load-bearing for the mix path. It is scaffolding: once the rate and
         * cost questions are closed it goes, and this becomes a single hook. */
        if (mod_safe_read(vt + VT_SLOT_READ, &fn, sizeof(fn)) != 0)
            return;
        if (mod_patch_slot("stemSource", vt + VT_SLOT_READ, fn,
                           (void *)stem_source_read, &g_src.orig) != 0)
            return;
        g_src.vt = vt;
    }

    __atomic_store_n(&g_src.obj, readable, __ATOMIC_RELAXED);
}

PROBE_WRAPPER(0, probe_read_pagebuf)
PROBE_WRAPPER(2, probe_read_thru)
PROBE_WRAPPER(3, probe_read_seq)
PROBE_WRAPPER(4, probe_read_simple)
PROBE_WRAPPER(5, probe_read_preview)

static void *const k_probe_wrapper[N_PROBE] = {
    (void *)probe_read_pagebuf,
    (void *)probe_read_stretch,
    (void *)probe_read_thru,
    (void *)probe_read_seq,
    (void *)probe_read_simple,
    (void *)probe_read_preview,
};

/* The hook is armed regardless of g_stems_on. STEMS is a runtime toggle owned
 * by the MOD SETTINGS row, so gating installation on it would leave the slot
 * unpatched for anyone who switches STEMS on after boot -- and a vtable slot on
 * the audio path is not something to start patching once samples are flowing.
 * The wrapper chains straight to the stock call, so with STEMS off the cost is
 * a predictable branch per block; the mix itself gets gated per call. */
/* ---- the load event ------------------------------------------------------ */

/* onLoadResult's closure: the PcmBufferFunctionHandler at +0x28, the SourceId
 * at +0x18/+0x20 (sub_107ee20 stores the two words of the id there) and the
 * Result at +0x30. The stock run (3.19 sub_107e3d8) takes the result only while
 * the handler is loading (+0x110 == 1) and the id is its current load, then
 * sets +0x110 to 2 for a good load and 3 for a failed one. */
#define LOAD_HANDLER_OFF        0x28
#define LOAD_SID_OFF            0x18
#define HANDLER_LOAD_STATE_OFF  0x110
#define LOAD_STATE_LOADED       2
#define VT_SLOT_RUN             0x10

struct stem_load_state g_load;
static uintptr_t g_orig_loadresult;
/* One writer at a time on the seqlock; the message-thread reader stays
 * lock-free. Two task threads with the same base would leave gen even over a
 * torn id. */
static pthread_mutex_t g_load_mu = PTHREAD_MUTEX_INITIALIZER;

static void stem_load_result(void *task)
{
    uint64_t lo = 0, hi = 0;
    uintptr_t handler = 0;
    int32_t state = 0;
    int have = mod_safe_read((uintptr_t)task + LOAD_HANDLER_OFF, &handler, sizeof(handler)) == 0 &&
               mod_safe_read((uintptr_t)task + LOAD_SID_OFF, &lo, sizeof(lo)) == 0 &&
               mod_safe_read((uintptr_t)task + LOAD_SID_OFF + 8, &hi, sizeof(hi)) == 0;

    ((void (*)(void *))g_orig_loadresult)(task);

    /* Judged AFTER the stock run, by the handler's own state: a failed or a
     * superseded load leaves it anything but LOADED, and acting on that tore
     * down the stems of the track that is actually playing. */
    if (!have || !handler ||
        mod_safe_read(handler + HANDLER_LOAD_STATE_OFF, &state, sizeof(state)) != 0 ||
        state != LOAD_STATE_LOADED)
        return;

    pthread_mutex_lock(&g_load_mu);
    {
        uint32_t gen = g_load.gen;

        __atomic_store_n(&g_load.gen, gen + 1, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        g_load.sid_lo = lo;
        g_load.sid_hi = hi & 0xffffffffull;   /* masked as the reads mask it */
        __atomic_thread_fence(__ATOMIC_RELEASE);
        __atomic_store_n(&g_load.gen, gen + 2, __ATOMIC_RELAXED);
    }
    pthread_mutex_unlock(&g_load_mu);
}

static int stem_audio_install(void)
{
    char name[64];
    int i;

    /* Not fatal: without it a track loaded and left paused waits for its first
     * read, which is how every build before this behaved. */
    if (mod_patch_vslot("stemLoadResult", EP122_PCM_LOADRESULT_TASK, VT_SLOT_RUN,
                        (void *)stem_load_result, &g_orig_loadresult) != 0)
        g_orig_loadresult = 0;
    MDBG("stem_audio: load result %s\n", g_orig_loadresult ? "armed" : "SKIPPED");

    for (i = 0; i < N_PROBE; i++) {
        struct probe *p = &g_probe[i];

        p->vt = ep122_sym(p->sym);
        snprintf(name, sizeof(name), "stemRead:%s", p->name);
        if (mod_patch_vslot(name, p->sym, VT_SLOT_READ,
                            k_probe_wrapper[i], &p->orig) != 0)
            p->orig = 0;
    }

    for (i = 0; i < N_PROBE; i++)
        MDBG("stem_audio: %-16s %s\n", g_probe[i].name,
             g_probe[i].orig ? "armed" : "SKIPPED");

    g_op_probe.vt = ep122_sym(g_op_probe.sym);
    if (mod_patch_vslot("stemOperate", EP122_TSMGR, VT_SLOT_OPERATE,
                        (void *)stem_operate, &g_orig_operate) != 0)
        g_orig_operate = 0;
    MDBG("stem_audio: operate %s (cntfrq %llu Hz)\n",
         g_orig_operate ? "armed" : "SKIPPED",
         (unsigned long long)stem_cntfrq());

    /* setSource carries the mix point: it is what hands us the object the
     * stretcher pulls from, by identity rather than by class. */
    if (mod_patch_vslot("stemSetSource", EP122_TSMGR, VT_SLOT_SETSOURCE,
                        (void *)stem_set_source, &g_orig_setsource) != 0)
        g_orig_setsource = 0;
    MDBG("stem_audio: setSource %s\n",
         g_orig_setsource ? "armed" : "SKIPPED");

    /* The probes above are diagnostics and each may skip on its own. These two
     * are the feature: operate is where stem audio is mixed in, setSource is what
     * identifies the object to mix into. Without both there is no stem playback,
     * however many probes armed. */
    return (g_orig_operate && g_orig_setsource) ? 0 : -1;
}

KIT_MOD(k_mod_stem_audio,
        .name = "stem_audio", .prio = 40, .install = stem_audio_install,
        .what = "pcmbuf read() mix point");
