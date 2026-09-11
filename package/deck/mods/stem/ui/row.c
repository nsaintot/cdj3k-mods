// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * ui/row.c - captions, mute, bypass and the progress bar
 *
 * Part of the STEMS play-screen UI. The shared contract, and the reasoning
 * behind the design, is in ui.h.
 */
#include "stem/ui/ui.h"

/* What colour the plate is wearing, in one place. Three things need it and they must
 * agree: the outline juce::Label draws, the lettering, and the rings the paint override
 * adds inside them. paint cannot ask the Label what colour it was handed -- findColour
 * walks a keyed array -- so it recomputes from the same state instead. */
static uint32_t stems_caption_colour(int i)
{
    if (i < 0 || i >= N_STEMS) return mod_ui()->dead;
    return !stems_ready()  ? mod_ui()->dead
           : stems_g_bypass_on ? mod_ui()->text_off
                           : mod_ui()->stem[i];
}

/* Which caption a component is, or -1. Three entries, so a scan beats a back-pointer --
 * the same trade as stems_wedge_index. */
static int stems_caption_index(uintptr_t comp)
{
    int i;

    for (i = 0; i < N_STEMS; i++)
        if (comp && stems_g_caption[i] == comp) return i;
    return -1;
}

/* Thicken the plate's edge, which juce::Label has no setting for: its paint ends with a
 * one-pixel drawRect and that is the whole of its outline.
 *
 * Chained rather than replaced, so the fill, the lettering and the outer ring stay
 * exactly what the class draws and this only adds rings inside them. Drawing inwards is
 * what keeps the geometry honest -- the bounds are the touch target and the wedge starts
 * at the pixel below, so an edge that grew outwards would either overlap the bars or
 * force the layout to give the plate pixels it does not otherwise need.
 *
 * Every clickable label shares this vtable, so this is also where the two kinds are
 * told apart -- the quick-menu button gets the stipple, the captions get the rings,
 * and anything else built on the clone gets plain Label. */
