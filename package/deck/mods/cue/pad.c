// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * cue/pad.c - hooks the deck's hot-cue path.
 *
 * Every slot the hot-cue behaviours need is hooked here, decoded once and
 * passed on as a cue_event. The behaviours (gate.c, smart.c, preview.c) hold no
 * addresses; see cue.h for the contract.
 *
 * Mechanism (non-PIE ET_EXEC @0x400000)
 * -------------------------------------
 * A physical hot-cue pad is handled by input_devices::DeckOperationPadHandler
 * <HotCueOperation>. The pad DOWN/UP each post a meow::AsyncTask whose "run"
 * slot (vtable+0x10, in .rodata) executes on the deck thread:
 *   - pressPad   run -> jump to cue + PLAY
 *   - releasePad run -> pad state bookkeeping
 * PLAY posts a PlayPauseHandler::pushOn task.
 * Those three run slots are repointed at wrappers that call the stock run back
 * through saved pointers.
 *
 * Once the releasePad run pops the last held pad it passes &closure[0x28] to
 * the handler's release method (vtable+0x20), which reads the
 * pad index at +0 and dispatches on an op word at +4: 1 returns to the cue and
 * pauses, anything else is the plain release. A behaviour that wants a back-cue
 * writes that word and the deck's release performs it; see cue_set_release_op.
 *
 * The preview needle comes from usecase::deck::PreviewController, driven by the
 * strip's touch: slot +0x10 carries the touched point as a normalised fraction
 * and +0x18 clears it. Hooking the pair tells the layer when the zone is held.
 *
 * Every input handler embeds a dj_player::PlayerState at +0x80, the deck's own
 * snapshot of the player. The deck's input-state snapshot derives its PAUSED
 * bit from it as: load state +0x20 == 2, and play mode == 1, where play mode is
 *     +0x65 ? 0 : (+0x66 && !+0x6b) ? 3 : +0x67 ? 2 : 1
 * 2 is playing, 1 paused; 0 and 3 are modes the PLAY button and the load lock
 * treat as not paused. cue_deck_paused evaluates the same expression, read at
 * the press off the handler the closure names.
 */
#include "cue/cue.h"
#include "kit/mod.h"

/* ================================================================== */
/* EP122 ABI                                                          */
/* ================================================================== */

#define CUE_RUN_SLOT       0x10   /* AsyncTask::run, on every closure class */
#define PREVIEW_SET_SLOT   0x10   /* PreviewController: the touched point   */
#define PREVIEW_CLEAR_SLOT 0x18   /* ...and letting go                      */

/* dj_player::PlayerState inside every input handler, and the bytes the deck's
 * own paused predicate reads. */
#define HANDLER_STATE_OFF  0x80
#define STATE_LOAD_OFF     0x20   /* int32; 2 = a track is loaded            */
#define STATE_LOADED       2
#define STATE_FLAGS_OFF    0x65   /* +0x65 +0x66 +0x67, and +0x6b, one byte each */
#define STATE_FLAGS_LEN    7

#define FN_LINK_DECK       ep122_sym(EP122_CUE_LINK_DECK)
#define FN_LINK_FACADE     ep122_sym(EP122_CUE_LINK_FACADE)
#define PADREL_SENTINEL    ep122_sym(EP122_GATE_PADREL_SENTINEL)

/* The pad handler reaches the CueController the stock press path uses:
 * handler+0x30 is a meow::MappedObjPtr whose vtable[0x38] hands it out. */
#define DECK_RESOLVE_OFF     0x38
#define HANDLER_DECK_MOP_OFF 0x30

/* The shared pad/release closure. The last two form one pair: the stock release
 * passes `&closure[0x28]` to the handler's release method, which reads the
 * index at +0 and dispatches on the op at +4. */
#define CLOSURE_HANDLER_OFF     0x18  /* handler `this` pointer                */
#define CLOSURE_PAD_OFF         0x28  /* the pad's own index, 0-based (A=0)    */
#define CLOSURE_RELEASE_OP_OFF  0x2c  /* what the release means; 1 = back-cue  */

