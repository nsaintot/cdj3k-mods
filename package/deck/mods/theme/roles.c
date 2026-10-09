// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * roles.c - the UI roles: the colours the mods' own controls are painted with.
 *
 * See struct theme_ui in juce/draw.h for what a role is.
 *
 * ORIGINAL reproduces the deck's own colours exactly, so nothing the mods draw differs
 * from stock. Anything that changes on screen under ORIGINAL is a bug.
 */
#include "theme/theme.h"
#include "juce/draw.h"

#include <stddef.h>   /* offsetof, for the role pairs below */

/* ================================================================== */
/* ORIGINAL                                                           */
/* ================================================================== */

/* The deck's own values. Constraints each role must satisfy, in any theme:
 *
 *   bypass   Orange, not the accent: on this deck blue means "on, selected, working".
 *            Orange is the deck's override colour (MASTER PLAYER, the +-10 range, the
 *            master BPM). The +-10 badge's colour.
 *
 *   stem[]   Three hues distinct from each other: colour is the only thing naming which
 *            stem a wedge, caption or icon bar belongs to. A duotone would pull all three
 *            toward one ramp, so these cannot be derived generically.
 *
 *   text_on_accent   Near-black, for lettering on a lit plate. Amber is a light fill and
 *            white on it does not read at arm's length. A theme with a dark accent should
 *            raise this.
 *
 *   text_lit   White, for lettering on the accent: the deck's selected DJ SETTING row is
 *            white on blue. Differs from text_on_accent, whose fills (amber, yellow, a
 *            stem) are bright: opposite polarity on this deck, see draw.h.
 *
 *   dead vs text_off   text_off is "switched off", dead is "nothing here yet"; dead is
 *            darker so the two do not read alike.
 *
 *   warn / refuse   Amber: nothing is broken, there is nothing to work with. Red: the
 *            deck is refusing a press, shown only for the flash. Keep them distinct or
 *            the badge reads as an error.
 */
const struct theme_ui k_ui_original = {
    .surface        = 0xff323232u,   /* button off / wedge off, one value twice          */
    .surface2       = 0xff232323u,   /* the deck's own second checker grey               */
    .edge           = 0xff5a5a5au,
    /* The deck's lit quick-menu plate (THEME_DECK_SELECT and THEME_DECK_SELECT2). */
    .accent         = 0xff007de1u,
    .accent2        = 0xff0064a5u,
    /* The source badge's hue at a lit plate's brightness. The badge itself (#87780a)
     * carries near-black at only 3.9:1; scaled (same channel ratios, so the same hue)
     * the ink reads at 11:1. */
    .mode           = 0xffe8ce11u,
    .bypass         = 0xffdc631eu,
    .xpad           = 0xffff0c21u,   /* the RMX-1000's own red, unthemed by design */
    .xpad_on        = 0xff34c04au,   /* HOLD / OVERDUB armed -- the deck's own green */
    .stem           = { 0xffe03a3au, /* DRUMS     red     */
                        0xff2f8fe8u, /* HARMONICS blue    */
                        0xff34c04au  /* VOCALS    green   */ },
    .text           = 0xffffffffu,
    .text_deck      = 0xffffffffu,   /* the deck's own lettering */
    .text_dim       = 0xffafafafu,   /* the skin grey of the Ver label */
    .text_value     = 0xff7d7d7du,   /* a DJ SETTING row's value */
    .text_off       = 0xff6e6e6eu,
    .text_on_accent = 0xff1a1a1au,
    .text_lit       = 0xffffffffu,   /* the deck's own lettering on its selected row */
    .dead           = 0xff3a3a3au,
    .icon_disabled  = 0xffb7b7b7u,
    .track          = 0xff3c3c3cu,
    .tick           = 0xff505050u,
    .mark           = 0xff0a0a0au,
    .warn           = 0xffe8a317u,
    .refuse         = 0xffd93025u,
    .bar            = 0xff7d7d7du,   /* MOD_COL_BTN_BAR    -- stock */
    .bar_on         = 0xffafafafu,   /* MOD_COL_BTN_BAR_ON -- stock */
};

/* ================================================================== */
/* Derivation                                                         */
/* ================================================================== */

/* Derived roles for one theme, and that theme's id. Rebuilt only when the selection
 * changes, so a paint costs a compare. -1 means nothing cached (0 is ORIGINAL). */
static struct theme_ui g_derived;
static int             g_derived_id = -1;

/* Roles do not take the fill path's exempt_blue carve-out. That carve-out protects
 * selection rows carrying a label; roles (stem colours, accent bars, a fader's lit steps)
 * are ink on the panel and must darken on a light ground or they become invisible. */
#define ROLE_IS_FILL 0

