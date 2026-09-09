// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * ui/hooks.c - the anchor hooks, the polls they drive, and install
 *
 * Part of the STEMS play-screen UI. The shared contract, and the reasoning
 * behind the design, is in ui.h.
 */
#include "stem/ui/ui.h"
#include "cue/cue.h"      /* the groove pad's blink rides this timer */
#include "kit/mod.h"

/* ================================================================== */
/* Hooks on the anchor                                                */
/* ================================================================== */

/* paint runs per frame, and every build attempt costs two /proc/self/mem reads, so a
 * situation we never recover from must not be retried forever. A few seconds of frames
 * is far longer than the view takes to finish wiring itself up. */
#define STEMS_MAX_ATTEMPTS 240

static void stems_try_build(uintptr_t anchor)
{
    static int attempts;

    if (stems_g_built || stems_g_building || !stems_g_api_ok) return;
    /* Nothing is allocated until ENABLE STEMS is on, so a deck that never opts in is
     * unchanged -- and the attempt budget below only starts burning once it has. */
    if (!g_stems_on) return;
    if (++attempts > STEMS_MAX_ATTEMPTS) {
        if (attempts == STEMS_MAX_ATTEMPTS + 1)
            MERR("stems: gave up after %d attempts -> feature off\n", STEMS_MAX_ATTEMPTS);
        stems_g_api_ok = 0;
        return;
    }
    stems_build(anchor);
}

/* The processing row, driven by the job snapshot job.c publishes.
 *
 * stem_ui_read() is a seqlock copy, so it cannot block the one thread that must
 * never block, and a torn read costs a single repaint.
 *
 * Polled only while our row is up, so a deck with the panel shut pays nothing. */
void stems_progress_poll(void)
{
    struct stem_ui_state st;
    int busy;

    if (!stem_ui_read(&st)) {
        stems_processing_set(NULL);
        return;
    }

    /* Everything between the request and the stems being resident counts as busy.
     * Named exclusions rather than a range: LOADING sits ABOVE the two terminal
     * values, because the wire enum is the sidecar's and ours only extends it. */
    busy = st.stage != STEM_STAGE_IDLE &&
           st.stage != STEM_STAGE_DONE &&
           st.stage != STEM_STAGE_FAILED;
    if (!busy) {
        if (stems_g_processing)
            MDBG("stems: processing finished (stage %d)\n", st.stage);
        stems_processing_set(NULL);
        return;
    }
    if (!stems_g_processing)
        MDBG("stems: processing started (stage %d)\n", st.stage);
    stems_processing_set(&st);
}

/* Can STEMS do anything at all right now?
 *
 * NOT "is the server up". The on-media cache exists precisely so a deck with no
 * network plays stems, so gating on reachability would disable the feature in the
 * one case it was built for -- stick in, track loads, stems play. What actually
 * makes the button useless is having neither: no stems in hand for this track, none
 * on the way, and nobody to ask for them.
 *
 * Unknown is not unavailable. Before the first STATUS `reachable` is merely zero,
 * which reads identically to "no server", and a warning shown at boot before
 * anything has been asked is one the DJ cannot check. */
int stems_available(void)
{
    struct stem_ui_state st;

    if (g_stem_ready)                       /* stems are loaded and playing */
        return 1;
    if (!stem_ui_read(&st) || !st.status_seen)
        return 1;                           /* nothing asked yet: say nothing */
    /* A pair coming off the media needs no server: the stems are on the way
     * whatever the sidecar thinks of the network, and for a long track that leg
     * runs a minute. Only the OFF-MEDIA load; a separation's own LOADING leg
     * belongs to the server test below. */
    if (st.stage == STEM_STAGE_LOADING && !st.via_server)
        return 1;
    /* A SERVER THE SIDECAR HAS CALLED UNUSABLE IS THE ANSWER, whatever stage a
     * job happens to be in. This test used to sit below the one after it, and
     * that ordering broke the moment failures started being retried: every five
     * seconds the doomed attempt put the stage into a running one, this said
     * "available" for a second, and the badge was hidden and the refusal blink
     * torn down mid-flash -- which is how the plate ended up stranded red and
     * still. A retry against a server that has already said no is not a job that
     * "may well land". */
    if (!st.reachable || !st.compatible)
        return 0;
    if (st.stage != STEM_STAGE_IDLE && st.stage != STEM_STAGE_DONE &&
        st.stage != STEM_STAGE_FAILED)
        return 1;                           /* a job is running: it may well land */
    return 1;
}

