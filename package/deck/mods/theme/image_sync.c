// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * mods/theme/image_sync.c - bring one image in line with the theme in force.
 */
#include "theme/image_internal.h"
#include <pthread.h>

struct theme_img_slot g_img[THEME_IMG_MAX];
int    g_img_n;
size_t g_img_bytes;

/* The table is written from two threads, and a slot's fields are only consistent together.
 *
 * The message thread arrives through drawImage and setFill; the database reply thread
 * through the waveform replyers, which bake an overview per track (hence g_theme_in_bake
 * is __thread). A bake is a first sighting, so it runs img_evict_live, and eviction
 * compacts: img_release moves the last slot into the freed index and zeroes the tail. A
 * message-thread sighting holds a bare index across the fingerprint, reconcile and map
 * pass, so a concurrent bake could move that slot.
 *
 * The result would be `applied` and `orig` describing different images: img_reconcile
 * maps orig with the wrong palette, treats every pixel as the app's, and snapshots our
 * own output as the pristine copy, which is then undetectable (a lightness inversion on
 * neutral grey is an involution).
 *
 * Held across the whole sighting: classification, identity tests, copy and map are one
 * transaction. Nothing inside blocks or re-enters, so a plain mutex suffices; libpthread
 * is already a DT_NEEDED. */
static pthread_mutex_t g_img_lock = PTHREAD_MUTEX_INITIALIZER;

/* Has a palette-bearing theme been selected at least once? Only ever goes 0 -> 1. */
static int g_img_ever_on;

/* Bytes held by live content, and a sighting counter for eviction order. A wrap after
 * four billion sightings only costs one round of evictions in the wrong order. */
static size_t   g_img_live_bytes;
static uint32_t g_img_clock;

/* Put the pixels back, if what is in the buffer is still our doing.
 *
 * Invariant: a buffer we stop tracking must not be left carrying our transform. `orig` is
 * the only record of what was underneath; without it the next sighting adopts the buffer
 * afresh, snapshots our output as the original and maps it again. Each evict/re-adopt
 * cycle (e.g. browse previews on a long list) would then add one more palette pass. N
 * passes of WHITE:
 *
 *   passes    0        1        2        3        4        5        6
 *   ground    000000   ffffff   000000   ffffff   000000   ffffff   000000
 *   peaks     ffffff   000000   ffffff   000000   ffffff   000000   ffffff
 *   orange    ffa600   b27400   c9983c   a2792d   b59150   9a7a41   aa8d5a
 *   blue      0055e1   1654bb   3668bb   3963a9   4a6fab   4a6ba0   5573a2
 *
 * The background flips between white and black (a 2-cycle) while the ink converges to a
 * fixed point and gets muddier each cycle. Dark palettes move only 3..32 in their worst
 * channel on a second pass (WHITE: 255), so mostly WHITE shows it.
 *
 * Must run before the pin is dropped: the pin is what makes this pointer safe to follow.
 *
 * Every guard is re-read from the object, not trusted from adoption time. STOCK means
 * nothing of ours to put back; a changed vptr, geometry or buffer address means it is no
 * longer the object the copy describes.
 *
 * MIXED is not declined: its copy was just reconciled against the buffer and the
 * transform is still there to remove. */
static void img_restore(struct theme_img_slot *e)
{
    /* Its own BitmapData: the caller may be part-way through filling one in for the
     * image being adopted. */
    struct theme_bitmapdata bd;

    if (e->applied == THEME_APPLIED_STOCK || e->orig == NULL || e->pd == NULL) return;
    if (*(const uintptr_t *)e->pd != THEME_VT_SOFTPIXELDATA) return;
    if (*(int32_t *)((uintptr_t)e->pd + IPD_WIDTH_OFF)  != e->w ||
        *(int32_t *)((uintptr_t)e->pd + IPD_HEIGHT_OFF) != e->h) return;
    if (!theme_bitmap((void *)(uintptr_t)e->pd, e->w, e->h, &bd)) return;
    if ((size_t)bd.line_stride * (size_t)e->h != e->bytes) return;
    /* The checks above can all pass for a different image at this address; writing
     * e->bytes into it would overflow the heap. */
    if (bd.data != e->data) return;
    memcpy(bd.data, e->orig, e->bytes);
    e->applied = THEME_APPLIED_STOCK;
}

