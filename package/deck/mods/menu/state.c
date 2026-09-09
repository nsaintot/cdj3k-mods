// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * menu/state.c - the state the overlay's files share. Declarations and their
 * documentation are in internal.h.
 *
 * All message-thread state: JUCE calls every hook in menu/ from the thread that
 * owns the UI, so none of it is synchronised.
 */
#include "menu/internal.h"

uintptr_t menu_g_orig_numrows, menu_g_orig_paintcell, menu_g_orig_mousedown, menu_g_orig_switch,
                 menu_g_orig_touchsel, menu_g_orig_selchanged, menu_g_orig_rowchanged, menu_g_orig_rcellclick,
                 menu_g_orig_input, menu_g_orig_rselchanged, menu_g_orig_rrefresh,
                 menu_g_orig_rnumrows;
int       menu_g_render_ok;   /* JUCE render primitives verified at install */
int       menu_g_mod_mode;    /* 1 while the mod overlay is drawing over DJ SETTING */
int       menu_g_list_rows = MOD_LIST_ROWS_STOCK;
uintptr_t menu_g_model;       /* DJSettingTableModel, captured in paintCell */
uintptr_t menu_g_view;        /* UTILITY view, captured from the hooks that get it */
int       menu_g_bouncing;    /* re-entry guard for the title-row bounce */
int       menu_g_rsel_guard;  /* set while WE drive the right pane, so its selection
                                 * callback does not adopt a value we just wrote */
int       menu_g_in_input;    /* set while the stock focus/rotary dispatch is running,
                                 * which is how a rotary tick is told from a touch tap */
int       menu_g_strarr_ok;   /* juce::StringArray primitives verified at install */
int       menu_g_setstr_ok;   /* right-model setStrings verified at install */
int       menu_g_kbd_ok;      /* software-keyboard show/hide verified at install */

int       menu_g_editor_ok;
uintptr_t menu_g_orig_kbdkey;
int       menu_g_setting_row = MOD_ROW_FIRST;
