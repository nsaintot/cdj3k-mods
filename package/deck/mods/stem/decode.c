// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * stem_decode.c - getting a whole track out of the deck as 44.1 kHz PCM.
 *
 * The stem model is trained at 44.1 kHz, so the separation server takes PCM at
 * that rate and nothing else. The deck runs its engine at 96 kHz, so none of the
 * PCM already in flight is usable: the page pool holds resampled audio (the
 * class that fills it is literally ResamplingReaderCache), and taking it from
 * there would mean 44.1 -> 96 -> 44.1, two conversions to arrive back where we
 * started. It also only ever holds a window around the needle, never the track.
 *
 * So we decode the file a second time, ourselves, at the rate we want:
 *
 *   AudioReaderFactory::createReaderFor(path, &err, open=1, seekTable, frameInfo)
 *        -> FileReadFlac/Mp3/Aac/Alac/Mp4/Aiff/Wav   (one per container)
 *   SampleRateConverter::setSource(reader, 44100, own)
 *        -> fileRead(pos, meow::Float2 *dst, frames)  as fast as we can pull it
 *
 * The one thing that recipe needs and cannot invent is `open`'s 2nd and 3rd
 * arguments - the SeekTable and FrameInfoList the deck built during track
 * analysis. For a VBR MP3 those are what make seeking correct, and they live in
 * the track info repository behind a request/reply API we would have to drive.
 *
 * We do not have to. The deck opens a reader for the track anyway, so this TU
 * hooks `open` and keeps what it was handed. Then creating our own second,
 * independent reader is just calling the same stock factory back with the same
 * arguments - no analysis data synthesised, no reader shared with the player,
 * and no lifetime entanglement with the page filler's copy.
 *
 * The hook records what every open() was handed (the reporter prints it) and
 * keeps, for the track the deck opened last, the path and those two tables;
 * stem_decode_pull hands them back to createReaderFor for that path.
 *
 * Threading: `open` runs on the page-filler thread, which the track loader
 * blocks on - stall it and the load times out in
 * AsyncLoadFunctionHandler::waitForAsyncProcessing, which surfaces as a deck
 * stuck at "Not Loaded" with the hot cues blinking rather than as a crash. So
 * the hook does bounded /proc/self/mem reads and NOTHING else; all printing
 * happens in mod_stem_decode_report() on the message thread.
 */
#include "stem/stem.h"
#include "kit/mod.h"

/* The only mod that needs a thread of its own. Undefined at link time and
 * resolved from the host process, which already links libpthread. */
#include <pthread.h>

/* ---- audio_format::AbstractReader (EP122-3.19) ---------------------------
 *
 * Found by walking RTTI base arrays (scripts/ep122sym.py impls
 * audio_format::AbstractReader), which is also how we know the list is complete.
 * The seven file readers share AbstractFileReader::open; the JUCE wrapper has
 * its own. One wrapper serves all eight, telling them apart by vptr.
 *
 *   bool open(const juce::String &path, const audio_format::SeekTable &,
 *             const audio_format::FrameInfoList &)
 *
 * getSampleRate is slot +0x50. That is not a guess: SampleRateConverter::setSource
 * special-cases this exact function pointer and reads the field directly when it
 * matches, so the binary itself identifies the slot. The default implementation
 * is two instructions - `ldr w0, [x0, #0x88]; ret` - so we read the field rather
 * than call into the guest from a thread the loader is blocked on. */
#define VT_SLOT_OPEN        0x10
#define VT_SLOT_GETRATE     0x50
#define FN_READER_GETRATE   ep122_sym(EP122_READER_FLAC_GETRATE)
#define READER_RATE_OFF     0x88

/* audio_format::AudioReaderFactory - the singleton the deck creates readers
 * through, and createReaderFor itself. Non-virtual, hence the direct address.
 *
 *   AbstractReader *createReaderFor(const juce::String &path, ReaderErrorType *err,
 *                                   bool alsoOpen, const SeekTable &,
 *                                   const FrameInfoList &, kind)
 *
 * With alsoOpen = 0 it constructs without opening; with 1 it calls open() and
 * unwinds on failure. Recorded here for the step that follows this probe.
 *
 * Nothing in .text takes the singleton's address -- every caller of
 * createReaderFor already holds it in a register -- so there is no ADRP pair to
 * read it out of, and it lives in .bss so there is nothing in the file either.
 * What it does have at run time is its vptr, which is unique to its class: the
 * object is found by looking for that, resolved lazily because the static-init
 * pass has to have run first. */
#define ADDR_READER_FACTORY decode_reader_factory()
#define FN_CREATE_READER    ep122_sym(EP122_CREATE_READER_FOR)

/* audio_format::SampleRateConverter. Constructed in place by the page filler
 * inside a shared_ptr control block (operator new(0x98), object at +0x10), so
 * the object itself is 0x88 bytes.
 *
 *   vt +0x10  setSource(AbstractReader *, int targetRate, bool own)
 *   vt +0x18  ReaderErrorType fileRead(aint pos, meow::Float2 *dst, aint frames)
 *
 * setSource takes the target rate as a plain int - it is not pinned to the
 * engine's 96 kHz - which is the entire reason this approach works. */
