// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * kit/menu.h - the rows a feature declares for the MOD SETTINGS overlay.
 *
 * A feature declares its rows and registers them from its install. mods/menu/
 * renders whatever is registered and knows no feature, so adding a mod with a
 * setting does not touch the overlay.
 *
 * A row is one of two kinds, selected by `text != NULL`:
 *
 *   CHOICE  `state` is an index into `values[0 .. nvalues-1]`. OFF/ON rows use
 *           kit_off_on; AUTO/MANUAL is a CHOICE row with its own two strings.
 *   TEXT    `text` is the buffer the deck's software keyboard edits.
 *
 * Registration is [init]; everything else here is [message], the thread the
 * overlay runs on.
 */
#ifndef EP122_MOD_KIT_MENU_H
#define EP122_MOD_KIT_MENU_H

#include "core/mod_core.h"

#ifdef __cplusplus
extern "C" {
#endif


/* The value list every OFF/ON row shares. */
extern const char *const kit_off_on[2];

struct kit_row {
    const char *label;

    /* Unique among siblings (rows sharing this row's `parent`); each level is
     * walked in idx order. Top-level rows take the KIT_IDX_* values below;
     * children number from 0 inside their parent. */
    unsigned char idx;

    /* NULL = top level. The row is revealed when its parent is live and
     * *parent->state == show_when, so a subtree is hidden with its root. */
    const struct kit_row *parent;
    int                   show_when;

    /* Called after a commit and before the value is persisted; a feature that
     * rejects the input clears it here and the cleared value is saved. May be
     * NULL. */
    void (*changed)(void);

    /* CHOICE */
    int               *state;
    const char *const *values;
    unsigned char      nvalues;

    /* TEXT */
    char           *text;
    unsigned short  text_cap;
};

/* The common case: a two-value row over an int flag. Trailing designated
 * initialisers -- idx, parent, changed -- follow the state pointer. */
#define KIT_ROW_BOOL(lbl, st, ...) \
    { .label = (lbl), .state = (st), .values = kit_off_on, .nvalues = 2, __VA_ARGS__ }

/* Top-level idx values, kept in one place so they cannot collide. Spaced
 * apart so a feature can be inserted without renumbering. */
#define KIT_IDX_GATE   10
#define KIT_IDX_SMART  15
#define KIT_IDX_PREVIEW 17
#define KIT_IDX_THEME  20
#define KIT_IDX_XPAD   25
#define KIT_IDX_STEMS  30

/* How many rows can be visible at once. The list has no scrollbar, so this is
 * also how many rows a DJ can reach. The overlay sizes the list for
 * MOD_ROWS_VISIBLE rows (what the UTILITY panel has room for) and row 0 is the
 * "MOD SETTINGS" title, so this is one less. Every mod on with STEMS set to
 * MANUAL is eight rows.
 *
 * The live count grows as children are revealed, so the limit is enforced when
 * the list is flattened: the overflow is logged and the surplus dropped, rather
 * than left invisible and unreachable. The limit in force is what the overlay
 * could size the list for (kit_menu_set_shown): a list it could not grow shows
 * seven, and the eighth is dropped with the same log line. */
#define KIT_MENU_MAX_ROWS 9

/* The rows the list can show right now, 1..KIT_MENU_MAX_ROWS. */
void kit_menu_set_shown(int n);                         /* [message] */

/* The longest buffer a TEXT row may have; the overlay's pre-edit copy is sized
 * for it. */
#define KIT_ROW_TEXT_MAX 64

/* Register `n` rows. The array must have static storage: it is kept by
 * reference, and a child's `parent` points into it. Rows that are not usable as
 * declared are logged and skipped. */
void kit_menu_add(const struct kit_row *rows, int n);   /* [init] */

/* The flattened live list: top-level rows in idx order, each revealed child
 * directly under its parent. Positions shift as rows are revealed or hidden, so
 * a row is addressed by its current position. */
int                   kit_menu_count(void);         /* [message] */
const struct kit_row *kit_menu_row(int i);          /* [message] NULL past the end */

/* Why there is no list, or NULL when there is one. The rows are all-or-nothing:
 * a clash between siblings disables every row. A caller about to show the
 * overlay displays this instead. */
const char *kit_menu_problem(void);                 /* [message] */


#ifdef __cplusplus
}
#endif

#endif /* EP122_MOD_KIT_MENU_H */
