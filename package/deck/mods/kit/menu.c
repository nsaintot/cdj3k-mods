// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * kit/menu.c - the MOD SETTINGS instance: registered categories, rows and
 * pages, and each category's live row list.
 *
 * The contract is in menu.h.
 */
#include "kit/menu.h"

const char *const kit_off_on[2] = { "OFF", "ON" };

const struct kit_cat kit_cat_player  = { "player",  "PLAYER\nSETTING", KIT_ORDER_PLAYER };
const struct kit_cat kit_cat_display = { "display", "DISPLAY",         KIT_ORDER_DISPLAY };

/* Fixed arrays: registration runs during init, where a failed allocation has
 * nowhere to go. */
#define KIT_MENU_MAX_DEFS 32

struct kit_menu {
    struct {
        const struct kit_cat  *cat;
        const struct kit_page *page;
        int                    rows;     /* rows registered under it */
    } cats[KIT_MENU_MAX_CATS];
    int ncat;

    struct {
        const struct kit_row *row;
        const struct kit_cat *cat;
    } defs[KIT_MENU_MAX_DEFS];
    int ndef;

    const struct kit_row *live[KIT_MENU_MAX_DEFS];
    int nlive;

    char problem[96];       /* empty while the rows are usable */
    int  checked_defs;      /* the registrations `problem` was decided against */
    int  checked_cats;

    void (*repaint)(void);
};

static struct kit_menu g_menu = { .checked_defs = -1 };

struct kit_menu *kit_menu_instance(void)
{
    return &g_menu;
}

/* WARN: each message is a registration error in the source. */
#define KMSG(...) MWARN("menu: " __VA_ARGS__)

/* ================================================================== */
/* Categories                                                         */
/* ================================================================== */

/* The slot for `cat`, created on first use. Categories are matched by id, so
 * two features naming the same id share the first descriptor. Kept sorted by
 * (order, id). */
static int cat_slot(struct kit_menu *m, const struct kit_cat *cat)
{
    int i, at;

    if (!cat || !cat->id || !cat->label) {
        KMSG("a category with no id or label -> refused\n");
        return -1;
    }
    for (i = 0; i < m->ncat; i++)
        if (strcmp(m->cats[i].cat->id, cat->id) == 0) return i;
    if (m->ncat >= KIT_MENU_MAX_CATS) {
        KMSG("%d categories fit the rail; \"%s\" -> refused\n", KIT_MENU_MAX_CATS, cat->id);
        return -1;
    }
    for (at = m->ncat; at > 0; at--) {
        const struct kit_cat *p = m->cats[at - 1].cat;

        if (p->order < cat->order || (p->order == cat->order && strcmp(p->id, cat->id) < 0))
            break;
        m->cats[at] = m->cats[at - 1];
    }
    m->cats[at].cat = cat;
    m->cats[at].page = NULL;
    m->cats[at].rows = 0;
    m->ncat++;
    return at;
}

/* ================================================================== */
/* Registration                                                       */
/* ================================================================== */

/* `say` logs why a row is refused. */
static int row_ok(const struct kit_row *r, int say)
{
    if (!r->label) {
        if (say) KMSG("a row with no label -> skipped\n");
        return 0;
    }
    if (!!r->text + !!r->values + !!r->info != 1) {
        if (say) KMSG("\"%s\" must declare exactly one of a value list, a text "
                      "buffer or an info string -> skipped\n", r->label);
        return 0;
    }
    if (r->text && (r->text_cap < 2 || r->text_cap > KIT_ROW_TEXT_MAX)) {
        if (say) KMSG("\"%s\" offers %u bytes of text, want 2..%d -> skipped\n",
                      r->label, r->text_cap, KIT_ROW_TEXT_MAX);
        return 0;
    }
    if (r->values && (!r->state || r->nvalues < 2)) {
        if (say) KMSG("\"%s\" is a CHOICE row without a state or two values -> "
                      "skipped\n", r->label);
        return 0;
    }
    if (r->parent && !r->parent->state) {
        if (say) KMSG("\"%s\" hangs off \"%s\", which has no state -> skipped\n",
                      r->label, r->parent->label);
        return 0;
    }
    return 1;
}

int kit_menu_rows(struct kit_menu *m, const struct kit_cat *cat,
                  const struct kit_row *rows, int n)
{
    int c, i, usable = 0, kept = 0;

    /* A category takes a rail entry only once it has something to show. */
    for (i = 0; m && rows && i < n; i++)
        usable += row_ok(&rows[i], 0);
    if (!usable) {
        for (i = 0; rows && i < n; i++) row_ok(&rows[i], 1);
        return 0;
    }
    c = cat_slot(m, cat);
    if (c < 0) return 0;
    if (m->cats[c].page) {
        KMSG("\"%s\" shows a page; its rows -> refused\n", m->cats[c].cat->id);
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (!row_ok(&rows[i], 1)) continue;
        if (m->ndef >= KIT_MENU_MAX_DEFS) {
            KMSG("%d rows registered; \"%s\" does not fit -> skipped\n",
                 KIT_MENU_MAX_DEFS, rows[i].label);
            continue;
        }
        m->defs[m->ndef].row = &rows[i];
        m->defs[m->ndef].cat = m->cats[c].cat;
        m->ndef++;
        m->cats[c].rows++;
        kept++;
    }
    return kept;
}

