// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * ui/wedge.c - the three faders and the STEMS switch, drawn and dragged
 *
 * Part of the STEMS play-screen UI. The shared contract, and the reasoning
 * behind the design, is in ui.h.
 */
#include "stem/ui/ui.h"

/* ================================================================== */
/* The fader                                                          */
/* ================================================================== */

/* One Component per stem, painted by us: the name, the value, the wedge and the cap.
 * The level is a plain int. A lane is the cell holding a part's MUTE key and fader.
 *
 * The fader is a Label with a second cloned vtable (paint and the three mouse slots),
 * so the Label ctor does the allocation and Component setup; Label's paint is never
 * reached. */
uintptr_t stems_g_wedge[N_STEMS];
static int32_t   g_wedge_w[N_STEMS];    /* the drawn width: the wedge is laid out in it */
int       stems_g_level[N_STEMS] = { 100, 100, 100 };   /* percent, 0..STEM_LEVEL_MAX */
/* MUTE per stem, from the MUTE key or the pad. Kept separate from the level so
 * unmuting restores the fader's value. */
int       stems_g_mute[N_STEMS];

/* The audio side's half of the mute; see stem.h for which thread owns which. */
int       g_stem_mute_want[N_STEMS];
int       g_stem_mute_live[N_STEMS];
unsigned  g_stem_mute_commits;
float     g_stem_gain_unmuted[N_STEMS] = { 1.0f, 1.0f, 1.0f };

static uintptr_t g_wedge_vt[2 + VT_CLONE_SLOTS];
static uintptr_t g_wedge_vptr;
/* ---- one gesture, one control ------------------------------------------------------
 *
 * Whoever takes the gesture at mouseDown owns every control in the row until the finger
 * lifts, so a sweep that starts on a fader cannot mute a part through its MUTE key or
 * flip the STEMS switch. A control that did not start the gesture does nothing,
 * including highlighting. */
uintptr_t stems_g_grab;        /* who owns the gesture right now */
uintptr_t stems_g_grab_last;   /* who owned it most recently */
static unsigned  g_grab_freed;  /* the display tick it was let go on */
unsigned  stems_g_ticks;       /* display ticks; exists only for the cooldown */

/* After a mouseUp, another fader cannot take the grab for GRAB_COOLDOWN ticks.
 *
 * A drag across a boundary arrives as an up on the fader being left and a down on the
 * next, microseconds apart, with no event to tell that from a new touch, so the
 * distinction is made by time; without it the next fader would take the rest of the
 * drag. Four ticks is about 90 ms: far longer than a crossing, too short to notice when
 * moving to another fader. Re-taking the same control is never delayed. */
#define GRAB_COOLDOWN 4

static int stems_wedge_index(uintptr_t comp);

int stems_grab_take(uintptr_t self)
{
    if (stems_g_grab)
        return stems_g_grab == self;
    /* The cooldown applies only to faders; on buttons it would drop presses. */
    if (stems_wedge_index(self) >= 0 &&
        stems_g_grab_last && stems_g_grab_last != self &&
        (unsigned)(stems_g_ticks - g_grab_freed) < GRAB_COOLDOWN)
        return 0;
    stems_g_grab = self;
    stems_g_grab_last = self;
    return 1;
}

void stems_grab_release(void)
{
    if (!stems_g_grab) return;
    g_grab_freed = stems_g_ticks;
    stems_g_grab = 0;
}

void stems_progress_poll(void);
int  stems_available(void);


/* Invalidate a whole component. The rect is passed by pointer; see FN_COMP_REPAINT. */
void stems_repaint(uintptr_t comp)
{
    int32_t r[4];

    if (!comp || stems_bounds(comp, r) != 0) return;
    r[0] = 0;
    r[1] = 0;
    ((void (*)(void *, const int32_t *))FN_COMP_REPAINT)((void *)comp, r);
}
/* How far a component is pressed while it holds the gesture, as a Q8 for the draw
 * kit; zero at rest.
 *
 * It presses the colour the button is changing to: mouseDown changes the state before
 * the coalesced paint runs, so pressing an unlit button flashes a pressed accent and
 * pressing a lit one a pressed grey.
 *
 * Returns the amount, not a colour: the checker's two halves are pressed independently
 * (see mod_checker_press). */