#define SRC_SIZE            0x88
#define FN_SRC_CTOR         ep122_sym(EP122_SRC_CTOR)
#define FN_SRC_DTOR         ep122_sym(EP122_SRC_DTOR)  /* destroys, does not free */
#define VT_SRC              ep122_sym(EP122_SRC)
#define VT_SLOT_SETSOURCE   0x10
#define VT_SLOT_FILEREAD    0x18

/* Fields setSource fills in, so we can report what the chain actually agreed on
 * rather than what we asked for. Rates are int32, lengths int64 frames. */
#define SRC_SRCRATE_OFF     0x20
#define SRC_DSTRATE_OFF     0x24
#define SRC_INLEN_OFF       0x28
#define SRC_OUTLEN_OFF      0x30

/* juce::String is constructed in place from a C literal all over the binary's
 * own assert code - that is where these two come from.
 *
 *   sub_1a55060(juce::String *this, const char *utf8)
 *   sub_1a1d9b0(juce::String *this)                    // destructor
 */
#define FN_JSTR_CTOR        ep122_sym(EP122_JUCE_STRING_CTOR_CSTR)
#define FN_JSTR_DTOR        ep122_sym(EP122_JUCE_STRING_DTOR)

/* What the stem model wants, and so the rate the UPLOAD is decoded at. Most DJ
 * libraries are already 44.1 kHz, in which case setSource's rate branch makes
 * the converter a passthrough.
 *
 * Playback is a different rate: stems have to land on the pool's timeline, which
 * is 96 kHz on this deck (see stem_pool_rate). Same converter, same source file
 * rate, same target -- so the deck's own resample and ours agree by being the
 * identical computation, not by being close. */
#define STEM_TARGET_RATE    44100

/* Frames per fileRead. 4096 frames = 32 KB of meow::Float2, big enough that the
 * per-call overhead disappears and small enough to stay off the loader's toes.
 * Reads must be sequential: fileRead compares the requested position against
 * m_currentPosition and takes a seek path when they differ. */
#define DECODE_CHUNK        4096

/* juce::String is one pointer to UTF-8 character data (a refcount and a byte
 * count sit behind it, which we never touch). So a `const juce::String &` is a
 * pointer to a pointer to the text. */
#define PATH_MAX_CAP        192

/* audio_format::IIndividualReaderFactory - one per container, held in an array
 * on the AudioReaderFactory. createReaderFor walks them in order and asks each
 * to build a reader; they dispatch purely on the path's extension (the Flac one
 * literally tests ".FLAC" then ".FLA"), so the first that recognises the name
 * wins. Slot +0x10 is that call:
 *
 *   AbstractReader *create(const juce::String &path, const Config &, Kind)
 *
 * Hooked for one value only: `kind`. It reaches the reader's constructor and is
 * the single argument of createReaderFor we cannot read anywhere statically -
 * it comes from a field of the ResamplingReaderCache, whose address we do not
 * have. Everything else about creating our own reader is already known. */
#define VT_SLOT_FACTORY_CREATE  0x10

/* `sym` names the class; `vt` is filled in at install from the resolver, and a
 * class that is not present simply leaves 0 there and is skipped. The wrapper
 * still identifies its caller by comparing vptrs, so `vt` has to be cached
 * rather than looked up per call -- open() runs on the page-filler thread. */
struct reader_vt {
    const char *name;
    int         sym;
    uintptr_t   vt;
    uintptr_t   orig;      /* stock open(), or 0 if the slot was not patched */
};

struct factory_vt {
    const char *name;
    int         sym;
    uintptr_t   vt;
    uintptr_t   orig;      /* stock create(), distinct per factory */
};

/* Order is cosmetic. Wav and Aiff go through their own readers rather than the
 * juce wrapper, which is why they are here. */
static struct reader_vt g_reader[] = {
    { "Flac", EP122_READER_FLAC, 0, 0 },
    { "Mp3",  EP122_READER_MP3,  0, 0 },
    { "Aac",  EP122_READER_AAC,  0, 0 },
    { "Alac", EP122_READER_ALAC, 0, 0 },
    { "Mp4",  EP122_READER_MP4,  0, 0 },
    { "Aiff", EP122_READER_AIFF, 0, 0 },
    { "Wav",  EP122_READER_WAV,  0, 0 },
    /* The factory's fallback for what the seven refuse -- a 32-bit float WAV
     * among them. The deck plays those through it, so this is the open that
     * succeeds for such a track, and the only one that names its path. */
    { "Juce", EP122_READER_JUCE, 0, 0 },
};
#define N_READER_VT ((int)(sizeof(g_reader) / sizeof(g_reader[0])))

/* The same set of containers on the factory side. Each has its own create(), so
 * the wrapper identifies the caller by vptr rather than needing one wrapper per
 * factory -- and the stock create() now comes out of the slot instead of being
 * listed, which is what removed seven more addresses from this table. */
