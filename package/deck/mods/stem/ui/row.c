// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * ui/row.c - captions, mute, bypass and the progress bar
 *
 * Part of the STEMS play-screen UI. The shared contract, and the reasoning
 * behind the design, is in ui.h.
 */
#include "stem/ui/ui.h"

/* The caption's colour, shared by the outline juce::Label draws, the lettering and
 * the rings the paint override adds. paint recomputes it from state rather than
 * calling findColour, which walks a keyed array. */
static uint32_t stems_caption_colour(int i)
{
    if (i < 0 || i >= N_STEMS) return mod_ui()->dead;
    return !stems_ready()  ? mod_ui()->dead
           : stems_g_bypass_on ? mod_ui()->text_off
                           : mod_ui()->stem[i];
}

/* Which caption a component is, or -1. */
static int stems_caption_index(uintptr_t comp)
{
    int i;

    for (i = 0; i < N_STEMS; i++)
        if (comp && stems_g_caption[i] == comp) return i;
    return -1;
}

/* Paint for every clickable label (they share this vtable): the quick-menu button
 * gets the stipple and the bottom bar, the captions get a thicker edge, anything else
 * paints as a plain Label.
 *
 * Label::paint is chained, and the caption edge is drawn as rings inside Label's
 * one-pixel outline: the bounds are the touch target and the wedge starts on the pixel
 * below, so the edge cannot grow outwards. */
void stems_label_paint(void *self, void *g)
{
    int32_t b[4];
    int i, k, w, h;

    if (stems_bounds((uintptr_t)self, b) != 0) {
        /* Bracketed: Label paints from colours we set (roles already resolved through
         * the theme), so the generic theme pass must not resolve them again. One
         * synchronous call, so the bracket cannot leak. */
        mod_draw_enter();
        ((void (*)(void *, void *))LABEL_FN_PAINT)(self, g);
        mod_draw_leave();
        return;
    }
    w = b[2];
    h = b[3];

    /* The button's surface goes down before Label::paint, which draws background and
     * lettering in one call; a stipple laid afterwards would cover the word. The Label's
     * own background is transparent and stems_g_btn_state gives the state. */
    if ((uintptr_t)self == stems_g_btn_stems) {
        uint32_t lift = stems_touch_lift(stems_g_btn_stems);

        mod_checker_plate(g, 0, 0, w, h, stems_btn_surface(), lift);
        /* The bar shows whether the panel is up, independent of the plate's refusal red.
         * A single colour, so it is lifted directly. */
        mod_btn_bar(g, 0, 0, w, h,
                    mod_colour_lift(stems_g_row_open ? mod_ui()->bar_on : mod_ui()->bar,
                                    lift));
    }

    /* Bracketed, as above. Without it the STEMS lettering is resolved twice and comes
     * out #7b809d instead of the deck buttons' #262944. */
    mod_draw_enter();
    ((void (*)(void *, void *))LABEL_FN_PAINT)(self, g);
    mod_draw_leave();

    i = stems_caption_index((uintptr_t)self);
    if (i < 0) return;
    mod_gfx_colour(g, stems_caption_colour(i));
    for (k = 1; k < CAPTION_EDGE_W; k++) {
        if (w <= 2 * k || h <= 2 * k) break;         /* nothing left to draw into */
        mod_gfx_fill(g, k,         k,         w - 2 * k, 1);           /* top    */
        mod_gfx_fill(g, k,         h - k - 1, w - 2 * k, 1);           /* bottom */
        mod_gfx_fill(g, k,         k,         1,         h - 2 * k);   /* left   */
        mod_gfx_fill(g, w - k - 1, k,         1,         h - 2 * k);   /* right  */
    }
}

/* Set the caption's text and colours from the current state. Called wherever that
 * state changes. The outlined plate marks the caption as pressable. */
void stems_caption_sync(int i)
{
    /* The word currently shown, so it is only rewritten on change: setting the text
     * repaints unconditionally, and this also runs on every bypass toggle. */
    static int shown[N_STEMS] = { -1, -1, -1 };
    uint32_t col;

    if (i < 0 || i >= N_STEMS || !stems_g_caption[i]) return;
    /* While held, the caption reads MUTED instead of the stem name. */
    if (shown[i] != stems_g_mute[i]) {
        shown[i] = stems_g_mute[i];
        stems_text(stems_g_caption[i], stems_g_mute[i] ? "MUTED" : k_stem_name[i]);
    }
    /* Outline and lettering in the stem's colour, matching the wedge below. The fill
     * is left to the mute blink: empty at rest, filled only while held. */
    col = stems_caption_colour(i);
    stems_colour(stems_g_caption[i], LBL_COL_BG, 0x00000000u);
    stems_colour(stems_g_caption[i], LBL_COL_OUTLINE, col);
    stems_colour(stems_g_caption[i], LBL_COL_TEXT, col);
}

