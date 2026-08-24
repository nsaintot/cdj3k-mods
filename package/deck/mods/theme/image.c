// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * image.c - the pixels a theme cannot reach any other way.
 *
 * setFill sees every colour the UI computes, but not the ones that were decided
 * when a PNG was authored. Sprites are blitted, so this file is where they get
 * the same treatment: classify each image once, keep a pristine copy of the ones
 * we may touch, and hand every pixel to palette.c.
 *
 * Nothing here knows what any theme looks like -- it asks mod_theme() for a
 * palette and passes it along.
 */
#include "theme/image_internal.h"




/* juce::Image::BitmapData -- data, pixelFormat, lineStride, pixelStride, w, h.
 * Over-sized and zeroed so the tail (the releaser slot) is definitely NULL. */


static uintptr_t g_orig_drawimg;









/* Fetch the pixels. Only ever called for a verified SoftwarePixelData.
 * initialiseBitmapData fills in data/format/strides but NOT width/height -- the
 * real BitmapData constructor sets those itself -- so we supply them. */
/* Is this pixel's alpha actually COVERAGE?
 *
 * JUCE stores ARGB PREMULTIPLIED, and that constrains the data: every colour channel is
 * already scaled by alpha, so each one must be <= alpha. A pixel that breaks the
 * inequality cannot be premultiplied -- its alpha is a byte nobody filled in, and the
 * colour beside it is the real, opaque colour.
 *
 * This is an invariant, not a heuristic: no valid premultiplied pixel can fail it, so it
 * never misfires on the buffers that do carry proper alpha. It matters twice over on the
 * deck's extended overview -- a 1024x66 ARGB strip the app blits with alpha ignored --
 * and each half accounts for one of the two faults that were reported as separate bugs:
 *
 *   - most of its pixels carry alpha 0 over real colour. Reading that as "fully
 *     transparent, nothing to do" left the entire strip stock. That is the overview
 *     "refusing to be themed", and it hid itself for a long time: the diagnostic that
 *     was meant to answer "did this buffer even reach the hook" carried the same alpha-0
 *     skip, so it answered a confident no. A marker must never share a predicate with the
 *     code it is checking.
 *   - the rest carry small NON-zero alphas. Those took the unpremultiply path, which
 *     divides by an alpha that never scaled anything, blows the colour up to white and
 *     re-multiplies it back down to near-black: a dark cross-hatch through the ink,
 *     reported as "corruption when switching track".
 *
 * A third consequence is not about the mapping at all but about the BLIT, and it is why
 * theme_map_pixel writes the alpha byte rather than preserving it -- see the note there.
 */

/* Unpremultiply, map, re-premultiply. JUCE stores ARGB premultiplied, in memory
 * order B,G,R,A; RGB is B,G,R. Transforming premultiplied channels directly would
 * darken every soft edge, so anti-aliased glyph borders go the long way round.
 *
 * The palette is passed in rather than looked up: this runs once per PIXEL, and
 * resolving the selected theme per pixel would be a load and two branches for an
 * answer that cannot change inside one image. */


static uint8_t *g_scratch;
static size_t   g_scratch_size;

/* Wide, big and short: the waveform itself and the beat-grid rulers that frame
 * it. The rulers are ARGB rather than RGB, so format is not the test -- shape
 * is. Artwork fails on height or on aspect (it has to fail only one); sprites
 * are too small to reach here at all. */
/* ---- geometry probe ----
 *
 * Every buffer wide enough to reach the waveform classifier, reported once per SHAPE
 * rather than per call: the strip is repainted every frame and a per-call line would
 * bury everything else, while a per-pd one would miss a second buffer of the same kind.
 * Width is left out of the key on purpose -- a partly-occluded repaint hands us a
 * narrower slice of the same buffer, and that is the same shape for this purpose.
 *
 * line_stride vs w*pixel_stride is the whole reason this exists: the loops below walk
 * rows by line_stride and pixels by pixel_stride, and take the buffer's LENGTH to be
 * line_stride*h. If a buffer's real row pitch is not what it reports, the restoring
 * memcpy writes the wrong length into memory we do not own. */
