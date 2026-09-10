// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * test_theme.c - the colour model (theme/palette.c) and the UI roles
 * (theme/roles.c) over the themes in theme/presets.c.
 */
#include "mods/theme/theme.h"
/* Declarations only, and the grid panel's plate colours with them. They are deck
 * values rather than roles, so nothing above covers them -- see the note on them in
 * panel_internal.h, and grid_plates_over_every_theme below. */
#include "mods/grid/panel_internal.h"
/* ...and the browse drag's two surfaces, for the same reason. */
#include "mods/browse/browse.h"

#include "test.h"

#define N_ROLES (sizeof(struct theme_ui) / sizeof(uint32_t))

/* The deck's own colours, transcribed from the panel measurements rather than
 * from roles.c: with ORIGINAL selected the mods must paint exactly these. */
static const char *const k_role_name[] = {
    "surface", "surface2", "edge", "accent", "accent2", "mode", "bypass",
    "xpad", "xpad_on",
    "stem[0]", "stem[1]", "stem[2]",
    "text", "text_deck", "text_dim", "text_value", "text_off", "text_on_accent",
    "text_lit",
    "dead", "icon_disabled", "track", "tick", "mark", "warn", "refuse",
    "bar", "bar_on"
};

/* Lightness, 0..255, as palette.c defines it. */
static int lightness(uint32_t argb)
{
    int r = (int)((argb >> 16) & 0xff), g = (int)((argb >> 8) & 0xff), b = (int)(argb & 0xff);
    int max = r > g ? r : g, min = r < g ? r : g;

    if (b > max) max = b;
    if (b < min) min = b;
    return (max + min) / 2;
}

static int distance(uint32_t a, uint32_t b)
{
    int d = lightness(a) - lightness(b);

    return d < 0 ? -d : d;
}

static void select_theme(int id)
{
    g_theme_id = id;
}

/* Force the derived-role cache to miss on the next select. */
static void evict(int id)
{
    select_theme((id + 1) % MOD_THEME_MAX);
    (void)mod_ui();
}

static void roles_named_compare(const char *what, const struct theme_ui *got,
                                const struct theme_ui *want)
{
    const uint32_t *g = (const uint32_t *)got, *w = (const uint32_t *)want;
    unsigned i;

    for (i = 0; i < N_ROLES; i++) {
        t_checks++;
        if (g[i] != w[i])
            T_FAILED("%s.%s: got %08x, want %08x", what, k_role_name[i], g[i], w[i]);
    }
}

static void original_is_exact(void)
{
    T_CASE("ORIGINAL exact");
    CHECK_INT((int)N_ROLES, (int)(sizeof k_role_name / sizeof *k_role_name));
    CHECK_STR(k_mod_themes[0].name, "ORIGINAL");
    CHECK(k_mod_themes[0].palette == NULL);

    select_theme(0);
    roles_named_compare("ORIGINAL", mod_ui(), &k_ui_original);

    /* Still exact after another theme has filled the derived cache. */
    evict(0);
    select_theme(0);
    roles_named_compare("ORIGINAL after evict", mod_ui(), &k_ui_original);

    /* An id off the eMMC is input, not a fact: out of range reads as ORIGINAL. */
    select_theme(MOD_THEME_MAX + 3);
    roles_named_compare("theme id overflow", mod_ui(), &k_ui_original);
    select_theme(-1);
    roles_named_compare("theme id negative", mod_ui(), &k_ui_original);
    select_theme(0);
}

static void every_role_opaque(void)
{
    int t;

    T_CASE("roles opaque");
    for (t = 0; t < MOD_THEME_MAX; t++) {
        const uint32_t *w;
        unsigned i;

        select_theme(t);
        w = (const uint32_t *)mod_ui();
        for (i = 0; i < N_ROLES; i++) {
            t_checks++;
            if ((w[i] >> 24) != 0xffu)
                T_FAILED("%s.%s alpha %02x", k_mod_themes[t].name, k_role_name[i],
                         (unsigned)(w[i] >> 24));
        }
    }
    select_theme(0);
}