static struct factory_vt g_factory[] = {
    { "Flac", EP122_FACTORY_FLAC, 0, 0 },
    { "Mp3",  EP122_FACTORY_MP3,  0, 0 },
    { "Aac",  EP122_FACTORY_AAC,  0, 0 },
    { "Alac", EP122_FACTORY_ALAC, 0, 0 },
    { "Mp4",  EP122_FACTORY_MP4,  0, 0 },
    { "Aiff", EP122_FACTORY_AIFF, 0, 0 },
    { "Wav",  EP122_FACTORY_WAV,  0, 0 },
};
#define N_FACTORY_VT ((int)(sizeof(g_factory) / sizeof(g_factory[0])))

typedef int (*open_fn_t)(void *self, const void *path, const void *seek_table,
                         const void *frame_info);
typedef void *(*create_fn_t)(void *self, const void *path, const void *cfg,
                             uint64_t kind);

/* What one open() call was handed. Filled by the hook, drained by the report.
 *
 * `pending` is the handshake: the hook writes the fields, then sets it last, and
 * the reporter clears it after printing. A plain int is enough - there is one
 * page-filler thread and one message thread, the fields are only read once
 * pending is set, and a missed capture costs a log line, not correctness. */
struct open_capture {
    volatile int pending;
    int          which;                /* index into g_reader                */
    int          ret;                  /* what open() returned               */
    int          rate;                 /* getSampleRate() after open         */
    uintptr_t    reader;
    uintptr_t    seek_table;           /* open()'s 2nd argument              */
    uintptr_t    frame_info;           /* open()'s 3rd argument              */
    uintptr_t    path_ref;             /* the juce::String &, for re-use     */
    char         path[PATH_MAX_CAP];
};
static struct open_capture g_cap;

static unsigned g_open_calls;

/* The path of the track the deck most recently opened. Distinct from
 * g_cap.path, which the reporter consumes and clears; the decode worker needs
 * the path to stay valid for as long as the track is loaded. */
static char g_track_path[PATH_MAX_CAP];

/* Set the instant a SUCCESSFUL deck open refreshes g_track_path, cleared the
 * first time a new sourceId consumes it. A track whose OWN open failed never
 * sets it, so g_track_path still points at the previous, successfully-opened
 * track -- and a new id must NOT bind to that stale path. Consume-once is what
 * tells "the deck just opened this track" apart from "g_track_path is left over
 * from a different song": only the former is fresh. Written from the page-filler
 * thread with RELEASE after the memcpy, read from the message thread with
 * ACQUIRE, so a reader that sees fresh=1 also sees the path bytes. */
static int g_track_fresh;

/* The SeekTable and FrameInfoList the deck built for g_track_path -- open()'s
 * 2nd and 3rd arguments, which the worker cannot invent. Both are handed back
 * to createReaderFor so our reader opens against the same analysis the deck's
 * did -- for a VBR MP3 that is what lets setSource size it, since the reader
 * alone carries no length. Captured on the SAME successful open as g_track_path
 * so the three always describe one track, and only used while the pull's path
 * still matches, i.e. for the track the deck has loaded and whose analysis it
 * is holding. They are read during createReaderFor only: the MP3 reader builds
 * its own frame index from them at open (sub_a47b20), so the exposure is that
 * call, not the decode that follows. Never used for a re-served track: nothing
 * says the deck still holds the analysis it handed over the first time. */
static uintptr_t g_track_seek;
static uintptr_t g_track_frame;

/* Set while THIS thread is inside stem_decode_pull.
 *
 * The open hook is global: it fires for every reader the factory builds, and we
 * build readers ourselves to load stems. Without this the hook recorded
 * /dev/shm/harmonics.flac as "the track", and the consequences were not subtle:
 *
 *   - the next job uploaded that stem instead of the track, so the server saw a
 *     brand new signal every time and its content-addressed cache never hit --
 *     visible as the frame count climbing 17534160 -> 17538276 -> 17542392, one
 *     4116-frame decoder pad per generation of separating our own output;
 *   - key_of() hashed a transient tmpfs file, so the on-media cache never hit
 *     either, and happily STORED entries under keys that could not recur;
 *   - and what finally played was a separation of a stem, misaligned against
 *     the track on the deck -- which reads as "the faders do nothing".
 *
 * Thread-local rather than a global flag because the deck's own opens come from
 * the page filler thread and ours from the worker: a global would suppress a
 * genuine capture that happened to overlap one of our decodes. */
static __thread int g_decode_self;

/* CLOCK_MONOTONIC ms of the deck's own most recent reader open, or 0 if it has
 * not opened one yet. Written from whichever thread the factory ran on and read
 * by the worker, so plain atomics rather than a lock.
 *
 * This is the deck's track loader saying it is still working. The stretcher
 * measurement that used to gate the stem decode does not see that at all: on a
 * cache hit it read healthy through the whole of a load that was still opening
 * files 1.3 s later. See wait_for_deck. */
static volatile uint64_t g_deck_open_ms;