const struct theme_ui *mod_ui_stock(void)
{
    return &k_ui_original;
}

static uint32_t lum_gap(uint32_t a, uint32_t b);

static void derive(struct theme_ui *out, const struct theme_palette *pal)
{
    const uint32_t *src = (const uint32_t *)&k_ui_original;
    uint32_t *dst = (uint32_t *)out;
    unsigned i, n = sizeof(k_ui_original) / sizeof(uint32_t);
    uint32_t w, k;

    /* Walked as a flat array of ARGB words (every member is one), so a role added to the
     * struct is covered automatically. */
    for (i = 0; i < n; i++)
        dst[i] = theme_palette_argb(pal, src[i], ROLE_IS_FILL);

    /* text_lit follows the accent's polarity, which the palette does not carry: WHITE
     * turns white lettering black but leaves the blue plate blue (black on blue on the
     * GATE CUE plate). Rule, as the deck's +-10 badge does it: of white and black through
     * this palette, use the one further from the accent. */
    w = theme_palette_argb(pal, 0xffffffffu, ROLE_IS_FILL);
    k = theme_palette_argb(pal, 0xff000000u, ROLE_IS_FILL);
    out->text_lit = lum_gap(out->accent, w) >= lum_gap(out->accent, k) ? w : k;
}

/* ================================================================== */
/* Seed -> roles                                                      */
/* ================================================================== */

/* Every rule here is a shade of one of the seven seed colours, expressed by purpose
 * (contrast against the panel or not) so the same rules serve dark and light grounds. */
/* Approximate perceptual luminance, 0..255. Integer: runs once per theme change and
 * keeps floating point out of the shim. */
static uint32_t lum8(uint32_t c)
{
    return (54u * ((c >> 16) & 0xffu) +
           183u * ((c >>  8) & 0xffu) +
            19u * ( c        & 0xffu)) >> 8;
}

static uint32_t lum_gap(uint32_t a, uint32_t b)
{
    uint32_t la = lum8(a), lb = lum8(b);

    return la > lb ? la - lb : lb - la;
}

/* Rotate a colour around the hue wheel, keeping its value and chroma exactly. Grey is
 * returned unchanged.
 *
 * The alarm family differs by hue, not weight: refuse is #d93025 (4 degrees) and warn is
 * #e8a317 (40). Deriving warn only by pulling the alarm toward the lettering gives the
 * same hue at two brightnesses (dE 14 to 23, against the deck's 59). */
