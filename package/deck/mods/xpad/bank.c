// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * xpad/bank.c - eight samples off the stick, decoded onto the pool's timeline.
 *
 * The first eight audio files in mods/loops/, sorted by name, become pads A..H.
 * No config file: GROOVE CIRCUIT's config states the loop's BPM, which a
 * one-shot played at its own length does not need.
 *
 * Read from the first media root only (stem_media_first_root), like the
 * circuit's slots: the banks belong to the deck, not a track, so there is
 * nothing to choose between two sticks.
 *
 * Threading. [worker] scans and decodes and is the only writer of the table;
 * [audio] reads it under the same acquire/release scheme as the stems, so a
 * re-scan never frees a buffer under a sounding voice.
 */
#include "xpad/xpad.h"
#include "stem/stem.h"
#include "kit/mod.h"

#include <dirent.h>

#define XP_PATH_MAX     STEM_CACHE_PATH_MAX
/* A whole d_name, so the name logged for each pad is never truncated. Eight of
 * these is 2 KB, negligible next to the sample buffers. */
#define XP_NAME_MAX     256

struct xpad_bank {
    int16_t *pcm;                   /* interleaved stereo at the pool rate */
    int64_t  frames;
    char     name[XP_NAME_MAX];     /* the file's own, for the readout */
};

static struct xpad_bank xpad_g_bank[XP_BANKS];
static int  xpad_g_nbank;

/* What the table was built for. A change of pool rate or stick triggers a
 * re-scan. */
static char xpad_g_root[XP_PATH_MAX];
static int  xpad_g_rate;

/* Readers in the mix, as in the stem store: the audio thread never waits and
 * the worker never frees early. */
static int  xpad_g_readers;
static int  xpad_g_live;

/* ---- the table, as the mix sees it ---------------------------------------- */

int xpad_bank_acquire(int bank, struct xpad_bank_view *out)
{
    if (bank < 0 || bank >= XP_BANKS)
        return 0;
    __atomic_fetch_add(&xpad_g_readers, 1, __ATOMIC_ACQ_REL);
    if (!__atomic_load_n(&xpad_g_live, __ATOMIC_ACQUIRE) || !xpad_g_bank[bank].pcm) {
        __atomic_fetch_sub(&xpad_g_readers, 1, __ATOMIC_ACQ_REL);
        return 0;
    }
    out->pcm    = xpad_g_bank[bank].pcm;
    out->frames = xpad_g_bank[bank].frames;
    return 1;
}

void xpad_bank_release(void)
{
    __atomic_fetch_sub(&xpad_g_readers, 1, __ATOMIC_ACQ_REL);
}

int xpad_bank_ready(int bank)
{
    if (bank < 0 || bank >= XP_BANKS)
        return 0;
    if (!__atomic_load_n(&xpad_g_live, __ATOMIC_ACQUIRE))
        return 0;
    return xpad_g_bank[bank].pcm != NULL;
}

int xpad_bank_count(void)
{
    return __atomic_load_n(&xpad_g_live, __ATOMIC_ACQUIRE) ? xpad_g_nbank : 0;
}

const char *xpad_bank_name(int bank)
{
    if (bank < 0 || bank >= XP_BANKS || !xpad_g_bank[bank].pcm)
        return "";
    return xpad_g_bank[bank].name;
}

/* ---- loading (worker) ----------------------------------------------------- */

/* One decode's state. stem_decode_pull delivers float chunks, converted to s16
 * as they arrive, so peak memory is the s16 buffer plus one chunk. */
struct xpad_load {
    int16_t *pcm;
    int64_t  cap;
    int64_t  n;
};

static int xpad_sink(const float *in, int64_t frames, void *user)
{
    struct xpad_load *l = user;
    int64_t i, take = frames;

    if (l->n + take > l->cap)
        take = l->cap - l->n;
    for (i = 0; i < take * 2; i++) {
        float v = in[i] * 32767.0f;

        /* Clipped, not scaled: the file's level is left as the DJ made it. */
        if (v >  32767.0f) v =  32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        l->pcm[l->n * 2 + i] = (int16_t)v;
    }
    l->n += take;
    return take < frames;          /* full -> stop the decoder */
}

/* Decode one file into `b`. Returns 1 when the bank ends up playable. */
static int xpad_load_one(const char *path, const char *name, int rate,
                         struct xpad_bank *b)
{
    struct xpad_load l;
    int64_t frames;

    /* The decoder's output length, not the file's frame count: the deck's
     * chain pads, and the buffer must hold the padded length. */
    frames = stem_decode_pull(path, rate, NULL, NULL);
    if (frames <= 0) {
        MDBG("xpad: %s will not decode\n", path);
        return 0;
    }
    if (frames > (int64_t)rate * XP_MAX_SECONDS) {
        MDBG("xpad: %s is %lld frames, truncated to %d s\n",
             path, (long long)frames, XP_MAX_SECONDS);
        frames = (int64_t)rate * XP_MAX_SECONDS;
    }

    l.pcm = malloc((size_t)frames * 2 * sizeof(int16_t));
    if (!l.pcm) {
        MDBG("xpad: no memory for %lld frames of %s\n", (long long)frames, path);
        return 0;
    }
    l.cap = frames;
    l.n   = 0;

    if (stem_decode_pull(path, rate, xpad_sink, &l) < 0 || l.n <= 0) {
        MDBG("xpad: %s decoded nothing\n", path);
        free(l.pcm);
        return 0;
    }
    b->pcm    = l.pcm;
    b->frames = l.n;
    snprintf(b->name, sizeof(b->name), "%s", name);
    return 1;
}