/* Mute is held, not latched: press mutes, release restores the level the wedge shows.
 * stems_g_level is never touched; stems_publish_gain combines it with the mute. */
static void stems_mute_set(int i, int on)
{
    if (i < 0 || i >= N_STEMS || stems_g_mute[i] == !!on) return;
    if (on && !stems_ready()) return;      /* nothing to silence */
    stems_g_mute[i] = !!on;
    stems_publish_gain(i);
    stems_caption_sync(i);
    stems_repaint(stems_g_wedge[i]);
    MDBG("stems: %s %s\n", k_stem_name[i],
         on ? "held MUTE" : "released -> level restored");
}

/* The held-mute blink, on the display tick. Blinking marks the state as momentary.
 * Same period as the warn badge (MOD_BLINK_PERIOD). */
#define MUTE_BLINK_PERIOD  12

void stems_mute_blink(void)
{
    static int phase, lit = -1;
    /* Log when the mix applies a mute. The caption changes at once but the audio
     * waits for the quarter, so the press log alone does not show the stem went
     * quiet. [message] reads a counter [audio] adds to; a lost update costs a line. */
    static unsigned said_commits;
    unsigned commits = __atomic_load_n(&g_stem_mute_commits, __ATOMIC_RELAXED);

    if (commits != said_commits) {
        said_commits = commits;
        MDBG("stems: mix applied a mute change (%u since boot)\n", commits);
    }
    int i, want, any = 0;

    for (i = 0; i < N_STEMS; i++)
        if (stems_g_mute[i]) { any = 1; break; }
    if (!any) {
        phase = 0;
        lit = -1;
        return;
    }
    want = ((phase++ / MUTE_BLINK_PERIOD) & 1) == 0;
    if (want == lit) return;
    lit = want;
    for (i = 0; i < N_STEMS; i++) {
        if (!stems_g_mute[i]) continue;
        /* Alternate between a stem-colour fill with text_on_accent lettering and an
         * empty plate with stem-colour lettering. The outline does not change. */
        stems_colour(stems_g_caption[i], LBL_COL_BG,
                     want ? mod_ui()->stem[i] : 0x00000000u);
        stems_colour(stems_g_caption[i], LBL_COL_TEXT,
                     want ? mod_ui()->text_on_accent : mod_ui()->stem[i]);
    }
}

/* BYPASS latches, like an EQ bypass: the levels are kept, so releasing it restores
 * the mix as it was. While on, the wedges are greyed and inert. */
static void stems_bypass_toggle(void)
{
    int i;

    /* No stems resident: nothing to take out of circuit, so ignore the press. */
    if (!stems_ready()) {
        MDBG("stems: no stems resident -> BYPASS ignored\n");
        return;
    }
    stems_g_bypass_on = !stems_g_bypass_on;
    stem_bypass_set(stems_g_bypass_on);
    stems_repaint(stems_g_btn_bypass);   /* its paint reads the state itself */
    for (i = 0; i < N_STEMS; i++) {
        stems_caption_sync(i);
        stems_repaint(stems_g_wedge[i]);   /* the wedge reads stems_g_bypass_on in its paint */
    }
    MDBG("stems: BYPASS %s -- stems %s\n",
         stems_g_bypass_on ? "on" : "off",
         stems_g_bypass_on ? "bypassed, wedges inert" : "back in circuit");
}

/* The caption for each stage, indexed by enum stem_stage. A job takes tens of seconds
 * on a cold track, so the caption follows the sidecar's stages. DONE and FAILED are
 * terminal and the row is back to the controls before either is drawn.
 *
 * RECONSTRUCTING and WRITING are both the server packaging the model's output, share
 * one segment of the bar and report no count, so they share one word. */
static const char *const k_stage_name[] = {
    "WAITING",          /* IDLE           */
    "UPLOADING",        /* UPLOADING      */
    "QUEUED",           /* QUEUED         */
    "ANALYZING",        /* ANALYZING      */
    "SEPARATING",       /* SEPARATING     */
    "PREPARING",        /* RECONSTRUCTING */
    "PREPARING",        /* WRITING        */
    "DOWNLOADING",      /* FETCHING       */
    "DONE",             /* DONE           */
    "FAILED",           /* FAILED         */
    "LOADING",          /* LOADING        */
};

int stems_prog_px(int pct)
{
    return (int)(((long)pct * stems_g_prog_w) / 100);
}

/* Leg-local percent -> position on the whole bar, in percent of the bar.
 *
 * `via_server` comes from the snapshot, not the stage: a cache hit is one leg across
 * the whole bar. See the map in ui.h.
 *
 * Every leg is anchored at both ends, so a handover lands exactly on its delimiter.
 * A new stage needs its own case; the default is only for the queue-to-model group. */
