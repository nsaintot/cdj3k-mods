// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * cue/led.c - writes the panel's pad lamps.
 *
 * This file only finds the lamps, maps ordinals to pads, keeps each lamp's
 * holder and writes to it. What a pad should look like is decided in
 * lamp/lamp.h (a triple is a hue; the slot is the whole brightness).
 *
 * The hook is in the app, not on the ioctl the lamp frame leaves through, so
 * mods/ needs no syscall hook.
 *
 * Mechanism
 * ---------
 * hui::HuiIndicatorAbs is a lamp. It keeps an UpdateBehavior at +0x10 and its
 * update tail-calls that behaviour's: SingleColor for an on/off lamp, MultiColor
 * for a coloured one such as the hot cue pads.
 *
 * MultiColor::update (vtable slot 2) asks the source at +0x08 for a
 * packed u64 through its vtable +0x18, unpacks it, and passes the result to the
 * write function every lamp in the app goes through.
 *
 *   packed  byte 0     the LED index -- which lamp
 *           bytes 4-6  R, G, B
 *
 * So one wrapper sees every coloured lamp the app draws.
 *
 * The wrapper replaces that update instead of running after it, because the
 * write is a change-notifier: a second write with a different colour would look
 * to listeners like two changes. The stock body is reproduced exactly, and any
 * failed read falls back to calling it.
 *
 * The pad-to-ordinal mapping is fixed here (ORD_HOTCUE_A), not read from the
 * app. The first colour each pad lamp takes is logged with its ordinal.
 */
#include "cue/cue.h"
#include "lamp/lamp.h"       /* what a pad should look like, and whose it is */
#include "kit/mod.h"

/* MultiColor's own layout. */
#define MC_SOURCE_OFF      0x08   /* the thing that knows this lamp's colour */
#define MC_COLOUR_SLOT     0x18   /* ...asked through its vtable            */

/* indicator_control::IndicatorWithIdentifier<DeckIndicatorKind>, built 35 to a
 * loop by DeckIndicatorController: 0x70 bytes, vtable at +0x00 and the loop
 * index -- the ordinal -- at +0x68. */
#define SRC_ORDINAL_OFF    0x68

#define VT_IND_DECK_ID     ep122_sym(EP122_IND_DECK_ID)

/* Ordinals. There are 35 deck indicators and the eight hot cue pads are
 * consecutive from this one. Use the ordinal the source was constructed with,
 * not the position in the holder array, which is offset by one. */
#define ORD_HOTCUE_A       4

/* The packed u64 that comes back. */
#define PACK_INDEX(v)      ((unsigned)((v) & 0xffu))
#define PACK_R(v)          ((uint8_t)(((v) >> 32) & 0xffu))
#define PACK_G(v)          ((uint8_t)(((v) >> 40) & 0xffu))
#define PACK_B(v)          ((uint8_t)(((v) >> 48) & 0xffu))

#define FN_LED_WRITE       ep122_sym(EP122_HUI_LED_WRITE)

typedef int64_t  (*mc_update_fn_t)(void *self, void *dst);
typedef int64_t  (*led_write_fn_t)(void *dst, uint32_t index, const void *rgb);
typedef uint64_t (*colour_fn_t)(void *src);

static uintptr_t led_g_orig;

/* ---- which lamp is which pad -------------------------------------------
 *
 * Read from the colour source. Its vtable identifies the family (media, browse
 * and other lamps have a different one and pass through untouched) and +0x68
 * holds its ordinal. Only deck indicator sources can be pads. */
static int led_pad_of(uintptr_t src, uintptr_t vt)
{
    int32_t ordinal = 0;

    if (!VT_IND_DECK_ID || vt != VT_IND_DECK_ID ||
        mod_safe_read(src + SRC_ORDINAL_OFF, &ordinal, sizeof(ordinal)) != 0)
        return -1;
    ordinal -= ORD_HOTCUE_A;
    return (ordinal >= 0 && ordinal < CUE_PADS) ? (int)ordinal : -1;
}

/* A lamp has several colour slots and holds one at a time: the write function
 * stores the index at +0xc0 and the colour at +0xc1, and runs the pair through
 * a transform at +0xb0 whose result goes to the panel. The index selects how
 * the colour is rendered; it does not composite.
 *
 * The index is brightness and slot 2 is lit. The app writes
 * slot 2 for a pad that holds a cue and slot 1 otherwise, and 255 reaches the
 * panel as 0x7f through slot 2 and 0x0c through slot 1. A pad we colour is
 * written on slot 2 whatever the app asked for; otherwise empty pads would show
 * our colour at about a twentieth of the brightness. */
#define LED_PAD_SLOT      2
#define LED_PAD_DIM_SLOT  1

/* Each pad's lamp holder and the app's last write to it.
 *
 * The app writes a lamp only when its own state changes, and opening the stems
 * row is not such a change, so we repaint from the holder ourselves.
 *
 * The app's write is kept whole (colour and index) so restoring a pad replays
 * it verbatim. */
static struct {
    uintptr_t holder;
    uint8_t   app[3];
    uint32_t  app_index;
    uint8_t   known;
    uint8_t   ours;      /* we coloured it last, so we owe it a restore */
} led_g_pad_hw[CUE_PADS];

/* Converts a lamp value: fills the triple and returns the slot.
 *
 * LAMP_OFF writes black. The panel scales a triple by its own maximum, so any
 * hue on the dim slot renders as LAMP_DIM; only black on the dim slot is
 * dark. */
