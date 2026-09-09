// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * cue/cue.h - the hot-cue interception layer, and what a behaviour is.
 *
 * ONE place hooks the deck's hot-cue path. pad.c owns every slot involved --
 * the pad's own press and release, PLAY, the CUE button, and the preview needle
 * -- decodes each one into an event, and hands it round. Nothing else in the
 * tree touches those slots, and no behaviour repeats the ABI.
 *
 * A behaviour is a `struct cue_handler` in the ep122_cue section, next to the
 * code that implements it (CUE_HANDLER below). Adding one is a new file: no
 * central table, no header that names every behaviour, and the same
 * (prio, name) ordering the mod registry uses. That is what makes per-slot work
 * -- a colour rule, a blink, a pad that means something different -- an
 * addition rather than another edit to a function three features already share.
 *
 * Everything here runs on the DECK thread ([deck]) except the preview needle,
 * which is written from [message] and read from [deck]; pad.c does that with an
 * atomic and hands behaviours a snapshot, so a handler never races it.
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
     * happened yet, which is where a behaviour arms something it wants in place
     * before any other event can arrive. */
    CUE_PAD_DOWN,

    /* After it. `status` and `assigned` are valid, so this is where a behaviour
     * acts on what the deck just did -- or corrects it. */
    CUE_PAD_PRESSED,

    /* After the deck's own release run. */
    CUE_PAD_UP,
};

struct cue_event {
    /* The deck's pad closure. Opaque to behaviours except as the argument the
     * stock helpers in pad.c take. */
    void *task;

    /* Which pad, two ways. `pad` is the deck's own 0-based index (A=0),
     * measured off the closure; `kind` is the CueKind the cue APIs want, which
     * is that plus one. Both, because a behaviour talking to the deck needs the
     * kind and one talking about the pad wants the index. */
    int pad;
    int kind;

    /* CUE_PAD_PRESSED only. The deck's press run returns 0 for a pad that was
     * already set -- it jumps to it -- and 1 for one that was not, which it
     * then sets AT THE PLAY HEAD. Measured on the deck; holding the preview
     * needle does not change it, which is why intercepting is our job. */
    int64_t status;
    int     assigned;

    /* The preview needle at the moment of the event: whether the zone is held,
     * and where, as a NORMALISED fraction of the track. The deck multiplies it
     * by the source length and snaps it to the beat grid itself, so a behaviour
     * that wants "where the strip was touched, on the grid" passes the fraction
     * on rather than doing any arithmetic of its own. */
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

    /* PLAY, while at least one pad is down. Returning non-zero CONSUMES the
     * press: the deck never sees it. Only one behaviour may claim a press --
     * the first that does wins, and pad.c stops there. */
    int (*play_while_held)(void);

    /* Asked after CUE_PAD_DOWN and BEFORE the deck's own press run. Returning
     * non-zero TAKES THE PAD: the deck never runs, so it does not jump, does not
     * play, and does not set a cue on an empty pad -- the press means whatever
     * the claimer says it means, for as long as it is held.
     *
     * A claimed pad is the claimer's alone. It gets CUE_PAD_PRESSED and
     * CUE_PAD_UP; nobody else does, because the deck's press they were reacting
     * to did not happen -- gate.c must not back-cue a jump that was never made.
     * `status` and `assigned` are meaningless on a claimed press and are left
     * zero: nothing ran to report them, so a claimer reads the cue table itself.
     *
     * The converse holds too, and it is what keeps a claimer honest: declaring
     * `pad_claim` means `pad` is called for CUE_PAD_DOWN on every pad, and for
     * the LATER PHASES ONLY on the pads this behaviour took. A behaviour that
     * replaces a press is not an observer of the ones it waved through, and
     * acting on those would run its effect on whatever the last claim left
     * behind.
     *
     * Claim ONLY what can be honoured. A press taken and then not acted on is a
     * dead pad, which is worse than the stock behaviour it replaced -- so a
     * behaviour that needs stems, or a second cue, or anything else that might
     * not be there checks first and declines. First claimer in (prio, name)
     * order wins. */
    int (*pad_claim)(const struct cue_event *ev);

    /* What the deck's OWN release should mean for this press. Asked BEFORE the
     * stock release run; return an op (see CUE_OP_* below) to give the deck one,
     * or 0 to leave the press alone. First non-zero in (prio, name) order wins.
     *
     * This is how to make the deck do something on a release, rather than doing
     * it alongside: the release closure carries an op the handler reads, so
     * setting it puts the work in the deck's own task, in the deck's own order.
     * Issuing the same operation SEPARATELY races the press it belongs to --
     * measured, before this existed, as a back-cue that took on a long hold and
     * was lost on a short one, because the press's jump was still in flight. */
    int (*release_op)(const struct cue_event *ev);
};

/* `used` because nothing in C refers to it: the section is the reference. Same
 * shape as KIT_MOD, and for the same reason. */
#define CUE_HANDLER(sym, ...) \
    static const struct cue_handler sym \
        __attribute__((used, section("ep122_cue"))) = { __VA_ARGS__ }

/* The section bounds, defined by GNU ld for any C-identifier section name. */
extern const struct cue_handler __start_ep122_cue[] __attribute__((visibility("hidden")));
extern const struct cue_handler __stop_ep122_cue[] __attribute__((visibility("hidden")));

/* ================================================================== */
/* What pad.c knows, for behaviours that need more than the event      */
/* ================================================================== */

/* Did the interception layer install? A behaviour's install must refuse when it
 * did not, rather than leave a MOD SETTINGS row that does nothing. */
int cue_pad_ready(void);           /* [init] */