static int stems_prog_pos(int stage, int pct, int via_server)
{
    if (!via_server)
        return pct;
    switch (stage) {
    case STEM_STAGE_UPLOADING:
        return pct * PROG_BOUND_UPLOAD / 100;
    /* The server's tail, sharing the third segment with the download. See ui.h. */
    case STEM_STAGE_RECONSTRUCTING:
        return PROG_BOUND_SEPARATE +
               pct * (PROG_BOUND_RECON - PROG_BOUND_SEPARATE) / 100;
    case STEM_STAGE_WRITING:
        return PROG_BOUND_RECON +
               pct * (PROG_BOUND_WRITE - PROG_BOUND_RECON) / 100;
    case STEM_STAGE_FETCHING:
        return PROG_BOUND_WRITE +
               pct * (PROG_BOUND_FETCH - PROG_BOUND_WRITE) / 100;
    case STEM_STAGE_LOADING:
        return PROG_BOUND_FETCH + pct * (100 - PROG_BOUND_FETCH) / 100;
    /* Normally never drawn, but the default below would put a finished job at
     * PROG_BOUND_SEPARATE. */
    case STEM_STAGE_DONE:
        return 100;
    default:
        /* QUEUED, ANALYZING, SEPARATING share the second segment. Only separating
         * reports a count; the other two send 0. Most of a job's time is spent here,
         * hence the widest segment. */
        return PROG_BOUND_UPLOAD +
               pct * (PROG_BOUND_SEPARATE - PROG_BOUND_UPLOAD) / 100;
    }
}

/* What is already on the bar, so a tick that changes nothing costs a compare. These
 * cache drawn values, not inputs: the fill position depends on the stage as well as
 * the percent, so a percent-keyed guard would miss a leg change.
 *
 * -1 means nothing drawn yet. */
static int  g_prog_w_drawn  = -1;
static int  g_prog_marks_up = -1;
static char g_prog_caption[PROG_CAPTION_MAX];

void stems_progress_forget(void)
{
    g_prog_w_drawn  = -1;
    g_prog_marks_up = -1;
    g_prog_caption[0] = '\0';
}

/* The processing state replaces the whole row.
 *
 * Called from the display tick, so every update is guarded on change: setBounds
 * invalidates and setting a Value repaints unconditionally.
 *
 * Nothing is latched from a transition: the tick only runs while the row is open, and
 * a run can begin, finish and be replaced while it is shut. Everything drawn is derived
 * from `st` on this call; see stems_progress_forget. */
void stems_processing_set(const struct stem_ui_state *st)
{
    char caption[PROG_CAPTION_MAX];
    int pct, stage, marks, pos, w, i;

    if (!st) {
        if (stems_g_processing) {
            stems_g_processing = 0;
            stems_set_visible(stems_g_controls, 1);
            stems_set_visible(stems_g_progress, 0);
            stems_progress_forget();
        }
        return;
    }
    if (!stems_g_processing) {
        stems_g_processing = 1;
        stems_set_visible(stems_g_controls, 0);
        stems_set_visible(stems_g_progress, 1);
        stems_progress_forget();
    }

    pct = st->percent;
    if (pct < 0) pct = 0; else if (pct > 100) pct = 100;
    stage = st->stage;
    if (stage < 0 || stage >= (int)(sizeof(k_stage_name) / sizeof(k_stage_name[0])))
        stage = STEM_STAGE_IDLE;

    /* A cache hit shows no delimiters. Read from the snapshot every tick. */
    marks = st->via_server ? 1 : 0;
    if (marks != g_prog_marks_up) {
        g_prog_marks_up = marks;
        for (i = 0; i < N_PROG_BOUND; i++)
            stems_set_visible(stems_g_prog_mark[i], marks);
    }

    /* The caption and the fill both use pos. */
    pos = stems_prog_pos(stage, pct, st->via_server);
    w   = stems_prog_px(pos);
    if (w != g_prog_w_drawn && stems_g_prog_fill && stems_g_prog_w > 0) {
        g_prog_w_drawn = w;
        ((bounds_t)FN_SET_BOUNDS)((void *)stems_g_prog_fill, stems_g_prog_x,
                                  stems_g_prog_y, w, PROG_BAR_H);
    }

    /* Stage word plus the whole-bar percentage (or queue depth), as one centred
     * string. Compared as the finished string to skip unchanged repaints. */
    if (stage == STEM_STAGE_QUEUED && st->queue_position > 0)
        snprintf(caption, sizeof(caption), "%s  %d AHEAD", k_stage_name[stage],
                 st->queue_position);
    else
        snprintf(caption, sizeof(caption), "%s  %d%%", k_stage_name[stage], pos);
    if (stems_g_prog_text && strcmp(caption, g_prog_caption) != 0) {
        snprintf(g_prog_caption, sizeof(g_prog_caption), "%s", caption);
        stems_text(stems_g_prog_text, caption);
    }
}