int kit_menu_page(struct kit_menu *m, const struct kit_cat *cat, const struct kit_page *page)
{
    int c = (m && page) ? cat_slot(m, cat) : -1;

    if (c < 0) return -1;
    if (m->cats[c].page || m->cats[c].rows) {
        KMSG("\"%s\" already has content; the page -> refused\n", m->cats[c].cat->id);
        return -1;
    }
    m->cats[c].page = page;
    return 0;
}

void kit_menu_set_repaint(struct kit_menu *m, void (*repaint)(void))
{
    m->repaint = repaint;
}

void kit_menu_page_repaint(struct kit_menu *m)
{
    if (m && m->repaint) m->repaint();
}

/* ================================================================== */
/* Validation                                                         */
/* ================================================================== */

static int registered(const struct kit_menu *m, const struct kit_row *r)
{
    int i;

    for (i = 0; i < m->ndef; i++)
        if (m->defs[i].row == r) return 1;
    return 0;
}

/* Checked once per set of registrations. Any failure disables all rows; each
 * one is a source error. */
static int rows_ok(struct kit_menu *m)
{
    int i, j;

    if (m->checked_defs == m->ndef && m->checked_cats == m->ncat)
        return m->problem[0] == '\0';
    m->checked_defs = m->ndef;
    m->checked_cats = m->ncat;
    m->problem[0] = '\0';

    if (m->ncat == 0) {
        snprintf(m->problem, sizeof(m->problem), "nothing registered");
        return 0;
    }
    for (i = 0; i < m->ndef; i++) {
        const struct kit_row *a = m->defs[i].row;

        for (j = i + 1; j < m->ndef; j++) {
            const struct kit_row *b = m->defs[j].row;

            if (a->parent != b->parent || a->idx != b->idx) continue;
            if (!a->parent && m->defs[i].cat != m->defs[j].cat) continue;
            snprintf(m->problem, sizeof(m->problem), "idx %u: \"%s\" and \"%s\"",
                     a->idx, a->label, b->label);
            KMSG("idx %u claimed by \"%s\" and \"%s\" -> rows disabled\n",
                 a->idx, a->label, b->label);
            return 0;
        }
        if (a->parent && !registered(m, a->parent)) {
            snprintf(m->problem, sizeof(m->problem), "\"%s\" has no parent row", a->label);
            KMSG("\"%s\" hangs off \"%s\", which was never registered -> rows disabled\n",
                 a->label, a->parent->label);
            return 0;
        }
    }
    return 1;
}

const char *kit_menu_problem(const struct kit_menu *m)
{
    return rows_ok((struct kit_menu *)m) ? NULL : m->problem;
}

/* ================================================================== */
/* Categories and live rows                                           */
/* ================================================================== */

int kit_menu_ncats(const struct kit_menu *m)
{
    return m->ncat;
}

const struct kit_cat *kit_menu_cat(const struct kit_menu *m, int c)
{
    return (c >= 0 && c < m->ncat) ? m->cats[c].cat : NULL;
}

const struct kit_page *kit_menu_cat_page(const struct kit_menu *m, int c)
{
    return (c >= 0 && c < m->ncat) ? m->cats[c].page : NULL;
}

static int shown(const struct kit_row *r)
{
    return !r->parent || *r->parent->state == r->show_when;
}

/* One level, in idx order, recursing into each shown row so its children land
 * directly under it. Rows are only reached from the top level, so a parent cycle
 * emits nothing instead of recursing forever. `cat` filters the top level only. */
static void emit(struct kit_menu *m, const struct kit_cat *cat, const struct kit_row *parent)
{
    unsigned last = 0;
    int first = 1;

    for (;;) {
        const struct kit_row *next = NULL;
        int i;

        for (i = 0; i < m->ndef; i++) {
            const struct kit_row *r = m->defs[i].row;

            if (r->parent != parent) continue;
            if (!parent && m->defs[i].cat != cat) continue;
            if (!first && r->idx <= last) continue;
            if (next && r->idx >= next->idx) continue;
            next = r;
        }
        if (!next) return;
        first = 0;
        last = next->idx;

        if (!shown(next)) continue;
        if (m->nlive >= KIT_MENU_MAX_DEFS) return;
        m->live[m->nlive++] = next;
        emit(m, cat, next);
    }
}

/* Rebuilt per call: what is shown follows feature globals that anything may
 * write, with no event to invalidate a cache on. */
static int flatten(const struct kit_menu *cm, int c)
{
    struct kit_menu *m = (struct kit_menu *)cm;

    m->nlive = 0;
    if (c >= 0 && c < m->ncat && !m->cats[c].page && rows_ok(m))
        emit(m, m->cats[c].cat, NULL);
    return m->nlive;
}

int kit_menu_count(const struct kit_menu *m, int c)
{
    return flatten(m, c);
}

const struct kit_row *kit_menu_row(const struct kit_menu *m, int c, int i)
{
    int n = flatten(m, c);

    return (i >= 0 && i < n) ? m->live[i] : NULL;
}
