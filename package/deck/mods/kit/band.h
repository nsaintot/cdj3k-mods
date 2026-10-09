// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * kit/band.h - borrowing the strip under the waveform.
 *
 * The deck's four quick-menu panels (BEAT LOOP, KEY SHIFT, BEAT JUMP, GRID) all
 * share one rect inside the waveform view, and opening one shrinks the waveform
 * to free it. A mod borrows that rect the same way, by writing the app's
 * quick-menu mode register, so the app's own code lays out and rescales the
 * waveform.
 *
 * The band holds one panel (an app rule). STEMS and X-PAD both want it and a
 * stock panel may take it from either, so the arbitration lives in this kit.
 *
 * A client declares itself next to its own code, the way KIT_MOD does, and is
 * told when the band goes away, but not who took it.
 *
 * Everything here is [message]: the juce UI thread, which is also the only
 * repaint tick.
 */
#ifndef EP122_MOD_KIT_BAND_H
#define EP122_MOD_KIT_BAND_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


struct kit_band {
    const char *name;               /* for the log; tags nothing else */

    /* The quick-menu mode this client claims. It must be unique among clients
     * and past the app's own four: the mode register is the authoritative record
     * of who holds the band, and the watch reads it back to detect a takeover.
     *
     * setQuickMenuMode branches only on 0 (close) and 4 (the grid panel); every other
     * value is stored and opens the band with no stock panel attached to it. */
    int32_t mode;

    /* The band went to something else: a stock quick menu, another client, or an
     * unknown route. Put the row away but do not hand the band back; whoever
     * wrote that mode owns it now and releasing would close their panel. */
    void (*closed)(void);

    /* The setting that enables this client, or NULL if always shown. Used to
     * pack the slots: a disabled client holds none, the enabled ones close up
     * rightward and the track title gets the space back. */
    const int *shown;

    /* Put the quick-menu button at `x`. Called only when the set of shown
     * clients changes (a settings toggle). */
    void (*reslot)(int32_t x);

    /* Position among the shown clients: 0 is the slot nearest the app's own
     * three buttons. Explicit because link order is arbitrary. Must be unique. */
    unsigned char order;
};

/* One descriptor. `used` because only the section refers to it. */
#define KIT_BAND(sym, ...) \
    static const struct kit_band sym __attribute__((used, \
        section("ep122_band"))) = { __VA_ARGS__ }

extern const struct kit_band __start_ep122_band[] __attribute__((visibility("hidden")));
extern const struct kit_band __stop_ep122_band[] __attribute__((visibility("hidden")));

/* ---- install ------------------------------------------------------------- */

/* [init] Verify the mode setter resolved and hook the stock quick-menu buttons.
 * Returns 0 when the band can be borrowed. A failure is reported but not fatal:
 * kit_band_take falls back to moving components by hand. */
int kit_band_install(void);

/* [message] The waveform title bar drew. Idempotent and cheap after the first
 * call; every client's anchor calls it and the first one resolves the tree.
 * Returns 0 once the view and the rect are known. */
int kit_band_attach(uintptr_t touch_aria);

/* [message] Record the closed layout, so opening is a straight write of the
 * other one. Only valid while no panel is showing. */
void kit_band_snapshot(void);

/* [message] Mark a component as ours. The scans that find the app's panels
 * compare bounds, and a client's strip and button have the stock sizes, so they
 * are excluded by identity. Call once each for the strip and the button after
 * building them. */
void kit_band_own(uintptr_t comp);

/* ---- the tree, for building into ----------------------------------------- */

/* The title bar's own quick-menu buttons, as its ctor places them:
 * BEAT LOOP at x=898, KEY SHIFT at 1022, BEAT JUMP at 1146, each 114x90 on a
 * 124px stride. The app leaves one gap in front of them, at x=774.
 *
 * The second slot is taken from the track title. Its Label is {216, 10, 650, 40},
 * running to x=866, so a button one stride further left overlaps the text; the
 * title is cut back to end before the new slot (KIT_BAND_TITLE_W). This costs
 * 226px of track name, which ellipsizes. The alternative, a half-height button
 * in the corner, is hard to reach behind a retracted bezel.
 *
 * Slot positions are assigned here so two clients cannot collide. */
#define KIT_BAND_BTN_W      114     /* a STOCK button, which is how the scans find one */
#define KIT_BAND_BTN_H      90
#define KIT_BAND_SLOT_W     114
#define KIT_BAND_SLOT_H     90
#define KIT_BAND_SLOT_STRIDE 124    /* the app's own, between its three */
#define KIT_BAND_SLOT_X(n)  (774 - (n) * KIT_BAND_SLOT_STRIDE)
#define KIT_BAND_SLOT_Y     0

/* Where a client's button goes, given which clients are enabled: with one of
 * two features disabled, the other moves up. */
int32_t kit_band_slot_x(const struct kit_band *c);

/* [message] A client's gate moved. Re-places every shown client's button and
 * re-cuts the track title. */
void kit_band_slots_changed(void);

/* The track title's Label and its reduced width. Matched on the three bounds
 * that do not change so the resize cannot hit another child, and re-applied on
 * the tick because the app resets them on a track change. */
#define KIT_BAND_TITLE_X    216
#define KIT_BAND_TITLE_Y    10
#define KIT_BAND_TITLE_H    40
#define KIT_BAND_TITLE_W    (KIT_BAND_SLOT_X(1) - KIT_BAND_TITLE_X - 10)

uintptr_t       kit_band_bar(void);     /* the title bar: a quick-menu button's parent */
uintptr_t       kit_band_view(void);    /* the waveform view: the strip's parent       */
const int32_t  *kit_band_rect(void);    /* {x,y,w,h} the stock panels share            */

/* True while one of the app's own panels is up: it has already shrunk the
 * waveform, so ours must neither shrink it again nor restore it on close. */
int kit_band_stock_up(void);

/* ---- holding it ---------------------------------------------------------- */

/* Take the band. Any client already holding it is told first, so two strips
 * never draw into the same rect.
 *
 *   1  taken through the app's own mode register: it laid the waveform out
 *   0  the mode setter could not be reached, so the band is held by a by-hand
 *      shrink; usable, but the layout may not have rescaled
 *  -1  the app refused (it does with no track loaded). The caller must not
 *      open: there is no panel rect, and nothing needs to be given back. */
int  kit_band_take(const struct kit_band *c);

/* Hand it back, by whichever route it was taken. Safe to call when not held. */
void kit_band_give(const struct kit_band *c);

int  kit_band_holds(const struct kit_band *c);

/* [message] Per display tick, from any client's clock. Detects the band being
 * taken without a press on one of our buttons (e.g. GRID ADJUST, which opens on
 * a one-second rotary hold) and tells the holder. Also reconciles a band left
 * shrunk around a mode nobody owns. */
void kit_band_poll(void);


#ifdef __cplusplus
}
#endif

#endif /* EP122_MOD_KIT_BAND_H */
