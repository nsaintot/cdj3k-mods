// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * kit/band.c - borrowing the strip under the waveform.
 *
 * The contract is in band.h. Each offset is documented where it is used.
 */
#include "kit/band.h"
#include "juce/juce.h"
#include "kit/mod.h"

/* ---- the anchor: gui::WaveformViewTitleWidget ----------------------------
 *
 * The bar carrying the artwork, the track labels and the three quick-menu
 * buttons. It inherits juce::Component virtually, so its vtable group is awkward
 * to hook, but its plain Component child `TouchAria` has the bar as parent. A
 * client hooks TouchAria's paint and passes us its `this`. */
#define BAR_W               0x500   /* 1280x90 -- checked before we attach */
#define BAR_H               0x5a

/* ---- the three stock quick-menu buttons ----
 *
 * They share one listener, kept in the array at button+0x1f8 and reached through
 * its vtable. Hooking the button's mouseDown is simpler than implementing a
 * listener and tells arbitration that a stock quick menu was picked. */
#define BTN_LISTENERS_OFF   0x1f8   /* Array<IListener*>: data ptr, then cap/count */

/* ---- the mode register, which lays the waveform out ----
 *
 * The shared listener is `gui::WaveformView`. Its press handler maps the button's
 * EasyID to a mode and calls setQuickMenuMode(owner, mode), which stores the mode
 * at owner+0x3b8 and posts a state-machine event; the layout, including the
 * waveform's internal redraw scale, follows from it. Setting bounds by hand does
 * not reproduce that scale: identical component bounds still draw differently.
 *
 * setQuickMenuMode branches only on `mode == 0` (post the close event) and
 * `mode == 4` (take the panel from owner+0x378 instead of owner+0xe8); any other
 * value is stored and opens the band. So a mode past the app's own four gets the
 * layout with no stock panel attached. */
#define FN_QM_SETMODE       ep122_sym(EP122_SET_QUICKMENU_MODE)
#define QM_OWNER_OFF        0xd00    /* owner pointer, read off the listener */
#define QM_MODE_OFF         0x3b8    /* the mode it stores */
#define QM_MODE_NONE        0
/* The app's grid mode, a last resort for a firmware that rejects an unused value
 * downstream. It is a real panel, so a client using it leaves the deck in GRID
 * with that panel under ours; the watch below cannot use it. */
#define QM_MODE_STRIP       4

/* The footer's clearance above the panel band, used to derive how far the app
 * slides its layout. */
#define PANEL_GAP           10

/* ---- state --------------------------------------------------------------- */

static uintptr_t band_g_bar;        /* the title bar: a quick-menu button's parent */
static uintptr_t band_g_host;       /* the bar's parent -- the whole player view   */
static uintptr_t band_g_view;       /* the waveform view: panels and waveform      */
static int32_t   band_g_rect[4];    /* the rect the stock quick-menu panels share  */
static int       band_g_ready;

static int       band_g_qm_ok;      /* setQuickMenuMode resolved at install */
static uintptr_t band_g_qm_owner;
static uintptr_t band_g_orig_btn_mousedown;

/* Who has it, and by which route. `claimed` is the mode we got, meaningful only
 * while `used_qm` is set; see kit_band_poll for why the fallback mode cannot be
 * watched. */
static const struct kit_band *band_g_holder;
static int       band_g_used_qm;
static int32_t   band_g_claimed = -1;
static int       band_g_shrunk;     /* we applied a by-hand shrink and owe a restore */

/* Client components, excluded by identity from the shape-based scans for the
 * app's panels and buttons, which they would otherwise match. */
#define BAND_MAX_OWN 16
static uintptr_t band_g_own[BAND_MAX_OWN];
static int       band_g_nown;

/* Both layouts of each component that moves when the band opens, computed once.
 * Applying is a straight write of one or the other, so open and close are
 * symmetric. */
#define BAND_MAX_SNAP 32
struct band_snap { uintptr_t comp; int32_t closed[4], open[4]; };
static struct band_snap band_g_snap[BAND_MAX_SNAP];
static int       band_g_nsnap;
static int32_t   band_g_delta;      /* how far the app slides to free the rect */
static int32_t   band_g_tall_h, band_g_tall_y;

static int band_is_ours(uintptr_t comp)
{
    int i;

    for (i = 0; i < band_g_nown; i++)
        if (band_g_own[i] == comp) return 1;
    return 0;
}

