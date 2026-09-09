// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * browse.h - the contract between the parts of the browse-list reorder UI.
 *
 * The feature: drag a track to a new position in a PLAYLIST, and have the stick
 * keep it. The persistence half is done and lives elsewhere -- mod_djdb_move_track
 * in db/db.h writes DJDBSONGPLAYLIST.TRACKNO through the deck's own update, on a
 * library server thread. Nothing in this directory writes anything; it decides
 * WHICH move to ask for.
 *
 * ---- why a mode, and why the mode is a button ------------------------------
 *
 * The screen takes ONE touch point, and a one-finger drag already means "scroll
 * the list". So a drag cannot mean both, and a reorder gesture has to be entered
 * deliberately. The CDJ-3000X answers this with an EDIT toggle in the browse
 * header, and this is the same answer: while EDIT is on a drag moves a track,
 * while it is off the list scrolls exactly as it always did.
 *
 * Long-press on a ROW is left alone. On the 3X that copies the track name to a
 * clipboard for the keyboard search. In 3.19 a row long-press is unused --
 * meow::SkinLoadableButtonLongPress feeds gui::BrowseTitleWidget::onButtonLongPress
 * and is a BUTTON concept -- so it is free here, but taking it would collide with
 * the newer firmware and spend a gesture that already has plumbing.
 *
 * PLAYLISTS ONLY. A playlist's `#` is DJDBSONGPLAYLIST.TRACKNO, a stored position
 * that belongs to the list. An artist's or album's `#` is DJDBCONTENT.TRACKNO --
 * the track's own album track number, out of its tags -- so a drag there would
 * rewrite metadata that shows up everywhere else. Different feature.
 *
 * ---- and this is how it knows -----------------------------------------------
 *
 * THE LIST CACHE IT IS SERVED FROM. Every cached list keeps the condition it
 * was asked for, a playlist's carries a playlist hierarchy, and the cache on
 * screen is the one held by someone other than the collector.
 * mod_djdb_playlist_shown is that walk, shared with the reorder's write.
 *
 * Under gui::PlayListView -- the PLAYLIST button's screen -- a track list is a
 * playlist by construction.
 *
 * Nothing else answers. The browse sidebar's categories come off the stick
 * (djdbMenuItems), so neither its row count nor PLAYLIST's index is a constant.
 * gui::TrackListWidget serves a playlist and an album from one object, and
 * gui::BrowseTitleWidget holds only a title and an icon bitmap.
 *
 * And the `#` column is a second, independent condition rather than a
 * replacement: it keeps EDIT off the split preview pane, where a playlist is
 * selected but its tracks have no position column of their own.
 *
 * ---- where it attaches -----------------------------------------------------
 *
 * gui::BrowseTitleWidget is the 1280x90 header. It inherits juce::Component
 * VIRTUALLY through gui::WidgetBase, so its Component slots are in a second
 * vtable of its own group -- EP122_BROWSE_TITLE, which the spec resolves by
 * asking for that base rather than by writing the offset down. Its paint is the
 * anchor: `self` is the header, live and parented, every time the bar draws.
 *
 * Fail open, everywhere. This sits in front of the deck's own browsing, and a
 * browse list that cannot be scrolled is a worse deck than one that cannot
 * reorder.
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
 * {894,0,386,90}, laid out 114 wide on a 124 stride. Ours takes the free slot to
 * the LEFT of that group, and hangs off the HEADER rather than the group -- a
 * child at x=-124 of a 386-wide parent is a child juce clips away.
 *
 * Both numbers are read off the group's own buttons at run time; these are only
 * the fallback for a group that turns out to hold one button, and the floor
 * under a placement that would otherwise land off the bar. */
/* How many gui::BrowseTitleWidgets to carry a plate on. There are two -- the
 * browse screen's and the dedicated playlist screen's -- and they are built once
 * and kept, so this is the whole set with room to spare. */
#define BE_MAX_BARS    4

#define BE_GAP        10
#define BE_MIN_X       0

/* The lettering, measured against the baked PREVIEW beside it: 15px caps. The
 * waveform title bar's quick menu is a bigger 24; the two bars do not share a
 * size, and taking the one already in the tree came out a third too large.
 *
 * The word is drawn by our own paint rather than by the Label, which can only
 * centre in its own bounds and whose bounds ARE the plate -- and this plate says
 * two things, a name at the top and a mark under it. */
#define BE_FONT       20.0f
#define BE_TEXT       "EDIT"
#define BE_LABEL_CY   22            /* the caps' own centre, not the font box's */

