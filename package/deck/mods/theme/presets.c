// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * presets.c - the themes.
 *
 * The only file adding a theme has to touch. See struct theme_palette in
 * theme.h for the fields and their order. This file is data only, so a new theme
 * can look wrong but cannot break the machinery.
 *
 * WHITE's darkening factor, 77/65280 (Q8), is within one level per channel of
 * 30/25500 for every (chroma, value) pair. The other themes are authored against
 * the model, which
 * predicts polarity and saturation but not taste: adjust `tint_q8` first and
 * `sat_q8` second.
 */
#include "theme/theme.h"

/* Lightness inversion with the saturated-colour darkening a light background
 * needs, and the blue carve-out that keeps selection rows readable. No duotone:
 * the deck's own hues, opposite polarity. */
static const struct theme_palette k_pal_white = {
    .invert_l      = 1,
    .exempt_blue   = 1,
    .sat_darken_q8 = 77,      /* ~0.30 */
    .sat_q8        = 256,
};



/* ---- the authored themes ----
 *
 * These are authored (colours chosen), not derived: two transforms of the same source at
 * the same polarity converge however they are tuned.
 *
 * Each also has a `theme_palette`: the seed only colours our controls, and the deck's
 * screens need the palette to match.
 *
 * The duotone's highlight must have chroma. It is the accent lifted ~44% toward white, so
 * white lettering at the top of the ramp is not at full chroma and mid-brightness. The
 * seed's ink would give a near-black to near-white ramp, i.e. only a faint cast.
 *
 * The ramp only gets greys (chrome, panels, lettering). Chromatic pixels go through .hue,
 * so the deck's blue/orange/red/green rotate onto the nearest of the theme's hues and
 * stay distinct.
 *
 * sat_q8 stays at unity: scaling the deck's chroma on top of the hue map blows out the
 * contrast. */

/* Sky is the accent and yellow the alarm. The palette has no red, so it follows the
 * deck: blue means working, amber means override, and the brightest colour goes on what
 * must be noticed.
 *
 * The palette's mid-tone #9a9f17 is not a hue target: at 44/256 it is five steps from the
 * yellow's 39 and would waste a slot. */
static const struct theme_seed k_seed_cyberpunk = {
    .ground = 0x00060e, .ink    = 0xdff3f7,
    .alarm  = 0xfee801,                           /* yellow: the override family */
    /* DRUMS must not be the alarm's yellow (the bypass icon would match the drums
     * wedge), and VOCALS not a second cyan: stems need dE 40 apart (sky vs teal was
     * 31). */
    .stem   = { 0xff2e88, 0x54c1e6, 0x2bf58a },   /* magenta / sky / spring */
};
static const struct theme_palette k_pal_cyberpunk = {
    .selection     = 0x007ba5,   /* ink contrast 4.2 as on the deck; the mapped #00a7e0 gave 2.41 */
    .sat_q8 = 256,          /* explicit; 0 also means unchanged (palette.c) */
    .shadow = 0x00060e, .highlight = 0x9fdcf1, .tint_q8 = 96,
    .hue = { 0xfee801, 0x54c1e6, 0x39c4b6 }, .nhue = 3,
};

static const struct theme_seed k_seed_neon = {
    .ground = 0x0b0d17, .ink    = 0xc9d1d9,
    /* Orange, not the DRUMS pink, so the bypass icon differs from the drums wedge.
     * Orange is also the deck's override colour. */
    .alarm  = 0xff9100,
    .stem   = { 0xff2daa, 0x00e5ff, 0x7c4dff },   /* pink / cyan / violet */
};
static const struct theme_palette k_pal_neon = {
    .selection     = 0x006874,   /* ink contrast 4.2 as on the deck; the mapped #00cae0 gave 1.29 */
    .sat_q8 = 256,          /* explicit; 0 also means unchanged (palette.c) */
    .shadow = 0x0b0d17, .highlight = 0x70f0ff, .tint_q8 = 96,
    .hue = { 0xff2daa, 0x00e5ff, 0x7c4dff }, .nhue = 3,
};

/* Catppuccin Mocha, from the published palette:
 *
 *   Base #1e1e2e   Text #cdd6f4   Mauve #cba6f7   Peach #fab387
 *   Red  #f38ba8   Blue #89b4fa   Green #a6e3a1
 *
 * Mauve, Mocha's signature, was chosen as the accent to keep the lit control clear of
 * Blue, a stem. Peach is the alarm, matching the deck's orange override colour (MASTER
 * PLAYER, the +-10 badge). Red/Blue/Green as stems keep ORIGINAL's DRUMS/HARMONICS/VOCALS
 * ordering and are all more than 100 degrees apart, the widest separation here.
 *
 * Peach is also the fourth hue: with three, the deck's oranges fall nearer Red and the
 * override colour loses its own hue.
 *
 * tint_q8 is 160, not 96 as in CYBERPUNK and NEON: at 96 the background lands two thirds
 * of the way back to black. The ramp only affects greys, so this is safe. */
