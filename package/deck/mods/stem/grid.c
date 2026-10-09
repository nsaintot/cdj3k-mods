// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * stem/grid.c - the loaded track's beat grid, reduced to samples per beat.
 *
 * With the beat length, a loop recorded at 124 BPM plays in time under a 126 BPM
 * track; without it, the loop runs at its own tempo and drifts within a bar.
 *
 * Sources. The deck's own beat-grid reply is preferred (see stem_grid_take).
 * The fallback is a cue slot, which embeds a pcmbuf::PositionWithSourceInfo
 * whose last field is the page source the position belongs to. The deck's cue
 * quantiser walks it to reach the grid (from setAt), and this walks the same
 * chain:
 *
 *     pwsi + 0x28    ->  source     the page source, a meow::RefCountedObjEx
 *     source + 0x68  ->  holder     the grid as this source publishes it
 *     holder + 0x28  ->  appnd_trk_info::BeatGrid::Content
 *     holder + 0x30  ->  origin, added to every beat position
 *     holder + 0x40  ->  the rate those positions are counted at
 *     content + 0x28 ->  the beats, 16 bytes each: int64 position, then that
 *                        beat's BPM as a DOUBLE (see BEAT_STRIDE)
 *     content + 0x30 ->  how many
 *
 * The audio thread's block position cannot be used: read() takes a range of two
 * bare pcmbuf::Position, which carry a source ID, not the source. The cue table
 * keeps the longer form and is already on a pad press's path.
 *
 * The first and last beats and the count give the average beat length, exact
 * for a fixed tempo. For a tempo that moves, the whole beat array is also
 * copied and published (see GRID_BEATS_MAX in grid_internal.h).
 *
 * Nothing read here is trusted. The pointers are the app's, several links deep,
 * read while it plays: every step goes through mod_safe_read, objects are
 * checked against the tag the app writes into them, and the result is refused
 * unless the rate is positive, the beats are ordered and the tempo is in
 * range. A refused grid means loops play at their own tempo.
 *
 * Threading. [message] receives replies and arms from the tick; [deck] reads
 * cue slots when a pad is pressed. [audio] loads one double and the beat
 * array.
 */
#include "stem/grid_internal.h"
#include "cue/cue.h"
#include "db/db.h"
#include "kit/mod.h"









/* Pool-rate samples per beat, as IEEE bits so the load is one aligned integer.
 * 0 means unknown (also 0.0 as a double). [deck] writes, [audio] reads. */
static int64_t grid_g_spb_bits;

/* The grid's first downbeat on the pool timeline. Read by [deck] when a slot is
 * armed; the mix sees it only as the engage position gc publishes. */
static int64_t grid_g_beat0;

/* The loaded track's grid holder, remembered wherever one is walked
 * (grid_read, grid_wrap_reply) so an edit in grid_edit.c does not need a cue
 * slot. [deck] */
uintptr_t grid_g_holder;

/* The track that holder is for, or 0/0 if unknown.
 *
 * An edit only needs the holder, but saving names a track. A holder reached
 * through a cue slot has no id (the slot may hold the previous track's
 * source); only a holder from a reply, which carries its TrackID, can be saved.
 *
 * 0/0 is never a real id: registerBeatGrid's isValid() refuses one whose first
 * byte is zero. [deck] and [message] both write it together with
 * grid_g_holder. */
uint64_t grid_g_holder_tid_lo, grid_g_holder_tid_hi;



/* Left by [message], taken by [deck]. NULL when there is nothing new. */
static struct grid_beats *grid_g_pending;


/* Replies kept per track (see GRID_REPLY_SLOTS).
 *
 * Only loading a track produces a reply; browsing and selecting a row do not
 * (whether auditioning with PREVIEW does is unknown). The TrackID that comes
 * with a reply is the pool sourceId as the same two little-endian words: sid
 * 1010200000001:b arrives as 01000000 02010100 0b000000 00000000. A reply is matched to the loaded
 * track by comparing the two, so a stale one is never used. */