/* ---- the reorder mark -----------------------------------------------------
 *
 * Two arrows against three bars: the icon this gesture wears everywhere, and
 * what says the button is about ORDER rather than about editing a track.
 *
 * Whole pixels at one size, no scaling. Nothing in the app's LowLevelGraphics
 * vtable that we have walked antialiases, so a diagonal is a staircase however
 * it is expressed -- which makes an arrowhead the one shape here that has to be
 * drawn row by row, and makes a size that divides evenly worth more than a size
 * that is convenient.
 *
 * The head is SOLID, where the reference draws an open chevron. At 11px across
 * with a 3px stroke the two arms leave a one-pixel gap, which at this size is
 * not an open shape, it is a stipple -- and the first attempt, two runs per row
 * stepping outward, came out solid for its first rows and then split, so the
 * arms read as stopping in mid-air rather than as closing.
 *
 * Everything is symmetric about the middle bar: the two arrows' centres and the
 * two short bars' sit the same distance either side of row 18, which is what
 * makes the mark look upright rather than assembled.
 */
#define BE_GLYPH_W    44
#define BE_GLYPH_H    37
#define BE_GLYPH_Y    40            /* under the word, clear of the bottom lip */
#define BE_GLYPH_T     3            /* one stroke */
#define BE_ARROW_X     7            /* the shaft's left edge */
#define BE_ARROW_Y    26            /* the DOWN arrow's top; the up one is at 0 */
#define BE_ARROW_H    11
#define BE_HEAD_ROWS   5

/* Is the reorder mode on? Read by the gesture half. [message] */
int browse_edit_on(void);

/* The gui::TrackListWidget EDIT was turned on over, or 0 when the mode is off.
 * The gesture's real gate: meow::TouchableTableListBox::RowComp is the row of
 * EVERY touchable table in the app, so "EDIT is on" alone makes the browse
 * sidebar and DJ SETTINGS draggable too -- measured, a tap on the sidebar's
 * TRACK tab became a drag and the tab never switched. [message] */
uintptr_t browse_edit_list(void);

/* ---- sort.c: the sort EDIT borrows and gives back ------------------------
 *
 * A reorder is only meaningful against the list's own stored order, so EDIT
 * forces `#` ascending -- and because that is the DJ's sort it is taking away,
 * it gives it back on the way out. This is what the CDJ-3000X does, and it is
 * better than refusing to reorder under another sort: the re-sort is something
 * the DJ asked for by pressing EDIT rather than a surprise mid-gesture.
 *
 * Both go through juce::TableHeaderComponent, which gui::TrackListHeader is,
 * unmodified but for its mouseDown -- so setting the sort here is the same call
 * the deck makes when a column is tapped, down to the async re-sort it triggers.
 *
 * While it is held, every column loses its `sortable` flag. That is one bit and
 * it disables the header BOTH ways at once: juce's own columnClicked refuses to
 * re-sort a column without it, and the deck's LookAndFeel stops drawing the
 * column's arrow. No custom paint, and nothing to keep in step.
 *
 * `bar` is the browse header, which is where the walk to the list starts.
 * Returns 0 when the sort really was taken. [message] */
int  browse_sort_take(uintptr_t bar);

/* Does the list on screen carry a `#` -- a stored position -- at all? The gate
 * in front of the plate itself, so EDIT is not offered over an artist's tracks
 * or the all-tracks view. It does NOT separate a playlist from an ALBUM: both
 * show `#`, and an album's is the track's own tag rather than a position. That
 * distinction is the one piece still missing; see the head of this file. */
int  browse_sort_has_position(uintptr_t bar);

/* Make the deck fetch the list again after a reorder has been asked for. It
 * sorts AWAY and BACK, because a sort-kind change is a library message and a
 * direction flip is not -- see the note on it. Also what gives the queued write
 * a thread to run on. [message] */
void browse_sort_refetch(void);

void browse_sort_give_back(int resort);

/* Re-assert the disabled header. The deck puts the columns' `sortable` bit back
 * on its own -- measured: the arrows returned mid-mode with no notify and no
 * give-back -- so holding it clear is a poll rather than a one-shot write.
 * Costs a handful of reads and writes only what has drifted.
 *
 * ALSO THE MODE'S OWN EXPIRY: -1 once the `#` it borrowed is no longer a visible
 * column. That is the question worth asking -- a list with no `#` has no
 * position to move a row to -- and it is what works: watching the
 * gui::TrackListWidget change instead left EDIT lit over the all-tracks view
 * with the playlist's sort still clamped onto its header. [message] */
int browse_sort_hold(void);

/* The gui::TrackListWidget on screen, or 0 -- the browse view carries two and
 * shows one. Both halves of this feature ask the same question: EDIT is only
 * offered where there are tracks, and the gesture only runs on the list the
 * mode was entered on. [message] */
uintptr_t browse_track_list(uintptr_t bar);

/* End a drag that stopped arriving. [message] */
void browse_drag_tick(void);