static uint64_t decode_now_ms(void)
{
    struct timespec ts;

    /* CLOCK_MONOTONIC, not the wall clock: the shim time-shifts gettimeofday. */
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000);
}

uint64_t stem_decode_deck_quiet_ms(void)
{
    uint64_t last = __atomic_load_n(&g_deck_open_ms, __ATOMIC_ACQUIRE);
    uint64_t now;

    if (!last)
        return STEM_DECK_NEVER_OPENED;
    now = decode_now_ms();
    return now > last ? now - last : 0;
}

#define DECK_QUIET_POLL_MS  25

int stem_decode_wait_deck_quiet(unsigned quiet_ms, unsigned max_ms)
{
    unsigned waited;

    for (waited = 0; waited < max_ms; waited += DECK_QUIET_POLL_MS) {
        uint64_t quiet = stem_decode_deck_quiet_ms();

        if (quiet >= quiet_ms) {
            if (waited)
                MDBG("stem_decode: deck loader quiet for %llu ms,"
                     " waited %u ms for it\n",
                     (unsigned long long)quiet, waited);
            return 1;
        }
        usleep(DECK_QUIET_POLL_MS * 1000);
        if (!stem_job_load_wanted())
            return 0;               /* the DJ loaded something else */
    }
    MDBG("stem_decode: deck still opening readers after %u ms\n", max_ms);
    return 0;
}

/* createReaderFor's last two arguments, as seen by whichever factory accepted
 * the file. Recorded once - they are per-cache constants, not per-track. */
static volatile int g_factory_seen;
static uintptr_t    g_factory_cfg;
static uint64_t     g_factory_kind;
static int          g_factory_which;

/* Copy a NUL-terminated string out of the guest's own address space without
 * risking a fault. mod_safe_read insists on a full read, so a string near the
 * end of a mapping fails a big request and succeeds a small one - hence the
 * chunked walk rather than one PATH_MAX_CAP read. */
static void cap_juce_string(uintptr_t str_ref, char *out, size_t n)
{
    uintptr_t text = 0;
    size_t i = 0;

    out[0] = '\0';
    if (mod_safe_read(str_ref, &text, sizeof(text)) != 0 || !text)
        return;

    while (i + 8 < n) {
        char chunk[8];
        int k;

        if (mod_safe_read(text + i, chunk, sizeof(chunk)) != 0)
            break;
        for (k = 0; k < 8; k++) {
            out[i + k] = chunk[k];
            if (!chunk[k])
                return;
        }
        i += 8;
    }
    out[i] = '\0';
}

/* The hook. Chains first so a capture never changes what the deck sees, then
 * records - including the sample rate, which is only meaningful once the file
 * header has been parsed. Bounded reads only: see the threading note up top. */
static int stem_reader_open(void *self, const void *path, const void *seek_table,
                            const void *frame_info)
{
    int which = -1, i;
    uintptr_t vptr = 0;
    int ret;

    for (i = 0; i < N_READER_VT; i++) {
        if (g_reader[i].orig &&
            mod_safe_read((uintptr_t)self, &vptr, sizeof(vptr)) == 0 &&
            vptr == ep122_sym(g_reader[i].sym)) {
            which = i;
            break;
        }
    }
    if (which < 0) {
        /* Unknown vptr: find any armed slot to chain through rather than
         * dropping the call, which would fail the load outright. */
        for (i = 0; i < N_READER_VT; i++)
            if (g_reader[i].orig) {
                which = i;
                break;
            }
        if (which < 0)
            return 0;
        ret = ((open_fn_t)g_reader[which].orig)(self, path, seek_table, frame_info);
        g_open_calls++;
        if (!g_decode_self)
            __atomic_store_n(&g_deck_open_ms, decode_now_ms(), __ATOMIC_RELEASE);
        return ret;
    }

    ret = ((open_fn_t)g_reader[which].orig)(self, path, seek_table, frame_info);
    g_open_calls++;
    if (!g_decode_self)
        __atomic_store_n(&g_deck_open_ms, decode_now_ms(), __ATOMIC_RELEASE);

    /* A failed open holds the capture only until one succeeds. The factory
     * falls through its readers -- FileReadWav refuses a float WAV and the JUCE
     * wrapper takes it -- and the open that succeeded is the one that names the
     * track; keeping the first would report the refusal and lose the path. */
    if (!g_cap.pending || (!g_cap.ret && ret)) {
        uintptr_t rate_fn = 0;

        g_cap.which = which;
        g_cap.ret = ret;
        g_cap.reader = (uintptr_t)self;
        g_cap.seek_table = (uintptr_t)seek_table;
        g_cap.frame_info = (uintptr_t)frame_info;
        g_cap.path_ref = (uintptr_t)path;
        cap_juce_string((uintptr_t)path, g_cap.path, sizeof(g_cap.path));
        /* Kept separately: the reporter consumes g_cap, but the decode worker
         * needs the path to survive for as long as the track is loaded.
         *
         * Never from a reader WE opened -- that is our own stem file, not the
         * track. g_cap still records it, because seeing those opens in the log
         * is how the stem decode is debugged; only the thing that decides what
         * gets uploaded is protected. */
        if (ret && g_cap.path[0] && !g_decode_self) {
            memcpy(g_track_path, g_cap.path, sizeof(g_track_path));
            /* Same open as the path: the analysis these describe is this
             * track's, and the worker hands both back to createReaderFor. */
            __atomic_store_n(&g_track_seek, (uintptr_t)seek_table, __ATOMIC_RELEASE);
            __atomic_store_n(&g_track_frame, (uintptr_t)frame_info, __ATOMIC_RELEASE);
            __atomic_store_n(&g_track_fresh, 1, __ATOMIC_RELEASE);
        }

        /* Read the field the stock getter would have returned, but only once
         * the slot has confirmed it IS that getter -- an override would mean
         * the rate lives somewhere else and +0x88 is meaningless. */
        g_cap.rate = 0;
        if (mod_safe_read(ep122_sym(g_reader[which].sym) + VT_SLOT_GETRATE, &rate_fn,
                          sizeof(rate_fn)) == 0 && rate_fn == FN_READER_GETRATE)
            mod_safe_read((uintptr_t)self + READER_RATE_OFF, &g_cap.rate,
                          sizeof(g_cap.rate));

        g_cap.pending = 1;
    }
    return ret;
}

