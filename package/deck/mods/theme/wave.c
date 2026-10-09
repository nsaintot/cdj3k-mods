// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * wave.c - theme the detailed waveform at its colour table.
 *
 * The rest of this directory recolours pixels. The detailed waveform is computed by the
 * deck: a column carries a colour index naming a set of bands, and the index becomes RGB
 * through a table of eight entries.
 *
 * The strip is 1280x190 (243,200 pixels blitted per frame) and holds 21 distinct
 * colours: 16.4 M palette lookups a second, against ~1,700 setFills. Theming the table
 * instead costs eight writes per track load.
 *
 * ---- the table ----
 *
 * Eight juce::Colour at 8-byte stride (four bytes of colour, four of padding), sitting
 * inside a larger colour array:
 *
 *   +0x00  ffffffff   peaks            +0x20  ffd2dcfa
 *   +0x08  ffffa600   orange ink       +0x28  ffb4690a
 *   +0x10  ff0055e1   blue ink         +0x30  fff5ebd7
 *   +0x18  fffff0d7                    +0x38  ff000000   ground
 *
 * The other 13 colours in the strip are anti-aliased blends between these eight and
 * follow them.
 *
 * ---- finding it ----
 *
 * The values are constructed at run time, not stored: the binary contains no 0055e1 or
 * ffa600. They live in the anonymous rw mapping after .data and are found by scanning
 * for the stock group below. A firmware that changes it makes the scan find zero sites
 * and log so.
 *
 * About 185 copies exist. Only one draws the on-screen strip, but patching all of them
 * changes nothing else on the play screen, so all are treated the same.
 *
 * ---- keeping it ----
 *
 * The app rebuilds the table on every track load, so re-applying must be idempotent at
 * any moment.
 *
 * It is stateless: a site is written when it holds the stock group or the group we last
 * wrote; anything else is left alone. The second condition matters on a theme switch:
 * after WHITE -> MOCHA every site holds WHITE's colours, which a stock-only test would
 * skip until the next track load.
 *
 * ---- the ground ----
 *
 * The eight colours are the ink. The strip's background is not among them: mapping entry 7
 * (0xff000000) changes nothing on screen. Entry 7 does appear as a column colour at
 * de-zoom.
 *
 * The ground is 72% of the strip's pixels. It comes from renderBackground, which writes
 * one colour per column into the array renderWaveform_new lerps against, and is themed
 * there (wrap_render_bg). Nothing walks the finished strip: such a pass has to key on
 * exact black and eats peaks, since the peak ink themes to 0x010101 and its blends round
 * to exact black.
 *
 * ---- the contour ----
 *
 * The dark edge along the waveform on a light theme comes from renderWaveform_new, which
 * lerps between the source columns under one output pixel; draw_new only picks one of
 * seven solid colours per column per row. Both lerp endpoints let black in, and neither is
 * the palette:
 *
 *   renderBackground   one colour per column, every entry 0x00000000. This is the
 *                      colour the outer edge is averaged against. See
 *                      wrap_render_bg.
 *   the provider       a band a column does not have comes back as a default-constructed
 *                      juce::Colour. Its out pointer is in x8, which C cannot name, so it
 *                      is wrapped by an assembly stub. See WAVE_PROVIDER_OFF.
 *
 * Both are fixed at the source. A per-pixel pass cannot do it: blends between two inks
 * (already correct) are indistinguishable from blends toward black in the buffer, and
 * any rule that lightens the edge also lightens interior colour boundaries, which flickers.
 *
 * A theme whose ground stays black skips all of this.
 *
 * Everything here is [message]: the juce UI thread.
 */
#include "theme/theme.h"

#define WAVE_N        8               /* colours in the group                    */
#define WAVE_STRIDE   2               /* uint32 words per entry: colour + pad    */
#define WAVE_WORDS    (WAVE_N * WAVE_STRIDE)
#define WAVE_BYTES    (WAVE_WORDS * 4)