void kit_band_own(uintptr_t comp)
{
    if (!comp || band_is_ours(comp)) return;
    if (band_g_nown >= BAND_MAX_OWN) {
        MDBG("band: more than %d client components -> %#lx not excluded from the scans\n",
             BAND_MAX_OWN, (unsigned long)comp);
        return;
    }
    band_g_own[band_g_nown++] = comp;
}

uintptr_t      kit_band_bar(void)  { return band_g_bar; }
uintptr_t      kit_band_view(void) { return band_g_view; }
const int32_t *kit_band_rect(void) { return band_g_rect; }

/* ---- finding the tree ---------------------------------------------------- */

/* Find the waveform view and the rect its quick-menu panels share: three or more
 * siblings with identical bounds spanning the view's full width. No rect is
 * hardcoded, so a different skin or panel count still resolves. */
static int band_find_panel_slot(uintptr_t host, uintptr_t *view_out, int32_t rect[4])
{
    int nh = juce_comp_nchild(host), i, j, k;

    for (i = 0; i < nh; i++) {
        uintptr_t view = juce_comp_child(host, i);
        int32_t vb[4], ab[4], bb[4];
        int nv, same;

        if (!view || juce_comp_bounds(view, vb) != 0) continue;
        nv = juce_comp_nchild(view);
        for (j = 0; j < nv; j++) {
            uintptr_t a = juce_comp_child(view, j);

            if (!a || juce_comp_bounds(a, ab) != 0) continue;
            if (ab[0] != 0 || ab[2] != vb[2] || ab[3] <= 0 || ab[3] >= vb[3]) continue;
            same = 0;
            for (k = 0; k < nv; k++) {
                uintptr_t b = juce_comp_child(view, k);

                if (k == j || !b) continue;
                if (juce_comp_bounds(b, bb) == 0 &&
                    bb[0] == ab[0] && bb[1] == ab[1] &&
                    bb[2] == ab[2] && bb[3] == ab[3])
                    same++;
            }
            if (same >= 2) {                    /* three or more identical siblings */
                *view_out = view;
                rect[0] = ab[0]; rect[1] = ab[1]; rect[2] = ab[2]; rect[3] = ab[3];
                return 0;
            }
        }
    }
    return -1;
}

/* ---- the track title, cut back to leave a second button slot -------------
 *
 * Found by shape, not index (the bar has thirteen children in app-defined order).
 * x, y and height are fixed and only the width changes, so matching on those plus
 * a width wider than the target is exact and idempotent.
 *
 * The component is cached because this runs on the display tick: the app resets
 * the title's bounds whenever a track loads. Cached, the tick costs one bounds
 * read. */
static uintptr_t band_g_title;
static int32_t   band_g_title_w0;   /* the width the app built it with */

/* ---- the slots, packed over the enabled clients ---------------------------
 *
 * A disabled client has no button and holds no slot: X-PAD alone sits where STEMS
 * would be, and with both off the track title gets its full width back.
 *
 * Packed rightward from the app's gap at x=774 in kit_band::order, not link
 * order. */
static int band_slots_used(void)
{
    const struct kit_band *c;
    int n = 0;

    for (c = __start_ep122_band; c < __stop_ep122_band; c++)
        if (!c->shown || *c->shown) n++;
    return n;
}

int32_t kit_band_slot_x(const struct kit_band *c)
{
    const struct kit_band *p;
    int n = 0;

    if (!c) return KIT_BAND_SLOT_X(0);
    if (c->shown && !*c->shown) return KIT_BAND_SLOT_X(0);
    /* How many shown clients sit nearer the app's own buttons than this one. */
    for (p = __start_ep122_band; p < __stop_ep122_band; p++)
        if (p != c && (!p->shown || *p->shown) && p->order < c->order)
            n++;
    return KIT_BAND_SLOT_X(n);
}

/* The title width: 10px clear of the leftmost slot in use, or the app's original
 * width when no client has a button. */
static int32_t band_title_w(void)
{
    int n = band_slots_used();

    if (n <= 0) return band_g_title_w0;
    return KIT_BAND_SLOT_X(n - 1) - KIT_BAND_TITLE_X - 10;
}

