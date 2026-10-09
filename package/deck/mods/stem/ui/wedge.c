// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * ui/wedge.c - the three level controls, drawn and dragged
 *
 * Part of the STEMS play-screen UI. The shared contract, and the reasoning
 * behind the design, is in ui.h.
 */
#include "stem/ui/ui.h"

/* ================================================================== */
/* The wedge                                                          */
/* ================================================================== */

/* One Component per stem, painted by us: a ramp of bars, short at the left and full
 * height at the right, lit from the left up to the level. The level is a plain int.
 *
 * The wedge is a Label with a second cloned vtable (paint and the three mouse slots),
 * so the Label ctor does the allocation and Component setup; Label's paint is never
 * reached. */
uintptr_t stems_g_wedge[N_STEMS];
static int32_t   g_wedge_w[N_STEMS];    /* cached: the drag maps x onto this */
int       stems_g_level[N_STEMS] = { 100, 100, 100 };   /* percent, 0..STEM_LEVEL_MAX */
/* MUTE per stem, held via the caption above the wedge. Kept separate from the level
 * so releasing it restores the fader's value. */
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
 * lifts. A wedge jumps to wherever it is touched, so without this a sweep across one
 * fader would set its neighbours, mute a stem via its caption, or toggle BYPASS. A
 * control that did not start the gesture does nothing, including highlighting. */
uintptr_t stems_g_grab;        /* who owns the gesture right now */
uintptr_t stems_g_grab_last;   /* who owned it most recently */
static unsigned  g_grab_freed;  /* the display tick it was let go on */
unsigned  stems_g_ticks;       /* display ticks; exists only for the cooldown */

/* After a mouseUp, another wedge cannot take the grab for GRAB_COOLDOWN ticks.
 *
 * Dragging across a boundary produces an up on the control being left and a down on the
 * one entered, microseconds apart, with no event to tell that from a new touch, so the
 * distinction is made by time. Four ticks is about 90 ms: far longer than a crossing,
 * too short to notice when moving to another fader. Re-taking the same control is never
 * delayed. */
#define GRAB_COOLDOWN 4

static int stems_wedge_index(uintptr_t comp);

