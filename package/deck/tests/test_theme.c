// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * test_theme.c - the colour model (theme/palette.c) and the UI roles
 * (theme/roles.c) over the themes in theme/presets.c.
 */
#include "mods/theme/theme.h"
/* Declarations only, plus the grid panel's plate colours. These are deck values,
 * not roles, so the role checks do not cover them; see the note in
 * panel_internal.h and grid_plates_over_every_theme below. */
#include "mods/grid/panel_internal.h"
/* ...and the browse drag's two surfaces, for the same reason. */
#include "mods/browse/browse.h"

#include "test.h"

#define N_ROLES (sizeof(struct theme_ui) / sizeof(uint32_t))

/* The deck's own colours, transcribed from panel measurements, not from
 * roles.c: with ORIGINAL selected the mods must paint exactly these. */
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

    /* The id comes from the eMMC and is untrusted: out of range reads as ORIGINAL. */
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
    /* ORIGINAL (no palette) returns the input unchanged. */
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
    /* exempt_blue: the deck's accent keeps its lift as a fill and darkens as ink. */
    CHECK_U32(theme_palette_argb(white, 0xff007de1u, 1), 0xff1e9bffu);
    CHECK(theme_palette_argb(white, 0xff007de1u, 0) != 0xff1e9bffu);
}

/* theme.h promises a field left zero does nothing. For sat_q8 that is an
 * explicit rule: 0 is the identity, 1 is greyscale. */
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
 * Every colour in the strip, through every theme. These are not roles: they reach
 * the screen through mod_colour_stock (the setFill hook's transform, called
 * directly), so roles_stay_legible does not cover them.
 *
 * The polarity check is the important one: a plate left dark under a light theme
 * still passes the contrast checks. */
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

    /* ORIGINAL is not a transform, so a deck value must come back bit-exact:
     * these are the reference design's own greys. */
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

        /* The plate follows the ground. No other check here catches a light theme
         * that leaves it dark. */
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

        /* Lettering against its plate, in each of the three states. The disabled
         * pair is deliberately close (there is nothing to undo), so it has a lower
         * floor than the live states. */
        CHECK(distance(text, fill) >= 96);
        CHECK(distance(line_on, fill_on) >= 72);
        CHECK(distance(text_off, fill_off) >= 24);

        /* The border must read against the plate it outlines, and the group's
         * backdrop against the buttons on it; the backdrop is meant to be subtle,
         * hence its low floor. */
        CHECK(distance(line, fill) >= 32);
        CHECK(distance(line_off, fill_off) >= 8);
        CHECK(distance(group, fill) >= 8);
        CHECK(distance(cap, group) >= 48);

        /* The three states of one control must be distinguishable. */
        CHECK(fill != fill_on);
        CHECK(fill != fill_off);
        CHECK(fill_on != fill_off);
    }
    select_theme(0);
}

/* ---- the browse drag's two marks ------------------------------------------
 *
 * The hole shows where the track came from, the marker where it will land. Both
 * are deck values (the list's ground and its selected-row green), so both go
 * through mod_colour_stock and neither is a role. They are tested here because a
 * theme, not the drag, is what can break them: the transform must keep them apart
 * and keep the hole light on a light theme and dark on a dark one. */
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

        /* The hole is the ground colour: light theme, light hole. */
        t_checks++;
        if ((lightness(ground) > 128) != (k_mod_themes[t].light != 0))
            T_FAILED("%s: the drag hole is L=%d on a %s list",
                     k_mod_themes[t].name, lightness(ground),
                     k_mod_themes[t].light ? "light" : "dark");

        /* The marker must be visible on the hole. It is a solid 4px bar, so its
         * floor is lower than lettering needs. */
        CHECK(distance(marker, ground) >= 48);
    }
    select_theme(0);
}

/* Mapping a buffer twice is a defect.
 *
 * The image cache assumes a strip's pixels go through the palette exactly once;
 * nothing in the types enforces it. Routes to a second pass include an eviction that
 * frees the pristine copy but leaves the buffer mapped, a re-snapshot that takes our
 * own output as the original, and a source-themed strip mapped again on its way to
 * the screen.
 *
 * A dark palette's transform is near-idempotent, so a second pass changes almost
 * nothing; a light one's is destructive, and WHITE's is an exact involution that
 * restores the stock pixels. A double map is therefore only visible on light themes.
 *
 * This test pins that property, not any one code path. If a future palette were made
 * idempotent, a double map would no longer be visible, and the reasoning in
 * image_sync.c would need rewriting. */
static void mapping_twice_is_destructive(void)
{
    /* The detailed waveform's stock colour group (k_wave_stock in wave.c).
     * Entry 7 is the ground, 72% of what is on screen. */
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
            /* is_fill 0: these are ink and ground in a picture, as theme_map_pixel
             * treats every pixel. */
            uint32_t once  = theme_palette_argb(pal, k_stock[i], 0);
            uint32_t twice = theme_palette_argb(pal, once, 0);
            int c;

            for (c = 0; c < 24; c += 8) {
                int d = (int)((once >> c) & 0xffu) - (int)((twice >> c) & 0xffu);

                if (d < 0) d = -d;
                if (d > worst) worst = d;
            }
        }

        /* Checks magnitude, not inequality: every palette drifts a level or two under a
         * second pass. Across the group above, the dark themes' worst channel moves
         * 3, 7, 8 and 32, WHITE 255 and SANDSTONE 167. On the deck's dark default
         * a double map is unobservable.
         *
         * Only light themes are asserted. A dark theme drifting further is not a fault,
         * but a light theme that stopped showing a double map would remove the only
         * check that can be done by eye. */
        if (k_mod_themes[t].light && worst < 64)
            T_FAILED("%s is light yet a double map moves it only %d -- nothing would be "
                     "left to catch one by eye", k_mod_themes[t].name, worst);
    }

    /* WHITE exactly: peaks and ground are pure white and pure black, the inversion
     * swaps them, and a second pass restores the same bytes, so a doubly-mapped strip
     * looks untouched. */
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