static const struct theme_seed k_seed_mocha = {
    /* Mocha's Base and Text. The plate is derived through the palette below and lands
     * between Surface0 and Surface1, which confirms the anchors. */
    .ground = 0x1e1e2e, .ink    = 0xcdd6f4,
    .alarm  = 0xfab387,                           /* Peach                  */
    .stem   = { 0xf38ba8, 0x89b4fa, 0xa6e3a1 },   /* Red / Blue / Green     */
};
static const struct theme_palette k_pal_mocha = {
    .sat_q8 = 256,          /* explicit; 0 also means unchanged (palette.c) */
    /* Solved so the deck's black and white come out at Mocha's Base and Text. They are
     * ramp ends, not palette entries; only the middle of the ramp is used. */
    .shadow = 0x30304a, .highlight = 0xafbded, .tint_q8 = 160,
    .hue = { 0xf38ba8, 0x89b4fa, 0xa6e3a1, 0xfab387 }, .nhue = 4,
};

/* ---- AURORA ----
 *
 * Green, blue and violet over near-black, with a pale mint at the top. Seven colours:
 *
 *   #141414 ground   #292929 plate   #F0FEF9 mint
 *   #00D26C green    #00E575 green   #006AFB blue    #B869FF violet
 *
 * The two greys are panel colours, not ramp anchors. A duotone is linear in lightness,
 * so anchoring black at #141414 with white on the mint fixes the slope at 0.86 and lifts
 * every deck grey by twenty levels, compressing the range. So the ramp runs the full
 * range:
 *
 *   shadow     #000000 with tint_q8 256, so the deck's black chrome stays black.
 *   highlight  #F0FEF9 itself. At full tint a grey becomes a per-channel gain of
 *              240/254/249: contrast unchanged, tinted mint.
 *
 * The greys are used as panels:
 *
 *   ground  #141414, our panel on the deck's black screen.
 *   plate   #292929 in the palette; the derived plate is #2f3130, six levels above.
 *   ink     #F0FEF9, also the top of the ramp, so our lettering and the deck's match.
 *
 * Assignments:
 *
 *   stems   pink #FF4FC3 / blue #006AFB / green #00E575 (DRUMS / HARMONICS / VOCALS).
 *           VOCALS takes #00E575, not #00D26C: same hue within a fifth of a degree,
 *           brighter. DRUMS is pink because the palette's violet is only dE 33 from the
 *           blue.
 *   alarm   violet #D451FF, distinct from every stem. The lit colour, derived from the
 *           deck's blue, lands on the blue stem.
 *   hue[]   green / blue / violet.
 *
 * With no warm entry, the deck's amber maps to green, so the waveform is blue over green.
 *
 * The deck's red family cannot be placed well. Nearest-hue puts it on the violet (the
 * green/violet boundary sits at 22/256): ambers and yellows go green; reds and deeper
 * oranges (cue line, hot-cue markers, the +-10 badge) go violet. Dropping violet from
 * hue[] is worse: red is then 103 from the blue against 107 from the green, so the
 * markers land on the blue and vanish into the waveform, and #d93025 ties and is
 * resolved by list order. */
static const struct theme_seed k_seed_aurora = {
    .ground = 0x141414, .ink    = 0xf0fef9,
    /* Violet: a green alarm would match VOCALS and make the X-PAD's value identical to
     * its armed flags (xpad_on is the VOCALS stem). */
    .alarm  = 0xd451ff,                           /* violet: the override family  */
    /* Pink for DRUMS: violet is only dE 33 from this blue. */
    .stem   = { 0xff4fc3, 0x006afb, 0x00e575 },   /* pink / blue / green          */
};
static const struct theme_palette k_pal_aurora = {
    .sat_q8 = 256,          /* explicit; 0 also means unchanged (palette.c) */
    .shadow = 0x000000, .highlight = 0xf0fef9, .tint_q8 = 256,
    .hue = { 0x00d26c, 0x006afb, 0xb869ff }, .nhue = 3,
};