static void band_title_squeeze(void)
{
    int32_t want, b[4];
    int n, i;

    if (!band_g_bar) return;

    if (!band_g_title) {
        n = juce_comp_nchild(band_g_bar);
        for (i = 0; i < n; i++) {
            uintptr_t c = juce_comp_child(band_g_bar, i);

            if (!c || band_is_ours(c)) continue;
            if (juce_comp_bounds(c, b) != 0) continue;
            if (b[0] != KIT_BAND_TITLE_X || b[1] != KIT_BAND_TITLE_Y ||
                b[3] != KIT_BAND_TITLE_H || b[2] <= KIT_BAND_TITLE_W) continue;
            band_g_title    = c;
            band_g_title_w0 = b[2];
            MDBG("band: track title {%d,%d,%d,%d}, %d slot(s) in use\n",
                 b[0], b[1], b[2], b[3], band_slots_used());
            break;
        }
        if (!band_g_title) return;
    }

    /* Works in both directions: the app resets these bounds on every track load,
     * and disabling a client gives width back. */
    want = band_title_w();
    if (juce_comp_bounds(band_g_title, b) != 0 || b[2] == want) return;
    ((void (*)(void *, int, int, int, int))FN_SET_BOUNDS)
        ((void *)band_g_title, b[0], b[1], want, b[3]);
}

void kit_band_slots_changed(void)
{
    const struct kit_band *c;

    for (c = __start_ep122_band; c < __stop_ep122_band; c++)
        if (c->reslot && (!c->shown || *c->shown))
            c->reslot(kit_band_slot_x(c));
    band_title_squeeze();
}

/* `touch_aria` is a TouchAria, whose parent is the title bar's Component base. */
int kit_band_attach(uintptr_t touch_aria)
{
    uintptr_t bar, host;
    int32_t bb[4];

    if (band_g_ready) return 0;

    bar = juce_comp_parent(touch_aria);
    if (!bar || juce_comp_bounds(bar, bb) != 0) return -1;
    if (bb[2] != BAR_W || bb[3] != BAR_H) {
        MDBG("band: anchor parent is {%d,%d,%d,%d}, not the %dx%d title bar -> skip\n",
             bb[0], bb[1], bb[2], bb[3], BAR_W, BAR_H);
        return -1;
    }
    host = juce_comp_parent(bar);
    if (!host) {
        MDBG("band: title bar has no parent yet -> retry on the next paint\n");
        return -1;
    }
    if (band_find_panel_slot(host, &band_g_view, band_g_rect) != 0) {
        MDBG("band: no quick-menu panel slot found yet -> retry on the next paint\n");
        return -1;
    }
    band_g_bar   = bar;
    band_g_host  = host;
    band_g_ready = 1;
    band_title_squeeze();
    MDBG("band: bar %#lx view %#lx panel {%d,%d,%d,%d}\n",
         (unsigned long)bar, (unsigned long)band_g_view,
         band_g_rect[0], band_g_rect[1], band_g_rect[2], band_g_rect[3]);
    return 0;
}

/* The mode owner, reached the way the app's handler reaches it: any stock
 * quick-menu button -> its listener array -> the shared WaveformView -> +0xd00.
 * Walked eagerly so the first tap already has it, and validated by reading the
 * mode back.
 *
 * The valid range includes client modes: if a previous session left the band
 * borrowed, rejecting them would make the owner undiscoverable and the band
 * impossible to hand back. */
static int band_mode_plausible(int32_t mode)
{
    const struct kit_band *c;

    if (mode >= QM_MODE_NONE && mode <= QM_MODE_STRIP) return 1;
    for (c = __start_ep122_band; c < __stop_ep122_band; c++)
        if (mode == c->mode) return 1;
    return 0;
}

static uintptr_t band_qm_owner(void)
{
    int n, i;

    if (band_g_qm_owner || !band_g_bar || !band_g_qm_ok) return band_g_qm_owner;
    n = juce_comp_nchild(band_g_bar);
    for (i = 0; i < n; i++) {
        uintptr_t c = juce_comp_child(band_g_bar, i), arr = 0, lis = 0, owner = 0;
        int32_t b[4], mode = -1;

        if (!c || band_is_ours(c)) continue;
        if (juce_comp_bounds(c, b) != 0 || b[2] != KIT_BAND_BTN_W || b[3] != KIT_BAND_BTN_H) continue;
        if (mod_safe_read(c + BTN_LISTENERS_OFF, &arr, sizeof(arr)) != 0 || !arr) continue;
        if (mod_safe_read(arr, &lis, sizeof(lis)) != 0 || !lis) continue;
        if (mod_safe_read(lis + QM_OWNER_OFF, &owner, sizeof(owner)) != 0 || !owner) continue;
        if (mod_safe_read(owner + QM_MODE_OFF, &mode, sizeof(mode)) != 0) continue;
        if (!band_mode_plausible(mode)) continue;
        band_g_qm_owner = owner;
        MDBG("band: quick-menu owner %#lx via button at x=%d (mode %d)\n",
             (unsigned long)owner, b[0], mode);
        break;
    }
    return band_g_qm_owner;
}

