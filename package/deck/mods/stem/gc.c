// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * stem/gc.c - GROOVE CIRCUIT: eight slots, each replacing one stem with a file.
 *
 * Put loops on the stick and name them in mods/gc/gc-config.txt:
 *
 *     # audio path, slot, stem, bpm
 *     loops/hard-kick.wav,  1, d, 124
 *     Contents/pads/Am.wav, 4, h, 124
 *     loops/shout.wav,      8, v, 90
 *
 * Slot 1..8 is pad A..H and the letter says which stem is replaced: d drums,
 * h harmonics, v vocals. A pad with no entry stays a normal hot cue.
 *
 * A config list instead of a file naming rule lets a loop stay in its own
 * folder, be used by two slots, and carry its BPM.
 *
 * One slot at a time across the whole feature: arming a slot disarms the
 * previous one, even for a different stem.
 *
 * A file, not a region of the track: a region could only be a stem the deck
 * already has, which excludes drums (the residual exists only at the play head)
 * and anything the track does not contain. A file costs a few MB.
 *
 * First bank only: slots belong to the deck, not a track, so there is no track
 * to decide between two sticks (see stem_media_first_root).
 *
 * Threading. [worker] scans and decodes and is the only writer of the table;
 * [audio] reads it under the same acquire/release discipline the stems use, and
 * [deck] arms a slot. Nothing here allocates on the audio thread.
 */
#include "stem/stem.h"
#include "lamp/lamp.h"
#include "cue/cue.h"
#include "kit/mod.h"

/* ---- what a slot is ------------------------------------------------------- */

/* Our own subdirectory, so it cannot collide with the DJ's folders; a stick
 * without `mods` has no slots. */
#define GC_DIR          "mods/gc"
#define GC_CONFIG       GC_DIR "/gc-config.txt"
#define GC_PATH_MAX     STEM_CACHE_PATH_MAX
#define GC_LINE_MAX     512

/* Cap per loop, so a mis-named full track cannot take the stems' memory: at the
 * pool's 96 kHz stereo s16 this is about 11 MB a slot, 92 MB for all eight,
 * next to ~350 MB of stems. Longer files are truncated, not
 * refused, since a truncated loop still plays. */
#define GC_MAX_SECONDS  30

struct gc_slot {
    int16_t *pcm;        /* interleaved stereo at the pool rate, NULL if empty */
    int64_t  frames;     /* what the decoder produced, padding included        */
    int64_t  span;       /* what the loop is -- whole beats when a BPM is known*/
    double   spb;        /* samples a beat at the pool rate, 0 without a BPM   */
    int      part;       /* STEM_PART_* -- which stem this one stands in for   */
};

static struct gc_slot gc_g_slot[CUE_PADS];

/* What the table was built for; a change of either triggers a re-scan. */
static char gc_g_root[GC_PATH_MAX];
static int  gc_g_rate;

/* Readers in the mix, so a scan cannot free a buffer in use. Same scheme as the
 * stem store: the audio thread never waits and the worker never frees early. */
static int gc_g_readers;
static int gc_g_live;

/* The armed slot and its phase anchor. `active` is published last and cleared
 * first: a reader that sees a slot sees the position written before it, and one
 * that catches a writer mid-update sees -1 and mixes the track for a block.
 *
 * The file's index is (track pos - engage) at the loop's tempo, wrapped over its
 * length, so there is no cursor to drift and a seek moves the loop with the
 * track.
 *
 * `engage` is the grid's first downbeat, not the moment of the press, so the
 * loop's downbeat lands on the track's. A track with no readable grid falls
 * back to the play head.
 *
 * [deck] writes, [audio] and [message] read. */
static int64_t gc_g_engage;
static int     gc_g_active = -1;

/* ---- the table, as the mix sees it ---------------------------------------- */

int gc_acquire(int slot, struct gc_view *out)
{
    if (slot < 0 || slot >= CUE_PADS)
        return 0;
    __atomic_fetch_add(&gc_g_readers, 1, __ATOMIC_ACQ_REL);
    if (!__atomic_load_n(&gc_g_live, __ATOMIC_ACQUIRE) || !gc_g_slot[slot].pcm) {
        __atomic_fetch_sub(&gc_g_readers, 1, __ATOMIC_ACQ_REL);
        return 0;
    }
    out->pcm    = gc_g_slot[slot].pcm;
    out->frames = gc_g_slot[slot].frames;
    out->span   = gc_g_slot[slot].span;
    out->spb    = gc_g_slot[slot].spb;
    out->part   = gc_g_slot[slot].part;
    return 1;
}