static unsigned led_wire(const struct lamp *l, uint8_t *rgb)
{
    if (l->level == LAMP_OFF) {
        rgb[0] = rgb[1] = rgb[2] = 0;
        return LED_PAD_DIM_SLOT;
    }
    rgb[0] = l->rgb[0];
    rgb[1] = l->rgb[1];
    rgb[2] = l->rgb[2];
    return l->level == LAMP_LIT ? LED_PAD_SLOT : LED_PAD_DIM_SLOT;
}

/* Signals lamp/ once all eight pads have written at least once: the hook is
 * in, the ordinals are mapped and every holder is known. A successful install
 * only means the slots were patched. */
static void led_ready(void)
{
    int p;

    for (p = 0; p < CUE_PADS; p++)
        if (!led_g_pad_hw[p].known)
            return;
    lamp_panel_ready();
}

/* Repaint the pads when lamp_word() changes.
 *
 * Driven from the display timer, because the app writes a lamp only when its
 * own state changes, and nothing lamp/ decides (a running groove, a sounding
 * sample, the startup sweep) is such a change. The stems row's progress bar chains the app's
 * refresh timer for the same reason.
 *
 * Only on a change, since each write notifies listeners. It calls the write
 * function directly, not the hooked update, so there is no re-entry. */
void cue_led_tick(void)
{
    static uint32_t last_word;
    static int      have_last;
    uint32_t        word = lamp_word();
    int             p;

    if (have_last && word == last_word)
        return;
    have_last = 1;
    last_word = word;

    for (p = 0; p < CUE_PADS; p++) {
        struct lamp l;
        uint8_t     rgb[4];

        if (!led_g_pad_hw[p].known)
            continue;

        if (!lamp_pad(p, &l)) {
            /* No longer ours: restore the app's colour and index once. Reached
             * when the row closes, the stick is ejected, the stems go away or
             * the sweep finishes; the app would not repaint the lamp itself. */
            if (led_g_pad_hw[p].ours) {
                led_g_pad_hw[p].ours = 0;
                rgb[0] = led_g_pad_hw[p].app[0];
                rgb[1] = led_g_pad_hw[p].app[1];
                rgb[2] = led_g_pad_hw[p].app[2];
                rgb[3] = 0;
                ((led_write_fn_t)FN_LED_WRITE)((void *)led_g_pad_hw[p].holder,
                                               led_g_pad_hw[p].app_index, rgb);
            }
            continue;
        }
        rgb[3] = 0;
        led_g_pad_hw[p].ours = 1;
        ((led_write_fn_t)FN_LED_WRITE)((void *)led_g_pad_hw[p].holder,
                                       led_wire(&l, rgb), rgb);
    }
}

/* ---- the hook ------------------------------------------------------------ */

static int64_t led_wrap_update(void *self, void *dst)
{
    uintptr_t src = 0, vt = 0, fn = 0;
    uint64_t packed;
    uint8_t rgb[4];
    unsigned idx;

    if (!FN_LED_WRITE ||
        mod_safe_read((uintptr_t)self + MC_SOURCE_OFF, &src, sizeof(src)) != 0 ||
        !src ||
        mod_safe_read(src, &vt, sizeof(vt)) != 0 ||
        mod_safe_read(vt + MC_COLOUR_SLOT, &fn, sizeof(fn)) != 0 || !fn)
        return ((mc_update_fn_t)led_g_orig)(self, dst);

    /* Asked once, as in the stock body. */
    packed = ((colour_fn_t)fn)((void *)src);
    idx    = PACK_INDEX(packed);
    rgb[0] = PACK_R(packed);
    rgb[1] = PACK_G(packed);
    rgb[2] = PACK_B(packed);
    rgb[3] = 0;

    {
        int pad = led_pad_of(src, vt);

        if (pad >= 0) {
            /* The holder is the lamp, so take it from a write on either slot;
             * the app writes slot 2 only for a pad that holds a cue. */
            if (!led_g_pad_hw[pad].known)
                MDBG("led: pad %c is ordinal %d, lamp %#lx\n", 'A' + pad,
                     pad + ORD_HOTCUE_A, (unsigned long)dst);
            led_g_pad_hw[pad].holder = (uintptr_t)dst;
            led_g_pad_hw[pad].known  = 1;
            /* Keep the whole write, whatever its index, for restoring. */
            led_g_pad_hw[pad].app[0]    = rgb[0];
            led_g_pad_hw[pad].app[1]    = rgb[1];
            led_g_pad_hw[pad].app[2]    = rgb[2];
            led_g_pad_hw[pad].app_index = idx;

            /* The startup sweep waits for every holder to be known. */
            led_ready();

            {
                struct lamp l;

                if (lamp_pad(pad, &l)) {
                    /* Use our slot, not the app's. The app keeps writing an
                     * empty pad on the dim slot, which would undo our brightness
                     * between repaints. */
                    idx = led_wire(&l, rgb);
                    led_g_pad_hw[pad].ours = 1;
                }
            }
        }
    }

    return ((led_write_fn_t)FN_LED_WRITE)(dst, (uint32_t)idx, rgb);
}

static int led_install(void)
{
    if (!FN_LED_WRITE) {
        MDBG("led: no write function -> lamps left alone\n");
        return -1;
    }
    if (mod_patch_vslot("huiMultiColor", EP122_HUI_MULTICOLOR, 0x10,
                        (void *)led_wrap_update, &led_g_orig) != 0)
        return -1;
    MDBG("led: coloured lamps intercepted\n");
    return 0;
}

KIT_MOD(k_mod_cue_led,
        .name = "cue_led", .prio = 6, .install = led_install,
        .what = "the panel's coloured lamps, decided in the app");
