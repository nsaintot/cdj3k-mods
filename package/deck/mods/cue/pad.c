// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * cue/pad.c - the one place that hooks the deck's hot-cue path.
 *
 * Every slot the hot-cue behaviours need is taken here, decoded once, and
 * handed round as a cue_event. The behaviours (gate.c, smart.c, preview.c) hold
 * no addresses and repeat no ABI; see cue.h for the contract.
 *
 * Mechanism (RE-verified against EP122-3.19; non-PIE ET_EXEC @0x400000)
 * --------------------------------------------------------------------
 * A physical hot-cue pad is handled by input_devices::DeckOperationPadHandler
 * <HotCueOperation>. The pad DOWN/UP each post a meow::AsyncTask whose "run"
 * slot (vtable+0x10, in .rodata) executes on the deck thread:
 *   - pressPad   run  sub_183cf98  (vtable 0x21f7168) -> jump to cue + PLAY
 *   - releasePad run  sub_183d130  (vtable 0x21f7198) -> pad state bookkeeping
 * PLAY posts PlayPauseHandler::pushOn run sub_185ae50 (vtable 0x21fbe00).
 * Those three run slots are repointed at wrappers that call the stock run back
 * through saved pointers.
 *
 * The release already carries what it MEANS. Once sub_183d130 pops the last
 * held pad it hands &closure[0x28] to the handler's own release method
 * (vtable+0x20, the default sub_183b348), which reads the pad index at +0 and
 * dispatches on an op word at +4: 1 returns to the cue and pauses, anything else
 * is the plain release. So a behaviour that wants a back-cue writes that word
 * and the DECK's release performs it -- see cue_set_release_op.
 *
 * The preview needle comes from usecase::deck::PreviewController, which is what
 * the strip's touch drives: slot +0x10 carries the touched point as a
 * normalised fraction and +0x18 clears it. Hooking the pair is how the layer
 * knows the zone is held without going anywhere near the strip's own drawing.
 *
 * Every input handler embeds a dj_player::PlayerState at +0x80, the deck's own
 * snapshot of the player. Its flag bytes at +0x65..+0x67 read as one state --
 * sub_11459f0: +0x65 -> 0, else +0x67 ? 2 : 1, +0x66 -> 3 -- and +0x67 is the
 * one that says PLAYING. Read at the press, off the handler the closure names.
 */
#include "cue/cue.h"
#include "kit/mod.h"

/* ================================================================== */
/* EP122-3.19 ABI                                                     */
/* ================================================================== */

#define CUE_RUN_SLOT       0x10   /* AsyncTask::run, on every closure class */
#define PREVIEW_SET_SLOT   0x10   /* PreviewController: the touched point   */
#define PREVIEW_CLEAR_SLOT 0x18   /* ...and letting go                      */

/* dj_player::PlayerState inside every input handler, and its play byte. */
#define HANDLER_STATE_OFF  0x80
#define STATE_PLAYING_OFF  0x67

#define FN_LINK_DECK       ep122_sym(EP122_CUE_LINK_DECK)
#define FN_LINK_FACADE     ep122_sym(EP122_CUE_LINK_FACADE)
#define PADREL_SENTINEL    ep122_sym(EP122_GATE_PADREL_SENTINEL)

/* The pad handler reaches the CueController the stock press path uses:
 * handler+0x30 is a meow::MappedObjPtr whose vtable[0x38] hands it out. */
#define DECK_RESOLVE_OFF     0x38
#define HANDLER_DECK_MOP_OFF 0x30

/* The shared pad/release closure. The last two are ONE pair as far as the deck
 * is concerned: the stock release hands `&closure[0x28]` to the handler's own
 * release method, which reads the index at +0 and dispatches on the op at +4. */
#define CLOSURE_HANDLER_OFF     0x18  /* handler `this` pointer                */
#define CLOSURE_PAD_OFF         0x28  /* the pad's own index, 0-based (A=0)    */
#define CLOSURE_RELEASE_OP_OFF  0x2c  /* what the release means; 1 = back-cue  */