/* WHAT COLOURS DOES THE RENDERER ACTUALLY EMIT?
 *
 * The strip is a COMPUTED frame, not authored art: docs/waveform-3band-re.md has the
 * column format, and a column carries a colour INDEX -- a nibble naming a set of bands,
 * eight values -- rather than a colour. So the renderer holds an index-to-RGB mapping
 * with single digits of entries, and theming THAT would cost nothing per frame instead
 * of transforming 150k pixels.
 *
 * This prints the set, off the pixels as they arrive and before anything of ours has
 * touched them, which is the list to go looking for in the binary. Once per shape, and
 * only under debug: it is a full pass over the strip.
 *
 * Counting rather than sampling on purpose. The question is how MANY distinct colours
 * there are, and a sample can only ever put a floor under that. */

static void wrap_drawimg(void *ctx, const void *image, const void *transform)
{
    const struct theme_palette *pal = mod_theme()->palette;
    void *pd = image ? *(void **)image : NULL;
    struct theme_bitmapdata bd;
    int32_t w = 0, h = 0;
    int has_alpha = 0;
    size_t bytes;

    /* Before any classification, so a buffer that is REJECTED still shows up -- "it was
     * never offered to us" and "we took it and got the geometry wrong" look identical
     * on screen and need telling apart. */
    if (MLOG_AT(MOD_LOG_DEBUG) && pd != NULL) {
        int32_t pw = *(int32_t *)((uintptr_t)pd + IPD_WIDTH_OFF);
        int32_t ph = *(int32_t *)((uintptr_t)pd + IPD_HEIGHT_OFF);
        int32_t pf = *(int32_t *)((uintptr_t)pd + IPD_PIXELFORMAT_OFF);

        if (pw > THEME_IMG_MAXDIM) {
            struct theme_bitmapdata pb;
            int got = (*(uintptr_t *)pd == THEME_VT_SOFTPIXELDATA) &&
                      theme_bitmap(pd, pw, ph, &pb);
            theme_img_probe(pd, got ? &pb : NULL, pw, ph, pf,
                            *(uintptr_t *)pd != THEME_VT_SOFTPIXELDATA ? "NOT SoftwarePixelData"
                            : ph > THEME_WAVE_MAX_H                    ? "too tall -> artwork"
                            : pw < THEME_WAVE_ASPECT * ph              ? "squarish -> artwork"
                            : (pf != PIXFMT_RGB && pf != PIXFMT_ARGB)  ? "odd format"
                            : "waveform");
        }
    }

    /* theme_img_adopted comes first among the shape tests, because a buffer that setFill
     * already adopted is carrying our output right now. Stashing that, mapping it a
     * second time and restoring afterwards would both double the transform and overwrite
     * the pristine copy with it. An adopted image only ever needs the sync -- which will
     * see applied == want and do nothing -- and then the blit. */
    if (pd == NULL || pal == NULL ||
        theme_img_adopted(pd) ||
        !theme_is_waveform(pd, &w, &h, &has_alpha)) {
        int lent;

        theme_sync_image(image);                              /* sprite path */
        /* A BLIT INTO AN OFFSCREEN SURFACE IS NOT A BLIT ONTO THE SCREEN: the app builds
         * small images out of other images, and a copy taken from a sprite we have
         * already mapped is born themed and then themed again when we adopt it in its own
         * right. theme_img_lend_stock puts the stock pixels back for exactly that blit --
         * and says nothing at all for the display, which is where the transform belongs. */
        lent = pal != NULL && theme_img_lend_stock(image, ctx);
        ((drawimg_t)g_orig_drawimg)(ctx, image, transform);
        if (lent) theme_sync_image(image);                    /* and take it back */
        return;
    }

    /* THE DETAILED WAVEFORM IS ALREADY THEMED, at its source -- see wave.c. Its pixels
     * were computed from a colour table we mapped once, so running them through the
     * palette again here would apply the transform twice, and it is also the single most
     * expensive thing this file does: 243,200 pixels a frame, measured at 16.4 M palette
     * lookups a second and 31.8% of the deck's whole CPU.
     *
     * OPAQUE RGB is what tells it apart, and it is not a guess -- but it is NOT the only
     * opaque strip, which this used to claim. Read out of a running instance's adoption
     * table, by dividing each retained copy by its own w*h:
     *
     *   1200x128  stride 3600  3.00 B/px  RGB    the extended overview
     *   1024x66   stride 4096  4.00 B/px  ARGB
     *   1280x30   stride 5120  4.00 B/px  ARGB   the beat-grid rulers
     *
     * So the overview lands on this branch alongside the detailed waveform, and what
     * saves it is the second half of the test rather than the first: theme_wave_source_on
     * is the source route having found ITS table, which is the detailed waveform's.
     * Worth knowing before anything is hung on "opaque means the detailed waveform".
     *
     * Gated on that source route, so a firmware that moved the colours falls back to the
     * old cost rather than to an unthemed strip. */
    if (!has_alpha && theme_wave_source_on()) {
        /* The ink came out of the table already themed; the ground is the part the
         * renderer never wrote, so it is put in here. One idempotent pass, straight into
         * the app's buffer -- no stash and no restore, because a substitution cannot
         * compound the way the palette could. */
        if (theme_bitmap(pd, w, h, &bd)) {
            /* Once, and here rather than in the diagnostic further down, which this path
             * returns before reaching. It is the address to put a hardware WRITE
             * watchpoint on under the emulator's gdbserver: the edge softener writes its
             * blended pixel into this buffer, so a watchpoint inside it catches that code
             * in the act. Nothing in-process can -- the colours it reads share a page
             * with far hotter neighbours, so page-granular tricks catch those instead. */
            static const void *told;
            if (MLOG_AT(MOD_LOG_DEBUG) && told != bd.data) {
                told = bd.data;
                MDBG("theme: strip buffer %dx%d at %p stride=%d pix=%d\n",
                     (int)w, (int)h, (void *)bd.data,
                     (int)bd.line_stride, (int)bd.pixel_stride);
            }
            /* NO GROUND FILL. renderBackground now hands the blend the themed ground, so
             * the deck paints its own background in the right colour and there is nothing
             * left here to correct. Filling anyway ATE PEAKS: the peak ink themes to
             * 0x010101 on a light ground, a blend of it at low weight rounds to exact
             * black, and a fill keyed on exact black then repainted those pixels as
             * background -- peaks winking in and out as the waveform scrolled. Reported
             * off the deck as "black peaks appearing disappearing". */
            (void)bd;
        }
        ((drawimg_t)g_orig_drawimg)(ctx, image, transform);
        return;
    }

    /* ARGB strips -- the beat rulers, the browse previews -- are ADOPTED rather than
     * transformed on every blit. The stateless treatment below is right for a buffer the
     * app genuinely rewrites each frame, and these are not that: measured at 2.2 MILLION
     * palette lookups a second on a PAUSED deck, reproducing pixels that had not changed
     * (verified by diffing two screenshots a second apart -- not one row moved).
     *
     * The adopted path already answers this properly. It fingerprints what it left behind,
     * so a strip still carrying our output is recognised and skipped for the cost of a
     * 192-sample hash, while one the app has repainted is re-snapshotted and re-mapped
     * exactly as before. A static strip therefore costs almost nothing and a live one
     * costs what it always did.
     *
     * Only the FIRST sighting needs routing here: once adopted, theme_img_adopted above
     * catches it and the sync happens on the sprite path.
     *
     * The opaque RGB detailed waveform is deliberately NOT sent this way -- it really is
     * rewritten every frame, and it is handled at its source instead (wave.c). */
    if (has_alpha) {
        theme_sync_image_deferred(image);
        ((drawimg_t)g_orig_drawimg)(ctx, image, transform);
        return;
    }

    /* Stateless: recolour, let the blit happen, put the original straight back.
     *
     * Persisting the recolour here instead was tried and is WRONG for this path. These
     * buffers are repainted by the app every frame, so leaving our output in them means
     * the next frame's snapshot is taken from pixels we already transformed -- the
     * detailed waveform came up themed and then drifted off colour within a few frames.
     * The stash/restore bracket is what makes a per-frame buffer safe, and it is correct
     * precisely because the blit consumes the pixels inside this call. */
    if (!theme_bitmap(pd, w, h, &bd)) {
        ((drawimg_t)g_orig_drawimg)(ctx, image, transform);
        return;
    }
    bytes = (size_t)bd.line_stride * (size_t)h;

    if (g_scratch_size < bytes) {                             /* grown once, then reused */
        uint8_t *p = realloc(g_scratch, bytes);
        if (p == NULL) {
            ((drawimg_t)g_orig_drawimg)(ctx, image, transform);
            return;
        }
        g_scratch = p;
        g_scratch_size = bytes;
    }

    /* One line per shape, sampled either side of the mapping. Which buffers this branch
     * claims was already known; what it DOES to them was not, and "recoloured it and the
     * screen did not change" and "never touched those pixels" look identical from a
     * screenshot. Samples come off the middle row, where a waveform strip has ink. */
    if (MLOG_AT(MOD_LOG_DEBUG)) {
        static struct { int32_t w, h, fmt; } seen[16];
        static int n;
        int i, dup = 0;

        for (i = 0; i < n; i++)
            if (seen[i].w == w && seen[i].h == h && seen[i].fmt == bd.pixel_format) dup = 1;
        if (!dup && n < (int)(sizeof(seen) / sizeof(seen[0]))) {
            uint8_t *mid = bd.data + (size_t)(h / 2) * bd.line_stride;
            uint32_t a0 = 0, a1 = 0, a2 = 0, b0, b1, b2;
            int x0 = w / 4, x1 = w / 2, x2 = (3 * w) / 4;

            memcpy(&a0, mid + (size_t)x0 * bd.pixel_stride, 4);
            memcpy(&a1, mid + (size_t)x1 * bd.pixel_stride, 4);
            memcpy(&a2, mid + (size_t)x2 * bd.pixel_stride, 4);
            b0 = a0; b1 = a1; b2 = a2;
            theme_map_pixel(pal, (uint8_t *)&b0, has_alpha);
            theme_map_pixel(pal, (uint8_t *)&b1, has_alpha);
            theme_map_pixel(pal, (uint8_t *)&b2, has_alpha);
            seen[n].w = w; seen[n].h = h; seen[n].fmt = bd.pixel_format; n++;
            /* The BUFFER ADDRESS, because it is the thing to put a hardware write
             * watchpoint on under the emulator's gdbserver: the edge softener writes its
             * blended pixel here, so a watchpoint inside the strip catches that code in
             * the act. See the same note on the source path above. */
            MDBG("theme: wave %dx%d fmt%d pix%d alpha=%d data=%p stride=%d  "
                 "%08x->%08x  %08x->%08x  %08x->%08x\n",
                 (int)w, (int)h, (int)bd.pixel_format, (int)bd.pixel_stride, has_alpha,
                 (void *)bd.data, (int)bd.line_stride,
                 a0, b0, a1, b1, a2, b2);
            theme_wave_census(&bd, w, h);
        }
    }

    const size_t rowbytes = (size_t)w * (size_t)bd.pixel_stride;

    /* STASH A ROW AT A TIME, INSIDE THE MAPPING LOOP. Copying the whole strip first
     * and then walking it again reads 600 KB twice and writes it twice, and on this
     * deck the copy alone showed up as libc's largest entry. Fused, each row is
     * still in L1 when the mapping that follows reads it. Same bytes saved and the
     * same bytes put back -- only the order changed.
     *
     * Only w*pixel_stride per row: the mapping never writes past it, so whatever the
     * line stride pads with was never ours to restore. */
    for (int y = 0; y < h; y++) {
        uint8_t *row = bd.data + (size_t)y * bd.line_stride;

        memcpy(g_scratch + (size_t)y * bd.line_stride, row, rowbytes);
        for (int x = 0; x < w; x++)
            theme_map_pixel(pal, row + (size_t)x * bd.pixel_stride, has_alpha);
    }
    ((drawimg_t)g_orig_drawimg)(ctx, image, transform);

    for (int y = 0; y < h; y++)                               /* hand it back untouched */
        memcpy(bd.data + (size_t)y * bd.line_stride,
               g_scratch + (size_t)y * bd.line_stride, rowbytes);
}