/* ---- SANDSTONE ----
 *
 * An authored palette, five colours:
 *
 *   #E8705D coral   #FE985F orange   #FEE5A0 butter   #6C7C6B sage   #393F61 navy
 *
 * It has no background colour, so ground and ink are deduced from the palette instead
 * of derived from the deck's black and white through the duotone (which would leave only
 * tint strength and pigments as variables, and every such theme looking alike). Only the
 * plate is derived, to match the deck's buttons.
 *
 *   ground  #fbf0d9  the butter at ~92% lightness with less chroma (the butter itself is
 *           too strong for a full screen).
 *   ink     #262a44  the navy deepened, so lettering differs from the HARMONICS stem
 *           (the navy as given) while keeping the hue.
 *
 * Assignments:
 *
 *   stems   coral / navy / sage, at 8, 231 and 117 degrees: the only three dark enough
 *           for ink on cream and far enough apart. They keep ORIGINAL's DRUMS-red,
 *           HARMONICS-blue, VOCALS-green ordering. (The seed lifts the sage; see below.)
 *   alarm   amber #fdb03f (see the seed).
 *   hue[]   coral / orange / sage / navy. The orange is 14 degrees off the coral.
 *
 * The butter is not a hue target, although it is 36 degrees clear of the coral: the
 * amber slot covers half the screen (the waveform is blue against orange), so a pale
 * yellow there makes the deck cream-and-gold. The butter appears anyway as the paper.
 *
 * 14 degrees is enough because nearest-hue is a comparison: the deck's red at 0 is 8 from
 * the coral and 22 from the orange; its amber at 35 is 13 from the orange and 27 from the
 * coral.
 */
static const struct theme_seed k_seed_sandstone = {
    /* Deduced from the palette; see the header above. */
    .ground = 0xfbf0d9, .ink    = 0x262a44,
    /* Amber: a salmon alarm was only dE 21 from the coral DRUMS. This is 45 (the deck's
     * own pair is 27). */
    .alarm  = 0xfdb03f,                           /* amber                   */
    /* The sage lifted away from the navy: as given they are exactly dE 40, the minimum. */
    .stem   = { 0xe8705d, 0x393f61, 0x869a5f },   /* coral / navy / sage     */
};
static const struct theme_palette k_pal_sandstone = {
    .selection     = 0x7c89ce,   /* ink contrast 4.2 as on the deck; the mapped #2c40b0 gave 1.65 */
    /* invert_l as in WHITE, so the deck's chrome is not left dark around our paper
     * panels. exempt_blue keeps lit controls bright enough for the inverted, dark
     * lettering. */
    .invert_l      = 1,
    .exempt_blue   = 1,
    /* Well above WHITE's 77: saturated colours are near fixed points of the inversion,
     * so the waveform would stay electric on cream. Fill blues are exempt anyway. */
    .sat_darken_q8 = 115,
    .sat_q8        = 256,
    /* Solved so the deck's black and white come out at #fbf0d9 and #262a44. #3d436d is
     * within a few levels of the palette's navy, which confirms the deduction.
     *
     * The duotone runs after the inversion (palette.c order), when the deck's black
     * chrome is already white, so `highlight` lands on the paper and `shadow` on the
     * lettering. In dark-theme order a light theme comes out inverted twice. */
    .shadow = 0x3d436d, .highlight = 0xf9e7c2, .tint_q8 = 160,
    .hue = { 0xe8705d, 0xfe985f, 0x6c7c6b, 0x393f61 }, .nhue = 4,
    /* Without this a light theme only nudges hues and looks like WHITE (see hue_pull_q8).
     * At 128 the palette's lightness and chroma lead while the deck's shading still shows;
     * 256 would flatten each hue to one colour and lose the waveform's relief. */
    .hue_pull_q8 = 128,
};

/* Order is part of the on-disk format (the settings file stores an index): append only;
 * see theme.h. Index 0 must stay palette-less: hooks treat a NULL palette as "do nothing",
 * and every clamp falls back to 0. */
/* The third column is the mods' UI roles. NULL derives them (from the seed if there is
 * one, else from ORIGINAL's through the palette). Author one only where derivation gets a
 * role wrong, typically stem[]. See struct theme_ui. */
const struct mod_theme k_mod_themes[MOD_THEME_MAX] = {
    /*  name           palette              ui    seed                  light */
    { "ORIGINAL",    NULL,                  NULL, NULL,                 0 },
    { "WHITE",       &k_pal_white,          NULL, NULL,                 1 },
    { "CYBERPUNK",   &k_pal_cyberpunk,      NULL, &k_seed_cyberpunk,    0 },
    { "NEON",        &k_pal_neon,           NULL, &k_seed_neon,         0 },
    { "MOCHA",       &k_pal_mocha,          NULL, &k_seed_mocha,        0 },
    { "AURORA",      &k_pal_aurora,         NULL, &k_seed_aurora,       0 },
    { "SANDSTONE",   &k_pal_sandstone,      NULL, &k_seed_sandstone,    1 },
};

const struct mod_theme *mod_theme(void)
{
    int id = __atomic_load_n(&g_theme_id, __ATOMIC_RELAXED);

    /* The id comes from the eMMC. Out of range (e.g. a settings file from a build
     * with more themes) reads as ORIGINAL. */
    if (id < 0 || id >= MOD_THEME_MAX)
        id = 0;
    return &k_mod_themes[id];
}

const char *mod_theme_name(int id)
{
    if (id < 0 || id >= MOD_THEME_MAX)
        id = 0;
    return k_mod_themes[id].name;
}
