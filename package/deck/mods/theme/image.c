// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * image.c - theme blitted images: sprites, strips and the baked overview.
 *
 * setFill sees every colour the UI computes but not those authored into PNGs.
 * Sprites are blitted, so this file hooks drawImage: classify each image once,
 * keep a pristine copy of the ones we may touch, and hand every pixel to
 * palette.c with the palette from mod_theme().
 */
#include "theme/image_internal.h"






static uintptr_t g_orig_drawimg;











static uint8_t *g_scratch;
static size_t   g_scratch_size;


static void wrap_drawimg(void *ctx, const void *image, const void *transform)
{
    const struct theme_palette *pal = mod_theme()->palette;
    void *pd = image ? *(void **)image : NULL;
    struct theme_bitmapdata bd;
    int32_t w = 0, h = 0;
    int has_alpha = 0;
    size_t bytes;

    /* Before classification, so rejected buffers are logged too: "never offered" and
     * "wrong geometry" look identical on screen. */
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

    /* theme_img_adopted comes before the shape tests: a buffer setFill already adopted
     * carries our output, and the stateless path would map it twice and overwrite the
     * pristine copy. An adopted image needs only the sync (a no-op when applied == want)
     * and the blit. */
    if (pd == NULL || pal == NULL ||
        theme_img_adopted(pd) ||
        !theme_is_waveform(pd, &w, &h, &has_alpha)) {
        int lent;

        theme_sync_image(image);                              /* sprite path */
        /* The app builds small images from other images; a copy of an already-mapped
         * sprite would be themed again when adopted. For blits into an offscreen surface,
         * theme_img_lend_stock puts the stock pixels back; blits to the screen are left
         * themed. */
        lent = pal != NULL && theme_img_lend_stock(image, ctx);
        ((drawimg_t)g_orig_drawimg)(ctx, image, transform);
        if (lent) theme_sync_image(image);                    /* and take it back */
        return;
    }

    /* The detailed waveform is already themed at its source (wave.c): its pixels come
     * from a colour table mapped once, so mapping them here would apply the transform
     * twice, at 243,200 pixels a frame (16.4 M lookups a second).
     *
     * It is opaque RGB, but not the only opaque strip (bytes per pixel = retained copy
     * size / w*h):
     *
     *   1200x128  stride 3600  3.00 B/px  RGB    the extended overview
     *   1024x66   stride 4096  4.00 B/px  ARGB
     *   1280x30   stride 5120  4.00 B/px  ARGB   the beat-grid rulers
     *
     * So the overview also reaches this branch; theme_wave_source_on only says the
     * detailed waveform's table was found. Do not treat "opaque" as meaning the detailed
     * waveform.
     *
     * Gated on the source route, so if a firmware moves the colours the strip falls back
     * to per-pixel theming. */
    if (!has_alpha && theme_wave_source_on()) {
        /* The ink is already themed by the table. */
        if (theme_bitmap(pd, w, h, &bd)) {
            /* Log the buffer address once (this path returns before the diagnostic below):
             * the edge softener writes its blended pixels here. */
            static const void *told;
            if (MLOG_AT(MOD_LOG_DEBUG) && told != bd.data) {
                told = bd.data;
                MDBG("theme: strip buffer %dx%d at %p stride=%d pix=%d\n",
                     (int)w, (int)h, (void *)bd.data,
                     (int)bd.line_stride, (int)bd.pixel_stride);
            }
            /* No ground fill: renderBackground already blends against the themed ground.
             * A fill keyed on exact black would also repaint peaks, whose ink themes to
             * 0x010101 and whose low-weight blends round to exact black. */
            (void)bd;
        }
        ((drawimg_t)g_orig_drawimg)(ctx, image, transform);
        return;
    }

    /* ARGB strips (beat rulers, browse previews) are adopted instead of transformed on
     * every blit: they rarely change, and the stateless path re-maps every blit, even on a
     * paused deck. The adopted path fingerprints what it left behind, so an
     * unchanged strip costs a 192-sample hash and a repainted one is re-snapshotted and
     * re-mapped.
     *
     * Only the first sighting comes here; once adopted, theme_img_adopted above routes it
     * to the sprite path. The opaque RGB detailed waveform is rewritten every frame and
     * is themed at its source (wave.c). */
    if (has_alpha) {
        theme_sync_image_deferred(image);
        ((drawimg_t)g_orig_drawimg)(ctx, image, transform);
        return;
    }

    /* Stateless: recolour, let the blit happen, put the original back.
     *
     * Do not persist the recolour here: these buffers are repainted every frame, so the
     * next snapshot would be taken from transformed pixels and the colours drift. The
     * stash/restore is correct because the blit consumes the pixels inside this call. */
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

    /* One line per shape, sampling pixels before and after the mapping, to tell "mapped
     * but unchanged on screen" from "never touched". Samples come off the middle row,
     * where a waveform strip has ink. */
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
            MDBG("theme: wave %dx%d fmt%d pix%d alpha=%d data=%p stride=%d  "
                 "%08x->%08x  %08x->%08x  %08x->%08x\n",
                 (int)w, (int)h, (int)bd.pixel_format, (int)bd.pixel_stride, has_alpha,
                 (void *)bd.data, (int)bd.line_stride,
                 a0, b0, a1, b1, a2, b2);
            theme_wave_census(&bd, w, h);
        }
    }

    const size_t rowbytes = (size_t)w * (size_t)bd.pixel_stride;

    /* Stash a row at a time inside the mapping loop, so each row is still in L1 when
     * it is mapped. A separate whole-strip copy would read and write 600 KB twice.
     *
     * Only w*pixel_stride per row: the mapping never writes the line-stride padding. */
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


