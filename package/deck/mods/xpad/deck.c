// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * xpad/deck.c - the eight pads and the three panel controls, borrowed.
 *
 * This file enforces "the panel is the mode". With the X-PAD shut, each control
 * does its stock job: the pads are hot cues, MEMORY stores a memory cue,
 * CALL/DELETE deletes one and VINYL SPEED ADJUST sets the brake. While the panel
 * is open they belong to the sampler. Only an explicit open changes them, which
 * is why taking the destructive DELETE button is acceptable.
 *
 * All three controls reach the app as an AsyncTask whose run carries the
 * gesture. The gate is simply not calling the stock run: the value never reaches
 * the deck, and closing the panel restores the control with no state to undo.
 *
 *   MEMORY   input_devices::MemoryCueHandler::memoryCue   -> HOLD
 *   DELETE   input_devices::MemoryCueHandler::deleteCue   -> OVERDUB
 *   VINYL    dj_player::PlayPauseControlFacade::vinylSpeedAdjust -> VOL
 *
 * The first two capture only `this`, so each run is one press. The vinyl task
 * captures its float (the knob) at +0x18 and the VinylSpeedAdjustKind (which of
 * the two curves) at +0x1c.
 *
 * Everything here is [deck]: the pads on the deck's pad path, the three tasks on
 * whichever thread runs them.
 */
#include "xpad/xpad.h"
#include "cue/cue.h"
#include "stem/stem.h"       /* stem_grid_take: the roll's clock */
#include "kit/mod.h"

/* ---- the pads ------------------------------------------------------------- */

/* An open panel claims all eight pads, loaded or not.
 *
 * This is an exception to cue.h's rule of claiming only what can be honoured: an
 * unclaimed pad fires a HOT CUE, which jumps the playhead mid-mix, while a dead
 * pad is silent.
 *
 * The open gate in ui.c refuses to open the panel with no banks, so an open
 * panel always has at least one live pad. */
static int xpad_pad_claim(const struct cue_event *ev)
{
    (void)ev;
    return xpad_open();
}

static void xpad_pad(const struct cue_event *ev, enum cue_phase phase)
{
    /* A press selects the bank and plays it once; the release is ignored.
     *
     * DOWN arrives for every pad, claimed or not (see cue.h), so the work is on
     * PRESSED, which only reaches claimed pads. */
    if (phase != CUE_PAD_PRESSED)
        return;

    /* A pad press is the only point where a cue slot, and so the track's beat
     * grid, is reachable. Without it there is no clock: no quantize, no roll, no
     * bar. Re-read on every press (a handful of reads) so it is never the
     * previous track's, and before the empty check, which is unrelated. */
    stem_grid_take(ev);
    /* An empty pad is silent. The claim keeps it from the hot cue; returning
     * here keeps xpad_fire from selecting an empty bank for the strip. */
    if (!xpad_bank_ready(ev->pad)) {
        MDBG("xpad: pad %c has no sample -> swallowed, no hot cue\n", 'A' + ev->pad);
        return;
    }
    xpad_fire(ev->pad);
    MDBG("xpad: pad %c selected -> %s\n", 'A' + ev->pad,
         xpad_bank_name(ev->pad));
}

/* Ahead of GROOVE CIRCUIT at 5. The two cannot both claim (the circuit is gated
 * on the stem row, this on the X-PAD panel, and the band holds one panel), but
 * the order is explicit anyway. */
CUE_HANDLER(k_cue_xpad,
            .name = "xpad", .prio = 4,
            .pad = xpad_pad, .pad_claim = xpad_pad_claim);

/* ---- the three panel controls --------------------------------------------- */

static uintptr_t xpad_g_orig_memory, xpad_g_orig_delete, xpad_g_orig_vinyl;

/* The knob's float, as the readout's percentage.
 *
 * The knob's units are unknown (a normalised position or a brake time). The
 * mapping assumes 0..1 and clamps, and the first eight raw values are logged. */
#define VINYL_TASK_FLOAT_OFF  0x18
#define VINYL_TASK_KIND_OFF   0x1c

