// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * browse.h - the contract between the parts of the browse-list reorder UI.
 *
 * The feature: drag a track to a new position in a PLAYLIST and have the stick
 * keep it. mod_djdb_move_track in db/db.h does the write
 * (DJDBSONGPLAYLIST.TRACKNO through the deck's own update, on a library server
 * thread). This directory only decides which move to request.
 *
 * ---- the EDIT mode ---------------------------------------------------------
 *
 * The screen takes one touch point and a one-finger drag already scrolls the
 * list, so reordering needs an explicit mode. As on the CDJ-3000X, an EDIT
 * toggle in the browse header switches it: with EDIT on a drag moves a track,
 * with it off the list scrolls as stock.
 *
 * Row long-press is not used. The stock app does not use it
 * (meow::SkinLoadableButtonLongPress feeds
 * gui::BrowseTitleWidget::onButtonLongPress, a button concept), but on the 3X
 * it copies the track name for the keyboard search.
 *
 * Playlists only. A playlist's `#` is DJDBSONGPLAYLIST.TRACKNO, a position
 * stored with the list. An artist's or album's `#` is DJDBCONTENT.TRACKNO, the
 * track's album track number from its tags; dragging there would rewrite
 * metadata.
 *
 * ---- detecting a playlist --------------------------------------------------
 *
 * From the list cache it is served from: each cached list keeps the condition
 * it was requested with, a playlist's carries a playlist hierarchy, and the list
 * on screen comes from the newest cache held by something other than the
 * collector. mod_djdb_playlist_shown reads that cache and is re-asked whenever
 * the list on screen changes; the write uses the same scan through
 * mod_djdb_playlist_now.
 *
 * Under gui::PlayListView (the PLAYLIST button's screen) every track list is a
 * playlist.
 *
 * No other source works. The browse sidebar's categories come from the stick
 * (djdbMenuItems), so neither its row count nor PLAYLIST's index is constant.
 * gui::TrackListWidget serves playlists and albums from one object, and
 * gui::BrowseTitleWidget holds only a title and an icon bitmap.
 *
 * The `#` column is an additional, independent condition: it keeps EDIT off
 * the split preview pane, where a playlist is selected but its tracks have no
 * position column.
 *
 * ---- where it attaches -----------------------------------------------------
 *
 * gui::BrowseTitleWidget is the 1280x90 header. It inherits juce::Component
 * virtually through gui::WidgetBase, so its Component slots are in a secondary
 * vtable of its group, EP122_BROWSE_TITLE, which the spec resolves by base
 * class. Its paint is the anchor: `self` is the live, parented header each time
 * the bar draws.
 *
 * Fail open everywhere: an unscrollable browse list is worse than no reorder.
 */
#ifndef EP122_MODS_BROWSE_H
#define EP122_MODS_BROWSE_H

#include "juce/juce.h"
#include "juce/draw.h"
#include "kit/mod.h"
#include "db/db.h"

#ifdef __cplusplus
extern "C" {
#endif


/* The header's right-hand plates are not its children: they are the three
 * TogglesImageButtons inside gui::BrowseDispSwitchButtonsWidget, a group at
 * {894,0,386,90}, 114 wide on a 124 stride. Ours takes the free slot left of
 * that group and is a child of the header, since juce would clip a child at
 * x=-124 of the group.
 *
 * The stride is read from the group's buttons at run time; BE_GAP is the
 * fallback when the group holds one button, and BE_MIN_X keeps the plate on
 * the bar. */
/* How many gui::BrowseTitleWidgets can carry a plate. There are two (the browse
 * screen's and the playlist screen's), built once and kept. */
#define BE_MAX_BARS    4

#define BE_GAP        10
#define BE_MIN_X       0

/* The lettering matches the baked PREVIEW beside it: 15px caps. (The waveform
 * title bar's quick menu uses 24, which is too large here.)
 *
 * The word is drawn by our own paint, not the Label: the Label can only centre
 * in its bounds, which are the whole plate, and the plate has a word at the top
 * and a mark under it. */
#define BE_FONT       20.0f
#define BE_TEXT       "EDIT"
#define BE_LABEL_CY   22            /* the caps' own centre, not the font box's */

/* ---- the reorder mark -----------------------------------------------------
 *
 * Two arrows beside three bars: the standard reorder icon, marking the button
 * as reordering rather than editing a track.
 *
 * Whole pixels at one size, no scaling. The app's LowLevelGraphics vtable has
 * no antialiased primitive, so arrowheads are drawn row by row and
 * sizes are chosen to divide evenly.
 *
 * The head is solid, not an open chevron: at 11px across with a 3px stroke the
 * arms leave a one-pixel gap that reads as a stipple.
 *
 * The mark is symmetric about the middle bar: the arrows' centres and the
 * short bars sit the same distance either side of row 18.
 */
#define BE_GLYPH_W    44
#define BE_GLYPH_H    37
#define BE_GLYPH_Y    40            /* under the word, clear of the bottom lip */
#define BE_GLYPH_T     3            /* one stroke */
#define BE_ARROW_X     7            /* the shaft's left edge */
#define BE_ARROW_Y    26            /* the DOWN arrow's top; the up one is at 0 */
#define BE_ARROW_H    11
#define BE_HEAD_ROWS   5

/* Whether EDIT is on. Read by the gesture half. [message] */
int browse_edit_on(void);

/* The gui::TrackListWidget EDIT was turned on over, or 0 when EDIT is off.
 * This is the gesture's gate: meow::TouchableTableListBox::RowComp is the row
 * of every touchable table in the app, so checking only that EDIT is on would
 * make the browse sidebar and DJ SETTINGS draggable too (a tap on the
 * sidebar's TRACK tab became a drag). [message] */
uintptr_t browse_edit_list(void);

/* ---- sort.c: the sort EDIT borrows and gives back ------------------------
 *
 * A reorder only makes sense against the list's stored order, so EDIT forces
 * `#` ascending and restores the DJ's sort on exit, as the CDJ-3000X does.
 *
 * Both go through juce::TableHeaderComponent (gui::TrackListHeader overrides
 * only mouseDown), so setting the sort is the same call the deck makes when a
 * column is tapped, including the async re-sort.
 *
 * While EDIT holds the sort, every column's `sortable` flag is cleared. That
 * one bit makes juce's columnClicked refuse to re-sort and stops the deck's
 * LookAndFeel drawing the column's arrow.
 *
 * `bar` is the browse header, where the walk to the list starts. Returns 0 when
 * the sort was taken. [message] */
int  browse_sort_take(uintptr_t bar);

/* Whether the list on screen has a `#` column. Gates the plate, so EDIT is not
 * offered over an artist's tracks or the all-tracks view. It does not tell a
 * playlist from an album (both show `#`); see the head of this file for that
 * check. */
int  browse_sort_has_position(uintptr_t bar);

/* Make the deck fetch the list again after a reorder request. It sorts away
 * and back, because a sort-kind change is a library message and a direction
 * flip is not (see sort.c). That message also runs the queued write.
 * [message] */
void browse_sort_refetch(void);

void browse_sort_give_back(int resort);

/* Re-assert the disabled header. The deck sets the columns' `sortable` bit
 * again on its own (the arrows returned mid-mode with no notify), so this is
 * polled. A few reads; it writes only what changed.
 *
 * Also ends the mode: returns -1 once the borrowed `#` is no longer a visible
 * column, since without it there is no position to move a row to. Watching for
 * a gui::TrackListWidget change instead missed the all-tracks view. [message] */
int browse_sort_hold(void);

/* The gui::TrackListWidget on screen, or 0; the browse view has two and shows
 * one. EDIT is only offered where there are tracks, and the gesture only runs
 * on the list EDIT was entered on. [message] */
uintptr_t browse_track_list(uintptr_t bar);
/* The first visible component of the class at vtable `vt` under `comp`. */
uintptr_t bs_find_visible_class(uintptr_t comp, uintptr_t vt);

/* End a drag that stopped arriving. [message] */
void browse_drag_tick(void);

/* Hook the row's touch and paint slots. 0 when the gesture can run.
 *
 * Without it the plate is not shown, since EDIT would still take the DJ's sort
 * away without being able to reorder. Called from the KIT_MOD in edit.c, the
 * feature's entry point. [init] */
int browse_drag_install(void);

/* ---- drag.c: the gesture -------------------------------------------------
 *
 * While EDIT is on, a finger on a row carries it. Hooked on
 * meow::TouchableTableListBox::RowComp, whose juce::Component is its primary
 * vtable and which overrides all three mouse handlers. Not chaining to the
 * stock handler stops the list scrolling under the drag.
 *
 * That vtable is shared by every touchable table in the app, DJ SETTINGS
 * included, so every hook falls through to stock unless a drag is live. */
#define DG_LINE_H     4         /* the insertion mark, over the row's top edge */
/* The insertion mark's colour: the list's own selected-row green, not
 * mod_ui()->accent (the quick menu's blue), so it matches the browser's
 * selection.
 *
 * Passed through mod_colour_stock where used, so it follows the theme. */
#define DG_MARK_COL   0xff00fb29u
/* Fill for the gap the carried row leaves. Otherwise the content component's
 * dark teal background (#1a3439) shows, which looks like a glitch; black
 * matches an empty list area. */
#define DG_HOLE_COL   0xff000000u
/* Opacity of the carried row's transparency layer (see drag.c). The layer is
 * opened in the wrapper's paint and closed in its paintOverChildren, so it also
 * covers the RowComp's children (the waveform strip and the artwork dot), which
 * paint after it. */
#define DG_GHOST_ALPHA  0.55f

/* Display ticks between polls of the list caches (a few dozen /proc/self/mem
 * reads, too many for 44 Hz). 11 ticks is 250 ms, within the list's own
 * transition. */
#define BE_GATE_TICKS  11

/* Row geometry and identity, as RowComp::paint reads them; it hands all three
 * to the model:
 *
 *   rowcomp + 0x168   int    the row number
 *   rowcomp + 0x148   the owning meow::TouchableTableListBox
 *   listbox + 0x140   the model, whose vtable +0x10 is getNumRows */
#define RC_ROW_OFF    0x168
/* Whether the row draws as selected. RowComp::paint passes this
 * byte as `isSelected` to both the row-background and the per-cell paint, so it
 * controls the blue plate. */
#define RC_SELECTED_OFF 0x170
#define RC_OWNER_OFF  0x148
#define LB_MODEL_OFF  0x140
#define MODEL_NUMROWS 0x10

/* What a row shows is stored on the wrapper, not the RowComp. As
 * ListViewport's visible-area update uses it:
 *
 *   wrap + 0x148   the component the model handed back
 *   wrap + 0x150   the owning list
 *   wrap + 0x158   int    which row it is showing
 *   wrap + 0x15c   byte   selected
 *   list + 0xd8    the model, whose vtable +0x20 is
 *                  refreshComponentForRow(row, selected, existing)
 *
 * The pool is handed out as `components[row % N]` (N is about twelve for a
 * ten-row window), so scrolling two rows reassigns a carried row's component.
 * The RowComp's +0x168 follows this, so the `#` column changes with it while
 * the title, in a child filled by the refresh, does not. */
#define ROW_CHILD_OFF   0x148
#define ROW_OWNER_OFF   0x150
#define ROW_INDEX_OFF   0x158
#define ROW_SEL_OFF     0x15c
#define LB_ROW_MODEL    0xd8
#define MODEL_REFRESH   0x20

/* juce::MouseEvent's first member is the component-relative Point<float>
 * position, so the finger's y in the parent is the row's current y plus this,
 * also while the row is moving. */
#define ME_Y_OFF      0x04

/* The finger could not be located (a row with no wrapper). Distinct from y=0,
 * the top of the list. */
#define DG_NO_FINGER  (-1 << 24)


#ifdef __cplusplus
}
#endif

#endif /* EP122_MODS_BROWSE_H */
