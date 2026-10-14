// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * kit/menu.h - what a feature puts in MOD SETTINGS.
 *
 * MOD SETTINGS is an instance (struct kit_menu) handed to every feature whose
 * mod descriptor has a `menu` callback (kit/mod.h). The feature registers rows,
 * or a page with its own handlers, under a category. mods/menu/ renders what
 * is registered and names no feature.
 *
 * Categories
 *   A category is a rail entry. Its descriptor is declared by the feature that
 *   owns it; categories that several features list rows under are declared
 *   here (kit_cat_player, kit_cat_display). A category holds rows or one page,
 *   not both.
 *
 * Rows, one of three kinds:
 *   CHOICE  `state` is an index into `values[0 .. nvalues-1]`. OFF/ON rows use
 *           kit_off_on; AUTO/MANUAL is a CHOICE row with its own two strings.
 *   TEXT    `text` is the buffer the deck's software keyboard edits.
 *   INFO    `info` is shown as the value and cannot be edited.
 *
 * Pages
 *   A page draws its own content and takes the input while it has focus. Draw
 *   with kit/ui.h to match the stock screens.
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


struct kit_menu;

/* ---- categories ---- */

struct kit_cat {
    const char *id;      /* unique; features sharing a category pass the same descriptor */
    const char *label;   /* rail label, "\n" between lines */
    short       order;   /* rail position, ascending */
};

/* Rail order, kept in one place so positions cannot collide. */
#define KIT_ORDER_PLAYER   10
#define KIT_ORDER_STEMS    20
#define KIT_ORDER_DISPLAY  30
#define KIT_ORDER_ABOUT    90

/* The rail has room for this many categories. */
#define KIT_MENU_MAX_CATS  7

/* Shared categories. */
extern const struct kit_cat kit_cat_player;    /* PLAYER SETTING */
extern const struct kit_cat kit_cat_display;   /* DISPLAY */

/* ---- rows ---- */

/* The value list every OFF/ON row shares. */
extern const char *const kit_off_on[2];

struct kit_row {
    /* Keep it short: like the stock lists, a label that does not fit is
     * squeezed and can run over the value. */
    const char *label;

    /* Unique among siblings (rows sharing this row's `parent`); each level is
     * walked in idx order. Top-level rows of a shared category take the
     * KIT_IDX_* values below; children number from 0 inside their parent. */
    unsigned char idx;

    /* NULL = top level. The row is shown only while its parent is shown and
     * *parent->state == show_when, so a subtree disappears with its root. */
    const struct kit_row *parent;
    int                   show_when;

    /* Called after a commit and before the value is saved. A feature that
     * rejects the value clears it here, and the cleared value is saved. NULL
     * if not needed. */
    void (*changed)(void);

    /* CHOICE */
    int               *state;
    const char *const *values;
    unsigned char      nvalues;

    /* TEXT */
    char           *text;
    unsigned short  text_cap;

    /* INFO */
    const char *info;
};

/* The common case: a two-value row over an int flag. Trailing designated
 * initialisers (idx, parent, changed) follow the state pointer. */
#define KIT_ROW_BOOL(lbl, st, ...) \
    { .label = (lbl), .state = (st), .values = kit_off_on, .nvalues = 2, __VA_ARGS__ }

/* Row order in the shared categories, spaced so a row can be inserted. */
#define KIT_IDX_GATE    10
#define KIT_IDX_SMART   15
#define KIT_IDX_PREVIEW 17
#define KIT_IDX_XPAD    25
#define KIT_IDX_THEME   20

/* The longest buffer a TEXT row may have; the overlay's pre-edit copy is sized
 * for it. */
#define KIT_ROW_TEXT_MAX 64

/* ---- pages ---- */

/* A page fills the content area right of the rail. Every handler may be NULL. */
struct kit_page {
    void *ctx;

    /* Draw the content area, `w` x `h`, origin at its top left. */
    void (*paint)(void *ctx, void *g, int w, int h);

    /* Input while the page has focus. Only `back`'s return is read: 0 gives
     * focus back to the rail. */
    int  (*turn)(void *ctx, int delta);
    int  (*push)(void *ctx);
    int  (*back)(void *ctx);

    /* A touch in the content area, in its coordinates. It gives the page focus. */
    void (*tap)(void *ctx, int x, int y);

    /* The page came on screen (1) or left it (0). */
    void (*shown)(void *ctx, int shown);
};

/* ---- registration [init] ---- */

/* Register `n` rows under `cat`. Arrays and descriptors must have static
 * storage: both are kept by reference. Rows not usable as declared are logged
 * and skipped. Returns the rows kept. */
int kit_menu_rows(struct kit_menu *m, const struct kit_cat *cat,
                  const struct kit_row *rows, int n);

/* Register a page as `cat`'s content. -1 if the category already has content. */
int kit_menu_page(struct kit_menu *m, const struct kit_cat *cat, const struct kit_page *page);

/* Ask for the page to be drawn again; nothing happens while it is off screen. */
void kit_menu_page_repaint(struct kit_menu *m);                       /* [message] */

/* The instance core/common.c hands to the mods. [init, message] */
struct kit_menu *kit_menu_instance(void);

/* ---- the overlay's side [message] ---- */

/* Categories in rail order. */
int                    kit_menu_ncats(const struct kit_menu *m);
const struct kit_cat  *kit_menu_cat(const struct kit_menu *m, int c);
const struct kit_page *kit_menu_cat_page(const struct kit_menu *m, int c);  /* NULL: rows */

/* One category's live rows: top-level rows in idx order, each shown child
 * directly under its parent. Positions shift as rows appear and disappear. */
int                   kit_menu_count(const struct kit_menu *m, int c);
const struct kit_row *kit_menu_row(const struct kit_menu *m, int c, int i);  /* NULL past the end */

/* Why MOD SETTINGS cannot open, or NULL. Nothing registered, a duplicate
 * sibling idx, or a child whose parent was never registered disables all rows. */
const char *kit_menu_problem(const struct kit_menu *m);

/* Set by the overlay: how kit_menu_page_repaint reaches the screen. */
void kit_menu_set_repaint(struct kit_menu *m, void (*repaint)(void));


#ifdef __cplusplus
}
#endif

#endif /* EP122_MOD_KIT_MENU_H */