/* The stock group. Also the scan's search key (see the header). */
static const uint32_t k_wave_stock[WAVE_N] = {
    0xffffffffu, 0xffffa600u, 0xff0055e1u, 0xfffff0d7u,
    0xffd2dcfau, 0xffb4690au, 0xfff5ebd7u, 0xff000000u,
};

/* Well above the ~185 present with every pane opened. Overflow would silently leave part of
 * the UI on the slow path. */
#define WAVE_MAX_SITES 512

static uint32_t *g_site[WAVE_MAX_SITES];
static int       g_site_n;
static int       g_scanned;

/* The group through the current palette. Recomputed when the theme changes, and the
 * only thing ever written to a site (see the header). */
static uint32_t  g_themed[WAVE_N];
static int       g_themed_id = -1;

/* The themed ground. Also arms the provider wrappers and the ground fill: zero (a dark
 * theme, or ORIGINAL) makes both do nothing. */
static uint32_t  g_ground;
static void      wave_provider_install(void);

/* The live palette, for the two modes whose inks cannot be themed at rest. Set together
 * with g_ground. */
static const struct theme_palette *g_pal;

/* What we last wrote, so a site still carrying the previous theme's colours is
 * recognised as ours and updated. See the header. */
static uint32_t  g_written[WAVE_N];
static int       g_written_ok;

/* The style that last replied. A WAVEFORM COLOR change swaps in a different widget that
 * may bring a table created after the scan, so a style change triggers a rescan (the scan
 * walks 58 MB, too much for a timer). */
static int g_style = -1;

/* ---- the scan ------------------------------------------------------------ */

/* Anonymous and writable, excluding the heap. The group lives in the mapping after
 * .data (about 58 MB); the heap is hundreds of megabytes and cannot hold it. */
static int wave_region_wanted(const char *line, uintptr_t *lo, uintptr_t *hi)
{
    unsigned long a, b;
    char perms[8], rest[256];
    int n;

    rest[0] = '\0';
    n = sscanf(line, "%lx-%lx %7s %*s %*s %*s %255[^\n]", &a, &b, perms, rest);
    if (n < 3) return 0;
    if (perms[0] != 'r' || perms[1] != 'w') return 0;
    if (rest[0] != '\0') return 0;                  /* named: file, heap, stack, vdso */
    if (b <= a || b - a > (256u << 20)) return 0;
    *lo = a; *hi = b;
    return 1;
}

static void wave_note(uint32_t *p)
{
    int i;

    for (i = 0; i < g_site_n; i++)
        if (g_site[i] == p) return;
    if (g_site_n < WAVE_MAX_SITES) g_site[g_site_n++] = p;
}

/* Walk every candidate mapping for the stock group. The inner loop compares only the
 * first word (0xffffffff), which is rare enough that the full compare seldom runs. */
static void wave_scan(void)
{
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    int before = g_site_n;

    g_scanned = 1;
    if (f == NULL) { MDBG("theme: wave scan: no maps\n"); return; }

    while (fgets(line, sizeof(line), f) != NULL) {
        uintptr_t lo, hi, a;

        if (!wave_region_wanted(line, &lo, &hi)) continue;
        for (a = lo; a + WAVE_BYTES <= hi; a += 4) {
            const uint32_t *p = (const uint32_t *)a;
            int k;

            if (p[0] != k_wave_stock[0]) continue;
            for (k = 0; k < WAVE_N; k++)
                if (p[k * WAVE_STRIDE] != k_wave_stock[k] ||
                    p[k * WAVE_STRIDE + 1] != 0u) break;
            if (k == WAVE_N) wave_note((uint32_t *)a);
        }
    }
    fclose(f);

    /* Log zero sites: a firmware that changes any of the eight colours makes the scan
     * find nothing and the waveform stays stock. */
    if (g_site_n == 0)
        MDBG("theme: wave scan found NO colour table -- waveform stays on the pixel "
             "path (did the deck's own colours change?)\n");
    else
        MDBG("theme: wave scan found %d colour tables (%d new)\n",
             g_site_n, g_site_n - before);
}

/* ---- applying ------------------------------------------------------------ */