uint32_t stems_touch_lift(uintptr_t comp)
{
    return comp && stems_g_grab == comp ? MOD_CHECKER_HOT_Q8 : 0;
}

/* State -> role, resolved at paint so a theme change needs no invalidation. */
uint32_t stems_btn_surface(void)
{
    const struct theme_ui *ui = mod_ui();

    return stems_g_btn_state == BTN_ON     ? ui->accent
         : stems_g_btn_state == BTN_REFUSE ? ui->refuse
                                     : ui->surface;
}

/* Repaints only on a change; called from the display tick's warn blink. */
void stems_btn_state(enum btn_state st)
{
    if (stems_g_btn_state == st) return;
    stems_g_btn_state = st;
    stems_repaint(stems_g_btn_stems);
}


/* Which stem a component is, or -1. */
static int stems_wedge_index(uintptr_t comp)
{
    int i;

    for (i = 0; i < N_STEMS; i++)
        if (comp && stems_g_wedge[i] == comp) return i;
    return -1;
}

/* Publish a stem's level and mute to the audio thread. The gain array is the audio
 * side's only view of the row and is written only here. */
void stems_publish_gain(int i)
{
    float open_gain = (float)stems_g_level[i] / (float)STEM_LEVEL_MAX;

    if (i < 0 || i >= N_STEMS) return;
    /* The fader's own gain, for the audio thread to restore after a mute. */
    __atomic_store(&g_stem_gain_unmuted[i], &open_gain, __ATOMIC_RELAXED);
    __atomic_store_n(&g_stem_mute_want[i], stems_g_mute[i], __ATOMIC_RELAXED);

    /* The level applies immediately; only the mute waits for the beat boundary.
     * The gain published here follows the mute currently applied, and the audio
     * side's commit changes it at the boundary. */
    stem_gain_set(i, __atomic_load_n(&g_stem_mute_live[i], __ATOMIC_RELAXED)
                     ? 0.0f : open_gain);
}

void stems_level_set(int i, int pct)
{
    if (i < 0 || i >= N_STEMS) return;
    if (pct < 0) pct = 0;
    else if (pct > STEM_LEVEL_MAX) pct = STEM_LEVEL_MAX;
    if (pct == stems_g_level[i]) return;
    stems_g_level[i] = pct;
    stems_publish_gain(i);
    stems_repaint(stems_g_wedge[i]);
}

/* Is a stem set resident right now? The row can be open with none (failed probe, no
 * server, job never started); the controls go inert then. */
int stems_ready(void)
{
    return __atomic_load_n(&g_stem_ready, __ATOMIC_ACQUIRE) != 0;
}

/* Out of circuit: no stems resident, or BYPASS on. The faders and MUTE keys draw at
 * the stock 30 % and take no input. A muted part stays live: its cap still moves. */
int stems_row_inert(void)
{
    return !stems_ready() || stems_g_bypass_on;
}

/* ---- drawing helpers, shared with the MUTE keys (row.c) ---------------------------- */

/* The stock disabled art: the colour at 30 % over the panel's ground, so it fades toward
 * black on a dark theme and toward the paper on a light one. */
uint32_t stems_dim(uint32_t argb, int dim)
{
    uint32_t ground, out = argb & 0xff000000u;
    int sh;

    if (!dim) return argb;
    ground = mod_colour_stock(COL_SKIN_BLACK);
    for (sh = 0; sh <= 16; sh += 8) {
        uint32_t c = (argb >> sh) & 0xffu, b = (ground >> sh) & 0xffu;

        out |= ((c * LANE_DISABLED_Q8 + b * (256u - LANE_DISABLED_Q8) + 128u) >> 8) << sh;
    }
    return out;
}

/* The stock 2 px inner outline. */
void stems_frame2(void *g, int x, int y, int w, int h, uint32_t argb)
{
    if (w < 4 || h < 4) return;
    mod_gfx_colour(g, argb);
    mod_gfx_fill(g, x, y, w, 2);
    mod_gfx_fill(g, x, y + h - 2, w, 2);
    mod_gfx_fill(g, x, y, 2, h);
    mod_gfx_fill(g, x + w - 2, y, 2, h);
}

/* ---- the wedge's geometry -----------------------------------------------------------
 *
 * Bars every LANE_BAR_PITCH px, centred in the fader; the cap travels inside them, so
 * the wedge's ends are the fader's visible ends at 0 and 100 %. */

