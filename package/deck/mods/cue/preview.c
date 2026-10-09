// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * cue/preview.c - PREVIEW HOTCUE: assign where the needle is, not where the
 * play head is.
 *
 * Hold the preview zone (green needle) and press an unassigned pad: the cue is
 * set under the needle on the press, and nothing else happens.
 *
 * The press is claimed, so no other behaviour sees it: the pad does not jump,
 * GATE CUE does not back-cue on release, and SMART CUE does not follow.
 *
 * The handler then runs the deck's own press itself. The claim stops the other
 * behaviours; the deck's press sets a complete cue, including its colour (see
 * below).
 *
 * Mechanism
 * ---------
 * Every cue is set by one setter over a
 * pcmbuf::PositionWithSourceInfo, and the cue engine has two entry points:
 *
 *     setHere(engine, kind, quantize, desc)     the PLAY HEAD
 *     setAt  (engine, kind, pos, onGrid, desc)  a given position
 *
 * The redirect is one hook on setHere: when a kind is armed, it calls setAt with
 * the needle's position instead, forwarding the caller's `desc`.
 *
 * `desc` is the colour: three caller-owned bytes that the setter copies into
 * the slot at +0x10..+0x12. That is why only the position is redirected: a cue
 * set through setPoint gets setPoint's default 255,113,0 (orange) instead of
 * the deck's 0,127,0.
 *
 * The needle's position is already a cue record, snapped to the grid.
 * The deck converts and snaps the strip's touch and stores it through the
 * per-kind accessor the hot cues use, at CueKind 9 (after the memory cue's 0
 * and the hot cues' 1..8). Every slot embeds its PositionWithSourceInfo at
 * +0x28, so the arm is a 0x30-byte snapshot plus the beat-grid flag.
 *
 * The slot is not written directly because setting a cue does more than store
 * a position. The setter also takes a reference on the source, sets the exists
 * byte at +0x20 and the on-grid byte at +0x21, bumps the version at +0x08,
 * writes the state at +0x0c and resets the second position block at +0x70; the
 * facade above it then notifies the lamp and the waveform marker.
 *
 * The arm is a snapshot, not a pointer to slot 9, because slot 9 keeps moving
 * under the finger while setPoint's write is in flight.
 */
#include "cue/cue.h"
#include "kit/menu.h"
#include "kit/mod.h"

int g_preview_on;               /* persisted; see core/common.c */

/* CueKind 9 is the preview needle. */
#define CUE_KIND_PREVIEW    9

/* Inside a cue slot: the embedded position record, and the byte saying it sits
 * on a beat. Both are read from slot 9 and handed to setAt unchanged. */
#define SLOT_POS_INFO_OFF   0x28
#define SLOT_POS_INFO_LEN   0x30
#define SLOT_ON_GRID_OFF    0x21

#define FN_SET_AT           ep122_sym(EP122_CUE_SET_AT)

typedef void    (*set_here_fn_t)(void *engine, uint32_t kind, uint32_t quantize,
                                 void *desc);
typedef int64_t (*set_at_fn_t)(void *engine, uint32_t kind, const void *pos,
                               uint32_t on_grid, void *desc);

static uintptr_t preview_g_set_here;     /* the stock setHere, behind the hook */

/* The cue engine, captured from a live setHere call. Every cue the deck sets
 * passes through it, so it is known long before it is needed, and it is the
 * pointer the deck's own setter uses. Single-player, so the last one seen is the
 * only one. */
static void *preview_g_engine;

void *cue_engine(void) { return __atomic_load_n(&preview_g_engine, __ATOMIC_ACQUIRE); }
static int       preview_g_ready;

/* The arm. `kind` is published last and consumed with a compare-exchange, so
 * the pair above it is visible to the thread that runs the write and exactly
 * one write consumes it. Written [deck], read on whichever thread setPoint's
 * task runs.
 *
 * One at a time; a new press clears any stale arm. */
static uint8_t preview_g_pos[SLOT_POS_INFO_LEN] __attribute__((aligned(16)));
static uint8_t preview_g_on_grid;
static int     preview_g_kind;           /* 0 = nothing armed; kinds are 1..8 */

/* ---- the redirect --------------------------------------------------------- */

static void preview_wrap_set_here(void *engine, uint32_t kind, uint32_t quantize,
                                  void *desc)
{
    int want = __atomic_load_n(&preview_g_kind, __ATOMIC_ACQUIRE);

    __atomic_store_n(&preview_g_engine, engine, __ATOMIC_RELEASE);

    if (want && want == (int)kind &&
        __atomic_compare_exchange_n(&preview_g_kind, &want, 0, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        int64_t at = 0;

        memcpy(&at, preview_g_pos + 0x18, sizeof(at));
        MDBG("preview: kind %u lands at %lld, not the play head\n",
             kind, (long long)at);
        ((set_at_fn_t)FN_SET_AT)(engine, kind, preview_g_pos,
                                 preview_g_on_grid, desc);
        return;
    }
    ((set_here_fn_t)preview_g_set_here)(engine, kind, quantize, desc);
}

/* ---- the gesture ---------------------------------------------------------- */

/* Claim the press when it means "put a cue here".
 *
 * Not claimed: an assigned pad (the press jumps to it and must not move it), a
 * needle with no position yet, or a build where the redirect did not install
 * (the cue would land at the play head). */
static int preview_claim(const struct cue_event *ev)
{
    int64_t at;

    if (!g_preview_on || !preview_g_ready || !ev->needle_up)
        return 0;
    if (cue_slot_pos(ev, ev->kind, &at))
        return 0;
    if (!cue_slot_pos(ev, CUE_KIND_PREVIEW, &at))
        return 0;
    return 1;
}

static void preview_pad(const struct cue_event *ev, enum cue_phase phase)
{
    uintptr_t src;

    switch (phase) {
    case CUE_PAD_DOWN:
        /* Called for every pad, claimed or not; drops any stale arm. */
        __atomic_store_n(&preview_g_kind, 0, __ATOMIC_RELEASE);
        return;

    case CUE_PAD_UP:
        /* Pairs with the press below. Does not disarm: on a quick tap the
         * release can run before the write, which would then land at the play
         * head. */
        cue_stock_release(ev);
        return;

    default:
        break;
    }

    src = cue_slot(ev, CUE_KIND_PREVIEW);
    if (!src ||
        mod_safe_read(src + SLOT_POS_INFO_OFF, preview_g_pos,
                      sizeof(preview_g_pos)) != 0 ||
        mod_safe_read(src + SLOT_ON_GRID_OFF, &preview_g_on_grid, 1) != 0) {
        MDBG("preview: needle slot unreadable -> pad %d does nothing\n", ev->pad);
        return;
    }

    /* Armed before the press, since its write can run on another thread
     * immediately. */
    __atomic_store_n(&preview_g_kind, ev->kind, __ATOMIC_RELEASE);
    MDBG("preview: pad %d takes the needle at %d/1000\n",
         ev->pad, (int)(ev->needle_at * 1000.0f));
    cue_stock_press(ev);
}

CUE_HANDLER(k_cue_preview,
            .name = "preview", .prio = 30,
            .pad = preview_pad, .pad_claim = preview_claim);

/* ---- the row -------------------------------------------------------------- */

/* On by default: the gesture already sets a hot cue in stock; this only
 * changes where. */
static const struct kit_row k_rows[] = {
    KIT_ROW_BOOL("PREVIEW HOTCUE", &g_preview_on, .idx = KIT_IDX_PREVIEW),
};

static int preview_install(void)
{
    if (!cue_pad_ready()) {
        MDBG("preview: no cue interception -> no row\n");
        return -1;
    }
    if (!FN_SET_AT) {
        MDBG("preview: no setAt -> no row\n");
        return -1;
    }
    if (mod_patch_fn("cueSetHere", ep122_sym(EP122_CUE_SET_HERE),
                     (void *)preview_wrap_set_here, &preview_g_set_here) != 0) {
        MDBG("preview: no setHere hook -> no row\n");
        return -1;
    }
    preview_g_ready = 1;
    kit_menu_add(k_rows, (int)(sizeof(k_rows) / sizeof(k_rows[0])));
    return 0;
}

KIT_MOD(k_mod_cue_preview,
        .name = "cue_preview", .prio = 12, .install = preview_install,
        .what = "preview hot cue: assign under the needle");