/* Show, hide and blink the badge. Called from the display tick, so it does nothing
 * unless the answer changed -- setVisible invalidates. */
/* g_stem_ready is written by the worker at a moment the message thread knows nothing
 * about, so the row is brought into line on the tick rather than at the change. */
static void stems_ready_poll(void)
{
    static int shown = -1;
    int now = stems_ready();
    int i;

    if (now == shown) return;
    shown = now;
    /* A set going away takes the bypass with it: leaving BYPASS latched over nothing
     * would silently apply to whatever lands next. */
    if (!now && stems_g_bypass_on) {
        stems_g_bypass_on = 0;
        stem_bypass_set(0);
    }
    stems_repaint(stems_g_btn_bypass);
    for (i = 0; i < N_STEMS; i++) {
        stems_caption_sync(i);
        stems_repaint(stems_g_wedge[i]);
    }
    MDBG("stems: %s\n", now ? "stems resident -> row live"
                             : "no stems resident -> row inert");
}

/* "The stems are not leaving this track alone", in the corner of the STEMS
 * button.
 *
 * THE ROW IS WHERE THE STATE SHOWS, AND THE ROW IS USUALLY CLOSED. Levels stay
 * where the DJ put them and a groove circuit keeps running when the row goes
 * away, which is what makes the row a place to keep controls rather than the
 * feature itself -- and which leaves nothing on screen saying so. The title bar
 * button is on screen either way, so the mark goes there.
 *
 * ONE MARK, ONE COLOUR. The question a glance asks is "is this track coming out
 * as it went in", which is a yes or a no -- a fader off unity and a stem being
 * replaced are the same answer, and a mark that changed colour between them
 * would be asking the DJ to learn a code for a distinction the row shows them
 * anyway. Blue because on this deck blue means on, selected, working.
 *
 * Except on the lit plate, where the accent IS the button and a mark in it would
 * disappear -- there it goes white, which is the same swap the warn badge makes
 * and the same colour the lettering beside it is already wearing.
 *
 * BYPASS is not edited. It is the DJ taking the whole feature out of circuit,
 * which is the track as it came, so it does not earn a mark. */
static void stems_edit_poll(void)
{
    const struct theme_ui *ui = mod_ui();
    uint32_t want = 0;
    int edited = 0, i;

    if (!stems_g_edit)
        return;

    if (g_stems_on && !stem_bypass_get()) {
        edited = gc_active_part() >= 0;
        for (i = 0; !edited && i < N_STEMS; i++)
            if (stem_gain_get(i) != 1.0f)
                edited = 1;
    }
    if (edited)
        want = stems_g_btn_state == BTN_ON ? ui->text : ui->accent;

    /* Only on the turn. This runs at the display rate and setColour is a
     * change-notifier, so telling it the same colour fifty times a second is a
     * great deal of dispatch for a picture that is already right. */
    if (want == stems_g_edit_col)
        return;
    if (want)
        stems_colour(stems_g_edit, LBL_COL_TEXT, want);
    stems_set_visible(stems_g_edit, want != 0);
    stems_g_edit_col = want;
}