static int band_mode_read(int32_t *out)
{
    uintptr_t owner = band_qm_owner();

    *out = -1;
    if (!owner) return -1;
    return mod_safe_read(owner + QM_MODE_OFF, out, sizeof(*out));
}

/* ---- the closed layout, and the shrink ----------------------------------- */

static void band_record(uintptr_t c, const int32_t b[4], int dh, int dy)
{
    struct band_snap *s;

    if (band_g_nsnap >= BAND_MAX_SNAP) return;
    s = &band_g_snap[band_g_nsnap++];
    s->comp = c;
    s->closed[0] = b[0];      s->closed[1] = b[1];
    s->closed[2] = b[2];      s->closed[3] = b[3];
    s->open[0]   = b[0];      s->open[1]   = b[1] + dy;
    s->open[2]   = b[2];      s->open[3]   = b[3] + dh;
}

/* Shrinking the waveform's frame is not enough: every child has to follow, or the
 * drawing keeps its old size and is clipped. Three shapes, told apart by how each
 * sits in the closed frame:
 *
 *   playhead        {348,0,5,237}    y == 0 and full height   -> shorten
 *   render layers   {0,8,1280,221}   inset equally top/bottom -> shorten (this is
 *                                     the one that makes the waveform redraw
 *                                     smaller rather than clipped)
 *   beat ruler etc. {0,218,1280,19}  flush with the bottom    -> move up
 */
static void band_snapshot_waveform(uintptr_t wf, int32_t closed_h)
{
    int n = juce_comp_nchild(wf), i;

    for (i = 0; i < n; i++) {
        uintptr_t c = juce_comp_child(wf, i);
        int32_t b[4];

        if (!c || juce_comp_bounds(c, b) != 0) continue;
        if (b[1] == 0 && b[3] == closed_h)                   band_record(c, b, -band_g_delta, 0);
        else if (b[1] > 0 && b[1] + b[3] + b[1] == closed_h)  band_record(c, b, -band_g_delta, 0);
        else if (b[1] + b[3] == closed_h)                     band_record(c, b, 0, -band_g_delta);
    }
}

/* Whether this child is one of the app's quick-menu panels (has the shared rect). */
static int band_is_panel(const int32_t b[4])
{
    return b[0] == band_g_rect[0] && b[1] == band_g_rect[1] &&
           b[2] == band_g_rect[2] && b[3] == band_g_rect[3];
}

/* Snapshot the closed layout of everything in the view that is not a panel, and
 * work out how far the app slides it when a panel opens. Only valid while no
 * panel is showing; a snapshot of a shrunk layout would restore to the wrong
 * place. */
void kit_band_snapshot(void)
{
    int n, i, bottom = 0;
    uintptr_t tall = 0;
    int32_t tb[4];

    if (!band_g_ready) return;
    n = juce_comp_nchild(band_g_view);
    band_g_nsnap  = 0;
    band_g_tall_h = 0;
    /* Two passes: `delta` needs the content bottom and the tallest child before
     * any entry's open bounds can be computed. */
    for (i = 0; i < n; i++) {
        uintptr_t c = juce_comp_child(band_g_view, i);
        int32_t b[4];

        if (!c || band_is_ours(c)) continue;
        if (juce_comp_bounds(c, b) != 0 || !juce_comp_visible(c) || band_is_panel(b)) continue;
        if (b[1] + b[3] > bottom) bottom = b[1] + b[3];
        if (b[3] > band_g_tall_h) { band_g_tall_h = b[3]; band_g_tall_y = b[1]; tall = c; }
    }
    band_g_delta = bottom + PANEL_GAP - band_g_rect[1];
    if (band_g_delta <= 0 || !tall) {
        MDBG("band: layout not resolvable (bottom %d delta %d) -> strips will overlay\n",
             bottom, band_g_delta);
        band_g_delta = 0;
        return;
    }

    for (i = 0; i < n; i++) {
        uintptr_t c = juce_comp_child(band_g_view, i);
        int32_t b[4];

        if (!c || band_is_ours(c)) continue;
        if (juce_comp_bounds(c, b) != 0 || !juce_comp_visible(c) || band_is_panel(b)) continue;
        if (c == tall)                   band_record(c, b, -band_g_delta, 0);
        else if (b[1] > band_g_tall_y)   band_record(c, b, 0, -band_g_delta);
    }
    juce_comp_bounds(tall, tb);
    band_snapshot_waveform(tall, tb[3]);
    MDBG("band: layout snapshot %d movers, content bottom %d, delta %d\n",
         band_g_nsnap, bottom, band_g_delta);
}