static void seeded_themes_have_no_black_role(void)
{
    int t, seen = 0;

    T_CASE("no black role from a seed");
    for (t = 0; t < MOD_THEME_MAX; t++) {
        const uint32_t *w;
        unsigned i;

        if (k_mod_themes[t].seed == NULL)
            continue;
        seen++;
        select_theme(t);
        w = (const uint32_t *)mod_ui();
        for (i = 0; i < N_ROLES; i++) {
            t_checks++;
            if ((w[i] & 0x00ffffffu) == 0)
                T_FAILED("%s.%s is black", k_mod_themes[t].name, k_role_name[i]);
        }
    }
    CHECK(seen >= 5);
    select_theme(0);
}

static void derivation_is_deterministic(void)
{
    int t;

    T_CASE("derivation deterministic");
    for (t = 0; t < MOD_THEME_MAX; t++) {
        struct theme_ui first, expand_a, expand_b;

        select_theme(t);
        first = *mod_ui();
        evict(t);
        select_theme(t);
        roles_named_compare(k_mod_themes[t].name, mod_ui(), &first);

        if (k_mod_themes[t].seed == NULL)
            continue;
        memset(&expand_a, 0xa5, sizeof expand_a);
        memset(&expand_b, 0x5a, sizeof expand_b);
        theme_ui_expand(&expand_a, k_mod_themes[t].palette, k_mod_themes[t].seed,
                        k_mod_themes[t].light);
        theme_ui_expand(&expand_b, k_mod_themes[t].palette, k_mod_themes[t].seed,
                        k_mod_themes[t].light);
        roles_named_compare(k_mod_themes[t].name, &expand_b, &expand_a);

        /* A seed carries no alpha; expanding one without a palette must still
         * produce opaque roles. */
        memset(&expand_a, 0, sizeof expand_a);
        theme_ui_expand(&expand_a, NULL, k_mod_themes[t].seed, k_mod_themes[t].light);
        {
            const uint32_t *w = (const uint32_t *)&expand_a;
            unsigned i;

            for (i = 0; i < N_ROLES; i++) {
                t_checks++;
                if ((w[i] >> 24) != 0xffu)
                    T_FAILED("%s.%s alpha %02x with no palette", k_mod_themes[t].name,
                             k_role_name[i], (unsigned)(w[i] >> 24));
            }
        }
    }
    select_theme(0);
}

static void roles_stay_legible(void)
{
    int t;

    T_CASE("roles legible");
    for (t = 0; t < MOD_THEME_MAX; t++) {
        const struct theme_ui *u;

        select_theme(t);
        u = mod_ui();

        /* The stem colour is the only thing naming which stem a wedge belongs to. */
        CHECK(u->stem[0] != u->stem[1]);
        CHECK(u->stem[1] != u->stem[2]);
        CHECK(u->stem[0] != u->stem[2]);
        /* Lettering has to stand off the plate it sits on: dim ink on the
         * surface, lit ink on the accent (GATE CUE's plate when it is on). */
        CHECK(distance(u->text, u->surface) >= 64);
        CHECK(distance(u->text_lit, u->accent) >= 64);
        /* warn and refuse have to stay tellable apart. */
        CHECK(u->warn != u->refuse);

        if (k_mod_themes[t].seed == NULL)
            continue;
        /* Three text states, each further into the ground than the last. */
        {
            uint32_t ground = k_mod_themes[t].seed->ground | 0xff000000u;

            CHECK(distance(u->text, ground) > distance(u->text_dim, ground));
            CHECK(distance(u->text_dim, ground) > distance(u->text_off, ground));
        }
    }
    select_theme(0);
}