void stems_label_paint(void *self, void *g)
{
    int32_t b[4];
    int i, k, w, h;

    if (stems_bounds((uintptr_t)self, b) != 0) {
        /* Bracketed: Label paints its background and its lettering from colours WE set on
         * it (roles, already resolved through the theme), so the generic pass must not have
         * a second go at them. One synchronous call, so the bracket cannot leak. */
        mod_draw_enter();
        ((void (*)(void *, void *))LABEL_FN_PAINT)(self, g);
        mod_draw_leave();
        return;
    }
    w = b[2];
    h = b[3];

    /* The button's surface goes down BEFORE Label::paint, never after: Label draws its
     * background and its lettering in one call, so a stipple laid afterwards would fall
     * across the word. Its own background colour is transparent for exactly this reason
     * and stems_g_btn_state is what says which state it is in. */
    if ((uintptr_t)self == stems_g_btn_stems) {
        uint32_t lift = stems_touch_lift(stems_g_btn_stems);

        mod_checker_plate(g, 0, 0, w, h, stems_btn_surface(), lift);
        /* The bar tracks whether the PANEL is up, not what the plate is doing: the plate
         * also goes red to refuse a press, and a refusal is not a state the button is in.
         * One colour, so it lifts directly rather than through the surface call. */
        mod_btn_bar(g, 0, 0, w, h,
                    mod_colour_lift(stems_g_row_open ? mod_ui()->bar_on : mod_ui()->bar,
                                    lift));
    }

    /* Bracketed for the same reason as the early path above, which said so and then
     * only did it on the branch that never runs. Label paints its lettering from a
     * colour WE set on it, already resolved through the theme once -- unbracketed the
     * generic pass had a second go, and the STEMS button's word came out #7b809d where
     * every deck button beside it was #262944. */
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

/* Paint the caption for what it is. Called wherever the state moves, so the plate and
 * the audio can never disagree.
 *
 * A plate with an edge, wearing the same amber as the bypass: it is the same idea
 * applied to one stem instead of three. A bare word over black would say what the
 * wedge below it is and nothing about being pressable. */
void stems_caption_sync(int i)
{
    /* What the plate currently READS, so the word is only rewritten when it changes.
     * Setting the text goes through the Label's juce::Value and repaints unconditionally,
     * and this is also called for every bypass toggle. */
    static int shown[N_STEMS] = { -1, -1, -1 };
    uint32_t col;

    if (i < 0 || i >= N_STEMS || !stems_g_caption[i]) return;
    /* The word changes with the state: while it is held down the plate stops naming the
     * stem and says what is happening to it. A pulsing coloured plate says "something is
     * being held"; only the word says which something. */
    if (shown[i] != stems_g_mute[i]) {
        shown[i] = stems_g_mute[i];
        stems_text(stems_g_caption[i], stems_g_mute[i] ? "MUTED" : k_stem_name[i]);
    }
    /* Outline and lettering only, both in the stem's own colour: it names the wedge under
     * it, so wearing that colour is what ties the two together -- and an unfilled plate
     * sits over the row without weighing it down the way a solid one did.
     *
     * The FILL is left to the blink. Resting empty and filling while held is the whole
     * signal: a button that is filled only while a finger is on it cannot be mistaken for
     * one that stays where it is put. */
    col = stems_caption_colour(i);
    stems_colour(stems_g_caption[i], LBL_COL_BG, 0x00000000u);
    stems_colour(stems_g_caption[i], LBL_COL_OUTLINE, col);
    stems_colour(stems_g_caption[i], LBL_COL_TEXT, col);
}

/* HELD, not latched. The finger is the switch: down mutes, up puts the level straight
 * back where the wedge still says it is.
 *
 * That is why the mute never touches stems_g_level -- the fader position is not a thing to
 * restore afterwards, it was never moved. Only the published gain changes, and
 * stems_publish_gain reads both. */
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

/* The blink, on the display tick.
 *
 * A held control has to look different from a latched one or the DJ has no way to know
 * whether letting go will change anything. A steady amber plate is what BYPASS does and
 * BYPASS stays where you put it; alternating says "this is only true while you are
 * holding it". Same period as the warn badge, so the two read as one language. */
#define MUTE_BLINK_PERIOD  12

void stems_mute_blink(void)
{
    static int phase, lit = -1;
    /* WHAT THE MIX ACTUALLY DID. The press and the sound are deliberately not
     * the same moment -- the caption answers at once and the audio waits for the
     * quarter -- so the log above is not evidence the stem went quiet. This is.
     * [message] reading a counter [audio] adds to; a lost update costs a line. */
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
        /* Fill in the stem colour and put the lettering in black on it; empty again and
         * the lettering goes back to the colour. The outline never moves, so the plate
         * stays the same size and only its inside pulses. */
        stems_colour(stems_g_caption[i], LBL_COL_BG,
                     want ? mod_ui()->stem[i] : 0x00000000u);
        stems_colour(stems_g_caption[i], LBL_COL_TEXT,
                     want ? mod_ui()->text_on_accent : mod_ui()->stem[i]);
    }
}

/* BYPASS is a latching bypass, not a reset: the levels are KEPT, so releasing it puts
 * the mix back exactly as it was, the way an EQ bypass does. While it is on the wedges
 * are desaturated AND inert -- the look alone would be a lie, since a drag would still
 * move a value nothing is reading.
 *
 * Unity and bypassed are the same audio, so "reset to full mix" would sound identical;
 * what a bypass buys is the A/B, which a reset throws away. */
