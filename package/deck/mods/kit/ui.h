// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * kit/ui.h - drawing in the stock UTILITY style, for MOD SETTINGS and the
 * pages features add to it (kit/menu.h).
 *
 * The colours, fonts and row paint are those of the stock UTILITY lists, and go
 * through the theme the way stock colours do. Everything here is [message] and
 * takes the juce::Graphics a paint receives.
 */
#ifndef EP122_MOD_KIT_UI_H
#define EP122_MOD_KIT_UI_H

#include "core/mod_core.h"

#ifdef __cplusplus
extern "C" {
#endif


/* Stock colours, ARGB. */
#define KIT_UI_PANEL      0xff191919u   /* row, rail cell, panel           */
#define KIT_UI_BLACK      0xff000000u   /* divider, gaps                   */
#define KIT_UI_FOCUS      0xff007de1u   /* cursor in the focused column    */
#define KIT_UI_SELECTED   0xff646465u   /* cursor in an unfocused column   */
#define KIT_UI_RULE       0xff252525u   /* bottom rule                     */
#define KIT_UI_WHITE      0xffffffffu   /* labels                          */
#define KIT_UI_VALUE      0xff7d7d7du   /* values                          */
#define KIT_UI_DIMMED     0xff414041u   /* locked text                     */

/* Stock fonts and metrics. */
#define KIT_UI_FONT_LABEL 32.0f
#define KIT_UI_FONT_VALUE 28.0f
#define KIT_UI_ROW_H      50
#define KIT_UI_INSET      14            /* text inset from the row edges   */

/* juce::Justification flags. */
#define KIT_UI_LEFT       0x01
#define KIT_UI_RIGHT      0x02
#define KIT_UI_HCENTRE    0x04
#define KIT_UI_VCENTRE    0x20
#define KIT_UI_CENTRED    (KIT_UI_HCENTRE | KIT_UI_VCENTRE)

/* How a list row looks. */
enum kit_ui_row {
    KIT_UI_ROW_NORMAL,              /* not the cursor row              */
    KIT_UI_ROW_CURSOR,              /* cursor, column not focused      */
    KIT_UI_ROW_FOCUSED,             /* cursor, column focused          */
};

void kit_ui_colour(void *g, uint32_t argb);
void kit_ui_rect(void *g, int x, int y, int w, int h);

/* One line of text in the deck's font, squeezed to fit like stock. */
void kit_ui_text(void *g, const char *text, float font_h, int x, int y, int w, int h,
                 int justification);

/* A list row's background and the 2 px divider under it. */
void kit_ui_row_bg(void *g, int w, int h, enum kit_ui_row look);

/* A settings row: label on the left, value on the right, as DJ SETTING draws
 * them. The value is white on the cursor row. */
void kit_ui_setting(void *g, int w, int h, const char *label, const char *value,
                    enum kit_ui_row look);


#ifdef __cplusplus
}
#endif

#endif /* EP122_MOD_KIT_UI_H */