/* Reproduce what the app does when one of its own panels opens, by writing one
 * of the two precomputed layouts. */
static void band_layout(int shrink)
{
    int i;

    if (band_g_nsnap <= 0 || band_g_delta <= 0) return;
    for (i = 0; i < band_g_nsnap; i++) {
        const int32_t *b = shrink ? band_g_snap[i].open : band_g_snap[i].closed;

        ((void (*)(void *, int, int, int, int))FN_SET_BOUNDS)
            ((void *)band_g_snap[i].comp, b[0], b[1], b[2], b[3]);
    }
    band_g_shrunk = shrink;
}

int kit_band_stock_up(void)
{
    int n, i;

    if (!band_g_ready) return 0;
    n = juce_comp_nchild(band_g_view);
    for (i = 0; i < n; i++) {
        uintptr_t c = juce_comp_child(band_g_view, i);
        int32_t b[4];

        if (!c || band_is_ours(c)) continue;
        if (juce_comp_bounds(c, b) == 0 && juce_comp_visible(c) && band_is_panel(b))
            return 1;
    }
    return 0;
}

/* ---- holding it ---------------------------------------------------------- */

/* Ask the app to open (or close) the band as its own buttons do.
 *
 *   1  the mode took
 *   0  the owner could not be reached; the caller may fall back
 *  -1  the app refused and wrote the register back. With no track loaded there
 *      is no waveform to shrink and no panel rect, so it refuses its own GRID
 *      mode too.
 *
 * Distinguishing 0 from -1 lets a caller obey a refusal and still fall back by
 * hand where the setter never resolved. */
static int band_qm_mode(int32_t mode)
{
    uintptr_t owner = band_qm_owner();
    int32_t got = -1;

    if (!owner) return 0;
    ((void (*)(void *, int))FN_QM_SETMODE)((void *)owner, mode);
    if (mod_safe_read(owner + QM_MODE_OFF, &got, sizeof(got)) != 0 || got != mode) {
        MDBG("band: quick-menu mode %d refused (owner now %d)\n", mode, got);
        return -1;
    }
    return 1;
}

/* The app never writes a client's mode, so the mode register is the
 * authoritative record that we shrank the band. Reading it back means a desynced
 * flag cannot leave the band shrunk with no panel in it.
 *
 * The QM_MODE_STRIP fallback is the app's own GRID mode, so seeing it says
 * nothing about who opened it, and clearing it would close a stock panel. That
 * case is tracked by band_g_used_qm instead. */
static int band_qm_release(void)
{
    int32_t mode = -1;

    if (band_mode_read(&mode) != 0) return 0;
    if (mode == QM_MODE_NONE || mode <= QM_MODE_STRIP) return 0;
    if (!band_mode_plausible(mode)) return 0;
    ((void (*)(void *, int))FN_QM_SETMODE)((void *)band_g_qm_owner, QM_MODE_NONE);
    band_g_used_qm = 0;
    MDBG("band: handed back (mode %d -> %d)\n", mode, QM_MODE_NONE);
    return 1;
}

/* The holder loses the band. Tells the client and forgets it without touching
 * the mode: every caller is about to write it or has just seen someone else
 * write it. */
static void band_drop_holder(void)
{
    const struct kit_band *h = band_g_holder;

    band_g_holder = 0;
    if (h && h->closed) h->closed();
}

int kit_band_holds(const struct kit_band *c)
{
    return band_g_holder == c;
}

/* Take the band for a client's strip.
 *
 * Preferred path: borrow the app's quick-menu mode so the app lays out and
 * rescales the waveform. The by-hand layout is the fallback when the owner
 * cannot be reached.
 *
 * The mode is claimed even when a stock panel already has the band open:
 * setting the mode is the only way to hide that panel's controls, matching how
 * any quick-menu button closes the previous one. */
