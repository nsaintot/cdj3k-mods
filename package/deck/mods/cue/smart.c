// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * cue/smart.c - SMART CUE: the memory cue follows the pad you last pressed.
 *
 * The memory cue is set to the hot cue's position, read from the pad's slot,
 * on the release.
 *
 * Reading the slot instead of the play head means nothing depends on the
 * press's jump having landed, so the decision can wait for the release, the
 * first point where an erase can be told from a recall. A press under
 * CALL/DELETE erases the cue, and the deck reports it the same way as a recall
 * ("the pad was already set").
 *
 * Only a press that recalled an existing cue is followed: the pad must have had
 * a cue when it went down and still have one when it comes up. A press on an
 * empty pad sets a hot cue at the play head, which is not a recall.
 *
 * The cue is placed through the deck's own setAt, not by writing the slot,
 * because setting a cue also takes a reference on the source, sets the exists
 * and on-grid bytes, bumps the version and notifies the lamp and the waveform
 * marker (see preview.c). The engine setAt needs is captured by preview.c's
 * setHere hook.
 */
#include "cue/cue.h"
#include "kit/menu.h"
#include "kit/mod.h"

int g_smart_on;                 /* persisted; see core/common.c */

typedef int64_t (*set_at_fn_t)(void *engine, uint32_t kind, const void *pos,
                               uint32_t on_grid, void *desc);

/* Inside a cue slot, as preview.c reads them: the embedded position record, the
 * byte saying it sits on a beat, and the three colour bytes a setter copies in
 * as `desc`. */
#define SLOT_POS_INFO_OFF   0x28
#define SLOT_POS_INFO_LEN   0x30
#define SLOT_ON_GRID_OFF    0x21
#define SLOT_DESC_OFF       0x10
#define SLOT_DESC_LEN       3

#define FN_SET_AT           ep122_sym(EP122_CUE_SET_AT)

/* setPoint's default cue colour. Used only when there is no memory cue to take
 * a colour from. */
static const uint8_t k_mem_desc[SLOT_DESC_LEN] = { 255, 113, 0 };

/* The pressed pad's cue, read at press, and the memory cue's colour so moving
 * it does not recolour it. [deck] */
static uint8_t smart_g_pos[SLOT_POS_INFO_LEN];
static uint8_t smart_g_desc[SLOT_DESC_LEN];
static uint8_t smart_g_on_grid;
static int     smart_g_have;

/* Bit per pad: the pad had a cue when it went down. */
static unsigned smart_g_had;

/* Copy the pad's cue out of its slot, plus a colour for the memory cue. */
static void smart_take(const struct cue_event *ev)
{
    uintptr_t pad_slot = cue_slot(ev, ev->pad + 1);
    uintptr_t mem_slot = cue_slot(ev, CUE_KIND_MEMORY);
    int64_t at = 0;

    smart_g_have = 0;
    if (!pad_slot || !cue_slot_pos(ev, ev->pad + 1, &at))
        return;
    if (mod_safe_read(pad_slot + SLOT_POS_INFO_OFF, smart_g_pos,
                      sizeof(smart_g_pos)) != 0 ||
        mod_safe_read(pad_slot + SLOT_ON_GRID_OFF, &smart_g_on_grid,
                      sizeof(smart_g_on_grid)) != 0)
        return;
    /* The memory cue keeps its own colour; a track without one gets the colour
     * the deck would have given it. */
    if (!mem_slot || !cue_slot_pos(ev, CUE_KIND_MEMORY, &at) ||
        mod_safe_read(mem_slot + SLOT_DESC_OFF, smart_g_desc,
                      sizeof(smart_g_desc)) != 0)
        memcpy(smart_g_desc, k_mem_desc, sizeof(smart_g_desc));
    smart_g_have = 1;
}

/* Put the memory cue on the cue the press went to. */
static void smart_follow(int pad)
{
    void *engine = cue_engine();

    if (!smart_g_have || !engine || !FN_SET_AT)
        return;
    ((set_at_fn_t)FN_SET_AT)(engine, CUE_KIND_MEMORY, smart_g_pos,
                             smart_g_on_grid, smart_g_desc);
    MDBG("smart: memory cue follows pad %d\n", pad);
}

static void smart_pad(const struct cue_event *ev, enum cue_phase phase)
{
    int64_t at = 0;

    if (!g_smart_on)
        return;

    switch (phase) {
    case CUE_PAD_DOWN:
        /* Read before the deck's press can change them: whether this pad held
         * a cue, and where. */
        if (cue_slot_pos(ev, ev->pad + 1, &at))
            smart_g_had |= 1u << ev->pad;
        else
            smart_g_had &= ~(1u << ev->pad);
        smart_take(ev);
        break;

    case CUE_PAD_PRESSED:
        break;

    case CUE_PAD_UP:
        /* Had a cue and still has one: the press went to it. Had one and lost it:
         * the press erased it under CALL/DELETE, and there is nothing to follow.
         * Had none: the press set one, which is not a jump either. */
        if (!(smart_g_had & (1u << ev->pad)))
            break;
        if (!cue_slot_pos(ev, ev->pad + 1, &at)) {
            MDBG("smart: pad %d lost its cue -> memory cue stays put\n", ev->pad);
            break;
        }
        smart_follow(ev->pad);
        break;
    }
}

CUE_HANDLER(k_cue_smart, .name = "smart", .prio = 20, .pad = smart_pad);

static const struct kit_row k_rows[] = {
    KIT_ROW_BOOL("SMART CUE", &g_smart_on, .idx = KIT_IDX_SMART),
};

static int smart_install(void)
{
    if (!cue_pad_ready()) {
        MDBG("smart: no cue interception -> no row\n");
        return -1;
    }
    kit_menu_add(k_rows, (int)(sizeof(k_rows) / sizeof(k_rows[0])));
    return 0;
}

KIT_MOD(k_mod_cue_smart,
        .name = "cue_smart", .prio = 11, .install = smart_install,
        .what = "smart cue: the memory cue follows the pressed pad");
