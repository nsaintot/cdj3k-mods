// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * menu/internal.h - private to menu/.
 *
 * The overlay spans one list model, one view and one right-pane model, so its
 * files share state: the cursor row, whether the overlay is armed, and the stock
 * functions the hooks chain to. That state lives in menu/state.c.
 *
 * Nothing here knows about specific features. This directory renders and edits
 * whatever rows the features registered (../kit/menu.h).
 *
 * The EP122 offsets and primitives below are the overlay's whole ABI surface.
 * Nothing here is visible outside the directory: the mod descriptor in hooks.c
 * (../kit/mod.h) is the overlay's only entry point.
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


/* ---- DJSettingTableModel row hooks ---- */
#define MOD_VT_NUMROWS_SLOT   (ep122_sym(EP122_DJSET_MODEL) + 0x10)  /* vtable[0x10] getNumRows */
#define MOD_VT_PAINTCELL_SLOT (ep122_sym(EP122_DJSET_MODEL) + 0x20)  /* vtable[0x20] paintCell  */
#define MOD_FN_NUMROWS        ep122_sym(EP122_DJSET_NUMROWS)
#define MOD_FN_PAINTCELL      ep122_sym(EP122_DJSET_PAINTCELL)

/* ---- UTILITY view ---- */
#define VIEW_FOCUS_OFF        0x388        /* focus level (0 category / 1 list / 2 option) */
#define VIEW_CAT_OFF          0x38c        /* current category index                       */
#define FN_SWITCH             ep122_sym(EP122_VIEW_SWITCH)  /* rotary category nav                         */
#define MOD_SWITCH_SLOT       (ep122_sym(EP122_UTILITY_VIEW) + 0x198)  /* view vtable+0x198 -> FN_SWITCH (unlatch hook) */
/* Touch category-select handler (view vtable+0x178). Framework-invoked when a
 * sidebar item is tapped; applies the picked category and writes view+0x38c via
 * the per-category setup fns (DJ SETTING's ends with *(view+0x38c)=0).
 * Touch analogue of the rotary nav. Do not hook the focus-level input dispatch
 * (vtable+0x190) for this: it early-returns on touch (arg3==0). */
#define FN_TOUCHSEL           ep122_sym(EP122_VIEW_TOUCHSEL)  /* touch category-select handler             */
#define MOD_TOUCHSEL_SLOT     (ep122_sym(EP122_UTILITY_VIEW) + 0x178)  /* view vtable+0x178 -> FN_TOUCHSEL (touch unlatch hook) */

/* juce ListBox::updateContent: reads model @ list+0xd8, calls its
 * getNumRows (vtable+0x10 -> our hook), rebuilds the row layout + viewport. We
 * call it on the DJ SETTING list to make it re-query the row count after the
 * mode flips (a plain repaint keeps the cached 8 rows). */
#define FN_UPDATECONTENT      ep122_sym(EP122_JUCE_LISTBOX_UPDATE)
#define VIEW_DJLIST_OFF       0x3f8        /* *(view+0x3f8) = cat-0 (DJ SETTING) left list */
#define VIEW_MODEL_REF_OFF    0x390        /* model ref the cat-0 list-nav clamps against    */
#define MODEL_GETNUMROWS_SLOT 2            /* model vtable +0x10: ListBoxModel::getNumRows */
#define LIST_MODEL_SCAN       0x200        /* bytes of the list to search for our model ptr */

/* ---- UTILITY view mouseDown (vtable+0x28) : the "Ver." touch entry ----
 * The stock view mouseDown only dismisses a popup and ignores the tap
 * coordinates, so a tap on the non-interactive "Ver.X.XX" label falls through
 * to it. We hook the slot and read the MouseEvent ourselves. */
#define MOD_VT_MOUSEDOWN_SLOT (ep122_sym(EP122_UTILITY_VIEW) + 0x28)  /* view vtable+0x28 (mouseDown)         */
#define MOD_FN_MOUSEDOWN      ep122_sym(EP122_VIEW_MOUSEDOWN)   /* mouseDown thunk                      */
#define MEVENT_POS_OFF        0x00          /* MouseEvent: position x,y floats at +0x00/+0x04 */
/* "Ver.X.XX" hit box in screen pixels, top-right corner. Coords are not mirrored
 * at the JUCE layer: Ver taps land at x 1127..1236, y 13..22. */