static int lane_bars(int w)
{
    int n = (w + LANE_BAR_GAP) / LANE_BAR_PITCH;

    return n < 2 ? 2 : n;
}

static int lane_span(int n)
{
    return n * LANE_BAR_PITCH - LANE_BAR_GAP;
}

/* The first bar's left edge. */
static int lane_x0(int w, int n)
{
    return (w - lane_span(n)) / 2;
}

/* The cap's centre for a level: it travels from half a cap in to half a cap short of
 * each end. */
static int lane_cap_x(int x0, int span, int pct)
{
    return x0 + LANE_CAP_W / 2 +
           (pct * (span - LANE_CAP_W) + STEM_LEVEL_MAX / 2) / STEM_LEVEL_MAX;
}

/* Left and right edges of the wedge in a fader `w` px wide. stems_lanes_place splits
 * the gaps between controls at these edges. */
void stems_lane_ink(int w, int *l, int *r)
{
    int n = lane_bars(w);

    *l = lane_x0(w, n);
    *r = lane_x0(w, n) + lane_span(n);
}

/* Widths of the names at FONT_LANE_NAME in the deck's face, so the glyph and the MUTE
 * tag can follow the name. There is no text measurement in the kit. */
static const int k_lane_name_w[N_STEMS] = LANE_NAME_W;

/* ---- the gesture --------------------------------------------------------------------
 *
 * A touch anywhere on the fader grabs the cap where it is and changes nothing. A drag
 * moves the level by the distance dragged, so a stray touch or a palm cannot set a
 * level. A drag step longer than LANE_JUMP_PX is not a finger (the panel reports one
 * averaged point for two contacts, and a lift can report far away): the anchor moves
 * there and the level stays. A drag past either end re-anchors at the end, so moving
 * back moves the level at once. */
static int g_drag_x0;       /* the anchor, in the fader's pixels */
static int g_drag_lvl0;     /* the level at the anchor */
static int g_drag_last;     /* the previous event's x */
static int g_drag_from;     /* the level when the grab began, for the log */

static int lane_event_x(void *event)
{
    float pos[2];

    memcpy(pos, event, sizeof(pos));
    return (int)pos[0];
}

/* Paint one fader:
 *
 *   name [glyph] [MUTE]                                55%
 *                                       .:|||||[cap]
 *                          .:|||||||||||||||||||||||||
 *
 * The wedge's bars rise from LANE_WEDGE_FLOOR to LANE_WEDGE_H; both ends and the thirds
 * are scale marks, a pixel wider and lighter. A bar is lit when its centre is left of
 * the cap's index line. The cap is OFF art at rest and ON art while held. The name and
 * value line spans the wedge exactly. */
