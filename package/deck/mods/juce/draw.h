// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * draw.h - the mod's shared drawing kit: juce::Graphics primitives, colour maths,
 *          and the deck's own button surface.
 *
 * Everything here takes a juce::Graphics* as the app passes it to a paint
 * override, so any mod that owns a paint slot can use it (the stem row, the MOD
 * SETTINGS overlay).
 *
 * ---- the stipple ----
 *
 * Every touchable control on this deck is a 4x4 checkerboard of two colours, 50/50,
 * with no antialiasing:
 *
 *     quick-menu button, unlit    #323232 / #232323     x0.70
 *     quick-menu button, lit      #007de1 / #0064a5     x0.73
 *     panel pad (BEAT LOOP)       #323232 / #191919     x0.50
 *
 * In every pair the dark half is a uniform scale of the light one (same hue,
 * different value), but the factor is set per element. A caller that knows its
 * pair passes both (mod_checker_pair); otherwise it passes the light colour and
 * gets the quick-menu factor, which reproduces stock's #232323 exactly.
 *
 * The stipple marks a control as touchable; a flat plate should be a deliberate
 * choice.
 */
#ifndef EP122_MOD_DRAW_H
#define EP122_MOD_DRAW_H

#include "core/mod_core.h"

#ifdef __cplusplus
extern "C" {
#endif


/* juce::Graphics, as the paint overrides receive it. */
#define MOD_FN_GFX_SETCOLOUR  ep122_sym(EP122_JUCE_GFX_SETCOLOUR)     /* setColour(const Colour&)          */
#define MOD_FN_GFX_FILLRECT   ep122_sym(EP122_JUCE_GFX_FILLRECT_INT)  /* fillRect(int x,int y,int w,int h)  */

#define MOD_FN_GFX_SETFONT    ep122_sym(EP122_JUCE_GFX_SETFONT)       /* setFont(const Font&)              */
#define MOD_FN_GFX_DRAWTEXT   ep122_sym(EP122_JUCE_GFX_DRAWTEXT)      /* drawText(String, x,y,w,h, just, e) */

void mod_gfx_colour(void *g, uint32_t argb);
void mod_gfx_fill(void *g, int x, int y, int w, int h);

/* One line of text into a rect, in the deck's own face. For a paint override
 * that draws a whole control; a juce::Label would add its own component, which
 * would then have to be kept from intercepting the mouse. Justification is
 * juce's flag word; JUCE_JUSTIFY_* in juce.h names the useful ones. */
void mod_gfx_text(void *g, const char *text, float font_h, uint32_t argb,
                  int x, int y, int w, int h, int justification);

/* ---- "these pixels are ours" ----
 *
 * The theme's setFill hook re-colours every fill the app performs, and our drawing
 * goes through it too (juce::Graphics::setColour is setFill). Our colours come from
 * mod_ui(), already resolved through the theme, so these brackets tell the hook to
 * skip them.
 *
 * A bracket may span only one synchronous call into juce. mod_gfx_colour does that
 * internally; the only other valid use is around a chain to a stock paint whose
 * colours we set (Label::paint draws its background and lettering from colours we
 * gave it). Never hold it across anything that can return to the app with it still
 * set; nothing would clear it.
 *
 * Not reentrant; painting happens only on the message thread. */
void mod_draw_enter(void);
void mod_draw_leave(void);
int  mod_drawing(void);

/* ================================================================== */
/* The UI roles -- what the mod's own controls are painted with      */
/* ================================================================== */

/*
 * The theme's palette re-colours what the app draws; these are the colours the mod's
 * own controls use. They are declared here because every mod that owns a paint slot
 * needs them and has no other use for the theme layer; theme/roles.c implements them.
 *
 * Our fills go through juce::Graphics::setColour, which is
 * LowLevelGraphicsContext::setFill, the slot theme/theme.c hooks. That generic pass
 * cannot know:
 *
 *   - that stem[0..2] must stay distinguishable from each other. A duotone collapses
 *     hues toward one ramp, and two converged stems can no longer be told apart in
 *     the wedge, the caption or the bypass icon.
 *   - that the checker's two halves are one surface at a fixed ratio.
 *   - that `exempt_blue` was tuned for the deck's selection rows, not for our accent.
 *
 * So a control asks for a role and gets a colour already resolved for the current
 * theme, and the generic pass skips our pixels (mod_draw_enter).
 *
 * A theme that authors no roles gets each one derived by running ORIGINAL's through
 * its palette, and only needs to author the roles the generic transform gets wrong
 * (in practice, the stems).
 */
struct theme_ui {
    uint32_t surface;         /* unlit plate, wedge-off grey                   */
    /* The unlit plate's other checker half. A role, not derived by the draw kit: the
     * deck's plate is two authored greys (#323232 and #232323, 15 levels apart) and
     * both go through the duotone, which compresses them. On a strongly tinted theme
     * the deck's pair closes to 11 levels while a fixed ratio keeps ours at 19, a
     * visibly coarser texture. Deriving it from the palette matches the deck exactly.
     *
     * Only the unlit plate needs it: the duotone touches only grey. The lit and
     * refusing plates are chromatic and take the hue mapping, which holds lightness,
     * so their derived partner lands within a few levels of the deck's. */
    uint32_t surface2;
    uint32_t edge;            /* button border                                 */
    uint32_t accent;          /* lit plate                                     */
    /* The lit plate's other checker half. A role because the deck's two quick-menu
     * pairs use different ratios (unlit #323232/#232323 at 0.70, lit #007de1/#0064a5
     * at 0.73); the unlit ratio gives #005b9e instead of the deck's #0064a5. */
    uint32_t accent2;
    /* A lit mode plate (a mode changes what a gesture does). Not the accent, which
     * on this deck means "on, selected, working". Sampled from the yellow source
     * badge, which the deck uses to show the current mode. */
    uint32_t mode;
    uint32_t bypass;          /* the latched-override amber                    */
    /* The X-PAD's live value: its fill, its lit brick name, and its two state
     * flags. The RMX-1000's red, the loudest mark in the strip. */
    uint32_t xpad;
    /* The X-PAD's two state flags when lit. Green, not the accent: blue means "on,
     * selected" on every other plate in the band, while HOLD and OVERDUB mean the DJ
     * armed something. Under a palette it follows the palette's one green, the
     * vocals stem's. */
    uint32_t xpad_on;
    uint32_t stem[3];         /* DRUMS / HARMONICS / VOCALS -- keep them apart */
    uint32_t text;
    /* Lettering on a surface the deck owns (a quick-menu button in its band, a row in
     * its settings list): the deck's white through the palette, not the seed's ink.
     * On the lit quick-menu button the ink gives #c9d1d9 where the deck shows #c9f9ff
     * on NEON, and #4e5681 where it shows #202540 on SANDSTONE. Use `text` on our
     * own panels. */
    uint32_t text_deck;
    uint32_t text_dim;
    /* The value column of a row in the deck's settings list. The MOD SETTINGS rows
     * sit inside DJ SETTING among the deck's own rows, so they must match its grey:
     * #7d7d7d, against text_dim's #afafaf (the skin grey, from the Ver label), which
     * is right for our own controls.
     *
     * Derived from the deck's colour, never authored, like the plate. */
    uint32_t text_value;
    uint32_t text_off;        /* disabled lettering, greyed stems              */
    uint32_t text_on_accent;  /* near-black: white does not hold on a light fill */
    /* Lettering on the accent plate. Despite its name, text_on_accent is used on the
     * deck's bright chromatic fills (the BYPASS amber, the edit yellow, a stem
     * colour), where the deck uses near-black. The accent is the deck's blue, which
     * the deck letters in white on its selected row. The two have opposite polarity
     * on ORIGINAL, so they need separate roles.
     *
     * Follows the fill's polarity, as the deck's badges do under a theme (on WHITE
     * the +-10 plate darkens and its black lettering turns white): whichever of the
     * theme's ink and ground is further from the accent is used. */
    uint32_t text_lit;
    uint32_t dead;            /* "nothing here yet", darker than disabled      */
    uint32_t icon_disabled;
    uint32_t track;           /* progress rail, unfilled                       */
    uint32_t tick;            /* wedge scale mark                              */
    uint32_t mark;            /* progress handover cut                         */
    uint32_t warn;
    uint32_t refuse;
    uint32_t bar;             /* title-bar button's bottom bar, unlit          */
    uint32_t bar_on;          /* ...and lit                                    */
};

/* The roles for the current theme. Never NULL. Cheap enough for a paint: the derived
 * set is built once per theme change and returned by pointer after that. */
const struct theme_ui *mod_ui(void);

/* A counter that changes when the roles do.
 *
 * A fill colour is read from mod_ui() at paint time and follows the theme. A colour
 * stored on a component does not: juce::Label paints from its stored lettering colour
 * (e.g. the X-PAD button's word stayed SANDSTONE's near-black navy after a switch to
 * ORIGINAL, invisible on the dark plate). Anything holding a stored colour compares
 * this against its last value and re-applies the colour when it changes. [any] */
unsigned mod_ui_gen(void);

/* The same roles, untransformed: ORIGINAL's values.
 *
 * For a colour stored on a component that JUCE paints. Every juce fill goes through
 * the setFill hook once, so a stored value must be in ORIGINAL's space; a mod_ui()
 * colour there gets the palette applied twice. On a duotone that gives a slightly
 * wrong shade; on an inversion two passes cancel (the stem row's progress caption
 * stored as mod_ui()->text rendered #ffffff on its #ffffff plate under WHITE).
 *
 * A stored stock colour needs no re-applying when the theme changes. Use mod_ui() for
 * colours we fill with, and for colours stored on a component whose paint is ours
 * and brackets mod_draw_enter(). */
const struct theme_ui *mod_ui_stock(void);

/* One stock colour, resolved as the setFill hook would resolve it.
 *
 * For a colour that is a deck value rather than a role, e.g. the grid panel's plates,
 * which use the reference design's greys beside the deck's five stock buttons. Those
 * buttons are ARGB sprites the theme maps per pixel, so ours take the same transform.
 *
 * Needed because mod_gfx_colour brackets internally, so draw-kit colours never reach
 * the hook. Returns the input unchanged under ORIGINAL.
 *
 * Not a substitute for mod_ui(): use a role wherever one exists. A colour subject to
 * any of the three limits listed at the top of the roles section must be authored,
 * not derived. [any] */
uint32_t mod_colour_stock(uint32_t argb);

/* ================================================================== */
/* Colour                                                             */
/* ================================================================== */

/* Both keep the alpha and move all three channels together, so the hue is kept and
 * only the value changes. Q8: 256 == 1.0, rounded. */
uint32_t mod_colour_scale(uint32_t argb, uint32_t q8);   /* darker: c * q8            */
uint32_t mod_colour_lift(uint32_t argb, uint32_t q8);    /* brighter: c + headroom*q8 */

/* ================================================================== */
/* The button surface                                                 */
/* ================================================================== */

/* Sanity cap on any rect this kit fills. Component bounds are read from the app, so
 * they are untrusted input; see mod_checker_pair for the per-cell cost. */
#define MOD_DRAW_MAX         2048

#define MOD_CHECKER_CELL     4
/* The checker's second half, derived from the surface. Both the distance and the
 * direction depend on the ground (set by mod_draw_ground).
 *
 * On stock's dark grey the halves are 15 levels apart and the second is darker:
 * 179/256 reproduces that exactly (#323232 -> #232323).
 *
 * On a light ground it goes the other way, as the deck's own buttons do: a lightness
 * inversion of #323232/#232323 gives #cdcdcd/#dcdcdc, with the partner above the
 * surface. A lift of 77/256 puts 205 at exactly 220, the same 15 levels (deriving
 * downward gave 205/191 against the deck's 220/205).
 *
 * The magnitude is set per ground because a ratio preserves relative difference while
 * the eye reads the absolute one: 179 on #cdcdcd gives #8f8f8f. */
#define MOD_CHECKER_ALT_Q8         179   /* dark ground:  scale DOWN, 15 levels */
#define MOD_CHECKER_ALT_LIGHT_Q8    77   /* light ground: lift UP,    15 levels */

/* Whether the ground is light. Set by the theme layer when the selection changes, so
 * the draw kit does not depend on it. */
void mod_draw_ground(int light);
/* Pressed state: the surface lifts toward white while the gesture is live.
 *
 * 59/256 matches a held stock button: both halves of the grey pair (0x32->0x61,
 * 0x23->0x55) to within a level, and also the lit blue's two halves and both bar
 * greys, all within 1/255. */
#define MOD_CHECKER_HOT_Q8   59

/* Fill a rect with the deck's button surface. Phase is anchored to the rect's own
 * top-left, as on stock buttons.
 *
 * Cost is one fill per dark cell: ~340 on a 114x90 quick-menu button, ~104 on a
 * 64x52 one, 126 for the whole three-wedge fader row. Fine for repaints on user
 * action, not per frame. The stock route is a tiled 8x8 juce::Image through a
 * FillType (one call, same pixels, one cached recolour under a theme); switching to
 * it would only touch draw.cc. */
void mod_checker_pair(void *g, int x, int y, int w, int h,
                      uint32_t light, uint32_t dark);
void mod_checker(void *g, int x, int y, int w, int h, uint32_t light);

/* The same surface, lifted by q8 (0 = at rest, MOD_CHECKER_HOT_Q8 = touched).
 *
 * The dark half is derived from the resting light colour, then both halves are
 * lifted. Lifting first and deriving after gives too much contrast: against a held
 * stock button the dark half should be #555555, that order gives #474747. */
void mod_checker_lift(void *g, int x, int y, int w, int h,
                      uint32_t light, uint32_t q8);

/* The same, for a caller that has the other half instead of deriving it.
 *
 * A derived partner sits a fixed distance from the surface; the deck's does not,
 * because the duotone compresses both its greys (see theme_ui::surface2).
 * mod_checker_alt is the derivation, for states with no authored partner (the lit
 * and refusing plates are chromatic, which the duotone does not touch). */
void     mod_checker_lift2(void *g, int x, int y, int w, int h,
                           uint32_t base, uint32_t alt, uint32_t q8);
uint32_t mod_checker_alt(uint32_t base);

/* A plate with its partner: the theme's (surface2 for an unlit plate, accent2 for a
 * lit one) when defined, the derivation otherwise. Quick-menu buttons call this. */
void     mod_checker_plate(void *g, int x, int y, int w, int h,
                           uint32_t base, uint32_t q8);

/* ---- the refusal blink ----
 *
 * A title-bar button refusing a press flashes its plate theme_ui.refuse three times
 * and does nothing. Shared so every button blinks at the same speed.
 *
 * Counted in display ticks from the app's refresh timer, so any mod hooking that slot
 * gets the same duration. The bar keeps showing the button's real state throughout.
 *
 * One int holds both the remaining budget and the phase: it counts down a tick at a
 * time and its half-cycle picks the colour. The press paints the first flash, so the
 * count starts on a lit half; zero is at rest, and the last tick is always the resting
 * colour so it matches an ordinary repaint. */
#define MOD_BLINK_PERIOD     12    /* ticks per half-cycle: about a quarter second */
#define MOD_BLINK_FLASHES    3
#define MOD_BLINK_TICKS      (MOD_BLINK_FLASHES * 2 * MOD_BLINK_PERIOD)

/* ---- the title-bar button's bottom bar ----
 *
 * 56x3, flush with the button's bottom edge and centred across its 114px
 * width, over the stipple. #7d7d7d while the button is unlit, #afafaf while lit.
 *
 * Separate from mod_checker because only title-bar buttons have it; the panel pads
 * below use the same stipple with an orange border and no bar. */
#define MOD_BTN_BAR_W        56
#define MOD_BTN_BAR_H        3
#define MOD_COL_BTN_BAR      0xff7d7d7du   /* unlit */
#define MOD_COL_BTN_BAR_ON   0xffafafafu   /* lit -- the skin grey, as on the Ver label */

/* Centred at the bottom of the given rect. Draws nothing in a rect too small to hold
 * it; the stock bar has a fixed size and a scaled one would look like a progress bar. */
void mod_btn_bar(void *g, int x, int y, int w, int h, uint32_t col);


#ifdef __cplusplus
}
#endif

#endif /* EP122_MOD_DRAW_H */