/* ---- who owns the overview waveform ----
 *
 * All three OverviewWaveform*Widget classes draw nothing themselves -- gui::WidgetBase'
 * draw slot is the no-op default for every one of them -- and each embeds a
 * juce::ImageComponent, whose stock paint is g.drawImage(image, ...). So the overview is
 * a BITMAP in every waveform mode, which is the opposite of what its behaviour suggests:
 * 3BAND arrives at our drawImage hook and themes, BLUE and RGB never appear at all.
 *
 * juce::Image is a single ref-counted pointer, so "has an image" is one load. Taken from
 * ImageComponent::paint itself rather than guessed off the constructor:
 *
 *     setOpacity(1.0f);
 *     drawImage(this+0xd8, targetArea, placement=this+0xe0, fillAlphaWithBrush=0);
 *
 * juce::Component is the primary base, so `self` is the ImageComponent and the image is
 * at +0xd8. The last argument being 0 matters too: it is the branch that reaches
 * context->drawImage, so a valid image here MUST arrive at our hook. A null pointer
 * means JUCE skips the draw outright, which would explain the silence without any of
 * the exotic render paths being involved. */
#define IMAGECOMP_IMAGE_OFF  0xd8
#define IMAGECOMP_PAINT_SLOT 0xd0        /* juce::Component::paint */

