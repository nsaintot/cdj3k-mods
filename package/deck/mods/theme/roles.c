// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * roles.c - the UI roles: what the mod's own controls are painted with.
 *
 * See struct theme_ui in theme.h for what a role is and why the generic pixel transform
 * cannot stand in for one.
 *
 * ORIGINAL reproduces the deck's own colours exactly: with it selected nothing the
 * mods draw differs from stock. Anything that moves on screen under ORIGINAL is a bug.
 */
#include "theme/theme.h"
#include "juce/draw.h"

#include <stddef.h>   /* offsetof, for the role pairs below */

/* ================================================================== */
/* ORIGINAL                                                           */
/* ================================================================== */

/* The deck's measured values. What each role has to satisfy -- which is what a NEW
 * theme has to answer:
 *
 *   bypass   Orange, not the accent. On this deck blue means "on, selected, working",
 *            so lighting the bypass blue announced the stems were on at the exact moment
 *            they were switched out. Orange is the deck's own word for an override --
 *            MASTER PLAYER, the +-10 range, the master BPM all wear it -- and a latched
 *            bypass belongs in that family. Sampled off the +-10 badge, not invented.
 *
 *   stem[]   Three hues that must stay apart from EACH OTHER, because the colour is the
 *            only thing naming which stem a wedge, a caption or an icon bar belongs to.
 *            This is the role a generic transform cannot derive: any duotone pulls all
 *            three toward one ramp and the naming collapses.
 *
 *   text_on_accent   Near-black, for lettering on a lit plate. Amber is a light fill and
 *            white on it is the one combination on this deck that does not hold at arm's
 *            length. A theme with a dark accent should raise this.
 *
 *   text_lit   White, for lettering on the ACCENT: the deck's selected DJ SETTING row is
 *            white on its blue. Not text_on_accent, whose fills (amber, yellow, a stem)
 *            are the bright ones -- opposite polarity on this deck, see draw.h.
 *
 *   dead vs text_off   Two different absences. text_off is "switched off", dead is
 *            "nothing here yet", and dead is the darker of the two so the two states do
 *            not read alike.
 *
 *   warn / refuse   Amber says nothing is broken, there is just nothing to work with.
 *            Red is reserved for the answer to a press the deck is refusing, and it
 *            lasts only as long as the flash. Keep them distinct or the badge starts
 *            reading as an error.
 */
const struct theme_ui k_ui_original = {
    .surface        = 0xff323232u,   /* button off / wedge off, one value twice          */
    .surface2       = 0xff232323u,   /* the deck's own second checker grey, measured      */
    .edge           = 0xff5a5a5au,
    /* The deck's own lit quick-menu plate, measured off the panel -- the pair the
     * comment two hundred lines down already names and this did not match. Ours came
     * out #0082e2/#005b9e against the deck's #007de1/#0064a5 on the button beside it,
     * which is a thing that moves under ORIGINAL, and nothing may. */
    .accent         = 0xff007de1u,
    .accent2        = 0xff0064a5u,
    /* The source badge's HUE at a lit plate's brightness -- its own value (#87780a)
     * is a badge's, sat behind white lettering, and it carries near-black at only
     * 3.9:1. Scaled rather than lifted, so the channel ratios and therefore the hue
     * are exactly the badge's; the ink then reads at 11:1. */
    .mode           = 0xffe8ce11u,
    .bypass         = 0xffdc631eu,
    .xpad           = 0xffff0c21u,   /* the RMX-1000's own red, unthemed by design */
    .xpad_on        = 0xff34c04au,   /* HOLD / OVERDUB armed -- the deck's own green */
    .stem           = { 0xffe03a3au, /* DRUMS     red     */
                        0xff2f8fe8u, /* HARMONICS blue    */
                        0xff34c04au  /* VOCALS    green   */ },
    .text           = 0xffffffffu,
    .text_deck      = 0xffffffffu,   /* the deck's own lettering */
    .text_dim       = 0xffafafafu,   /* the skin grey, off the Ver label */
    .text_value     = 0xff7d7d7du,   /* a DJ SETTING row's value, measured off the list */
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
    .bar            = 0xff7d7d7du,   /* MOD_COL_BTN_BAR    -- measured off stock */
    .bar_on         = 0xffafafafu,   /* MOD_COL_BTN_BAR_ON -- measured off stock */
};

/* ================================================================== */
/* Derivation                                                         */
/* ================================================================== */

/* One theme's worth of derived roles, and which theme they belong to. Rebuilt when the
 * selection moves and not otherwise, so a paint costs a compare.
 *
 * -1 rather than 0 for "nothing cached": 0 is a real theme id (ORIGINAL), and seeding
 * the cache as if that had already been derived would hand out a zeroed struct for the
 * one theme that must be exact. */