static void xpad_vinyl_run(void *task)
{
    static int said;
    float v = 0.0f;
    int32_t kind = 0;
    int pct;

    if (!xpad_open()) {
        if (xpad_g_orig_vinyl)
            ((void (*)(void *))xpad_g_orig_vinyl)(task);
        return;
    }

    memcpy(&v, (const char *)task + VINYL_TASK_FLOAT_OFF, sizeof(v));
    memcpy(&kind, (const char *)task + VINYL_TASK_KIND_OFF, sizeof(kind));

    pct = (int)(v * 100.0f + 0.5f);
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    __atomic_store_n(&xpad_g_vol, pct, __ATOMIC_RELAXED);

    if (said < 8) {
        said++;
        MDBG("xpad: vinyl knob %.4f kind %d -> vol %d%%\n", (double)v, kind, pct);
    }
    /* The VOL rail is repainted here, since only the knob moves it. */
    xpad_repaint();
    /* Swallowed: the brake keeps the DJ's setting, including after the panel
     * shuts, until the knob moves again.
     *
     * Known gap. The value has one store in dj_player, reached three ways: this
     * task's run, a synchronous fallback inside the facade when its async
     * predicate fails, and a second public entry that tail-calls the store.
     * Only the task is a vtable slot, so only it is gated here. The gate holds
     * because the knob currently takes the async path.
     *
     * If it leaks: the object is *(*(facade + 0x58) + 0x80) with the facade at
     * task +0x20, holding the coefficient at +0x18 (kind 1) and +0x1c (kind 2)
     * and the raw 0..255 at +0x20 and +0x24. Snapshot those four when the panel
     * opens and re-assert them on the display tick to cover every writer.
     *
     * The knob sends kind 0, which the facade maps to kind 2, the brake.
     * Nothing on this chassis sends kind 1. */
}

static void xpad_memory_run(void *task)
{
    if (!xpad_open()) {
        if (xpad_g_orig_memory)
            ((void (*)(void *))xpad_g_orig_memory)(task);
        return;
    }
    xpad_g_hold = !xpad_g_hold;
    MDBG("xpad: MEMORY -> hold %s\n", xpad_g_hold ? "on" : "off");
    xpad_repaint();
}

static void xpad_delete_run(void *task)
{
    if (!xpad_open()) {
        if (xpad_g_orig_delete)
            ((void (*)(void *))xpad_g_orig_delete)(task);
        return;
    }
    xpad_overdub_set(!xpad_g_overdub);
    xpad_repaint();
}

/* ---- the deck's own QUANTIZE ----------------------------------------------
 *
 * gui::QuantizeState listens to both settings and stores both:
 *
 *   +0x8c  the mode, a bool
 *   +0x90  the beat value, 1..4
 *
 * The object's address comes from either of its two onSettingChanged closures,
 * at different offsets (+0x18 on the mode task, +0x20 on the value task). Both
 * fields are then read from the object, so one change learns both and a change
 * to either keeps them current.
 *
 * Until one fires, quantize is treated as off. The app applies its settings at
 * startup, so in practice it is known before the panel can open, and the value
 * is logged. */
#define QUANT_STATE_MODE_OFF   0x8c
#define QUANT_STATE_VALUE_OFF  0x90
#define QUANT_MODE_TASK_STATE  0x18
#define QUANT_VALUE_TASK_STATE 0x20

static uintptr_t xpad_g_orig_quant_mode, xpad_g_orig_quant_value;
static uintptr_t xpad_g_quant_state;
static int       xpad_g_quant_on, xpad_g_quant_val;

/* QuantizeBeatValue -> the divisor of a beat. The enum runs 1..4 finest first,
 * the reverse of the deck's menu order (value 4 is the menu's "1"). Values
 * outside 1..4 return 0, so quantize does not apply rather than applying at a
 * wrong length. */
static int quant_div_of(int v)
{
    switch (v) {
    case 1:  return 8;      /* 1/8 beat */
    case 2:  return 4;      /* 1/4      */
    case 3:  return 2;      /* 1/2      */
    case 4:  return 1;      /* 1 beat   */
    default: return 0;
    }
}

int xpad_quantize_div(void)
{
    if (!__atomic_load_n(&xpad_g_quant_on, __ATOMIC_RELAXED))
        return 0;
    return quant_div_of(__atomic_load_n(&xpad_g_quant_val, __ATOMIC_RELAXED));
}