/* The handler's release method slot; its stock body reads the op above. */
#define HANDLER_RELEASE_SLOT    0x20

/* The deck's press run returns this when the pad was already assigned. A
 * claimed press returns it too, since the task machinery treats 0 as "no
 * news". */
#define STATUS_ASSIGNED      0

/* From the CueController to the cue table:
 *   facade = *(cc + 0x18)      MappedObjPtr<ICueLoopSetter>, linked by the stock
 *                              press path before any of this runs
 *   P      = *(facade + 0x70)  the parent everything cue hangs off
 *   table  = *(P + 0x40)       the engine also reads it from +0x18 and +0x60;
 *                              all three hold the same pointer
 *   slot   = *(table + 0x10 + kind*8) */
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

int cue_deck_paused(const struct cue_event *ev)
{
    static int broken;
    uintptr_t handler = 0, state;
    int32_t   load = 0;
    uint8_t   f[STATE_FLAGS_LEN];   /* f[0..2] = +0x65..+0x67, f[6] = +0x6b */

    if (broken)
        return 0;
    if (mod_safe_read((uintptr_t)ev->task + CLOSURE_HANDLER_OFF, &handler, sizeof(handler)) != 0 ||
        !handler)
        return 0;
    state = handler + HANDLER_STATE_OFF;
    if (mod_safe_read(state + STATE_LOAD_OFF, &load, sizeof(load)) != 0 ||
        mod_safe_read(state + STATE_FLAGS_OFF, f, sizeof(f)) != 0)
        return 0;
    /* The flags are bools; any other value means the layout is wrong. Stop
     * gating rather than report every press as paused. */
    if (f[0] > 1 || f[1] > 1 || f[2] > 1 || f[6] > 1) {
        MERR("cue: handler+%#x is not a PlayerState (flags %02x %02x %02x .. %02x)"
             " -> pads never gate\n", HANDLER_STATE_OFF, f[0], f[1], f[2], f[6]);
        broken = 1;
        return 0;
    }
    MDBG("cue: player load %d flags %02x %02x %02x .. %02x\n", load, f[0], f[1], f[2], f[6]);
    return load == STATE_LOADED && !f[0] && !(f[1] && !f[6]) && !f[2];
}

/* ================================================================== */
/* Decoding an event                                                  */
/* ================================================================== */