#define VER_HIT_X_MIN         1080
#define VER_HIT_Y_MAX         45

/* Font-height fields on the model + theme colours (see stock paintCell). */
#define MODEL_LBL_FONTH_OFF   0x18         /* label font height (float) -- larger, white */
#define MODEL_VAL_FONTH_OFF   0x14         /* value font height (float) -- smaller, grey */
/* The overlay title and the armed Ver label use the accent role, so they follow the
 * active theme (theme/roles.c) like the stem row does. */
#define MOD_HEADER_COLOUR     (mod_ui()->accent)

/* ---- "MOD SETTINGS" title row must not be selectable ----
 * JUCE has no per-row selectable flag, so selectedRowsChanged (model vtable+0x60)
 * bounces row 0 to row 1 via the stock list-nav, which computes the row rect. */
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
 * view+0x180; its vtable slot 0 adjusts to the view and calls the right-pane rebuild
 * (view, row), which stores the option-set id at rightmodel+0x128, pushes that set's
 * strings into the model, then updateContent()s the right list and selects the
 * current value's index.
 * Row ids 0..7 map to the stock option arrays at view+0x4b8 + i*0x18; anything else
 * takes the default branch -> id 8 with an empty string list (blank pane).
 * The overlay re-points the rebuild: the title row asks for the empty set, and GATE
 * CUE borrows set 4 (the OFF/ON array at view+0x518) for natively styled OFF/ON rows,
 * then the id is stamped back to 8 so a tap cannot write a real DJ setting, and the
 * row matching the gate state is re-selected. */
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
 * touch tap on a value: this model overrides refreshComponentForCell, so
 * every value row is a radio Component that consumes the tap. Both touch and rotary
 * change the selection, so the value is adopted here. */
#define MOD_VT_RSELCHANGED_SLOT (ep122_sym(EP122_DJSET_RMODEL) + 0x60) /* right model vtable+0x60 (selectedRowsChanged) */
#define MOD_FN_RSELCHANGED      ep122_sym(EP122_DJSET_RSELCHANGED)

/* Right-pane refreshComponentForCell (its model's vtable+0x28). It picks the dot colour by
 * comparing the row against model+0x10, so that field, not the selection, marks the
 * current value. Writing it once on change is not enough: the stock value-chosen path
 * restamps it asynchronously from the disowned option set, showing the previous value.
 * This hook runs per row on every draw, so the dot always follows our state. */
#define MOD_VT_RREFRESH_SLOT    (ep122_sym(EP122_DJSET_RMODEL) + 0x28) /* right model vtable+0x28 (refreshComponentForCell) */
#define MOD_FN_RREFRESH         ep122_sym(EP122_DJSET_RREFRESH)
#define FN_REPAINT             ep122_sym(EP122_JUCE_COMP_REPAINT)  /* juce::Component::repaint() */
/* "enter DJ SETTING (category 0)": shows the cat-0 components, hides the others and
 * ends with *(view+0x38c)=0. Used to bring the overlay's host list to the front when
 * the Ver label is tapped from another category. */
#define FN_ENTER_DJSETTING     ep122_sym(EP122_ENTER_DJSETTING)

/* Focus-level input dispatch (view vtable+0x190). Level 0 -> category, level 1 ->
 * the list handler, level 2 -> the option-pane handler. Level 1 tail-calls the
 * right-pane rebuild directly, bypassing the listener hook, so entering the right
 * pane from the list rebuilds it with stock options for that row index. Our pane is
 * re-asserted after it. */
#define MOD_INPUT_SLOT         (ep122_sym(EP122_UTILITY_VIEW) + 0x190)  /* view vtable+0x190 -> FN_INPUT */
#define FN_INPUT               ep122_sym(EP122_VIEW_INPUT)
#define FN_CURRENT_ROW         ep122_sym(EP122_JUCE_LISTBOX_CURRENTROW)  /* juce::ListBox current/selected row */
#define FOCUS_SETTING_LIST     1            /* view+0x388 focus level: centre list */
#define FOCUS_OPTION_PANE      2            /* view+0x388 focus level: right pane */
/* The right model's checked value index, which draws the filled radio. It is not the
 * list selection: refreshComponentForCell styles the row whose index equals model+0x10
 * as the current value (entering DJ SETTING seeds it from the list's current row). */