/* How many pixels img_ground_themed samples. It runs on adoption and after a reconcile,
 * never per frame, so it samples the whole picture. */
#define IMG_GROUND_SAMPLES 1024

/* Does this buffer already carry the source route's themed ground?
 *
 * Two ARGB surfaces reach the adopted path with the same shape. A beat-grid ruler arrives
 * stock and must be mapped. A browse preview is baked through the themed colour tables, so
 * it already has the themed ground, and mapping it again would turn a white ground black.
 *
 * Whichever ground covers more of the picture is the one the strip was baked with; the
 * ground is most of a strip, so the count is not close. A buffer matching neither is
 * mapped.
 *
 * Sampled across the whole buffer, not the edges: a loud track's waveform fills the top
 * and bottom rows, and under WHITE a stock peak (0xffffff) equals the themed ground. */
static int img_ground_themed(const struct theme_bitmapdata *bd, int32_t w, int32_t h)
{
    const uint32_t themed = theme_wave_ground();
    const uint32_t stock  = theme_wave_ground_stock();
    int64_t npx = (int64_t)w * (int64_t)h, step, i;
    int hit_themed = 0, hit_stock = 0;

    /* Equal on a theme whose ground stays black; a second pass leaves it black anyway. */
    if (themed == 0 || themed == stock || npx <= 0 || bd->pixel_stride < 3) return 0;
    step = npx > IMG_GROUND_SAMPLES ? npx / IMG_GROUND_SAMPLES : 1;
    for (i = 0; i < npx; i += step) {
        const uint8_t *p = bd->data + (i / w) * (size_t)bd->line_stride
                                    + (i % w) * (size_t)bd->pixel_stride;
        uint32_t c = ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];

        if (c == themed)     hit_themed++;
        else if (c == stock) hit_stock++;
    }
    return hit_themed > hit_stock;
}

/* Give a slot back, in four steps:
 *   1. restore the pixels (img_restore), while the pin still holds;
 *   2. free our copy;
 *   3. drop the pin, so the app's own release can free the buffer (otherwise a 450 KB
 *      original stays pinned for ever);
 *   4. remove the slot: once unpinned the address can be recycled, and a stale entry
 *      would match the new image.
 *
 * The last slot is swapped in; no index is held across a call, since both scans complete
 * within one sighting. */
static void img_release(int k)
{
    struct theme_img_slot *e = &g_img[k];

    if (e->chrome) {
        img_restore(e);
        g_img_bytes -= e->bytes;
        if (e->live) g_img_live_bytes -= e->bytes;
    }
    /* Keyed on the pin, not chrome: a slot can lose its copy while keeping its reference,
     * which would leak the app's buffer. */
    if (e->pinned)
        __atomic_fetch_sub((int *)((uintptr_t)e->pd + IPD_REFCOUNT_OFF), 1,
                           __ATOMIC_RELAXED);
    free(e->orig);
    g_img_n--;
    if (k != g_img_n) g_img[k] = g_img[g_img_n];
    memset(&g_img[g_img_n], 0, sizeof(g_img[g_img_n]));
}

/* Which pixels of this buffer are still ours?
 *
 * map(orig) is exactly what we left, so a pixel that still equals it keeps its pristine
 * value, and one that differs was written by the app and becomes the new pristine. Per
 * pixel, because app and our pixels interleave in every row.
 *
 * Mapped with the palette named by `applied`, not the current one: after a theme switch
 * the current palette would mark every pixel as changed. A negative `applied` means orig
 * itself is in the buffer, so the compare is direct. */
