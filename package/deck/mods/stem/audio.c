// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * mods/stem/audio.c - the audio side of the STEMS mod.
 *
 * stem/ui/ owns the play-screen UI (quick-menu button, BYPASS toggle, three level
 * sliders, progress row). This file owns where that UI's levels meet PCM.
 *
 * The deck's audio path is:
 *
 *   file -> audio_format::AudioReaderFactory -> FileReadFlac/Mp3/Aac/...
 *        -> pcmbuf page pool  (~400 MB, 16 KB cells, ~5 track slots)
 *        -> pcmbuf::IReadable::read()                     <-- the mix point
 *        -> time_domain::CascadedTimeStretchManager::operate()
 *        -> time_domain::ReadableTimeStretchAdapter::read()
 *        -> dj_player::DjPlayerAudioProcessor::processBlockInternal()
 *        -> meow::AudioSystem::IOAdapter -> juce -> ALSA
 *
 * The mix goes at the source read, below the stretcher, for cost: summing above
 * operate() would need one CascadedTimeStretchManager per stem (three phase
 * vocoders where the deck ships one, each with 2.9 MB of state), whereas
 * summing below it costs two multiply-adds and the single stock stretcher
 * processes the sum. No CPU estimate is needed that way.
 *
 * How that point is reached:
 *
 *   ReadableTimeStretchAdapter::read      forwards to the manager, nothing else
 *   CascadedTimeStretchManager::operate   applies parameter tasks, then hands the
 *                                         caller's buffer to one of two engines
 *   TimeStretchScratch (this+0x30)        key-shift path, wraps KeyControlFGPR
 *   TimeStretchScan    (this+0x38)        plain path
 *   engine+0x50                           pcmbuf::IReadable* -- both engines pull
 *                                         the block they are about to stretch
 *                                         from here, via read() at slot +0x10
 *
 * and CascadedTimeStretchManager::setSource (vtable +0x38) is what installs that
 * pointer into both engines. So hooking setSource yields the exact object the
 * stretcher reads from, whatever its class, and patching that class's read slot
 * puts us in front of it.
 *
 * The mix is not at the page pool because of timing: the pool holds roughly
 * 80 s behind the needle and 130 s ahead, so a gain applied there would not be
 * audible for minutes. At the source read it lands within one block.
 * Timestretch, pitch, jog, loops, hot cues and slip all sit downstream, so the
 * stems summed here stay sample-aligned with no second transport to sync.
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

/* ---- pcmbuf::IReadable ---------------------------------------------------
 *
 * read() is slot 2 (+0x10) of the primary vtable in every implementation:
 * IReadable is the primary base and read() the first virtual after the two
 * destructor slots. Same offset everywhere, so the hook works whichever class
 * is in play.
 *
 *   pcmbuf::Position read(meow::Float2 *dst, const pcmbuf::Position &src,
 *                         meow::actual_time::Sample readLength)
 *
 * x0=this, x1=dst, x2=&src, x3=readLength, x8=sret. */
#define VT_SLOT_READ        0x10

/* Every implementation of IReadable, found by scanning for type_info objects
 * that list IReadable among their bases rather than by guessing class names.
 *
 * The layering is roughly PageBuffer -> ReadableTimeStretchAdapter -> player:
 * the deck player pulls from the adapter and PageBuffer is the paged store
 * underneath. RealtimeSRCBuffer belongs to the preview player, not the deck; it
 * is probed only to tell it apart from the play path. */

/* time_domain::CascadedTimeStretchManager
 *
 * operate(const pcmbuf::Position&, meow::Float2*, actual_time::Sample) is the
 * stretch itself; it is hooked for cost only. The argument order differs from
 * read(): position first, then the buffer.
 *   x0=this, x1=const Position*, x2=meow::Float2* dst, x3=len, x8=sret.
 *
 * setSource(pcmbuf::IReadable*) is the one the mix needs. It is called from
 * ReadableTimeStretchAdapter's constructor, which resolves the readable out of
 * a meow::MappedObjPtr<pcmbuf::IReadable,0> registry by id, and it writes that
 * pointer into both engines at +0x50 before resetting the KeyControlFGPR.
 * Hooking it gives us the stretcher's input object directly, without guessing
 * the class or finding the registry.
 *   x0=this, x1=pcmbuf::IReadable*. */
#define VT_SLOT_OPERATE     0x98
#define VT_SLOT_SETSOURCE   0x38

#define POS_BYTES           0x20

/* pcm_pos_t is >16 bytes, so the AAPCS returns it indirectly through x8, as
 * the stock function does. Declaring the return type this way makes GCC emit
 * the `add x8, ...`; the UI mods do the same for juce::Font and juce::String. */

