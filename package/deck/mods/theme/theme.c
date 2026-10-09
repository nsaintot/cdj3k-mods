// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * theme.c - the setFill choke point, and install.
 *
 * Alternatives not used
 * ----------------------
 * JUCE's LookAndFeel: the app subclasses it only for table headers and one
 * slider, so LookAndFeel::setColour would miss the deck and utility screens.
 * The stock skin: layout rows in skin16/layout are named `kind_#rrggbb_id`, the
 * same string is a literal in .rodata, and the lookup matches the full name, hex
 * included. Patching works but needs both sides changed in
 * lockstep, the CSVs live on the ramdisk, and colours are resolved at widget
 * construction, so it needs an app restart.
 *
 * Mechanism
 * ---------
 * Every colour the UI paints (rect fills, paths and text glyphs) goes through
 * one pure-virtual choke point:
 *
 *     juce::LowLevelGraphicsContext::setFill (const FillType&)
 *
 * The app renders with juce::LowLevelGraphicsSoftwareRenderer, where setFill is
 * virtual index 18. Repointing that slot recolours the whole interface on the
 * next repaint, so themes switch live.
 *
 * juce::FillType (JUCE 5.3.2):
 *     +0x00  Colour colour        (uint32 ARGB)   <- all we touch
 *     +0x08  unique_ptr<ColourGradient> gradient
 *     +0x10  Image image
 *     +0x18  AffineTransform transform (6 floats)
 *     sizeof == 0x30
 * setFill deep-copies the gradient and ref-counts the image without taking
 * ownership of the object, so we pass a byte-copy on the stack instead of writing
 * through the caller's `const FillType&`, which may live in read-only memory.
 *
 * Images go through image.c: blitted sprites and the waveform via drawImage, and
 * tiled textures via the Image carried inside the FillType. Gradient stops are
 * mapped into a private copy of the ColourGradient (see wrap_setfill).
 */
#include "theme/theme.h"
#include "stem/stem.h"
#include "kit/menu.h"
#include "kit/mod.h"

#define FILLTYPE_SIZE      0x30u
#define FILLTYPE_GRAD_OFF  0x08u   /* unique_ptr<ColourGradient> inside FillType */
#define FILLTYPE_IMAGE_OFF 0x10u   /* juce::Image (a bare Ptr) inside FillType */

/* juce::ColourGradient (5.3.2):
 *     +0x00  Point<float> point1
 *     +0x08  Point<float> point2
 *     +0x10  bool isRadial
 *     +0x18  ColourPoint* elements
 *     +0x20  int numAllocated
 *     +0x28  int numUsed
 *     ColourPoint { double position; Colour colour; }   == 16 bytes, colour at +8
 *
 * numUsed is at +0x28, not the +0x24 that counting fields suggests: Array holds
 * ArrayAllocationBase by value, padded to 16 bytes. +0x24 reads the padding (0). */
#define GRAD_ELEMS_OFF     0x18u
#define GRAD_NUMALLOC_OFF  0x20u
#define GRAD_NUMUSED_OFF   0x28u
#define GRAD_POINT_SIZE    16u
#define GRAD_POINT_COLOUR  8u
/* Capacity of the private stop array. The waveform uses three; a gradient with more
 * than this is left untouched. */
#define GRAD_MAX_STOPS     16

typedef void (*setfill_t)(void *ctx, const void *fill);

static uintptr_t g_orig_setfill;

/* Gradient counters: fills carrying one, those that validated as a ColourGradient, and
 * those substituted. seen>0 with sub==0 means validation rejects them. */
static unsigned g_grad_seen, g_grad_ok, g_grad_sub;

/* ---- who paints the overview (debug) ----
 *
 * Counts fills per drawing function. The strip is hundreds of columns painted in one
 * burst, so its painter stands out from the few fills a button or row costs.
 *
 * Call shape:
 *
 *     WidgetBase::paint -> <the widget's draw> -> Graphics::fillRect -> context fillRect
 *
 * From this wrapper, depth 0 is inside juce::Graphics::fillRect, depth 1 the widget's
 * draw, depth 2 WidgetBase::paint. Use depth 1: depth 2 is shared by every widget. */