static void img_reconcile(struct theme_img_slot *e, const struct theme_bitmapdata *bd)
{
    const struct theme_palette *was =
        (e->applied >= 0 && e->applied < MOD_THEME_MAX)
            ? k_mod_themes[e->applied].palette : NULL;
    const int has_alpha = (bd->pixel_format == PIXFMT_ARGB);
    const int n = bd->pixel_stride > 4 ? 4 : bd->pixel_stride;
    int32_t x, y;

    for (y = 0; y < e->h; y++) {
        uint8_t *brow = bd->data + (size_t)y * (size_t)bd->line_stride;
        uint8_t *orow = e->orig  + (size_t)y * (size_t)bd->line_stride;

        for (x = 0; x < e->w; x++) {
            uint8_t *bp = brow + (size_t)x * (size_t)bd->pixel_stride;
            uint8_t *op = orow + (size_t)x * (size_t)bd->pixel_stride;
            /* p[3] is only read behind has_alpha; zeroed for RGB instead of taking the
             * next pixel's blue. */
            uint8_t mine[4];
            int k;

            mine[0] = op[0]; mine[1] = op[1]; mine[2] = op[2];
            mine[3] = has_alpha ? op[3] : 0;
            if (was) theme_map_pixel(was, mine, has_alpha);

            for (k = 0; k < n; k++)
                if (bp[k] != mine[k]) break;
            if (k == n)
                continue;                          /* still ours: keep the pristine */
            for (k = 0; k < n; k++)
                op[k] = bp[k];                     /* the app wrote here */
        }
    }
}

/* Make room for `want` bytes of live content, oldest sighting first.
 *
 * Only live slots are candidates; an evicted sprite would be re-adopted on its next draw.
 * Linear, but it runs once per bake (track load or grid change) over a few hundred slots. */
static void img_evict_live(size_t want)
{
    while (g_img_live_bytes + want > THEME_LIVE_BUDGET) {
        uint32_t oldest = 0xffffffffu;
        int k, victim = -1;

        for (k = 0; k < g_img_n; k++)
            if (g_img[k].chrome && g_img[k].live && g_img[k].seen <= oldest) {
                oldest = g_img[k].seen;
                victim = k;
            }
        if (victim < 0)
            return;               /* nothing live left: the caller goes unadopted */
        MDBG("theme: evicting live %dx%d (%zuK, seen %u) for %zuK\n",
             (int)g_img[victim].w, (int)g_img[victim].h, g_img[victim].bytes >> 10,
             g_img[victim].seen, want >> 10);
        img_release(victim);
    }
}


/* Bring one image in line with the theme in force.
 *
 * Does nothing until a theme that maps images has been selected at least once, so
 * ORIGINAL never catalogues images (each would cost a slot, a pixel copy, a pin and a
 * log line). Callers still reach here under ORIGINAL, since that is how a themed image
 * is put back.
 *
 * Selecting a theme repaints the whole UI, so every visible image comes through here
 * with stock pixels, which is when the original is captured. `ever_on` only goes
 * 0 -> 1, so a race with a toggle on another thread costs at most one extra pass. */
void theme_sync_image(const void *image)
{
    theme_sync_image_x(image, THEME_BLIT_NOW);
}

/* setFill's image, and the baked overviews. See enum theme_blit in image_internal.h. */
void theme_sync_image_deferred(const void *image)
{
    theme_sync_image_x(image, THEME_BLIT_LATER);
}

/* Has this image been adopted for recolouring? The stateless path checks this so the two
 * treatments never apply to the same buffer (that would double the transform). */
int theme_img_adopted(const void *pd)
{
    int i, r = 0;

    /* Locked: a bake on the database reply thread can be compacting the table. */
    pthread_mutex_lock(&g_img_lock);
    for (i = 0; i < g_img_n; i++)
        if (g_img[i].pd == pd) { r = g_img[i].chrome; break; }
    pthread_mutex_unlock(&g_img_lock);
    return r;
}

/* Is this blit's destination one we will theme ourselves?
 *
 * See GFXCTX_STATE_OFF for the chain and why walking it cannot fault. Yes only for a
 * destination this file would adopt as a sprite; any other surface must keep receiving
 * themed pixels or it stays stock. Waveform surfaces are handled by the source route and
 * nothing blits a sprite into one. */
