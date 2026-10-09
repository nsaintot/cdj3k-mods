// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * theme.h - the palette model, the theme registry and the internal seam.
 *
 * The model, ORIGINAL's role and how to add a theme: docs/mods.md.
 *
 *   theme.h    this: the palette model, the registry, the internal seam
 *   presets.c  the themes themselves -- the only file a new theme touches
 *   palette.c  the generic transform: one colour in, one colour out
 *   image.c    sprite/waveform pixels: classify, pin, recolour, restore
 *   theme.c    the two hooks and install
 *
 * Add a `struct theme_palette` and a `{ name, &palette }` row at the end of
 * k_mod_themes: the settings file stores the index, so the table's order is part
 * of the on-disk format.
 *
 * All themes are selectable from the menu. The pane borrows the deck's two-row
 * OFF/ON option set, so a longer list hooks getNumRows instead of writing the
 * field behind it: a written count outlives its row and the model then claims
 * more rows than the array holds. menu_rnumrows and menu_pane_labels both take
 * their length from menu_pane_rows(), so the two cannot disagree.
 */
#ifndef EP122_MOD_THEME_H
#define EP122_MOD_THEME_H

#include "core/mod_core.h"
#include "juce/draw.h"      /* struct theme_ui: the roles, shared with every mod's paint */

