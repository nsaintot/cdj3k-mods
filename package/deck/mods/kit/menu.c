// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * kit/menu.c - the registered rows, and the live list flattened out of them.
 *
 * The contract, the two kinds of row and the capacity rule are in menu.h.
 */
#include "kit/menu.h"

const char *const kit_off_on[2] = { "OFF", "ON" };

/* Every row any feature registered. A fixed array: registration runs during
 * static init, where an allocation failure has nowhere to go. */
#define KIT_MENU_MAX_DEFS 24

static const struct kit_row *g_def[KIT_MENU_MAX_DEFS];
static int                   g_ndef;

static const struct kit_row *g_live[KIT_MENU_MAX_ROWS];
static int                   g_nlive;
static int                   g_overflow;      /* the flatten in progress hit the ceiling */
static int                   g_shown = KIT_MENU_MAX_ROWS;   /* rows the list can show */

static char g_problem[96];                    /* empty while the row list is usable */
static int  g_checked = -1;                   /* the g_ndef g_problem was decided against */

/* WARN, not DEBUG: every message here is a row a developer declared wrongly, so the
 * row is missing or inert on the deck and the mistake is in the source rather than in
 * anything the DJ did. Not ERROR -- the rest of the menu still works. */
#define KMSG(...) MWARN("menu: " __VA_ARGS__)

/* ================================================================== */
/* Registration                                                       */
/* ================================================================== */

void kit_menu_add(const struct kit_row *rows, int n)
{
    int i;

    for (i = 0; rows && i < n; i++) {
        const struct kit_row *r = &rows[i];

        if (!r->label) {
            KMSG("a row with no label -> skipped\n");
            continue;
        }
        if (r->text && r->values) {
            KMSG("\"%s\" declares both a text buffer and a value list -> skipped\n",
                 r->label);
            continue;
        }
        if (r->text) {
            if (r->text_cap < 2 || r->text_cap > KIT_ROW_TEXT_MAX) {
                KMSG("\"%s\" offers %u bytes of text, want 2..%d -> skipped\n",
                     r->label, r->text_cap, KIT_ROW_TEXT_MAX);
                continue;
            }
        } else if (!r->state || !r->values || r->nvalues < 2) {
            KMSG("\"%s\" is neither a CHOICE nor a TEXT row -> skipped\n", r->label);
            continue;
        }
        /* A parent supplies the value the child is revealed by. */
        if (r->parent && !r->parent->state) {
            KMSG("\"%s\" hangs off \"%s\", which has no state -> skipped\n",
                 r->label, r->parent->label);
            continue;
        }
        if (g_ndef >= KIT_MENU_MAX_DEFS) {
            KMSG("%d rows registered; \"%s\" does not fit -> skipped\n",
                 KIT_MENU_MAX_DEFS, r->label);
            continue;
        }
        g_def[g_ndef++] = r;
    }
}

/* ================================================================== */
/* Validation                                                         */
/* ================================================================== */

static const char *parent_name(const struct kit_row *r)
{
    return r->parent ? r->parent->label : "the top level";
}

static int registered(const struct kit_row *r)
{
    int i;

    for (i = 0; i < g_ndef; i++)
        if (g_def[i] == r) return 1;
    return 0;
}

/* Decided once per set of registrations, and all-or-nothing: a tree that is not
 * the one the features declared is worse than no rows at all, and only a
 * developer can reach either failure -- the mods are statically linked. */
static int rows_ok(void)
{
    int i, j;

    if (g_checked == g_ndef) return g_problem[0] == '\0';
    g_checked = g_ndef;
    g_problem[0] = '\0';

    if (g_ndef == 0) {
        snprintf(g_problem, sizeof(g_problem), "no rows registered");
        KMSG("no rows registered\n");
        return 0;
    }
    for (i = 0; i < g_ndef; i++) {
        const struct kit_row *a = g_def[i];

        for (j = i + 1; j < g_ndef; j++) {
            const struct kit_row *b = g_def[j];

            if (a->parent != b->parent || a->idx != b->idx) continue;
            snprintf(g_problem, sizeof(g_problem), "idx %u: \"%s\" and \"%s\"",
                     a->idx, a->label, b->label);
            KMSG("idx %u claimed by \"%s\" and \"%s\" under %s -> rows disabled\n",
                 a->idx, a->label, b->label, parent_name(a));
            return 0;
        }
        if (a->parent && !registered(a->parent)) {
            snprintf(g_problem, sizeof(g_problem), "\"%s\" has no parent row", a->label);
            KMSG("\"%s\" hangs off \"%s\", which was never registered -> rows disabled\n",
                 a->label, a->parent->label);
            return 0;
        }
    }
    return 1;
}

/* ================================================================== */
/* The live list                                                      */
/* ================================================================== */

static int revealed(const struct kit_row *r)
{
    return !r->parent || *r->parent->state == r->show_when;
}

static void dropped(const struct kit_row *r)
{
    static const char *told;

    g_overflow = 1;
    if (told == r->label) return;
    told = r->label;
    KMSG("%d rows fit; \"%s\" and anything under or after it is not shown\n",
         g_shown, r->label);
}

void kit_menu_set_shown(int n)
{
    g_shown = n < 1 ? 1 : n > KIT_MENU_MAX_ROWS ? KIT_MENU_MAX_ROWS : n;
}

/* One level, in idx order, recursing into each revealed row. A row's children
 * therefore land directly under it and no sibling's position depends on how
 * many of them there are. Rows are only ever reached from the top level, so a
 * mis-authored parent CYCLE emits nothing rather than recursing. */
static void emit(const struct kit_row *parent)
{
    unsigned last = 0;
    int first = 1;

    for (;;) {
        const struct kit_row *next = NULL;
        int i;

        if (g_overflow) return;
        for (i = 0; i < g_ndef; i++) {
            const struct kit_row *r = g_def[i];

            if (r->parent != parent) continue;
            if (!first && r->idx <= last) continue;
            if (next && r->idx >= next->idx) continue;
            next = r;
        }
        if (!next) return;
        first = 0;
        last = next->idx;

        if (!revealed(next)) continue;
        if (g_nlive >= g_shown) {
            dropped(next);
            return;
        }
        g_live[g_nlive++] = next;
        emit(next);
    }
}

/* Rebuilt per call rather than cached: what is live follows feature globals
 * that anything may write, and there is no event to invalidate a cache on. It
 * is a handful of rows. */
static int flatten(void)
{
    g_nlive = 0;
    g_overflow = 0;
    if (rows_ok()) emit(NULL);
    return g_nlive;
}

int kit_menu_count(void)
{
    return flatten();
}

const struct kit_row *kit_menu_row(int i)
{
    int n = flatten();

    return (i >= 0 && i < n) ? g_live[i] : NULL;
}

const char *kit_menu_problem(void)
{
    return rows_ok() ? NULL : g_problem;
}