static uint32_t hue_rotate(uint32_t c, int deg)
{
    int r = (int)((c >> 16) & 0xffu), g = (int)((c >> 8) & 0xffu), b = (int)(c & 0xffu);
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int d = mx - mn, h, i, f, v = mx, q, t, lo;

    if (!d)
        return c;
    if (mx == r)      h = (60 * (g - b)) / d;
    else if (mx == g) h = 120 + (60 * (b - r)) / d;
    else              h = 240 + (60 * (r - g)) / d;
    h = ((h + deg) % 360 + 360) % 360;

    i  = h / 60;
    f  = h % 60;
    lo = v - d;
    q  = v - (d * f) / 60;
    t  = v - (d * (60 - f)) / 60;
    switch (i) {
    case 0:  r = v;  g = t;  b = lo; break;
    case 1:  r = q;  g = v;  b = lo; break;
    case 2:  r = lo; g = v;  b = t;  break;
    case 3:  r = lo; g = q;  b = v;  break;
    case 4:  r = t;  g = lo; b = v;  break;
    default: r = v;  g = lo; b = q;  break;
    }
    return 0xff000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static uint32_t toward(uint32_t c, uint32_t target, uint32_t q8)
{
    uint32_t r = (c >> 16) & 0xffu, g = (c >> 8) & 0xffu, b = c & 0xffu;
    uint32_t tr = (target >> 16) & 0xffu, tg = (target >> 8) & 0xffu, tb = target & 0xffu;

    r = (r * (256u - q8) + tr * q8) >> 8;
    g = (g * (256u - q8) + tg * q8) >> 8;
    b = (b * (256u - q8) + tb * q8) >> 8;
    return 0xff000000u | (r << 16) | (g << 8) | b;
}

void theme_ui_expand(struct theme_ui *out, const struct theme_palette *pal,
                     const struct theme_seed *sd, int light)
{
    /* The two directions a derived role can move, by purpose, not brightness:
     *
     *   up    stand out from the background: a border, a scale tick, a button's bar.
     *   down  recede into it: dimmed, switched-off or disabled lettering.
     *
     * Both are polarity-independent (away from the background is toward the ink on any
     * theme). Treating them as brighter/darker breaks light themes. */
    uint32_t up   = sd->ink;
    uint32_t down = sd->ground;
    int i;

    /* Derived like the accent below: our X-PAD and STEMS buttons sit in the deck's band
     * between BEAT LOOP and KEY SHIFT and must match the deck's plates. surface2 is the
     * deck's other checker grey through the same palette, so both checker halves match
     * the deck's stipple in every theme. */
    out->surface        = pal ? theme_palette_argb(pal, k_ui_original.surface, ROLE_IS_FILL)
                              : k_ui_original.surface;
    out->surface2       = pal ? theme_palette_argb(pal, k_ui_original.surface2, ROLE_IS_FILL)
                              : k_ui_original.surface2;
    out->edge           = toward(out->surface, up, 64);  /* just enough to read as a border */
    /* Derived, not authored (see struct theme_seed). ORIGINAL's accent is the deck's lit
     * quick-menu colour (a checker on #007de1 with white lettering), so mapping it through
     * this palette keeps our lit button matching the deck's.
     *
     * ROLE_IS_FILL, not the fill path: the deck's plate is sprite pixels, which get no
     * blue carve-out, so the fill path would leave ours brighter. */
    out->accent         = pal ? theme_palette_argb(pal, k_ui_original.accent, ROLE_IS_FILL)
                              : k_ui_original.accent;
    out->accent2        = pal ? theme_palette_argb(pal, k_ui_original.accent2, ROLE_IS_FILL)
                              : k_ui_original.accent2;
    /* The alarm family toned down to a surface, for mode plates. Kept out of the accent's
     * family so "a mode is engaged" and "switched on" differ by hue, not shade. */
    out->mode           = toward(sd->alarm, out->surface, 64);
    out->bypass         = sd->alarm;
    /* The alarm at full, like refuse: the loudest mark on the strip. */
    out->xpad           = sd->alarm;
    /* The seed's VOCALS green: a duotone cannot derive a green. */
    out->xpad_on        = sd->stem[2];
    for (i = 0; i < 3; i++)
        out->stem[i]    = sd->stem[i];
    out->text           = sd->ink;
    /* Derived like the plate and accent: lettering on the deck's own surfaces. */
    out->text_deck      = pal ? theme_palette_argb(pal, k_ui_original.text_deck, ROLE_IS_FILL)
                              : k_ui_original.text_deck;
    out->text_dim       = toward(sd->ink, down, 80);
    /* Derived: it is a colour in the deck's own list. A fixed ink-to-ground fraction would
     * not match (the deck's value sits 35% of the way on ORIGINAL, 51% on SANDSTONE). */
    out->text_value     = pal ? theme_palette_argb(pal, k_ui_original.text_value, ROLE_IS_FILL)
                              : k_ui_original.text_value;
    out->text_off       = toward(sd->ink, down, 150);   /* switched off, still legible */
    /* Lettering on the accent: whichever of ground and ink is further from the accent in
     * luminance. Computed, not keyed on .light: SANDSTONE's accent is dark, and a
     * polarity rule would put dark ink on a dark fill. */
    out->text_on_accent = lum_gap(out->accent, sd->ink) > lum_gap(out->accent, sd->ground)
                          ? sd->ink : sd->ground;
    /* A separate role because on ORIGINAL the fills under it and text_on_accent have
     * opposite polarity (see draw.h). Here only the accent is measured, so they agree. */
    out->text_lit       = out->text_on_accent;
    (void)light;
    out->dead           = toward(out->surface, down, 96);  /* "nothing here yet": below the plate */
    out->icon_disabled  = toward(sd->ink, down, 110);
    out->track          = toward(out->surface, down, 48);
    out->tick           = toward(out->surface, up, 40);
    out->mark           = toward(sd->ground, down, 128);
    /* warn and refuse must stay distinct. As on the deck, separated by hue first (36
     * degrees toward amber), then by weight (toward the lettering); see hue_rotate. */
    out->warn           = toward(hue_rotate(sd->alarm, 36), sd->ink, 72);
    out->refuse         = sd->alarm;
    /* The deck's two bar greys through the palette, so our bars match the deck's row
     * (deriving from our plate does not). */
    out->bar            = pal ? theme_palette_argb(pal, k_ui_original.bar, ROLE_IS_FILL)
                              : k_ui_original.bar;
    out->bar_on         = pal ? theme_palette_argb(pal, k_ui_original.bar_on, ROLE_IS_FILL)
                              : k_ui_original.bar_on;

    /* Force every role opaque. Seeds are authored as 0xRRGGBB with no alpha, so roles
     * copied straight from a seed would be fully transparent (toward() sets alpha
     * itself). Done over the struct as a flat array of ARGB words, as in derive(), so new
     * roles are covered. */
    {
        uint32_t *p = (uint32_t *)out;
        unsigned w, n = sizeof(*out) / sizeof(uint32_t);

        for (w = 0; w < n; w++)
            p[w] |= 0xff000000u;
    }
}

/* Log role pairs that name different things but have the same colour.
 *
 * An exact-match test, not a contrast one: it catches a seed reusing one of its colours
 * for two jobs (e.g. `alarm` equal to a stem, putting the bypass icon in the wedge's own
 * colour) without flagging themes that are merely low-contrast.
 *
 * Once per theme change, on the message thread. [message] */
static void theme_ui_warn(const struct theme_ui *u, const char *name)
{
    static const struct { unsigned a, b; const char *what; } k_pair[] = {
        { offsetof(struct theme_ui, stem[0]), offsetof(struct theme_ui, stem[1]),
          "DRUMS and HARMONICS" },
        { offsetof(struct theme_ui, stem[0]), offsetof(struct theme_ui, stem[2]),
          "DRUMS and VOCALS" },
        { offsetof(struct theme_ui, stem[1]), offsetof(struct theme_ui, stem[2]),
          "HARMONICS and VOCALS" },
        { offsetof(struct theme_ui, xpad),    offsetof(struct theme_ui, xpad_on),
          "the X-PAD's value and its armed flags" },
        { offsetof(struct theme_ui, bypass),  offsetof(struct theme_ui, stem[0]),
          "the bypass icon and DRUMS" },
        { offsetof(struct theme_ui, bypass),  offsetof(struct theme_ui, stem[1]),
          "the bypass icon and HARMONICS" },
        { offsetof(struct theme_ui, bypass),  offsetof(struct theme_ui, stem[2]),
          "the bypass icon and VOCALS" },
        { offsetof(struct theme_ui, accent),  offsetof(struct theme_ui, mode),
          "a lit control and a mode plate" },
        { offsetof(struct theme_ui, warn),    offsetof(struct theme_ui, refuse),
          "a warning and a refusal" },
        { offsetof(struct theme_ui, text),    offsetof(struct theme_ui, surface),
          "lettering and the plate under it" },
    };
    const uint8_t *base = (const uint8_t *)u;
    unsigned i;

    for (i = 0; i < sizeof(k_pair) / sizeof(*k_pair); i++) {
        uint32_t a, b;

        memcpy(&a, base + k_pair[i].a, sizeof(a));
        memcpy(&b, base + k_pair[i].b, sizeof(b));
        if (a == b)
            MDBG("theme: %s paints %s the same colour (#%06x)\n",
                 name, k_pair[i].what, (unsigned)(a & 0xffffffu));
    }
}

/* Bumped whenever the roles change. See mod_ui_gen in draw.h: a colour stored on one of
 * our components (a juce::Label's lettering) is not re-read at paint time, so it must be
 * reset on a theme change. */
static unsigned ui_g_gen = 1;

unsigned mod_ui_gen(void)
{
    return __atomic_load_n(&ui_g_gen, __ATOMIC_RELAXED);
}

/* See draw.h. Same transform as the setFill hook, so this matches an unbracketed fill of
 * the same value; `is_fill` is 1 as in the hook. */
uint32_t mod_colour_stock(uint32_t argb)
{
    const struct theme_palette *pal = mod_theme()->palette;

    return pal ? theme_palette_argb(pal, argb, 1) : argb;
}

const struct theme_ui *mod_ui(void)
{
    const struct mod_theme *t = mod_theme();
    int id = __atomic_load_n(&g_theme_id, __ATOMIC_RELAXED);
    static int told = -1;
    static int gen_id = -1;

    if (id != gen_id) {
        gen_id = id;
        __atomic_add_fetch(&ui_g_gen, 1, __ATOMIC_RELAXED);
    }

    /* Tell the draw kit the ground polarity. Done here because every consumer calls
     * mod_ui(), so the kit cannot miss a theme change. */
    if (t->light != told) {
        told = t->light;
        mod_draw_ground(t->light);
    }

    /* Authored roles take precedence over derived ones. */
    if (t->ui)
        return t->ui;
    if (t->seed) {
        if (id != g_derived_id) {
            theme_ui_expand(&g_derived, t->palette, t->seed, t->light);
            theme_ui_warn(&g_derived, t->name);
            g_derived_id = id;
            MDBG("theme: expanded %s from its seed (%s ground)\n",
                 t->name, t->light ? "light" : "dark");
        }
        return &g_derived;
    }
    /* No palette (ORIGINAL): return the source table itself. */
    if (t->palette == NULL)
        return &k_ui_original;
    if (id != g_derived_id) {
        derive(&g_derived, t->palette);
        g_derived_id = id;
        MDBG("theme: derived UI roles for %s (%s ground)\n",
             t->name, t->light ? "light" : "dark");
    }
    return &g_derived;
}