/* Take the row's touch and paint slots. 0 when the gesture can run.
 *
 * The plate does not go up without this: EDIT that cannot reorder is worse than
 * no EDIT, because pressing it still takes the DJ's sort away. Installed by the
 * one KIT_MOD in edit.c, which is the whole feature's entry point. [init] */
int browse_drag_install(void);

/* ---- drag.c: the gesture -------------------------------------------------
 *
 * While EDIT is on, a finger on a row carries it. Hooked on
 * meow::TouchableTableListBox::RowComp, whose juce::Component IS its primary
 * vtable and which overrides all three mouse handlers -- so this sits exactly
 * where the deck's own row touch does, and NOT chaining it is what stops the
 * list scrolling under the drag.
 *
 * That vtable is shared by every touchable table in the app, DJ SETTINGS
 * included, so every hook here returns to stock in its first line unless a drag
 * is actually live. */
#define DG_LINE_H     4         /* the insertion mark, over the row's top edge */
/* And what it is drawn in: the list's OWN selected-row green.
 *
 * A deck value, not mod_ui()->accent. The accent is the quick-menu's lit blue and
 * it is the wrong word here -- this list already has a colour for "this is the row
 * in question", and the mark means exactly that about a row that does not exist
 * yet. Borrowing it says the gesture belongs to the browser rather than to us.
 *
 * Through mod_colour_stock at the point of use, so it tracks whatever the theme
 * does to the deck's own selection. */
#define DG_MARK_COL   0xff00fb29u
/* What the hole the carried row left is filled with. Left alone it shows the
 * content component's own background -- a dark teal, #1a3439 -- which is not a
 * colour this list has anywhere else, so it reads as damage rather than as a
 * gap. Black is what an empty list area already looks like. */
#define DG_HOLE_COL   0xff000000u
/* How far the carried row fades into the list it is crossing. A wash toward the
 * list's own near-black ground IS translucency here, and it has to go down in
 * paintOverChildren rather than paint: a RowComp has children -- the waveform
 * strip and the artwork dot -- which paint AFTER it, so a wash in paint left
 * them at full strength and only dimmed the lettering. */
#define DG_GHOST_ALPHA  0.55f

/* Display ticks between polls of the list caches: a few dozen /proc/self/mem
 * reads, not for 44 Hz. 11 ticks is 250 ms, inside the list's own transition. */
#define BE_GATE_TICKS  11

/* Row geometry and identity, read out of RowComp::paint (0x190f6a0), which
 * hands all three to the model:
 *
 *   rowcomp + 0x168   int    the row number
 *   rowcomp + 0x148   the owning meow::TouchableTableListBox
 *   listbox + 0x140   the model, whose vtable +0x10 is getNumRows */
#define RC_ROW_OFF    0x168
/* Whether the row draws itself SELECTED. RowComp::paint (0x190f6a0) hands this
 * byte to the model as the `isSelected` argument of both the row-background and
 * the per-cell paint, so it is the whole of the blue plate. */
#define RC_SELECTED_OFF 0x170
#define RC_OWNER_OFF  0x148
#define LB_MODEL_OFF  0x140
#define MODEL_NUMROWS 0x10

/* WHAT A ROW IS SHOWING lives on the WRAPPER, not on the RowComp, and this is
 * the whole of how a row gets its track. Read out of ListViewport's
 * visible-area update (0x1910940), which is the deck's own code for it:
 *
 *   wrap + 0x148   the component the model handed back
 *   wrap + 0x150   the owning list
 *   wrap + 0x158   int    which row it is showing
 *   wrap + 0x15c   byte   selected
 *   list + 0xd8    the model, whose vtable +0x20 is
 *                  refreshComponentForRow(row, selected, existing)
 *
 * The pool is handed out as `components[row % N]` -- N is about twelve for a ten
 * row window -- so scrolling two rows is enough to give a carried row away. The
 * RowComp's own +0x168 follows this, which is why the `#` column changes with it
 * while the title, which lives in a child the refresh fills in, does not. */
#define ROW_CHILD_OFF   0x148
#define ROW_OWNER_OFF   0x150
#define ROW_INDEX_OFF   0x158
#define ROW_SEL_OFF     0x15c
#define LB_ROW_MODEL    0xd8
#define MODEL_REFRESH   0x20

/* juce::MouseEvent's first member is Point<float> position, component-relative
 * -- so the finger's y in the PARENT is the row's current y plus this, which
 * stays true while the row is being moved. */
#define ME_Y_OFF      0x04

/* "The finger could not be located." A row with no wrapper, which is a row this
 * does not understand; distinct from y=0, which is the top of the list. */
#define DG_NO_FINGER  (-1 << 24)


#ifdef __cplusplus
}
#endif

#endif /* EP122_MODS_BROWSE_H */