/* usecase::deck::CueController for the deck this event belongs to, or NULL. The
 * same object the stock press path uses, reached the same way. */
void *cue_controller(const struct cue_event *ev);      /* [deck] */

/* How many hot-cue pads are down right now. */
int cue_pads_held(void);                               /* [deck] */

/* Was the deck playing when this event's press arrived? Read off the pad
 * handler's own dj_player::PlayerState. 0 when it cannot be read. [deck] */
int cue_deck_playing(const struct cue_event *ev);

/* CueController slots. Each is one thin forwarder to a dj_player::ICueLoopSetter
 * method -- see docs/mods.md and the memory note for the whole map.
 *
 *   SETPOINT   setPoint(CueKind, QuantizeSetUnit)  put a cue at the play head
 *   SETHOTCUE  the empty-pad path, (CueKind, pad)
 *   CUEING     the CUE BUTTON's own action; hooked, not called
 */
#define CUE_VT_CUEING      0x10
#define CUE_VT_SETPOINT    0x18
#define CUE_VT_SETHOTCUE   0xb0

/* CueKind. Hot cues are 1..8, so the memory cue is 0 -- and a real CUE press
 * says so: pad.c logs cueing(kind=0, slip=0, quantize=0) and shouts if a
 * firmware ever disagrees. QuantizeSetUnit 0 is what that same press carries,
 * so a behaviour passing it puts a cue exactly where CUE would have. */
#define CUE_KIND_MEMORY    0
#define CUE_QUANTIZE_DECK  0

/* Ops a `release_op` may ask the deck's release for. The handler's release
 * dispatches on this word: 1 returns to the pressed hot cue and pauses, and
 * anything else is the plain release the deck already does. */
#define CUE_OP_BACKCUE     1

/* The deck's own press and release runs, for a CLAIMER that wants the stock
 * behaviour with something about it changed rather than replaced.
 *
 * Taking a press stops the deck's from running, which is right when the press
 * is going to mean something else -- and wrong when it means the same thing
 * somewhere else. A cue the deck sets carries everything it sets ALONGSIDE the
 * position: the pad's colour, its bookkeeping, the marker and the lamp. Setting
 * one any other way gets a cue that is subtly not one, so a claimer that wants
 * a cue asks for the deck's press and arranges for it to land elsewhere.
 *
 * USE THEM IN PAIRS. The release is the other half of the press's pad state,
 * and pad.c skips it for a claimed pad -- so a claimer that runs one runs both.
 * The press returns what the deck's does: 0 for a pad that was already set. */
int64_t cue_stock_press(const struct cue_event *ev);   /* [deck] */
int64_t cue_stock_release(const struct cue_event *ev); /* [deck] */

/* ---- the cue table --------------------------------------------------------
 *
 * The deck keeps one slot per CueKind for the loaded track, and every kind's
 * slot has the same layout -- the memory cue's, the eight hot cues', and the
 * preview needle's at kind 9. `cue_slot` walks to one; anything that only wants
 * the position asks `cue_slot_pos`.
 *
 * Both are [deck] and valid only for the duration of the event: the table
 * belongs to the loaded track, and the pointer is stale the moment one is
 * loaded over it. Read what is needed and do not keep the address. */
uintptr_t cue_slot(const struct cue_event *ev, int kind);   /* [deck] */

/* The cue engine the deck's own setters take, or NULL before one has been seen.
 * Captured from a live setHere in preview.c -- see there for why it is taken
 * rather than walked to. Pair it with EP122_CUE_SET_AT to put a cue somewhere
 * specific the way the deck would. [any] */
void *cue_engine(void);

/* The sample position a slot holds. 1 when there is one, 0 when the slot is
 * unset or unreadable -- an unset slot reads as POS_INVALID rather than zero,
 * and zero is a legitimate position (a cue at the top of the track). */
int cue_slot_pos(const struct cue_event *ev, int kind, int64_t *out);  /* [deck] */

/* Where a slot keeps its position, and the value meaning "no position". Shared
 * because everything that reads the table needs the pair; the REST of the slot
 * layout belongs to preview.c, which is the only thing that writes one. */
#define CUE_SLOT_POS_OFF   0x40
#define CUE_POS_INVALID    0x7fffffffffffffffLL

/* Hot cues occupy kinds 1..8, so a pad index is 0..7. */
#define CUE_PADS           8

/* ================================================================== */
/* The behaviours' own state, shared with the settings record          */
/* ================================================================== */

/* Whether each behaviour acts. Read live on every event, so a MOD SETTINGS row
 * toggles it with nothing to re-install. Bare g_ because common.c persists them
 * and the play-screen shortcut reads one. */
extern int g_gate_on;      /* cue/gate.c    -- momentary pads          */
extern int g_smart_on;     /* cue/smart.c   -- memory cue follows      */
extern int g_preview_on;   /* cue/preview.c -- assign under the needle  */

/* cue/shortcut.c: repaint the play-screen GATE CUE plate after something else
 * moved g_gate_on. No-op until it has been built. */
void cue_shortcut_refresh(void);

/* cue/led.c: advance the armed pad's blink. Called from the display timer,
 * because the app writes a lamp only when its OWN state changes and a groove
 * running is not one of them. [message] */
void cue_led_tick(void);

/* cue/pad.c: ask the deck again for a back-cue it may have dropped. Same clock,
 * because a release needs following up a few hundred milliseconds later and this
 * is the only periodic one the layer has. Does nothing unless a release armed
 * it. [message] */
void cue_pad_tick(void);


#ifdef __cplusplus
}
#endif

#endif /* EP122_MOD_CUE_H */