/* Records the two createReaderFor arguments we cannot read statically, then
 * chains. A factory that does not recognise the extension returns NULL and is
 * not the one we want, so only a successful create is recorded. */
static void *stem_factory_create(void *self, const void *path, const void *cfg,
                                 uint64_t kind)
{
    uintptr_t vptr = 0;
    int which = -1, i;
    void *reader;

    if (mod_safe_read((uintptr_t)self, &vptr, sizeof(vptr)) == 0)
        for (i = 0; i < N_FACTORY_VT; i++)
            if (g_factory[i].orig && vptr == g_factory[i].vt) {
                which = i;
                break;
            }
    if (which < 0) {
        for (i = 0; i < N_FACTORY_VT; i++)
            if (g_factory[i].orig) {
                which = i;
                break;
            }
        if (which < 0)
            return NULL;
    }

    reader = ((create_fn_t)g_factory[which].orig)(self, path, cfg, kind);
    if (reader && !g_factory_seen) {
        g_factory_cfg = (uintptr_t)cfg;
        g_factory_kind = kind;
        g_factory_which = which;
        g_factory_seen = 1;
    }
    return reader;
}

/* ---- decoding a track ourselves ------------------------------------------
 *
 * Everything above is observation. This is the part that builds a second,
 * independent decode chain for the loaded track and pulls it end to end at
 * 44.1 kHz. It runs on its own thread: it opens files and decodes a whole
 * track, neither of which belongs on the audio, loader or message threads.
 */

typedef void *(*create_reader_fn_t)(void *factory, const void *path, int32_t *err,
                                    int also_open, const void *seek_table,
                                    const void *frame_info, uint64_t kind);
typedef void (*src_ctor_fn_t)(void *self);
typedef void (*src_dtor_fn_t)(void *self);
typedef void (*setsource_fn_t)(void *self, void *reader, int target_rate, int own);
typedef int  (*fileread_fn_t)(void *self, int64_t pos, void *dst, int64_t frames);
typedef void (*jstr_ctor_fn_t)(void *self, const char *utf8);
typedef void (*jstr_dtor_fn_t)(void *self);