/* The handler's release method, and the stock body that reads the op above. */
#define HANDLER_RELEASE_SLOT    0x20

/* The deck's press run returns this when the pad was already assigned. It is
 * also what a CLAIMED press returns: nothing ran, so there is nothing to report,
 * and 0 is the value the task machinery treats as "no news". */
#define STATUS_ASSIGNED      0

/* From the CueController to the cue table:
 *   facade = *(cc + 0x18)      MappedObjPtr<ICueLoopSetter>, linked by the stock
 *                              press path before any of this runs
 *   P      = *(facade + 0x70)  the parent everything cue hangs off
 *   table  = *(P + 0x40)       one object, reached three ways -- the engine takes
 *                              it from +0x18, +0x40 and +0x60, and all three read
 *                              the same pointer live
 *   slot   = *(table + 0x10 + kind*8)        sub_10d67d8 */
#define FACADE_MOP_OFF      0x18
#define FACADE_PARENT_OFF   0x70
#define PARENT_TABLE_OFF    0x40
#define TABLE_SLOT0_OFF     0x10

typedef int64_t (*task_run_t)(void *task);
typedef int64_t (*link_fn_t)(void *mop);
typedef void   *(*resolve_fn_t)(void *self);
typedef int64_t (*cueing_fn_t)(void *cc, uint32_t kind, uint32_t slip, uint32_t quantize);

/* ================================================================== */
/* State                                                              */
/* ================================================================== */

static const struct cue_handler *cue_g_order[16];
static int cue_g_n;
static int cue_g_ready;

static uintptr_t cue_g_orig_pp;
static uintptr_t cue_g_orig_rp;
static uintptr_t cue_g_orig_play;
static uintptr_t cue_g_orig_cueing;
static uintptr_t cue_g_orig_prev_set;
static uintptr_t cue_g_orig_prev_clear;

/* Pads currently down. [deck], but PLAY arrives on its own task, so atomic. */
static int cue_g_held;

/* The preview needle. Written from [message], read from [deck]. */
static int   cue_g_needle_up;
static float cue_g_needle_at;

int cue_pad_ready(void) { return cue_g_ready; }
int cue_pads_held(void) { return __atomic_load_n(&cue_g_held, __ATOMIC_ACQUIRE); }

int cue_deck_playing(const struct cue_event *ev)
{
    uintptr_t handler = 0;
    uint8_t   st[4];

    if (mod_safe_read((uintptr_t)ev->task + CLOSURE_HANDLER_OFF, &handler, sizeof(handler)) != 0 ||
        !handler ||
        mod_safe_read(handler + HANDLER_STATE_OFF + 0x64, st, sizeof(st)) != 0)
        return 0;
    MDBG("cue: player state %02x %02x %02x %02x\n", st[0], st[1], st[2], st[3]);
    return st[STATE_PLAYING_OFF - 0x64] != 0;
}

/* ================================================================== */
/* Decoding an event                                                  */
/* ================================================================== */

static int cue_decode(void *task, struct cue_event *ev)
{
    int32_t pad = 0;

    if (mod_safe_read((uintptr_t)task + CLOSURE_PAD_OFF, &pad, sizeof(pad)) != 0)
        return -1;
    /* A pad outside the bank means the closure is not laid out the way this
     * file assumes, and every offset below it is then a guess. Refuse the whole
     * event rather than index anything with it. */
    if (pad < 0 || pad >= CUE_PADS) {
        static int said;

        if (!said) {
            said = 1;
            MERR("cue: closure[%#x] = %d, not a pad 0..%d -> events passed through\n",
                 CLOSURE_PAD_OFF, (int)pad, CUE_PADS - 1);
        }
        return -1;
    }
    ev->task      = task;
    ev->pad       = (int)pad;
    ev->kind      = (int)pad + 1;
    ev->status    = 0;
    ev->assigned  = 0;
    /* A snapshot, so every handler in one dispatch sees the same needle even if
     * the message thread lets go halfway through. */
    ev->needle_up = __atomic_load_n(&cue_g_needle_up, __ATOMIC_ACQUIRE);
    ev->needle_at = cue_g_needle_at;
    return 0;
}

