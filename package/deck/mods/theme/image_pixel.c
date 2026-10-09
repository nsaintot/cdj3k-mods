// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * mods/theme/image_pixel.c - reading a sprite's pixels, and deciding what it is.
 */
#include "theme/image_internal.h"

#define WAVE_CENSUS_MAX 64

uint64_t theme_fingerprint(const struct theme_bitmapdata *bd, int32_t w, int32_t h)
{
    /* The overview can have a 3-byte pixel stride, where a 32-bit load at the
     * last pixel reads one byte past the buffer, so pixels are assembled bytewise. For
     * 4-byte pixels the result equals a 32-bit load. */
    const int wide = bd->pixel_stride >= 4;
    uint64_t fp = 1469598103934665603ull;          /* FNV-1a offset basis */
    int64_t npx = (int64_t)w * (int64_t)h;
    int64_t step = npx > THEME_FP_SAMPLES ? npx / THEME_FP_SAMPLES : 1;
    int64_t i;

    if (npx <= 0 || bd->pixel_stride < 3) return 0;
    if (!(step & 1)) step++;                        /* keep it odd */
    for (i = 0; i < npx; i += step) {
        const uint8_t *p = bd->data + (i / w) * (size_t)bd->line_stride +
                                      (i % w) * (size_t)bd->pixel_stride;
        uint32_t px = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);

        if (wide) px |= (uint32_t)p[3] << 24;
        fp = (fp ^ px) * 1099511628211ull;
    }
    /* Geometry is hashed too: a buffer reused at another size is a different image, and
     * restoring the old byte count into it would corrupt memory. */
    fp = (fp ^ (uint64_t)(uint32_t)w) * 1099511628211ull;
    fp = (fp ^ (uint64_t)(uint32_t)h) * 1099511628211ull;
    return fp;
}

/* Sprite-sized, or a full-width banner (see THEME_BANNER_* in image_internal.h). */
int theme_sprite_size(int32_t w, int32_t h)
{
    if (w <= 0 || h <= 0) return 0;
    if (w <= THEME_IMG_MAXDIM && h <= THEME_IMG_MAXDIM) return 1;
    return w <= THEME_BANNER_MAXW && h <= THEME_BANNER_MAXH &&
           w >= THEME_BANNER_RATIO * h;
}

/* Fetch the pixels. Only called for a vptr-checked SoftwarePixelData.
 * initialiseBitmapData fills in data/format/strides but not width/height (the
 * BitmapData constructor sets those), so we supply them. */
int theme_bitmap(void *pd, int32_t w, int32_t h, struct theme_bitmapdata *bd)
{
    uintptr_t vt = *(uintptr_t *)pd;
    initbitmap_t init = (initbitmap_t)(*(uintptr_t *)(vt + IPD_VT_INITBITMAP * 8));

    memset(bd, 0, sizeof(*bd));
    init(pd, bd, 0, 0, 2 /* readWrite */);
    bd->width = w;
    bd->height = h;
    return bd->data != NULL && bd->line_stride > 0 && bd->pixel_stride > 0 &&
           w > 0 && h > 0;
}

/* Wide and short: the waveform and the beat-grid rulers that frame it. The rulers are
 * ARGB, so shape is the test, not format. Artwork fails on height or aspect; sprites
 * never get past the width test. */
int theme_is_waveform(const void *pd, int32_t *pw, int32_t *ph, int *palpha)
{
    int32_t fmt, w, h;

    if (*(const uintptr_t *)pd != THEME_VT_SOFTPIXELDATA) return 0;
    fmt = *(int32_t *)((uintptr_t)pd + IPD_PIXELFORMAT_OFF);
    w   = *(int32_t *)((uintptr_t)pd + IPD_WIDTH_OFF);
    h   = *(int32_t *)((uintptr_t)pd + IPD_HEIGHT_OFF);
    if ((fmt != PIXFMT_RGB && fmt != PIXFMT_ARGB) || w <= 0 || h <= 0) return 0;
    if (w <= THEME_IMG_MAXDIM) return 0;                      /* small -> sprite path */
    if (h > THEME_WAVE_MAX_H) return 0;                       /* tall -> artwork */
    if (w < THEME_WAVE_ASPECT * h) return 0;                  /* squarish -> artwork */
    *pw = w; *ph = h; *palpha = (fmt == PIXFMT_ARGB);
    return 1;
}

