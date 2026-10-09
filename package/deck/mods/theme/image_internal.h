/* SPDX-License-Identifier: MIT OR Apache-2.0 */
/*
 * mods/theme/image_internal.h - declarations shared by the theme's image files.
 *
 * image.c owns the drawImage and paint hooks, image_sync.c brings one image in
 * line with the theme, image_pixel.c reads a sprite's pixels and classifies it.
 * The pixel helpers are inline here: they run once per pixel of every waveform
 * frame.
 */
#ifndef EP122_MOD_THEME_IMAGE_INTERNAL_H
#define EP122_MOD_THEME_IMAGE_INTERNAL_H

#include "theme/theme.h"

/* juce::SoftwarePixelData, and initialiseBitmapData as ImagePixelData virtual
 * index 5. Pixels are touched only behind an exact vptr match:
 * SoftwarePixelData::initialiseBitmapData is the one implementation that only
 * fills the struct in (no BitmapDataReleaser), so a zeroed stack BitmapData
 * needs no destructor. Other image types are left alone. */
#define THEME_VT_SOFTPIXELDATA  ep122_sym(EP122_SOFT_PIXEL_DATA)

#define IPD_VT_INITBITMAP       5

/* juce::ImagePixelData fields. The base ReferenceCountedObject is vptr + a bare
 * Atomic<int> refCount with no tail padding, so pixelFormat packs into the same
 * 8-byte word at +0x0c and the dimensions follow at +0x10/+0x14; NamedValueSet
 * userData starts at +0x18. pixelFormat is always 1..3. */
#define IPD_PIXELFORMAT_OFF  0x0c

#define IPD_WIDTH_OFF        0x10

#define IPD_HEIGHT_OFF       0x14

/* juce::Image::PixelFormat */
#define PIXFMT_RGB           1

#define PIXFMT_ARGB          2

/*
 * Baked skin PNGs (the timer and tempo sprite sheets, buttons, badges, icons)
 * are blitted and never reach setFill, so they are handled here.
 *
 * Album art and the waveform must not be recoloured, so each image is classified
 * once, on first sight, and the verdict cached against its ImagePixelData. Chrome
 * is an ARGB sprite of sprite-like size; artwork is opaque RGB, and the waveform
 * panel is far wider than any sprite.
 *
 * Each classified sprite keeps a private copy of its original pixels. Recolouring
 * always reads from that copy and switching back to ORIGINAL is a memcpy, so
 * restoring is exact for any transform, including non-involutions such as
 * sat_darken.
 *
 * Adopted sprites are also pinned (refcount bumped once). Otherwise a freed
 * ImagePixelData whose address is reused would match a stale entry, recolouring
 * the wrong image or calling a vtable on an object of another type. The cost is a
 * few MB of sprites JUCE meant to keep cached anyway.
 */
/* Every image seen takes a slot, adopted or not, and late-opened panes (beat loop,
 * beat jump, key shift) each bring their own; an idle deck already shows ~280. A slot
 * is ~40 bytes, and overflow silently leaves whole panes unthemed. */
#define THEME_IMG_MAX     2048

#define THEME_IMG_MAXDIM  256           /* wider/taller than any sprite -> artwork */

#define THEME_COPY_BUDGET (24u << 20)   /* cap on retained originals */

/* The part of the budget live content may hold. Sprites are authored and finite (about
 * 1 MB in all); live strips are baked, one new buffer per track load and grid state, with
 * no bound, and in a shared pool would spend the whole budget within minutes and leave
 * later images unadopted. So live content has its own ceiling and eviction.
 *
 * It must hold the visible working set. A strip averages ~590 KB, and a browse page
 * shows more preview rows than 4 MB holds, plus the
 * extended overview, beat-grid rulers and current preview. Evicting on-screen strips makes
 * every scroll re-adopt and re-map them (correct, since img_restore makes evict/re-adopt
 * idempotent, but a memcpy and a palette pass per row). 12 MB holds about twenty strips
 * and leaves twelve for sprites. */
#define THEME_LIVE_BUDGET (12u << 20)

/* Full-width chrome counts as a sprite, e.g. the software keyboard's 1280x284 ARGB
 * backdrop (capping both dimensions at 256 left it stock behind a themed middle row).
 * Shape keeps artwork out: a banner is at least 3:1, while album art and logos are
 * square-ish (the 320x240 ARGB image on screen stays excluded at 1.33:1). The ARGB test
 * separates sprites from photos, theme_is_waveform claims the waveform earlier, and the
 * copy budget bounds what is retained. */
#define THEME_BANNER_MAXW  2048

#define THEME_BANNER_MAXH  512

#define THEME_BANNER_RATIO 3

#define IPD_REFCOUNT_OFF  0x08