static void cue_dispatch(const struct cue_event *ev, enum cue_phase phase)
{
    int i;

    for (i = 0; i < cue_g_n; i++) {
        const struct cue_handler *h = cue_g_order[i];

        if (!h->pad)
            continue;
        /* A behaviour that TAKES presses is not an observer of the ones it let
         * through. It sees DOWN for every pad -- that is where it decides -- and
         * then only the pads it actually owns, delivered straight to it rather
         * than from here.
         *
         * Without this a claimer's own PRESSED runs for pads it DECLINED, on
         * whatever state the last claim left behind: measured as an empty pad
         * being set by the deck and simultaneously starting a loop over the
         * previous pad's span. */
        if (h->pad_claim && phase != CUE_PAD_DOWN)
            continue;
        h->pad(ev, phase);
    }
}

/* ================================================================== */
/* Helpers behaviours call                                            */
/* ================================================================== */

void *cue_controller(const struct cue_event *ev)
{
    uintptr_t handler = 0, resolver = 0, rvt = 0, resolve = 0;

    if (mod_safe_read((uintptr_t)ev->task + CLOSURE_HANDLER_OFF, &handler, sizeof(handler)) != 0)
        return NULL;
    /* LINK IT FIRST. The MappedObjPtr at +0x30 caches its pointer on first use
     * and starts empty, so reading the slot raw found nothing until the deck's
     * own press path had run once -- and the first pad press after a track load
     * fell through every behaviour that needs the controller. The deck's own
     * back-cue links the same member the same way before resolving it, and a
     * pointer already cached costs a load and a branch. */
    if (FN_LINK_DECK)
        ((link_fn_t)FN_LINK_DECK)((void *)(handler + HANDLER_DECK_MOP_OFF));
    if (mod_safe_read(handler + HANDLER_DECK_MOP_OFF, &resolver, sizeof(resolver)) != 0 || !resolver) {
        MDBG("cue: deck resolver would not link -> skip\n");
        return NULL;
    }
    if (mod_safe_read(resolver, &rvt, sizeof(rvt)) != 0) return NULL;
    if (mod_safe_read(rvt + DECK_RESOLVE_OFF, &resolve, sizeof(resolve)) != 0) return NULL;
    return ((resolve_fn_t)resolve)((void *)resolver);
}

uintptr_t cue_slot(const struct cue_event *ev, int kind)
{
    uintptr_t cc, facade = 0, parent = 0, table = 0, slot = 0;

    cc = (uintptr_t)cue_controller(ev);
    if (!cc) return 0;
    /* The second MappedObjPtr on the way here, and empty for the same reason as
     * the handler's. setPoint links it before every use; so does this. */
    if (FN_LINK_FACADE)
        ((link_fn_t)FN_LINK_FACADE)((void *)(cc + FACADE_MOP_OFF));
    if (mod_safe_read(cc + FACADE_MOP_OFF, &facade, sizeof(facade)) != 0 || !facade)
        return 0;
    if (mod_safe_read(facade + FACADE_PARENT_OFF, &parent, sizeof(parent)) != 0 || !parent)
        return 0;
    if (mod_safe_read(parent + PARENT_TABLE_OFF, &table, sizeof(table)) != 0 || !table)
        return 0;
    if (mod_safe_read(table + TABLE_SLOT0_OFF + (uintptr_t)kind * 8, &slot, sizeof(slot)) != 0)
        return 0;
    return slot;
}

int cue_slot_pos(const struct cue_event *ev, int kind, int64_t *out)
{
    uintptr_t slot = cue_slot(ev, kind);
    int64_t at = 0;

    if (!slot) return 0;
    if (mod_safe_read(slot + CUE_SLOT_POS_OFF, &at, sizeof(at)) != 0)
        return 0;
    if (at == CUE_POS_INVALID)
        return 0;
    *out = at;
    return 1;
}