static void stems_wedge_paint(void *self, void *g)
{
    const struct theme_ui *ui = mod_ui();
    int32_t b[4];
    int i, k, w, n, x0, span, cx, dim, muted, held, lvl, nx, right, ox, m1, m2;
    uint32_t grey, stem, val_c, a;
    char num[8];

    i = stems_wedge_index((uintptr_t)self);
    if (i < 0 || stems_bounds((uintptr_t)self, b) != 0) return;
    stems_hit_get((uintptr_t)self, b[2], &ox, &w);
    if (w < 2 * LANE_CAP_W || b[3] < LANE_H) return;

    dim   = stems_row_inert();
    muted = stems_g_mute[i];
    held  = !dim && stems_g_grab == (uintptr_t)self;
    lvl   = stems_g_level[i];
    grey  = stems_dim(mod_colour_stock(COL_SKIN_GREY), dim);
    stem  = stems_dim(ui->stem[i], dim);

    n     = lane_bars(w);
    span  = lane_span(n);
    x0    = ox + lane_x0(w, n);
    cx    = lane_cap_x(x0, span, lvl);
    right = x0 + span;

    /* The header: name, then the wave-wait glyph, then the MUTE tag. */
    nx = x0;
    mod_gfx_text(g, k_stem_name[i], FONT_LANE_NAME, muted ? grey : stem,
                 nx, LANE_HEAD_Y + LANE_NAME_DY, span / 2, LANE_HEAD_H, JUCE_JUSTIFY_MID_L);
    nx += k_lane_name_w[i] + LANE_HEAD_GAP;
    a = stems_wavewait_level();
    if (a) {
        stems_wavewait_draw(g, nx, LANE_HEAD_Y + (LANE_HEAD_H - WAVEWAIT_H) / 2, a);
        nx += WAVEWAIT_W + LANE_HEAD_GAP;
    }
    /* With the MUTE keys folded away, the tag is the screen's only sign of a mute. It
     * blinks on the mute blink: filled in the part's colour with text_on_accent
     * lettering, then outlined with lettering in the colour. */
    if (muted && g_stem_mute_pads) {
        int ty = LANE_NAME_MID - LANE_TAG_H / 2;
        int fill = stems_g_mute_lit;

        if (fill) {
            mod_gfx_colour(g, stem);
            mod_gfx_fill(g, nx, ty, LANE_TAG_W, LANE_TAG_H);
        } else
            stems_frame2(g, nx, ty, LANE_TAG_W, LANE_TAG_H, stem);
        mod_gfx_text(g, "MUTE", FONT_LANE_TAG, fill ? stems_dim(ui->text_on_accent, dim) : stem,
                     nx, ty, LANE_TAG_W, LANE_TAG_H, JUCE_JUSTIFY_CENTRED);
    }

    /* The value, flush with the wedge's right end: figures and a smaller %. */
    val_c = muted ? grey : stems_dim(ui->text, dim);
    snprintf(num, sizeof(num), "%d", lvl);
    mod_gfx_text(g, num, FONT_LANE_VALUE, val_c, right - LANE_PCT_W - LANE_PCT_GAP - LANE_VALUE_W,
                 LANE_HEAD_Y, LANE_VALUE_W, LANE_HEAD_H, JUCE_JUSTIFY_MID_R);
    mod_gfx_text(g, "%", FONT_LANE_PCT, val_c, right - LANE_PCT_W,
                 LANE_HEAD_Y, LANE_PCT_W, LANE_HEAD_H - LANE_PCT_DROP, JUCE_JUSTIFY_BOT_R);

    /* Scale marks at both ends and the thirds. */
    m1 = n / 3;
    m2 = (2 * (n - 1) + 1) / 3;
    for (k = 0; k < n; k++) {
        int bx   = x0 + k * LANE_BAR_PITCH;
        int bh   = LANE_WEDGE_FLOOR +
                   (k * (LANE_WEDGE_H - LANE_WEDGE_FLOOR) + (n - 1) / 2) / (n - 1);
        int on   = 2 * bx + LANE_BAR_W < 2 * cx;
        int mark = k == 0 || k == m1 || k == m2 || k == n - 1;
        uint32_t c;

        if (mark)
            c = on ? (muted ? stems_dim(mod_colour_stock(COL_SKIN_MARK_OFF), dim)
                            : stems_dim(mod_colour_press(ui->stem[i], LANE_MARK_Q8), dim))
                   : stems_dim(ui->tick, dim);
        else
            c = on ? (muted ? grey : stem) : stems_dim(ui->surface, dim);
        mod_gfx_colour(g, c);
        mod_gfx_fill(g, bx, LANE_WEDGE_Y + LANE_WEDGE_H - bh,
                     mark ? LANE_BAR_W : LANE_BAR_THIN, bh);
    }

    {
        int capx = cx - LANE_CAP_W / 2;

        if (held)
            mod_checker_pair(g, capx, LANE_CAP_Y, LANE_CAP_W, LANE_CAP_H,
                             mod_colour_stock(COL_SKIN_GREY), mod_colour_stock(COL_SKIN_ON2));
        else
            mod_checker_pair(g, capx, LANE_CAP_Y, LANE_CAP_W, LANE_CAP_H,
                             stems_dim(ui->surface, dim),
                             stems_dim(mod_colour_stock(COL_SKIN_DARK), dim));
        stems_frame2(g, capx, LANE_CAP_Y, LANE_CAP_W, LANE_CAP_H,
                     held ? ui->text : grey);
        mod_gfx_colour(g, stems_dim(ui->text, dim));
        mod_gfx_fill(g, capx + (LANE_CAP_W - LANE_CAP_LINE_W) / 2, LANE_CAP_Y + LANE_CAP_LINE_Y,
                     LANE_CAP_LINE_W, LANE_CAP_H - 2 * LANE_CAP_LINE_Y);
    }
}