static void palette_identity_and_alpha(void)
{
    const struct theme_palette *white = k_mod_themes[1].palette;

    T_CASE("palette identity and alpha");
    CHECK_STR(k_mod_themes[1].name, "WHITE");
    /* ORIGINAL: not even an identity pass. */
    CHECK_U32(theme_palette_argb(NULL, 0xff123456u, 0), 0xff123456u);
    CHECK_U32(theme_palette_argb(NULL, 0x00abcdefu, 1), 0x00abcdefu);
    /* Alpha is the caller's throughout. */
    CHECK_U32(theme_palette_argb(white, 0x80123456u, 0) & 0xff000000u, 0x80000000u);
    CHECK_U32(theme_palette_argb(white, 0x00123456u, 1) & 0xff000000u, 0x00000000u);
}

/* The four worked examples documented on stage 1 in palette.c. */
static void palette_lightness_inversion(void)
{
    static const struct theme_palette invert_only = { .invert_l = 1, .sat_q8 = 256 };
    static const struct { uint32_t in, out; } k_cases[] = {
        { 0xff191919u, 0xffe6e6e6u },
        { 0xffffffffu, 0xff000000u },
        { 0xff007de1u, 0xff1e9bffu },
        { 0xffff0000u, 0xffff0000u },
    };
    unsigned i;

    T_CASE("stage 1 inversion");
    for (i = 0; i < sizeof k_cases / sizeof *k_cases; i++) {
        CHECK_U32(theme_palette_argb(&invert_only, k_cases[i].in, 0), k_cases[i].out);
        CHECK_U32(theme_palette_argb(&invert_only, k_cases[i].in, 1), k_cases[i].out);
    }
}

static void palette_white_polarity(void)
{
    const struct theme_palette *white = k_mod_themes[1].palette;
    int v;

    T_CASE("WHITE polarity");
    /* A grey has no chroma, so nothing but the inversion touches it. */
    for (v = 0; v < 256; v++) {
        uint32_t in = 0xff000000u | ((uint32_t)v * 0x010101u);
        uint32_t want = 0xff000000u | ((uint32_t)(255 - v) * 0x010101u);

        CHECK_U32(theme_palette_argb(white, in, 0), want);
    }
    /* exempt_blue: the deck's accent keeps its lift as a FILL and darkens as ink. */
    CHECK_U32(theme_palette_argb(white, 0xff007de1u, 1), 0xff1e9bffu);
    CHECK(theme_palette_argb(white, 0xff007de1u, 0) != 0xff1e9bffu);
}

/* theme.h promises a field left zero does nothing. sat_q8 is the one that has
 * to say so explicitly: 0 is the identity, 1 is greyscale. */
static void palette_sat_q8_zero_is_identity(void)
{
    static const struct theme_palette none = { .sat_q8 = 0 };
    static const struct theme_palette grey = { .sat_q8 = 1 };
    uint32_t got;
    int r, g, b;

    T_CASE("sat_q8 default");
    CHECK_U32(theme_palette_argb(&none, 0xff2f8fe8u, 0), 0xff2f8fe8u);
    CHECK_U32(theme_palette_argb(&none, 0xffe03a3au, 1), 0xffe03a3au);

    got = theme_palette_argb(&grey, 0xff2f8fe8u, 0);
    r = (int)((got >> 16) & 0xff);
    g = (int)((got >> 8) & 0xff);
    b = (int)(got & 0xff);
    CHECK(r - g <= 1 && g - r <= 1);
    CHECK(g - b <= 1 && b - g <= 1);
}

static void palette_stays_in_range(void)
{
    int t, f, r, g, b, over = 0;

    T_CASE("palette range");
    for (t = 0; t < MOD_THEME_MAX; t++) {
        for (f = 0; f < 2; f++) {
            for (r = 0; r < 256; r += 5) {
                for (g = 0; g < 256; g += 5) {
                    for (b = 0; b < 256; b += 5) {
                        uint32_t pr = (uint32_t)r, pg = (uint32_t)g, pb = (uint32_t)b;

                        theme_palette_rgb(k_mod_themes[t].palette, &pr, &pg, &pb, f);
                        if (pr > 255u || pg > 255u || pb > 255u)
                            over++;
                    }
                }
            }
        }
    }
    CHECK_INT(over, 0);
}

