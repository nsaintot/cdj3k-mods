// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * cue/cue.h - the hot-cue interception layer and the behaviour interface.
 *
 * pad.c is the only code that hooks the deck's hot-cue path: the pad's press
 * and release, PLAY, the CUE button and the preview needle. It decodes each into
 * an event and passes it to the behaviours, so no behaviour touches the ABI.
 *
 * A behaviour is a `struct cue_handler` in the ep122_cue section, declared next
 * to its code with CUE_HANDLER below. Adding one needs only a new file; there is
 * no central table. Handlers run in (prio, name) order, as in the mod registry.
 *
 * Everything here runs on the deck thread ([deck]) except the preview needle,
 * which is written from [message] and read from [deck]. pad.c reads it
 * atomically and hands behaviours a snapshot.
 */
#ifndef EP122_MOD_CUE_H
#define EP122_MOD_CUE_H

#include "core/mod_core.h"

#ifdef __cplusplus
extern "C" {
#endif


/* ================================================================== */
/* The event                                                          */
/* ================================================================== */

enum cue_phase {
    /* Before the deck's own press run. The pad is identified but nothing has
     * happened yet; a behaviour arms here anything it needs before other events
     * arrive. */
    CUE_PAD_DOWN,

    /* After the deck's press run. `status` and `assigned` are valid. */
    CUE_PAD_PRESSED,

    /* After the deck's own release run. */
    CUE_PAD_UP,
};

struct cue_event {
    /* The deck's pad closure. Opaque to behaviours except as the argument the
     * stock helpers in pad.c take. */
    void *task;

    /* `pad` is the deck's 0-based pad index (A=0), read off the closure;
     * `kind` is the CueKind the cue APIs take, which is pad + 1. */
    int pad;
    int kind;

    /* CUE_PAD_PRESSED only. The deck's press run returns 0 for a pad that was
     * already set (it jumps to it) and 1 for one that was not (it sets it at the
     * play head). Holding the preview needle does not change this, so
     * preview.c has to intercept. */
    int64_t status;
    int     assigned;

    /* The preview needle at the moment of the event: whether the zone is held,
     * and where, as a normalised fraction of the track. The deck multiplies it
     * by the source length and snaps it to the beat grid itself, so pass the
     * fraction on unchanged. */
    int   needle_up;
    float needle_at;
};

/* ================================================================== */
/* A behaviour                                                        */
/* ================================================================== */

struct cue_handler {
    const char *name;
    int         prio;                 /* (prio, name) ascending, like KIT_MOD */

    /* Called for every pad event of the matching phase. NULL for the phases a
     * behaviour does not care about. */
    void (*pad)(const struct cue_event *ev, enum cue_phase phase);

    /* PLAY, while at least one pad is down. Returning non-zero consumes the
     * press so the deck never sees it. The first behaviour to claim it wins. */
    int (*play_while_held)(void);

    /* Asked after CUE_PAD_DOWN and before the deck's own press run. Returning
     * non-zero takes the pad: the deck's press does not run, so it does not
     * jump, play, or set a cue on an empty pad. The first claimer in
     * (prio, name) order wins.
     *
     * Only the claimer gets CUE_PAD_PRESSED and CUE_PAD_UP for a claimed pad;
     * other behaviours do not, since the deck's press did not happen (gate.c
     * must not back-cue a jump that was never made). `status` and `assigned`
     * are left zero; a claimer reads the cue table itself.
     *
     * Conversely, a behaviour that declares `pad_claim` gets CUE_PAD_DOWN for
     * every pad but the later phases only for the pads it took.
     *
     * Claim only a press that can be acted on; a claimed press that does
     * nothing is a dead pad. A behaviour that needs stems, a second cue or
     * anything else that might be missing checks first and declines. */
    int (*pad_claim)(const struct cue_event *ev);

    /* The op the deck's own release should perform for this press. Asked
     * before the stock release run; return a CUE_OP_* value, or 0 for the plain
     * release. The first non-zero in (prio, name) order wins.
     *
     * The release closure carries the op, so the work runs in the deck's own
     * task and order. Issuing the op separately races the press: a back-cue
     * issued that way was lost on short presses while the press's jump was
     * still in flight. */
    int (*release_op)(const struct cue_event *ev);
};

/* `used` because nothing in C refers to the symbol; the section does. Same
 * shape as KIT_MOD. */
#define CUE_HANDLER(sym, ...) \
    static const struct cue_handler sym \
        __attribute__((used, section("ep122_cue"))) = { __VA_ARGS__ }

/* The section bounds, defined by GNU ld for any C-identifier section name. */
extern const struct cue_handler __start_ep122_cue[] __attribute__((visibility("hidden")));
extern const struct cue_handler __stop_ep122_cue[] __attribute__((visibility("hidden")));

/* ================================================================== */
/* What pad.c knows, for behaviours that need more than the event      */
/* ================================================================== */

/* Whether the interception layer installed. A behaviour's install must fail
 * when it did not, so no MOD SETTINGS row is left that does nothing. */
int cue_pad_ready(void);           /* [init] */

/* usecase::deck::CueController for the event's deck, or NULL. Same object and
 * same path as the stock press. */
void *cue_controller(const struct cue_event *ev);      /* [deck] */

/* How many hot-cue pads are down right now. */
int cue_pads_held(void);                               /* [deck] */

/* Whether the deck was paused at this event's press, by the deck's definition
 * (track loaded, not playing, not in a cue mode). Read from the pad handler's
 * dj_player::PlayerState; 0 when that cannot be read. */
int cue_deck_paused(const struct cue_event *ev);                /* [deck] */

/* CueController slots. Each forwards to one dj_player::ICueLoopSetter method;
 * see docs/mods.md for the full map.
 *
 *   SETPOINT   setPoint(CueKind, QuantizeSetUnit)  put a cue at the play head
 *   SETHOTCUE  the empty-pad path, (CueKind, pad)
 *   CUEING     the CUE button's own action; hooked, not called
 */
#define CUE_VT_CUEING      0x10
#define CUE_VT_SETPOINT    0x18
#define CUE_VT_SETHOTCUE   0xb0

/* CueKind. Hot cues are 1..8 and the memory cue is 0; a real CUE press logs
 * cueing(kind=0, slip=0, quantize=0) in pad.c, which warns if a firmware
 * differs. QuantizeSetUnit 0 is what that press carries, so passing it places
 * a cue where CUE would. */
#define CUE_KIND_MEMORY    0
#define CUE_QUANTIZE_DECK  0

/* Ops a `release_op` may return. The deck's release dispatches on this word:
 * 1 returns to the pressed hot cue and pauses; anything else is the plain
 * release. */
#define CUE_OP_BACKCUE     1

/* The deck's own press and release runs, for a claimer that wants the stock
 * behaviour with a change. A cue set by the deck's press also gets the pad's
 * colour, bookkeeping, marker and lamp; a cue set any other way lacks them. So a
 * claimer that wants a cue elsewhere runs the deck's press and redirects it.
 *
 * Use them in pairs: the release completes the press's pad state, and pad.c
 * skips it for a claimed pad. The press returns what the deck's does: 0 for a
 * pad that was already set. */
int64_t cue_stock_press(const struct cue_event *ev);   /* [deck] */
int64_t cue_stock_release(const struct cue_event *ev); /* [deck] */

/* ---- the cue table --------------------------------------------------------
 *
 * The deck keeps one slot per CueKind for the loaded track, all with the same
 * layout: the memory cue, the eight hot cues, and the preview needle at kind 9.
 * `cue_slot` returns a slot; `cue_slot_pos` returns just its position.
 *
 * Both are [deck] and valid only during the event: the table belongs to the
 * loaded track and goes stale when another is loaded. Do not keep the
 * address. */
uintptr_t cue_slot(const struct cue_event *ev, int kind);   /* [deck] */

/* The cue engine the deck's own setters take, or NULL before one has been seen.
 * Captured from a live setHere in preview.c (see there for why). Pass it to
 * EP122_CUE_SET_AT to place a cue at a given position the way the deck does.
 * [any] */
void *cue_engine(void);

/* The sample position a slot holds. Returns 1 when set, 0 when unset or
 * unreadable. An unset slot holds POS_INVALID, since zero is a valid position
 * (a cue at the top of the track). */
int cue_slot_pos(const struct cue_event *ev, int kind, int64_t *out);  /* [deck] */

/* A slot's position offset and the "no position" value. The rest of the slot
 * layout lives in preview.c, the only writer. */
#define CUE_SLOT_POS_OFF   0x40
#define CUE_POS_INVALID    0x7fffffffffffffffLL

/* Hot cues occupy kinds 1..8, so a pad index is 0..7. */
#define CUE_PADS           8

/* ================================================================== */
/* The behaviours' own state, shared with the settings record          */
/* ================================================================== */

/* Whether each behaviour is on. Read on every event, so a MOD SETTINGS row
 * takes effect without re-installing. Global because core/common.c persists
 * them and the play-screen shortcut reads one. */
extern int g_gate_on;      /* cue/gate.c    -- momentary pads          */
extern int g_smart_on;     /* cue/smart.c   -- memory cue follows      */
extern int g_preview_on;   /* cue/preview.c -- assign under the needle  */

/* cue/shortcut.c: repaint the play-screen GATE CUE plate after something else
 * moved g_gate_on. No-op until it has been built. */
void cue_shortcut_refresh(void);

/* cue/led.c: advance the armed pad's blink. Called from the display timer,
 * because the app writes a lamp only when its own state changes, and a running
 * groove does not change it. [message] */
void cue_led_tick(void);

/* cue/pad.c: re-request a back-cue the deck may have dropped. Runs on the
 * display timer, the layer's only periodic clock, since a release needs a
 * follow-up a few hundred milliseconds later. No-op unless a release armed it.
 * [message] */
void cue_pad_tick(void);


#ifdef __cplusplus
}
#endif

#endif /* EP122_MOD_CUE_H */