void gc_release(void)
{
    __atomic_fetch_sub(&gc_g_readers, 1, __ATOMIC_ACQ_REL);
}

/* The single "this slot can run" test, used by both the pad claim and the lamp
 * so they never disagree. The stems must be resident, since every replacement
 * is expressed against the track's harmonics and vocals.
 *
 * Gated on the STEMS row: otherwise the slots would take the hot cues for as
 * long as the stick is in. With the row closed every pad is stock; open, the
 * pads belong to the circuit.
 *
 * The gate applies to the pads, not the circuit: closing the row leaves a
 * running replacement running, like the stem levels. So this returns -1 even
 * for the slot currently replacing a stem, since the pad cannot toggle it while
 * the row is closed; the replacement shows on the row (and the title bar badge,
 * see gc_active_part) instead. */
int gc_slot_part(int slot)
{
    if (slot < 0 || slot >= CUE_PADS)
        return -1;
    if (!stems_row_open())
        return -1;
    if (!__atomic_load_n(&gc_g_live, __ATOMIC_ACQUIRE) || !gc_g_slot[slot].pcm)
        return -1;
    if (!stem_store_complete())
        return -1;
    return gc_g_slot[slot].part;
}

int gc_active_slot(void)
{
    return __atomic_load_n(&gc_g_active, __ATOMIC_ACQUIRE);
}

/* ---- the pads' colours ---------------------------------------------------
 *
 * A slot's colour is the stem it replaces: red drums, blue harmonics, green
 * vocals, indexed by STEM_PART_*. Unlit when gc_slot_part() returns -1 (row
 * closed, no file, or no stems resident), the same test the press uses.
 *
 * The blink uses the monotonic clock, not a draw count, because the lamp's
 * redraw rate varies with deck load. */
#define GC_BLINK_MS 350

static const uint8_t k_gc_part_hue[N_STEMS][3] = {
    { 255,   0,   0 },      /* STEM_PART_DRUMS     */
    {   0,   0, 255 },      /* STEM_PART_HARMONICS */
    {   0, 255,   0 },      /* STEM_PART_VOCALS    */
};

int gc_pad_lamp(int pad, struct lamp *out)
{
    int part = gc_slot_part(pad);

    if (part < 0 || part >= N_STEMS)
        return 0;
    if (pad == gc_active_slot() && ((lamp_now_ms() / GC_BLINK_MS) & 1)) {
        lamp_dark(out);                 /* the dark half of the blink */
        return 1;
    }
    lamp_set(out, k_gc_part_hue[part][0], k_gc_part_hue[part][1],
             k_gc_part_hue[part][2], LAMP_LIT);
    return 1;
}

/* Which stem the running replacement stands in for, or -1 when none is running.
 *
 * Not gated on the row, unlike gc_slot_part: it drives the title bar badge,
 * the only indicator of a running replacement while the row is closed. */
int gc_active_part(void)
{
    int slot = __atomic_load_n(&gc_g_active, __ATOMIC_ACQUIRE);

    if (slot < 0 || slot >= CUE_PADS || !__atomic_load_n(&gc_g_live, __ATOMIC_ACQUIRE))
        return -1;
    return gc_g_slot[slot].pcm ? gc_g_slot[slot].part : -1;
}

int gc_active(int64_t *engage)
{
    int slot = __atomic_load_n(&gc_g_active, __ATOMIC_ACQUIRE);

    if (slot >= 0)
        *engage = gc_g_engage;
    return slot;
}

void gc_arm(int slot, int64_t engage)
{
    __atomic_store_n(&gc_g_active, -1, __ATOMIC_RELEASE);
    gc_g_engage = engage;
    __atomic_store_n(&gc_g_active, slot, __ATOMIC_RELEASE);
}

void gc_disarm(void)
{
    __atomic_store_n(&gc_g_active, -1, __ATOMIC_RELEASE);
}

/* ---- loading (worker) ----------------------------------------------------- */

/* One decode's state. Float chunks are converted to s16 as they arrive, so the
 * peak is the s16 buffer plus one chunk, not a float copy of the file. */
struct gc_load {
    int16_t *pcm;
    int64_t  cap;        /* frames the buffer holds */
    int64_t  n;          /* frames written so far   */
};