static uintptr_t g_orig_imgcomp_paint;

static void wrap_imgcomp_paint(void *self, void *g)
{
    if (MLOG_AT(MOD_LOG_DEBUG)) {
        static struct { const void *self, *pd; } seen[16];
        static int n;
        const void *pd = *(const void *const *)((const uint8_t *)self + IMAGECOMP_IMAGE_OFF);
        int i;

        for (i = 0; i < n; i++)
            if (seen[i].self == self) break;
        /* Report on first sight and on every CHANGE of the image, because the change is
         * the event: an ImageComponent handed a new bitmap per track is the normal case,
         * one that stays null is a component that never draws anything. */
        if (i == n || seen[i].pd != pd) {
            if (i == n && n < (int)(sizeof(seen) / sizeof(seen[0]))) { seen[n].self = self; n++; }
            if (i < (int)(sizeof(seen) / sizeof(seen[0]))) seen[i].pd = pd;
            /* The component's own bounds as well as the image's, because paint SCALES
             * the image to fit them -- a 100px image can be a 1000px strip on screen, so
             * the image size alone cannot be matched against what is visible. Bounds live
             * at +0x28; paint reads the same field to build its target rect. */
            const int32_t *bnds = (const int32_t *)((const uint8_t *)self + 0x28);

            if (pd == NULL) {
                MTRACE("theme: imagecomp %p bounds %dx%d -> NO IMAGE\n",
                     self, (int)bnds[0], (int)bnds[1]);
            } else {
                MTRACE("theme: imagecomp %p bounds %dx%d -> pd=%p %dx%d fmt%d\n", self,
                     (int)bnds[0], (int)bnds[1], pd,
                     (int)*(const int32_t *)((uintptr_t)pd + IPD_WIDTH_OFF),
                     (int)*(const int32_t *)((uintptr_t)pd + IPD_HEIGHT_OFF),
                     (int)*(const int32_t *)((uintptr_t)pd + IPD_PIXELFORMAT_OFF));
            }
        }
    }
    ((void (*)(void *, void *))g_orig_imgcomp_paint)(self, g);
}