/* Stock also uses it to index the option array behind the pane, so a value past
 * that array's end is an out-of-bounds read on the message thread. A persisted
 * theme index (0..4, e.g. 3) written here crashes the deck on the Ver tap that
 * opens the overlay. */
#define RMODEL_CHECKED_OFF     0x10

/* The borrowed option set's own length. The pane can be longer (menu_rnumrows),
 * but only after setStrings has put a longer array behind it. */
#define MOD_PANE_ROWS_STOCK    2

/* Bounds the stack array that builds the value list. */
#define MOD_ROW_VALUES_MAX     8

/* juce::Component bounds: x, y, w, h as four int32. */
#define COMP_BOUNDS_OFF        0x20

/* Limit on how far the value list may grow (not a layout constant): the panel
 * is 720 tall, and past that the list scrolls instead. */
#define MOD_SCREEN_H           720

#define LABEL_TEXT_COLOUR_ID   0x1000281    /* juce::Label::textColourId */
#define VIEW_VERLBL_OFF        0x4b0        /* *(view+0x4b0) = the Ver.X.XX label */
#define VER_COLOUR_IDLE        0xffe8e8e8u  /* brighter than skin #afafaf: reads as tappable */
#define VER_COLOUR_ARMED       (mod_ui()->accent)   /* accent while the MOD overlay is armed */

/* ---- "-m": the mods are installed ----
 * Installation is all-or-nothing (common.c returns before installing anything
 * if a single symbol is missing), so the "-m" suffix means every hook is live.
 * It is the place to check when a deck behaves stock.
 *
 * Only the label's own copy of the string changes. UtilityView's ctor builds
 * "Ver." + version once and hands copies to its other consumers before building
 * this label, so the version reported over ProLink is untouched. */
#define FN_LABEL_SETTEXT       ep122_sym(EP122_JUCE_LABEL_SETTEXT)  /* juce::Label::setText(const String&, NotificationType) */
#define LABEL_LASTTEXT_OFF     0x170        /* juce::Label::lastTextValue (a juce::String) */
#define VER_MOD_TAG            "-m"
#define NOTIFY_NONE            0            /* juce::dontSendNotification */

/* JUCE render primitives. */
#define FN_G_SETFONT   ep122_sym(EP122_JUCE_GFX_SETFONT)  /* juce::Graphics::setFont(Graphics*, Font*)        */
#define FN_G_SETCOL    ep122_sym(EP122_JUCE_GFX_SETCOLOUR)  /* juce::Graphics::setColour(Graphics*, Colour*)    */
#define FN_DRAW_TEXT   ep122_sym(EP122_JUCE_GFX_DRAWTEXT)  /* drawText(g,String*,x,y,w,h,Justification*,ellip) */

/* No prologue guards: every primitive below comes from a masked signature match
 * or a vtable found by the RTTI walk, which validates the target better. */

/* Right-pane value strings. The stock rebuild fills a local StringArray
 * from the option table, passes it to this, then updateContent()s the list. It swaps
 * the arrays, so afterwards our array holds the model's previous strings and
 * destroying it frees them, as stock does. This lets a borrowed OFF/ON set show other
 * strings such as AUTO/MANUAL. */
#define FN_RMODEL_SETSTRINGS ep122_sym(EP122_RMODEL_SETSTRINGS)  /* setStrings(rightModel, juce::StringArray&) */

/* ---- software keyboard (gui::SoftwareKeyboardPopupWidget) ----
 * The view builds one in its constructor and keeps it; UTILITY > SYSTEM > HISTORY NAME
 * is its stock user, and that row's right-pane rebuild (row 2) calls the
 * show below. A text row needs no widget of its own, only these two calls.
 *
 * `show` seeds the editor at +0x778 from the view's juce::String at +0x710, makes the
 * editor and the keyboard visible, and grabs focus; `hide` is its counterpart, already
 * invoked by the stock category-change paths (touch and rotary, both hooked).
 * Both reach the editor through +0x778, so a text row can put its own editor there and
 * reuse the whole path. */