static int gc_sink(const float *in, int64_t frames, void *user)
{
    struct gc_load *l = user;
    int64_t i, take = frames;

    if (l->n + take > l->cap)
        take = l->cap - l->n;
    for (i = 0; i < take * 2; i++) {
        float v = in[i] * 32767.0f;

        /* Clipped, not scaled: the level of the DJ's file is theirs to set. */
        if (v >  32767.0f) v =  32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        l->pcm[l->n * 2 + i] = (int16_t)v;
    }
    l->n += take;
    return take < frames;          /* full -> stop the decoder */
}

static int gc_part_of(char c)
{
    switch (c) {
    case 'd': case 'D': return STEM_PART_DRUMS;
    case 'h': case 'H': return STEM_PART_HARMONICS;
    case 'v': case 'V': return STEM_PART_VOCALS;
    default:            return -1;
    }
}

static const char *gc_part_name(int part)
{
    switch (part) {
    case STEM_PART_DRUMS:     return "drums";
    case STEM_PART_HARMONICS: return "harmonics";
    case STEM_PART_VOCALS:    return "vocals";
    default:                  return "?";
    }
}

/* Decode one file into `s`. Returns 1 when the slot ends up playable. */
static int gc_load_one(const char *path, int rate, int part, float bpm,
                       struct gc_slot *s)
{
    struct gc_load l;
    int64_t frames;

    /* The decoder's frame count, which includes the deck chain's padding; that
     * is what the buffer must hold. */
    frames = stem_decode_pull(path, rate, NULL, NULL);
    if (frames <= 0) {
        MDBG("gc: %s will not decode\n", path);
        return 0;
    }
    if (frames > (int64_t)rate * GC_MAX_SECONDS) {
        MDBG("gc: %s is %lld frames, truncated to %d s\n",
             path, (long long)frames, GC_MAX_SECONDS);
        frames = (int64_t)rate * GC_MAX_SECONDS;
    }

    l.pcm = malloc((size_t)frames * 2 * sizeof(int16_t));
    if (!l.pcm) {
        MDBG("gc: no memory for %lld frames of %s\n", (long long)frames, path);
        return 0;
    }
    l.cap = frames;
    l.n   = 0;

    if (stem_decode_pull(path, rate, gc_sink, &l) < 0 || l.n <= 0) {
        MWARN("gc: %s decoded nothing\n", path);
        free(l.pcm);
        return 0;
    }
    s->pcm    = l.pcm;
    s->frames = l.n;
    s->part   = part;
    s->spb    = 0.0;
    s->span   = l.n;

    /* The loop length is a whole number of beats, not the buffer: the buffer
     * includes decoder padding (an encoder delay is tens of milliseconds, a gap
     * at the end of every bar). With a stated BPM, rounding to the nearest whole
     * beat puts the wrap on the beat and leaves the padding unread.
     *
     * Nearest, not floor: padding makes the buffer longer than the loop, so
     * floor would drop a real beat when padding exceeds half a beat. */
    if (bpm > 0.0f) {
        double spb = (double)rate * 60.0 / (double)bpm;
        int64_t beats = (int64_t)((double)l.n / spb + 0.5);
        int64_t span;

        if (beats < 1)
            beats = 1;
        span = (int64_t)((double)beats * spb + 0.5);
        if (span > l.n)
            span = l.n;              /* a BPM that says the file is longer */
        s->spb  = spb;
        s->span = span;
    }
    return 1;
}

/* Drop the table and wait for the mix to let go of it. */
static void gc_unload(void)
{
    int i;

    __atomic_store_n(&gc_g_live, 0, __ATOMIC_RELEASE);
    while (__atomic_load_n(&gc_g_readers, __ATOMIC_ACQUIRE) > 0)
        usleep(1000);
    for (i = 0; i < CUE_PADS; i++) {
        free(gc_g_slot[i].pcm);
        gc_g_slot[i].pcm    = NULL;
        gc_g_slot[i].frames = 0;
        gc_g_slot[i].part   = -1;
        gc_g_slot[i].span   = 0;
        gc_g_slot[i].spb    = 0.0;
    }
    gc_disarm();
}

/* ---- the config file ------------------------------------------------------
 *
 *     # audio path, slot, stem, bpm
 *     loops/hard-kick.wav, 1, d, 124
 *
 * Comma-separated, so paths may contain spaces without quoting. `#` starts a
 * comment to end of line, blank lines are skipped, and whitespace around each
 * field is trimmed.
 *
 * A relative path is relative to the volume root, not mods/gc, since the audio
 * usually sits with the DJ's tracks. An absolute path is used as given.
 *
 * BPM may be omitted or 0, meaning the loop's tempo is not stated. */