/* ---- who draws the overview waveform ----
 *
 * The three OverviewWaveform*Widget classes draw nothing themselves (gui::WidgetBase's
 * draw slot is the no-op default for each); each embeds a juce::ImageComponent, whose
 * stock paint is g.drawImage(image, ...). So the overview is a bitmap in every waveform
 * mode, although only 3BAND reaches our drawImage hook; BLUE and RGB do not.
 *
 * juce::Image is a single ref-counted pointer, so "has an image" is one load. From
 * ImageComponent::paint:
 *
 *     setOpacity(1.0f);
 *     drawImage(this+0xd8, targetArea, placement=this+0xe0, fillAlphaWithBrush=0);
 *
 * juce::Component is the primary base, so `self` is the ImageComponent and the image is
 * at +0xd8. fillAlphaWithBrush=0 is the branch that reaches context->drawImage, so a
 * valid image here must arrive at our hook; a null pointer means JUCE skips the draw. */
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
        /* Report on first sight and on every change of image: a new bitmap per track is
         * normal, a pointer that stays null means the component never draws. */
        if (i == n || seen[i].pd != pd) {
            if (i == n && n < (int)(sizeof(seen) / sizeof(seen[0]))) { seen[n].self = self; n++; }
            if (i < (int)(sizeof(seen) / sizeof(seen[0]))) seen[i].pd = pd;
            /* Also log the component's bounds: paint scales the image to fit them, so a
             * 100px image can be a 1000px strip on screen. Bounds are at +0x28, the field
             * paint reads for its target rect. */
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
 * The overview is baked, not painted. Once per track, on the database reply thread,
 * db_access_proxy::WaveformReplyer_<mode> renders the point data into an offscreen
 * juce::Image and caches it on itself; every repaint just blits that cache.
 * WaveformReplyer_400Pt:
 *
 *     if (this->image == 0) { bake(..., 1200, 128, &img); this->image = img; }
 *
 * Paint-time hooks cannot see the bake, and a hook installed after the track loaded
 * misses it.
 *
 * So it is recoloured here, once per track. theme_sync_image does the work: it keeps the
 * pristine pixels, so a theme switch re-maps from stock, and the fingerprint detects the
 * next track's re-bake. THEME_BLIT_LATER because the image is blitted long after this
 * call returns; it also admits the image past the live-buffer shape test. */
#define WAVEREPLY_SLOT_ONREPLY  0x10
/* juce::Image member WaveformReplyer_400Pt caches the bake in. */
#define WAVEREPLY_IMAGE_OFF     0x20

/* ---- reaching the 1200Pt bake ----
 *
 * 400Pt caches its bake on itself; 1200Pt hands the image straight to a listener and
 * keeps nothing:
 *
 *     img = bake(...);                       // a local
 *     listener = request[5];
 *     (*(*listener + 0x10))(listener, &no, tid_lo, tid_hi, kind, img, 0);
 *
 * There is no member to read, but the listener call is virtual: the request is our
 * second argument, the listener is a field on it, and its vtable slot is patched at run
 * time without needing a class name.
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

/* The listener call. `image` is a juce::Image passed by value; it is a single ref-counted
 * pointer, so &image is what theme_sync_image wants. */
static void wrap_wavelistener(void *self, void *a1, void *a2, void *a3, void *a4,
                              void *image, void *a6)
{
    theme_bake_recolour_img(&image, "1200Pt");
    ((void (*)(void *, void *, void *, void *, void *, void *, void *))
        g_orig_wavelistener)(self, a1, a2, a3, a4, image, a6);
}

/* Patch the listener's slot the first time we see one, from the replyer hook, the only
 * place the listener is identified. */
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

    /* The class is only known at run time, so the expected value passed to
     * mod_patch_slot is the slot's current value: this keeps the unreadable-address check
     * without an identity check. */
    if (mod_safe_read(vt + WAVELISTENER_SLOT, &fn, sizeof(fn)) != 0 || fn == 0) return;
    MDBG("theme: waveform listener vt=%#lx slot holds %#lx\n",
         (unsigned long)vt, (unsigned long)fn);

    mod_patch_slot("waveform image listener", vt + WAVELISTENER_SLOT, fn,
                   (void *)wrap_wavelistener, &g_orig_wavelistener);
}

