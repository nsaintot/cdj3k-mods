// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * mods/stem/grid_read.c - reading a grid out of the deck's own objects.
 */
#include "stem/grid_internal.h"
#include "cue/cue.h"
#include "db/db.h"
#include "kit/mod.h"

/* Grid units to pool samples. The one expression used for both the beat array
 * and the anchor: the anchor is one of the beats, and two differently written
 * float computations might not land on the same sample. */
int64_t grid_to_pool(int64_t raw, int pool_rate, int grid_rate)
{
    return (int64_t)((double)raw * (double)pool_rate / (double)grid_rate);
}

/* Beat `i`'s own BPM field (a double). */
double grid_beat_bpm(uintptr_t beats, int i)
{
    double b = 0.0;

    if (mod_safe_read(beats + (uintptr_t)i * BEAT_STRIDE + BEAT_BPM_OFF,
                      &b, sizeof(b)) != 0)
        return 0.0;
    return b;
}

/* The grid's anchor is its first beat: the cell's second field is a BPM, not a
 * bar number, so nothing in the cells marks a downbeat. */
int grid_downbeat(uintptr_t beats, int count)
{
    (void)beats; (void)count;
    return 0;
}

/* One grid holder (meow::UpdatableSharedObject<appnd_trk_info::BeatGrid>)
 * reduced to a beat length and a downbeat; returns 0 if any check fails. Shared
 * by both routes to a holder: a cue slot's source and the deck's grid reply.
 *
 * Results are in the grid's own units: the reply arrives while the track loads,
 * before the audio path has a pool rate. The grid's own rate is enough to
 * sanity-check the tempo; callers convert to pool samples. */
int grid_from_holder(uintptr_t holder, double *spb_out,
                            int64_t *beat0_out, int *rate_out,
                            const char **why)
{
    uintptr_t content = 0, beats = 0;
    int64_t first = 0, last = 0, origin = 0, anchor = 0;
    int32_t count = 0, rate = 0;
    double spb, bpm;
    int db;

    *why = "no grid";
    if (mod_safe_read(holder + HOLDER_CONTENT_OFF, &content,
                      sizeof(content)) != 0 || !content)
        return 0;

    *why = "grid rate";
    if (mod_safe_read(holder + HOLDER_RATE_OFF, &rate, sizeof(rate)) != 0 ||
        rate <= 0)
        return 0;

    *why = "beat count";
    if (mod_safe_read(content + CONTENT_COUNT_OFF, &count, sizeof(count)) != 0 ||
        count < GRID_MIN_BEATS || count > GRID_MAX_BEATS)
        return 0;

    *why = "no beats";
    if (mod_safe_read(content + CONTENT_BEATS_OFF, &beats, sizeof(beats)) != 0 ||
        !beats)
        return 0;

    *why = "beat positions";
    if (mod_safe_read(beats, &first, sizeof(first)) != 0 ||
        mod_safe_read(beats + (uintptr_t)(count - 1) * BEAT_STRIDE, &last,
                      sizeof(last)) != 0 || last <= first)
        return 0;

    spb = (double)(last - first) / (double)(count - 1);

    /* Checked against the grid's own rate, which depends on nothing outside
     * the grid. */
    *why = "tempo out of range";
    bpm = (double)rate * 60.0 / spb;
    if (!(bpm >= GRID_MIN_BPM) || !(bpm <= GRID_MAX_BPM))
        return 0;

    /* The anchor needs the origin the deck adds to every beat (it cancels out
     * of the tempo). A missing origin is treated as zero, not a failure. */
    if (mod_safe_read(holder + HOLDER_ORIGIN_OFF, &origin, sizeof(origin)) != 0)
        origin = 0;
    db = grid_downbeat(beats, count);
    if (mod_safe_read(beats + (uintptr_t)db * BEAT_STRIDE,
                      &anchor, sizeof(anchor)) != 0)
        anchor = first;
    *spb_out   = spb;
    *beat0_out = origin + anchor;
    *rate_out  = rate;

    /* Log the anchor in full to tell a phase error from a tempo error. */
    MDBG("grid: origin %lld first %lld anchor %lld (beat %d of %d, cell BPM"
         " %.2f %.2f) rate %d -> %.1f BPM from the interval, beat0 %lld\n",
         (long long)origin, (long long)first, (long long)anchor, db, count,
         grid_beat_bpm(beats, 0), grid_beat_bpm(beats, count - 1),
         rate, bpm, (long long)*beat0_out);

    *why = NULL;
    return 1;
}

/* Log where in a page source its own sourceId sits, if anywhere. A cue slot
 * can name the previous track's source; if the source carries its id, the walk
 * could check it against the id the audio thread reports. Scans the first
 * GRID_SID_SCAN bytes, once. */