struct grid_reply {
    uint64_t tid_lo, tid_hi;    /* 0/0 for an empty slot */
    int64_t  spb_bits;          /* as IEEE bits, like the published one */
    int64_t  beat0;
    int      rate;
    /* Our copy of its beats at the grid's own rate, so a reloaded track still
     * gets the array route. A couple of thousand beats per long track, so all
     * sixteen slots cost a few hundred kilobytes at worst. */
    struct grid_beats *beats;
};

static struct grid_reply grid_g_replies[GRID_REPLY_SLOTS];
static int grid_g_reply_next;

/* Duplicate a beats array, so a slot and the mix each own one: the published
 * array is converted to the pool rate in place, while the slot keeps the
 * reply's rate. */
static struct grid_beats *grid_beats_dup(const struct grid_beats *b)
{
    struct grid_beats *d;
    size_t n;

    if (!b || b->count <= 0)
        return NULL;
    n = sizeof(*b) + (size_t)b->count * sizeof(b->pos[0]);
    d = malloc(n);
    if (d)
        memcpy(d, b, n);
    return d;
}

/* [message] Keep this track's answer, reusing its own slot if it has one so a
 * frequently reloaded track does not evict the others. */
static void grid_reply_keep(uint64_t lo, uint64_t hi, int64_t bits,
                            int64_t beat0, int rate,
                            const struct grid_beats *beats)
{
    struct grid_reply *r;
    int i;

    if (!lo && !hi)
        return;
    for (i = 0; i < GRID_REPLY_SLOTS; i++)
        if (grid_g_replies[i].tid_lo == lo && grid_g_replies[i].tid_hi == hi)
            break;
    if (i == GRID_REPLY_SLOTS) {
        i = grid_g_reply_next;
        grid_g_reply_next = (i + 1) % GRID_REPLY_SLOTS;
    }
    r = &grid_g_replies[i];
    __atomic_store_n(&r->tid_lo, (uint64_t)0, __ATOMIC_RELEASE);
    __atomic_store_n(&r->tid_hi, (uint64_t)0, __ATOMIC_RELAXED);
    free(r->beats);
    r->beats = grid_beats_dup(beats);
    __atomic_store_n(&r->spb_bits, bits, __ATOMIC_RELAXED);
    __atomic_store_n(&r->beat0, beat0, __ATOMIC_RELAXED);
    __atomic_store_n(&r->rate, rate, __ATOMIC_RELAXED);
    __atomic_store_n(&r->tid_hi, hi, __ATOMIC_RELAXED);
    __atomic_store_n(&r->tid_lo, lo, __ATOMIC_RELEASE);
}

/* Put this track's kept beats where an arm will find them, if nothing fresher
 * is already staged. Called from inside the arm, which is serialised. */
static int grid_reply_stage_beats(uint64_t lo, uint64_t hi)
{
    struct grid_beats *d;
    int i;

    if (__atomic_load_n(&grid_g_pending, __ATOMIC_ACQUIRE))
        return 0;                       /* a live reply's copy is newer */
    for (i = 0; i < GRID_REPLY_SLOTS; i++) {
        struct grid_reply *r = &grid_g_replies[i];

        if (__atomic_load_n(&r->tid_lo, __ATOMIC_ACQUIRE) != lo ||
            __atomic_load_n(&r->tid_hi, __ATOMIC_RELAXED) != hi || !r->beats)
            continue;
        d = grid_beats_dup(r->beats);
        if (!d)
            return 0;
        free(__atomic_exchange_n(&grid_g_pending, d, __ATOMIC_ACQ_REL));
        return 1;
    }
    return 0;
}