/* ---- the grid panel's plates ---------------------------------------------
 *
 * Every colour in the strip, through every theme. Not roles: these reach the screen
 * by mod_colour_stock, which is the setFill hook's own transform asked for by hand,
 * so roles_stay_legible says nothing about them.
 *
 * The bug this exists for: the plates were literals behind a mod_draw_enter bracket,
 * which is a declaration that they are ALREADY themed. They were not, and the panel
 * stayed a black slab under WHITE while the deck's five sprites beside it inverted.
 * A contrast check alone would not have caught that -- black on black-grey is as
 * legible as it ever was -- so the polarity check below is the one that matters. */
static void grid_plates_over_every_theme(void)
{
    static const struct { const char *name; uint32_t argb; } k_grid[] = {
        { "GROUP",    GP_COL_GROUP    }, { "LINE",     GP_COL_LINE     },
        { "FILL",     GP_COL_FILL     }, { "LINE_ON",  GP_COL_LINE_ON  },
        { "FILL_ON",  GP_COL_FILL_ON  }, { "LINE_OFF", GP_COL_LINE_OFF },
        { "FILL_OFF", GP_COL_FILL_OFF }, { "TEXT",     GP_COL_TEXT     },
        { "TEXT_OFF", GP_COL_TEXT_OFF }, { "CAP",      GP_COL_CAP      },
    };
    unsigned i;
    int t;

    T_CASE("grid plates over every theme");

    /* ORIGINAL is not a transform, so a deck value has to come back the deck value.
     * Bit-exact, not merely close: these are the reference design's own greys. */
    select_theme(0);
    for (i = 0; i < sizeof k_grid / sizeof *k_grid; i++)
        CHECK_U32(mod_colour_stock(k_grid[i].argb), k_grid[i].argb);
    select_theme(MOD_THEME_MAX + 3);
    CHECK_U32(mod_colour_stock(GP_COL_FILL), GP_COL_FILL);

    for (t = 0; t < MOD_THEME_MAX; t++) {
        uint32_t group, line, fill, line_on, fill_on, line_off, fill_off;
        uint32_t text, text_off, cap;

        select_theme(t);
        group    = mod_colour_stock(GP_COL_GROUP);
        line     = mod_colour_stock(GP_COL_LINE);
        fill     = mod_colour_stock(GP_COL_FILL);
        line_on  = mod_colour_stock(GP_COL_LINE_ON);
        fill_on  = mod_colour_stock(GP_COL_FILL_ON);
        line_off = mod_colour_stock(GP_COL_LINE_OFF);
        fill_off = mod_colour_stock(GP_COL_FILL_OFF);
        text     = mod_colour_stock(GP_COL_TEXT);
        text_off = mod_colour_stock(GP_COL_TEXT_OFF);
        cap      = mod_colour_stock(GP_COL_CAP);

        /* THE PLATE FOLLOWS THE GROUND. A light theme that leaves this dark is the
         * whole bug, and it is invisible to every other check here. */
        for (i = 0; i < sizeof k_grid / sizeof *k_grid; i++) {
            t_checks++;
            if ((mod_colour_stock(k_grid[i].argb) >> 24) != 0xffu)
                T_FAILED("%s grid %s lost its alpha", k_mod_themes[t].name,
                         k_grid[i].name);
        }
        t_checks++;
        if ((lightness(fill) > 128) != (k_mod_themes[t].light != 0))
            T_FAILED("%s: plate L=%d on a %s ground", k_mod_themes[t].name,
                     lightness(fill), k_mod_themes[t].light ? "light" : "dark");
        t_checks++;
        if ((lightness(text) < 128) != (k_mod_themes[t].light != 0))
            T_FAILED("%s: lettering L=%d on a %s ground", k_mod_themes[t].name,
                     lightness(text), k_mod_themes[t].light ? "light" : "dark");

        /* Lettering off its plate, in each of the three states. The disabled pair is
         * deliberately close -- it is saying there is nothing to undo -- so it gets a
         * floor of its own rather than the one the live states hold. */
        CHECK(distance(text, fill) >= 96);
        CHECK(distance(line_on, fill_on) >= 72);
        CHECK(distance(text_off, fill_off) >= 24);

        /* The border has to read against the plate it outlines, and the plate the
         * group sits on against the buttons on it -- the second is a backdrop and is
         * meant to be subtle, which is why its floor is where it is. */
        CHECK(distance(line, fill) >= 32);
        CHECK(distance(line_off, fill_off) >= 8);
        CHECK(distance(group, fill) >= 8);
        CHECK(distance(cap, group) >= 48);

        /* Three states of one control, and a DJ has to be able to tell which. */
        CHECK(fill != fill_on);
        CHECK(fill != fill_off);
        CHECK(fill_on != fill_off);
    }
    select_theme(0);
}