static int wave_is(const uint32_t *p, const uint32_t *want)
{
    int k;

    for (k = 0; k < WAVE_N; k++)
        if (p[k * WAVE_STRIDE] != want[k]) return 0;
    return 1;
}

int theme_wave_source_on(void)
{
    return g_site_n > 0 && g_themed_id > 0;
}

uint32_t theme_wave_ground(void)
{
    return g_ground;
}

/* The ground entry before the palette, as carried by a strip baked before the tables
 * were themed. img_ground_themed tells the two apart. */
uint32_t theme_wave_ground_stock(void)
{
    return k_wave_stock[WAVE_N - 1] & 0xffffffu;
}

/* [message] Re-theme every table that has gone stock. The common pass compares eight
 * words per site and writes nothing. */
void theme_wave_apply(void)
{
    const struct theme_palette *pal = mod_theme()->palette;
    int id = __atomic_load_n(&g_theme_id, __ATOMIC_RELAXED);
    int i, k, wrote = 0;

    if (pal == NULL) {
        /* ORIGINAL: restore what we themed. A table the app has since rebuilt is
         * already stock and does not match g_written. */
        if (g_written_ok) {
            int back = 0;

            for (i = 0; i < g_site_n; i++)
                if (wave_is(g_site[i], g_written)) {
                    for (k = 0; k < WAVE_N; k++)
                        g_site[i][k * WAVE_STRIDE] = k_wave_stock[k];
                    back++;
                }
            MDBG("theme: wave source -> ORIGINAL, %d/%d tables restored\n",
                 back, g_site_n);
            g_written_ok = 0;
        }
        g_themed_id = -1;
        /* Disarms every wrapper; they stay patched and read these. */
        g_ground = 0;
        g_pal = NULL;
        return;
    }

    if (!g_scanned) wave_scan();
    g_pal = pal;
    wave_provider_install();

    if (id != g_themed_id) {
        /* is_fill 0, as on the per-pixel route, so both routes give the same colours. */
        for (k = 0; k < WAVE_N; k++) {
            uint32_t c = theme_palette_map(pal, k_wave_stock[k] & 0xffffffu, 0);

            /* Pure black is reserved: theme_wave_ground_fill treats it as background.
             * Under WHITE the peaks (stock 0xffffff) invert to black and would be filled
             * with the ground colour, so they are nudged one level to 0x010101 in the
             * table itself. The ground entry is exempt: black there means a dark theme,
             * and then neither the fill nor the provider wrappers run. */
            if (c == 0 && k != WAVE_N - 1) c = 0x010101u;
            g_themed[k] = 0xff000000u | c;
        }
        g_ground = g_themed[WAVE_N - 1] & 0xffffffu;
        g_themed_id = id;
        MDBG("theme: wave source palette: ink %06x->%06x  ground %06x->%06x\n",
             k_wave_stock[2] & 0xffffffu, g_themed[2] & 0xffffffu,
             k_wave_stock[7] & 0xffffffu, g_themed[7] & 0xffffffu);
    }

    for (i = 0; i < g_site_n; i++) {
        /* Already themed: the usual case, checked first so the pass does not rewrite
         * every group on every tick. */
        if (wave_is(g_site[i], g_themed)) continue;
        /* Stock (the app rebuilt it) or our previous write (the theme changed): ours to
         * write. Anything else is left alone. */
        if (!wave_is(g_site[i], k_wave_stock) &&
            !(g_written_ok && wave_is(g_site[i], g_written))) continue;
        for (k = 0; k < WAVE_N; k++)
            g_site[i][k * WAVE_STRIDE] = g_themed[k];
        wrote++;
    }
    if (wrote) {
        for (k = 0; k < WAVE_N; k++) g_written[k] = g_themed[k];
        g_written_ok = 1;
        MDBG("theme: wave source re-applied to %d/%d tables\n", wrote, g_site_n);
    }
}

/* [message] A waveform replyer ran (track load, which rebuilds the tables). `style` is one
 * of the three WAVEFORM COLOR modes; only a style change rescans, since it can bring a
 * widget that did not exist at scan time. */