static uintptr_t g_orig_fillrect, g_orig_fillrectf, g_orig_fillrectlist, g_orig_fillpath;

/* One step up the AArch64 frame chain per level: x29 holds the caller's frame pointer and
 * the return address sits beside it. Alignment and range are checked at every hop, since
 * a bad chain would fault inside the app's paint.
 *
 * Always inline, and called from the wrapper, not from census(): depth is measured from
 * the frame this expands into, so an extra real call frame shifts every level by one. */
static inline __attribute__((always_inline)) uintptr_t frame_lr(int depth)
{
    uintptr_t fp = (uintptr_t)__builtin_frame_address(0);
    uintptr_t lr = 0;
    int i;

    for (i = 0; i <= depth; i++) {
        if (fp < 0x10000u || (fp & 15u)) return 0;
        lr = *(const uintptr_t *)(fp + 8);
        fp = *(const uintptr_t *)fp;
    }
    return lr;
}

static struct { uintptr_t lr; unsigned n; uint8_t tag, fired; } g_who[128];
static int g_who_n, g_who_burst;
static const char *const k_who_tag[] = { "rect", "rectf", "rectlist", "path" };

static void census(int tag, uintptr_t lr)
{
    int i;

    /* EP122 callers only, by address: the app is non-PIE at 0x400000 and the shim is mapped
     * at 0xffff.... This excludes our own stem-row checkerboard (hundreds of fills), which
     * mod_drawing() does not cover since that bracket spans only setColour. */
    if (lr < 0x400000u || lr > 0x3000000u) return;

    for (i = 0; i < g_who_n; i++)
        if (g_who[i].lr == lr && g_who[i].tag == tag) break;
    if (i == g_who_n && g_who_n < (int)(sizeof(g_who) / sizeof(g_who[0]))) {
        g_who[g_who_n].lr = lr; g_who[g_who_n].n = 0;
        g_who[g_who_n].tag = (uint8_t)tag; g_who_n++;
    }
    if (i >= g_who_n) return;
    g_who[i].n++;

    /* Cumulative, never reset. A reset on a burst threshold clips every painter at that
     * threshold; totals over time separate a per-column strip from a button that fills
     * once. */
    if (((++g_who_burst) & 0x1fff) == 0) {
        int j;

        MDBG("theme: --- draw totals @%d ---\n", g_who_burst);
        for (j = 0; j < g_who_n; j++)
            if (g_who[j].n >= 200)
                MDBG("theme: %s draw %#lx x%u\n", k_who_tag[g_who[j].tag],
                     (unsigned long)g_who[j].lr, g_who[j].n);
    }
}

static void wrap_fillrect(void *ctx, const void *a, int r, int64_t v)
{
    census(0, frame_lr(1));
    ((void (*)(void *, const void *, int, int64_t))g_orig_fillrect)(ctx, a, r, v);
}
static void wrap_fillrectf(void *ctx, const void *a)
{
    census(1, frame_lr(1));
    ((void (*)(void *, const void *))g_orig_fillrectf)(ctx, a);
}
static void wrap_fillrectlist(void *ctx, const void *a)
{
    census(2, frame_lr(1));
    ((void (*)(void *, const void *))g_orig_fillrectlist)(ctx, a);
}
static void wrap_fillpath(void *ctx, const void *p, const void *xf)
{
    census(3, frame_lr(1));
    ((void (*)(void *, const void *, const void *))g_orig_fillpath)(ctx, p, xf);
}