/* ---- the browse drag's two marks ------------------------------------------
 *
 * The gesture is read from a pair: the HOLE says where the track came from, the
 * MARKER says where it will land. Both used to be literals -- black and white --
 * which is correct against the deck's black list and wrong on every light theme,
 * where the hole was a black rectangle punched through it and the marker was white
 * on white. They are checked here rather than in a browse test because what can
 * break them is a theme, not the drag.
 *
 * Both are DECK values -- the list's ground and the list's own selected-row green --
 * so both go through mod_colour_stock and neither is a role. What is checked is that
 * the transform keeps them apart and keeps the hole on the right side of the ground. */
static void browse_drag_marks_over_every_theme(void)
{
    int t;

    T_CASE("browse drag marks over every theme");

    select_theme(0);
    CHECK_U32(mod_colour_stock(DG_HOLE_COL), DG_HOLE_COL);
    CHECK_U32(mod_colour_stock(DG_MARK_COL), DG_MARK_COL);

    for (t = 0; t < MOD_THEME_MAX; t++) {
        uint32_t ground, marker;

        select_theme(t);
        ground = mod_colour_stock(DG_HOLE_COL);
        marker = mod_colour_stock(DG_MARK_COL);

        /* The hole IS the ground, so it follows it: light theme, light hole. */
        t_checks++;
        if ((lightness(ground) > 128) != (k_mod_themes[t].light != 0))
            T_FAILED("%s: the drag hole is L=%d on a %s list",
                     k_mod_themes[t].name, lightness(ground),
                     k_mod_themes[t].light ? "light" : "dark");

        /* And the marker has to be seen ON it. A solid 4px bar, so the floor is
         * lower than lettering would take -- but not zero, which is what a literal
         * white gave on WHITE. */
        CHECK(distance(marker, ground) >= 48);
    }
    select_theme(0);
}

/* MAPPING A BUFFER TWICE IS A DEFECT, and this is the test that says so out loud.
 *
 * The whole image cache rests on one unstated assumption: a strip's pixels go through the
 * palette exactly once. Nothing in the types enforces it, and every bug in this area has
 * been a second pass arriving by some route nobody had drawn on the diagram -- an eviction
 * that freed the pristine copy while leaving the buffer mapped, a re-snapshot that took our
 * own output as the original, a source-themed strip mapped again on the way to the screen.
 *
 * They all hid the same way. A dark palette's transform is NEAR-IDEMPOTENT, so the second
 * pass changes almost nothing and five of the seven themes look perfect; a light one's is
 * destructive, and WHITE's is an exact INVOLUTION that puts the pixels straight back to
 * stock. That is the entire content of "dark themes are fine, only white goes weird", and
 * it is why these bugs survived so long: the deck's default theme cannot show them.
 *
 * So this pins the property rather than any one code path. If a future palette were made
 * idempotent, a double map would stop being visible -- and the invariant would still be
 * worth keeping, but the reasoning in image_sync.c would need rewriting rather than
 * silently becoming untestable. */