static void stems_warn_poll(void)
{
    static int settle;
    int want = !stems_available();

    if (!stems_g_warn)
        return;

    /* Why the badge is or is not up, said whenever any input to that answer
     * moves. They come from three different layers -- the store, the job's UI
     * snapshot and the sidecar's own status -- so "the warning did not appear"
     * is otherwise a question with no cheap way to ask. It was `seen` that
     * answered it: a sidecar older than the shim sends a STATUS shorter than the
     * struct, handle_frame drops it on the length check, and stems_available
     * goes on saying "nothing asked yet" for ever. */
    {
        static int said[5] = { -1, -1, -1, -1, -1 };
        struct stem_ui_state st;

        if (stem_ui_read(&st) &&
            (said[0] != want || said[1] != g_stem_ready ||
             said[2] != st.status_seen || said[3] != st.stage ||
             said[4] != (st.reachable && st.compatible))) {
            said[0] = want;          said[1] = g_stem_ready;
            said[2] = st.status_seen; said[3] = st.stage;
            said[4] = st.reachable && st.compatible;
            MDBG("stems: warn %s (ready=%d seen=%d stage=%d reach=%d compat=%d)\n",
                 want ? "WANTED" : "not wanted", g_stem_ready,
                 st.status_seen, st.stage, st.reachable, st.compatible);
        }
    }

    /* Appearing is damped, vanishing is not.
     *
     * A track change opens a real gap where the answer is honestly "nothing": the
     * worker clears g_stem_ready and publishes IDLE, and only tens of milliseconds
     * later does the cache lookup put it back into a running state. Undamped, that
     * showed as the badge blipping on at every track change on an offline deck --
     * which is worse than useless, because a warning that flashes at moments the DJ
     * has not asked about anything is one they learn to ignore. A second of the
     * condition holding is still far quicker than they can reach the button. */
    settle = want ? settle + 1 : 0;
    if (want != stems_g_warn_up && (!want || settle > WARN_SETTLE_TICKS)) {
        stems_g_warn_up = want;
        stems_set_visible(stems_g_warn, want);
        if (!want) {
            /* A blink cut short leaves the plate on whichever half it was on, and
             * half of those are red -- so the resting state is forced rather than
             * left to whatever the last tick happened to draw. */
            stems_g_warn_blink = 0;
            stems_btn_state(BTN_OFF);
        }
        /* Retract the row on the way in. Refusing to OPEN it is only half the
         * gate: skipping from a cached track to an uncached one with no server
         * leaves the faders sitting there, live-looking, over a mix they no longer
         * touch -- and a fader that does nothing is worse than an absent one.
         *
         * Safe from here because our hook chains the app's own callback FIRST, so
         * by this point the display pass is finished and re-laying out the band is
         * the same work a tap would do. */
        if (want && stems_g_row_open) {
            MDBG("stems: nothing left to control -> closing the row\n");
            stems_toggle_row();
        }
    }
    if (!want || !stems_g_warn_blink)
        return;
    /* One colour change per period, not per tick: at 44 Hz an every-frame toggle is
     * a shimmer rather than a blink.
     *
     * The button and the badge flip together, so it reads as one object flashing
     * rather than two things happening at once. The badge goes white on the red
     * half because amber on that red is mush. */
    if (--stems_g_warn_blink % MOD_BLINK_PERIOD == 0) {
        /* The tap paints the first red itself, so the phase here starts cold; the
         * terminal tick is forced cold whatever the parity works out to, because
         * the resting state has to be the one sync would draw. */
        int hot = stems_g_warn_blink && !((stems_g_warn_blink / MOD_BLINK_PERIOD) & 1);

        stems_btn_state(hot ? BTN_REFUSE : BTN_OFF);
        stems_colour(stems_g_warn,      LBL_COL_TEXT, hot ? mod_ui()->text   : mod_ui()->warn);
    }
}

/* Push the roles back into the Labels we set at build time.
 *
 * Everything OUR paint draws is already live -- it asks mod_ui() per frame. This is for
 * the other half: a colour handed to juce::Component::setColour is COPIED into the
 * component, so it keeps whatever theme was in force when the row was built. Without
 * this, a theme change leaves the progress rail, the STEMS lettering and the warn
 * badge on the previous palette until something rewrites them.
 *
 * Cheap and rare -- it runs on a theme change and at no other time. */