static void stems_wedge_down(void *self, void *event)
{
    int i = stems_wedge_index((uintptr_t)self);

    if (i < 0 || !event || stems_row_inert())
        return;                         /* out of circuit: inert, not merely greyed */
    if (!stems_grab_take((uintptr_t)self)) return;
    g_drag_x0   = g_drag_last = lane_event_x(event);
    g_drag_lvl0 = g_drag_from = stems_g_level[i];
    stems_repaint((uintptr_t)self);     /* the ON cap */
}

static void stems_wedge_drag(void *self, void *event)
{
    int i = stems_wedge_index((uintptr_t)self);
    int x, dx, travel, lvl;

    if (i < 0 || !event || stems_g_grab != (uintptr_t)self || stems_row_inert()) return;
    x  = lane_event_x(event);
    dx = x - g_drag_last;
    g_drag_last = x;
    if (dx > LANE_JUMP_PX || dx < -LANE_JUMP_PX) {
        g_drag_x0   = x;
        g_drag_lvl0 = stems_g_level[i];
        MDBG("stems: %s drag jumped %dpx -> re-anchored, level kept\n", k_stem_name[i], dx);
        return;
    }
    travel = lane_span(lane_bars(g_wedge_w[i])) - LANE_CAP_W;
    if (travel <= 0) return;
    dx  = x - g_drag_x0;
    lvl = g_drag_lvl0 + (dx * STEM_LEVEL_MAX + (dx >= 0 ? travel : -travel) / 2) / travel;
    if (lvl < 0 || lvl > STEM_LEVEL_MAX) {
        lvl = lvl < 0 ? 0 : STEM_LEVEL_MAX;
        g_drag_x0   = x;
        g_drag_lvl0 = lvl;
    }
    stems_level_set(i, lvl);
}

static void stems_wedge_up(void *self, void *event)
{
    int i = stems_wedge_index((uintptr_t)self);
    int held = stems_g_grab == (uintptr_t)self;

    (void)event;
    if (held && i >= 0)
        MDBG("stems: %s = %d%% (from %d%%)\n", k_stem_name[i], stems_g_level[i], g_drag_from);
    /* Released on any up, not only the owner's, so a stuck grab cannot leave the row
     * unresponsive. */
    stems_grab_release();
    if (held) stems_repaint((uintptr_t)self);
}

/* Same clone-and-override as the button vtable, plus mouseDrag. The paint slot is
 * checked against Label's own paint before it is replaced. */
static int stems_wedge_vt_ready(void)
{
    if (g_wedge_vptr) return 1;
    if (mod_safe_read(LABEL_VTABLE - 2 * sizeof(uintptr_t),
                      g_wedge_vt, sizeof(g_wedge_vt)) != 0) return 0;
    if (g_wedge_vt[2 + VT_SLOT_PAINT / sizeof(uintptr_t)] != LABEL_FN_PAINT) {
        MERR("stems: label vtable paint slot holds %#lx, expected %#lx -> no faders\n",
             (unsigned long)g_wedge_vt[2 + VT_SLOT_PAINT / sizeof(uintptr_t)],
             (unsigned long)LABEL_FN_PAINT);
        return 0;
    }
    g_wedge_vt[2 + VT_SLOT_PAINT     / sizeof(uintptr_t)] = (uintptr_t)stems_wedge_paint;
    g_wedge_vt[2 + VT_SLOT_MOUSEDOWN / sizeof(uintptr_t)] = (uintptr_t)stems_wedge_down;
    g_wedge_vt[2 + VT_SLOT_MOUSEDRAG / sizeof(uintptr_t)] = (uintptr_t)stems_wedge_drag;
    g_wedge_vt[2 + VT_SLOT_MOUSEUP   / sizeof(uintptr_t)] = (uintptr_t)stems_wedge_up;
    g_wedge_vptr = (uintptr_t)&g_wedge_vt[2];
    MDBG("stems: cloned juce::Label vtable for faders -> %#lx\n",
         (unsigned long)g_wedge_vptr);
    return 1;
}

/* The STEMS switch: the stock BEAT JUMP 1 | 2 page switch, worded OFF | ON. ON is lit
 * (the skin grey block, black word) while the stems play; OFF is lit in the bypass
 * colour when they are switched off. Pressing it toggles BYPASS. Drawn at 30 % while
 * no stems are resident. */
static uintptr_t g_switch_vt[2 + VT_CLONE_SLOTS];
uintptr_t stems_g_switch_vptr;

