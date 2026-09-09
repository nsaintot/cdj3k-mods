// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * draw.h - the mod's shared drawing kit: juce::Graphics primitives, colour maths,
 *          and the deck's own button surface.
 *
 * Everything here takes a juce::Graphics* exactly as the app hands it to a paint
 * override, so any mod that owns a paint slot can use it: the stem row, the MOD
 * SETTINGS overlay, anything added later.
 *
 * ---- the stipple ----
 *
 * Every touchable control on this deck is a 4x4 checkerboard of two colours, 50/50,
 * with no antialiasing -- measured off the running deck, not inferred:
 *
 *     quick-menu button, unlit    #323232 / #232323     x0.70
 *     quick-menu button, lit      #007de1 / #0064a5     x0.73
 *     panel pad (BEAT LOOP)       #323232 / #191919     x0.50
 *
 * In every pair the dark half is a uniform scale of the light one -- so the hue is
 * shared and only the value differs -- but the FACTOR is authored per element. A
 * caller that knows its pair passes both (mod_checker_pair); a caller that just wants
 * a button passes the light colour and gets the quick-menu factor, which reproduces
 * stock's #232323 exactly.
 *
 * This is what "the button is touchable" means visually on this deck, so drawing a
 * flat plate anywhere is now a deliberate choice rather than a default.
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
 * that is drawing a whole control rather than dressing a juce::Label -- a Label
 * would bring its own component, which would then have to be kept out of the
 * mouse's way over somebody else's button. Justification is juce's flag word;
 * JUCE_JUSTIFY_* in juce.h names the ones worth having. */
void mod_gfx_text(void *g, const char *text, float font_h, uint32_t argb,
                  int x, int y, int w, int h, int justification);

/* ---- "these pixels are ours" ----
 *
 * The theme's setFill hook re-colours every fill the app performs, and our drawing goes
 * through the same choke point -- juce::Graphics::setColour IS setFill. But our colours
 * arrived from mod_ui(), already resolved through the theme once, so a second pass is
 * the transform applied twice. These say "leave this one alone".
 *
 * The contract that makes it safe: a bracket may only ever span ONE synchronous call
 * into juce. mod_gfx_colour does exactly that internally, and the only other legitimate
 * use is around a chain to a stock paint whose colours we set ourselves (Label::paint
 * draws its background and lettering from colours we gave it). Never hold it across
 * anything that can return to the app with it still set -- there is no path back.
 *
 * Not reentrant and does not need to be: painting is the message thread's alone. */
void mod_draw_enter(void);
void mod_draw_leave(void);
int  mod_drawing(void);

/* ================================================================== */
/* The UI roles -- what OUR OWN controls are painted with             */
/* ================================================================== */

/*
 * The theme's palette re-colours what the APP draws. This is the other half: the colours
 * the mod's own controls choose for themselves. It lives here because every mod that owns
 * a paint slot asks for one, and none of them has any other business with the theme layer;
 * theme/roles.c is what answers.
 *
 * Why they cannot just be the transform. Our fills go through juce::Graphics::setColour,
 * which is LowLevelGraphicsContext::setFill, which is the slot theme.c hooks -- so the
 * mod's UI was already being re-coloured, by a pass that knows nothing about it. That
 * pass cannot know:
 *
 *   - that stem[0..2] must stay DISTINGUISHABLE FROM EACH OTHER. A duotone collapses
 *     hues toward one ramp, and the moment two stems converge the colour stops naming
 *     which stem it is -- in the wedge, on the caption, and on the bypass icon at once.
 *   - that the checker's two halves are one surface at a fixed ratio, not two colours
 *     that may drift apart.
 *   - that `exempt_blue` was tuned for the deck's selection rows, not for our accent.
 *
 * So a control asks for a ROLE and gets a colour that is already right for the theme in
 * force, and the generic pass is told to leave our pixels alone (mod_draw_enter). One
 * transform, in the place that knows what the colour is FOR.
 *
 * A theme that authors none of this gets every role derived by running ORIGINAL's
 * through its own palette -- so a new theme is coherent with the rest of the UI from
 * the moment it exists, and only has to author the roles where the generic transform
 * gets it wrong. Which, in practice, means the stems.
 */