static void stems_theme_restamp(void)
{
    const struct theme_ui *ui = mod_ui();
    int i;

    /* THE PROGRESS RAIL IS NOT HERE. Its colours are stored in ORIGINAL's space and the
     * theme hook resolves them at paint time, so they follow the palette on their own and
     * restamping them with a resolved value is what put the caption at #ffffff on WHITE's
     * #ffffff plate. See mod_ui_stock. */
    stems_colour(stems_g_btn_stems, LBL_COL_TEXT, ui->text_deck);
    stems_colour(stems_g_warn,      LBL_COL_TEXT, ui->warn);
    /* Its colour is a role in the new palette, so forget the old one and let the
     * next tick resolve it rather than restamping a value from the last theme. */
    stems_g_edit_col = 0;
    /* The captions already have one place that decides their three colours; reusing it
     * keeps a second copy of that logic from existing here. */
    for (i = 0; i < N_STEMS; i++)
        stems_caption_sync(i);
    stems_repaint(stems_g_row);
    stems_repaint(stems_g_btn_stems);
    MDBG("theme: restamped the stem row for %s\n", mod_theme()->name);
}

/* Watch the selection rather than hooking the setting, so it catches every route in --
 * the MOD SETTINGS row, a value restored from the settings file at startup, anything
 * added later. One int compare per tick. */
static void stems_theme_poll(void)
{
    static int last = -1;
    int id = __atomic_load_n(&g_theme_id, __ATOMIC_RELAXED);

    /* Nothing to stamp yet -- and crucially, do NOT record the id in that case. Marking
     * it handled before the row exists is how a theme restored from the settings file at
     * startup never got stamped at all: the first tick beat the build, recorded the id,
     * and every tick afterwards compared equal. Only a restamp that actually happened
     * may move `last`. */
    if (!stems_g_built) return;
    if (id == last) return;
    last = id;
    stems_theme_restamp();
}

/* The display tick. Chained onto the app's own refresh timer, so the progress row
 * advances on the frame the job advances rather than whenever something else happens
 * to invalidate the title bar.
 *
 * Nothing but the row's own state belongs here. Building the row still hangs off
 * paint, where a repaint is proof the tree exists; this fires from the very first
 * frame, long before there is anything of ours to talk to. */
static void stems_drc_timer(void *self)
{
    static int ticks;
    static time_t t0;

    if (stems_g_orig_drc_timer)
        ((timercb_t)stems_g_orig_drc_timer)(self);
    stems_g_ticks++;

    /* The groove circuit's armed pad blinks off this same clock, and for the
     * same reason the row below does: the panel writes a lamp only when its own
     * state changes, which a running groove is not. */
    cue_led_tick();

    /* And the follow-up a gate-cue release may have left pending, which needs a
     * clock for the same reason and has no other one. */
    cue_pad_tick();

    /* And the beat grid, which the deck answers for on load and which nothing
     * else collects until a pad is pressed. The X-PAD's clock runs on it and the
     * row's own quantized MUTE rides that clock, so a track played straight
     * through without a pad press would otherwise have neither. */
    stem_grid_tick();

    /* Measured once and printed, because everything timed off this clock -- the
     * blink period above, any future animation -- is otherwise guessing at it. */
    if (t0 == 0) t0 = time(NULL);
    else if (++ticks && t0 > 0 && time(NULL) - t0 >= 5) {
        MDBG("stems: display tick is %d Hz\n", ticks / (int)(time(NULL) - t0));
        t0 = -1;
    }

    if (!stems_g_built)
        return;
    /* First: it can close the row, and the polls below would otherwise spend a frame
     * updating a strip that another panel is already drawing over. */
    stems_theme_poll();
    kit_band_poll();
    if (stems_g_row_open) {
        stems_progress_poll();
        stems_wavewait_poll();
        stems_mute_blink();
        stems_ready_poll();
    }
    stems_warn_poll();
    /* Outside the row-open guard above, deliberately: the whole point of the
     * mark is the state the closed row is hiding. */
    stems_edit_poll();
}

/* The title bar draws whenever the loaded track or its labels change, which is what
 * makes paint the trigger: it needs no user action and the parent chain is already
 * wired by the time anything is drawn. */