/* ---- the blit's destination ----
 *
 * juce::LowLevelGraphicsSoftwareRenderer holds its current SavedState at +0x08, and the
 * state names the destination image's ImagePixelData at +0x68.
 *
 * The chain is walked only through mod_safe_read and trusted only when it lands on a
 * SoftwarePixelData. The screen is not one (it has a different vptr), so this separates
 * offscreen blits from blits to the screen. If a firmware moves either offset, the read
 * is not a SoftwarePixelData and the caller declines, without faulting. */
#define GFXCTX_STATE_OFF    0x08

#define GFXSTATE_IMAGE_OFF  0x68

/* ---- has the image changed since we recoloured it? ----
 *
 * The sprite cache assumes pixels never change: snapshot once, recolour once, and
 * `applied` short-circuits every repaint. The extended overview waveform breaks that: it
 * is a 100x58 image (scaled ~10x on screen), sprite-sized by shape, and rewritten on
 * every track load. Without a check the new track shows stock colours and the kept
 * copy holds the previous track's pixels, which a forced re-apply would write back.
 *
 * So we fingerprint what we leave behind; if the buffer differs next time, the app has
 * repainted and the snapshot is stale.
 *
 * A fixed number of samples regardless of size, so the cost does not scale with the
 * 1280x284 banners, at a stride that is not a power of two so regular patterns in the
 * image cannot alias with the sampling. */
#define THEME_FP_SAMPLES 192

/* Values of `applied` that are not a theme id.
 *
 * STOCK: the app's own pixels, nothing of ours. MIXED: our output with a fresh repaint
 * through it; the pristine copy has been reconciled against the buffer, but the buffer
 * still carries the transform. Treating MIXED as STOCK would make the next snapshot take
 * our own output for the original. */
#define THEME_APPLIED_STOCK (-1)
#define THEME_APPLIED_MIXED (-2)


/*
 * The waveform is a wide opaque RGB buffer the app repaints every frame as the
 * track scrolls, so a cached "already done" flag would be wiped by the next repaint
 * and re-applying on our own output would compound. It is handled statelessly:
 * recolour, let the blit happen, put the original back. That is correct whether or
 * not the app rewrote the buffer and keeps no pointer that could go stale.
 *
 * Album art is also opaque RGB; shape separates them: strips are wide and short,
 * artwork is roughly square. Both tests are needed:
 *
 *   height  - stable under occlusion. When a popup covers part of the waveform
 *             the app repaints only the exposed slice, so the width shrinks
 *             while the height does not (the MENU shortcut overlay at x=712
 *             leaves a 712x190 slice, which a 4:1 test would reject).
 *   aspect  - a height ceiling alone would catch short artwork. Loose (2:1) so
 *             a clipped strip still passes; square cover art does not.
 */
#define THEME_WAVE_ASPECT  2       /* width >= 2*height; loose, see above */

#define THEME_WAVE_MAX_H   256     /* strips are short; artwork is tall too */

/* juce::Image::BitmapData: data, pixelFormat, lineStride, pixelStride, w, h.
 * Over-sized and zeroed so the tail (the releaser slot) is NULL. */
struct theme_bitmapdata {
    uint8_t *data;
    int32_t  pixel_format;
    int32_t  line_stride;
    int32_t  pixel_stride;
    int32_t  width;
    int32_t  height;
    uint8_t  tail[32];
};

typedef void (*drawimg_t)(void *ctx, const void *image, const void *transform);

typedef void (*initbitmap_t)(void *pd, void *bitmap, int x, int y, int mode);

/*
 * When the pixels are read, which only the caller knows.
 *
 *   NOW    the blit happens inside the call being wrapped (drawImage only). A live
 *          buffer can be recoloured, blitted and restored, which is correct however
 *          often the app rewrites it and leaves no state to go stale.
 *   LATER  the pixels are read after we return: a FillType image by the fillRect that
 *          follows setFill, a baked overview frames later. Nothing can be restored in
 *          time, so the recolour persists and the fingerprint detects repaints.
 *
 * The extended overview needs LATER: it is a live buffer by shape but arrives through
 * setFill, not drawImage.
 */
enum theme_blit { THEME_BLIT_NOW, THEME_BLIT_LATER };

/* Is this pixel's alpha real coverage?
 *
 * JUCE stores ARGB premultiplied, so every colour channel must be <= alpha. A pixel that
 * breaks this cannot be premultiplied: its alpha byte was never filled in and the colour
 * is opaque. No valid premultiplied pixel fails the test.
 *
 * The extended overview (a 1024x66 ARGB strip blitted with alpha ignored) needs it: most
 * pixels carry alpha 0 over real colour, which would be skipped as transparent, and the
 * rest carry small non-zero alphas, which unpremultiplying blows up to white and back to
 * near-black (a dark cross-hatch through the ink). theme_map_pixel also writes the alpha
 * byte for such pixels; see the note there. */
static inline int theme_alpha_is_coverage(const uint8_t *p, int has_alpha)
{
    uint32_t a = p[3];

    return has_alpha && p[0] <= a && p[1] <= a && p[2] <= a;
}