int64_t cue_stock_press(const struct cue_event *ev)
{
    return ((task_run_t)cue_g_orig_pp)(ev->task);
}

int64_t cue_stock_release(const struct cue_event *ev)
{
    return ((task_run_t)cue_g_orig_rp)(ev->task);
}

/* ---- asking again --------------------------------------------------------
 *
 * A short hold loses its back-cue, and nothing in this layer decides that: the
 * deck's release is reached with the same state and told to return to the same
 * hot cue whether the pad was down for 30 ms or 400. What differs is below it,
 * where the press's own jump-and-play is still settling and lands last.
 *
 * So ask again. Returning to a hot cue is IDEMPOTENT -- a deck already parked
 * there parks there again -- which is the whole reason this is allowed to be a
 * repeat rather than a condition: there is nothing to test, and a request that
 * was already honoured costs a seek to where the play head already is.
 *
 * ON A SPREAD, because the settle is not a fixed number and the tail is long. A
 * 400 ms hold returned three times out of three and a 200 ms hold none, so one
 * shot inside that spread is a coin toss -- and two were still one, because a
 * press of a few milliseconds sometimes has its play land after both. The last
 * ask is out at a second for that tail; every one before it is what makes the
 * common case quick.
 *
 * CANCELLED BY THE DJ. Any new pad press and any PLAY press drops it, because
 * both say the deck is wanted somewhere other than where the last release left
 * it, and a repeat arriving after either would undo a deliberate act.
 *
 * On the DISPLAY thread, which is where the app drives usecases from whenever a
 * finger is involved -- the same clock the pad lamps already run on. */
#define CUE_AGAIN_SHOTS    4
static const long k_cue_again_ms[CUE_AGAIN_SHOTS] = { 120, 280, 560, 1000 };

static struct {
    uintptr_t handler;
    int32_t   pair[2];     /* {pad index, op}, exactly as the release reads it */
    long      t0;          /* when the release armed it, monotonic ms          */
    int       shot;        /* how many have been fired                          */
    long      due;         /* monotonic ms; 0 = nothing pending [atomic]        */
} cue_g_again;

typedef int64_t (*release_op_fn_t)(void *handler, const int32_t pair[2]);

static long cue_now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void cue_again_cancel(void)
{
    __atomic_store_n(&cue_g_again.due, 0, __ATOMIC_RELEASE);
}

/* `due` is published LAST and taken FIRST, which is the whole synchronisation:
 * the display thread only ever reads a payload written before the due it saw. */
static void cue_again_arm(uintptr_t handler, int pad, int32_t op)
{
    long t0 = cue_now_ms();

    cue_again_cancel();
    cue_g_again.handler = handler;
    cue_g_again.pair[0] = (int32_t)pad;
    cue_g_again.pair[1] = op;
    cue_g_again.t0      = t0;
    cue_g_again.shot    = 0;
    /* Every due is measured from the release, not from the last shot, so a slow
     * tick cannot walk the schedule out. */
    __atomic_store_n(&cue_g_again.due, t0 + k_cue_again_ms[0], __ATOMIC_RELEASE);
}

