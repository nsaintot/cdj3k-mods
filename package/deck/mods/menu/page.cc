// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * menu/page.cc - the content area a feature's page draws into.
 *
 * A juce::Label with paint and mouseDown replaced, over the list and pane
 * area. It forwards both to the page shown (kit_page in kit/menu.h); the
 * rotary, push and BACK reach the page through overlay.cc.
 */
#include "menu/internal.h"
#include "juce/juce.hh"

static uintptr_t g_comp;
static uintptr_t g_vt[VT_CLONE_WORDS];
static const struct kit_page *g_page;

static void page_paint(void *, void *g)
{
    if (g_page && g_page->paint) g_page->paint(g_page->ctx, g, PAGE_W, PAGE_H);
}

/* MouseEvent::position, relative to this component, is two floats at +0. */
static void page_mousedown(void *, void *event)
{
    float pos[2];

    if (!g_page || !event || mod_safe_read((uintptr_t)event, pos, sizeof(pos)) != 0) return;
    menu_set_focus(FOCUS_LIST);
    if (g_page->tap) g_page->tap(g_page->ctx, (int)pos[0], (int)pos[1]);
}

int menu_page_build(uintptr_t parent)
{
    static const struct juce_vt_override ov[] = {
        { JUCE_VT_PAINT,     (void *)page_paint,     0 },
        { JUCE_VT_MOUSEDOWN, (void *)page_mousedown, 0 },
    };
    uintptr_t vptr = juce_label_vt_clone(g_vt, ov, (int)(sizeof(ov) / sizeof(ov[0])));

    if (!vptr) return -1;
    g_comp = juce_label(parent, "", KIT_UI_FONT_LABEL, 0, 0, vptr, PAGE_X, PAGE_Y, PAGE_W, PAGE_H);
    if (!g_comp) return -1;
    juce_comp_set_visible(g_comp, 0);
    return 0;
}

/* Show `p` in the content area, or hide the area for NULL. */
void menu_page_show(const struct kit_page *p)
{
    if (p == g_page) return;
    if (g_page && g_page->shown) g_page->shown(g_page->ctx, 0);
    g_page = p;
    juce_comp_set_visible(g_comp, p != NULL);
    if (p && p->shown) p->shown(p->ctx, 1);
    menu_page_repaint();
}

void menu_page_repaint(void)
{
    if (g_comp && g_page) ep_call(void(void *))::at(FN_COMP_REPAINT, (void *)g_comp);
}