void grid_probe_sid(uintptr_t src)
{
    static int said;
    uint64_t lo = 0, hi = 0, a = 0, b = 0;
    unsigned off;

    if (said || !stem_source_id(&lo, &hi) || (!lo && !hi))
        return;
    for (off = 0; off + 16 <= GRID_SID_SCAN; off += 8) {
        if (mod_safe_read(src + off, &a, sizeof(a)) != 0 ||
            mod_safe_read(src + off + 8, &b, sizeof(b)) != 0)
            continue;
        if (a == lo && b == hi) {
            said = 1;
            MDBG("grid: source carries its sourceId at +%#x\n", off);
            return;
        }
    }
    said = 1;
    MDBG("grid: source does NOT carry the playing sourceId (%llx:%llx)"
         " in its first %#x\n", (unsigned long long)lo,
         (unsigned long long)hi, GRID_SID_SCAN);
}

/* One holder's beat length and anchor in pool samples, the unit the mix and
 * the engage position use. The grid and pool rates are the same today; the
 * scale keeps this correct if they differ. */
double grid_scale(uintptr_t holder, int pool_rate, int64_t *beat0,
                         const char **why)
{
    double spb = 0.0;
    int64_t raw = 0;
    int rate = 0;

    if (pool_rate <= 0) {
        *why = "no pool rate";
        return 0.0;
    }
    if (!grid_from_holder(holder, &spb, &raw, &rate, why))
        return 0.0;
    *beat0 = grid_to_pool(raw, pool_rate, rate);
    return spb * (double)pool_rate / (double)rate;
}

/* Every beat of `holder`, in the grid's own units with the origin added. NULL
 * when there is nothing to copy, leaving the caller on the average.
 *
 * Read in chunks: mod_safe_read is a pread on /proc/self/mem, so one beat at a
 * time would be thousands of syscalls during a pad press.
 *
 * Must be ascending: stem_beat_at bisects it, and a bisection over an unordered
 * array silently gives wrong answers. Equal neighbours are allowed (a
 * hand-edited grid can contain them); a beat going backwards is refused and
 * logged. */
struct grid_beats * grid_copy_beats(uintptr_t holder)
{
    uintptr_t content = 0, beats = 0;
    struct grid_beats *b;
    int64_t origin = 0;
    int32_t count = 0, i = 0;
    uint8_t *tmp;
    int rate = 0;

    if (mod_safe_read(holder + HOLDER_CONTENT_OFF, &content,
                      sizeof(content)) != 0 || !content ||
        mod_safe_read(holder + HOLDER_RATE_OFF, &rate, sizeof(rate)) != 0 ||
        rate <= 0 ||
        mod_safe_read(content + CONTENT_COUNT_OFF, &count, sizeof(count)) != 0 ||
        count < GRID_MIN_BEATS ||
        mod_safe_read(content + CONTENT_BEATS_OFF, &beats, sizeof(beats)) != 0 ||
        !beats)
        return NULL;

    if (count > GRID_BEATS_MAX) {
        MDBG("grid: %d beats is past the %d we keep -> the average instead\n",
             (int)count, GRID_BEATS_MAX);
        return NULL;
    }
    if (mod_safe_read(holder + HOLDER_ORIGIN_OFF, &origin, sizeof(origin)) != 0)
        origin = 0;

