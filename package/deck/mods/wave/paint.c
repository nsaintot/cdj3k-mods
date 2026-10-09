// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * paint.c - the pristine copy, and turning gains into pixels.
 *
 * Part of the waveform-follows-the-faders feature. Shared declarations and the
 * design notes are in wave.h.
 */
#include "wave/wave.h"

/* ---- the pristine copy ---------------------------------------------------- */

static uint32_t digest(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    size_t i;

    for (i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

/* Where `obj` currently points, or 0 if it will not resolve. Three reads, run
 * every tick. */
int wave_resolve_obj(uintptr_t obj, struct wave_array *out)
{
    uintptr_t content, columns;
    uint32_t count = 0;

    if (!obj)
        return 0;
    content = wave_deref(obj + OBJ_CONTENT_OFF);
    columns = content ? wave_deref(content + CONTENT_COLUMNS_OFF) : 0;
    if (!columns ||
        mod_safe_read(content + CONTENT_COUNT_OFF, &count, sizeof(count)) != 0 ||
        count == 0 || count > MAX_COLUMNS)
        return 0;
    out->obj = obj;
    out->content = content;
    out->columns = columns;
    out->count = count;
    return 1;
}

/* The object a style is bound to, which is not always the last one replied.
 *
 * Browse previews come back through the same Reception as the deck view's
 * waveform, so the latch moves to other tracks' arrays while a list is scrolled.
 *
 * So a style binds once: the first reply that yields a filled array wins, and the
 * binding is dropped only with the copy (a real track change, or the deck
 * refilling the array). Nothing here can tell a preview from the deck view;
 * wave_stale's track-id check catches a binding to another track's array. */
int wave_resolve(int style, struct wave_array *out)
{
    struct style_state *st = &wave_g_st[style];

    return wave_resolve_obj(st->from_obj ? st->from_obj : wave_g_obj_style[style],
                            out);
}

/* Has the deck put something else in the array since we last wrote to it?
 * Content is the only reliable test (see wave.h). `written` is what the array
 * should hold; before any write, that is the pristine copy. */
static int array_diverged(struct style_state *st)
{
    const uint8_t *ref = st->written;
    size_t bytes = (size_t)st->ncols * st->stride;
    uint8_t buf[ARRAY_PROBE_BYTES];
    int i, differed = 0;

    if (!ref) {
        /* Modified but no record of what was written (wave_write_style could not
         * allocate one). Comparing against pristine would report a change every
         * tick, so report none. */
        if (st->modified)
            return 0;
        ref = st->pristine;
    }
    if (!ref || !st->columns || bytes < sizeof(buf))
        return 0;
    for (i = 0; i < ARRAY_PROBES; i++) {
        size_t at = (bytes - sizeof(buf)) / (ARRAY_PROBES - 1) * (size_t)i;

        if (mod_safe_read(st->columns + at, buf, sizeof(buf)) != 0)
            return 1;                    /* unreadable: treat as changed */
        if (memcmp(buf, ref + at, sizeof(buf)) != 0 &&
            ++differed >= ARRAY_PROBE_DIFFS)
            return 1;
    }
    return 0;
}

/* Does the copy we hold still describe what the deck is drawing? */
int wave_stale(int style)
{
    struct style_state *st = &wave_g_st[style];
    struct wave_array a;
    uint64_t lo = 0, hi = 0;

    if (!st->pristine)
        return 0;                        /* nothing to be stale */

    /* Check the copy's track against the playing one once the sourceId is known.
     * A reply taken on trust before then is verified here. */
    if (stem_source_id(&lo, &hi) &&
        (st->from_tid.lo != lo || st->from_tid.hi != hi)) {
        MDBG("wave_stems: %s copy is track %llx:%llx, playing %llx:%llx\n",
             wave_k_style_name[style],
             (unsigned long long)st->from_tid.hi,
             (unsigned long long)st->from_tid.lo,
             (unsigned long long)hi, (unsigned long long)lo);
        return 1;
    }
    if (!wave_resolve(style, &a))
        return 1;
    if (a.obj != st->from_obj || a.content != st->from_content ||
        a.columns != st->columns || a.count != st->ncols)
        return 1;
    return array_diverged(st);
}

/* What we last left in an array, kept across a track-change invalidate when the
 * array could not be restored.
 *
 * Our painted columns are stable and not blank, so a capture would otherwise adopt
 * them as the pristine copy.
 *
 * Kept only when our version differed from the deck's. At unity they are identical,
 * adopting them is correct, and refusing would stall every reload of the track. */
static struct {
    uint8_t  *bytes;
    uintptr_t columns;
    uint32_t  ncols;
} g_left[3];

static void forget_left(int style)
{
    free(g_left[style].bytes);
    memset(&g_left[style], 0, sizeof(g_left[style]));
}

/* Is this array still exactly what we left in it (the deck has not yet written
 * the new track's columns)? */
static int still_our_paint(int style, const struct wave_array *a,
                           const uint8_t *probe, size_t bytes)
{
    if (!g_left[style].bytes || g_left[style].columns != a->columns ||
        g_left[style].ncols != a->count)
        return 0;
    return memcmp(probe, g_left[style].bytes, bytes) == 0;
}

/* Put the deck's own columns back before letting go of the array.
 *
 * The deck keeps a waveform per track and returns the same array when the track is
 * loaded again, still holding what we last wrote. Unrestored, the next capture
 * would adopt the reduced picture as pristine and each visit would compound it.
 *
 * The array is read back first: if it was freed and reused it will not match what
 * we left, and the restore is skipped. mod_safe_write cannot fault either way. */
static void restore_if_ours(int style, size_t bytes)
{
    struct style_state *st = &wave_g_st[style];
    uint8_t *live;

    if (!st->modified || !st->columns || !st->written || !st->pristine || !bytes)
        return;
    if (memcmp(st->written, st->pristine, bytes) == 0)
        return;                          /* nothing of ours is in it */
    live = malloc(bytes);
    if (!live)
        return;
    if (mod_safe_read(st->columns, live, bytes) == 0 &&
        memcmp(live, st->written, bytes) == 0) {
        wave_write_style(style, st->pristine);
        st->modified = 0;
        MDBG("wave_stems: %s array handed back as the deck drew it\n",
             wave_k_style_name[style]);
    }
    free(live);
}

void wave_invalidate(int style)
{
    struct style_state *st = &wave_g_st[style];
    size_t bytes = (size_t)st->ncols * st->stride;

    restore_if_ours(style, bytes);
    forget_left(style);
    /* If the restore was not made (array already refilled, or unreadable), this
     * record stops the capture adopting our paint. */
    if (st->written && st->pristine && bytes &&
        memcmp(st->written, st->pristine, bytes) != 0) {
        g_left[style].bytes = st->written;
        g_left[style].columns = st->columns;
        g_left[style].ncols = st->ncols;
        st->written = NULL;              /* handed over, not freed */
    }
    free(st->pristine);
    free(st->scratch);
    free(st->written);
    free(st->probe);
    memset(st, 0, sizeof(*st));
}

/* Is this array not filled in yet?
 *
 * An untouched array is all zeros, which is stable, so the stability wait alone
 * would adopt it. The first paint would then write that blank over the waveform
 * the deck has since drawn: a flat line, never corrected.
 *
 * A 3-band column encodes silence as 0x1008 with zero heights, so even a silent
 * track has two non-zero bytes per column; only an unwritten array is all zero.
 * The 1-in-64 floor is far below any real waveform. */
static int not_filled_yet(const uint8_t *p, size_t n)
{
    size_t i, nonzero = 0;

    for (i = 0; i < n; i++) {
        if (p[i])
            nonzero++;
    }
    return nonzero * 64 < n;
}

/* Log why a capture is not finishing, so a permanent "not yet" is visible.
 * Heavily throttled. */
#define STALL_TICKS 100

static void capture_stalled(int style, const char *why)
{
    static int ticks[3];
    static const char *last[3];
    int first;

    first = last[style] != why;
    if (first) {
        last[style] = why;
        ticks[style] = 0;
    }
    if (ticks[style]++ % STALL_TICKS)
        return;
    /* A new reason at debug level once; repeats only at trace level. */
    if (first)
        MDBG("wave_stems: %s capture waiting: %s\n",
             wave_k_style_name[style], why);
    else
        MTRACE("wave_stems: %s capture waiting: %s\n",
               wave_k_style_name[style], why);
}

/* One poll towards a pristine copy. 1 when the copy is complete.
 *
 * Called once per worker tick while `pristine` is NULL; returns immediately on
 * most ticks (see POLL_TICKS in wave.h). */
int wave_capture_step(int style)
{
    struct style_state *st = &wave_g_st[style];
    struct wave_trackid tid;
    struct wave_array a;
    uint64_t lo = 0, hi = 0;
    size_t bytes;
    uint32_t d;
    int ours;
    /* The object this capture binds to and the track it is a copy of. Normally
     * the latched reply's; see the fallback below. */
    uintptr_t bind_obj = wave_g_obj_style[style];
    struct wave_trackid bound = { 0, 0 };
    int rebound = 0;

    /* Only bind to a reply about the playing track. Until the sourceId is known
     * the latest reply is taken on trust; wave_stale rechecks it later. */
    if (stem_source_id(&lo, &hi) && wave_latched_tid(style, &tid) &&
        (tid.lo != lo || tid.hi != hi)) {
        /* The latch is on another track. A track loaded from an earlier index in
         * the same list gets no reply, so fall back to the object last replied for
         * this track id, which is the one the deck is showing. */
        bind_obj = wave_obj_for_tid(style, lo, hi);
        if (!bind_obj) {
            capture_stalled(style, "latched reply is about another track, and"
                            " this one has never been replied for");
            return 0;
        }
        bound.lo = lo;
        bound.hi = hi;
        rebound = 1;
    }

    /* A probe stays on the object it started on: replies keep arriving during the
     * ~3 s it takes to settle, and following them would never converge. */
    if (st->probe && !wave_resolve_obj(st->probe_obj, &a)) {
        free(st->probe);
        st->probe = NULL;
    }
    if (!st->probe && !wave_resolve_obj(bind_obj, &a)) {
        capture_stalled(style, "no array to read -- nothing latched, or it will"
                        " not resolve");
        return 0;
    }
    if (st->probe && (a.columns != st->probe_columns || a.count != st->probe_ncols)) {
        free(st->probe);                 /* refilled under us: start over */
        st->probe = NULL;
        capture_stalled(style, "array moved under the probe, starting over");
    }
    bytes = (size_t)a.count * wave_k_stride[style];
    if (!st->probe) {
        st->probe = malloc(bytes);
        if (!st->probe) {
            capture_stalled(style, "no memory for the probe buffer");
            return 0;
        }
        st->probe_obj = a.obj;
        st->probe_columns = a.columns;
        st->probe_ncols = a.count;
        st->probe_blank = 0;
        st->probe_digest = 0;
        st->probe_polls = 0;
        st->probe_stable = 0;
        st->probe_wait = 0;
    }
    if (st->probe_wait > 0) {
        st->probe_wait--;
        return 0;
    }
    st->probe_wait = POLL_TICKS;

    if (mod_safe_read(a.columns, st->probe, bytes) != 0) {
        free(st->probe);
        st->probe = NULL;
        capture_stalled(style, "array will not read back");
        return 0;
    }
    /* Stable but not this track's: never written, or still our paint for the
     * previous track. Wait, do not adopt. */
    ours = still_our_paint(style, &a, st->probe, bytes);
    if (ours || not_filled_yet(st->probe, bytes)) {
        /* Reset, since there is no valid earlier digest to compare against. */
        st->probe_stable = 0;
        st->probe_digest = 0;
        st->probe_polls = 0;
        capture_stalled(style, ours ? "array is still our own last paint"
                                    : "array has nothing in it yet");
        if (++st->probe_blank >= CAPTURE_GIVEUP_POLLS) {
            MDBG("wave_stems: %s array still %s after %d polls, restarting\n",
                 wave_k_style_name[style], ours ? "our own last paint" : "blank",
                 st->probe_blank);
            free(st->probe);
            st->probe = NULL;
            /* If the deck reuses an array without rewriting it, stop refusing it. */
            if (ours)
                forget_left(style);
        }
        return 0;
    }
    st->probe_blank = 0;

    d = digest(st->probe, bytes);
    st->probe_stable = (st->probe_polls && d == st->probe_digest)
                       ? st->probe_stable + 1 : 0;
    st->probe_digest = d;
    st->probe_polls++;

    if (st->probe_stable < STABLE_POLLS) {
        capture_stalled(style, "array still changing under the probe");
        /* Never settled: log and restart, which also re-resolves the array. */
        if (st->probe_polls >= CAPTURE_GIVEUP_POLLS) {
            MDBG("wave_stems: %s array still moving after %d polls, restarting\n",
                 wave_k_style_name[style], st->probe_polls);
            free(st->probe);
            st->probe = NULL;
        }
        return 0;
    }

    free(st->pristine);
    free(st->scratch);
    free(st->written);
    st->written = NULL;      /* a new array: nothing of ours is in it yet */
    forget_left(style);      /* and the deck's own columns are in it now */
    st->pristine = st->probe;
    st->probe = NULL;
    st->scratch = malloc(bytes);
    if (!st->scratch) {
        MDBG("wave_stems: out of memory for %u %s columns\n", a.count,
             wave_k_style_name[style]);
        free(st->pristine);              /* a half copy would block the retry
                                          * without painting */
        st->pristine = NULL;
        return 0;
    }
    memcpy(st->scratch, st->pristine, bytes);  /* unpainted spans stay the deck's */
    st->ncols = a.count;
    st->columns = a.columns;
    st->from_obj = a.obj;
    st->from_content = a.content;
    /* The track this copy is of. From the fallback it is the playing track; the
     * latched id would make wave_stale reject the copy on the next tick. */
    if (rebound)
        st->from_tid = bound;
    else
        wave_latched_tid(style, &st->from_tid);
    st->stride = wave_k_stride[style];
    st->modified = 0;
    MDBG("wave_stems: pristine %s copy, %u columns at %p%s\n",
         wave_k_style_name[style], a.count, (void *)a.columns,
         rebound ? " (bound by track, no reply for it)" : "");
    return 1;
}

/* ---- applying a fader position -------------------------------------------- */

/* Writing the arrays is the expensive half of a fader move (three arrays, 655 KB,
 * through /proc/self/mem). A small fader step leaves most quantised bytes unchanged
 * (the fields are 5 and 3 bits wide), so only blocks that differ from what we last
 * wrote are sent. Block granularity because the syscall is the cost. */
#define WRITE_BLOCK 4096

/* Write one span with the same differential block scheme, bounded to the span
 * so the two-phase paint does not rescan the whole array. */
void wave_write_style_range(int style, uint32_t lo, uint32_t hi)
{
    struct style_state *st = &wave_g_st[style];
    size_t start, end, off;

    if (!st->columns || !st->scratch)
        return;
    if (!st->written) {
        /* No baseline to diff against; scratch equals pristine outside the
         * painted span, so a full write is still exact. */
        wave_write_style(style, st->scratch);
        return;
    }
    start = (size_t)lo * st->stride;
    end = (size_t)hi * st->stride;
    for (off = start; off < end; off += WRITE_BLOCK) {
        size_t n = end - off < WRITE_BLOCK ? end - off : WRITE_BLOCK;

        if (memcmp(st->written + off, st->scratch + off, n) == 0)
            continue;
        if (mod_safe_write(st->columns + off, st->scratch + off, n) != 0)
            return;
        memcpy(st->written + off, st->scratch + off, n);
    }
}

void wave_write_style(int style, const uint8_t *src)
{
    struct style_state *st = &wave_g_st[style];
    size_t bytes, off;

    if (!st->columns || !st->ncols)
        return;
    bytes = (size_t)st->ncols * st->stride;

    if (!st->written) {
        if (mod_safe_write(st->columns, src, bytes) != 0) {
            MDBG("wave_stems: %s write of %u columns failed\n",
                 wave_k_style_name[style], st->ncols);
            return;
        }
        st->written = malloc(bytes);
        if (st->written)
            memcpy(st->written, src, bytes);
        return;
    }

    for (off = 0; off < bytes; off += WRITE_BLOCK) {
        size_t n = bytes - off < WRITE_BLOCK ? bytes - off : WRITE_BLOCK;

        if (memcmp(st->written + off, src + off, n) == 0)
            continue;
        if (mod_safe_write(st->columns + off, src + off, n) != 0) {
            MWARN("wave_stems: %s write at +%zu failed\n",
                 wave_k_style_name[style], off);
            return;
        }
        memcpy(st->written + off, src + off, n);
    }
}

void wave_restore(void)
{
    int k;

    for (k = 0; k < 3; k++) {
        if (wave_g_st[k].pristine && wave_g_st[k].modified) {
            wave_write_style(k, wave_g_st[k].pristine);
            wave_g_st[k].modified = 0;
        }
    }
}

/* Ratios for one span of columns. */
void wave_ratios_for(const float *g, uint32_t lo, uint32_t hi)
{
    uint32_t i, n = wave_g_ncols;
    int b, st;

    for (i = lo; i < hi; i++) {
        double unity_all = 0.0, moved_all = 0.0;

        for (b = 0; b < MOD_WAVE_BANDS; b++) {
            double unity = 0.0, moved = 0.0;

            for (st = 0; st < N_STEMS; st++) {
                double p = wave_g_power[((size_t)st * MOD_WAVE_BANDS + b) * n + i];

                unity += p;
                moved += (double)g[st] * g[st] * p;
            }
            /* A band with no energy keeps its column (avoids 0/0). */
            wave_g_ratio[i][b] = unity > 0.0 ? (float)sqrt(moved / unity) : 1.0f;
            unity_all += unity;
            moved_all += moved;
        }
        wave_g_ratio_broad[i] = unity_all > 0.0
                         ? (float)sqrt(moved_all / unity_all) : 1.0f;
    }
}

/* Scale one span and write it, for every latched style. */
void wave_paint(uint32_t lo, uint32_t hi)
{
    int k;

    for (k = 0; k < 3; k++) {
        struct style_state *st = &wave_g_st[k];
        uint32_t a = lo, b = hi;
        size_t off;

        if (!st->pristine || !st->scratch)
            continue;
        /* The analysis and the array can disagree by a column or two at the
         * end; scale what both cover and leave the tail as the deck drew it. */
        if (b > st->ncols)
            b = st->ncols;
        if (a >= b)
            continue;
        off = (size_t)a * st->stride;
        if (k == WS_STYLE_3BAND)
            mod_wave_scale_ratios(st->pristine + off, st->scratch + off,
                                  b - a, wave_g_ratio + a);
        else if (k == WS_STYLE_RGB)
            mod_wave_scale_rgb(st->pristine + off, st->scratch + off, b - a,
                               wave_g_ratio + a, wave_g_ratio_broad + a);
        else
            mod_wave_scale_blue(st->pristine + off, st->scratch + off, b - a,
                                wave_g_ratio_broad + a);
        wave_write_style_range(k, a, b);
        st->modified = 1;
    }
}

/* Paint the columns outside the last window (still at the previous gains).
 * Called once the fader has stopped. */
void wave_paint_tail(void)
{
    if (!wave_g_tail_dirty || !wave_g_have_analysis)
        return;
    if (wave_g_tail_lo > 0) {
        wave_ratios_for(wave_g_applied, 0, wave_g_tail_lo);
        wave_paint(0, wave_g_tail_lo);
    }
    if (wave_g_tail_hi < wave_g_ncols) {
        wave_ratios_for(wave_g_applied, wave_g_tail_hi, wave_g_ncols);
        wave_paint(wave_g_tail_hi, wave_g_ncols);
    }
    wave_g_tail_dirty = 0;
    MDBG("wave_stems: tail painted [0..%u) and [%u..%u)\n",
         wave_g_tail_lo, wave_g_tail_hi, wave_g_ncols);
}

void wave_apply(const float *g)
{
    static struct timespec last_report;
    struct timespec t0, t1, t2;
    uint32_t n = wave_g_ncols, centre = 0, w0 = 0, w1 = 0;

    if (!wave_g_have_analysis || !wave_g_ratio || !wave_g_ratio_broad)
        return;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    /* The column under the playhead, from the audio path. Without one (nothing
     * played yet) the whole track is painted at once. */
    {
        int64_t pos = stem_source_pos();
        int rate = stem_pool_rate();

        if (pos > 0 && rate > 0) {
            int64_t c = pos * COLUMNS_PER_SEC / rate;

            if (c > 0 && c < (int64_t)n)
                centre = (uint32_t)c;
        }
    }

    if (centre) {
        w0 = centre > VISIBLE_BEHIND ? centre - VISIBLE_BEHIND : 0;
        w1 = centre + VISIBLE_AHEAD;
        if (w1 > n)
            w1 = n;
        wave_ratios_for(g, w0, w1);
        wave_paint(w0, w1);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);    /* visible window done */

    memcpy(wave_g_applied, g, sizeof(wave_g_applied));
    if (!centre) {
        /* No playhead yet, so no window. */
        wave_ratios_for(g, 0, n);
        wave_paint(0, n);
        wave_g_tail_dirty = 0;
    } else {
        /* The remainder is left for wave_paint_tail once the fader stops. */
        wave_g_tail_lo = w0;
        wave_g_tail_hi = w1;
        wave_g_tail_dirty = (w0 > 0 || w1 < n);
    }

    clock_gettime(CLOCK_MONOTONIC, &t2);
    if (t2.tv_sec != last_report.tv_sec) {
        long vis = (t1.tv_sec - t0.tv_sec) * 1000000L +
                   (t1.tv_nsec - t0.tv_nsec) / 1000;
        long all = (t2.tv_sec - t0.tv_sec) * 1000000L +
                   (t2.tv_nsec - t0.tv_nsec) / 1000;

        last_report = t2;
        MDBG("wave_stems: wave_apply visible[%u..%u] %ldus  total %ldus%s\n",
             w0, w1, vis, all, wave_g_tail_dirty ? "  (tail deferred)" : "");
    }
}

int wave_gains_moved(const float *g)
{
    int s;

    for (s = 0; s < N_STEMS; s++) {
        if (fabsf(g[s] - wave_g_applied[s]) > GAIN_EPSILON)
            return 1;
    }
    return 0;
}