void theme_wave_replied(int style)
{
    if (style != g_style) {
        g_style = style;
        g_scanned = 0;
    }
    theme_wave_apply();
}

/* ---- the ground, and the black the blender averages against --------------- */

/* ---- the provider slot, reached through an assembly stub ------------------ */

/* [message] The colour providers the lerp calls, wrapped in assembly.
 *
 * The three providers, one per WAVEFORM COLOR mode, answer one column and band with
 * {height, B, G, R, A}. A band a column does not have comes back as a default-constructed
 * juce::Colour, the second black the lerp can average against.
 *
 * The out pointer arrives in x8: the return type has a non-trivial destructor, so AAPCS
 * returns it indirectly whatever its size, and C cannot name x8.
 *
 * Do not wrap in C with `out` as a fourth parameter: that reads x3, which holds garbage,
 * silently corrupts memory, and segfaults whenever x3 happens to be unmapped (e.g. on a
 * track load or the SHORTCUT menu).
 *
 * The stub only has to leave x8 untouched until the call, which C cannot promise. The
 * original returns the pointer in x0, so the fix-up is an ordinary C function taking it
 * as an argument. x16 is the procedure-call scratch register, as a linker veneer uses.
 *
 * `map` asks for the ink itself to go through the palette, which only two modes need:
 *
 *   3Band  a table of 8 colours in .bss, themed at rest: the ink arrives already
 *          themed and mapping it again would transform it twice
 *   Blue   a table too, just below 3Band's, but the scan only knows 3Band's
 *          stock group so nothing themes it
 *   RGB    no table: the colour is built from packed 3-3-3 bits of the track's
 *          analysis data, so this is the only place it can be themed
 */
#define WAVE_PROVIDER_OFF 0x30

/* Not static: the stubs below reach these by name through the assembler. */
uintptr_t wave_orig_3band, wave_orig_rgb, wave_orig_blue;

__thread int g_theme_in_bake;

void *wave_sample_fix(unsigned char *out, int map);

void *wave_sample_fix(unsigned char *out, int map)
{
    /* Not during a bake (see g_theme_in_bake): theme_bake_recolour expects the stock
     * image and maps it once. */
    if (out == NULL || g_theme_in_bake) return out;

    if (out[4] == 0u) {                               /* absent band -> themed ground */
        /* This gate applies to the ground only. Themes that map black to black (MOCHA)
         * need no substitution. The ink branch below must not share it, or RGB and Blue
         * stay stock on every dark theme. */
        if (g_ground == 0u) return out;
        out[1] = (unsigned char)g_ground;             /* B */
        out[2] = (unsigned char)(g_ground >> 8);      /* G */
        out[3] = (unsigned char)(g_ground >> 16);     /* R */
        out[4] = 0xffu;
    } else if (map && g_pal != NULL) {
        uint32_t rgb = ((uint32_t)out[3] << 16) | ((uint32_t)out[2] << 8) | out[1];

        /* is_fill 0: ink in a picture, the same question the per-pixel route asked. */
        rgb = theme_palette_map(g_pal, rgb, 0);
        out[1] = (unsigned char)rgb;
        out[2] = (unsigned char)(rgb >> 8);
        out[3] = (unsigned char)(rgb >> 16);
    }
    return out;
}

/* x0/w1/w2 and x8 pass through untouched to the original; x16 is the only register
 * touched before the call. */