    /* One-shot ownership probe for grid editing. A x2 needs 2N-1 cells where N
     * exist, so the deck must accept a different beat array. This logs:
     *
     *   before the array   the allocator header, which says who frees it (a
     *                      glibc chunk has a size at -8 with flag bits; freeing
     *                      a malloc'd pointer through a pool free corrupts the
     *                      heap)
     *   after the last cell the overrun guard the app asserts on
     *                      (Checker<Cell,int>::assertIfSignitureBroken), which a
     *                      replacement must reproduce
     *   the slack          enough rounding would allow x2 in place
     *
     * Reads only, once, around an array the deck already holds. */
    {
        static int probed;

        if (!probed) {
            uint64_t before[4] = { 0, 0, 0, 0 }, after[6] = { 0, 0, 0, 0, 0, 0 };
            int k;

            probed = 1;
            for (k = 0; k < 4; k++)
                (void)mod_safe_read(beats - 32 + (uintptr_t)k * 8, &before[k],
                                    sizeof(before[k]));
            for (k = 0; k < 6; k++)
                (void)mod_safe_read(beats + (uintptr_t)count * BEAT_STRIDE +
                                    (uintptr_t)k * 8, &after[k], sizeof(after[k]));
            MDBG("grid: array %#lx, %d cells (%#lx bytes)\n",
                 (unsigned long)beats, (int)count,
                 (unsigned long)((uintptr_t)count * BEAT_STRIDE));
            MDBG("grid:  before -32..-1: %016llx %016llx %016llx %016llx\n",
                 (unsigned long long)before[0], (unsigned long long)before[1],
                 (unsigned long long)before[2], (unsigned long long)before[3]);
            MDBG("grid:  after  end+0..: %016llx %016llx %016llx %016llx"
                 " %016llx %016llx\n",
                 (unsigned long long)after[0], (unsigned long long)after[1],
                 (unsigned long long)after[2], (unsigned long long)after[3],
                 (unsigned long long)after[4], (unsigned long long)after[5]);

            /* The whole cell, including the BPM double after the position,
             * and the holder's head in case a length or count there must move
             * too. */
            for (k = 0; k < 4 && k < count; k++) {
                uint64_t c[2] = { 0, 0 };

                (void)mod_safe_read(beats + (uintptr_t)k * BEAT_STRIDE, &c[0],
                                    sizeof(c[0]));
                (void)mod_safe_read(beats + (uintptr_t)k * BEAT_STRIDE + 8,
                                    &c[1], sizeof(c[1]));
                MDBG("grid:  cell[%d] pos=%lld extra=%016llx\n", k,
                     (long long)c[0], (unsigned long long)c[1]);
            }
            for (k = 0; k < 12; k++) {
                uint64_t h = 0;

                (void)mod_safe_read(holder + (uintptr_t)k * 8, &h, sizeof(h));
                MDBG("grid:  holder+%#04x = %016llx\n", k * 8,
                     (unsigned long long)h);
            }

            /* The bar array (see CONTENT_BARS_OFF): the PQTZ writer gets a
             * beat's place in its bar from a helper taking (content, i), which reads
             * the int32 array at content+0x38, count at +0x40. Probed like the
             * cells: the bytes before say who frees it, the bytes after which
             * guard to reproduce. */
            {
                uintptr_t bars = 0;
                int32_t   nbar = 0, first = 0, last = 0;
                uint32_t  lo = 0, hi = 0;

                (void)mod_safe_read(content + CONTENT_BARS_OFF, &bars,
                                    sizeof(bars));
                (void)mod_safe_read(content + CONTENT_BARCNT_OFF, &nbar,
                                    sizeof(nbar));
                (void)mod_safe_read(bars - BAR_STRIDE, &lo, sizeof(lo));
                (void)mod_safe_read(bars + (uintptr_t)nbar * BAR_STRIDE, &hi,
                                    sizeof(hi));
                (void)mod_safe_read(bars, &first, sizeof(first));
                (void)mod_safe_read(bars + (uintptr_t)(nbar - 1) * BAR_STRIDE,
                                    &last, sizeof(last));
                MDBG("grid:  bars %#lx x %d, %d..%d, guards %08x %08x\n",
                     (unsigned long)bars, (int)nbar, (int)first, (int)last,
                     lo, hi);
            }
        }
    }

    tmp = malloc((size_t)GRID_BEATS_CHUNK * BEAT_STRIDE);
    b   = malloc(sizeof(*b) + (size_t)count * sizeof(b->pos[0]));
    if (!tmp || !b) {
        free(tmp);
        free(b);
        return NULL;
    }
    b->count  = count;
    b->rate   = rate;
    b->tid_lo = b->tid_hi = 0;

    while (i < count) {
        int32_t n = count - i < GRID_BEATS_CHUNK ? count - i : GRID_BEATS_CHUNK;
        int32_t k;

        if (mod_safe_read(beats + (uintptr_t)i * BEAT_STRIDE, tmp,
                          (size_t)n * BEAT_STRIDE) != 0) {
            free(tmp);
            free(b);
            return NULL;
        }
        for (k = 0; k < n; k++) {
            int64_t p;

            memcpy(&p, tmp + (size_t)k * BEAT_STRIDE, sizeof(p));
            p += origin;
            if (i + k > 0 && p < b->pos[i + k - 1]) {
                MDBG("grid: beat %d goes backwards -> the average instead\n",
                     (int)(i + k));
                free(tmp);
                free(b);
                return NULL;
            }
            b->pos[i + k] = p;
        }
        i += n;
    }
    free(tmp);
    return b;
}

void grid_hex(uintptr_t at, int n, char *out)
{
    static const char k_hex[] = "0123456789abcdef";
    uint8_t b;
    int i;

    for (i = 0; i < n; i++) {
        if (mod_safe_read(at + (uintptr_t)i, &b, sizeof(b)) != 0)
            b = 0;
        out[i * 2]     = k_hex[b >> 4];
        out[i * 2 + 1] = k_hex[b & 15];
    }
    out[n * 2] = '\0';
}