/* Debug: log every buffer wide enough to reach the waveform classifier, once per shape.
 * Width is not in the key, since a partly occluded repaint gives a narrower slice of the
 * same buffer.
 *
 * Mainly checks line_stride against w*pixel_stride: the loops take a buffer's length to
 * be line_stride*h, so a wrong reported pitch makes the restoring memcpy write memory we
 * do not own. */
void theme_img_probe(const void *pd, const struct theme_bitmapdata *bd,
                            int32_t w, int32_t h, int32_t fmt, const char *verdict)
{
    static struct { uintptr_t vt; int32_t h, fmt, ls, ps; } seen[32];
    static int n;
    uintptr_t vt = *(const uintptr_t *)pd;
    int32_t ls = bd ? bd->line_stride : 0, ps = bd ? bd->pixel_stride : 0;
    int i;

    if (!MLOG_AT(MOD_LOG_DEBUG)) return;
    for (i = 0; i < n; i++)
        if (seen[i].vt == vt && seen[i].h == h && seen[i].fmt == fmt &&
            seen[i].ls == ls && seen[i].ps == ps) return;
    if (n == (int)(sizeof(seen) / sizeof(seen[0]))) return;
    seen[n].vt = vt; seen[n].h = h; seen[n].fmt = fmt;
    seen[n].ls = ls; seen[n].ps = ps;
    n++;

    MDBG("theme: img %dx%d fmt%d vt=%#lx stride=%d pix=%d w*pix=%d%s -> %s\n",
         (int)w, (int)h, (int)fmt, (unsigned long)vt, (int)ls, (int)ps,
         (int)(w * ps), (ls && w * ps > ls) ? " OVERRUN" : "", verdict);
}

/* Debug: count the distinct colours in a strip as it arrives, before we touch it, and
 * print the most frequent. A full pass, so callers run it once per shape. Counts instead
 * of sampling, since a sample only gives a lower bound. */
void theme_wave_census(const struct theme_bitmapdata *bd, int32_t w, int32_t h)
{
    struct wave_bin { uint32_t rgb; uint32_t n; } c[WAVE_CENSUS_MAX];
    int nc = 0, over = 0, i, j;
    int32_t x, y;

    for (y = 0; y < h; y++) {
        const uint8_t *row = bd->data + (size_t)y * bd->line_stride;

        for (x = 0; x < w; x++) {
            const uint8_t *p = row + (size_t)x * bd->pixel_stride;
            uint32_t v = ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];

            for (i = 0; i < nc; i++)
                if (c[i].rgb == v) { c[i].n++; break; }
            if (i == nc) {
                if (nc == WAVE_CENSUS_MAX) { over++; continue; }
                c[nc].rgb = v; c[nc].n = 1; nc++;
            }
        }
    }

    for (i = 1; i < nc; i++) {                      /* by frequency, descending */
        struct wave_bin t = c[i];
        for (j = i; j > 0 && c[j - 1].n < t.n; j--) c[j] = c[j - 1];
        c[j] = t;
    }

    MDBG("theme: wave census %dx%d -- %d distinct%s\n", (int)w, (int)h, nc,
         over ? " (CAPPED, more exist)" : "");
    for (i = 0; i < nc && i < 12; i++)
        MDBG("theme:   #%06x  x%u  (%u%%)\n", c[i].rgb, c[i].n,
             (unsigned)(100ull * c[i].n / ((uint64_t)w * (uint64_t)h)));
}