/* Reached only by Labels with our cloned vtable. An unrecognised `self` is ignored;
 * there is nothing to chain, since stock Label::mouseDown is the empty stub. */
void stems_label_mousedown(void *self, void *event)
{
    (void)event;
    /* First, and for STEMS too: a sweep that started on a fader must not toggle the
     * panel. */
    if (!stems_grab_take((uintptr_t)self)) return;
    /* Both buttons paint their touch highlight from stems_g_grab, so repaint them here;
     * not every path below repaints (e.g. BYPASS with no set resident). */
    if ((uintptr_t)self == stems_g_btn_stems || (uintptr_t)self == stems_g_btn_bypass)
        stems_repaint((uintptr_t)self);
    if ((uintptr_t)self == stems_g_btn_stems) {
        /* A press with no usable server asks the sidecar to probe now instead of
         * waiting up to 30 s for the status refresh. The probe re-checks the known
         * address and falls back to an mDNS browse. Skipped when a server is usable:
         * each probe is a blocking health round trip, and the 30 s refresh covers a
         * server going away. */
        {
            struct stem_ui_state st;

            if (!stem_ui_read(&st) || !st.status_seen ||
                !st.reachable || !st.compatible) {
                MDBG("stems: pressed with no server -> asking the sidecar to look now\n");
                stem_job_probe_now();
            }
        }
        /* Only opening is gated; an open row can always be closed. */
        if (!stems_g_row_open && !stems_available()) {
            /* Refuse in this frame, and show the badge even inside its settle
             * window. */
            stems_g_warn_blink = MOD_BLINK_TICKS;
            stems_g_warn_up = 1;
            stems_set_visible(stems_g_warn, 1);
            stems_btn_state(BTN_REFUSE);
            stems_colour(stems_g_warn,      LBL_COL_TEXT, mod_ui()->text);
            MDBG("stems: no stems and no server -> refusing to open, blinking\n");
            return;
        }
        stems_toggle_row();
    } else if ((uintptr_t)self == stems_g_btn_bypass) {
        stems_bypass_toggle();
    } else {
        int i;

        for (i = 0; i < N_STEMS; i++)
            if ((uintptr_t)self == stems_g_caption[i]) { stems_mute_set(i, 1); return; }
    }
}

/* BYPASS's resting fill. The state is shown by the glyph (stems_bypass_paint), so the
 * plate has two appearances: this, and this lifted while touched.
 *
 * No hover tier: on this touchscreen juce sends mouseExit only when the next touch
 * lands elsewhere, so a hover state would stick after release. */
uint32_t stems_bypass_colour(void)
{
    return mod_ui()->surface;
}

/* Chained: juce::Label overrides mouseUp for its edit-on-click path. */
void stems_label_mouseup(void *self, void *event)
{
    int i;

    /* Release every mute, not only this caption's: a gesture that ends off-component
     * or is stolen would otherwise leave a stem silent. */
    for (i = 0; i < N_STEMS; i++)
        if (stems_g_mute[i]) stems_mute_set(i, 0);
    stems_grab_release();
    /* After the grab is cleared: paint reads the touch highlight from stems_g_grab. */
    if ((uintptr_t)self == stems_g_btn_bypass || (uintptr_t)self == stems_g_btn_stems)
        stems_repaint((uintptr_t)self);
    if (stems_g_label_mouseup)
        ((mousedown_t)stems_g_label_mouseup)(self, event);
}

/* Put every level back to full mix. Message thread: it moves Components.
 *
 * Called on a track change. Kept levels would carry a muted part into the next track,
 * and would apply only once the new stems land, seconds into playback (until then
 * stem_mix has no set and plays the full mix), so the audio would change mid-track.
 *
 * BYPASS is left as is: it is not a level, and unity and bypassed sound the same.
 *
 * stems_level_set publishes to the audio thread as part of the move. */
void mod_stems_reset_levels(void)
{
    int i, moved = 0;

    for (i = 0; i < N_STEMS; i++) {
        /* Mutes are cleared too. */
        if (stems_g_mute[i]) { stems_g_mute[i] = 0; stems_caption_sync(i); moved = 1; }
        if (stems_g_level[i] != STEM_LEVEL_MAX) { stems_level_set(i, STEM_LEVEL_MAX); moved = 1; }
        stems_publish_gain(i);
    }
    if (moved)
        MDBG("stems: track change -> levels back to full mix\n");
}