typedef pcm_pos_t (*operate_fn_t)(void *self, const void *pos, void *dst,
                                  int64_t len);
uintptr_t g_orig_operate;

/* Timer for the cost of one operate() call (does 3x fit in a block?).
 * CNTVCT_EL0 rather than clock_gettime: the shim hooks clock_gettime, so calling
 * it from the audio thread would re-enter our own code and measure that too.
 * The counter is a register read of a few ns; CNTFRQ_EL0 gives its rate (24 MHz
 * on RK3399, ~42 ns resolution against a call in the tens of microseconds). */
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

/* What setSource handed the stretcher, and the read slot patched in front of
 * it.
 *
 * `obj` is written on the track-load thread and read on the audio thread. It is
 * a single aligned pointer, so relaxed atomics are enough: the audio thread
 * sees either the old or the new source, both valid, and an aligned 8-byte
 * store cannot tear.
 *
 * `vt` is the vtable we patched. A second, different class (a second player, a
 * different decode path) is not patched, because one wrapper cannot chain to
 * two stock implementations; the report flags it instead. */
struct stem_src_state g_src;

static uintptr_t g_orig_setsource;

/* Whether the post-stretch mix point found the play path. Counted, not logged,
 * because it is decided on the audio thread; reported beside the probe rates to
 * tell a silent sampler from a gate that never matched. */
struct stem_xp_gate g_xp_gate;

/* How far the source read runs ahead of the block being written.
 *
 * stem_source_pos() is where the stretcher last read; `dst` in
 * probe_read_stretch is what it just wrote, and the stretcher buffers between
 * the two. The Position handed to that read is the block's own place in the
 * track, so the difference is the error the sampler's clock carries. Signed:
 * positive means the read position is ahead of the audio in the buffer. */
struct stem_xp_lag g_xp_lag;


/* ---- the pool's rate ------------------------------------------------------ */


int g_pool_rate;   /* 0 until measured; written by the message thread */

/* The pool rate as the time stretcher sees it. Written from the report window
 * (audio_probe.c) or by stem_engine_rate_measure, 0 until one has completed.
 *
 * This is the rate to use before anything has played: the stretcher is pulled
 * on the engine's timeline, which is the pool's, and something further down
 * resamples to whatever the DAC wants. Two audio configurations:
 *
 *   deck        DAC (hw_params)   stretcher     pool
 *   ----        ---------------   ---------     ----
 *   audio on    96000             ~96000        ~96000
 *   audio off   48000             ~96000        ~96000
 *
 * The second row is why /proc/asound is not used: hw_params gives the DAC's
 * rate, which differs from the pool's as soon as anything resamples between
 * them (48000 there would decode a stem set at half rate).
 *
 * Unlike the position measurement this needs no playback: the stretcher is
 * clocked by the output device, so it runs on a paused deck. It is also tempo-independent: tempo changes how much the
 * stretcher consumes from the pool, not the block it is asked to produce. */
int g_engine_rate;


/* meow::Float2 is a stereo float32 pair: read() indexes its backing store with
 * `base + (offset << 3)`, and processBlockInternal takes a
 * juce::AudioSampleBuffer (float). 8 bytes per frame. */
#define FLOAT2_BYTES        8

/* `sym` names the class; `vt` is filled in at install from the resolver and
 * cached because the wrappers compare a live object's vptr against it on the
 * audio thread. No expected function or prologue guard: the stock read is taken
 * from the slot itself. */

struct probe g_probe[N_PROBE] = {
    { "PageBuffer",     EP122_PCM_PAGEBUF, 0, 0 },
    { "TimeStretch",    EP122_PCM_STRETCH, 0, 0 },
    { "ThruSampleRate", EP122_PCM_THRU,    0, 0 },
    { "SeqBuffer",      EP122_PCM_SEQ,     0, 0 },
    { "SimpleBuffer",   EP122_PCM_SIMPLE,  0, 0 },
    { "PreviewSRC",     EP122_PCM_PREVIEW, 0, 0 },
};

/* operate() is hooked separately (different signature and vtable) but uses the
 * same counters, so it appears in the same rate table. */
struct probe g_op_probe = { "operate", EP122_TSMGR, 0, 0 };

int64_t stem_g_mute_quarter;
int stem_g_mute_quarter_ok;

/* One thin wrapper per class so the hot path needs no lookup to know which
 * vtable it came through. Observation only: the stock call gets the arguments
 * untouched and its result is returned verbatim. The mix happens in
 * stem_source_read, on whichever class setSource names, which may or may not be
 * one of these. */
#define PROBE_WRAPPER(idx, sym)                                             \
    static pcm_pos_t sym(void *self, void *dst, const void *src, int64_t len) \
    {                                                                       \
        struct probe *p = &g_probe[idx];                                    \
        probe_tick(p, len);                                                 \
        return ((read_fn_t)p->orig)(self, dst, src, len);                   \
    }