/* Read both fields off the state object and publish them. Called after the stock
 * run, so what is read is what the app just stored. */
static void quant_refresh(uintptr_t state)
{
    int32_t mode = 0, val = 0;
    static int said_on = -1, said_val = -1;

    if (!state) return;
    xpad_g_quant_state = state;
    if (mod_safe_read(state + QUANT_STATE_MODE_OFF, &mode, sizeof(mode)) != 0 ||
        mod_safe_read(state + QUANT_STATE_VALUE_OFF, &val, sizeof(val)) != 0)
        return;
    __atomic_store_n(&xpad_g_quant_on, mode != 0, __ATOMIC_RELAXED);
    __atomic_store_n(&xpad_g_quant_val, (int)val, __ATOMIC_RELAXED);
    if (mode != said_on || val != said_val) {
        said_on = mode; said_val = val;
        MDBG("xpad: deck quantize %s, value %d -> 1/%d beat\n",
             mode ? "on" : "off", (int)val, quant_div_of((int)val));
    }
}

static void xpad_quant_mode_run(void *task)
{
    uintptr_t state = 0;

    if (xpad_g_orig_quant_mode)
        ((void (*)(void *))xpad_g_orig_quant_mode)(task);
    if (mod_safe_read((uintptr_t)task + QUANT_MODE_TASK_STATE,
                      &state, sizeof(state)) == 0)
        quant_refresh(state);
}

static void xpad_quant_value_run(void *task)
{
    uintptr_t state = 0;

    if (xpad_g_orig_quant_value)
        ((void (*)(void *))xpad_g_orig_quant_value)(task);
    if (mod_safe_read((uintptr_t)task + QUANT_VALUE_TASK_STATE,
                      &state, sizeof(state)) == 0)
        quant_refresh(state);
}

/* ---- install -------------------------------------------------------------- */

static int xpad_deck_install(void)
{
    int ok = 0;

    if (!cue_pad_ready())
        MERR("xpad: no cue interception -> the pads stay hot cues\n");

    if (mod_patch_vslot("xpadMemory", EP122_MEMCUE_MEMORY_TASK, 0x10,
                        (void *)xpad_memory_run, &xpad_g_orig_memory) == 0)
        ok++;
    else
        MWARN("xpad: no MEMORY task -> HOLD has no button\n");

    if (mod_patch_vslot("xpadDelete", EP122_MEMCUE_DELETE_TASK, 0x10,
                        (void *)xpad_delete_run, &xpad_g_orig_delete) == 0)
        ok++;
    else
        MWARN("xpad: no DELETE task -> OVERDUB has no button\n");

    if (mod_patch_vslot("xpadVinyl", EP122_VINYL_ADJ_TASK, 0x10,
                        (void *)xpad_vinyl_run, &xpad_g_orig_vinyl) == 0)
        ok++;
    else
        MWARN("xpad: no VINYL task -> VOL has no knob\n");

    /* Chained, never swallowed: we only listen to QUANTIZE. If either patch
     * fails the sampler behaves as if quantize were off; logged, not fatal. */
    if (mod_patch_vslot("xpadQuantMode", EP122_QUANT_MODE_TASK, 0x10,
                        (void *)xpad_quant_mode_run, &xpad_g_orig_quant_mode) != 0)
        MDBG("xpad: no QUANTIZE mode task -> the sampler will not snap\n");
    if (mod_patch_vslot("xpadQuantValue", EP122_QUANT_VALUE_TASK, 0x10,
                        (void *)xpad_quant_value_run, &xpad_g_orig_quant_value) != 0)
        MDBG("xpad: no QUANTIZE value task -> the sampler will not snap\n");

    /* Each missing control is logged and the rest keep working; install fails
     * only if none of the three patched. */
    return ok > 0 ? 0 : -1;
}

KIT_MOD(k_mod_xpad_deck,
        .name = "xpad_deck", .prio = 36, .install = xpad_deck_install,
        .what = "X-PAD SAMPLER: the pads, and MEMORY/DELETE/VINYL while it is open");