void cue_pad_tick(void)
{
    uintptr_t handler, vt = 0, fn = 0;
    int32_t pair[2];
    long due, t0;
    int shot;

    due = __atomic_load_n(&cue_g_again.due, __ATOMIC_ACQUIRE);
    if (!due || cue_now_ms() < due)
        return;

    handler = cue_g_again.handler;
    pair[0] = cue_g_again.pair[0];
    pair[1] = cue_g_again.pair[1];
    t0      = cue_g_again.t0;
    shot    = cue_g_again.shot + 1;

    /* Take it before firing, so a tick that overruns cannot fire it twice. */
    if (!__atomic_compare_exchange_n(&cue_g_again.due, &due, 0, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;

    if (mod_safe_read(handler, &vt, sizeof(vt)) != 0) return;
    if (mod_safe_read(vt + HANDLER_RELEASE_SLOT, &fn, sizeof(fn)) != 0) return;
    if (fn != PADREL_SENTINEL)
        return;

    MDBG("cue: pad %d back-cue asked again (%d/%d)\n",
         (int)pair[0], shot, CUE_AGAIN_SHOTS);
    ((release_op_fn_t)fn)((void *)handler, pair);

    if (shot < CUE_AGAIN_SHOTS) {
        cue_g_again.shot = shot;
        __atomic_store_n(&cue_g_again.due, t0 + k_cue_again_ms[shot],
                         __ATOMIC_RELEASE);
    }
}

/* Give the deck's own release an op, if any behaviour wants one.
 *
 * The release closure carries {pad index, op} at +0x28, and the stock release
 * hands that pair straight to the handler's own release method once the last
 * held pad is popped. So a behaviour that wants the deck to do something on the
 * release writes the op and lets the deck's task do it -- rather than performing
 * the same operation alongside, which races the press it belongs to. */
static void cue_set_release_op(const struct cue_event *ev)
{
    uintptr_t handler = 0, vt = 0, slot20 = 0;
    int32_t op = 0;
    int i;

    for (i = 0; i < cue_g_n; i++)
        if (cue_g_order[i]->release_op &&
            (op = (int32_t)cue_g_order[i]->release_op(ev)) != 0)
            break;
    if (!op)
        return;

    /* The op only means anything to the release that reads it, so check that is
     * still the one this layout was measured against. */
    if (mod_safe_read((uintptr_t)ev->task + CLOSURE_HANDLER_OFF, &handler, sizeof(handler)) != 0)
        return;
    if (mod_safe_read(handler, &vt, sizeof(vt)) != 0) return;
    if (mod_safe_read(vt + HANDLER_RELEASE_SLOT, &slot20, sizeof(slot20)) != 0) return;
    if (slot20 != PADREL_SENTINEL) {
        MDBG("cue: handler vtable[%#x]=%#lx != sentinel -> release op left alone\n",
             HANDLER_RELEASE_SLOT, (unsigned long)slot20);
        return;
    }
    if (mod_safe_write((uintptr_t)ev->task + CLOSURE_RELEASE_OP_OFF, &op, sizeof(op)) != 0)
        return;
    MDBG("cue: pad %d release op %d -> the deck's own release does it\n",
         ev->pad, (int)op);
    cue_again_arm(handler, ev->pad, op);
}

/* ================================================================== */
/* The hooks                                                          */
/* ================================================================== */

/* Who, if anyone, is taking this press instead of the deck. First in
 * (prio, name) order wins, and one claimer is the whole answer -- two
 * behaviours meaning different things by one press is not a state to resolve. */
static const struct cue_handler *cue_claimer(const struct cue_event *ev)
{
    int i;

    for (i = 0; i < cue_g_n; i++)
        if (cue_g_order[i]->pad_claim && cue_g_order[i]->pad_claim(ev))
            return cue_g_order[i];
    return NULL;
}

/* Which behaviour owns each pad that is down, so the release goes back to the
 * one that took the press and to nobody else. Indexed by pad, [deck]. */
static const struct cue_handler *cue_g_owner[CUE_PADS];

static int64_t cue_wrap_press(void *task)
{
    struct cue_event ev;
    const struct cue_handler *owner;
    int64_t r;

    if (cue_decode(task, &ev) != 0)
        return ((task_run_t)cue_g_orig_pp)(task);

    /* A new press says where the deck is wanted; the last release's repeat no
     * longer does. */
    cue_again_cancel();

    __atomic_fetch_add(&cue_g_held, 1, __ATOMIC_ACQ_REL);
    cue_dispatch(&ev, CUE_PAD_DOWN);

    owner = cue_claimer(&ev);
    if (owner) {
        /* The deck's press never runs, so the pad does not jump, does not play
         * and does not set a cue on an empty slot. */
        cue_g_owner[ev.pad] = owner;
        MDBG("cue: pad %d taken by %s -> the deck's press does not run\n",
             ev.pad, owner->name);
        if (owner->pad)
            owner->pad(&ev, CUE_PAD_PRESSED);
        return STATUS_ASSIGNED;
    }

    r = ((task_run_t)cue_g_orig_pp)(task);

    ev.status   = r;
    ev.assigned = (r == STATUS_ASSIGNED);
    MDBG("cue: pad %d down, status %ld, needle %s at %d/1000\n",
         ev.pad, (long)r, ev.needle_up ? "UP" : "down",
         (int)(ev.needle_at * 1000.0f));
    cue_dispatch(&ev, CUE_PAD_PRESSED);
    return r;
}

static int64_t cue_wrap_release(void *task)
{
    struct cue_event ev;
    const struct cue_handler *owner;
    int64_t r;
    int prev;

    if (cue_decode(task, &ev) != 0)
        return ((task_run_t)cue_g_orig_rp)(task);

    /* Clamped rather than trusted: a release whose press we never saw would
     * otherwise drive the count negative and wedge PLAY consumption. */
    prev = __atomic_fetch_sub(&cue_g_held, 1, __ATOMIC_ACQ_REL);
    if (prev <= 0)
        __atomic_store_n(&cue_g_held, 0, __ATOMIC_RELAXED);

    owner = cue_g_owner[ev.pad];
    if (owner) {
        /* Symmetric with the press: the deck's release is skipped too, because
         * there is no press of its own for it to be the other half of. */
        cue_g_owner[ev.pad] = NULL;
        if (owner->pad)
            owner->pad(&ev, CUE_PAD_UP);
        return 0;
    }

    /* Before the stock run, because the stock run is what reads it. */
    cue_set_release_op(&ev);
    r = ((task_run_t)cue_g_orig_rp)(task);
    if (prev > 0)
        cue_dispatch(&ev, CUE_PAD_UP);
    return r;
}

static int64_t cue_wrap_play(void *task)
{
    int i;

    /* PLAY is the DJ asking for playback. A repeat landing after it would take
     * that away. */
    cue_again_cancel();

    if (cue_pads_held() > 0)
        for (i = 0; i < cue_g_n; i++)
            if (cue_g_order[i]->play_while_held && cue_g_order[i]->play_while_held()) {
                MDBG("cue: PLAY consumed by %s\n", cue_g_order[i]->name);
                return 0;
            }
    return ((task_run_t)cue_g_orig_play)(task);
}

/* The CUE button, passed straight through -- and the standing check on the two
 * values cue.h hands behaviours. This slot carries the memory cue and nothing
 * else (hot-cue pads set theirs through CueController +0xb0), so every call
 * through here should name the same pair. A firmware where it does not is one
 * where anything using them moves the wrong thing, and that says so on the
 * first CUE press rather than on a dance floor. */
static int64_t cue_wrap_cueing(void *cc, uint32_t kind, uint32_t slip, uint32_t quantize)
{
    if (kind != CUE_KIND_MEMORY || quantize != CUE_QUANTIZE_DECK) {
        static int said;

        if (!said) {
            said = 1;
            MERR("CUE button says cueing(kind=%u, quantize=%u), cue.h assumes "
                 "(%u, %u) -> THE CUE BEHAVIOURS ARE WRONG ON THIS BUILD\n",
                 kind, quantize, CUE_KIND_MEMORY, CUE_QUANTIZE_DECK);
        }
    } else {
        MDBG("cue: CUE button cueing(kind=%u, slip=%u, quantize=%u)\n", kind, slip, quantize);
    }
    return ((cueing_fn_t)cue_g_orig_cueing)(cc, kind, slip, quantize);
}

static void cue_wrap_preview_set(void *self, float at)
{
    cue_g_needle_at = at;
    __atomic_store_n(&cue_g_needle_up, 1, __ATOMIC_RELEASE);
    MDBG("cue: needle at %d/1000\n", (int)(at * 1000.0f));
    if (cue_g_orig_prev_set)
        ((void (*)(void *, float))cue_g_orig_prev_set)(self, at);
}

static void cue_wrap_preview_clear(void *self)
{
    __atomic_store_n(&cue_g_needle_up, 0, __ATOMIC_RELEASE);
    MDBG("cue: needle down\n");
    if (cue_g_orig_prev_clear)
        ((void (*)(void *))cue_g_orig_prev_clear)(self);
}

/* ================================================================== */
/* Install                                                            */
/* ================================================================== */

/* (prio, name) ascending, sorted here because link order is the Makefile's
 * wildcard -- the same reason and the same rule as the mod registry. */
static void cue_sort_handlers(void)
{
    const struct cue_handler *h;
    int i, j;

    for (h = __start_ep122_cue; h < __stop_ep122_cue; h++) {
        if (cue_g_n == (int)(sizeof(cue_g_order) / sizeof(cue_g_order[0]))) {
            MDBG("cue: more than %d handlers -> %s dropped\n", cue_g_n, h->name);
            break;
        }
        cue_g_order[cue_g_n++] = h;
    }
    for (i = 1; i < cue_g_n; i++) {
        const struct cue_handler *k = cue_g_order[i];

        for (j = i - 1; j >= 0 &&
             (cue_g_order[j]->prio > k->prio ||
              (cue_g_order[j]->prio == k->prio &&
               strcmp(cue_g_order[j]->name, k->name) > 0)); j--)
            cue_g_order[j + 1] = cue_g_order[j];
        cue_g_order[j + 1] = k;
    }
}

static int cue_pad_install(void)
{
    const struct {
        const char *name;
        int         vt;
        int         slot;
        void       *wrapper;
        uintptr_t  *saved;
    } hooks[] = {
        { "cuePress",   EP122_GATE_PRESSPAD_TASK,   CUE_RUN_SLOT,
          (void *)cue_wrap_press,         &cue_g_orig_pp         },
        { "cueRelease", EP122_GATE_RELEASEPAD_TASK, CUE_RUN_SLOT,
          (void *)cue_wrap_release,       &cue_g_orig_rp         },
        { "cuePlay",    EP122_GATE_PLAYPAUSE_TASK,  CUE_RUN_SLOT,
          (void *)cue_wrap_play,          &cue_g_orig_play       },
        { "cueCueing",  EP122_CUE_CONTROLLER,       CUE_VT_CUEING,
          (void *)cue_wrap_cueing,        &cue_g_orig_cueing     },
        { "cuePrevSet", EP122_PREVIEW_CTL,          PREVIEW_SET_SLOT,
          (void *)cue_wrap_preview_set,   &cue_g_orig_prev_set   },
        { "cuePrevClr", EP122_PREVIEW_CTL,          PREVIEW_CLEAR_SLOT,
          (void *)cue_wrap_preview_clear, &cue_g_orig_prev_clear },
    };
    const int n = (int)(sizeof(hooks) / sizeof(hooks[0]));
    int i, ok = 0;

    for (i = 0; i < n; i++)
        ok += (mod_patch_vslot(hooks[i].name, hooks[i].vt, hooks[i].slot,
                               hooks[i].wrapper, hooks[i].saved) == 0);

    /* All of them or none. The behaviours on top of this assume a complete
     * event stream, and two thirds of a momentary pad is one that plays and
     * never comes back. The registry unwinds what did go in. */
    if (ok != n) {
        MDBG("cue: partial install (%d/%d) -> refused\n", ok, n);
        return -1;
    }

    cue_sort_handlers();
    cue_g_ready = 1;
    MDBG("cue: interception in (%d hooks, %d handlers)\n", ok, cue_g_n);
    return 0;
}

KIT_MOD(k_mod_cue_pad,
        .name = "cue_pad", .prio = 5, .install = cue_pad_install,
        .what = "hot-cue interception: pad, PLAY, CUE button, preview needle");