static void theme_bake_recolour(void *self, const char *which)
{
    const void *img = (const uint8_t *)self + WAVEREPLY_IMAGE_OFF;

    /* Logged even without an image, to tell "never ran" from "nothing at +0x20". The
     * offset is from WaveformReplyer_400Pt; 1200Pt hands its bake to the listener, so a
     * null here is expected for that mode. */
    if (*(void *const *)img == NULL) {
        static int told;
        if (!told) { told = 1; MDBG("theme: %s replyer ran, no image at +%#x\n",
                                    which, WAVEREPLY_IMAGE_OFF); }
        return;
    }
    theme_sync_image_x(img, THEME_BLIT_LATER);
    MDBG("theme: baked %s overview -> themed\n", which);
}

/* The listener is hooked before chaining, since the replyer calls it during the wrapped
 * call. theme_wave_replied runs after the chain, since the replyer resets the deck's colour
 * tables to stock (see wave.c). */
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

/* Hook drawImage only if the SoftwarePixelData vtable is readable: every pixel touch
 * in this file is gated on that vptr. */
int theme_image_install(void)
{
    uintptr_t vt = 0;

    /* One per WAVEFORM COLOR mode; only the mode in force fires, and the user can
     * change it. */
    mod_patch_vslot("WaveformReplyer_400Pt", EP122_WAVEREPLY_400PT,
                    WAVEREPLY_SLOT_ONREPLY, (void *)wrap_wr400, &g_orig_wr400);
    mod_patch_vslot("WaveformReplyer_1200Pt", EP122_WAVEREPLY_1200PT,
                    WAVEREPLY_SLOT_ONREPLY, (void *)wrap_wr1200, &g_orig_wr1200);
    mod_patch_vslot("WaveformReplyer_3Band", EP122_WAVEREPLY_3BAND,
                    WAVEREPLY_SLOT_ONREPLY, (void *)wrap_wr3band, &g_orig_wr3band);

    /* Diagnostic only, not fatal. */
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