#define FN_KBD_SHOW        ep122_sym(EP122_KBD_SHOW)   /* show(view): seed editor, show keyboard, focus */
#define FN_KBD_HIDE        ep122_sym(EP122_KBD_HIDE)   /* hide(view)                                    */
#define VIEW_EDITOR_OFF    0x778         /* the juce::TextEditor showing the value        */
#define FN_EDITOR_SETTEXT  ep122_sym(EP122_JUCE_EDITOR_SETTEXT)   /* juce::TextEditor::setText(const String&, bool) */
/* The editor is positioned for the stock text setting (SYSTEM's HISTORY NAME, row 2),
 * so it is moved onto the row being edited. setTopLeftPosition keeps the width/height
 * it reads from +0x28/+0x2c, which places the bounds at +0x20 as {x,y,w,h} int32s. */
#define FN_SET_TOPLEFT     ep122_sym(EP122_JUCE_COMP_SETTOPLEFT)   /* juce::Component::setTopLeftPosition(x, y) */
#define COMP_BOUNDS_OFF    0x20          /* {x,y,w,h} int32 */
#define KBD_STOCK_ROW      2             /* the row the stock editor is positioned for */
#define MOD_ROW_H          50            /* list row pitch, in pixels */

/* ---- our own juce::TextEditor ----
 * A text row must not type into the view's editor. The SYSTEM pane is registered on it
 * as a juce::TextEditor::Listener, and textEditorTextChanged fires per keystroke with
 * the editor by reference (compare gui::SearchTitleWidget::textEditorTextChanged, which
 * getText()s its argument), so every character would be saved to the stock HISTORY NAME
 * setting. The commit is unconditional.
 *
 * So a private editor is swapped into +0x778 while the keyboard is up. Nothing listens
 * to it, and since show/hide/focus all go through +0x778 the stock keyboard path drives
 * it unchanged.
 *
 * The constructor is the plain juce::TextEditor(const String&, juce_wchar); the vtable
 * it installs is the one the view's own editor carries, so it is the same class and not
 * a skinned subclass. Its size is 0x2e8: gui's styled subclass stores its first member
 * there. */
#define FN_EDITOR_CTOR     ep122_sym(EP122_JUCE_EDITOR_CTOR)  /* juce::TextEditor::TextEditor(const String&, wchar) */
#define FN_EDITOR_SETFONT  ep122_sym(EP122_JUCE_EDITOR_SETFONT)  /* juce::TextEditor::setFont(const Font&)             */
#define FN_EDITOR_JUSTIFY  ep122_sym(EP122_JUCE_EDITOR_JUSTIFY)  /* juce::TextEditor::setJustification(const Justification&) */
/* juce::Justification: right|top, as the view's own editor uses; puts the text in the
 * value column instead of over the row label. */
#define ED_JUSTIFY_RIGHT   0xa
#define FN_EDITOR_FOCUS    ep122_sym(EP122_JUCE_COMP_GRABFOCUS)  /* juce::Component::grabKeyboardFocus()               */
#define EDITOR_ALLOC_SIZE  0x2e8
#define EDITOR_FONT_H      32.0f        /* what the view gives its own editor */
/* juce::TextEditor::ColourIds, in the order gui's styled editor sets them. 0x1000204 is the
 * caret's id; stock sets it on the editor anyway. */
#define ED_COL_BG          0x1000200
#define ED_COL_TEXT        0x1000201
#define ED_COL_HIGHLIGHT   0x1000202
#define ED_COL_CARET       0x1000204
#define ED_COL_OUTLINE     0x1000205
#define ED_COL_FOCUSED     0x1000206
#define ED_COL_SHADOW      0x1000207
/* The two skin colours those ids are filled from. juce::Colour is a bare ARGB word and
 * the skin's colour getter is a 4-byte copy out of these globals, so reading them directly keeps the
 * editor on the loaded theme. */