/* [any] This track's answer, or 0 if no reply for it has been seen. */
static int grid_reply_find(uint64_t lo, uint64_t hi, struct grid_reply *out)
{
    int i;

    if (!lo && !hi)
        return 0;
    for (i = 0; i < GRID_REPLY_SLOTS; i++) {
        struct grid_reply *r = &grid_g_replies[i];

        if (__atomic_load_n(&r->tid_lo, __ATOMIC_ACQUIRE) != lo ||
            __atomic_load_n(&r->tid_hi, __ATOMIC_RELAXED) != hi)
            continue;
        out->spb_bits = __atomic_load_n(&r->spb_bits, __ATOMIC_RELAXED);
        out->beat0    = __atomic_load_n(&r->beat0, __ATOMIC_RELAXED);
        out->rate     = __atomic_load_n(&r->rate, __ATOMIC_RELAXED);
        /* Still this track's after the read, or the slot was taken under us. */
        if (__atomic_load_n(&r->tid_lo, __ATOMIC_ACQUIRE) != lo ||
            __atomic_load_n(&r->tid_hi, __ATOMIC_RELAXED) != hi)
            continue;
        return 1;
    }
    return 0;
}

/* What the mix reads: the array converted to pool samples. Readers are
 * counted so a publish cannot free one mid-read, as in the stem store and the
 * groove circuit. */
static struct grid_beats *grid_g_beats;
static int grid_g_beats_readers;
static int grid_g_beats_live;

/* The pool rate the live array was converted for. The raw positions are gone
 * after conversion, so on a rate change the array is rebuilt or dropped (see
 * grid_beats_for). */
static int grid_g_beats_rate;

int stem_grid_beats_acquire(struct stem_grid_view *out)
{
    __atomic_fetch_add(&grid_g_beats_readers, 1, __ATOMIC_ACQ_REL);
    if (!__atomic_load_n(&grid_g_beats_live, __ATOMIC_ACQUIRE) ||
        !grid_g_beats || grid_g_beats->count < 2) {
        __atomic_fetch_sub(&grid_g_beats_readers, 1, __ATOMIC_ACQ_REL);
        return 0;
    }
    out->beats = grid_g_beats->pos;
    out->count = grid_g_beats->count;
    return 1;
}

void stem_grid_beats_release(void)
{
    __atomic_fetch_sub(&grid_g_beats_readers, 1, __ATOMIC_ACQ_REL);
}

double stem_grid_spb(void)
{
    int64_t bits = __atomic_load_n(&grid_g_spb_bits, __ATOMIC_RELAXED);
    double spb;

    memcpy(&spb, &bits, sizeof(spb));
    return spb;
}

int64_t stem_grid_beat0(void)
{
    return __atomic_load_n(&grid_g_beat0, __ATOMIC_RELAXED);
}

static void grid_publish(double spb, int64_t beat0)
{
    int64_t bits;

    memcpy(&bits, &spb, sizeof(bits));
    __atomic_store_n(&grid_g_beat0, beat0, __ATOMIC_RELAXED);
    __atomic_store_n(&grid_g_spb_bits, bits, __ATOMIC_RELAXED);
}

/* Take the array out of the mix's reach, instantly. Not freed, since freeing
 * waits for readers; the next publish frees it. [any] */
static void grid_beats_drop(void)
{
    __atomic_store_n(&grid_g_beats_live, 0, __ATOMIC_RELEASE);
}

/* Which track and rate the published grid is for, so a repeat arm is cheap. */
static uint64_t grid_g_armed_lo, grid_g_armed_hi;
static int      grid_g_armed_rate;
static int      grid_g_arming;

void stem_grid_forget(void)
{
    /* Drop only the published answer, its array and the armed latch. The reply
     * cache is keyed by TrackID and stays: the new track's reply arrives before
     * the change is seen. The latch must be cleared, or grid_arm_reply would
     * keep answering "already armed" for the new track after it was dropped. */
    grid_beats_drop();
    grid_publish(0.0, 0);
    __atomic_store_n(&grid_g_armed_lo, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&grid_g_armed_hi, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&grid_g_armed_rate, 0, __ATOMIC_RELAXED);
}


/* Make `b` the array the mix reads, converted to pool samples. Takes ownership
 * of it and of whatever it replaces. [deck] */
