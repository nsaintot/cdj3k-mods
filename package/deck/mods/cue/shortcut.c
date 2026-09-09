// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * cue/shortcut.c - the GATE CUE shortcut on the play screen's bottom rack.
 *
 * The same flag as the MOD SETTINGS row, with a thumb on it: lettering on the
 * accent while the gate is armed, dim lettering on the deck's unlit-button grey
 * while it is not.
 *
 * The rack is gui::NormalPlayerInfoWidget. The gui::PlayerInfo*Widget classes
 * are UpdaterComponent models whose juce::Component is a secondary subobject,
 * so the rack and its neighbours are found by typeinfo (see juce.h). The plate
 * sits in the gap between the timer and the tempo, both read off the live
 * components so a firmware that moves them moves the plate. On 3.19, in rack
 * coordinates: timer {395,13,327,80}, tempo {839,13,190,83}.
 */
#include "cue/cue.h"
#include "juce/juce.h"
#include "core/mod_settings.h"
#include "theme/theme.h"
#include "kit/mod.h"

/* Typeinfo of the three widgets; see juce.h for why not the vtable. */
#define RACK_TI   juce_class_of(ep122_sym(EP122_PLAYERINFO_RACK))
#define TIME_TI   juce_class_of(ep122_sym(EP122_PLAYERINFO_TIME))
#define TEMPO_TI  juce_class_of(ep122_sym(EP122_PLAYERINFO_TEMPO))

/* Insets from the neighbours' boxes. Unequal because their ink is not at their
 * box edges: the timer's last digit stops 1px short, the tempo's +/- sign (WIDE
 * only) sits 5px inside. Gives 12px to the digits and 9px to the sign; on 3.19
 * the plate lands at x=733, w=102. */
#define BTN_INSET_L   11
#define BTN_INSET_R   4

/* Plate 32px: the rack's badges are 28 but are read, not pressed. The label is
 * BTN_HIT_H tall with the plate centred in it; the extra rows are transparent
 * and only widen the touch target. Centred on the timer's box plus BTN_DROP,
 * which puts the plate's centre on the badges' row (y=515 on 3.19). */
#define BTN_PLATE_H   32
#define BTN_HIT_H     36
#define BTN_DROP      3
#define BTN_MIN_W     70

/* 15 draws 13px caps, AUTO CUE's height. "GATE CUE" is 76px of the 92px the
 * Label leaves inside 102, and it starts to squash around 17.5. */
#define BTN_FONT      15.0f
#define BTN_TEXT      "GATE CUE"

static uintptr_t g_btn;
static uintptr_t g_rack;
static uintptr_t g_vptr;
static uintptr_t g_vt[VT_CLONE_WORDS];
static int       g_shown;               /* what the button is currently painted as */
static unsigned  g_ink_gen;             /* the theme the label's lettering was set under */

/* Fills: accent when lit, `surface` (the title-bar touch plates' grey) when
 * not, both read from mod_ui() at paint time. Ink: `text_lit`, which follows
 * the accent's polarity under a theme, or `text_dim`. The ink lives on the
 * label and is restamped when the theme generation moves (cf. xpad_ink_sync). */
static void cue_shortcut_paint_state(void)
{
    const struct theme_ui *ui = mod_ui();
    int on = g_gate_on ? 1 : 0;

    if (!g_btn) return;
    juce_comp_colour(g_btn, LBL_COL_TEXT, on ? ui->text_lit : ui->text_dim);
    g_shown   = on;
    g_ink_gen = mod_ui_gen();
}

/* The plate first, then Label::paint for the lettering (the label's own
 * background is transparent). Bracketed so the stored ink is not re-themed. */
static void cue_shortcut_paint(void *self, void *g)
{
    const struct theme_ui *ui = mod_ui();
    int32_t b[4];

    if (mod_ui_gen() != g_ink_gen)
        cue_shortcut_paint_state();
    if (juce_comp_bounds((uintptr_t)self, b) == 0) {
        mod_gfx_colour(g, g_gate_on ? ui->accent : ui->surface);
        mod_gfx_fill(g, 0, (b[3] - BTN_PLATE_H) / 2, b[2], BTN_PLATE_H);
    }
    mod_draw_enter();
    ((void (*)(void *, void *))LABEL_FN_PAINT)(self, g);
    mod_draw_leave();
}

/* Toggles the flag the MOD SETTINGS row owns, and persists it. */
static void cue_shortcut_mousedown(void *self, void *event)
{
    (void)self; (void)event;
    g_gate_on = !g_gate_on;
    cue_shortcut_paint_state();
    mods_settings_save();
    MDBG("cue_shortcut: GATE CUE -> %s (shortcut)\n", g_gate_on ? "ON" : "OFF");
}