#define WAVE_STUB(name, orig, mapval)                                   \
    __asm__(".text\n"                                                   \
            ".align  2\n"                                               \
            ".hidden " #name "\n"                                       \
            ".globl  " #name "\n"                                       \
            ".type   " #name ", %function\n"                            \
            #name ":\n"                                                 \
            "   stp  x29, x30, [sp, #-16]!\n"                           \
            "   mov  x29, sp\n"                                         \
            "   adrp x16, " #orig "\n"                                  \
            "   ldr  x16, [x16, #:lo12:" #orig "]\n"                    \
            "   blr  x16\n"                                             \
            "   mov  w1, #" #mapval "\n"                                \
            "   bl   wave_sample_fix\n"                                 \
            "   ldp  x29, x30, [sp], #16\n"                             \
            "   ret\n"                                                  \
            ".size   " #name ", .-" #name "\n")

WAVE_STUB(wave_stub_3band, wave_orig_3band, 0);
WAVE_STUB(wave_stub_rgb,   wave_orig_rgb,   1);
WAVE_STUB(wave_stub_blue,  wave_orig_blue,  1);

void wave_stub_3band(void);
void wave_stub_rgb(void);
void wave_stub_blue(void);

/* [message] The colour the waveform's outer edge is averaged against.
 *
 * renderBackground fills one juce::Colour per column into the array renderWaveform_new
 * lerps the ink against, so it sets the edge between waveform and background. Every entry
 * is 0x00000000. On a light theme that black shows as a
 * dark trail along a blue edge as the waveform scrolls.
 *
 * Only black entries are replaced; a non-black value from the widget is left as is. */
typedef void *(*render_bg_t)(void *, unsigned char **, int, void *, float);
static uintptr_t g_tramp_bg;

static void *wrap_render_bg(void *colours, unsigned char **span, int x0,
                            void *info, float scale)
{
    void *r = ((render_bg_t)g_tramp_bg)(colours, span, x0, info, scale);

    if (g_ground != 0u && span != NULL && span[0] != NULL && span[1] != NULL) {
        unsigned char *p = span[0], *end = span[1];

        for (; p + 4 <= end; p += 4)
            if ((p[0] | p[1] | p[2]) == 0u) {
                p[0] = (unsigned char)g_ground;             /* B */
                p[1] = (unsigned char)(g_ground >> 8);      /* G */
                p[2] = (unsigned char)(g_ground >> 16);     /* R */
                p[3] = 0xffu;
            }
    }
    return r;
}

/* Patched once and left in place. The wrappers are inert while g_ground is 0, so a theme
 * change needs no repatching and ORIGINAL costs one predictable branch per band. */
static void wave_provider_install(void)
{
    static int done;

    if (done) return;
    done = 1;
    mod_patch_fn("waveRenderBg", ep122_sym(EP122_WAVE_RENDER_BG),
                 (void *)wrap_render_bg, &g_tramp_bg);
    mod_patch_vslot("waveSample3Band", EP122_WAVE_PROVIDER_3BAND, WAVE_PROVIDER_OFF,
                    (void *)wave_stub_3band, &wave_orig_3band);
    /* Not fatal: a miss only loses the fix for that style. */
    mod_patch_vslot("waveSampleRGB", EP122_WAVE_PROVIDER_RGB, WAVE_PROVIDER_OFF,
                    (void *)wave_stub_rgb, &wave_orig_rgb);
    mod_patch_vslot("waveSampleBlue", EP122_WAVE_PROVIDER_BLUE, WAVE_PROVIDER_OFF,
                    (void *)wave_stub_blue, &wave_orig_blue);
}

/* [message] Put the themed ground under a detailed waveform strip, in place.
 *
 * The strip's background is not a palette entry (see the header), so it is filled here.
 * Only exact black is touched: partially covered pixels are already the deck's blend
 * against the themed ground (see WAVE_PROVIDER_OFF).
 *
 * Idempotent, so it writes into the app's buffer with no stash or restore: the ground is
 * not black, so a second pass finds nothing to fill. */
void theme_wave_ground_fill(void *data, int32_t w, int32_t h,
                            int32_t line_stride, int32_t pixel_stride)
{
    int32_t x, y;

    if (data == NULL || pixel_stride < 3 || g_ground == 0) return;

    for (y = 0; y < h; y++) {
        uint8_t *p = (uint8_t *)data + (size_t)y * line_stride;

        for (x = 0; x < w; x++, p += pixel_stride) {
            if ((p[0] | p[1] | p[2]) != 0) continue;
            p[0] = (uint8_t)g_ground;
            p[1] = (uint8_t)(g_ground >> 8);
            p[2] = (uint8_t)(g_ground >> 16);
        }
    }
}