/* Unpremultiply, map, re-premultiply. JUCE stores ARGB premultiplied, in memory order
 * B,G,R,A; RGB is B,G,R. Mapping premultiplied channels directly would darken soft edges.
 * The palette is passed in since it cannot change within one image. */
static inline void theme_map_pixel(const struct theme_palette *pal, uint8_t *p,
                                   int has_alpha)
{
    uint32_t r, g, b;
    /* Effective alpha: 255 where the byte is not coverage, since the colour is then
     * already final. */
    int opaque = !theme_alpha_is_coverage(p, has_alpha);
    uint32_t a = opaque ? 255u : p[3];

    if (a == 0) return;                       /* premultiplied transparent: nothing there */
    if (a == 255u) {
        /* Packed form: one call per pixel of every waveform frame, so the colour stays
         * in registers instead of going through three pointers. */
        uint32_t v = theme_palette_map(pal, ((uint32_t)p[2] << 16) |
                                            ((uint32_t)p[1] << 8) | p[0],
                                       0 /* image: blue is ink, darken it */);
        r = v >> 16; g = (v >> 8) & 0xffu; b = v & 0xffu;
    } else {
        uint32_t v;

        r = (uint32_t)p[2] * 255u / a;        /* unpremultiply */
        g = (uint32_t)p[1] * 255u / a;
        b = (uint32_t)p[0] * 255u / a;
        if (r > 255u) r = 255u;
        if (g > 255u) g = 255u;
        if (b > 255u) b = 255u;
        v = theme_palette_map(pal, (r << 16) | (g << 8) | b, 0 /* image */);
        r = v >> 16; g = (v >> 8) & 0xffu; b = v & 0xffu;
        r = r * a / 255u; g = g * a / 255u; b = b * a / 255u;
    }
    p[2] = (uint8_t)r; p[1] = (uint8_t)g; p[0] = (uint8_t)b;

    /* Write alpha 255. A premultiplied source composites as `dst = src + dst*(1 - srcA)`,
     * so alpha 0 is additive, not invisible. On a black backdrop that makes no difference;
     * on a light theme's white backdrop the strip clips to white.
     * Safe because the test above found this alpha is not coverage. Guarded on has_alpha because
     * in an RGB buffer p[3] is the next pixel's blue. */
    if (opaque && has_alpha) p[3] = 255u;
}

/* The retained originals: one entry per image the theme has adopted. */
struct theme_img_slot {
    const void *pd;
    uint8_t     chrome;      /* passed classification -> we may recolour it */
    /* Baked, not authored: one buffer per track load or grid state, superseded and
     * never drawn again. Only these are evictable; an evicted sprite would be re-adopted
     * on its next draw for negligible memory saved. */
    uint8_t     live;
    /* When this was last sighted, off g_img_clock. Sets the eviction order. */
    uint32_t    seen;
    /* The theme these pixels currently carry: a theme id, or THEME_APPLIED_STOCK /
     * THEME_APPLIED_MIXED. A bool is not enough: switching palette to palette must
     * re-apply. */
    int16_t     applied;
    uint8_t    *orig;        /* pristine pixels */
    size_t      bytes;
    int32_t     w, h;
    /* The pixel buffer we adopted, used as an identity check. The app recycles pd
     * addresses, so a slot can match a pd that is now a different object; a different
     * allocation has a different data address. */
    const uint8_t *data;
    /* Recorded for every entry, adopted or refused, so a refusal is re-evaluated when the
     * address holds a different image. */
    int16_t     fmt;
    /* We hold one reference on pd. Not inferred from `chrome`, which a slot can lose
     * while the reference is outstanding. */
    uint8_t     pinned;
    /* Fingerprint of what we left in the buffer. See theme_fingerprint. */
    uint64_t    stamp;
};

extern struct theme_img_slot g_img[THEME_IMG_MAX];
extern int    g_img_n;
extern size_t g_img_bytes;

int  theme_sprite_size(int32_t w, int32_t h);
int  theme_bitmap(void *pd, int32_t w, int32_t h, struct theme_bitmapdata *bd);
int  theme_is_waveform(const void *pd, int32_t *pw, int32_t *ph, int *palpha);
uint64_t theme_fingerprint(const struct theme_bitmapdata *bd, int32_t w, int32_t h);
void theme_img_probe(const void *pd, const struct theme_bitmapdata *bd,
                            int32_t w, int32_t h, int32_t fmt, const char *verdict);
void theme_wave_census(const struct theme_bitmapdata *bd, int32_t w, int32_t h);
void theme_sync_image_x(const void *image, enum theme_blit when);

/* Has the theme adopted this image, and may it be recoloured? */
int theme_img_adopted(const void *pd);

/* Lend the blit about to run this sprite's stock pixels. See image_sync.c. */
int  theme_img_lend_stock(const void *image, const void *ctx);

#endif /* EP122_MOD_THEME_IMAGE_INTERNAL_H */