static void grid_publish_beats(struct grid_beats *b, int pool_rate)
{
    struct grid_beats *old = grid_g_beats;
    int32_t i;

    for (i = 0; i < b->count; i++)
        b->pos[i] = grid_to_pool(b->pos[i], pool_rate, b->rate);
    b->rate = pool_rate;

    __atomic_store_n(&grid_g_beats_live, 0, __ATOMIC_RELEASE);
    while (__atomic_load_n(&grid_g_beats_readers, __ATOMIC_ACQUIRE) > 0)
        usleep(1000);
    grid_g_beats      = b;
    grid_g_beats_rate = pool_rate;
    free(old);
    __atomic_store_n(&grid_g_beats_live, 1, __ATOMIC_RELEASE);
}

/* Point the mix's array at the loaded track, if it is not there already.
 *
 * `holder` is the one the cue walk reached, or the reply's holder, or 0; a
 * reply's array is copied when it lands and waits in `grid_g_pending`. An
 * array whose TrackID is not this track's is freed, never used. [deck] */
static void grid_beats_for(uint64_t lo, uint64_t hi, uintptr_t holder,
                           int pool_rate)
{
    struct grid_beats *b;

    if (grid_g_beats && __atomic_load_n(&grid_g_beats_live, __ATOMIC_ACQUIRE) &&
        grid_g_beats_rate == pool_rate &&
        grid_g_beats->tid_lo == lo && grid_g_beats->tid_hi == hi)
        return;

    b = __atomic_exchange_n(&grid_g_pending, (struct grid_beats *)NULL,
                            __ATOMIC_ACQ_REL);
    if (b && (b->tid_lo != lo || b->tid_hi != hi)) {
        free(b);
        b = NULL;
    }
    if (!b && holder) {
        b = grid_copy_beats(holder);
        if (b) {
            b->tid_lo = lo;
            b->tid_hi = hi;
        }
    }
    if (!b) {
        /* Nothing for this track; the average (fixed-tempo route) stands. */
        __atomic_store_n(&grid_g_beats_live, 0, __ATOMIC_RELEASE);
        return;
    }
    grid_publish_beats(b, pool_rate);
    MDBG("grid: %d beats on the mix's timeline -> the loop follows the tempo\n",
         (int)grid_g_beats->count);
}

/* Walk one PositionWithSourceInfo to its grid holder, then read it. */
static double grid_read(uintptr_t pwsi, int pool_rate, int64_t *beat0,
                        uintptr_t *holder_out, const char **why)
{
    uintptr_t src = 0, holder = 0;
    int32_t sig = 0;

    /* No tag test on the position: its constant differs between a bare
     * Position and this form. The source's signature below is the check the
     * app itself makes, and an empty slot fails it. */
    *why = "no cue to read";
    if (!pwsi ||
        mod_safe_read(pwsi + PWSI_SOURCE_OFF, &src, sizeof(src)) != 0 || !src)
        return 0.0;

    *why = "not a source";
    if (mod_safe_read(src + OBJ_SIG_OFF, &sig, sizeof(sig)) != 0 || sig != OBJ_SIG)
        return 0.0;

    grid_probe_sid(src);

    *why = "no grid holder";
    if (mod_safe_read(src + SRC_HOLDER_OFF, &holder, sizeof(holder)) != 0 ||
        !holder)
        return 0.0;

    *holder_out = holder;
    grid_g_holder = holder;
    /* A cue slot does not say whose source it holds, so this holder can be
     * edited but not saved -- see grid_g_holder_tid_lo. */
    grid_g_holder_tid_lo = grid_g_holder_tid_hi = 0;
    return grid_scale(holder, pool_rate, beat0, why);
}


typedef void (*grid_reply_fn_t)(void *self, void *req, void *tid, void *grid,
                                void *res);
static uintptr_t grid_g_orig_reply;


/* Hook on trackinfo_stocker::BeatGridRequestHandler::Reception::
 * replyBeatGridRequest, the only route to a grid that does not go through a
 * cue slot. A track with no cues has no reachable source (all ten kinds stay
 * silent).
 *
 * The SharedBeatGridPtr is tried both as the holder and as a pointer to it.
 * The TrackID is kept with the result and matched against the loaded track in
 * grid_arm_reply, so a reply for a track that was only browsed is never
 * armed. */