static inline uint64_t decode_cntvct(void)
{
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

static uint64_t decode_cntfrq(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

/* Refuse to call into any of it unless the binary still looks like the one
 * these addresses were read from. The factory is the interesting check: it
 * lives in .bss and is constructed at runtime, so a zero vptr means we are
 * early enough that it does not exist yet. */
/* The AudioReaderFactory singleton, found once and remembered. Deliberately not
 * looked up in the constructor: it is a global C++ object, so its vptr does not
 * exist until static init has run, and the shim loads before that. */
static uintptr_t decode_reader_factory(void)
{
    static uintptr_t cached;

    if (!cached)
        cached = ep122_find_instance(EP122_AUDIO_READER_FACTORY);
    return cached;
}

static int decode_targets_ok(void)
{
    /* Everything here is resolved by name or read out of a vtable, so "did it
     * resolve" is the whole check: a signature match proves more than a prologue
     * guard, and the factory is identified by its class. */
    if (!ADDR_READER_FACTORY) {
        MDBG("stem_decode: AudioReaderFactory instance not found, refusing\n");
        return 0;
    }
    if (!FN_CREATE_READER || !FN_SRC_CTOR || !FN_SRC_DTOR || !FN_JSTR_CTOR ||
        !VT_SRC || !ep122_sym(EP122_SRC_SETSOURCE) ||
        !ep122_sym(EP122_SRC_FILEREAD)) {
        MDBG("stem_decode: decode chain did not resolve, refusing\n");
        return 0;
    }
    return 1;
}

/* AbstractReader is refcounted; slot +0x08 is the release createReaderFor
 * itself calls when a reader opens but the format turns out wrong. */
static void release_reader(void *reader)
{
    uintptr_t vptr = 0, fn = 0;

    if (!reader)
        return;
    if (mod_safe_read((uintptr_t)reader, &vptr, sizeof(vptr)) != 0 || !vptr)
        return;
    if (mod_safe_read(vptr + 8, &fn, sizeof(fn)) != 0 || !fn)
        return;
    ((void (*)(void *))fn)(reader);
}

/* ---- sourceId -> path ------------------------------------------------------
 *
 * "The last file the deck opened" is NOT "the track now playing", and the gap
 * between them is not rare: the page pool keeps readers per sourceId, so
 * pressing NEXT to a track it already holds re-serves it with no
 * createReaderFor at all. The open hook stays silent, g_track_path keeps
 * pointing at whatever was opened last, and the job runs against the wrong
 * track -- uploading it, keying the cache on it, and publishing stems that do
 * not belong to what is playing. That is why the faders looked dead: the set
 * was real, just for another song.
 *
 * So the path is bound to the sourceId the first time that id is seen, and
 * looked up by id afterwards. A re-served track finds its own path even though
 * nothing was opened for it.
 *
 * Small and fixed: a booth session touches a handful of tracks, and the oldest
 * binding is the right one to lose. Message thread only -- it is called from
 * the track watch, which is where both the sid and a safe moment for string
 * work exist. */
#define SID_BINDS 16

static struct {
    uint64_t lo, hi;
    char     path[PATH_MAX_CAP];
    int      used;
} g_bind[SID_BINDS];
static int g_bind_next;

const char *stem_decode_path_for_sid(uint64_t lo, uint64_t hi)
{
    int i;

    for (i = 0; i < SID_BINDS; i++)
        if (g_bind[i].used && g_bind[i].lo == lo && g_bind[i].hi == hi) {
            /* A reload of a track the pool had evicted opens it again, which
             * set the flag; this binding is that open's, so spend it here or
             * the next reader-less load inherits it and binds to this path. */
            if (__atomic_load_n(&g_track_fresh, __ATOMIC_ACQUIRE) &&
                strcmp(g_bind[i].path, g_track_path) == 0)
                __atomic_store_n(&g_track_fresh, 0, __ATOMIC_RELEASE);
            return g_bind[i].path;
        }

    /* Unseen id: it must be the track the deck just opened, because an open is
     * the only way a new source enters the pool -- but ONLY if that open
     * succeeded and left a fresh path. A float WAV (or any format this reader
     * cannot open) fails its open, so g_track_path still holds the PREVIOUS
     * track; binding this id to it publishes that song's stems over the wrong
     * one. Consume the freshness so a second new id in the same gap -- a
     * re-served track has its own binding already and never reaches here --
     * cannot inherit it either. No fresh path means no stems, and the caller
     * dropping the resident set on a NULL is what stops the last song's faders
     * from bleeding through. */
    if (!g_track_path[0] ||
        !__atomic_exchange_n(&g_track_fresh, 0, __ATOMIC_ACQ_REL))
        return NULL;

    i = g_bind_next++ % SID_BINDS;
    g_bind[i].lo = lo;
    g_bind[i].hi = hi;
    g_bind[i].used = 1;
    memcpy(g_bind[i].path, g_track_path, sizeof(g_bind[i].path));
    MDBG("stem_decode: sid %llx:%llx -> %s\n",
         (unsigned long long)hi, (unsigned long long)lo, g_bind[i].path);
    return g_bind[i].path;
}

/* Build the chain, hand every chunk to `sink`, tear it down.
 *
 * With `sink == NULL` it stops after reading the converter's output length,
 * which is how the caller learns the frame count for Content-Length without
 * paying for a second full decode.
 *
 * The count is the DECODER's and not the file's: the deck pads (4116 frames on
 * the reference track), and stems built against the file's count come back
 * misaligned with the deck's own timeline.
 *
 * Returns frames delivered (or the length, in probe mode), -1 on failure. A
 * sink returning non-zero aborts and is also reported as -1: that is how
 * cancellation reaches a thread otherwise sitting inside the deck's decoder,
 * without a signal and without pthread_cancel. */
int64_t stem_decode_pull(const char *path, int rate, stem_pcm_sink_fn sink,
                         void *user)
{
    /* juce::String is a single pointer; the second word is slack so a wrong
     * guess about its size cannot scribble on the frame. */
    uintptr_t jstr[2] = { 0, 0 };
    /* FileReadFlac ignores open()'s 2nd and 3rd arguments outright, so zeroed
     * stand-ins are enough for it. A VBR MP3 is not: its length comes out of the
     * SeekTable/FrameInfoList, and with stubs setSource reports -1 frames and the
     * job never starts. So when the deck has already opened THIS track we reuse
     * the very tables it built (g_track_seek/g_track_frame); the stubs remain the
     * fallback for a track the deck has not opened for us to observe. */
    uint8_t seek_stub[64], frame_stub[64];
    const void *seek_arg, *frame_arg;
    uintptr_t cap_seek, cap_frame;
    int32_t err = 0;
    void *reader = NULL, *src = NULL;
    float *buf = NULL;
    int64_t pos = 0, total = 0, result = -1;
    uint64_t t0, dt, frq = decode_cntfrq();
    int rc = 0, src_rate = 0, dst_rate = 0;
    int64_t in_len = 0, out_len = 0;
    uintptr_t src_vt = 0, fn_setsource = 0, fn_fileread = 0;

    if (!path || !path[0] || rate <= 0 || !decode_targets_ok())
        return -1;

    /* Everything from here to `out:` opens readers of our own, and the open hook
     * must not mistake any of them for the track. Set before the first open and
     * cleared on every exit path -- they all funnel through `out:`. */
    g_decode_self = 1;

    memset(seek_stub, 0, sizeof(seek_stub));
    memset(frame_stub, 0, sizeof(frame_stub));

    /* Reuse the deck's own analysis for this track when we have it; the stubs
     * otherwise. strcmp against g_track_path is what ties the tables to the file
     * being decoded -- a mismatch (a re-served track the deck never re-opened,
     * or our own stem files) falls back rather than handing MP3 a stranger's
     * SeekTable. */
    seek_arg = seek_stub;
    frame_arg = frame_stub;
    cap_seek = __atomic_load_n(&g_track_seek, __ATOMIC_ACQUIRE);
    cap_frame = __atomic_load_n(&g_track_frame, __ATOMIC_ACQUIRE);
    if (cap_seek && cap_frame && g_track_path[0] &&
        strcmp(path, g_track_path) == 0) {
        seek_arg = (const void *)cap_seek;
        frame_arg = (const void *)cap_frame;
        MDBG("stem_decode: reusing deck seekTable=%#lx frameInfo=%#lx\n",
             (unsigned long)cap_seek, (unsigned long)cap_frame);
    }

    if (sink)
        buf = malloc((size_t)DECODE_CHUNK * 2 * sizeof(float));
    src = malloc(SRC_SIZE);
    if ((sink && !buf) || !src) {
        MDBG("stem_decode: out of memory\n");
        goto out;
    }
    memset(src, 0, SRC_SIZE);

    ((jstr_ctor_fn_t)FN_JSTR_CTOR)(jstr, path);

    t0 = decode_cntvct();
    reader = ((create_reader_fn_t)FN_CREATE_READER)((void *)ADDR_READER_FACTORY,
                                                    jstr, &err, 1,
                                                    seek_arg, frame_arg,
                                                    (uint64_t)g_factory_kind);
    dt = decode_cntvct() - t0;
    if (!reader) {
        MDBG("stem_decode: createReaderFor failed err=%d\n", err);
        goto out_str;
    }
    MDBG("stem_decode: reader=%p err=%d open in %llu us\n", reader, err,
         (unsigned long long)(dt * 1000000ull / (frq ? frq : 1)));

    ((src_ctor_fn_t)FN_SRC_CTOR)(src);

    /* Resolve the virtual calls through the object's OWN vptr, and only after
     * confirming the constructor installed the vtable we validated. Indexing
     * the object instead of the vtable calls whatever happens to sit in a data
     * field, which is a jump to garbage. */
    memcpy(&src_vt, src, sizeof(src_vt));
    if (src_vt != VT_SRC) {
        MDBG("stem_decode: SRC vptr %#lx != %#lx after ctor, aborting\n",
             (unsigned long)src_vt, (unsigned long)VT_SRC);
        /* setSource never ran, so ownership of the reader is still ours.
         * Slot +0x08 is the release the stock factory uses on its own
         * failure path. */
        release_reader(reader);
        reader = NULL;
        goto out_str;
    }
    memcpy(&fn_setsource, (char *)src_vt + VT_SLOT_SETSOURCE, sizeof(fn_setsource));
    memcpy(&fn_fileread, (char *)src_vt + VT_SLOT_FILEREAD, sizeof(fn_fileread));

    ((setsource_fn_t)fn_setsource)(src, reader, rate, 1);

    memcpy(&src_rate, (char *)src + SRC_SRCRATE_OFF, sizeof(src_rate));
    memcpy(&dst_rate, (char *)src + SRC_DSTRATE_OFF, sizeof(dst_rate));
    memcpy(&in_len, (char *)src + SRC_INLEN_OFF, sizeof(in_len));
    memcpy(&out_len, (char *)src + SRC_OUTLEN_OFF, sizeof(out_len));
    /* Probe mode: the length is all the caller wanted. */
    if (!sink) {
        MDBG("stem_decode: %d Hz -> %d Hz, %lld frames in, %lld out (%lld s)\n",
             src_rate, dst_rate, (long long)in_len, (long long)out_len,
             (long long)(dst_rate ? out_len / dst_rate : 0));
        result = out_len;
        goto out_src;
    }

    t0 = decode_cntvct();
    while (pos < out_len) {
        int64_t want = out_len - pos;

        if (want > DECODE_CHUNK)
            want = DECODE_CHUNK;
        /* Sequential positions stay on the streaming path: fileRead compares
         * the requested position against m_currentPosition and takes a seek
         * path when they differ. */
        rc = ((fileread_fn_t)fn_fileread)(src, pos, buf, want);
        if (rc != 0) {
            MDBG("stem_decode: fileRead rc=%d, stream ended at %lld of %lld\n",
                 rc, (long long)pos, (long long)out_len);
            break;
        }
        if (sink(buf, want, user) != 0) {
            MDBG("stem_decode: aborted by sink at frame %lld\n", (long long)pos);
            goto out_src;
        }
        pos += want;
        total += want;
    }
    dt = decode_cntvct() - t0;
    {
        uint64_t ms = dt * 1000ull / (frq ? frq : 1);
        uint64_t audio_ms = dst_rate ? (uint64_t)total * 1000ull / dst_rate : 0;

        MDBG("stem_decode: pulled %lld frames in %llu ms (%llux realtime)\n",
             (long long)total, (unsigned long long)ms,
             (unsigned long long)(ms ? audio_ms / ms : 0));
    }
    result = total;

out_src:
    /* own=1 handed the reader to the converter, so destroying the converter
     * releases it too. Complete dtor (its sibling at slot 1 is the deleting
     * one), then free what we malloc'd. */
    ((src_dtor_fn_t)FN_SRC_DTOR)(src);
    reader = NULL;

out_str:
    ((jstr_dtor_fn_t)FN_JSTR_DTOR)(jstr);
out:
    g_decode_self = 0;
    free(buf);
    free(src);
    return result;
}

/* Message thread. Prints a capture once, then releases the slot for the next
 * track load. */
void mod_stem_decode_report(void)
{
    static unsigned last_calls;
    static int factory_logged;

    if (g_factory_seen && !factory_logged) {
        factory_logged = 1;
        MDBG("stem_decode: factory %s create cfg=%#lx kind=%#llx\n",
             g_factory[g_factory_which].name, (unsigned long)g_factory_cfg,
             (unsigned long long)g_factory_kind);
    }

    if (!g_cap.pending) {
        if (g_open_calls != last_calls) {
            last_calls = g_open_calls;
            MDBG("stem_decode: %u open() calls, none captured\n", g_open_calls);
        }
        return;
    }

    MDBG("stem_decode: %s open -> %d  rate=%d  reader=%#lx\n",
         g_reader[g_cap.which].name, g_cap.ret, g_cap.rate,
         (unsigned long)g_cap.reader);
    MDBG("stem_decode:   path=\"%s\"\n", g_cap.path);
    MDBG("stem_decode:   seekTable=%#lx frameInfo=%#lx (open calls %u)\n",
         (unsigned long)g_cap.seek_table, (unsigned long)g_cap.frame_info,
         g_open_calls);

    last_calls = g_open_calls;
    g_cap.pending = 0;
}

static int stem_decode_install(void)
{
    char name[64];
    int i, armed = 0, readers_armed;

    /* Cache each class's vtable once: the wrappers compare a live object's vptr
     * against these on the page-filler thread, where a table walk per call would
     * be gratuitous. A class that did not resolve leaves 0 and is skipped. */
    for (i = 0; i < N_READER_VT; i++) {
        g_reader[i].vt = ep122_sym(g_reader[i].sym);
        if (!g_reader[i].vt)
            continue;
        snprintf(name, sizeof(name), "readerOpen:%s", g_reader[i].name);
        if (mod_patch_vslot(name, g_reader[i].sym, VT_SLOT_OPEN,
                            (void *)stem_reader_open, &g_reader[i].orig) != 0)
            g_reader[i].orig = 0;
        else
            armed++;
    }
    MDBG("stem_decode: %d/%d reader open slots armed\n", armed, N_READER_VT);
    readers_armed = armed;

    armed = 0;
    for (i = 0; i < N_FACTORY_VT; i++) {
        g_factory[i].vt = ep122_sym(g_factory[i].sym);
        if (!g_factory[i].vt)
            continue;
        snprintf(name, sizeof(name), "factoryCreate:%s", g_factory[i].name);
        if (mod_patch_vslot(name, g_factory[i].sym, VT_SLOT_FACTORY_CREATE,
                            (void *)stem_factory_create,
                            &g_factory[i].orig) != 0)
            g_factory[i].orig = 0;
        else
            armed++;
    }
    MDBG("stem_decode: %d/%d factory create slots armed\n", armed,
         N_FACTORY_VT);

    /* Both halves have to be present, but neither has to be complete: a track
     * arrives through ONE reader class and ONE factory, so arming a subset costs
     * only the formats whose class did not arm. Zero on either side is different
     * -- there is then no path from a track to our own decode at all. */
    return (readers_armed && armed) ? 0 : -1;
}

KIT_MOD(k_mod_stem_decode,
        .name = "stem_decode", .prio = 50, .install = stem_decode_install,
        .what = "reader open() capture for our own 44.1k decode");
