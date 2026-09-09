// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * kit/menu.h - the rows a feature declares for the MOD SETTINGS overlay.
 *
 * A feature owns its settings: it declares its rows and hands them over from its
 * install. mods/menu/ renders whatever is registered and knows no feature, so
 * adding a mod with a setting touches nothing inside the overlay.
 *
 * A row is one of TWO KINDS, and the kind is `text != NULL`:
 *
 *   CHOICE  `state` is an index into `values[0 .. nvalues-1]`. OFF/ON is not a
 *           kind of its own -- it is kit_off_on, and AUTO/MANUAL is the same row
 *           with two strings of its own.
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

    /* Unique among SIBLINGS -- the rows sharing this row's `parent`. A level is
     * walked in idx order, so a child of one feature can never collide with a
     * child of another. Top-level rows take the KIT_IDX_* values below;
     * children number from 0 inside their parent. */
    unsigned char idx;

    /* NULL = top level. The row is revealed when its parent is itself live AND
     * *parent->state == show_when, so a whole subtree goes away with its root
     * and no row can be edited into a state nothing reads. */
    const struct kit_row *parent;
    int                   show_when;

    /* After a commit and BEFORE the value is persisted: a feature that rejects
     * what was typed clears it here, and the cleared value is what lands on
     * disk. NULL when there is nothing to do. */
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

/* Top-level idx, in one place: a feature cannot pick a number without seeing
 * the ones already taken. Gaps of ten, so inserting a feature between two
 * others renumbers nobody. */
#define KIT_IDX_GATE   10
#define KIT_IDX_SMART  15
#define KIT_IDX_PREVIEW 17
#define KIT_IDX_THEME  20
#define KIT_IDX_XPAD   25
#define KIT_IDX_STEMS  30

/* How many rows can be SEEN at once, and therefore how many exist as far as a
 * DJ is concerned. The overlay sizes the list for MOD_ROWS_VISIBLE rows -- what
 * the UTILITY panel has room for, with no scrollbar -- and row 0 is the
 * "MOD SETTINGS" title, so this is one less. Every mod on with STEMS set to
 * MANUAL is eight rows.
 *
 * The live count is dynamic (revealing children grows it), so this is enforced
 * when the list is flattened rather than at registration: the overflow is
 * logged and the surplus dropped. A row that is silently invisible and
 * unreachable is the failure this exists to prevent. */
#define KIT_MENU_MAX_ROWS 9

/* The longest buffer a TEXT row may hand over. The overlay keeps the pre-edit
 * value in a buffer of its own, which has to be sized for any row. */
#define KIT_ROW_TEXT_MAX 64

/* Register `n` rows. The array must have static storage: it is kept by
 * reference, and a child's `parent` points into it. Rows that are not usable as
 * declared are logged and skipped. */
void kit_menu_add(const struct kit_row *rows, int n);   /* [init] */

/* The flattened LIVE list: top-level rows in idx order, each revealed child
 * directly under its parent. Positions therefore shift as gates open and close,
 * and a row is addressed by its position and never by a fixed index. */
int                   kit_menu_count(void);         /* [message] */
const struct kit_row *kit_menu_row(int i);          /* [message] NULL past the end */

/* Why there is no list, or NULL when there is one. The rows are all-or-nothing:
 * a clash between siblings disables every row rather than showing a tree that
 * is not the one a feature declared. A caller about to show the overlay puts
 * this on the glass instead. */
const char *kit_menu_problem(void);                 /* [message] */


#ifdef __cplusplus
}
#endif

#endif /* EP122_MOD_KIT_MENU_H */