/* Cost of one stretch, to check against the block period. A block is `len`
 * frames; at 96 kHz 64 frames is ~666 us. Observation only: the samples are
 * changed at the source read below, one stretcher earlier. */
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
 * untouched (not multiplied by 1.0), so BYPASS is bit-exact. */
static pcm_pos_t stem_source_read(void *self, void *dst, const void *src,
                                  int64_t len)
{
    pcm_pos_t r = ((read_fn_t)g_src.orig)(self, dst, src, len);

    if (self != __atomic_load_n(&g_src.obj, __ATOMIC_RELAXED)) {
        /* The pool holds one of these per track slot, so reads through this
         * class that are not the stretcher's are expected (the loader uses them
         * too). Recording the last one distinguishes "the player reads a
         * different instance" from "we recorded the wrong object". */
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
        /* The top 32 bits of `hi` are not part of the identity.
         *
         * The pool serves several ids for one track, for example:
         *
         *     8:1010200000001
         *     ffff00000008:1010200000001
         *     fffe00000008:1010200000001
         *
         * The low half of `hi` is the track; the top half is 0 once the track
         * is settled and counts down from -1 while it is still arriving
         * (several seconds, with `pos` pinned at 4096 and no rate: the deck
         * pre-buffering).
         *
         * Comparing all 64 bits would see one track as two and fire a change
         * twice: a wasted upload or two decodes of the same cached pair, and
         * the waveform discarding its pristine copy.
         *
         * Masked, not ignored: these pre-buffering reads are the only ones a
         * paused deck makes, so they are what loads stems on a track parked at
         * the cue point.
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
 * The stock call goes first so the engines and the KeyControlFGPR are
 * consistent before the audio thread can see our pointer; then the class's read
 * slot is patched on first sight. Patching here rather than at install time
 * removes the need for class discovery: the address comes from the object
 * itself, so mod_patch_slot's expect_fn check is exact. */
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
        /* The class found here may already be in the probe table (today it is
         * PageBuffer). Then `fn` is that probe's wrapper, not the stock read,
         * and we chain through it; the log shows "stock" as a shim address.
         * The mix path therefore depends on the probe table. The probes are
         * scaffolding: once the rate and cost questions are closed they go and
         * this becomes a single hook. */
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

/* ---- the load event ------------------------------------------------------ */

/* onLoadResult's closure: the PcmBufferFunctionHandler at +0x28, the SourceId
 * at +0x18/+0x20 (two words) and the Result at +0x30. The stock run() takes
 * the result only while the handler is loading (+0x110 == 1) and the id is its
 * current load, then sets +0x110 to 2 for a good load and 3 for a failed one. */
#define LOAD_HANDLER_OFF        0x28
#define LOAD_SID_OFF            0x18
#define HANDLER_LOAD_STATE_OFF  0x110
#define LOAD_STATE_LOADED       2
#define VT_SLOT_RUN             0x10

struct stem_load_state g_load;
static uintptr_t g_orig_loadresult;
/* One writer at a time on the seqlock; the message-thread reader stays
 * lock-free. Two task threads writing at once could leave gen even over a torn
 * id. */
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

    /* Checked after the stock run, by the handler's own state: a failed or
     * superseded load leaves it anything but LOADED, and acting on that would
     * tear down the stems of the track that is playing. */
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

/* The hooks are armed regardless of g_stems_on. STEMS is a runtime toggle, so
 * gating installation on it would leave the slot unpatched for anyone who
 * switches STEMS on after boot, and a vtable slot on the audio path should not
 * be patched once samples are flowing. With STEMS off the cost is a predictable
 * branch per block; the mix is gated per call. */
static int stem_audio_install(void)
{
    char name[64];
    int i;

    /* Not fatal: without it a track loaded and left paused waits for its first
     * read. */
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

    /* setSource hands us the object the stretcher pulls from, which is where
     * the mix goes. */
    if (mod_patch_vslot("stemSetSource", EP122_TSMGR, VT_SLOT_SETSOURCE,
                        (void *)stem_set_source, &g_orig_setsource) != 0)
        g_orig_setsource = 0;
    MDBG("stem_audio: setSource %s\n",
         g_orig_setsource ? "armed" : "SKIPPED");

    /* The probes above are diagnostics and each may skip on its own. operate
     * and setSource are required for stem playback. */
    return (g_orig_operate && g_orig_setsource) ? 0 : -1;
}

KIT_MOD(k_mod_stem_audio,
        .name = "stem_audio", .prio = 40, .install = stem_audio_install,
        .what = "pcmbuf read() mix point");