#define ADDR_SKIN_FG       ep122_sym(EP122_SKIN_FG)  /* text + caret       */
#define ADDR_SKIN_BG       ep122_sym(EP122_SKIN_BG)  /* fill, outline, ... */

/* ---- the keyboard's one listener callback ----
 * The keyboard sends a key code, not text, and the view does the edit itself:
 *
 *     String s = getText(*(view+0x778));
 *     if      (key == 0x4f) s = s.dropLastChar();
 *     else if (key == 0x51) s = "";
 *     else if (s.length() <= 0x1f) s += keyToText(key);
 *     setText(*(view+0x778), s, true);
 *     grabKeyboardFocus(*(view+0x778));
 *     if (link(view+0x718)) <post async task carrying s>   // the HISTORY NAME commit
 *
 * Because of that last line, swapping the editor is not enough: the commit reads
 * whatever is at +0x778, the same pointer the edit applies to, so text reaches HISTORY
 * NAME whichever editor is installed or when the pointer is restored.
 *
 * So the callback is hooked instead. The view registers `view+0x340` as the keyboard's
 * only listener (UtilityView ctor, appended into the array at kbd+0x168), and that
 * sub-object's vtable slot 0 is the single interface method. With that slot hooked, our
 * row edits its own buffer and the stock body, including the commit, never runs. The
 * editor at +0x778 keeps the real HISTORY NAME throughout. */
#define KBD_LISTENER_SLOT  (ep122_sym(EP122_UTILITY_AS_KBD_LISTENER) + 0x0)  /* UtilityView's IListener vtable, slot 0     */
#define FN_KBD_KEY         ep122_sym(EP122_VIEW_KBD_KEY)  /* the thunk that adjusts from view+0x340        */
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
extern int       menu_g_rsel_guard;  /* set while we drive the right pane, so its selection
                                 * callback does not adopt a value we just wrote */
extern int       menu_g_in_input;    /* set while the stock focus/rotary dispatch is running,
                                 * which is how a rotary tick is told from a touch tap */
extern int       menu_g_strarr_ok;   /* juce::StringArray primitives verified at install */
extern int       menu_g_setstr_ok;   /* right-model setStrings verified at install */
extern int       menu_g_kbd_ok;      /* software-keyboard show/hide verified at install */
extern int       menu_g_editor_ok;   /* our own TextEditor can be built */
extern uintptr_t menu_g_orig_kbdkey; /* the view's own keyboard IListener callback */

/* ---- the rows (menu/rows.c) ---------------------------------------------- */

/* Row 0 is a "MOD SETTINGS" title (the DJ SETTING sidebar tab is a baked PNG and
 * cannot be relabelled), then one row per setting. */
#define MOD_ROW_TITLE  0
#define MOD_ROW_FIRST  1                     /* first selectable row (title is a label) */

/* Rows reported to JUCE while armed, sized to show without a scrollbar. Stock
 * sizes the DJ SETTING list for eight rows and leaves the panel below blank; the
 * overlay grows the list into that space (menu_list_fit) and reports this many,
 * including the title. The row area ends about 620 px down and the list starts at
 * 92, so a tenth 50 px row ends at 592. Rows without content get the stock row
 * background + divider. */
#define MOD_ROWS_VISIBLE 10
_Static_assert(MOD_ROWS_VISIBLE - MOD_ROW_FIRST == KIT_MENU_MAX_ROWS,
               "the kit's row capacity must be what this list can show");

/* Rows the stock DJ SETTING list is sized for: the unit menu_list_fit measures
 * the row height from, and what is reported while the list is stock-sized. */
#define MOD_LIST_ROWS_STOCK 8

/* Where the panel's row area ends; the waveform strip is below it. */
#define MOD_LIST_BOTTOM 620

/* More than a scrollbar's thickness: how far past its target the list is grown
 * for one call so JUCE drops both scrollbars (menu_list_fit). */
#define MOD_LIST_GROW_SLACK 24

/* The software keyboard's top edge: the divider under the seventh row. A text row
 * below that is hidden unless the list is scrolled (menu_list_fit_above_kbd). */
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
int     menu_list_row_h(void);          /* measured, MOD_ROW_H until it is */
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