static char *gc_trim(char *p)
{
    char *e;

    while (*p == ' ' || *p == '\t') p++;
    e = p + strlen(p);
    while (e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
        *--e = '\0';
    return p;
}

/* Split `line` on commas into at most `max` trimmed fields. Returns the count. */
static int gc_split(char *line, char **out, int max)
{
    int n = 0;

    while (n < max) {
        char *c = strchr(line, ',');

        if (c) *c = '\0';
        out[n++] = gc_trim(line);
        if (!c) break;
        line = c + 1;
    }
    return n;
}

/* One config line into `gc_g_slot`. Returns 1 when a slot ended up playable. */
static int gc_config_line(const char *root, char *line, int rate)
{
    char *f[4], path[GC_PATH_MAX];
    char *hash = strchr(line, '#');
    int nf, slot, part;
    float bpm;

    if (hash) *hash = '\0';
    if (!*gc_trim(line))
        return 0;

    nf = gc_split(line, f, 4);
    if (nf < 3) {
        MDBG("gc: \"%s\" needs at least path, slot and stem -> ignored\n", line);
        return 0;
    }
    slot = atoi(f[1]);
    part = gc_part_of(f[2][0]);
    bpm  = nf >= 4 ? (float)atof(f[3]) : 0.0f;
    if (slot < 1 || slot > CUE_PADS) {
        MDBG("gc: slot %d is not 1..%d -> ignored\n", slot, CUE_PADS);
        return 0;
    }
    if (part < 0) {
        MDBG("gc: \"%s\" is not d, h or v -> slot %d ignored\n", f[2], slot);
        return 0;
    }
    if (gc_g_slot[slot - 1].pcm) {
        MDBG("gc: slot %d named twice -> the later entry is ignored\n", slot);
        return 0;
    }
    if ((size_t)(f[0][0] == '/'
                 ? snprintf(path, sizeof(path), "%s", f[0])
                 : snprintf(path, sizeof(path), "%s/%s", root, f[0]))
        >= sizeof(path)) {
        MDBG("gc: slot %d path is too long -> ignored\n", slot);
        return 0;
    }
    if (!gc_load_one(path, rate, part, bpm, &gc_g_slot[slot - 1]))
        return 0;

    /* Log both lengths: a gap between the beat span and the decoded frames
     * larger than a few milliseconds of padding means a wrong BPM. */
    if (bpm > 0.0f)
        MDBG("gc: slot %d = %s (%s, %.1f BPM, %.2f beats -> %lld of %lld frames)\n",
             slot, path, gc_part_name(part), (double)bpm,
             (double)gc_g_slot[slot - 1].frames / rate * bpm / 60.0,
             (long long)gc_g_slot[slot - 1].span,
             (long long)gc_g_slot[slot - 1].frames);
    else
        MDBG("gc: slot %d = %s (%s, %lld frames, no BPM stated)\n",
             slot, path, gc_part_name(part),
             (long long)gc_g_slot[slot - 1].frames);
    return 1;
}

/* Rebuild the table from `root`. Worker thread. */
static void gc_scan(const char *root, int rate)
{
    char path[GC_PATH_MAX], line[GC_LINE_MAX];
    FILE *fp;
    int n = 0;

    gc_unload();
    if ((size_t)snprintf(path, sizeof(path), "%s/%s", root, GC_CONFIG) >= sizeof(path))
        return;
    fp = fopen(path, "r");
    if (!fp) {
        /* No config means no slots, the usual case. Logged once per scan so
         * uncoloured pads can be told apart from a bug. */
        MDBG("gc: no %s -> no slots\n", path);
        snprintf(gc_g_root, sizeof(gc_g_root), "%s", root);
        gc_g_rate = rate;
        __atomic_store_n(&gc_g_live, 1, __ATOMIC_RELEASE);
        return;
    }
    while (fgets(line, sizeof(line), fp))
        n += gc_config_line(root, line, rate);
    fclose(fp);

    snprintf(gc_g_root, sizeof(gc_g_root), "%s", root);
    gc_g_rate = rate;
    __atomic_store_n(&gc_g_live, 1, __ATOMIC_RELEASE);
    MDBG("gc: %d slot%s loaded from %s at %d Hz\n",
         n, n == 1 ? "" : "s", path, rate);
}

/* Called from the worker's idle branch. Cheap when nothing changed: a rate
 * compare and a string compare. */
void mod_stem_gc_poll(void)
{
    char root[GC_PATH_MAX];
    int rate = stem_pool_rate();

    if (rate <= 0)
        return;                     /* no timeline to decode onto yet */
    if (!stem_media_first_root(root, sizeof(root))) {
        if (gc_g_root[0]) {
            MDBG("gc: media gone -> slots dropped\n");
            gc_unload();
            gc_g_root[0] = '\0';
            gc_g_rate = 0;
        }
        return;
    }
    if (rate == gc_g_rate && strcmp(root, gc_g_root) == 0)
        return;
    gc_scan(root, rate);
}

/* ---- the gesture (deck) --------------------------------------------------- */

/* Claim a press on a pad whose slot can run (gc_slot_part); every other pad
 * stays a hot cue. */
static int gc_claim(const struct cue_event *ev)
{
    return gc_slot_part(ev->pad) >= 0;
}

static void gc_pad(const struct cue_event *ev, enum cue_phase phase)
{
    int64_t at;

    /* The press does everything; the release is ignored. */
    if (phase != CUE_PAD_PRESSED)
        return;

    if (ev->pad == gc_active_slot()) {
        gc_disarm();
        MDBG("gc: slot %d off -> back with the track\n", ev->pad + 1);
        return;
    }
    if (gc_active_slot() >= 0)
        MDBG("gc: slot %d takes over from slot %d\n",
             ev->pad + 1, gc_active_slot() + 1);

    /* Read the grid here: a pad press is when a cue slot is reachable, and the
     * grid is only needed while a slot is armed. Re-read on every arm, which
     * costs a few reads and can never return the previous track's grid. */
    stem_grid_take(ev);

    /* Anchor the phase to the grid's first downbeat, not the press: anchoring to
     * the play head would offset the bar by however late the press was. The
     * phase is then beats into the track modulo the loop, so every press,
     * re-press and takeover lands in the same alignment. A press therefore does
     * not restart the loop from its first frame.
     *
     * Without a grid, the play head is the only reference. */
    at = stem_grid_spb() > 0.0 ? stem_grid_beat0() : stem_source_pos();
    if (at < 0)
        at = 0;                     /* nothing has been read yet; phase from 0 */
    gc_arm(ev->pad, at);

    /* Log both tempos to tell a wrong BPM in the config from an unreadable
     * track grid. */
    {
        double file_spb  = gc_g_slot[ev->pad].spb;
        double track_spb = stem_grid_spb();

        if (file_spb > 0.0 && track_spb > 0.0)
            MDBG("gc: slot %d on -> %s from the file, %.1f into %.1f BPM, "
                 "on the grid from %lld\n",
                 ev->pad + 1, gc_part_name(gc_slot_part(ev->pad)),
                 (double)gc_g_rate * 60.0 / file_spb,
                 (double)gc_g_rate * 60.0 / track_spb, (long long)at);
        else
            MDBG("gc: slot %d on -> %s from the file, at %lld, at its own tempo "
                 "(%s)\n", ev->pad + 1, gc_part_name(gc_slot_part(ev->pad)),
                 (long long)at,
                 file_spb > 0.0 ? "the track's tempo is not known"
                                : "no BPM in the config");
    }
}

/* Ahead of the handlers that react to the deck's press (gate at 10, smart at
 * 20, preview at 30), since this one replaces the press. */
CUE_HANDLER(k_cue_gc,
            .name = "groove", .prio = 5,
            .pad = gc_pad, .pad_claim = gc_claim);

static int gc_install(void)
{
    if (!cue_pad_ready()) {
        MDBG("gc: no cue interception -> GROOVE CIRCUIT unavailable\n");
        return -1;
    }
    return 0;
}

KIT_MOD(k_mod_stem_gc,
        .name = "stem_gc", .prio = 62, .install = gc_install,
        .what = "groove circuit: a file in place of a stem, eight slots");

/* ---- what takes a slot off ------------------------------------------------
 *
 * Besides the pad itself and a re-scan (gc_unload), only a track change
 * (stem_job_set_track): a replacement is tied to a track's stems and grid.
 *
 * Pausing does not disarm a slot; the pad and lamp show what is engaged.
 *
 * There is no play-state call here, and the play head cannot stand in for one:
 * stem_source_pos() is the stretcher's last read position and also stops
 * whenever the audio thread stalls (e.g. during a resample on load), so using
 * it to detect "stopped" would disarm slots spuriously. */