/* Built once the rack has both neighbours; until then the next repaint retries. */
static void cue_shortcut_build(uintptr_t rack)
{
    static const struct juce_vt_override ov[] = {
        { JUCE_VT_MOUSEDOWN, (void *)cue_shortcut_mousedown, NULL },
        { JUCE_VT_PAINT,     (void *)cue_shortcut_paint,     NULL },
    };
    uintptr_t time_w, tempo_w;
    int32_t tb[4], pb[4];
    int x, y, w;

    time_w  = juce_comp_child_of_class(rack, TIME_TI);
    tempo_w = juce_comp_child_of_class(rack, TEMPO_TI);
    if (!time_w || !tempo_w) {
        MDBG("cue_shortcut: rack %#lx has timer=%#lx tempo=%#lx -> not ready\n",
             (unsigned long)rack, (unsigned long)time_w, (unsigned long)tempo_w);
        return;
    }
    if (juce_comp_bounds(time_w, tb) != 0 || juce_comp_bounds(tempo_w, pb) != 0)
        return;

    x = tb[0] + tb[2] + BTN_INSET_L;
    w = pb[0] - x - BTN_INSET_R;
    y = tb[1] + (tb[3] - BTN_PLATE_H) / 2 + BTN_DROP - (BTN_HIT_H - BTN_PLATE_H) / 2;
    if (w < BTN_MIN_W) {
        MDBG("cue_shortcut: only %dpx between the timer and the tempo -> no shortcut\n", w);
        g_rack = rack;                  /* remembered, so this is not retried per frame */
        return;
    }

    if (!g_vptr) {
        g_vptr = juce_label_vt_clone(g_vt, ov, (int)(sizeof(ov) / sizeof(ov[0])));
        if (!g_vptr) return;
    }
    g_btn = juce_label(rack, BTN_TEXT, BTN_FONT, 0x00000000u, mod_ui()->text_dim,
                       g_vptr, x, y, w, BTN_HIT_H);
    if (!g_btn) return;
    g_rack = rack;
    cue_shortcut_paint_state();
    MDBG("cue_shortcut: GATE CUE shortcut at {%d,%d,%d,%d} in rack %#lx, plate %dpx\n",
         x, y, w, BTN_HIT_H, (unsigned long)rack, BTN_PLATE_H);
}

/* Anchored on the waveform title bar's TouchAria paint, like the STEMS row: it
 * fires once the play screen is wired up. The rack's own paint is unusable
 * because its Component vtable shares a group with the listener vtable. */
static uintptr_t g_orig_ta_paint;

static void cue_shortcut_ta_paint(void *self, void *g)
{
    if (g_orig_ta_paint)
        ((void (*)(void *, void *))g_orig_ta_paint)(self, g);

    if (!g_rack) {
        uintptr_t rack = juce_comp_find_class(juce_comp_root((uintptr_t)self), RACK_TI);

        if (rack) cue_shortcut_build(rack);
    }
}

/* Called by the MOD SETTINGS row's `changed`, so both views of the flag agree. */
void cue_shortcut_refresh(void)
{
    if (g_btn && g_shown != (g_gate_on ? 1 : 0))
        cue_shortcut_paint_state();
}

static int cue_shortcut_install(void)
{
    if (!FN_LABEL_CTOR || !FN_ADD_VISIBLE || !FN_SET_BOUNDS || !FN_FONT_BUILD ||
        !FN_LABEL_SETFONT || !FN_LABEL_JUSTIFY || !FN_COMP_SETCOLOUR) {
        MDBG("cue_shortcut: juce primitives did not resolve -> no shortcut\n");
        return -1;
    }
    if (!RACK_TI || !TIME_TI || !TEMPO_TI) {
        MDBG("cue_shortcut: player-info rack did not resolve -> no shortcut\n");
        return -1;
    }
    if (mod_patch_vslot("cueShortcutPaint", EP122_TOUCHARIA, JUCE_VT_PAINT,
                        (void *)cue_shortcut_ta_paint, &g_orig_ta_paint) != 0) {
        MDBG("cue_shortcut: no anchor -> no shortcut\n");
        return -1;
    }
    MDBG("cue_shortcut: installed (waiting for the play screen to draw)\n");
    return 0;
}

KIT_MOD(k_mod_cue_shortcut,
        .name = "cue_shortcut", .prio = 11, .install = cue_shortcut_install,
        .what = "GATE CUE shortcut on the player-info rack");