static struct theme_ui g_derived;
static int             g_derived_id = -1;

/* NOT the fill path, despite these being fills.
 *
 * `exempt_blue` spares blues from the darkening stage, for the deck's SELECTION ROWS:
 * surfaces with a label on top, where darkening costs the label its contrast. Roles are
 * the other thing -- a stem colour, an accent bar, a fader's lit steps are INK on the
 * panel, and on a light ground ink has to darken or it stops being visible. Taking the
 * fill carve-out leaves a light theme's fader bars lighter than their ground. */
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

    /* Walked as a flat array of ARGB words on purpose. Every member is one, and naming
     * each of the eighteen here would be a list to forget to extend -- a role added to
     * the struct and not to this loop would silently come out black. */
    for (i = 0; i < n; i++)
        dst[i] = theme_palette_argb(pal, src[i], ROLE_IS_FILL);

    /* The one role a palette cannot carry through: text_lit follows the ACCENT's
     * polarity, and WHITE turns the deck's white lettering black while its blue plate,
     * carved out, stays a blue -- black on blue, measured on the GATE CUE plate. The
     * deck's own +-10 badge under the same palette darkens its fill and turns its
     * lettering white, which is the rule: of the deck's white and black through this
     * palette, the one further from the accent goes on it. */
    w = theme_palette_argb(pal, 0xffffffffu, ROLE_IS_FILL);
    k = theme_palette_argb(pal, 0xff000000u, ROLE_IS_FILL);
    out->text_lit = lum_gap(out->accent, w) >= lum_gap(out->accent, k) ? w : k;
}

/* ================================================================== */
/* Seed -> roles                                                      */
/* ================================================================== */

/* Every rule here is a shade of one of the seven, and each says what it is FOR rather
 * than what it looks like -- which is what lets the same rules serve a near-black
 * ground and a near-white one.
 *
 * `toward` moves a colour toward the ink on a dark theme and toward the ground on a
 * light one, i.e. always toward "more contrast against the panel". That single flip is
 * what .light buys: without it every derivation would need writing twice. */
/* Perceptual-ish luminance, 0..255. Integer on purpose: this runs once per theme
 * change and has no business pulling a float pipeline into the shim. */
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