static void switch_paint(void *self, void *g)
{
    const struct theme_ui *ui = mod_ui();
    int32_t b[4];
    int w, h, ox, y, off_lit, dim;
    uint32_t grey, block, lit_text, lift = stems_touch_lift((uintptr_t)self);

    if (stems_bounds((uintptr_t)self, b) != 0) return;
    stems_hit_get((uintptr_t)self, b[2], &ox, &w);
    h = b[3];
    if (w < SW_W || h < SW_H) return;

    dim    = !stems_ready();
    off_lit = stems_g_bypass_on;
    grey   = stems_dim(mod_colour_stock(COL_SKIN_GREY), dim);
    lit_text = stems_dim(mod_colour_stock(COL_SKIN_BLACK), dim);
    block  = off_lit ? ui->bypass : mod_colour_stock(COL_SKIN_GREY);
    block  = mod_colour_press(block, lift);
    block  = stems_dim(block, dim);
    ox    += (w - SW_W) / 2;
    y      = (h - SW_H) / 2;

    mod_gfx_colour(g, stems_dim(mod_colour_stock(COL_SKIN_ON2), dim));   /* the frame */
    mod_gfx_fill(g, ox, y, SW_W, SW_H);
    /* Stock sprite colours go through mod_colour_stock, so a light theme turns the
     * well white and the lit lettering white, as it does on the stock switch. */
    mod_gfx_colour(g, stems_dim(mod_colour_stock(COL_SKIN_BLACK), dim));
    mod_gfx_fill(g, ox + SW_FRAME, y + SW_FRAME, SW_W - 2 * SW_FRAME, SW_H - 2 * SW_FRAME);
    mod_gfx_colour(g, block);
    mod_gfx_fill(g, ox + (off_lit ? SW_INSET : SW_W - SW_INSET - SW_BLOCK), y + SW_INSET,
                 SW_BLOCK, SW_BLOCK);
    mod_gfx_text(g, "OFF", FONT_SWITCH, off_lit ? lit_text : grey,
                 ox + SW_INSET, y + SW_INSET, SW_BLOCK, SW_BLOCK, JUCE_JUSTIFY_CENTRED);
    mod_gfx_text(g, "ON", FONT_SWITCH, off_lit ? grey : lit_text,
                 ox + SW_W - SW_INSET - SW_BLOCK, y + SW_INSET, SW_BLOCK, SW_BLOCK,
                 JUCE_JUSTIFY_CENTRED);
}

/* The button clone with paint taken as well, so the press behaviour is shared with
 * STEMS and only the drawing differs. */
int stems_switch_vt_ready(void)
{
    if (stems_g_switch_vptr) return 1;
    if (!stems_label_vt_ready()) return 0;
    memcpy(g_switch_vt, stems_g_label_vt, sizeof(g_switch_vt));
    g_switch_vt[2 + VT_SLOT_PAINT / sizeof(uintptr_t)] = (uintptr_t)switch_paint;
    stems_g_switch_vptr = (uintptr_t)&g_switch_vt[2];
    return 1;
}

uintptr_t stems_wedge(uintptr_t parent, int x, int y, int w, int h, int stem)
{
    uintptr_t p;

    if (!stems_wedge_vt_ready()) return 0;
    /* Transparent, no text, no outline: everything shown comes from our paint. */
    p = stems_label(parent, "", FONT_CAPTION, 0x00000000u, mod_ui()->text, 0, x, y, w, h);
    if (!p) return 0;
    *(uintptr_t *)p = g_wedge_vptr;
    stems_g_wedge[stem] = p;
    g_wedge_w[stem] = w;
    stem_gain_set(stem, (float)stems_g_level[stem] / (float)STEM_LEVEL_MAX);
    return p;
}

/* Move a fader, when the MUTE keys fold away or come back. `x`/`w` are the touch
 * area; the fader draws `dw` wide at `dl` inside it. */
void stems_wedge_place(int stem, int x, int y, int w, int h, int dl, int dw)
{
    if (stem < 0 || stem >= N_STEMS || !stems_g_wedge[stem]) return;
    ((bounds_t)FN_SET_BOUNDS)((void *)stems_g_wedge[stem], x, y, w, h);
    stems_hit_set(stems_g_wedge[stem], dl, dw);
    g_wedge_w[stem] = dw;
    stems_repaint(stems_g_wedge[stem]);
}