static int img_dest_is_ours(const void *ctx, int32_t *pdw, int32_t *pdh)
{
    uintptr_t state = 0, dst = 0, vt = 0;
    int32_t dw = 0, dh = 0, df = 0;

    if (ctx == NULL) return 0;
    if (mod_safe_read((uintptr_t)ctx + GFXCTX_STATE_OFF, &state, sizeof(state)) != 0)
        return 0;
    if (mod_safe_read(state + GFXSTATE_IMAGE_OFF, &dst, sizeof(dst)) != 0)
        return 0;
    if (mod_safe_read(dst, &vt, sizeof(vt)) != 0 || vt != THEME_VT_SOFTPIXELDATA)
        return 0;
    if (mod_safe_read(dst + IPD_PIXELFORMAT_OFF, &df, sizeof(df)) != 0 ||
        mod_safe_read(dst + IPD_WIDTH_OFF,  &dw, sizeof(dw)) != 0 ||
        mod_safe_read(dst + IPD_HEIGHT_OFF, &dh, sizeof(dh)) != 0)
        return 0;
    *pdw = dw;
    *pdh = dh;
    return df == PIXFMT_ARGB && theme_sprite_size(dw, dh);
}

/* Give one blit the sprite's stock pixels; returns whether it must be undone.
 *
 * The app builds small images from other images: a browse row's 48x48 artwork cell is the
 * skin's 50x50 img_noArtWork.png rescaled by a drawImage into an offscreen surface. With
 * our transform in the source the copy would be born themed, then adopted and mapped
 * again; on neutral greys a lightness inversion is an involution, so it ends up stock.
 *
 * So offscreen derivations get stock pixels and the screen keeps themed ones; the copy is
 * adopted and mapped once like any sprite. Cheap: img_dest_is_ours rejects every blit to
 * the display, nearly all of them. */
int theme_img_lend_stock(const void *image, const void *ctx)
{
    int32_t dw = 0, dh = 0;
    void *pd;
    int i, lent = 0;

    if (image == NULL || (pd = *(void **)image) == NULL) return 0;
    if (!img_dest_is_ours(ctx, &dw, &dh)) return 0;

    pthread_mutex_lock(&g_img_lock);
    for (i = 0; i < g_img_n; i++) {
        struct theme_img_slot *e = &g_img[i];
        struct theme_bitmapdata bd;

        if (e->pd != pd) continue;
        if (!e->chrome || e->applied == THEME_APPLIED_STOCK) break;
        img_restore(e);                       /* runs all identity tests */
        if (e->applied != THEME_APPLIED_STOCK) break;   /* declined: nothing changed */
        /* Stamped so the next sighting does not take the restore for an app repaint. */
        if (theme_bitmap(pd, e->w, e->h, &bd))
            e->stamp = theme_fingerprint(&bd, e->w, e->h);
        lent = 1;
        MDBG("theme: lending %dx%d stock to a %dx%d offscreen\n",
             (int)e->w, (int)e->h, (int)dw, (int)dh);
        break;
    }
    pthread_mutex_unlock(&g_img_lock);
    return lent;
}

