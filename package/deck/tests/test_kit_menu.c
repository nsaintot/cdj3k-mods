// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * test_kit_menu.c - the MOD SETTINGS instance (mods/kit/menu.c): categories,
 * rows, pages and each category's live row list.
 *
 * The instance is a file static, so this binary registers one set of features
 * and checks the menu as the parent rows' states change.
 */
#include "mods/kit/menu.h"

#include "test.h"

static int  g_gate, g_smart, g_stems, g_manual, g_theme;
static char g_addr[16];

static const char *const k_where[2] = { "AUTO", "MANUAL" };
static const char *const k_themes[3] = { "A", "B", "C" };

/* Two features sharing PLAYER SETTING, registered out of idx order. */
static const struct kit_row k_smart[] = { KIT_ROW_BOOL("SMART", &g_smart, .idx = 15) };
static const struct kit_row k_gate[]  = { KIT_ROW_BOOL("GATE", &g_gate, .idx = 10) };

/* A feature with a category of its own. */
static const struct kit_cat k_cat_stems = { "stems", "STEMS", KIT_ORDER_STEMS };
static const struct kit_row k_stems[] = {
    KIT_ROW_BOOL("STEMS", &g_stems),
    { .label = "WHERE", .parent = &k_stems[0], .show_when = 1,
      .state = &g_manual, .values = k_where, .nvalues = 2 },
    { .label = "ADDR", .parent = &k_stems[1], .show_when = 1,
      .text = g_addr, .text_cap = sizeof(g_addr) },
};

static const struct kit_row k_theme[] = {
    { .label = "THEME", .idx = KIT_IDX_THEME, .state = &g_theme, .values = k_themes, .nvalues = 3 },
};

static const struct kit_row k_bad[] = {
    { .label = "TWO KINDS", .idx = 51, .info = "x", .text = g_addr, .text_cap = sizeof(g_addr) },
    { .label = "ONE VALUE", .idx = 52, .state = &g_gate, .values = k_where, .nvalues = 1 },
    { .label = "OK", .idx = 53, .info = "v1" },
};

/* A page, registered by a feature that draws its own content. */
static int g_pushed;
static int page_push(void *ctx) { (void)ctx; g_pushed++; return 1; }
static const struct kit_cat  k_cat_status = { "status", "STATUS", 15 };
static const struct kit_page k_page = { .push = page_push };

static int cat_of(struct kit_menu *m, const char *id)
{
    int i;

    for (i = 0; i < kit_menu_ncats(m); i++)
        if (strcmp(kit_menu_cat(m, i)->id, id) == 0) return i;
    return -1;
}

static const char *label(struct kit_menu *m, int c, int i)
{
    const struct kit_row *r = kit_menu_row(m, c, i);

    return r ? r->label : NULL;
}

int main(void)
{
    struct kit_menu *m = kit_menu_instance();
    static const struct kit_cat k_cat_about = { "about", "ABOUT", KIT_ORDER_ABOUT };
    int player, stems, display, about, status;

    T_CASE("a page alone makes a menu, after an early query");
    CHECK(kit_menu_problem(m) != NULL);         /* nothing registered yet */
    CHECK_INT(kit_menu_page(m, &k_cat_status, &k_page), 0);
    CHECK(kit_menu_problem(m) == NULL);
    CHECK_INT(kit_menu_page(m, &k_cat_status, NULL), -1);

    T_CASE("a category whose rows are all refused takes no rail entry");
    {
        static const struct kit_cat k_cat_empty = { "empty", "EMPTY", 50 };

        CHECK_INT(kit_menu_rows(m, &k_cat_empty, k_bad, 2), 0);
        CHECK_INT(kit_menu_ncats(m), 1);
    }

    CHECK_INT(kit_menu_rows(m, &kit_cat_player, k_smart, 1), 1);
    CHECK_INT(kit_menu_rows(m, &k_cat_about, k_bad, 3), 1);
    CHECK_INT(kit_menu_rows(m, &k_cat_stems, k_stems, 3), 3);
    CHECK_INT(kit_menu_rows(m, &kit_cat_display, k_theme, 1), 1);
    CHECK_INT(kit_menu_rows(m, &kit_cat_player, k_gate, 1), 1);

    T_CASE("the rail, in order");
    CHECK_INT(kit_menu_ncats(m), 5);
    player = cat_of(m, "player");
    status = cat_of(m, "status");
    stems = cat_of(m, "stems");
    display = cat_of(m, "display");
    about = cat_of(m, "about");
    CHECK_INT(player, 0);
    CHECK_INT(status, 1);
    CHECK_INT(stems, 2);
    CHECK_INT(display, 3);
    CHECK_INT(about, 4);
    CHECK_STR(kit_menu_cat(m, player)->label, "PLAYER\nSETTING");
    CHECK(kit_menu_cat(m, 5) == NULL);

    T_CASE("features sharing a category, in idx order");
    CHECK(kit_menu_problem(m) == NULL);
    CHECK_INT(kit_menu_count(m, player), 2);
    CHECK_STR(label(m, player, 0), "GATE");
    CHECK_STR(label(m, player, 1), "SMART");
    CHECK(kit_menu_row(m, player, 2) == NULL);
    CHECK(kit_menu_row(m, player, -1) == NULL);
    CHECK_STR(label(m, display, 0), "THEME");

    T_CASE("malformed rows are refused, the rest stay");
    CHECK_INT(kit_menu_count(m, about), 1);
    CHECK_STR(label(m, about, 0), "OK");

    T_CASE("a page category has no rows and takes no more content");
    CHECK(kit_menu_cat_page(m, status) == &k_page);
    CHECK(kit_menu_cat_page(m, player) == NULL);
    CHECK_INT(kit_menu_count(m, status), 0);
    CHECK_INT(kit_menu_rows(m, &k_cat_status, k_gate, 1), 0);
    CHECK_INT(kit_menu_page(m, &kit_cat_player, &k_page), -1);
    CHECK_INT(kit_menu_cat_page(m, status)->push(NULL), 1);
    CHECK_INT(g_pushed, 1);

    T_CASE("children follow the gates, under their root's category");
    CHECK_INT(kit_menu_count(m, stems), 1);
    g_stems = 1;
    CHECK_INT(kit_menu_count(m, stems), 2);
    CHECK_STR(label(m, stems, 1), "WHERE");
    g_manual = 1;
    CHECK_INT(kit_menu_count(m, stems), 3);
    CHECK_STR(label(m, stems, 2), "ADDR");
    g_stems = 0;                               /* the subtree goes with its root */
    CHECK_INT(kit_menu_count(m, stems), 1);

    T_CASE("the same idx in two categories is fine");
    {
        static const struct kit_row other[] = { KIT_ROW_BOOL("OTHER", &g_gate, .idx = 10) };

        kit_menu_rows(m, &kit_cat_display, other, 1);
    }
    CHECK(kit_menu_problem(m) == NULL);
    CHECK_INT(kit_menu_count(m, display), 2);

    T_CASE("an idx clash inside a category disables every row");
    {
        static const struct kit_row clash[] = { KIT_ROW_BOOL("CLASH", &g_gate, .idx = 10) };

        kit_menu_rows(m, &kit_cat_player, clash, 1);
    }
    CHECK(kit_menu_problem(m) != NULL);
    CHECK_INT(kit_menu_count(m, player), 0);

    return t_done("kit_menu");
}