static void grid_wrap_reply(void *self, void *req, void *tid, void *grid,
                            void *res)
{
    char id[GRID_TID_BYTES * 2 + 1];
    uintptr_t deref = 0;
    const char *why = NULL;
    uint64_t tlo = 0, thi = 0;
    int64_t beat0 = 0;
    double spb = 0.0;
    int rate = 0, ok, hop = 0;

    grid_hex((uintptr_t)tid, GRID_TID_BYTES, id);
    if (mod_safe_read((uintptr_t)tid, &tlo, sizeof(tlo)) != 0 ||
        mod_safe_read((uintptr_t)tid + 8, &thi, sizeof(thi)) != 0)
        tlo = thi = 0;

    /* Straight first, then one hop. Whichever reads is the answer. */
    ok = grid_from_holder((uintptr_t)grid, &spb, &beat0, &rate, &why);
    if (!ok && mod_safe_read((uintptr_t)grid, &deref, sizeof(deref)) == 0 &&
        deref) {
        ok = grid_from_holder(deref, &spb, &beat0, &rate, &why);
        hop = ok;
    }

    if (ok) {
        struct grid_beats *b;
        int64_t bits;

        memcpy(&bits, &spb, sizeof(bits));

        /* Copied here, while the object is certainly alive. The next arm
         * collects it; an array displaced by a newer reply is freed by the
         * exchange. */
        grid_g_holder = hop ? deref : (uintptr_t)grid;
        grid_g_holder_tid_lo = tlo;
        grid_g_holder_tid_hi = thi;
        b = grid_copy_beats(grid_g_holder);
        if (b) {
            b->tid_lo = tlo;
            b->tid_hi = thi;
        }
        /* Kept before staging: the arm converts the staged array to the pool
         * rate in place, and the slot must keep the reply's rate. */
        grid_reply_keep(tlo, thi, bits, beat0, rate, b);
        if (b)
            free(__atomic_exchange_n(&grid_g_pending, b, __ATOMIC_ACQ_REL));

        /* The beat count sits next to the tempo, so a failed copy (the track
         * will use the average) shows up beside it. */
        MDBG("grid: reply track %s -> %.1f BPM, beat0 %lld at %d Hz%s, %d beats"
             " copied\n", id, (double)rate * 60.0 / spb, (long long)beat0, rate,
             hop ? " (via *ptr)" : "", b ? (int)b->count : 0);
    } else {
        MDBG("grid: reply track %s -> no grid (%s)\n", id, why ? why : "?");
    }

    ((grid_reply_fn_t)grid_g_orig_reply)(self, req, tid, grid, res);
}

static int grid_install(void)
{
    if (mod_patch_vslot("gridReply", EP122_GRID_RECEPTION,
                        GRID_RECEPTION_SLOT, (void *)grid_wrap_reply,
                        &grid_g_orig_reply) != 0) {
        MDBG("grid: no beat-grid reply -> cue slots are the only route\n");
        return -1;
    }
    return 0;
}

KIT_MOD(k_mod_stem_grid,
        .name = "stem_grid", .prio = 61, .install = grid_install,
        .what = "beat grid: watch the deck's own grid replies");

/* Arm the deck's own reply for the loaded track, without needing a pad press.
 * The X-PAD's clock and the stems row's quantized MUTE run on this grid, so it
 * must be armed as soon as the track loads.
 *
 * Returns 0 when there is no reply for the source the audio thread is reading;
 * stem_grid_take then falls back to the cue walk. Not [audio]: publishing
 * frees the displaced array once its readers are gone. */
