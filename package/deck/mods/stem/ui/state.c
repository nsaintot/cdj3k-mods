// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * ui/state.c - the state every part of the row shares
 *
 * Part of the STEMS play-screen UI. The shared contract, and the reasoning
 * behind the design, is in ui.h.
 */
#include "stem/ui/ui.h"

/* ================================================================== */
/* State                                                              */
/* ================================================================== */

uintptr_t stems_g_orig_ta_paint, stems_g_orig_ta_mousedown;
uintptr_t stems_g_orig_drc_timer;   /* the app's display refresh, chained by our tick */
int       stems_g_dumped_stock;   /* the app's own open layout has been logged once */
int       stems_g_api_ok;        /* every juce primitive verified at install */
int       stems_g_built;         /* the button + row exist and are attached  */
int       stems_g_building;      /* re-entry guard: building repaints        */
int       stems_g_row_open;      /* the STEMS button is lit / the row is up  */

uintptr_t stems_g_btn_stems;     /* the 4th quick-menu button (a Label)       */
/* Its state, read by the paint hook, which lays the stipple before Label::paint (the
 * Label's own background is transparent). Stored as state rather than ARGB so the
 * colour is resolved at paint from the theme in force. */
enum btn_state stems_g_btn_state = BTN_OFF;
uintptr_t stems_g_warn;          /* the badge in its corner, hidden unless earned */
/* Blink budget, counted down on the display tick; its value also gives the phase. */
int       stems_g_warn_blink;
int       stems_g_warn_up;       /* the badge is currently shown               */
uintptr_t stems_g_edit;          /* the edit mark, in the other corner            */
uint32_t  stems_g_edit_col;      /* what it is currently painted, 0 = hidden   */
uintptr_t stems_g_row;           /* the control strip (a Label, used as a panel) */
uintptr_t stems_g_btn_bypass;  /* BYPASS, left of the wedges              */
uintptr_t stems_g_caption[N_STEMS];

/* The row holds two full-size containers and shows exactly one, so swapping state is
 * two setVisible calls. */
uintptr_t stems_g_controls;      /* BYPASS + captions + wedges                    */
uintptr_t stems_g_progress;      /* the processing bar                            */
uintptr_t stems_g_prog_fill, stems_g_prog_text, stems_g_prog_track;
uintptr_t stems_g_prog_mark[N_PROG_BOUND];  /* one per handover boundary */
/* The bar's rect inside the progress container. Y is derived from the wedge band at
 * build time (stems_build_row). */
int32_t   stems_g_prog_x, stems_g_prog_y, stems_g_prog_w;
int       stems_g_bypass_on;   /* bypass: stems out of circuit, wedges inert     */

/* The audio thread's view of the row (see ../stem.h), so the audio callback never
 * reads UI state. */
float   g_stem_gain[N_STEMS] = { 1.0f, 1.0f, 1.0f };
int     g_stem_bypass;
int       stems_g_processing;

const char *const k_stem_name[N_STEMS] = { "DRUMS", "HARMONICS", "VOCALS" };



/* Our clone of the juce::Label vtable; see ui.h. */
uintptr_t stems_g_label_vt[VT_CLONE_WORDS];
uintptr_t stems_g_label_vptr;
uintptr_t stems_g_label_mouseup;  /* stock juce::Label::mouseUp, chained by ours */