static void stems_ta_paint(void *self, void *g)
{
    if (stems_g_orig_ta_paint)
        ((paint_t)stems_g_orig_ta_paint)(self, g);
    stems_try_build((uintptr_t)self);
    if (stems_g_built) stems_sync();
    /* Safety net for any close path that never reaches a handler of ours -- a screen
     * change, or the row being torn down from under us. The band being shrunk while our
     * row is shut is by definition wrong, so reconcile it here rather than trying to
     * enumerate every route out. Costs one read per repaint, and the title bar repaints
     * rarely. */
    if (stems_g_built && !stems_g_row_open) kit_band_poll();
    /* The audio side's counters are printed from here, on the message thread --
     * never from the read()/operate() hooks themselves. */
    mod_stem_audio_report();
    /* The progress row is NOT polled from here. This is a paint, so it fires when
     * something invalidates the title bar and at no other time -- which for a bar
     * that has to advance on its own is not a clock at all. stems_drc_timer has it. */
    /* One dump of the app's own open layout, for diffing against ours. */
    if (MLOG_AT(MOD_LOG_DEBUG) && stems_g_built && !stems_g_dumped_stock && kit_band_stock_up()) {
        stems_g_dumped_stock = 1;
        MDBG("tree --- STOCK open ---\n");
        stems_dump_tree(kit_band_view(), 0);
    }
}

/* Secondary trigger, for the case where the bar is drawn once before its parent is
 * attached and then never invalidated again. Chained: TouchAria really does use it. */
static void stems_ta_mousedown(void *self, void *event)
{
    stems_try_build((uintptr_t)self);
    if (stems_g_orig_ta_mousedown)
        ((mousedown_t)stems_g_orig_ta_mousedown)(self, event);
}

/* ================================================================== */
/* Install                                                            */
/* ================================================================== */

static int stems_ui_install(void)
{
    /* Every juce primitive the row is built out of. A signature match says more
     * than a prologue guard, so the check is simply whether the resolver found
     * them -- and it is a hard gate: half a row is worse than no row. */
    stems_g_api_ok = FN_ADD_VISIBLE && FN_SET_BOUNDS && FN_STR_DEFCTOR &&
               FN_FONT_BUILD && FN_LABEL_CTOR && FN_LABEL_SETFONT &&
               FN_LABEL_JUSTIFY && FN_VAR_DTOR && FN_VAR_FROM_STR &&
               FN_VALUE_SETVALUE &&
               /* Our own paint stands on these three; without them the wedges
                * cannot draw and there is no fallback shape to fall back to. */
               FN_GFX_SETCOLOUR && FN_GFX_FILLRECT && FN_COMP_REPAINT;
    if (!stems_g_api_ok) {
        MDBG("stems: juce primitives did not resolve -> feature off\n");
        return -1;
    }

    /* Both slots are on the TouchAria's own vtable, so nothing outside the
     * waveform title bar changes behaviour. */
    if (mod_patch_vslot("stemsPaint", EP122_TOUCHARIA, VT_SLOT_PAINT,
                        (void *)stems_ta_paint, &stems_g_orig_ta_paint) != 0) {
        MWARN("stems: no anchor -> feature off\n");
        stems_g_api_ok = 0;
        return -1;
    }
    mod_patch_vslot("stemsTouch", EP122_TOUCHARIA, VT_SLOT_MOUSEDOWN,
                    (void *)stems_ta_mousedown, &stems_g_orig_ta_mousedown);

    /* The clock for anything that moves. Not fatal if it is refused: the row still
     * builds and still works, it just goes back to updating only when something else
     * repaints the title bar -- so say so plainly rather than leaving a bar that
     * looks stuck with no explanation in the log. */
    if (mod_patch_vslot("stemsTick", EP122_DISPLAY_REFRESH, DRC_SLOT_TIMERCB,
                        (void *)stems_drc_timer, &stems_g_orig_drc_timer) != 0)
        MWARN("stems: no display tick -> the progress bar will not animate\n");

    MDBG("stems: installed (waiting for the waveform title bar to draw)\n");
    return 0;
}

KIT_MOD(k_mod_stems_ui,
        .name = "stems_ui", .prio = 30, .install = stems_ui_install,
        .what = "STEMS quick-menu button + stem control row");