static void stems_bypass_toggle(void)
{
    int i;

    /* Nothing to take out of circuit, so the latch does not move. Refusing is better than
     * latching into a state that will not survive the next set arriving. */
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

/* What each stage is called on the bar.
 *
 * The job is long enough -- tens of seconds on a cold track -- that one word held for
 * all of it reads as a hang, so the caption names the stage the work is actually in.
 * They are the sidecar's own stages and change when it says so, never on a timer.
 *
 * Indexed by enum stem_stage. DONE and FAILED are here for completeness only: both
 * are terminal, and the row is back to the sliders before either could be drawn.
 *
 * RECONSTRUCTING and WRITING share one word. They are the server packaging what the
 * model produced, they share one segment of the bar, and stemd gives neither a figure
 * of its own -- so two names for it would be naming an internal boundary the DJ has no
 * use for and cannot see move. */
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
 * `via_server` is the run's shape and comes from the snapshot, never from the stage in
 * hand: a cache hit is one leg wearing the whole bar. See the map in ui.h.
 *
 * Every leg is anchored at BOTH ends, so a handover lands exactly on the boundary the
 * delimiter is drawn at rather than near it. Written as a switch for the same reason:
 * a stage with no case is a stage nobody weighted, and the default has to be the one
 * group that genuinely shares a figure -- the server's queue-to-model run -- not a
 * catch-all that silently absorbs whatever gets added next. */
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
    /* Full, and said so. The row is back to the sliders by the time this could be
     * drawn, but the arithmetic below would put a finished job at PROG_BOUND_SEPARATE
     * -- a value that reads as a stall on any frame that does catch it. */
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

/* What is already on the bar, so a tick that changes nothing costs a compare.
 *
 * EVERY ONE OF THESE IS THE DRAWN VALUE, not an input that decides it. The fill's
 * position turns on the stage as much as on the percent, so a percent-keyed guard sees
 * "unchanged" while the bar is describing a different leg -- which is a fill left at the
 * previous run's separation while the caption reads UPLOADING. Guarding on the width
 * asks the only question worth asking: would this repaint move a pixel.
 *
 * -1 is "nothing drawn yet": a real width is 0..stems_g_prog_w and a real mark state is
 * 0 or 1, so neither can be mistaken for it. */
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
 * Called from the display tick, i.e. tens of times a second, so everything here is
 * conditional on having actually moved: setBounds invalidates and setting a Value
 * repaints unconditionally, so an unguarded call would put the whole strip through a
 * software repaint every frame for a bar that has not changed.
 *
 * Nothing is latched from a transition. The tick only runs while the row is open, so
 * there is no moment this can be sure it witnessed -- a run can begin, finish and be
 * replaced by another entirely behind a shut row. Everything drawn is derived from `st`
 * on this call; see stems_progress_forget for the other half of that. */
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

    /* A cache hit is one leg across the whole bar and has nothing to hand over, so it
     * shows no delimiters. Read from the snapshot every tick rather than latched. */
    marks = st->via_server ? 1 : 0;
    if (marks != g_prog_marks_up) {
        g_prog_marks_up = marks;
        for (i = 0; i < N_PROG_BOUND; i++)
            stems_set_visible(stems_g_prog_mark[i], marks);
    }

    /* Derived once and used by both halves, so the number the caption prints and the
     * width the fill is given can never come from different arithmetic. */
    pos = stems_prog_pos(stage, pct, st->via_server);
    w   = stems_prog_px(pos);
    if (w != g_prog_w_drawn && stems_g_prog_fill && stems_g_prog_w > 0) {
        g_prog_w_drawn = w;
        ((bounds_t)FN_SET_BOUNDS)((void *)stems_g_prog_fill, stems_g_prog_x,
                                  stems_g_prog_y, w, PROG_BAR_H);
    }

    /* The percentage rides in the caption rather than in a second Label: it belongs to
     * the same sentence, and one centred string reads better on a 1224px bar than a
     * number floating at one end of it.
     *
     * IT IS THE BAR'S FIGURE. The stage word says what is happening and the number says
     * how far the job has got, both on the one scale the fill is drawn on.
     *
     * Compared as the finished string, not field by field, because that is exactly the
     * question being asked -- would this repaint change a pixel. */
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

/* Only ever reached by Labels carrying our cloned vtable, so an unrecognised `self`
 * means a Label we built but do not act on (a caption): do nothing, and in
 * particular do not chain -- stock Label::mouseDown is the empty stub. */
void stems_label_mousedown(void *self, void *event)
{
    (void)event;
    /* Before anything else, and for STEMS too: a sweep that started on a fader must not
     * open or shut the panel out from under itself either. */
    if (!stems_grab_take((uintptr_t)self)) return;
    /* Both buttons paint their touch tier off stems_g_grab, so taking it has to invalidate
     * them. The state changes below repaint too, but not on every path -- BYPASS with
     * no set resident does nothing at all, and a button that ignores the press still has
     * to acknowledge the finger. */
    if ((uintptr_t)self == stems_g_btn_stems || (uintptr_t)self == stems_g_btn_bypass)
        stems_repaint((uintptr_t)self);
    if ((uintptr_t)self == stems_g_btn_stems) {
        /* A press is the DJ ASKING, so it is also when to go and look again.
         *
         * The status refresh runs every 30 s and that interval is pure latency: start
         * the server, press STEMS, and the deck goes on saying there is nothing there
         * for up to half a minute -- which is indistinguishable from the feature being
         * broken, and is exactly the moment a DJ decides it is. A probe re-checks the
         * address the sidecar already has and falls back to an mDNS browse when that
         * does not answer, so a server that has just come up is found on this press
         * rather than on the next tick that happens to fall due.
         *
         * Only when the answer we hold is NOT a usable server. Probing when one is
         * plainly there would put the sidecar into a blocking health round trip for
         * every open and close of the row, all set long, to confirm something already
         * confirmed -- and the 30 s refresh is what covers a server going away. */
        {
            struct stem_ui_state st;

            if (!stem_ui_read(&st) || !st.status_seen ||
                !st.reachable || !st.compatible) {
                MDBG("stems: pressed with no server -> asking the sidecar to look now\n");
                stem_job_probe_now();
            }
        }
        /* Only OPENING is gated. A row already up must always close, whatever the
         * server did while it was open -- trapping the DJ under a panel they can
         * see is worse than any warning. */
        if (!stems_g_row_open && !stems_available()) {
            /* Answer the press in the frame it happened, rather than a blink period
             * later -- and show the badge even if it is still inside its settle
             * window, because a tap IS the DJ asking. */
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

/* BYPASS's resting fill. State lives in the glyph, not in the plate -- see
 * stems_bypass_paint -- so the plate has exactly two appearances: this, and this lifted
 * while a finger is on it.
 *
 * There USED to be a hover tier between them, and it had to go: this panel is a
 * touchscreen, so there is no pointer to hover. juce raises mouseEnter when the finger
 * lands and mouseExit only when the NEXT touch lands somewhere else, which meant the
 * hover grey appeared on release and then sat there -- reporting "the pointer is over
 * this" for as long as the DJ did not touch anything else. A state that can only ever be
 * shown at the wrong moment is not a state worth having, and deleting it is a better fix
 * than clearing the flag on mouseUp: that would have papered over a tier that is
 * meaningless on this hardware either way. */
uint32_t stems_bypass_colour(void)
{
    return mod_ui()->surface;
}

/* Chained: juce::Label overrides mouseUp for its edit-on-click path, and dropping that
 * would be a behaviour change for a class we only meant to recolour. */
void stems_label_mouseup(void *self, void *event)
{
    int i;

    /* Released on ANY of them, not just the one still under the finger: juce delivers
     * mouseUp to whoever took the mouseDown, but a gesture that ends off-component or
     * gets stolen would otherwise leave a stem silent with no way back. Clearing them
     * all costs three comparisons and cannot strand one. */
    for (i = 0; i < N_STEMS; i++)
        if (stems_g_mute[i]) stems_mute_set(i, 0);
    stems_grab_release();
    /* After the grab is cleared, not before: the touch tier is read from stems_g_grab at paint
     * time, so invalidating first would just redraw the lit state again. */
    if ((uintptr_t)self == stems_g_btn_bypass || (uintptr_t)self == stems_g_btn_stems)
        stems_repaint((uintptr_t)self);
    if (stems_g_label_mouseup)
        ((mousedown_t)stems_g_label_mouseup)(self, event);
}

/* Put every level back to full mix. MESSAGE THREAD -- it moves Components.
 *
 * Called on a track change, and the reason is not tidiness. Levels that outlive
 * their track are a performance hazard in two ways:
 *
 *   - Kill the vocals on one track, load the next, and it starts with the vocals
 *     already gone. Nothing on screen says the missing part is a leftover.
 *   - Worse, the levels apply the moment the NEW stems land, which is seconds
 *     into playback. Until then the deck plays the untouched mix, because
 *     stem_mix has no set to acquire -- so the audio JUMPS mid-track, with no
 *     input from the DJ to explain it.
 *
 * Unity is the honest resting state for a track nobody has touched yet.
 *
 * BYPASS is deliberately left alone: it is a bypass, not a level, and a DJ who
 * put the stems out of circuit means it to stay that way across a load. Unity
 * and bypassed sound identical anyway, so there is nothing to reconcile.
 *
 * stems_level_set publishes to the audio thread as part of the move, so there is no
 * second step here that could be forgotten. */
void mod_stems_reset_levels(void)
{
    int i, moved = 0;

    for (i = 0; i < N_STEMS; i++) {
        /* Mutes go with the levels, and for the same reason: a part silenced on the last
         * track is a part missing from this one with nothing on screen to explain it. */
        if (stems_g_mute[i]) { stems_g_mute[i] = 0; stems_caption_sync(i); moved = 1; }
        if (stems_g_level[i] != STEM_LEVEL_MAX) { stems_level_set(i, STEM_LEVEL_MAX); moved = 1; }
        stems_publish_gain(i);
    }
    if (moved)
        MDBG("stems: track change -> levels back to full mix\n");
}