/* Drop the table and wait for the mix to let go of it. */
static void xpad_unload(void)
{
    int i;

    xpad_silence();
    __atomic_store_n(&xpad_g_live, 0, __ATOMIC_RELEASE);
    while (__atomic_load_n(&xpad_g_readers, __ATOMIC_ACQUIRE) > 0)
        usleep(1000);
    for (i = 0; i < XP_BANKS; i++) {
        free(xpad_g_bank[i].pcm);
        xpad_g_bank[i].pcm    = NULL;
        xpad_g_bank[i].frames = 0;
        xpad_g_bank[i].name[0] = '\0';
    }
    xpad_g_nbank = 0;
}

/* ---- the directory --------------------------------------------------------
 *
 * Sorted by name, which is how the DJ assigns pads. An insertion sort into eight
 * slots needs no allocation: any directory costs one pass and eight slots, and
 * files sorting past the eighth are skipped.
 *
 * Case-insensitive, so names order as the user reads them rather than by
 * ASCII. */

static int xpad_name_cmp(const char *a, const char *b)
{
    for (;;) {
        int ca = *a & 0xff, cb = *b & 0xff;

        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) return ca - cb;
        if (!ca) return 0;
        a++; b++;
    }
}

/* Whether a name is worth trying to decode. The decoder decides what it
 * supports, so this only skips dotfiles and rekordbox's .asd analysis files;
 * stem_decode_pull rejects the rest. */
static int xpad_name_wanted(const char *name)
{
    size_t n = strlen(name);

    if (name[0] == '.')
        return 0;
    if (n > 4 && strcmp(name + n - 4, ".asd") == 0)
        return 0;
    return 1;
}

struct xpad_pick { char name[XP_NAME_MAX]; };

/* Keep `name` if it belongs in the first XP_BANKS by name. `n` is how many are
 * held so far; returns the new count. */
static int xpad_pick_insert(struct xpad_pick *pick, int n, const char *name)
{
    int i, j;

    for (i = 0; i < n; i++)
        if (xpad_name_cmp(name, pick[i].name) < 0)
            break;
    if (i >= XP_BANKS)
        return n;                          /* sorts past the last pad */
    if (n < XP_BANKS)
        n++;
    for (j = n - 1; j > i; j--)
        pick[j] = pick[j - 1];
    snprintf(pick[i].name, sizeof(pick[i].name), "%s", name);
    return n;
}

/* Rebuild the table from `root`. Worker thread. */
static void xpad_scan(const char *root, int rate)
{
    struct xpad_pick pick[XP_BANKS];
    char dir[XP_PATH_MAX], path[XP_PATH_MAX];
    struct dirent *de;
    DIR *d;
    int n = 0, i, loaded = 0;

    xpad_unload();
    if ((size_t)snprintf(dir, sizeof(dir), "%s/%s", root, XP_LOOP_DIR) >= sizeof(dir))
        return;

    d = opendir(dir);
    if (!d) {
        /* No directory means no samples, the common case. Logged once per scan
         * so silent pads can be told from a bug. */
        MDBG("xpad: no %s -> no banks\n", dir);
        snprintf(xpad_g_root, sizeof(xpad_g_root), "%s", root);
        xpad_g_rate = rate;
        __atomic_store_n(&xpad_g_live, 1, __ATOMIC_RELEASE);
        return;
    }
    while ((de = readdir(d)) != NULL)
        if (xpad_name_wanted(de->d_name))
            n = xpad_pick_insert(pick, n, de->d_name);
    closedir(d);

    for (i = 0; i < n; i++) {
        if ((size_t)snprintf(path, sizeof(path), "%s/%s", dir, pick[i].name)
            >= sizeof(path)) {
            MDBG("xpad: %s is too long a path -> skipped\n", pick[i].name);
            continue;
        }
        /* Packed towards pad A: a file that will not decode gives its pad to
         * the next one, so there are no gaps. */
        if (xpad_load_one(path, pick[i].name, rate, &xpad_g_bank[loaded]))
            loaded++;
    }
    xpad_g_nbank = loaded;

    snprintf(xpad_g_root, sizeof(xpad_g_root), "%s", root);
    xpad_g_rate = rate;
    __atomic_store_n(&xpad_g_live, 1, __ATOMIC_RELEASE);

    MDBG("xpad: %d bank%s from %s at %d Hz\n",
         loaded, loaded == 1 ? "" : "s", dir, rate);
    for (i = 0; i < loaded; i++)
        MDBG("xpad:   %c = %s (%lld frames, %.2f s)\n",
             'A' + i, xpad_g_bank[i].name, (long long)xpad_g_bank[i].frames,
             (double)xpad_g_bank[i].frames / rate);
}

/* Called from the worker's idle branch. When nothing has changed this is one
 * integer compare and one string compare. */
void xpad_bank_poll(void)
{
    char root[XP_PATH_MAX];
    int rate = stem_pool_rate();

    if (rate <= 0)
        return;                     /* no timeline to decode onto yet */
    if (!stem_media_first_root(root, sizeof(root))) {
        if (xpad_g_root[0]) {
            MDBG("xpad: media gone -> banks dropped\n");
            xpad_unload();
            xpad_g_root[0] = '\0';
            xpad_g_rate = 0;
        }
        return;
    }
    if (rate == xpad_g_rate && strcmp(root, xpad_g_root) == 0)
        return;
    xpad_scan(root, rate);
}