static int grid_arm_reply(int rate, uint64_t tid_lo, uint64_t tid_hi, int have_id)
{
    struct grid_reply r;
    int64_t beat0;
    double  raw, spb;
    int     grate;

    /* A reply is this track's only if its id is the one the audio thread is
     * reading. */
    if (!have_id || !grid_reply_find(tid_lo, tid_hi, &r))
        return 0;

    grate = r.rate;
    memcpy(&raw, &r.spb_bits, sizeof(raw));
    if (!(raw > 0.0) || grate <= 0)
        return 0;

    if (tid_lo == __atomic_load_n(&grid_g_armed_lo, __ATOMIC_RELAXED) &&
        tid_hi == __atomic_load_n(&grid_g_armed_hi, __ATOMIC_RELAXED) &&
        rate   == __atomic_load_n(&grid_g_armed_rate, __ATOMIC_RELAXED))
        return 1;

    /* One arming at a time: [deck] (pad press) and [message] (tick) both
     * publish, which frees the old array, so concurrent arms would double
     * free. The loser returns 1 too; the winner publishes the same result. */
    if (__atomic_exchange_n(&grid_g_arming, 1, __ATOMIC_ACQ_REL))
        return 1;

    spb   = raw * (double)rate / (double)grate;
    beat0 = grid_to_pool(r.beat0, rate, grate);
    MDBG("grid: %.1f BPM from the deck's own reply\n",
         (double)rate * 60.0 / spb);
    /* Stage this track's kept beats, since a reloaded track gets no new reply,
     * and pass the reply's holder so a later arm (e.g. after the pool rate
     * settles, once the staged copy is consumed) can rebuild the array instead
     * of dropping it. */
    grid_reply_stage_beats(tid_lo, tid_hi);
    grid_beats_for(tid_lo, tid_hi,
                   (grid_g_holder_tid_lo == tid_lo &&
                    grid_g_holder_tid_hi == tid_hi) ? grid_g_holder : 0, rate);
    grid_publish(spb, beat0);
    __atomic_store_n(&grid_g_armed_lo, tid_lo, __ATOMIC_RELAXED);
    __atomic_store_n(&grid_g_armed_hi, tid_hi, __ATOMIC_RELAXED);
    __atomic_store_n(&grid_g_armed_rate, rate, __ATOMIC_RELAXED);
    __atomic_store_n(&grid_g_arming, 0, __ATOMIC_RELEASE);
    return 1;
}

/* Arm from the display clock, because the reply lands when the track loads
 * and the pool rate only once the audio path runs. Retried every frame until
 * it takes; cheap after that. [message] */
void stem_grid_tick(void)
{
    uint64_t lo = 0, hi = 0;
    int rate = stem_pool_rate();
    int have = stem_source_id(&lo, &hi);

    if (rate > 0)
        grid_arm_reply(rate, lo, hi, have);
}

/* The loaded track's grid: the deck's own answer first, cue slots second.
 *
 * Cue slots go stale: loading a track does not clear a slot the new track has
 * no cue for, so a slot can return the previous track's tempo and anchor. A
 * wrong-track grid is worse than none.
 *
 * The reply is produced only by a load and carries its TrackID. The cue walk
 * remains for a shim started mid-session, with no load since and no reply
 * cached.
 *
 * Called as a pad is pressed. [deck] */
