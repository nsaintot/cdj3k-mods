// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * menu/internal.h - private to menu/.
 *
 * The overlay spans one list model, one view and one right-pane model, so its
 * files share state: the cursor row, whether the overlay is armed, and the stock
 * functions the hooks chain to. That state lives in menu/state.c.
 *
 * Nothing here knows a feature. The rows are whatever the features registered
 * (../kit/menu.h); this directory renders them and edits them.
 *
 * The EP122 offsets and primitives below are the overlay's whole ABI surface.
 * Nothing in here is visible outside the directory: hooks.c registers the mod
 * (../kit/mod.h) and that descriptor is the overlay's only entry point.
 */
#ifndef EP122_MOD_MENU_INTERNAL_H
#define EP122_MOD_MENU_INTERNAL_H

#include "core/mod_core.h"
#include "juce/juce.h"
#include "juce/draw.h"
#include "kit/menu.h"
#include "kit/popup.h"
#include "core/mod_settings.h"

#ifdef __cplusplus
extern "C" {
#endif


/* ---- DJSettingTableModel row hooks (vtable 0x2152cc0) ---- */
#define MOD_VT_NUMROWS_SLOT   (ep122_sym(EP122_DJSET_MODEL) + 0x10)  /* vtable[0x10] getNumRows */
#define MOD_VT_PAINTCELL_SLOT (ep122_sym(EP122_DJSET_MODEL) + 0x20)  /* vtable[0x20] paintCell  */
#define MOD_FN_NUMROWS        ep122_sym(EP122_DJSET_NUMROWS)
#define MOD_FN_PAINTCELL      ep122_sym(EP122_DJSET_PAINTCELL)

/* ---- UTILITY view (vtable 0x20d9610) ---- */
#define VIEW_FOCUS_OFF        0x388        /* focus level (0 category / 1 list / 2 option) */
#define VIEW_CAT_OFF          0x38c        /* current category index                       */
#define FN_SWITCH             ep122_sym(EP122_VIEW_SWITCH)  /* rotary category nav (sub_1447b10)            */
#define MOD_SWITCH_SLOT       (ep122_sym(EP122_UTILITY_VIEW) + 0x198)  /* view vtable+0x198 -> FN_SWITCH (unlatch hook) */
/* Touch category-select handler (view vtable+0x178). Framework-invoked when a
 * sidebar item is tapped; applies the picked category and writes view+0x38c via
 * the per-category setup fns (DJ SETTING = sub_14361e0, ends *(view+0x38c)=0).
 * Touch analogue of the rotary nav sub_1447b10. The old sub_144af70 hook
 * (vtable+0x190) never fired on touch -- it early-returns when its arg3==0. */
#define FN_TOUCHSEL           ep122_sym(EP122_VIEW_TOUCHSEL)  /* sub_1448088: touch category-select handler */
#define MOD_TOUCHSEL_SLOT     (ep122_sym(EP122_UTILITY_VIEW) + 0x178)  /* view vtable+0x178 -> FN_TOUCHSEL (touch unlatch hook) */

/* juce ListBox::updateContent (sub_1bdb410): reads model @ list+0xd8, calls its
 * getNumRows (vtable+0x10 -> our hook), rebuilds the row layout + viewport. We
 * call it on the DJ SETTING list to make it re-query the row count after the
 * mode flips (a plain repaint keeps the cached 8 rows). */
#define FN_UPDATECONTENT      ep122_sym(EP122_JUCE_LISTBOX_UPDATE)
#define VIEW_DJLIST_OFF       0x3f8        /* *(view+0x3f8) = cat-0 (DJ SETTING) left list */
#define VIEW_MODEL_REF_OFF    0x390        /* model ref the cat-0 list-nav clamps against    */
#define MODEL_GETNUMROWS_SLOT 2            /* model vtable +0x10: ListBoxModel::getNumRows */
#define LIST_MODEL_SCAN       0x200        /* bytes of the list to search for our model ptr */

/* ---- UTILITY view mouseDown (vtable+0x28) : the "Ver." touch entry ----
 * sub_143cce8 (view mouseDown) only dismisses a popup and ignores the tap
 * coordinates, so a tap on the non-interactive "Ver.X.XX" label falls through
 * to it. We hook the slot and read the MouseEvent ourselves. */
#define MOD_VT_MOUSEDOWN_SLOT (ep122_sym(EP122_UTILITY_VIEW) + 0x28)  /* view vtable+0x28 (mouseDown)         */
#define MOD_FN_MOUSEDOWN      ep122_sym(EP122_VIEW_MOUSEDOWN)   /* j_sub_143cce8 (mouseDown thunk)      */
#define MEVENT_POS_OFF        0x00          /* MouseEvent: position x,y floats at +0x00/+0x04 */
/* "Ver.X.XX" hit box in screen pixels. Coords are NOT mirrored at the JUCE
 * layer -- live Ver taps land at x 1127..1236, y 13..22. Top-right corner. */
#define VER_HIT_X_MIN         1080
#define VER_HIT_Y_MAX         45

/* Font-height fields on the model + theme colours (see stock paintCell / sub_159c980). */
#define MODEL_LBL_FONTH_OFF   0x18         /* label font height (float) -- larger, white */
#define MODEL_VAL_FONTH_OFF   0x14         /* value font height (float) -- smaller, grey */
/* The overlay title and the armed Ver label wear the ACCENT ROLE, so they follow the
 * theme in force -- see theme/roles.c. A literal here would put the mod menu and the
 * stem row on different accents. */
#define MOD_HEADER_COLOUR     (mod_ui()->accent)

/* ---- "MOD SETTINGS" title row must not be selectable ----
 * JUCE has no per-row selectable flag, so instead we bounce: selectedRowsChanged
 * (model vtable+0x60) sees row 0 and re-selects row 1 via the stock list-nav,
 * which computes the row rect itself. */
#define MOD_VT_SELCHANGED_SLOT (ep122_sym(EP122_DJSET_MODEL) + 0x60)  /* model vtable+0x60 (selectedRowsChanged) */
#define MOD_FN_SELCHANGED      ep122_sym(EP122_DJSET_SELCHANGED)

/* ---- "Ver.X.XX" affordance (view+0x4b0 styled juce::Label) ----
 * The label is a juce::Label subclass built from the skin key
 * "txt_#afafaf_Ver_r"; recolouring it flags the hidden toggle. Component::setColour
 * stores under "jcclr_<id>" then calls colourChanged() (vtable+0x150) -> repaint,
 * so no paint hook is needed. Standard JUCE colour ids are in use in this build
 * (the ctor sets 0x10002xx on a TextEditor), so Label::textColourId applies. */
/* ---- right pane (the value list for the selected row) ----
 * The left list's selectedRowsChanged notifies a listener subobject living at
 * view+0x180; its vtable slot 0 (`sub_143ced0`) adjusts to the view and calls
 * `sub_1435c68(view, row)`, which is the whole right-pane rebuild: it stores the
 * option-set id at rightmodel+0x128, pushes that set's strings into the model, then
 * updateContent()s the right list and selects the current value's index.
 * Row ids 0..7 map to the stock option arrays at view+0x4b8 + i*0x18; anything else
 * takes the default branch -> id 8 with an EMPTY string list (blank pane).
 * So we re-point the rebuild: the mod title row asks for the empty set, and GATE CUE
 * borrows set 4 (the OFF/ON array at view+0x518) to get natively-styled OFF/ON rows,
 * after which we stamp the id back to 8 so a tap can't write a real DJ setting, and
 * re-select the row that matches the gate state. */
#define MOD_VT_ROWCHANGED_SLOT (ep122_sym(EP122_UTILITY_AS_MODEL_LISTENER) + 0x0)  /* listener (view+0x180) vtable slot 0 */
#define MOD_FN_ROWCHANGED      ep122_sym(EP122_VIEW_ROWCHANGED)
#define LISTENER_VIEW_DELTA    0x180        /* the listener subobject sits at view+0x180 */
#define VIEW_RIGHT_MODEL_OFF   0x3c8        /* *(view+0x3c8) = DJSettingRightPaneTableModel */
#define VIEW_RIGHT_LIST_OFF    0x430        /* *(view+0x430) = right pane TableListBox     */
#define RMODEL_OPTSET_OFF      0x128        /* option-set id on the right model            */
#define OPTSET_NONE            8            /* "no setting": empty list, writes disabled   */
#define OPTSET_OFF_ON          4            /* view+0x518 option array = OFF / ON          */
#define FN_SELECT_ROW          ep122_sym(EP122_JUCE_LISTBOX_SELECTROW)  /* juce::ListBox::selectRow(row, dontScroll, deselectOthers) */

/* Right-pane cellClicked (its model's vtable+0x30): the stock impl looks the clicked
 * string up (model+0xf8) and hands (row, String) to the view's write listener. While
 * armed we apply the value ourselves and never call it, so no stock setting is written. */
#define MOD_VT_RCELLCLICK_SLOT (ep122_sym(EP122_DJSET_RMODEL) + 0x30)  /* right model vtable+0x30 (cellClicked) */
#define MOD_FN_RCELLCLICK      ep122_sym(EP122_DJSET_RCELLCLICK)

/* Right-pane selection (its model's vtable+0x60). cellClicked above never fires for a
 * TOUCH tap on a value: unlike the left list this model overrides refreshComponentForCell
 * (0x159b988), so every value row is a real radio Component that consumes the tap itself.
 * The selection change is what both touch AND rotary produce, so that is where the value
 * is adopted from. */
#define MOD_VT_RSELCHANGED_SLOT (ep122_sym(EP122_DJSET_RMODEL) + 0x60) /* right model vtable+0x60 (selectedRowsChanged) */
#define MOD_FN_RSELCHANGED      ep122_sym(EP122_DJSET_RSELCHANGED)

/* Right-pane refreshComponentForCell (its model's vtable+0x28). It picks the dot colour by
 * comparing the row against model+0x10 -- so that field, not the selection, is what marks
 * the value in force. Writing it once when the value changes is not enough: the stock
 * value-chosen path restamps it (asynchronously, and from the option set we disowned), so
 * the dot ended up showing the PREVIOUS value. This hook runs per row on every draw, which
 * makes the selected row's own flag the single source of truth for the dot. */
#define MOD_VT_RREFRESH_SLOT    (ep122_sym(EP122_DJSET_RMODEL) + 0x28) /* right model vtable+0x28 (refreshComponentForCell) */
#define MOD_FN_RREFRESH         ep122_sym(EP122_DJSET_RREFRESH)
#define FN_REPAINT             ep122_sym(EP122_JUCE_COMP_REPAINT)  /* juce::Component::repaint() */
/* "enter DJ SETTING (category 0)": shows the cat-0 components, hides the others and
 * ends with *(view+0x38c)=0. Used to bring the overlay's host list to the front when
 * the Ver label is tapped from another category. */
#define FN_ENTER_DJSETTING     ep122_sym(EP122_ENTER_DJSETTING)

/* Focus-level input dispatch (view vtable+0x190). Level 0 -> category, level 1 ->
 * `sub_1444a98`, level 2 -> `sub_144aae8`. Level 1 TAIL-CALLS `sub_1435c68(view,row)`
 * DIRECTLY -- bypassing the listener hook -- so entering the right pane from the list
 * rebuilt it with the stock options for that row index. We re-assert our pane after it. */
#define MOD_INPUT_SLOT         (ep122_sym(EP122_UTILITY_VIEW) + 0x190)  /* view vtable+0x190 -> FN_INPUT */
#define FN_INPUT               ep122_sym(EP122_VIEW_INPUT)
#define FN_CURRENT_ROW         ep122_sym(EP122_JUCE_LISTBOX_CURRENTROW)  /* juce::ListBox current/selected row */
#define FOCUS_SETTING_LIST     1            /* view+0x388 focus level: centre list */
#define FOCUS_OPTION_PANE      2            /* view+0x388 focus level: right pane */
/* The right model's CHECKED value index -- what draws the filled radio. It is NOT the
 * list selection: refreshComponentForCell styles the row whose index equals model+0x10
 * as the current value (and sub_14361e0 seeds it from the list's current row). */
/* The row the model draws the selection dot on. It is an INDEX, and stock uses
 * it to index the option array behind the pane -- so anything we put here that
 * the array cannot answer for is a read off the end, on the message thread.
 *
 * That is not theory. Letting a theme index (0..4) reach this field crashed the
 * deck on the Ver-tap that opens the overlay, because the selected theme is
 * persisted and comes back as 3 on the next boot. */
#define RMODEL_CHECKED_OFF     0x10

/* The borrowed option set's own length. The pane can be LONGER than this -- see
 * menu_rnumrows -- but only after setStrings has put an array that long behind
 * it; until then this is what the model can answer for. */
#define MOD_PANE_ROWS_STOCK    2

/* Bounds the stack array that builds the value list. */
#define MOD_ROW_VALUES_MAX     8

/* juce::Component bounds: x, y, w, h as four int32. */
#define COMP_BOUNDS_OFF        0x20

/* A guard on how far the value list may grow, not a layout constant: the panel
 * is 720 tall, and a list that ran off the bottom would be worse than one that
 * scrolls. */
#define MOD_SCREEN_H           720

#define LABEL_TEXT_COLOUR_ID   0x1000281    /* juce::Label::textColourId */
#define VIEW_VERLBL_OFF        0x4b0        /* *(view+0x4b0) = the Ver.X.XX label */
#define VER_COLOUR_IDLE        0xffe8e8e8u  /* brighter than skin #afafaf: reads as tappable */
#define VER_COLOUR_ARMED       (mod_ui()->accent)   /* accent while the MOD overlay is armed */

/* ---- "-m": the mods are in ----
 * Installation is all-or-nothing (common.c returns before installing anything
 * if a single symbol is missing), so "the label says -m" and "every hook in
 * this file is live" are the same statement -- which makes the suffix a real
 * status readout rather than decoration, and the one place to look when a deck
 * comes up behaving stock.
 *
 * It changes the LABEL's own copy of the string and nothing else. UtilityView's
 * ctor builds "Ver." + version once and hands COPIES to its other consumers
 * before constructing this label, so the version this deck reports over ProLink
 * is untouched -- the suffix is on the glass only. */
#define FN_LABEL_SETTEXT       ep122_sym(EP122_JUCE_LABEL_SETTEXT)  /* juce::Label::setText(const String&, NotificationType) */
#define LABEL_LASTTEXT_OFF     0x170        /* juce::Label::lastTextValue (a juce::String) */
#define VER_MOD_TAG            "-m"
#define NOTIFY_NONE            0            /* juce::dontSendNotification */

/* JUCE render primitives (EP122-3.19 absolute VAs). */
#define FN_G_SETFONT   ep122_sym(EP122_JUCE_GFX_SETFONT)  /* juce::Graphics::setFont(Graphics*, Font*)        */
#define FN_G_SETCOL    ep122_sym(EP122_JUCE_GFX_SETCOLOUR)  /* juce::Graphics::setColour(Graphics*, Colour*)    */
#define FN_DRAW_TEXT   ep122_sym(EP122_JUCE_GFX_DRAWTEXT)  /* drawText(g,String*,x,y,w,h,Justification*,ellip) */

/* No prologue guards. Every primitive named below arrives from a masked
 * signature match or out of a vtable the RTTI walk found, either of which says
 * more about the target than its first four bytes ever did. */

/* Right-pane value strings. The stock rebuild (sub_1435c68) fills a local StringArray
 * from the option table, hands it over with this, then updateContent()s the list. It
 * SWAPS (sub_1a3c4c0), so after the call our array holds the model's previous strings
 * and destroying it frees them -- which is exactly what stock's throwaway copy does.
 * This is what lets a borrowed OFF/ON set read AUTO/MANUAL instead. */
#define FN_RMODEL_SETSTRINGS ep122_sym(EP122_RMODEL_SETSTRINGS)  /* setStrings(rightModel, juce::StringArray&) */

/* ---- software keyboard (gui::SoftwareKeyboardPopupWidget) ----
 * The view builds one in its constructor and keeps it; UTILITY > SYSTEM > HISTORY NAME
 * is the stock user of it, and its right-pane rebuild (sub_143a6e0, row 2) just calls
 * the show below. So a text row needs no widget of its own -- only these two calls.
 *
 * `show` seeds the editor at +0x778 from the view's own juce::String at +0x710, makes
 * the editor and the keyboard visible, and grabs focus on it; `hide` is its counterpart,
 * already invoked by the stock category-change paths (sub_1448088 / sub_14486e0, both of
 * which we already hook). Both reach the editor THROUGH +0x778, which is what lets a text
 * row stand its own editor in there and reuse the whole path unchanged. */
#define FN_KBD_SHOW        ep122_sym(EP122_KBD_SHOW)   /* show(view): seed editor, show keyboard, focus */
#define FN_KBD_HIDE        ep122_sym(EP122_KBD_HIDE)   /* hide(view)                                    */
#define VIEW_EDITOR_OFF    0x778         /* the juce::TextEditor showing the value        */
#define FN_EDITOR_SETTEXT  ep122_sym(EP122_JUCE_EDITOR_SETTEXT)   /* juce::TextEditor::setText(const String&, bool) */
/* The editor sits where the stock text setting is -- SYSTEM's HISTORY NAME, row 2 -- so
 * over our list it lands on whichever of our rows happens to be third. Move it onto the
 * row actually being edited. setTopLeftPosition keeps the width/height it reads from
 * +0x28/+0x2c, which pins the bounds rectangle at +0x20 as {x,y,w,h} int32s. */
#define FN_SET_TOPLEFT     ep122_sym(EP122_JUCE_COMP_SETTOPLEFT)   /* juce::Component::setTopLeftPosition(x, y) */
#define COMP_BOUNDS_OFF    0x20          /* {x,y,w,h} int32 */
#define KBD_STOCK_ROW      2             /* the row the stock editor is positioned for */
#define MOD_ROW_H          50            /* list row pitch, in pixels */

/* ---- our own juce::TextEditor ----
 * A text row must NOT type into the view's editor. That editor has the SYSTEM pane
 * registered on it as a juce::TextEditor::Listener, and textEditorTextChanged fires per
 * keystroke with the editor by reference (the shape is plain in the sibling
 * gui::SearchTitleWidget::textEditorTextChanged, which getText()s its argument) -- so
 * every character typed lands in the stock HISTORY NAME setting and persists there.
 * The commit is unconditional; there is no mode flag that suppresses it.
 *
 * So build one of our own and swap it into +0x778 while the keyboard is up. Nothing is
 * listening to ours, which makes the commit impossible rather than merely undone, and
 * because show/hide/focus all go through +0x778 the stock keyboard path drives it as-is.
 *
 * sub_1c00480 is the plain juce::TextEditor(const String&, juce_wchar) -- the vtable it
 * installs, 0x2516e90, is the one the view's own editor carries, so this is the same kind
 * of object and not a skinned subclass. Its size is pinned by gui's styled subclass
 * (sub_1468740), which stores its first member at +0x2e8. */
#define FN_EDITOR_CTOR     ep122_sym(EP122_JUCE_EDITOR_CTOR)  /* juce::TextEditor::TextEditor(const String&, wchar) */
#define FN_EDITOR_SETFONT  ep122_sym(EP122_JUCE_EDITOR_SETFONT)  /* juce::TextEditor::setFont(const Font&)             */
#define FN_EDITOR_JUSTIFY  ep122_sym(EP122_JUCE_EDITOR_JUSTIFY)  /* juce::TextEditor::setJustification(const Justification&) */
/* juce::Justification: right|top -- what the view gives its own editor, and what puts the
 * text in the value column instead of over the row label. */
#define ED_JUSTIFY_RIGHT   0xa
#define FN_EDITOR_FOCUS    ep122_sym(EP122_JUCE_COMP_GRABFOCUS)  /* juce::Component::grabKeyboardFocus()               */
#define EDITOR_ALLOC_SIZE  0x2e8
#define EDITOR_FONT_H      32.0f        /* what the view gives its own editor (sub_144c564) */
/* juce::TextEditor::ColourIds, in the order sub_1468740 sets them. 0x1000204 is the
 * caret's own id rather than the editor's; stock sets it on the editor all the same. */
#define ED_COL_BG          0x1000200
#define ED_COL_TEXT        0x1000201
#define ED_COL_HIGHLIGHT   0x1000202
#define ED_COL_CARET       0x1000204
#define ED_COL_OUTLINE     0x1000205
#define ED_COL_FOCUSED     0x1000206
#define ED_COL_SHADOW      0x1000207
/* The two skin colours those ids are filled from -- juce::Colour is a bare ARGB word and
 * sub_1ac62c0 is a 4-byte copy out of these globals, so reading them directly keeps our
 * editor on whatever theme is loaded instead of freezing a palette here. */
#define ADDR_SKIN_FG       ep122_sym(EP122_SKIN_FG)  /* text + caret       */
#define ADDR_SKIN_BG       ep122_sym(EP122_SKIN_BG)  /* fill, outline, ... */

/* ---- the keyboard's one listener callback ----
 * The keyboard sends a KEY CODE, not text, and the view does the whole edit itself:
 *
 *     String s = getText(*(view+0x778));         // 0x1bf3f20
 *     if      (key == 0x4f) s = s.dropLastChar();
 *     else if (key == 0x51) s = "";
 *     else if (s.length() <= 0x1f) s += keyToText(key);
 *     setText(*(view+0x778), s, true);
 *     grabKeyboardFocus(*(view+0x778));
 *     if (link(view+0x718)) <post async task carrying s>   // the HISTORY NAME commit
 *
 * That last line is why a text row of our own could not simply borrow the editor: the
 * commit reads whatever component is at +0x778, which is the same pointer the edit is
 * applied to, so our text reached HISTORY NAME no matter which editor was installed or
 * when the pointer was put back (both measured).
 *
 * Taking the callback instead settles it. The view registers `view+0x340` as the
 * keyboard's only listener (UtilityView ctor, appended into the array at kbd+0x168), and
 * that sub-object's vtable slot 0 is the single interface method. Own that slot and our
 * row does its own editing, against its own buffer, and the stock body -- commit and all
 * -- never runs. Nothing of the view's is written, so the editor at +0x778 keeps holding
 * the real HISTORY NAME throughout. */
#define KBD_LISTENER_SLOT  (ep122_sym(EP122_UTILITY_AS_KBD_LISTENER) + 0x0)  /* UtilityView's IListener vtable, slot 0     */
#define FN_KBD_KEY         ep122_sym(EP122_VIEW_KBD_KEY)  /* `sub x0,x0,#0x340; b 0x144b0b0` -- the thunk */
#define FN_KEY_TO_TEXT     ep122_sym(EP122_KBD_KEY_TO_TEXT)  /* juce::String keyToText(int key) -> x8       */
#define KBD_KEY_BACKSPACE  0x4f
#define KBD_KEY_CLEAR      0x51

/* font_ret_t / str_ret_t and the x8 return convention: ../juce.h */
typedef str_ret_t (*key_to_text_t)(int key);
typedef void      (*kbdkey_t)(void *listener, long key);
typedef void    (*draw_text_t)(void *g, void *str, int x, int y, int w, int h, int *justif, int ellipsis);
typedef int32_t (*numrows_t)(void *self);
typedef void    (*paintcell_t)(void *self, void *g, int row, int col, int w, int h, int sel);
typedef int64_t (*switch_t)(void *view, long a2, long delta);
typedef void    (*mousedown_t)(void *self, void *event);
typedef void    (*selchanged_t)(void *self, int row);
typedef void    (*rowchanged_t)(void *listener, int row);
typedef void    (*selectrow_t)(void *list, int row, int dontScroll, int deselectOthers);
typedef void    (*cellclick_t)(void *self, int row, int col, void *event);
typedef void *  (*rrefresh_t)(void *self, int row, int col, int isSelected, void *existing);

/* ---- shared state (menu/state.c) ---------------------------------------- */

extern uintptr_t menu_g_orig_numrows, menu_g_orig_paintcell, menu_g_orig_mousedown, menu_g_orig_switch,
                 menu_g_orig_touchsel, menu_g_orig_selchanged, menu_g_orig_rowchanged, menu_g_orig_rcellclick,
                 menu_g_orig_input, menu_g_orig_rselchanged, menu_g_orig_rrefresh,
                 menu_g_orig_rnumrows;
extern int       menu_g_render_ok;   /* JUCE render primitives verified at install */
extern int       menu_g_mod_mode;    /* 1 while the mod overlay is drawing over DJ SETTING */
extern int       menu_g_list_rows;   /* rows the DJ SETTING list is currently sized for */
extern uintptr_t menu_g_model;       /* DJSettingTableModel, captured in paintCell */
extern uintptr_t menu_g_view;        /* UTILITY view, captured from the hooks that get it */
extern int       menu_g_bouncing;    /* re-entry guard for the title-row bounce */
extern int       menu_g_rsel_guard;  /* set while WE drive the right pane, so its selection
                                 * callback does not adopt a value we just wrote */
extern int       menu_g_in_input;    /* set while the stock focus/rotary dispatch is running,
                                 * which is how a rotary tick is told from a touch tap */
extern int       menu_g_strarr_ok;   /* juce::StringArray primitives verified at install */
extern int       menu_g_setstr_ok;   /* right-model setStrings verified at install */
extern int       menu_g_kbd_ok;      /* software-keyboard show/hide verified at install */
extern int       menu_g_editor_ok;   /* our own TextEditor can be built */
extern uintptr_t menu_g_orig_kbdkey; /* the view's own keyboard IListener callback */

/* ---- the rows (menu/rows.c) ---------------------------------------------- */

/* Row 0 is a "MOD SETTINGS" title -- the DJ SETTING sidebar tab is a baked PNG
 * and cannot be re-lettered -- then one row per toggle. */
#define MOD_ROW_TITLE  0
#define MOD_ROW_FIRST  1                     /* first selectable row (title is a label) */

/* Rows reported to JUCE while armed, and the height the list is given to show
 * them without a scrollbar. Stock sizes the DJ SETTING list for eight rows and
 * leaves the panel below it blank; the overlay grows the list into that blank
 * (menu_list_fit) and reports this many, the title taking one of them. Ten is
 * what the panel has room for: its row area ends about 620 px down, and the
 * list starts at 92, so a tenth 50 px row ends at 592. Only the live rows carry
 * content; the rest get the stock row background + divider. */
#define MOD_ROWS_VISIBLE 10
_Static_assert(MOD_ROWS_VISIBLE - MOD_ROW_FIRST == KIT_MENU_MAX_ROWS,
               "the kit's row capacity must be what this list can show");

/* Rows the stock DJ SETTING list is sized for: the unit menu_list_fit measures
 * the row height from, and what is reported while the list is stock-sized. */
#define MOD_LIST_ROWS_STOCK 8

/* Where the panel's row area ends. Below it is the waveform strip, and a list
 * that ran into that would be worse than one that scrolls. */
#define MOD_LIST_BOTTOM 620

/* More than a scrollbar's thickness: what the list is grown past its target by,
 * for one call, so JUCE drops both scrollbars -- see menu_list_fit. */
#define MOD_LIST_GROW_SLACK 24

/* The software keyboard's top edge: it rises from the bottom of the screen to
 * the divider under the seventh row, so a text row below that is edited out of
 * sight unless the list is scrolled -- see menu_list_fit_above_kbd. */
#define MOD_KBD_TOP 444

/* Bounds the stack array that builds the value list. */
#ifndef MOD_ROW_VALUES_MAX
#define MOD_ROW_VALUES_MAX 8
#endif

/* The row the cursor is on: the setting the right pane edits. */
extern int menu_g_setting_row;

const struct kit_row *menu_row(int row);
int                   menu_row_last(void);
const char           *menu_row_label(int row);
int                   menu_row_nvalues(const struct kit_row *r);
const char           *menu_row_value(const struct kit_row *r, int idx);
int                   menu_row_index(const struct kit_row *r);
int                   menu_pane_rows(const struct kit_row *r);
int                   menu_pane_index(const struct kit_row *r);
int                   menu_active_on(void);

/* ---- drawing (menu/paint.c) ---------------------------------------------- */

void     menu_draw_field(void *self, void *g, uintptr_t fonth_off, uint32_t colour,
                    const char *text, int x, int w, int h, int justif);
void     menu_draw_mod_row(void *self, void *g, int w, int h, const char *label,
                           const char *value, int sel);
void     menu_draw_mod_header(void *self, void *g, int w, int h, const char *text);

/* ---- text rows: the software keyboard (menu/editor.c) -------------------- */

void   menu_kbd_key(void *self, long key);
void   menu_kbd_open(const struct kit_row *r);
void   menu_kbd_close(void);
int    menu_kbd_is_up(void);
const struct kit_row *menu_kbd_row(void);
size_t menu_str_copy(uintptr_t sp, char *out, size_t cap);

/* ---- view + category helpers (menu/view.c) ------------------------------- */

void      menu_refresh_djlist(void *view);
void      menu_style_ver(void *view);
void      menu_note_view(void *view);
uintptr_t menu_view_ptr(uintptr_t view, uintptr_t off);
void      menu_toggle_overlay(void *view);
void      menu_pane_show(uintptr_t view, int visible);

/* ---- the right pane (menu/pane.c) ---------------------------------------- */

void    menu_apply_value(int row, int focus_pane, const char *src);
void    menu_pane_fit(uintptr_t rlist, int rows);
void    menu_list_fit(uintptr_t list, int armed);
int     menu_list_fit_above_kbd(uintptr_t list, int row);
void    menu_pane_labels(uintptr_t view, uintptr_t rmodel, const struct kit_row *r);
void    menu_force_right_pane_sel(uintptr_t view, int sel_row);
void    menu_force_right_pane(uintptr_t view);
void    menu_rcellclicked(void *self, int row, int col, void *event);
int32_t menu_rnumrows(void *self);
void   *menu_rrefresh(void *self, int row, int col, int isSelected, void *existing);
void    menu_rselchanged(void *self, int row);
void    menu_rowchanged(void *self, int row);
int32_t menu_dot_field(void);


#ifdef __cplusplus
}
#endif

#endif /* EP122_MOD_MENU_INTERNAL_H */