static void wrap_setfill(void *ctx, const void *fill)
{
    /* A FillType can carry an image as well as a colour: tiled textures such as the
     * beat-loop pads are painted that way (cf. meow::TiledImageCache) and never reach
     * drawImage. Most fills have a null image, so the table lookup stays off the hot
     * path, and theme_sync_image returns immediately until a pixel theme has been on.
     *
     * _deferred, because setFill only arms the context and the pixels are read by the
     * following fillRect/fillPath, so the recolour must persist. The extended overview
     * (baked once per track, blitted as a FillType) is themed through this path. */
    if (fill != NULL && *(void *const *)((const uint8_t *)fill + FILLTYPE_IMAGE_OFF))
        theme_sync_image_deferred((const uint8_t *)fill + FILLTYPE_IMAGE_OFF);

    /* Also the message-thread heartbeat for the stem reports. stem/audio.c may only
     * print from the message thread (MDBG can block on a journald-drained stderr, which
     * stalls track loading on the audio or loader thread), and every repaint comes
     * through setFill.
     *
     * Throttled by a plain counter: setFill runs thousands of times per frame and
     * mod_stem_audio_report() starts with an isb+mrs. The report also throttles itself
     * to a 3 s window. */
    {
        static unsigned tick;
        if (((++tick) & 0x3ff) == 0) {
            MTRACE("theme: setfill %u  grad seen=%u ok=%u sub=%u\n",
                 tick, g_grad_seen, g_grad_ok, g_grad_sub);
            mod_stem_audio_report();
            mod_stem_decode_report();
            theme_memo_report();
            /* Eight word compares per table, no writes unless one has gone stock.
             * Applies theme switches and catches table rebuilds outside a waveform
             * reply. */
            theme_wave_apply();
        }
    }

    {
        const struct theme_palette *pal = mod_theme()->palette;

        /* Our own controls already use themed colours (mod_ui() roles resolved through
         * this palette), so mapping them again would apply the transform twice. The draw
         * kit brackets its calls and we chain straight through. The flag spans only one
         * synchronous call into juce (see mod_draw_enter), so it cannot leak into the
         * app's painting. */
        if (mod_drawing()) {
            ((setfill_t)g_orig_setfill)(ctx, fill);
            return;
        }

        if (pal == NULL || fill == NULL) {
            ((setfill_t)g_orig_setfill)(ctx, fill);
            return;
        }

        /* Byte-copy so we never write through the caller's const FillType&. */
        uint8_t copy[FILLTYPE_SIZE];
        memcpy(copy, fill, FILLTYPE_SIZE);
        *(uint32_t *)copy = theme_palette_argb(pal, *(const uint32_t *)fill, 1);

        /* ---- gradients ----
         *
         * A gradient fill ignores FillType::colour and takes its pixels from the stops in
         * the ColourGradient. The BLUE/RGB overview waveform is drawn as ~1200 per-column
         * gradient fills; 3BAND uses a prebaked image through drawImage.
         *
         * The byte-copy above copies only the pointer, so `copy` still refers to the
         * caller's ColourGradient.
         *
         * Do not write into that object: transforming the stops in place and restoring
         * them afterwards crashes the deck (SIGSEGV), since it relies on these offsets
         * and on setFill's timing for every gradient the app sets.
         *
         * Instead we build our own gradient and point the copy at it; the app's object is
         * only read. Anything that does not validate as a ColourGradient goes through
         * unthemed.
         *
         * A static is safe: setFill copies what it is given (as `copy` already relies
         * on), and painting is message-thread only. */
        {
            const uint8_t *gr = *(const uint8_t *const *)
                                    ((const uint8_t *)fill + FILLTYPE_GRAD_OFF);

            if (gr != NULL) g_grad_seen++;
            if (gr != NULL) {
                const uint8_t *el = *(const uint8_t *const *)(gr + GRAD_ELEMS_OFF);
                int nused  = *(const int *)(gr + GRAD_NUMUSED_OFF);
                int nalloc = *(const int *)(gr + GRAD_NUMALLOC_OFF);

                /* Validate as a ColourGradient. The counts alone would not catch a
                 * wrong-layout object; the strong test is that stop positions are
                 * doubles running 0..1 without going backwards. */
                int ok = el != NULL && nused >= 2 && nused <= GRAD_MAX_STOPS &&
                         nalloc >= nused;

                if (ok) {
                    double prev = -1.0;
                    for (int s = 0; s < nused; s++) {
                        double p = *(const double *)(el + s * GRAD_POINT_SIZE);
                        if (!(p >= 0.0 && p <= 1.0 && p >= prev)) { ok = 0; break; }
                        prev = p;
                    }
                }

                if (ok) g_grad_ok++;
                if (ok) {
                    /* sizeof(ColourGradient) -- header through numUsed, rounded up. */
                    static uint8_t mine[0x30];
                    static uint8_t stops[GRAD_MAX_STOPS * GRAD_POINT_SIZE];

                    memcpy(mine, gr, sizeof(mine));
                    memcpy(stops, el, (size_t)nused * GRAD_POINT_SIZE);
                    for (int s = 0; s < nused; s++) {
                        uint32_t *c = (uint32_t *)(stops + s * GRAD_POINT_SIZE +
                                                   GRAD_POINT_COLOUR);
                        *c = theme_palette_argb(pal, *c, 1);
                    }
                    /* numAllocated must describe our array, not the app's capacity,
                     * or a copy would read past the end of `stops`. */
                    *(uint8_t **)(mine + GRAD_ELEMS_OFF)  = stops;
                    *(int *)(mine + GRAD_NUMALLOC_OFF)    = nused;
                    *(int *)(mine + GRAD_NUMUSED_OFF)     = nused;
                    *(const uint8_t **)(copy + FILLTYPE_GRAD_OFF) = mine;
                    /* Log the caller of the first few substitutions. EP122 is non-PIE
                     * at 0x400000, so the return address maps directly to the binary. */
                    if (g_grad_sub < 3)
                        MDBG("theme: grad sub#%u from %p, stop0 %08x\n", g_grad_sub,
                             __builtin_return_address(0),
                             *(const uint32_t *)(stops + GRAD_POINT_COLOUR));
                    g_grad_sub++;
                }
            }
        }

        ((setfill_t)g_orig_setfill)(ctx, copy);
    }
}

