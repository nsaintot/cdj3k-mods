// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * kit/ui.cc - stock UTILITY drawing. See ui.h.
 */
#include "kit/ui.h"
#include "juce/draw.h"
#include "juce/juce.hh"

/* Plain Graphics::setColour, so the theme recolours it as it recolours stock. */
void kit_ui_colour(void *g, uint32_t argb)
{
    uint32_t c = argb;

    ep_call(void(void *, void *))::at(MOD_FN_GFX_SETCOLOUR, g, &c);
}

void kit_ui_rect(void *g, int x, int y, int w, int h)
{
    ep_call(void(void *, int, int, int, int))::at(MOD_FN_GFX_FILLRECT, g, x, y, w, h);
}

void kit_ui_text(void *g, const char *text, float font_h, int x, int y, int w, int h,
                 int justification)
{
    mod_gfx_text_in(g, text, font_h, x, y, w, h, justification);
}

void kit_ui_row_bg(void *g, int w, int h, enum kit_ui_row look)
{
    kit_ui_colour(g, look == KIT_UI_ROW_FOCUSED ? KIT_UI_FOCUS :
                     look == KIT_UI_ROW_CURSOR  ? KIT_UI_SELECTED : KIT_UI_PANEL);
    kit_ui_rect(g, 0, 0, w, h);
    kit_ui_colour(g, KIT_UI_BLACK);
    kit_ui_rect(g, 0, h - 2, w, 2);
}

void kit_ui_setting(void *g, int w, int h, const char *label, const char *value,
                    enum kit_ui_row look)
{
    kit_ui_colour(g, look == KIT_UI_ROW_NORMAL ? KIT_UI_VALUE : KIT_UI_WHITE);
    kit_ui_text(g, value, KIT_UI_FONT_VALUE, 0, 0, w - KIT_UI_INSET, h, KIT_UI_RIGHT);
    kit_ui_colour(g, KIT_UI_WHITE);
    kit_ui_text(g, label, KIT_UI_FONT_LABEL, KIT_UI_INSET, 0, w, h, KIT_UI_LEFT);
}