/* ---- the baked overview waveform ----
 *
 * The overview is not painted, it is BAKED. On the database reply thread, once per track,
 * db_access_proxy::WaveformReplyer_<mode> renders the point data into an offscreen
 * juce::Image and caches it on itself; every repaint after that is just a blit of that
 * cache. WaveformReplyer_400Pt is explicit about it:
 *
 *     if (this->image == 0) { bake(..., 1200, 128, &img); this->image = img; }
 *
 * That lifecycle is why months of paint-time instrumentation found nothing. Markers,
 * fill censuses and caller counts all observe REPAINTS, and the overview never repaints
 * -- the one moment its pixels are decided happens once, on another thread, outside any
 * paint. A probe can only see it if the track is loaded after the probe is armed, which
 * is exactly the test that was never run.
 *
 * So recolour it here, where the pixels are made. Once per track, no per-frame cost, and
 * the cached image the widget blits is themed from its first blit onward.
 *
 * theme_sync_image does the actual work rather than a private copy of it: it already
 * keeps the pristine pixels, so a later theme switch re-maps from stock instead of
 * compounding, and the fingerprint notices a re-bake for the next track and re-snapshots.
 * It is entered as THEME_BLIT_LATER because that is simply true here -- the image is
 * cached and blitted long after this call returns -- and that is also what admits it past
 * the live-buffer test, which by shape it would otherwise fail. */