int kit_band_take(const struct kit_band *c)
{
    int mine, strip;

    if (!c) return 0;
    if (band_g_holder && band_g_holder != c) band_drop_holder();
    band_g_holder = c;

    mine = band_qm_mode(c->mode);
    if (mine > 0)  { band_g_used_qm = 1; band_g_claimed = c->mode;       return 1; }
    strip = band_qm_mode(QM_MODE_STRIP);
    if (strip > 0) { band_g_used_qm = 1; band_g_claimed = QM_MODE_STRIP; return 1; }

    /* The app refused (no track loaded), so do not open. A by-hand shrink here
     * draws the row across the rekordbox logo with grey wedges and the poll
     * toggling it open and shut. Nothing was taken, so nothing to give back. */
    if (mine < 0 || strip < 0) {
        band_g_holder = 0;
        band_g_claimed = -1;
        return -1;
    }
    band_layout(1);
    band_g_claimed = -1;
    return 0;
}

/* Give it back, by whichever route it was taken. */
void kit_band_give(const struct kit_band *c)
{
    if (band_g_holder != c) return;
    band_g_holder = 0;
    if (band_qm_release()) return;
    if (band_g_used_qm)    { band_qm_mode(QM_MODE_NONE); band_g_used_qm = 0; }
    else if (band_g_shrunk) band_layout(0);
}

/* Detect another owner taking the band.
 *
 * The mode register is the app's record of which panel owns the band, and every
 * route writes it, including ones that never press a title-bar button. GRID
 * ADJUST opens on a one-second rotary hold, so no button hook runs; without this
 * watch two panels would draw into the same strip. Watching the register also
 * covers routes not yet identified.
 *
 * The band is not handed back here: whoever wrote that mode owns it now, and
 * releasing would close their panel.
 *
 * The STRIP fallback is not watched: it is the app's grid mode, so the register
 * reads the same whether we hold it or grid adjust took it, and the watch would
 * close the row on the tick after it opened. */
void kit_band_poll(void)
{
    int32_t mode = -1;

    if (!band_g_ready) return;
    band_title_squeeze();

    if (band_g_holder) {
        if (!band_g_used_qm || band_g_claimed != band_g_holder->mode) return;
        if (band_mode_read(&mode) != 0 || mode == band_g_holder->mode) return;
        MDBG("band: %s lost the band (mode %d)\n", band_g_holder->name, mode);
        band_g_used_qm = 0;      /* theirs now: not ours to hold or return */
        band_g_claimed = -1;
        band_drop_holder();
        return;
    }

    /* No client holds it, so a client mode left in the register means an empty
     * strip under the waveform. Every close path reaches this. */
    band_qm_release();
}

/* ---- the stock buttons --------------------------------------------------- */

/* A stock quick menu taking over closes ours; only one panel is up at a time.
 * The button class is generic, so only presses from buttons parented to our
 * title bar are acted on. */
static void band_btn_mousedown(void *self, void *event)
{
    int took_over = 0;

    if (band_g_holder && band_g_bar &&
        juce_comp_parent((uintptr_t)self) == band_g_bar && !band_is_ours((uintptr_t)self)) {
        int32_t b[4];

        juce_comp_bounds((uintptr_t)self, b);
        MDBG("band: stock quick menu at x=%d took over from %s\n",
             b[0], band_g_holder->name);
        took_over = 1;
        /* Only a by-hand shrink is undone here, before chaining, or the two
         * shrinks would compound. The borrowed mode is settled after the chain. */
        if (band_g_shrunk) band_layout(0);
        band_drop_holder();
    }
    if (band_g_orig_btn_mousedown)
        ((void (*)(void *, void *))band_g_orig_btn_mousedown)(self, event);
    /* Settled after chaining. Opening a stock panel overwrites our mode, so the
     * common case costs one read with no flicker. A press that closes the stock
     * button's own panel, or one on a button whose panel is already up, may leave
     * our mode in place; releasing afterwards covers both. */
    if (took_over) band_qm_release();
}

int kit_band_install(void)
{
    band_g_qm_ok = FN_QM_SETMODE != 0 && FN_SET_BOUNDS != 0;
    if (!band_g_qm_ok) {
        MDBG("band: no setQuickMenuMode -> strips will overlay the waveform\n");
        return -1;
    }
    mod_patch_vslot("bandQmBtn", EP122_BTN, JUCE_VT_MOUSEDOWN,
                    (void *)band_btn_mousedown, &band_g_orig_btn_mousedown);
    return 0;
}

/* Installed before every client, whose install may check whether the band can be
 * borrowed. A failure is not fatal to them: kit_band_take falls back to a by-hand
 * shrink. */
KIT_MOD(k_mod_band,
        .name = "band", .prio = 5, .install = kit_band_install,
        .what = "the waveform band, shared by the panels that borrow it");