struct theme_ui {
    uint32_t surface;         /* unlit plate, wedge-off grey                   */
    /* The unlit plate's OTHER checker half, and the reason it is a role rather than
     * something the draw kit derives: the deck's plate is two authored greys (#323232 and
     * #232323, 15 levels apart) and BOTH go through the duotone, which compresses them --
     * on a strongly tinted theme the deck's pair closes to 11 levels while a fixed ratio
     * keeps ours at 19, and our button reads as a coarser texture than the three beside
     * it. Deriving this one from the palette instead reproduces the deck exactly.
     *
     * Only the UNLIT plate needs it. Grey is all the duotone touches; the lit and refusing
     * plates are chromatic, so they take the hue mapping, which holds lightness -- their
     * derived partner already lands within a few levels of the deck's. */
    uint32_t surface2;
    uint32_t edge;            /* button border                                 */
    uint32_t accent;          /* lit plate                                     */
    /* The lit plate's OTHER checker half, and a role for the same reason surface2 is:
     * the deck's two quick-menu pairs sit at DIFFERENT ratios -- unlit #323232/#232323
     * at 0.70, lit #007de1/#0064a5 at 0.73 -- so one derived partner cannot serve both.
     * Deriving the lit half at the unlit ratio put ours at #005b9e against the deck's
     * #0064a5 on the button beside it. */
    uint32_t accent2;
    /* A lit MODE plate, and deliberately not the accent: on this deck blue means "on,
     * selected, working", which a mode that changes what a gesture DOES is not. The
     * deck's own word for that is the yellow on the source badge -- the one chip on
     * screen saying which world you are in rather than what is switched on -- and this
     * is that colour, sampled off it. */
    uint32_t mode;
    uint32_t bypass;          /* the latched-override amber                    */
    /* The X-PAD's live value: its fill, its lit brick name, and its two state
     * flags. A role of its own rather than the bypass amber it started on,
     * because the pad is not the deck's colour language -- it is the RMX-1000's,
     * and this is that unit's own red. It is the loudest mark the strip can make
     * and it is meant to be. */
    uint32_t xpad;
    /* The X-PAD's two state flags when they are lit. Green, and NOT the accent:
     * blue on this deck means "on, selected", which is what every other plate in
     * the band already says, and HOLD and OVERDUB have to read as a different
     * kind of statement -- something the DJ armed rather than something that is
     * merely switched on. Under a palette it follows the one green a palette
     * authors, which is the vocals stem's. */
    uint32_t xpad_on;
    uint32_t stem[3];         /* DRUMS / HARMONICS / VOCALS -- keep them apart */
    uint32_t text;
    /* Lettering on a surface the DECK owns -- a quick-menu button in its band, a row in
     * its settings list. The deck's own white through the palette, never the seed's ink:
     * measured on the lit quick-menu button, ours #c9d1d9 against the deck's #c9f9ff on
     * NEON and #4e5681 against #202540 on SANDSTONE, on the button beside it. `text` is
     * still the right one for a panel of OURS, which is where a theme's ink belongs. */
    uint32_t text_deck;
    uint32_t text_dim;
    /* The VALUE column of a row in the deck's own settings list, which is a different
     * grey from text_dim and has to be: the MOD SETTINGS rows sit inside DJ SETTING with
     * the deck's own rows above and below them, so their secondary column is the deck's
     * or ours read as a different weight of type. Measured: the list's value is #7d7d7d
     * where text_dim -- the skin grey, off the Ver label -- is #afafaf, and every other
     * user of text_dim is a control of OURS where the skin grey is the right one.
     *
     * Derived from the deck's, never authored, for the same reason the plate is. */
    uint32_t text_value;
    uint32_t text_off;        /* disabled lettering, greyed stems              */
    uint32_t text_on_accent;  /* near-black: white does not hold on a light fill */
    /* Lettering on the ACCENT plate specifically. text_on_accent is not that, despite
     * its name: every user puts it on one of the deck's BRIGHT chromatic fills (the
     * BYPASS amber, the edit yellow, a stem colour), where the deck's own answer is
     * near-black. The accent is the deck's blue, and on the deck's own selected row
     * the lettering on it is white. The two fills have opposite polarity on ORIGINAL,
     * so one role cannot serve both.
     *
     * Follows the fill's polarity, which is what the deck's own badges do under a
     * theme: on WHITE the +-10 plate darkens and its black lettering turns white, so
     * whichever of the theme's ink and ground is further from the accent is the one
     * that goes on it. */
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

/* The roles for the theme in force. Never NULL. Cheap enough for a paint: the derived
 * set is built once per theme change and handed back by pointer after that. */
const struct theme_ui *mod_ui(void);

/* A counter that moves when the roles do.
 *
 * A colour we FILL with is read out of mod_ui() at paint time, so it follows the theme
 * on its own. A colour we STORE on a component does not: juce::Label keeps its lettering
 * colour and paints from that, so a label built under one theme goes on wearing its ink
 * under the next. Measured: the X-PAD button's word stayed SANDSTONE's near-black navy
 * after a switch to ORIGINAL, invisible on the dark plate, while the four buttons beside
 * it were white -- and its own PLATE followed the theme correctly, because that is a
 * fill.
 *
 * So anything holding a stored colour compares this against its own last value and puts
 * the colour back when it moves. [any] */
unsigned mod_ui_gen(void);

/* THE SAME ROLES, UNTRANSFORMED -- ORIGINAL's own values.
 *
 * For a colour STORED on a component that JUCE paints rather than we do. The theme is a
 * hook on setFill, so every fill juce makes goes through the palette exactly once: a
 * stored value is therefore in ORIGINAL's space, the same as any literal we hand it, and
 * the hook resolves it on the way to the screen. Store a mod_ui() colour there instead
 * and the palette is applied TWICE.
 *
 * That is invisible on a duotone -- two passes leave a colour the wrong shade of the
 * right thing -- and fatal on an inversion, where two passes are the identity. Measured:
 * the stem row's progress caption on WHITE, stored as mod_ui()->text, came back #ffffff
 * on the row's own #ffffff plate, 680 pixels of it with no antialiasing anywhere. The
 * plate was right because it was stored as a literal.
 *
 * A stored stock colour also needs no restamping when the theme moves: the hook does the
 * work at paint time. mod_ui() is still the right answer for a colour WE fill with, and
 * for one stored on a component whose paint is ours and brackets mod_draw_enter(). */
const struct theme_ui *mod_ui_stock(void);

/* ONE STOCK COLOUR, resolved the way the setFill hook would have resolved it.
 *
 * For a colour of ours that is a DECK VALUE rather than a role. The grid panel's
 * plates are the reference design's own greys, chosen to sit beside the deck's five
 * stock buttons in the same strip; those five are ARGB sprites and the theme maps
 * them per pixel, so the right answer for ours is the same transform on the same
 * greys. A role would answer a question nobody is asking here -- there is no hue to
 * hold apart, no checker ratio, no accent.
 *
 * It has to be asked for, because mod_gfx_colour brackets internally: every colour
 * handed to the draw kit is declared already-resolved and never reaches the hook.
 * This is how a caller that WANTS the pass says so.
 *
 * Returns the input unchanged under ORIGINAL, which is the whole point -- a stock
 * value stays exactly stock, with no role to keep in step with the measurements.
 *
 * NOT a substitute for mod_ui(). Use a role wherever one exists: the three things
 * the transform cannot know are listed at the top of the roles section, and a colour
 * subject to any of them has to be authored rather than derived. [any] */
uint32_t mod_colour_stock(uint32_t argb);

/* ================================================================== */
/* Colour                                                             */
/* ================================================================== */

/* Both keep the alpha and move all three channels together, so the hue is exact and
 * only the value changes -- which is what makes a result read as the SAME colour in a
 * different light rather than as a different colour. Q8: 256 == 1.0, rounded. */
uint32_t mod_colour_scale(uint32_t argb, uint32_t q8);   /* darker: c * q8            */
uint32_t mod_colour_lift(uint32_t argb, uint32_t q8);    /* brighter: c + headroom*q8 */

/* ================================================================== */
/* The button surface                                                 */
/* ================================================================== */

/* A sanity ceiling on any rect this kit is asked to fill. Component bounds are read out
 * of the app, so they are input; see mod_checker_pair for what an unchecked one costs. */
#define MOD_DRAW_MAX         2048

#define MOD_CHECKER_CELL     4
/* The checker's SECOND half, derived from the surface -- and both how far it moves and
 * WHICH WAY depend on the ground, which is why mod_draw_ground exists.
 *
 * On stock's dark grey the two halves sit 15 levels apart and the second is the darker:
 * 179/256 reproduces that exactly (#323232 -> #232323).
 *
 * On a light ground it moves the OTHER WAY, and that is not a matter of taste -- it is
 * what the deck's own buttons do. The stock pair is #323232/#232323 with the surface role
 * taken from the lighter of the two, so a lightness inversion lands them at #cdcdcd and
 * #dcdcdc: the partner ends up ABOVE the surface. Deriving it downward instead put our
 * quick-menu plate at 205/191 against the deck's 220/205, which reads as our button
 * sitting a shade darker than the three next to it. A lift of 77/256 puts 205 at exactly
 * 220 -- the same 15 levels, on the correct side.
 *
 * The magnitude has to be stated per ground for a second reason: a RATIO preserves
 * relative difference and the eye reads the absolute one, so reusing 179 on #cdcdcd gives
 * #8f8f8f and the plate stops being one surface. */
#define MOD_CHECKER_ALT_Q8         179   /* dark ground:  scale DOWN, 15 levels */
#define MOD_CHECKER_ALT_LIGHT_Q8    77   /* light ground: lift UP,    15 levels */

/* Which way the ground goes. Set from the theme when the selection changes; the draw kit
 * stays independent of the theme layer and is simply told. */
void mod_draw_ground(int light);
/* "A finger is on it": the surface lifts toward white while the gesture is live.
 *
 * MEASURED off a stock button held down, not chosen -- 59/256 solves both halves of the
 * grey pair (0x32->0x61, 0x23->0x55) to within a level, and the same fraction then
 * predicts the lit blue's two halves and both bar greys. Every one lands inside 1/255,
 * which is as close as an integer pipeline gets to whatever float the app uses. */
#define MOD_CHECKER_HOT_Q8   59

/* Fill a rect with the deck's button surface. Phase is anchored to the rect's own
 * top-left, which is what makes it look native: a pattern phased to the screen would
 * land differently on every button and read as noise rather than as a surface.
 *
 * Cost is one fill per dark cell -- ~340 on a 114x90 quick-menu button, ~104 on a
 * 64x52 one, against 126 for the entire three-wedge fader row. Fine for anything that
 * repaints on user action rather than per frame. The stock route is a tiled 8x8
 * juce::Image through a FillType (one call, same pixels, and one cached recolour
 * instead of 340 under a theme); swapping to it is local to draw.c. */
void mod_checker_pair(void *g, int x, int y, int w, int h,
                      uint32_t light, uint32_t dark);
void mod_checker(void *g, int x, int y, int w, int h, uint32_t light);

/* The same surface, lifted by q8 (0 = at rest, MOD_CHECKER_HOT_Q8 = touched).
 *
 * The ORDER is the whole point and it is not the obvious one: the dark half is derived
 * from the RESTING light colour and only then are BOTH halves lifted. Lifting the light
 * one first and deriving the dark from that is what a reading of "lighten the colour"
 * gets you, and it comes out visibly wrong -- it darkens relative to a plate that is now
 * brighter, so the stipple gains contrast exactly when it should be losing it. Measured
 * against a held stock button: the dark half should be #555555 and that route gives
 * #474747, fourteen levels out.
 *
 * Put another way, the lift is applied to the SURFACE, not to the colour the surface is
 * generated from. */
void mod_checker_lift(void *g, int x, int y, int w, int h,
                      uint32_t light, uint32_t q8);

/* The same, for a caller that HAS the other half rather than deriving one.
 *
 * A derived partner is a fixed distance from the surface, and the deck's is not: its two
 * greys both go through the theme's duotone, which compresses them, so on a strongly
 * tinted theme the deck's pair closes up while ours stays put and our plate reads as the
 * coarser texture. theme_ui::surface2 is that half taken from the palette instead; see
 * the note beside it. mod_checker_alt is the derivation, exposed for the states that have
 * no authored partner and do not need one (the lit and refusing plates are chromatic,
 * which the duotone does not touch). */
void     mod_checker_lift2(void *g, int x, int y, int w, int h,
                           uint32_t base, uint32_t alt, uint32_t q8);
uint32_t mod_checker_alt(uint32_t base);

/* A plate, with the RIGHT partner for it: the measured one when the theme states it
 * (surface2 for an unlit plate, accent2 for a lit one) and the derivation otherwise.
 * What a quick-menu button should call, so a caller does not have to know which of its
 * plates has an authored second half. */
void     mod_checker_plate(void *g, int x, int y, int w, int h,
                           uint32_t base, uint32_t q8);

/* ---- the refusal blink ----
 *
 * How a button in the title bar answers a press it will not honour: the PLATE flashes
 * theme_ui.refuse three times and the press does nothing. Shared, because it is one
 * gesture with one meaning -- "not now, and that is why nothing moved" -- and two
 * buttons giving it at two different speeds would read as two different messages.
 *
 * Counted in DISPLAY TICKS, delivered by the app's own refresh timer, so any mod that
 * hooks that slot gets the same duration for the same number. The bar tracks the
 * button's real state throughout: a refusal is an answer to a press, not a state the
 * button is in.
 *
 * ONE INT CARRIES BUDGET AND PHASE. It counts down a tick at a time and the half-cycle
 * it lands in decides the colour, so there is no separate phase to fall out of step
 * with it. The press paints the first flash itself, which is why the count starts on a
 * loud half; zero is at rest, and the terminal tick is cold whatever the parity works
 * out to, because the resting colour has to be the one an ordinary repaint draws. */
#define MOD_BLINK_PERIOD     12    /* ticks per half-cycle: about a quarter second */
#define MOD_BLINK_FLASHES    3
#define MOD_BLINK_TICKS      (MOD_BLINK_FLASHES * 2 * MOD_BLINK_PERIOD)

/* ---- the title-bar button's bottom bar ----
 *
 * Measured: 56x3, flush with the button's bottom edge and centred across its 114px
 * width, over the stipple. It carries state like the plate does -- #7d7d7d while the
 * button is unlit, #afafaf while it is lit.
 *
 * A separate call rather than part of mod_checker, because it is not part of the
 * surface: it marks a TITLE-BAR button specifically. The panel pads below wear the
 * same stipple and no bar (an orange border instead), so a control that gets one is
 * saying which row it belongs to. */
#define MOD_BTN_BAR_W        56
#define MOD_BTN_BAR_H        3
#define MOD_COL_BTN_BAR      0xff7d7d7du   /* unlit */
#define MOD_COL_BTN_BAR_ON   0xffafafafu   /* lit -- the skin grey, as on the Ver label */

/* Centred at the bottom of the given rect. Refuses a rect too small to hold it rather
 * than shrinking to fit: the bar is a fixed mark at a fixed size in the stock design,
 * and a scaled one would read as a progress indicator instead. */
void mod_btn_bar(void *g, int x, int y, int w, int h, uint32_t col);


#ifdef __cplusplus
}
#endif

#endif /* EP122_MOD_DRAW_H */
