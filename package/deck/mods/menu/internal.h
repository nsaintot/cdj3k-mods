// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * menu/internal.h - private to menu/.
 *
 * The MOD SETTINGS overlay: a component of our own over the UTILITY view, with a
 * category rail, a settings list and a value pane. It copies the stock UTILITY
 * look: the geometry below and the colours and fonts in kit/ui.h.
 *
 * Categories, rows and pages come from the features through the kit_menu
 * instance (../kit/menu.h); nothing here knows a feature.
 */
#ifndef EP122_MOD_MENU_INTERNAL_H
#define EP122_MOD_MENU_INTERNAL_H

#include "core/mod_core.h"
#include "core/mod_settings.h"
#include "juce/juce.h"
#include "juce/draw.h"
#include "kit/menu.h"
#include "kit/ui.h"
#include "kit/popup.h"

#ifdef __cplusplus
extern "C" {
#endif


#define FN_COMP_REPAINT     ep122_sym(EP122_JUCE_COMP_REPAINT)
#define FN_COMP_TOFRONT     ep122_sym(EP122_JUCE_COMP_TOFRONT)

/* ---- UtilityView ---- */
#define VIEW_FOCUS_OFF      0x388   /* stock focus level: 0 rail, 1 list, 2 pane */
#define VIEW_RMODEL_OFF     0x3c8   /* DJSettingRightPaneTableModel*             */
#define VIEW_VERLBL_OFF     0x4b0   /* the "Ver.X.XX" juce::Label                */
#define VIEW_EDITOR_OFF     0x778   /* the view's juce::TextEditor               */
#define VIEW_KBD_OFF        0x780   /* gui::SoftwareKeyboardPopupWidget*         */

/* The dot images on the DJ right-pane model, one juce::Image each. */
#define RMODEL_DOT_ON_OFF   0x18
#define RMODEL_DOT_OFF_OFF  0x20

/* Button events (view vt+0x188). */
#define BTN_ID_BACK         0x173fa74b06b07b5ULL
#define BTN_TYPE_BACK       2

/* Ver label hit box, in the view's mouseDown coordinates. */
#define VER_HIT_X_MIN       1080
#define VER_HIT_Y_MAX       45
#define MEVENT_POS_OFF      0x00    /* MouseEvent: position x, y floats */

#define FN_LABEL_SETTEXT    ep122_sym(EP122_JUCE_LABEL_SETTEXT)  /* setText(const String&, NotificationType) */
#define LABEL_TEXT_COLOUR_ID 0x1000281
#define LABEL_LASTTEXT_OFF  0x170   /* juce::Label::lastTextValue */
#define VER_COLOUR_IDLE     0xffe8e8e8u
#define VER_MOD_TAG         "-m"
#define MENU_TITLE          "MOD SETTINGS"
#define MENU_TITLE_W        600     /* the title box while open; the Ver label starts at x 650 */
#define NOTIFY_NONE         0

/* ---- geometry: the overlay covers the view below the header band ---- */
#define OV_X                0
#define OV_Y                90
#define OV_W                1280
#define OV_H                540     /* down to the bottom rule; the player bar stays visible */

/* Everything below is in overlay coordinates (view y - OV_Y). */
#define RAIL_X              0
#define RAIL_Y              3
#define RAIL_W              101
#define RAIL_H              70
#define RAIL_PITCH          72
#define RAIL_CELLS          7       /* stock paints seven cells, used or not */

#define LIST_X              103
#define LIST_Y              4
#define LIST_W              743
#define LIST_ROWS_SHOWN     10
#define ROW_H               KIT_UI_ROW_H
#define LIST_H              (LIST_ROWS_SHOWN * ROW_H)

#define PANE_X              852
#define PANE_Y              4
#define PANE_W              428
#define PANE_H              532

/* A page fills the list and pane area. */
#define PAGE_X              LIST_X
#define PAGE_Y              LIST_Y
#define PAGE_W              (OV_W - LIST_X)
#define PAGE_H              PANE_H

#define COL_RAIL_OFF        0xff969696u   /* rail label, category not selected */

/* ---- focus ---- */
/* FOCUS_LIST is also a page's focus. */
enum ov_focus { FOCUS_RAIL, FOCUS_LIST, FOCUS_PANE };

/* ---- shared state (overlay.cc) ---- */
extern uintptr_t     menu_g_view;    /* UtilityView, captured from its hooks      */
extern int           menu_g_open;    /* the overlay is on screen                  */
extern struct kit_menu *menu_g_menu; /* the instance the features registered with */
extern int           menu_g_cat;     /* category shown, index in rail order       */
extern int           menu_g_row;     /* list row the cursor is on                 */
extern enum ov_focus menu_g_focus;


/* ---- overlay.cc ---- */
int  menu_overlay_ok(void);             /* every primitive resolved */
int  menu_overlay_built(void);          /* components exist, parented to menu_g_view */
void menu_open(void);
void menu_close(void);
void menu_turn(int delta);              /* rotary */
void menu_push(void);                   /* rotary push */
void menu_back(void);                   /* BACK */
void menu_select_cat(int c);
void menu_set_focus(enum ov_focus f);
void menu_list_changed(void);           /* the live rows may have changed */
void menu_commit(int value);            /* apply a value to the row under the cursor */
void menu_style_ver(void);

/* ---- rail.cc ---- */
int  menu_rail_build(uintptr_t parent);
void menu_rail_show(int c);

/* ---- page.cc ---- */
int  menu_page_build(uintptr_t parent);
void menu_page_show(const struct kit_page *p);   /* NULL hides it */
void menu_page_repaint(void);

/* ---- lists.cc ---- */
const struct kit_row *menu_row(void);    /* the row under the cursor, or NULL */
int       menu_lists_build(uintptr_t parent);
void      menu_list_reload(void);       /* rows of menu_g_cat; keeps menu_g_row */
void      menu_list_select(int row);
void      menu_pane_reload(void);       /* values of the row under the cursor */
void      menu_pane_select(int idx);
int       menu_pane_cursor(void);
void      menu_lists_repaint(void);
void      menu_lists_show(int visible);
int       menu_list_row_y(int row);     /* view y of a list row's top edge, -1 if hidden */
void      menu_list_fit(int bottom);    /* end the list above view y `bottom`; -1 restores it */

/* ---- editor.c ---- */
int  menu_kbd_install(void);
void menu_kbd_open(const struct kit_row *r);
void menu_kbd_close(void);
int  menu_kbd_is_up(void);
const struct kit_row *menu_kbd_row(void);

#ifdef __cplusplus
}
#endif

#endif /* EP122_MOD_MENU_INTERNAL_H */