#ifdef __cplusplus
extern "C" {
#endif


/* The theme in force, as an index into the registry below. 0 is ORIGINAL. Read on
 * every setFill, so a change recolours the UI on the next repaint without a restart. */
extern int g_theme_id;

/* Non-zero while a waveform replyer is baking the overview, on that replyer's thread.
 *
 * The overview is baked once per track on the database reply thread, right after the
 * deck resets its colour table to stock, so theme_bake_recolour maps a stock image
 * through the palette exactly once. Theming a colour during the bake feeds that pass
 * already-themed input and the overview comes out stock. Thread-local because the
 * detailed strip paints on the message thread and must not see the flag. */
extern __thread int g_theme_in_bake;

/* ================================================================== */
/* The palette model                                                  */
/* ================================================================== */

/*
 * Every stage is optional and they apply in this order. A field left zero does
 * nothing, so a minimal theme is one line.
 *
 *   1. invert_l     HSL lightness inversion: L -> 1-L, hue and chroma exact.
 *                   Inverting L leaves the chroma span unchanged, so the HSL
 *                   round trip reduces to one add per channel.
 *
 *   2. sat_darken_q8  Darken saturated colours in proportion to their chroma.
 *                   A lightness inversion leaves them untouched, which costs
 *                   contrast: the orange +-10 badge and the red hot-cue marker
 *                   were tuned to sit on black. Hue is exact (all channels
 *                   scale together) and greys are untouched (chroma 0).
 *
 *   3. sat_q8       Chroma scale about the pixel's own lightness. 0 and 256
 *                   leave it alone, 1 is greyscale, above 256 is punchier.
 *
 *   4. shadow/highlight + tint_q8   Duotone. Blend the result toward a ramp
 *                   between two anchor colours, indexed by lightness: `shadow`
 *                   is what black becomes, `highlight` what white becomes. The
 *                   other stages keep the deck's own hues; this one gives the
 *                   theme its colour.
 *
 * exempt_blue spares blues from stage 2 on the fill path only. A fill is UI
 * surface where blue is the selection row, focus bar or a badge, and darkening it
 * costs the contrast of the label on top. Pixels arriving through drawImage are
 * picture content, mostly the waveform, where blue is the ink and exempting it
 * washes the waveform out.
 *
 * Q8 means 256 == 1.0.
 */
struct theme_palette {
    uint8_t  invert_l;        /* 1 = flip lightness, hue and chroma exact       */
    uint8_t  exempt_blue;     /* 1 = spare blues from sat_darken on fills       */
    /* The selection blue, stated instead of transformed. 0 = the generic mapping.
     *
     * #007de1 always carries lettering: it is the selected row in every list and the
     * lit quick-menu plate, so it must stay legible under the ink. A lightness
     * inversion cannot ensure that because saturated colours are near its fixed points
     * (see hue_pull_q8 below): the ink flips and the blue does not, leaving near-black
     * on mid-blue in a light theme.
     *
     * Contrast, white-or-ink on the mapped blue: ORIGINAL 4.18, WHITE 5.96,
     * MOCHA 4.21, AURORA 5.45; NEON 1.29, SANDSTONE 1.65, CYBERPUNK 2.41 (unreadable,
     * on the deck's rows as well as ours). Themes in the second group state the colour
     * here; the rest leave it 0.
     *
     * exempt_blue only spares blues from darkening, which helps a dark theme and
     * cannot lighten anything. */
    uint32_t selection;
    int16_t  sat_darken_q8;   /* 0 = off; 77 is WHITE's value (~0.30)           */
    int16_t  sat_q8;          /* 0 or 256 = unchanged; 1 = greyscale            */
    uint32_t shadow;          /* duotone: what black becomes    (0xRRGGBB)      */
    uint32_t highlight;       /* duotone: what white becomes    (0xRRGGBB)      */
    int16_t  tint_q8;         /* 0 = no duotone; 256 = fully the ramp           */

    /* ---- the palette proper ----
     *
     * A duotone is monochrome: every colour it produces lies on one line in RGB space,
     * so it cannot express several accent hues, and raising tint_q8 only makes the
     * result more single-hued.
     *
     * Chromatic pixels therefore take this stage instead. The deck distinguishes things
     * by hue (blue selection, orange master badge, red warning, green marker), so each
     * chromatic pixel is rotated to the nearest of these hues, keeping its own lightness
     * and chroma. Greys have no hue and keep the shadow/highlight ramp above; the two
     * stages together cover every pixel. */
    uint32_t hue[4];          /* accent hues to map the deck's own onto          */
    uint8_t  nhue;            /* 0 = leave chromatic pixels to the duotone       */

    /* How much of the target a mapped pixel takes. 0 = its hue only (what themes
     * that predate this field rely on); 256 = the pixel becomes the palette colour.
     *
     * At 0 the result keeps the source's lightness and chroma and only the hue angle
     * changes. On a dark theme that is fine: the screen is mostly grey and the duotone
     * re-skins it. On a light theme invert_l leaves saturated colours nearly in place
     * (near fixed points of a lightness inversion), chromatic pixels return before the
     * duotone sees them, and every light theme ends up looking like WHITE with the hues
     * nudged.
     *
     * Above 0 the target's midpoint and chroma are blended in too, so the palette sets
     * how light and how strong its colours are. Avoid 256: every pixel mapped to one hue
     * becomes the same colour and the waveform's shading (lightness variation within a
     * hue) flattens to a silhouette.
     *
     * WHITE leaves this at 0: it is the deck's own hues at the opposite polarity. */
    int16_t  hue_pull_q8;
};

/* `struct theme_ui`, the roles the mods' own controls paint with, is in
 * juce/draw.h with the rest of the drawing kit, so mods that paint need nothing
 * else from the theme layer. */

/* ================================================================== */
/* The registry                                                       */
/* ================================================================== */

struct mod_theme {
    /* Shown in the MOD SETTINGS value column and in log lines. Not persisted (the
     * settings file stores the index), so a theme can be renamed freely. */
    const char *name;

    /* NULL for ORIGINAL. Non-NULL also means the theme recolours image pixels, so
     * sprites match the recoloured fills around them. */
    const struct theme_palette *palette;

    /* NULL to derive every role from ORIGINAL's through `palette` (exact for
     * ORIGINAL, whose palette is the identity). Author it only where the generic
     * transform gets a role wrong. */
    const struct theme_ui *ui;

    /* Seven colours, expanded at first use. Takes precedence over `palette`
     * derivation; a fully authored `ui` takes precedence over this. */
    const struct theme_seed *seed;

    /* 1 if the theme puts the UI on a light ground.
     *
     * Declared by the theme, not a user toggle: a global dark/light switch would double
     * the list and make themes differ only by polarity. Two things need it:
     *
     *   - the button stipple's contrast. Its two halves sit 15 levels apart on stock's
     *     dark grey; deriving the dark half by the same ratio on a bright surface opens
     *     that to 60-odd and the checker no longer reads as one surface.
     *   - which way an accent moves to stay legible: lighter on a dark ground, darker
     *     on a light one.
     */
    uint8_t light;
};

/* The deck's selection blue and its checker partner. The palette anchors
 * this pair when a theme states its own selection. */
#define THEME_DECK_SELECT   0x007de1u
#define THEME_DECK_SELECT2  0x0064a5u
/* draw.h records the lit pair at x0.73; 187/256 is that. */
#define THEME_SELECT2_Q8    187u

/* ---- authoring a theme, the short way ----
 *
 * A preset states seven colours (panel, unlit control, lettering, alarm and three stem
 * hues) and roles.c expands them into the eighteen roles. The three stem hues must be
 * distinguishable in the wedge, the caption and the bypass icon at once, and no rule
 * derives three distinct hues from one accent, so they are stated. */
/* No `accent` field: our lit button sits in the same quick-menu bar as the deck's, so
 * its colour is derived (the deck's lit colour through this theme's palette), as ground,
 * plate and ink are. An authored accent would make the lit STEMS button differ from the
 * deck buttons beside it. */
struct theme_seed {
    uint32_t ground;    /* the panel everything sits on                   */
    /* Unused. The X-PAD and STEMS buttons sit in the deck's band between BEAT LOOP
     * and KEY SHIFT, so the plate is derived (the deck's grey through this theme's
     * palette) and the stipple is the deck's in every theme.
     *
     * Kept so presets that still set it compile. Delete the field and the seeds'
     * initialisers together. */
    uint32_t plate;
    uint32_t ink;       /* lettering                                      */
    uint32_t alarm;     /* the override / refusal family                  */
    uint32_t stem[3];   /* DRUMS / HARMONICS / VOCALS -- keep them apart  */
};

/* Expand a seed into a full role set. `pal` is the theme's own palette, needed for the
 * roles that are derived from the deck rather than authored; `light` its own .light. */
/* The deck's own role colours. What ORIGINAL expands to, unchanged. */
extern const struct theme_ui k_ui_original;

void theme_ui_expand(struct theme_ui *out, const struct theme_palette *pal,
                     const struct theme_seed *seed, int light);

/* How many themes exist. Can be raised freely; the menu pane's row count is
 * covered in the file header. */
#define MOD_THEME_MAX 7

extern const struct mod_theme k_mod_themes[MOD_THEME_MAX];

/* g_theme_id is declared above; the MOD SETTINGS row that edits it is generic
 * over `int *`.
 *
 * The selected theme, never NULL. The id comes from a file on the eMMC, so an
 * out-of-range id reads as ORIGINAL. */
const struct mod_theme *mod_theme(void);

/* A theme's name, for the menu's value list and log lines. Out of range reads as
 * ORIGINAL. */
const char *mod_theme_name(int id);

/* ================================================================== */
/* Internal seam (palette.c / image.c / theme.c)                      */
/* ================================================================== */

/* The generic transform. `is_fill` selects the exempt_blue carve-out above.
 * Both forms take the palette explicitly, so a caller resolves it once per fill
 * or per image, not per pixel.
 *
 * ---- the memo ----
 *
 * The table is public so the lookup can be inlined: it runs once per pixel of
 * every waveform frame, and a call per hit is a measurable share of the cost.
 * Inline, the lookup folds into the pixel loop; the miss path and all the
 * arithmetic stay in palette.c.
 *
 * An entry, in one naturally-aligned word so aarch64 cannot tear it:
 *
 *   bits  0..23   the answer, 0xRRGGBB
 *   bits 24..48   the question: (0xRRGGBB << 1) | is_fill
 *   bit  49       occupied
 *
 * Sizing: palette.c. */
#define THEME_MEMO_BITS     10
#define THEME_MEMO_N        (1u << THEME_MEMO_BITS)
#define THEME_MEMO_Q_SHIFT  24
#define THEME_MEMO_OCCUPIED (1ull << 49)
/* Knuth's multiplicative hash. The colours that matter differ in their low bits (a
 * waveform's ink is one hue at many levels); this cheaply mixes them upward. */
#define THEME_MEMO_SLOT(q)  ((uint32_t)((q) * 2654435761u) >> (32 - THEME_MEMO_BITS))

extern uint64_t g_theme_memo[THEME_MEMO_N];
extern const struct theme_palette *g_theme_memo_pal;
extern unsigned g_theme_memo_hit[2], g_theme_memo_miss[2];

void     theme_memo_report(void);
/* The transform on a separated triplet. Components are updated in place and are
 * each 0..255 on return. A NULL palette is ORIGINAL and leaves them unchanged. */
void theme_palette_rgb(const struct theme_palette *p,
                       uint32_t *pr, uint32_t *pg, uint32_t *pb, int is_fill);

uint32_t theme_palette_slow(const struct theme_palette *p, uint32_t rgb, int is_fill);

/* One colour in, one colour out, 0xRRGGBB. A NULL palette is ORIGINAL and returns
 * immediately, so a stock deck pays nothing. */
static inline uint32_t theme_palette_map(const struct theme_palette *p, uint32_t rgb,
                                         int is_fill)
{
    uint32_t q;
    uint64_t ent;

    if (p == NULL)
        return rgb;

    q   = (rgb << 1) | (is_fill ? 1u : 0u);
    ent = __atomic_load_n(&g_theme_memo[THEME_MEMO_SLOT(q)], __ATOMIC_RELAXED);

    /* One compare covers question and occupied bit: nothing is stored above bit 49.
     * The palette check makes an entry left by the previous theme a miss (see
     * g_theme_memo_pal). */
    if (p == g_theme_memo_pal &&
        (ent >> THEME_MEMO_Q_SHIFT) ==
            ((uint64_t)q | (THEME_MEMO_OCCUPIED >> THEME_MEMO_Q_SHIFT))) {
        g_theme_memo_hit[is_fill ? 1 : 0]++;
        return (uint32_t)(ent & 0xffffffu);
    }
    return theme_palette_slow(p, rgb, is_fill);
}

uint32_t theme_palette_argb(const struct theme_palette *p, uint32_t argb,
                            int is_fill);

/* Bring one image in line with the theme in force. Cheap and idempotent: the
 * common case is a table hit and a compare.
 *
 * theme_sync_image is for a blit inside the call being wrapped; _deferred is for
 * pixels read after it returns (a FillType image, or a bake the app caches and blits
 * later). A deferred image passed to the plain entry point goes unthemed; a per-frame
 * buffer passed to _deferred is recoloured twice. */
void theme_sync_image(const void *image);
void theme_sync_image_deferred(const void *image);

/* image.c's half of install. Returns 0 if the drawImage hook went in. */
int  theme_image_install(void);

/* ---- the waveform, themed at its source (wave.c) -------------------------
 *
 * The detailed waveform is computed from a table of eight colours, so it is themed by
 * mapping those eight instead of a quarter of a million pixels a frame. wave.c covers
 * the table, how it is found and why re-applying is stateless. */

/* [message] Re-theme every table that has gone stock. Safe to call any time; a table
 * already carrying our colours is left alone. */
void theme_wave_apply(void);

/* [message] A waveform replyer ran (track load, which rebuilds the tables). `style` is
 * the WAVEFORM COLOR mode that replied, so a mode change can trigger a rescan. */
void theme_wave_replied(int style);

/* Non-zero if the source route is live. Zero means no table was found and the per-pixel
 * path does the work, so a firmware that moves the colours costs more but stays themed. */
int  theme_wave_source_on(void);

/* The colour the source route themed the waveform ground to, 0x00RRGGBB, or zero when it
 * themes none (ORIGINAL, or a dark theme whose ground stays black). A strip baked while
 * this is non-zero already carries it. */
uint32_t theme_wave_ground(void);

/* The same entry without the palette, as carried by a strip baked before the tables were
 * themed. img_ground_themed compares a buffer against both. */
uint32_t theme_wave_ground_stock(void);

/* [message] Put the themed ground under a detailed waveform strip, in place. The eight
 * table colours are the ink; the background is never written by the renderer, so it has
 * no colour object and is filled here. Idempotent, so no stash or restore (see wave.c). */
void theme_wave_ground_fill(void *data, int32_t w, int32_t h,
                            int32_t line_stride, int32_t pixel_stride);

/* juce::LowLevelGraphicsSoftwareRenderer virtuals we repoint. */
#define THEME_SLOT_SETFILL  (18 * 8)
#define THEME_SLOT_DRAWIMG  (25 * 8)
/* fillRect(Rectangle<int>, bool) -- the slot juce::Graphics::fillRect(int,int,int,int)
 * calls, at vtable + 0xa8. Every rect the UI paints goes through it. */
#define THEME_SLOT_FILLRECT (21 * 8)
/* The rest of the shape primitives, in juce::LowLevelGraphicsContext declaration order:
 * the three slots between fillRect(int) at 0xa8 and drawImage at 0xc8. */
#define THEME_SLOT_FILLRECTF    (22 * 8)   /* fillRect(Rectangle<float>)   */
#define THEME_SLOT_FILLRECTLIST (23 * 8)   /* fillRectList(RectangleList&) */
#define THEME_SLOT_FILLPATH     (24 * 8)   /* fillPath(Path&, transform)   */


#ifdef __cplusplus
}
#endif

#endif /* EP122_MOD_THEME_H */