static int cue_decode(void *task, struct cue_event *ev)
{
    int32_t pad = 0;

    if (mod_safe_read((uintptr_t)task + CLOSURE_PAD_OFF, &pad, sizeof(pad)) != 0)
        return -1;
    /* A pad outside the bank means the closure layout is not what this file
     * assumes, so pass the event through untouched. */
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
     * the message thread clears it mid-dispatch. */
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
        /* A claimer sees DOWN for every pad (where it decides) and later phases
         * only for pads it owns, delivered directly by the hooks. Otherwise its
         * PRESSED would run for declined pads on stale state, e.g. an empty pad
         * set by the deck also starting a loop over the previous pad's span. */
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
    /* Link first. The MappedObjPtr at +0x30 starts empty and caches its pointer
     * on first use, so a raw read finds nothing until the deck's press path has
     * run once (the first press after a track load). The deck's own back-cue
     * links it the same way; when already cached it is a load and a branch. */
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
    /* Second MappedObjPtr on the path, empty for the same reason. setPoint links
     * it before every use, as does this. */
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
 * A short hold can lose its back-cue: the deck's release runs with the same
 * state for a 30 ms hold as for a 400 ms one, but the press's jump-and-play is
 * still settling and lands after it.
 *
 * So the release op is re-sent. Returning to a hot cue is idempotent, so a
 * repeat needs no condition; one that was already honoured costs a seek to the
 * current position.
 *
 * Repeats are spread out because the settle time has a long tail: a 200 ms
 * hold can lose its back-cue, and with two shots a very short press can still
 * have its play land after both. The last shot at 1 s covers the tail; the
 * earlier ones keep the common case quick.
 *
 * Any new pad press or PLAY press cancels pending repeats, so a repeat never
 * undoes a deliberate action.
 *
 * Runs on the display thread, where the app drives usecases for touch input,
 * on the same clock as the pad lamps. */
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

/* `due` is published last and read first; that is the only synchronisation.
 * The display thread only reads a payload written before the `due` it saw. */
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
 * passes that pair to the handler's release method once the last held pad is
 * popped. Writing the op lets the deck's task perform it; performing it
 * separately would race the press. */
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

    /* Only write the op if the release method is the one this layout was
     * written for. */
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

/* The behaviour taking this press instead of the deck, if any. The first in
 * (prio, name) order wins; there is only ever one claimer. */
static const struct cue_handler *cue_claimer(const struct cue_event *ev)
{
    int i;

    for (i = 0; i < cue_g_n; i++)
        if (cue_g_order[i]->pad_claim && cue_g_order[i]->pad_claim(ev))
            return cue_g_order[i];
    return NULL;
}

/* The claimer of each pad that is down, so the release goes only to it.
 * Indexed by pad, [deck]. */
static const struct cue_handler *cue_g_owner[CUE_PADS];

static int64_t cue_wrap_press(void *task)
{
    struct cue_event ev;
    const struct cue_handler *owner;
    int64_t r;

    if (cue_decode(task, &ev) != 0)
        return ((task_run_t)cue_g_orig_pp)(task);

    /* A new press cancels the last release's repeats. */
    cue_again_cancel();

    __atomic_fetch_add(&cue_g_held, 1, __ATOMIC_ACQ_REL);
    cue_dispatch(&ev, CUE_PAD_DOWN);

    owner = cue_claimer(&ev);
    if (owner) {
        /* The deck's press does not run, so the pad does not jump, play, or
         * set a cue on an empty slot. */
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

    /* Clamped: a release whose press we never saw would otherwise drive the
     * count negative and break PLAY consumption. */
    prev = __atomic_fetch_sub(&cue_g_held, 1, __ATOMIC_ACQ_REL);
    if (prev <= 0)
        __atomic_store_n(&cue_g_held, 0, __ATOMIC_RELAXED);

    owner = cue_g_owner[ev.pad];
    if (owner) {
        /* The deck's press was skipped, so its release is skipped too. */
        cue_g_owner[ev.pad] = NULL;
        if (owner->pad)
            owner->pad(&ev, CUE_PAD_UP);
        return 0;
    }

    /* Before the stock run, which reads it. */
    cue_set_release_op(&ev);
    r = ((task_run_t)cue_g_orig_rp)(task);
    if (prev > 0)
        cue_dispatch(&ev, CUE_PAD_UP);
    return r;
}

static int64_t cue_wrap_play(void *task)
{
    int i;

    /* A repeat landing after PLAY would undo it. */
    cue_again_cancel();

    if (cue_pads_held() > 0)
        for (i = 0; i < cue_g_n; i++)
            if (cue_g_order[i]->play_while_held && cue_g_order[i]->play_while_held()) {
                MDBG("cue: PLAY consumed by %s\n", cue_g_order[i]->name);
                return 0;
            }
    return ((task_run_t)cue_g_orig_play)(task);
}

/* The CUE button, passed through unchanged. Also checks the two values cue.h
 * gives behaviours: this slot only carries the memory cue (hot-cue pads use
 * CueController +0xb0), so every call should pass the same pair. If a firmware
 * differs, an error is logged on the first CUE press. */
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

/* (prio, name) ascending, sorted here because link order follows the
 * Makefile's wildcard; same rule as the mod registry. */
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

    /* All or none: the behaviours assume a complete event stream (a momentary
     * pad without its release hook would play and never return). The registry
     * unwinds the hooks that did go in. */
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
