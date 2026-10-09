// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * kit/popup.h - a message on the glass, for any mod.
 *
 * The widget is the deck's own gui::MessagePopupWidget. It needs a parent
 * juce::Component, registered by whichever mod holds one. Until then, and on a
 * firmware where the JUCE primitives do not resolve, every call here is a no-op,
 * so callers need no guard and may call from install.
 *
 * [message] throughout: these build and show juce Components.
 */
#ifndef EP122_MOD_KIT_POPUP_H
#define EP122_MOD_KIT_POPUP_H

#include "core/mod_core.h"

#ifdef __cplusplus
extern "C" {
#endif


/* Line slots the NoTitle layout owns (*(popup + 0x1f8)). */
#define KIT_POPUP_MAX_LINES 11

/* The Component the popup is parented to and drawn over. */
void kit_popup_set_parent(uintptr_t comp);

/* One screen line per entry; the widget lays out 0x30 px per line and centres
 * them. A count outside 1..KIT_POPUP_MAX_LINES shows nothing. */
void kit_popup_show(const char *const *lines, int n);

/* Whether it is on screen; an input path tests this before swallowing the
 * event that dismisses it. */
int  kit_popup_is_up(void);

void kit_popup_dismiss(void);


#ifdef __cplusplus
}
#endif

#endif /* EP122_MOD_KIT_POPUP_H */