/* ================================================================== */
/* Install                                                            */
/* ================================================================== */

/* The MOD SETTINGS row. Its values are filled from the registry at install, so a
 * theme added in presets.c appears in the menu automatically. `state` is an index into
 * k_mod_themes, resolved at draw time, so switching needs no notification. */
static const char *k_theme_values[MOD_THEME_MAX];

static const struct kit_row k_rows[] = {
    { .label = "THEME", .idx = KIT_IDX_THEME, .state = &g_theme_id,
      .values = k_theme_values, .nvalues = MOD_THEME_MAX },
};

static int theme_install(void)
{
    int images, i;

    if (mod_patch_vslot("setFill", EP122_GFX_RENDERER, THEME_SLOT_SETFILL,
                        (void *)wrap_setfill, &g_orig_setfill) != 0) {
        MDBG("theme: setFill hook unavailable -> themes disabled\n");
        return -1;
    }

    /* Diagnostic, not fatal: names the code that paints the overview strip. */
    if (MLOG_AT(MOD_LOG_DEBUG)) {
        mod_patch_vslot("fillRect", EP122_GFX_RENDERER, THEME_SLOT_FILLRECT,
                        (void *)wrap_fillrect, &g_orig_fillrect);
        mod_patch_vslot("fillRectF", EP122_GFX_RENDERER, THEME_SLOT_FILLRECTF,
                        (void *)wrap_fillrectf, &g_orig_fillrectf);
        mod_patch_vslot("fillRectList", EP122_GFX_RENDERER, THEME_SLOT_FILLRECTLIST,
                        (void *)wrap_fillrectlist, &g_orig_fillrectlist);
        mod_patch_vslot("fillPath", EP122_GFX_RENDERER, THEME_SLOT_FILLPATH,
                        (void *)wrap_fillpath, &g_orig_fillpath);
    }

    /* Not fatal: without image hooks the vector chrome still re-themes. */
    images = theme_image_install() == 0;

    for (i = 0; i < MOD_THEME_MAX; i++)
        k_theme_values[i] = mod_theme_name(i);
    kit_menu_add(k_rows, (int)(sizeof(k_rows) / sizeof(k_rows[0])));

    MDBG("theme: installed (theme=%s images=%s, live-switchable)\n",
         mod_theme()->name, images ? "on" : "off");
    return 0;
}

KIT_MOD(k_mod_theme,
        .name = "theme", .prio = 70, .install = theme_install,
        .what = "live re-theme via setFill");