/* Turn a colour around the wheel, keeping its value and its chroma exactly.
 *
 * THE ALARM FAMILY IS TWO HUES, NOT TWO WEIGHTS. On this deck refuse is #d93025 at 4
 * degrees and warn is #e8a317 at 40 -- a red and an amber, and a DJ tells them apart by
 * colour. Deriving warn as the alarm pulled toward the lettering reproduces the weight
 * and not the hue, so on a seeded theme the two came back as one colour at two
 * brightnesses: dE 14 to 23 against the deck's 59, which is a warning that reads as a
 * refusal in the dark. Grey has no hue to turn and is returned as it came. */
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
    /* The two directions a derived role can move, named by what they are FOR, not by
     * which is brighter:
     *
     *   up    stand out from the background -- a border, a scale tick, a button's bar.
     *   down  recede into it -- dimmed, switched-off or disabled lettering.
     *
     * Neither depends on polarity: "away from the background" is the ink on a dark
     * theme and equally on a light one. Reading up/down as brighter/darker instead
     * makes a light theme derive its borders toward the paper (invisible) and its
     * text_dim/text_off toward the ink, collapsing three text states into one. */
    uint32_t up   = sd->ink;
    uint32_t down = sd->ground;
    int i;

    /* DERIVED, exactly like the accent below and for the same reason. An UNLIT control
     * is no more a thing a theme gets to invent than a lit one: our X-PAD and STEMS
     * buttons sit in the deck's own band, between BEAT LOOP and KEY SHIFT, and if our
     * plate does not agree with theirs the button reads as belonging to a different
     * program. Authored, it did not agree -- CYBERPUNK put two olive buttons next to
     * three grey ones, and every role hanging off the plate went with them: the checker's
     * second half landed 50 levels away instead of 15, and the label on it fell to 2.5
     * contrast against the deck's 12.8.
     *
     * The pair is the point. surface2 is the deck's OTHER checker grey through the same
     * palette, so taking both down one path is what makes our stipple the deck's stipple
     * -- same two greys, same distance, correct side, in every theme at once. Deriving
     * one and authoring the other is what let them come apart.
     *
     * A seed's `plate` is still what the rest of the plate family is measured from
     * BELOW; what it no longer does is decide the colour of a button standing next to
     * the deck's own. */
    out->surface        = pal ? theme_palette_argb(pal, k_ui_original.surface, ROLE_IS_FILL)
                              : k_ui_original.surface;
    out->surface2       = pal ? theme_palette_argb(pal, k_ui_original.surface2, ROLE_IS_FILL)
                              : k_ui_original.surface2;
    out->edge           = toward(out->surface, up, 64);  /* just enough to read as a border */
    /* DERIVED, not authored -- see struct theme_seed. ORIGINAL's accent IS the colour the
     * deck lights its own quick-menu buttons with (measured off the panel: a checker on
     * #007de1 with white lettering), so putting it through this theme's palette is what
     * makes our lit button and the three beside it agree in every theme at once. With no
     * palette there is nothing to put it through and the source value is already right. */
    /* ...and through the same path as everything else, which is worth stating because the
     * fill path looks like the right answer and is not. Its carve-out spares blues from
     * the darkening, on the grounds that a lit surface carries a label; but the deck's own
     * quick-menu plate is SPRITE pixels, not a fill, so the deck's lit button IS darkened
     * and taking the carve-out left ours bright cyan next to the deck's slate blue --
     * measured (29,179,255) against (18,107,152) on CREAM. Matching the deck means taking
     * the path the deck takes. */
    out->accent         = pal ? theme_palette_argb(pal, k_ui_original.accent, ROLE_IS_FILL)
                              : k_ui_original.accent;
    out->accent2        = pal ? theme_palette_argb(pal, k_ui_original.accent2, ROLE_IS_FILL)
                              : k_ui_original.accent2;
    /* The third weight of the alarm family, after warn and refuse -- calmed down far
     * enough to be a SURFACE rather than a signal, which is what a mode plate is. It
     * has to stay out of the accent's family, because "a mode is engaged" and "this is
     * switched on" are the two things a DJ must not have to tell apart by shade. */
    out->mode           = toward(sd->alarm, out->surface, 64);
    out->bypass         = sd->alarm;
    /* Follows the alarm at full, like the refusal: under a palette the pad has no
     * claim to a hue of its own, and the loudest chromatic thing a theme has is
     * the right weight for the loudest mark on the strip. */
    out->xpad           = sd->alarm;
    /* The palette's own green rather than a derivation of one: green is not a
     * role a duotone can invent, and the stems are where every palette in this
     * file already has to author one. */
    out->xpad_on        = sd->stem[2];
    for (i = 0; i < 3; i++)
        out->stem[i]    = sd->stem[i];
    out->text           = sd->ink;
    /* Derived, like the plate and the accent: a word ON one of the deck's own surfaces
     * is the deck's, whatever ink the theme prefers for its own panels. */
    out->text_deck      = pal ? theme_palette_argb(pal, k_ui_original.text_deck, ROLE_IS_FILL)
                              : k_ui_original.text_deck;
    out->text_dim       = toward(sd->ink, down, 80);
    /* DERIVED, like the plate: this one is not ours to invent, it is a colour in the
     * deck's own list. Pulling the seed's ink toward the ground by a fixed fraction
     * would reproduce it on a dark theme and not on a light one -- the deck goes 35%
     * of the way on ORIGINAL and 51% on SANDSTONE, because its grey is a COLOUR that
     * gets mapped and not a proportion that gets re-derived. */
    out->text_value     = pal ? theme_palette_argb(pal, k_ui_original.text_value, ROLE_IS_FILL)
                              : k_ui_original.text_value;
    out->text_off       = toward(sd->ink, down, 150);   /* switched off, still legible */
    /* Lettering ON the accent, and the ONE role here that genuinely turns on polarity --
     * everything else above is a direction, and directions do not flip. Whichever of
     * ground/ink the accent is FURTHER from is the one that will hold on it, and the
     * accent is always the BRIGHT thing: on a dark theme that makes it the ground, on a
     * light one the ink.
     *
     * That is a constraint on the seed, not just a derivation: a light theme whose accent
     * is dark gets dark lettering on a dark fill. Pick a light theme's accent bright
     * enough to carry ink -- an amber or a warm yellow, not a mid-blue. */
    /* MEASURED, not assumed from the polarity. The rule above is the right one; keying
     * it on .light is a proxy for it that holds only while a light theme's accent is
     * bright. SANDSTONE's is not, and the proxy handed it dark ink on a dark fill --
     * 1.23 contrast against the deck's 4.39, which is lettering you cannot read.
     * Asking which of the two the accent is actually further from costs two
     * subtractions and cannot be wrong. */
    out->text_on_accent = lum_gap(out->accent, sd->ink) > lum_gap(out->accent, sd->ground)
                          ? sd->ink : sd->ground;
    /* The same measurement, kept as its own role because on ORIGINAL the two fills it
     * and text_on_accent sit on have opposite polarity (see draw.h). Here the accent is
     * the only fill measured, so the two agree -- by construction, not by accident. */
    out->text_lit       = out->text_on_accent;
    (void)light;
    out->dead           = toward(out->surface, down, 96);  /* "nothing here yet": below the plate */
    out->icon_disabled  = toward(sd->ink, down, 110);
    out->track          = toward(out->surface, down, 48);
    out->tick           = toward(out->surface, up, 40);
    out->mark           = toward(sd->ground, down, 128);
    /* warn and refuse have to stay TELLABLE APART -- one says "nothing to work with",
     * the other answers a press. Same family, and the deck separates them by HUE and
     * then by weight: 36 degrees toward the amber, then pulled back toward the
     * lettering. Weight alone was not enough to tell them apart; see hue_rotate. */
    out->warn           = toward(hue_rotate(sd->alarm, 36), sd->ink, 72);
    out->refuse         = sd->alarm;
    /* The deck's own two bar greys through the palette. Derived from our plate instead,
     * the lit one missed the deck's on five of the six themes -- #cdd9d5 against
     * #a4aeaa on AURORA, #47485b against #686772 on SANDSTONE -- on a mark whose whole
     * job is to say the button belongs to that row. */
    out->bar            = pal ? theme_palette_argb(pal, k_ui_original.bar, ROLE_IS_FILL)
                              : k_ui_original.bar;
    out->bar_on         = pal ? theme_palette_argb(pal, k_ui_original.bar_on, ROLE_IS_FILL)
                              : k_ui_original.bar_on;

    /* Opaque, always.
     *
     * A seed is authored as plain 0xRRGGBB -- there is no alpha in it, and there should
     * not be: a seed states hues, not coverage. But the roles taken STRAIGHT off it
     * inherit that missing byte, so surface, text, accent, bypass, refuse,
     * text_on_accent and all three stems came out fully TRANSPARENT and the mod's own UI
     * painted nothing at all on every seeded theme. The roles routed through toward()
     * were fine, because it assembles its result with the alpha already in -- which is
     * why what survived on screen was one button edge and one bar, and nothing else.
     *
     * ORIGINAL and WHITE were the two that looked right, and that is not a coincidence
     * worth reading as evidence about them: they are simply the only themes that never
     * reach this function.
     *
     * Forced once here rather than at each assignment, over the struct as a flat array of
     * ARGB words -- the same reason derive() walks it that way. A role added to the struct
     * and forgotten in a hand-written list is precisely the omission that caused this. */
    {
        uint32_t *p = (uint32_t *)out;
        unsigned w, n = sizeof(*out) / sizeof(uint32_t);

        for (w = 0; w < n; w++)
            p[w] |= 0xff000000u;
    }
}

