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

/* paint runs per frame and every build attempt costs two /proc/self/mem reads, so
 * attempts are capped. A few seconds of frames is far longer than the view takes to
 * finish wiring itself up. */
#define STEMS_MAX_ATTEMPTS 240

static void stems_try_build(uintptr_t anchor)
{
    static int attempts;

    if (stems_g_built || stems_g_building || !stems_g_api_ok) return;
    /* Nothing is allocated until ENABLE STEMS is on, and the attempt budget only
     * starts counting then. */
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
 * stem_ui_read() is a seqlock copy, so it never blocks the message thread; a torn
 * read costs one repaint. Polled only while our row is up. */
void stems_progress_poll(void)
{
    struct stem_ui_state st;
    int busy;

    if (!stem_ui_read(&st)) {
        stems_processing_set(NULL);
        return;
    }

    /* Everything between the request and the stems being resident counts as busy.
     * Named exclusions, not a range: LOADING sits above the two terminal values,
     * because the wire enum is the sidecar's and ours only extends it. */
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

/* Can STEMS do anything right now?
 *
 * Not the same as "is the server up": the on-media cache lets a deck with no network
 * play stems. Unavailable means no stems loaded for this track, none on the way, and
 * no usable server.
 *
 * Before the first STATUS, `reachable` is zero but means unknown, so this reports
 * available. */
int stems_available(void)
{
    struct stem_ui_state st;

    if (g_stem_ready)                       /* stems are loaded and playing */
        return 1;
    if (!stem_ui_read(&st) || !st.status_seen)
        return 1;                           /* nothing asked yet: say nothing */
    /* A pair loading off the media needs no server (for a long track this takes
     * about a minute). A separation's own LOADING leg falls through to the server
     * test below. */
    if (st.stage == STEM_STAGE_LOADING && !st.via_server)
        return 1;
    /* An unusable server wins over any job stage. This must stay above the running
     * test: failed jobs are retried every five seconds, and each retry would
     * otherwise flip the answer to available, hiding the badge and cutting the
     * refusal blink mid-flash (leaving the plate red). */
    if (!st.reachable || !st.compatible)
        return 0;
    if (st.stage != STEM_STAGE_IDLE && st.stage != STEM_STAGE_DONE &&
        st.stage != STEM_STAGE_FAILED)
        return 1;                           /* a job is running: it may well land */
    return 1;
}

/* g_stem_ready is written by the worker, so the row follows it on the display tick.
 * Does nothing unless the value changed. */
static void stems_ready_poll(void)
{
    static int shown = -1;
    int now = stems_ready();
    int i;

    if (now == shown) return;
    shown = now;
    /* Clear BYPASS when the stems go away, so it does not apply to the next set. */
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

/* The edit mark in the corner of the STEMS button: the track is not playing as it
 * came, i.e. a level is off unity or a groove circuit is active.
 *
 * Levels and the groove circuit persist while the row is closed, so the mark goes on
 * the title bar button, which is always visible. One colour for every cause: the
 * accent (blue: on, selected, working), or the text colour on the lit plate, where
 * the accent would vanish. BYPASS plays the track as it came, so it shows no mark. */
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

    /* Only on change: this runs at the display rate and setColour notifies
     * listeners on every call. */
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

    /* Log the badge decision whenever any of its inputs changes; they come from the
     * store, the job's UI snapshot and the sidecar's status. Note: a sidecar older
     * than the shim sends a short STATUS that handle_frame drops on the length
     * check, so `seen` stays 0 and stems_available keeps reporting available. */
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

    /* Appearing is damped, vanishing is not. On a track change the worker clears
     * g_stem_ready and publishes IDLE, and the cache lookup moves to a running stage
     * only tens of milliseconds later; without the delay an offline deck blips the
     * badge on every track change. */
    settle = want ? settle + 1 : 0;
    if (want != stems_g_warn_up && (!want || settle > WARN_SETTLE_TICKS)) {
        stems_g_warn_up = want;
        stems_set_visible(stems_g_warn, want);
        if (!want) {
            /* A blink cut short can leave the plate red, so force the resting
             * state. */
            stems_g_warn_blink = 0;
            stems_btn_state(BTN_OFF);
        }
        /* Close the row when the badge appears: moving from a cached track to an
         * uncached one with no server would otherwise leave faders that control
         * nothing.
         *
         * Safe here because our hook chains the app's callback first, so the
         * display pass is finished and re-laying out the band is what a tap does. */
        if (want && stems_g_row_open) {
            MDBG("stems: nothing left to control -> closing the row\n");
            stems_toggle_row();
        }
    }
    if (!want || !stems_g_warn_blink)
        return;
    /* One colour change per MOD_BLINK_PERIOD, not per tick (the tick is about 44 Hz).
     * The button and the badge flip together; the badge goes white on the red half
     * because amber on red is unreadable. */
    if (--stems_g_warn_blink % MOD_BLINK_PERIOD == 0) {
        /* The tap paints the first red itself, so the phase here starts cold. The
         * last tick is forced cold so the resting state matches what sync draws. */
        int hot = stems_g_warn_blink && !((stems_g_warn_blink / MOD_BLINK_PERIOD) & 1);

        stems_btn_state(hot ? BTN_REFUSE : BTN_OFF);
        stems_colour(stems_g_warn,      LBL_COL_TEXT, hot ? mod_ui()->text   : mod_ui()->warn);
    }
}

/* Push the roles back into the Labels we coloured at build time. Our own paint asks
 * mod_ui() per frame, but juce::Component::setColour copies the colour, so the STEMS
 * lettering and the warn badge keep the old theme until restamped. Runs only on a
 * theme change. */
static void stems_theme_restamp(void)
{
    const struct theme_ui *ui = mod_ui();
    int i;

    /* Not the progress rail: its colours are stored as stock (ORIGINAL) roles and the
     * theme hook resolves them at paint time. Restamping them with resolved values
     * puts the caption at #ffffff on WHITE's #ffffff plate. See mod_ui_stock. */
    stems_colour(stems_g_btn_stems, LBL_COL_TEXT, ui->text_deck);
    stems_colour(stems_g_warn,      LBL_COL_TEXT, ui->warn);
    /* Let the next tick resolve the edit mark's colour from the new palette. */
    stems_g_edit_col = 0;
    for (i = 0; i < N_STEMS; i++)
        stems_caption_sync(i);
    stems_repaint(stems_g_row);
    stems_repaint(stems_g_btn_stems);
    MDBG("theme: restamped the stem row for %s\n", mod_theme()->name);
}

/* Watch the theme id rather than hooking the setting, so every route is caught (the
 * MOD SETTINGS row, the settings file at startup). One int compare per tick. */
static void stems_theme_poll(void)
{
    static int last = -1;
    int id = __atomic_load_n(&g_theme_id, __ATOMIC_RELAXED);

    /* Do not record the id before the row exists, or a theme restored at startup is
     * never stamped. Only a restamp that happened may move `last`. */
    if (!stems_g_built) return;
    if (id == last) return;
    last = id;
    stems_theme_restamp();
}

/* The display tick, chained onto the app's refresh timer, so the progress row updates
 * every frame. Building the row stays in paint, where a repaint proves the tree exists;
 * this tick fires from the first frame, before the tree is ready. */
static void stems_drc_timer(void *self)
{
    static int ticks;
    static time_t t0;

    if (stems_g_orig_drc_timer)
        ((timercb_t)stems_g_orig_drc_timer)(self);
    stems_g_ticks++;

    /* The groove circuit's armed pad blinks off this clock: the panel writes a
     * lamp only when its own state changes. */
    cue_led_tick();

    /* Any follow-up a gate-cue release left pending. */
    cue_pad_tick();

    /* Collect the beat grid the deck provides on load; otherwise nothing reads it
     * until a pad is pressed. The X-PAD clock and the quantized MUTE depend on it. */
    stem_grid_tick();

    /* Log the tick rate once; every duration here is counted in ticks. */
    if (t0 == 0) t0 = time(NULL);
    else if (++ticks && t0 > 0 && time(NULL) - t0 >= 5) {
        MDBG("stems: display tick is %d Hz\n", ticks / (int)(time(NULL) - t0));
        t0 = -1;
    }

    if (!stems_g_built)
        return;
    /* Theme and band first: the band poll can close the row, and the polls below
     * would otherwise update a strip another panel is drawing over. */
    stems_theme_poll();
    kit_band_poll();
    if (stems_g_row_open) {
        stems_progress_poll();
        stems_wavewait_poll();
        stems_mute_blink();
        stems_ready_poll();
    }
    stems_warn_poll();
    /* Outside the row-open guard: the mark shows state the closed row hides. */
    stems_edit_poll();
}

/* The title bar draws whenever the loaded track or its labels change, with no user
 * action and with the parent chain already wired, so paint is the build trigger. */
static void stems_ta_paint(void *self, void *g)
{
    if (stems_g_orig_ta_paint)
        ((paint_t)stems_g_orig_ta_paint)(self, g);
    stems_try_build((uintptr_t)self);
    if (stems_g_built) stems_sync();
    /* Safety net for close paths that bypass our handlers (a screen change, the row
     * torn down under us): reconcile the band while our row is shut. One read per
     * repaint, and the title bar repaints rarely. */
    if (stems_g_built && !stems_g_row_open) kit_band_poll();
    /* The audio side's counters are printed here on the message thread, never from
     * the read()/operate() hooks. */
    mod_stem_audio_report();
    /* The progress row is polled from stems_drc_timer, not here: paint only fires on
     * invalidation. */
    /* One dump of the app's own open layout, for diffing against ours. */
    if (MLOG_AT(MOD_LOG_DEBUG) && stems_g_built && !stems_g_dumped_stock && kit_band_stock_up()) {
        stems_g_dumped_stock = 1;
        MDBG("tree --- STOCK open ---\n");
        stems_dump_tree(kit_band_view(), 0);
    }
}

/* Secondary trigger, for when the bar is drawn once before its parent is attached and
 * never invalidated again. Chained, since TouchAria overrides mouseDown. */
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
    /* Every juce primitive the row uses must have resolved; there is no partial row. */
    stems_g_api_ok = FN_ADD_VISIBLE && FN_SET_BOUNDS && FN_STR_DEFCTOR &&
               FN_FONT_BUILD && FN_LABEL_CTOR && FN_LABEL_SETFONT &&
               FN_LABEL_JUSTIFY && FN_VAR_DTOR && FN_VAR_FROM_STR &&
               FN_VALUE_SETVALUE &&
               /* The wedge paint needs these three; there is no fallback. */
               FN_GFX_SETCOLOUR && FN_GFX_FILLRECT && FN_COMP_REPAINT;
    if (!stems_g_api_ok) {
        MDBG("stems: juce primitives did not resolve -> feature off\n");
        return -1;
    }

    /* Both slots are on TouchAria's own vtable, so nothing outside the waveform
     * title bar changes behaviour. */
    if (mod_patch_vslot("stemsPaint", EP122_TOUCHARIA, VT_SLOT_PAINT,
                        (void *)stems_ta_paint, &stems_g_orig_ta_paint) != 0) {
        MWARN("stems: no anchor -> feature off\n");
        stems_g_api_ok = 0;
        return -1;
    }
    mod_patch_vslot("stemsTouch", EP122_TOUCHARIA, VT_SLOT_MOUSEDOWN,
                    (void *)stems_ta_mousedown, &stems_g_orig_ta_mousedown);

    /* The clock for anything that moves. Not fatal: without it the row still works
     * but updates only when the title bar repaints. */
    if (mod_patch_vslot("stemsTick", EP122_DISPLAY_REFRESH, DRC_SLOT_TIMERCB,
                        (void *)stems_drc_timer, &stems_g_orig_drc_timer) != 0)
        MWARN("stems: no display tick -> the progress bar will not animate\n");

    MDBG("stems: installed (waiting for the waveform title bar to draw)\n");
    return 0;
}

KIT_MOD(k_mod_stems_ui,
        .name = "stems_ui", .prio = 30, .install = stems_ui_install,
        .what = "STEMS quick-menu button + stem control row");