#define WAVEREPLY_SLOT_ONREPLY  0x10
/* juce::Image member the replyer caches the bake in, read off WaveformReplyer_400Pt. */
#define WAVEREPLY_IMAGE_OFF     0x20

/* ---- reaching the 1200Pt bake ----
 *
 * 400Pt caches its bake on itself; 1200Pt does not -- it hands the image straight to a
 * listener and keeps nothing:
 *
 *     img = bake(...);                       // a local
 *     listener = request[5];
 *     (*(*listener + 0x10))(listener, &no, tid_lo, tid_hi, kind, img, 0);
 *
 * so there is no member to read and no free-function hook in this shim to catch the
 * return. The listener call is a VIRTUAL though, which is reachable: the request arrives
 * as our own second argument, the listener is a field on it, and its vtable can be read
 * and patched at run time. No class name is needed and nothing is guessed -- the object
 * itself says where to hook.
 *
 * The image is the sixth parameter, i.e. x5. */
#define WAVEREQ_LISTENER_IDX    5      /* request[5] -- the listener            */
#define WAVELISTENER_SLOT       0x10   /* its onWaveformImage-ish virtual       */

static uintptr_t g_orig_wr400, g_orig_wr1200, g_orig_wr3band;
static uintptr_t g_orig_wavelistener;

static void theme_bake_recolour_img(const void *image, const char *which)
{
    if (image == NULL || *(void *const *)image == NULL) return;
    theme_sync_image_x(image, THEME_BLIT_LATER);
    {
        static int told;
        if (!told) { told = 1; MDBG("theme: baked %s overview -> themed\n", which); }
    }
}

/* The listener call. `image` is a juce::Image passed BY VALUE in the ABI sense -- it is a
 * single ref-counted pointer, so the parameter is the Ptr itself and &image is what
 * theme_sync_image wants. */
static void wrap_wavelistener(void *self, void *a1, void *a2, void *a3, void *a4,
                              void *image, void *a6)
{
    theme_bake_recolour_img(&image, "1200Pt");
    ((void (*)(void *, void *, void *, void *, void *, void *, void *))
        g_orig_wavelistener)(self, a1, a2, a3, a4, image, a6);
}

/* Patch the listener's slot the first time we see one. Done from inside the replyer hook
 * because that is the only place the listener is identified for us. */
static void theme_hook_wavelistener(void *reqptr)
{
    void *req, *listener;
    uintptr_t vt, fn;

    if (g_orig_wavelistener != 0 || reqptr == NULL) return;
    req = *(void **)reqptr;
    if (req == NULL) return;
    listener = ((void **)req)[WAVEREQ_LISTENER_IDX];
    if (listener == NULL) return;
    if (mod_safe_read((uintptr_t)listener, &vt, sizeof(vt)) != 0 || vt == 0) return;

    /* The slot's CURRENT value is the expectation. There is no address to state here --
     * the class is whatever the request happened to carry, discovered at run time -- so
     * read the slot and hand mod_patch_slot what it already holds. That keeps the
     * unreadable-address check while dropping an identity check we have no basis for. */
    if (mod_safe_read(vt + WAVELISTENER_SLOT, &fn, sizeof(fn)) != 0 || fn == 0) return;
    MDBG("theme: waveform listener vt=%#lx slot holds %#lx\n",
         (unsigned long)vt, (unsigned long)fn);

    mod_patch_slot("waveform image listener", vt + WAVELISTENER_SLOT, fn,
                   (void *)wrap_wavelistener, &g_orig_wavelistener);
}