static void img_sync_locked(const void *image, enum theme_blit when)
{
    const struct theme_palette *pal;
    void *pd;
    int32_t w, h, fmt;
    struct theme_bitmapdata bd;
    int i, want, on;

    pal = mod_theme()->palette;
    on = pal != NULL ? 1 : 0;
    if (!on && !g_img_ever_on) return;
    g_img_ever_on |= on;

    if (image == NULL) return;
    pd = *(void **)image;                       /* juce::Image is just the Ptr */
    if (pd == NULL) return;

    fmt = *(int32_t *)((uintptr_t)pd + IPD_PIXELFORMAT_OFF);
    w   = *(int32_t *)((uintptr_t)pd + IPD_WIDTH_OFF);
    h   = *(int32_t *)((uintptr_t)pd + IPD_HEIGHT_OFF);

    for (i = 0; i < g_img_n; i++)
        if (g_img[i].pd == pd) break;

    if (i == g_img_n) {                          /* first sight: classify once */
        int ok, live = 0;

        /* Chrome vs artwork is decided by pixel format. Skin sprites are ARGB (fmt2);
         * decoded photos and the waveform buffer are opaque RGB (album art 240x240 fmt1,
         * waveform 1200x128 fmt1). Do not use colourfulness: a mean-chroma test filed the
         * orange-bordered beat-loop pads as artwork. */
        /* Live buffers (waveform strips, beat-grid rulers: wide, short, ARGB) also pass
         * theme_sprite_size's banner rule (w >= 3h), but their pixels change after the
         * snapshot, e.g. the widget exists before a track loads. So the shape test picks
         * the treatment, and the fingerprint below re-checks the buffer on every pass:
         *
         *   live + NOW    the stateless path owns it: no copy, no hashing, right for a
         *                 buffer redrawn every frame.
         *   live + LATER  cannot be restored in time, so adopted and tracked by
         *                 fingerprint. This is the extended overview: baked once per
         *                 track, blitted through a FillType.
         *   sprite        ARGB, small and static.
         *
         * theme_is_waveform is the live test (looser than the banner rule: 2:1, at most
         * 256 tall), shared with the drawImage path so the two cannot drift apart. */
        {
            int32_t ww, hh; int aa;

            ok = *(uintptr_t *)pd == THEME_VT_SOFTPIXELDATA;
            if (ok) {
                if (theme_is_waveform(pd, &ww, &hh, &aa)) {
                    ok = (when == THEME_BLIT_LATER);
                    /* Stored on the slot for eviction: shape alone cannot tell a strip
                     * from the keyboard backdrop. */
                    live = ok;
                } else {
                    ok = (fmt == PIXFMT_ARGB && theme_sprite_size(w, h));
                }
            }
            ok = ok && theme_bitmap(pd, w, h, &bd);
        }

        {
            size_t bytes = ok ? (size_t)bd.line_stride * (size_t)h : 0;

            /* Evict before choosing the slot index: img_evict_live compacts the table,
             * so an index chosen earlier could land past g_img_n. Only other live content
             * is evicted; a new bake supersedes an old one. */
            if (live && bytes)
                img_evict_live(bytes);

            /* Tested after eviction, which also frees slots. */
            if (g_img_n == THEME_IMG_MAX) {      /* never silently: a full table
                                                  * leaves whole panes unthemed */
                static int warned;
                if (!warned) { warned = 1; MWARN("theme: image table FULL at %d -> "
                                                 "later panes stay untouched\n",
                                                 THEME_IMG_MAX); }
                return;
            }
            i = g_img_n;
            memset(&g_img[i], 0, sizeof(g_img[i]));
            g_img[i].pd = pd;
            /* On every entry, refused ones included: a later sighting checks the address
             * against these, so a refusal is re-evaluated for a different image. */
            g_img[i].w = w;
            g_img[i].h = h;
            g_img[i].fmt = (int16_t)fmt;

            if (bytes && (!live || g_img_live_bytes + bytes <= THEME_LIVE_BUDGET) &&
                g_img_bytes + bytes <= THEME_COPY_BUDGET &&
                (g_img[i].orig = malloc(bytes)) != NULL) {
                memcpy(g_img[i].orig, bd.data, bytes);
                g_img[i].bytes = bytes;
                g_img[i].data = bd.data;
                g_img[i].live = (uint8_t)live;
                g_img_bytes += bytes;
                if (live) g_img_live_bytes += bytes;
                __atomic_fetch_add((int *)((uintptr_t)pd + IPD_REFCOUNT_OFF), 1,
                                   __ATOMIC_RELAXED);      /* pin: see img_release */
                g_img[i].pinned = 1;
                g_img[i].chrome = 1;
            } else if (bytes) {
                /* Live content evicts its own, so reaching this means the sprites
                 * exceed the budget. */
                static int warned;
                if (!warned) { warned = 1; MWARN("theme: copy budget spent at %zuK "
                                                 "(live %zuK) -> later sprites stay "
                                                 "untouched\n",
                                                 (size_t)(g_img_bytes >> 10),
                                                 (size_t)(g_img_live_bytes >> 10)); }
            }
        }
        g_img_n++;
        MDBG("theme: image #%d pd=%p %dx%d fmt%d -> %s\n", g_img_n, pd,
             (int)w, (int)h, (int)fmt, g_img[i].chrome ? "chrome" : "left alone");
    }

    /* Same address, different image; checked before the chrome gate. The app recycles
     * ImagePixelData memory, so a stale verdict is wrong either way: a refusal leaves a
     * themeable surface stock for ever, and an adoption memcpy's a copy of the wrong size
     * into the new buffer, corrupting the heap. */
    if (w != g_img[i].w || h != g_img[i].h || (int16_t)fmt != g_img[i].fmt) {
        MDBG("theme: image pd=%p was %dx%d fmt%d, now %dx%d fmt%d -- dropping the slot\n",
             pd, (int)g_img[i].w, (int)g_img[i].h, (int)g_img[i].fmt,
             (int)w, (int)h, (int)fmt);
        img_release(i);
        return;
    }

    if (!g_img[i].chrome) return;
    /* Sighted: the eviction order is least recently drawn. */
    g_img[i].seen = ++g_img_clock;
    want = on ? __atomic_load_n(&g_theme_id, __ATOMIC_RELAXED) : -1;

    /* Identity tests: first the vptr, since this may no longer be a SoftwarePixelData. */
    if (*(uintptr_t *)pd != THEME_VT_SOFTPIXELDATA) return;

    if (!theme_bitmap(pd, g_img[i].w, g_img[i].h, &bd)) return;
    if ((size_t)bd.line_stride * (size_t)g_img[i].h != g_img[i].bytes) return;
    /* The only test a recycled allocation of the same shape fails: a different data
     * address. */
    if (bd.data != g_img[i].data) {
        MDBG("theme: image pd=%p buffer moved %p -> %p -- dropping the slot\n",
             pd, (const void *)g_img[i].data, (const void *)bd.data);
        img_release(i);
        return;
    }

    /* Has the app repainted this since we left it? If so the pristine copy is stale
     * (usually the previous track) and both branches below would misuse it.
     *
     * The whole buffer cannot be taken as the new pristine: on a long browse list the app
     * recycles a row's buffer and draws the next track's waveform over our themed
     * background, so the snapshot would contain our output and compound on every reuse.
     * img_reconcile re-snapshots only the pixels that differ from map(orig).
     *
     * The fingerprint gates it, so an unchanged buffer (e.g. a paused deck) costs one
     * 192-sample hash. */
    {
        uint64_t now = theme_fingerprint(&bd, g_img[i].w, g_img[i].h);

        if (now != g_img[i].stamp) {
            img_reconcile(&g_img[i], &bd);
            /* MIXED, not STOCK: the copy is good again but our transform is still in the
             * buffer. STOCK would make want < 0 return without restoring, leaving the
             * transform on screen under ORIGINAL. */
            g_img[i].applied = THEME_APPLIED_MIXED;
        }
    }

    if (g_img[i].applied == want) return;

    /* Already themed by the source route: record it as applied without writing. There is
     * no stock copy to go back to; under ORIGINAL the tables revert and the app re-bakes. */
    if (want >= 0 && g_img[i].live && img_ground_themed(&bd, g_img[i].w, g_img[i].h)) {
        g_img[i].applied = (int16_t)want;
        g_img[i].stamp = theme_fingerprint(&bd, g_img[i].w, g_img[i].h);
        return;
    }

    if (want < 0) {
        memcpy(bd.data, g_img[i].orig, g_img[i].bytes);    /* exact restore */
    } else {
        const int has_alpha = (bd.pixel_format == PIXFMT_ARGB);

        memcpy(bd.data, g_img[i].orig, g_img[i].bytes);    /* always map from pristine */
        for (int y = 0; y < g_img[i].h; y++) {
            uint8_t *row = bd.data + (size_t)y * bd.line_stride;
            for (int x = 0; x < g_img[i].w; x++)
                theme_map_pixel(pal, row + (size_t)x * bd.pixel_stride, has_alpha);
        }
    }
    g_img[i].applied = (int16_t)want;
    /* Fingerprint what we leave behind so the next sighting can tell our output from a
     * repaint. After the write, on both branches, or every restore looks like a repaint. */
    g_img[i].stamp = theme_fingerprint(&bd, g_img[i].w, g_img[i].h);
}

void theme_sync_image_x(const void *image, enum theme_blit when)
{
    pthread_mutex_lock(&g_img_lock);
    img_sync_locked(image, when);
    pthread_mutex_unlock(&g_img_lock);
}