int stems_grab_take(uintptr_t self)
{
    if (stems_g_grab)
        return stems_g_grab == self;
    /* The cooldown applies only to wedges, which jump to the touch point; on buttons
     * it would drop presses. */
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
/* How much a component is lit while it holds the gesture, as a Q8 for the draw kit;
 * zero at rest. Both buttons and the bar use it.
 *
 * It lifts the colour the button is changing to: mouseDown changes the state before
 * the coalesced paint runs, so pressing an unlit button flashes a lightened accent and
 * pressing a lit one a lightened grey.
 *
 * Returns the lift, not a colour: the checker surface's two halves are lifted
 * independently (see mod_checker_lift). */
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


/* How many steps fit in width w (at least 2). */
static int wedge_steps(int w)
{
    int n = (w + WEDGE_BAR_GAP) / (WEDGE_BAR_W + WEDGE_BAR_GAP);

    return n < 2 ? 2 : n;
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

/* Is this stem out of the mix for any reason? The wedge greys and goes inert. */
static int stems_stem_off(int i)
{
    return !stems_ready() || stems_g_bypass_on || (i >= 0 && i < N_STEMS && stems_g_mute[i]);
}

/* Paint one wedge as n vertical bars, bottom-aligned and rising left to right, using
 * only setColour and fillRect. Bars up to the level are lit in the stem colour, so the
 * lit area grows with the value. */
static void stems_wedge_paint(void *self, void *g)
{
    int32_t b[4];
    int i, k, n, w, h, step, base, x0, lit;

    i = stems_wedge_index((uintptr_t)self);
    if (i < 0 || stems_bounds((uintptr_t)self, b) != 0) return;
    w = b[2];
    h = b[3];
    if (w <= 0 || h <= WEDGE_MIN_H) return;

    n = wedge_steps(w);
    /* The rise per bar is a whole number of pixels and the shortest bar absorbs the
     * remainder. A fractional rise truncates unevenly and shows a visible step every
     * few bars. */
    step = (h - WEDGE_MIN_H) / (n - 1);
    if (step < 1) step = 1;
    base = h - step * (n - 1);
    if (base < 1) base = 1;
    /* Rounded, not truncated, so the lit edge does not lag the finger by a step. */
    lit = (stems_g_level[i] * n + STEM_LEVEL_MAX / 2) / STEM_LEVEL_MAX;
    /* Centred, so the integer-division leftover is split between both ends. */
    x0 = (w - (n * WEDGE_BAR_W + (n - 1) * WEDGE_BAR_GAP)) / 2;

    /* Roles resolved once per paint, not per bar. */
    {
    const struct theme_ui *ui = mod_ui();
    int off = stems_stem_off(i);

    for (k = 0; k < n; k++) {
        int bh = base + step * k;
        int on = k < lit;
        /* Both ends are always marked; the modulo alone misses the last bar for most
         * counts. */
        int mark = (k % WEDGE_TICK) == 0 || k == n - 1;
        uint32_t col = on ? (off ? ui->text_off : ui->stem[i]) : ui->surface;

        /* A mark is brighter and a pixel wider; the pitch is unchanged. */
        if (mark)
            col = on ? stems_lighter(col) : ui->tick;

        mod_gfx_colour(g, col);
        mod_gfx_fill(g, x0 + k * (WEDGE_BAR_W + WEDGE_BAR_GAP), h - bh,
                       mark ? WEDGE_BAR_W : WEDGE_BAR_THIN, bh);
    }
    }
}

/* x within the component -> level. juce::MouseEvent begins with Point<float> position,
 * relative to the component handling the event.
 *
 * No filtering of jumps: the panel reports one averaged point for two contacts, so a
 * second finger moves the value, but a filter would also reject a fast sweep. */
static void stems_wedge_track(void *self, void *event)
{
    int i = stems_wedge_index((uintptr_t)self);
    float pos[2];

    if (i < 0 || !event || g_wedge_w[i] <= 0) return;
    memcpy(pos, event, sizeof(pos));
    stems_level_set(i, (int)((pos[0] * STEM_LEVEL_MAX) / (float)g_wedge_w[i] + 0.5f));
}

static void stems_wedge_down(void *self, void *event)
{
    if (stems_stem_off(stems_wedge_index((uintptr_t)self)))
        return;                         /* out of circuit: inert, not merely greyed */
    if (!stems_grab_take((uintptr_t)self)) return;
    stems_wedge_track(self, event);
}

static void stems_wedge_drag(void *self, void *event)
{
    if (stems_g_grab != (uintptr_t)self ||
        stems_stem_off(stems_wedge_index((uintptr_t)self))) return;
    stems_wedge_track(self, event);
}

static void stems_wedge_up(void *self, void *event)
{
    int i = stems_wedge_index((uintptr_t)self);

    (void)event;
    if (stems_g_grab == (uintptr_t)self && i >= 0)
        MDBG("stems: %s = %d%%\n", k_stem_name[i], stems_g_level[i]);
    /* Released on any up, not only the owner's, so a stuck grab cannot leave the row
     * unresponsive. */
    stems_grab_release();
}

/* Same clone-and-override as the button vtable, plus mouseDrag. The paint slot is
 * checked against Label's own paint before it is replaced. */
static int stems_wedge_vt_ready(void)
{
    if (g_wedge_vptr) return 1;
    if (mod_safe_read(LABEL_VTABLE - 2 * sizeof(uintptr_t),
                      g_wedge_vt, sizeof(g_wedge_vt)) != 0) return 0;
    if (g_wedge_vt[2 + VT_SLOT_PAINT / sizeof(uintptr_t)] != LABEL_FN_PAINT) {
        MERR("stems: label vtable paint slot holds %#lx, expected %#lx -> no wedges\n",
             (unsigned long)g_wedge_vt[2 + VT_SLOT_PAINT / sizeof(uintptr_t)],
             (unsigned long)LABEL_FN_PAINT);
        return 0;
    }
    g_wedge_vt[2 + VT_SLOT_PAINT     / sizeof(uintptr_t)] = (uintptr_t)stems_wedge_paint;
    g_wedge_vt[2 + VT_SLOT_MOUSEDOWN / sizeof(uintptr_t)] = (uintptr_t)stems_wedge_down;
    g_wedge_vt[2 + VT_SLOT_MOUSEDRAG / sizeof(uintptr_t)] = (uintptr_t)stems_wedge_drag;
    g_wedge_vt[2 + VT_SLOT_MOUSEUP   / sizeof(uintptr_t)] = (uintptr_t)stems_wedge_up;
    g_wedge_vptr = (uintptr_t)&g_wedge_vt[2];
    MDBG("stems: cloned juce::Label vtable for wedges -> %#lx\n",
         (unsigned long)g_wedge_vptr);
    return 1;
}

/* BYPASS, as a drawn icon: three bars in the stem colours standing on a base line.
 * Engaged, the bars go to icon_disabled and the line to the bypass colour. All
 * rectangles, since there is no antialiased primitive. */
static uintptr_t g_icon_vt[2 + VT_CLONE_SLOTS];
uintptr_t stems_g_icon_vptr;

#define ICON_BAR_W     8
#define ICON_BAR_GAP   5
#define ICON_BAR_H     20
#define ICON_BASE_H    3     /* the line the three stand on */
#define ICON_BASE_GAP  3     /* clearance between the bars and that line */
#define ICON_BASE_OVER 4     /* how far it runs past them at each end */

static void stems_bypass_paint(void *self, void *g)
{
    int32_t b[4];
    int w, h, i, span, x0, y0, by;

    if (stems_bounds((uintptr_t)self, b) != 0) return;
    w = b[2];
    h = b[3];
    if (w <= 0 || h <= 0) return;

    /* The plate stays grey in both states; only the glyph shows the state. */
    mod_checker_lift(g, 0, 0, w, h, stems_bypass_colour(),
                     stems_touch_lift((uintptr_t)self));
    mod_gfx_colour(g, mod_ui()->edge);
    mod_gfx_fill(g, 0, 0, w, 1);
    mod_gfx_fill(g, 0, h - 1, w, 1);
    mod_gfx_fill(g, 0, 0, 1, h);
    mod_gfx_fill(g, w - 1, 0, 1, h);

    span = N_STEMS * ICON_BAR_W + (N_STEMS - 1) * ICON_BAR_GAP;
    if (span + 2 * ICON_BASE_OVER > w) return;
    x0 = (w - span) / 2;
    y0 = (h - (ICON_BAR_H + ICON_BASE_GAP + ICON_BASE_H)) / 2;
    by = y0 + ICON_BAR_H + ICON_BASE_GAP;

    for (i = 0; i < N_STEMS; i++) {
        mod_gfx_colour(g, !stems_ready() ? mod_ui()->dead
                          : stems_g_bypass_on ? mod_ui()->icon_disabled : mod_ui()->stem[i]);
        mod_gfx_fill(g, x0 + i * (ICON_BAR_W + ICON_BAR_GAP), y0,
                       ICON_BAR_W, ICON_BAR_H);
    }
    /* The base line under the bars, always drawn; only its colour changes. */
    mod_gfx_colour(g, !stems_ready() ? mod_ui()->dead
                          : stems_g_bypass_on ? mod_ui()->bypass : mod_ui()->text_off);
    mod_gfx_fill(g, x0 - ICON_BASE_OVER, by, span + 2 * ICON_BASE_OVER, ICON_BASE_H);
}

/* The button clone with paint taken as well, so the press behaviour is shared with
 * STEMS and only the drawing differs. */
int stems_icon_vt_ready(void)
{
    if (stems_g_icon_vptr) return 1;
    if (!stems_label_vt_ready()) return 0;
    memcpy(g_icon_vt, stems_g_label_vt, sizeof(g_icon_vt));
    g_icon_vt[2 + VT_SLOT_PAINT / sizeof(uintptr_t)] = (uintptr_t)stems_bypass_paint;
    stems_g_icon_vptr = (uintptr_t)&g_icon_vt[2];
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