static void mapping_twice_is_destructive(void)
{
    /* The detailed waveform's colour group, off EP122 3.19 -- see k_wave_stock in wave.c.
     * Real strip pixels rather than invented ones, and entry 7 is the ground, which is 72%
     * of what is on screen. */
    static const uint32_t k_stock[] = {
        0xffffffffu, 0xffffa600u, 0xff0055e1u, 0xfff0d7ffu,
        0xffd2dcfau, 0xffb4690au, 0xfff5ebd7u, 0xff000000u,
    };
    int t;
    unsigned i;

    T_CASE("mapping twice is destructive");

    for (t = 0; t < MOD_THEME_MAX; t++) {
        const struct theme_palette *pal = k_mod_themes[t].palette;
        int worst = 0;

        if (pal == NULL) continue;                    /* ORIGINAL maps nothing */

        for (i = 0; i < sizeof k_stock / sizeof *k_stock; i++) {
            /* is_fill 0: these are ink and ground in a picture, which is the question
             * theme_map_pixel asks of every pixel it touches. */
            uint32_t once  = theme_palette_argb(pal, k_stock[i], 0);
            uint32_t twice = theme_palette_argb(pal, once, 0);
            int c;

            for (c = 0; c < 24; c += 8) {
                int d = (int)((once >> c) & 0xffu) - (int)((twice >> c) & 0xffu);

                if (d < 0) d = -d;
                if (d > worst) worst = d;
            }
        }

        /* MAGNITUDE, not mere inequality -- every palette here drifts a level or two under
         * a second pass and that is not what is being claimed. Measured across the group
         * above: the dark themes' worst channel moves 3, 7, 8 and 32, while WHITE moves a
         * full 255 and SANDSTONE 167. That gap IS the reason four of these bugs
         * shipped: on the deck's own dark default a double map is not merely subtle, it is
         * unobservable.
         *
         * Only the light half is asserted. A dark theme drifting further would be a fact
         * about that palette rather than a fault, but a LIGHT theme that stopped showing a
         * double map would quietly retire the only check anybody can run by eye. */
        if (k_mod_themes[t].light && worst < 64)
            T_FAILED("%s is light yet a double map moves it only %d -- nothing would be "
                     "left to catch one by eye", k_mod_themes[t].name, worst);
    }

    /* WHITE exactly, because it is the case the diagnosis turned on: peaks and ground are
     * pure white and pure black, the inversion swaps them, and a second pass swaps them
     * back to stock. Not "close to" stock -- the same bytes, which is why a doubly-mapped
     * strip looks untouched rather than damaged. */
    {
        const struct theme_palette *white = k_mod_themes[1].palette;
        uint32_t peaks_once, ground_once;

        CHECK_STR(k_mod_themes[1].name, "WHITE");
        peaks_once  = theme_palette_argb(white, 0xffffffffu, 0);
        ground_once = theme_palette_argb(white, 0xff000000u, 0);
        CHECK_U32(peaks_once,  0xff000000u);
        CHECK_U32(ground_once, 0xffffffffu);
        CHECK_U32(theme_palette_argb(white, peaks_once,  0), 0xffffffffu);
        CHECK_U32(theme_palette_argb(white, ground_once, 0), 0xff000000u);
    }

}

int main(void)
{
    original_is_exact();
    every_role_opaque();
    seeded_themes_have_no_black_role();
    derivation_is_deterministic();
    roles_stay_legible();
    palette_identity_and_alpha();
    palette_lightness_inversion();
    palette_white_polarity();
    palette_sat_q8_zero_is_identity();
    palette_stays_in_range();
    grid_plates_over_every_theme();
    browse_drag_marks_over_every_theme();
    mapping_twice_is_destructive();

    return t_done("theme");
}