static void theme_bake_recolour(void *self, const char *which)
{
    const void *img = (const uint8_t *)self + WAVEREPLY_IMAGE_OFF;

    /* Reported whether or not there is an image: "the replyer never ran" and "it ran and
     * cached nothing at +0x20" are different problems, and only one of them is about this
     * offset. The offset was read off WaveformReplyer_400Pt; the 1200Pt variant hands its
     * bake straight to the listener as a return value instead, so a null here is expected
     * for that mode rather than a failure. */
    if (*(void *const *)img == NULL) {
        static int told;
        if (!told) { told = 1; MDBG("theme: %s replyer ran, no image at +%#x\n",
                                    which, WAVEREPLY_IMAGE_OFF); }
        return;
    }
    theme_sync_image_x(img, THEME_BLIT_LATER);
    MDBG("theme: baked %s overview -> themed\n", which);
}

/* The listener is hooked BEFORE chaining, because the replyer calls it during the call we
 * are wrapping -- patching afterwards would miss the very bake that just happened. */
/* theme_wave_replied AFTER the chain, not before: the replyer is what rebuilds the deck's
 * colour tables back to stock, so re-asserting first would be overwritten by the very call
 * we are wrapping. See wave.c. */
#define WAVEREPLY_WRAP(name, saved, tag, style)                                       \
    static int64_t name(void *self, void *a2, void *a3, void *a4)                     \
    {                                                                                 \
        int64_t r;                                                                    \
        g_theme_in_bake++;                                                            \
        theme_hook_wavelistener(a2);                                                  \
        r = ((int64_t (*)(void *, void *, void *, void *))saved)(self, a2, a3, a4);   \
        theme_bake_recolour(self, tag);                                               \
        g_theme_in_bake--;                                                            \
        theme_wave_replied(style);                                                    \
        return r;                                                                     \
    }
WAVEREPLY_WRAP(wrap_wr400,   g_orig_wr400,   "400Pt/RGB",  0)
WAVEREPLY_WRAP(wrap_wr1200,  g_orig_wr1200,  "1200Pt/BLUE", 1)
WAVEREPLY_WRAP(wrap_wr3band, g_orig_wr3band, "3Band",       2)

/* Hook drawImage, but only if SoftwarePixelData is also reachable: that vptr is
 * what every pixel touch in this file is gated on, so without it there would be
 * a hook that can never do anything. */
int theme_image_install(void)
{
    uintptr_t vt = 0;

    /* One per WAVEFORM COLOR mode. Only the mode in force ever fires, but which one that
     * is changes under the DJ's hands, so all three are hooked. */
    mod_patch_vslot("WaveformReplyer_400Pt", EP122_WAVEREPLY_400PT,
                    WAVEREPLY_SLOT_ONREPLY, (void *)wrap_wr400, &g_orig_wr400);
    mod_patch_vslot("WaveformReplyer_1200Pt", EP122_WAVEREPLY_1200PT,
                    WAVEREPLY_SLOT_ONREPLY, (void *)wrap_wr1200, &g_orig_wr1200);
    mod_patch_vslot("WaveformReplyer_3Band", EP122_WAVEREPLY_3BAND,
                    WAVEREPLY_SLOT_ONREPLY, (void *)wrap_wr3band, &g_orig_wr3band);

    /* Diagnostic only, and deliberately not fatal: it answers where the overview goes,
     * but nothing else depends on it. */
    if (MLOG_AT(MOD_LOG_DEBUG))
        mod_patch_vslot("ImageComponent::paint", EP122_JUCE_IMAGECOMPONENT,
                        IMAGECOMP_PAINT_SLOT, (void *)wrap_imgcomp_paint,
                        &g_orig_imgcomp_paint);

    if (mod_safe_read(THEME_VT_SOFTPIXELDATA, &vt, sizeof(vt)) != 0 || vt == 0) {
        MWARN("theme: SoftwarePixelData vtable unreadable -> images left alone\n");
        return -1;
    }
    return mod_patch_vslot("drawImage", EP122_GFX_RENDERER, THEME_SLOT_DRAWIMG,
                           (void *)wrap_drawimg, &g_orig_drawimg);
}