int stem_grid_take(const struct cue_event *ev)
{
    int rate = stem_pool_rate();
    const char *why = "no cue to read";
    /* One character per kind, memory cue first: '-' no slot, '.' a slot that
     * led nowhere, 'g' a slot that gave a grid. Logged on every arm to show
     * how many slots answer. */
    char map[GRID_KIND_MAX + 2];
    uint64_t tid_lo = 0, tid_hi = 0;
    uintptr_t holder = 0;
    int kind, got = -1, have_id;
    int64_t beat0 = 0;
    double spb = 0.0;

    if (rate <= 0)
        return 0;

    have_id = stem_source_id(&tid_lo, &tid_hi);

    /* Probe once whether a source carries its sourceId (grid_probe_sid),
     * before the reply shortcut below can skip the walk. */
    {
        static int probed;
        int k;

        for (k = 0; !probed && k <= GRID_KIND_MAX; k++) {
            uintptr_t slot = cue_slot(ev, k), s = 0;
            int32_t sg = 0;

            if (!slot ||
                mod_safe_read(slot + SLOT_PWSI_OFF + PWSI_SOURCE_OFF, &s,
                              sizeof(s)) != 0 || !s ||
                mod_safe_read(s + OBJ_SIG_OFF, &sg, sizeof(sg)) != 0 ||
                sg != OBJ_SIG)
                continue;
            probed = 1;
            grid_probe_sid(s);
        }
    }

    /* The reply first; usually the tick has already armed it. */
    if (grid_arm_reply(rate, tid_lo, tid_hi, have_id))
        return 1;

    for (kind = 0; kind <= GRID_KIND_MAX; kind++) {
        uintptr_t slot = cue_slot(ev, kind), h = 0;
        int64_t b = 0;
        double s;

        if (!slot) {
            map[kind] = '-';
            continue;
        }
        s = grid_read(slot + SLOT_PWSI_OFF, rate, &b, &h, &why);
        map[kind] = s > 0.0 ? 'g' : '.';
        if (s > 0.0 && got < 0) {
            got = kind;
            spb = s;
            beat0 = b;
            holder = h;
        }
    }
    map[GRID_KIND_MAX + 1] = '\0';

    if (got < 0) {
        MDBG("grid: %s [%s] -> loops play at their own tempo\n", why, map);
        grid_beats_drop();
        grid_publish(0.0, 0);
        return 0;
    }
    MDBG("grid: %.1f BPM from cue kind %d [%s]\n",
         (double)rate * 60.0 / spb, got, map);

    /* Publish the array only with a track id to stamp it with; an unstamped
     * array could not be checked against the next track. */
    if (have_id)
        grid_beats_for(tid_lo, tid_hi, holder, rate);
    else
        grid_beats_drop();
    grid_publish(spb, beat0);
    return 1;
}

/* ---- grid editing state (the edits are in grid_edit.c) ------------------ */



/* What the deck loaded, so RESET is a restore. Both arrays, so a reset cannot
 * leave the original beats with our bars. [deck] */
uintptr_t grid_g_orig_content;
uintptr_t grid_g_orig_beats;
int32_t   grid_g_orig_count;
uintptr_t grid_g_orig_bars;
int32_t   grid_g_orig_barcnt;

/* The Content of the remembered holder, or 0. */
static uintptr_t grid_live_content(void)
{
    uintptr_t content = 0;

    if (!grid_g_holder ||
        mod_safe_read(grid_g_holder + HOLDER_CONTENT_OFF, &content,
                      sizeof(content)) != 0)
        return 0;
    return content;
}

uintptr_t stem_grid_id(void)
{
    return grid_live_content();
}

/* The first beat's own BPM field, the number the deck shows on the play
 * screen, so the panel and the deck always agree and a rescale moves both.
 *
 * Read from the holder, not derived from the published interval, which is in
 * pool-rate samples: with STEMS off there is no pool rate. */
double stem_grid_bpm(void)
{
    uintptr_t content = grid_live_content(), beats = 0;
    int32_t count = 0;

    if (!content ||
        mod_safe_read(content + CONTENT_BEATS_OFF, &beats, sizeof(beats)) != 0 ||
        !beats ||
        mod_safe_read(content + CONTENT_COUNT_OFF, &count, sizeof(count)) != 0 ||
        count < 1)
        return 0.0;
    return grid_beat_bpm(beats, 0);
}

/* The offset the deck's own grid modes move (all six move only this), so
 * non-zero means its RESET has something to undo. 0 when there is no grid,
 * which also means nothing to undo. */
int64_t stem_grid_offset(void)
{
    int64_t origin = 0;

    if (!grid_g_holder ||
        mod_safe_read(grid_g_holder + HOLDER_ORIGIN_OFF, &origin,
                      sizeof(origin)) != 0)
        return 0;
    return origin;
}

/* The tempo before any panel edit. Each edit is computed against this and
 * replaces the previous one instead of compounding, so a caller wanting two
 * fine-adjust presses to add up must pass the total, computed from this. */
double stem_grid_orig_bpm(void)
{
    uintptr_t content = grid_live_content();

    if (content && content == grid_g_orig_content && grid_g_orig_beats)
        return grid_beat_bpm(grid_g_orig_beats, 0);
    return stem_grid_bpm();
}