/* Two roles that NAME DIFFERENT THINGS must not be the same colour.
 *
 * Deliberately an exact-match test and not a contrast one. It is not trying to judge a
 * theme -- it is catching the one mistake a seed actually makes, which is reusing one of
 * its own colours for two jobs: three of the five seeds here had `alarm` set to one of
 * their own stems, so the bypass icon sat inside the wedge it exists to be told apart
 * from, and on one of them the X-PAD's armed flags came out identical to its value.
 * Nothing said so. An exact match cannot cry wolf about a theme that is merely tight.
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

/* Bumped whenever the roles under a caller change. See mod_ui_gen in draw.h for what
 * it is for: a colour STORED on one of our components -- a juce::Label's lettering --
 * is not re-read at paint time the way a fill from mod_ui() is, so it survives a theme
 * change unless something puts it back. */
static unsigned ui_g_gen = 1;

unsigned mod_ui_gen(void)
{
    return __atomic_load_n(&ui_g_gen, __ATOMIC_RELAXED);
}

/* See draw.h. The palette IS the hook's transform, so this and an unbracketed fill
 * of the same value land on the same pixel; `is_fill` is 1 for the same reason the
 * hook passes 1 -- this is a surface, not a glyph. */
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

    /* Tell the draw kit which way the ground goes. Done here rather than from the theme
     * switch because this is the call EVERY consumer already makes, so the kit cannot be
     * left on a stale polarity by a path that changed the theme without announcing it. */
    if (t->light != told) {
        told = t->light;
        mod_draw_ground(t->light);
    }

    /* Authored beats derived: a theme that says what its stems are gets to keep them. */
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
    /* ORIGINAL, and any theme that only re-tints images: no transform to apply, so hand
     * back the source rather than a copy of it. */
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
