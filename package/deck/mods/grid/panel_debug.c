// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * mods/grid/panel_debug.c - naming and dumping the panel's own component tree.
 */
#include "grid/panel_internal.h"
#include "core/ep122_syms.h"
#include "juce/draw.h"
#include "juce/juce.h"
#include "kit/mod.h"
#include "stem/stem.h"

static const char *gp_class_name(uintptr_t comp)
{
    uintptr_t ti = juce_comp_class(comp);

    if (!ti)
        return "?";
    if (ti == GP_TI_SNAP)  return "SnapGridButton";
    if (ti == GP_TI_SHIFT) return "ShiftGridButton";
    if (ti == GP_TI_RESET) return "ResetButton";
    if (ti == GP_TI_PANEL) return "GridAdjust";
    return "";
}

void gp_dump(uintptr_t comp, int depth)
{
    int32_t b[4] = { 0, 0, 0, 0 };
    int i, n;

    if (depth > 6)
        return;
    /* 0 is success: it forwards mod_safe_read. */
    if (juce_comp_bounds(comp, b) != 0)
        return;
    n = juce_comp_nchild(comp);
    MDBG("gpanel: %*s%p %-16s {%d,%d,%d,%d} vis=%d children=%d\n",
         depth * 2, "", (void *)comp, gp_class_name(comp),
         (int)b[0], (int)b[1], (int)b[2], (int)b[3],
         juce_comp_visible(comp), n);
    for (i = 0; i < n; i++) {
        uintptr_t c = juce_comp_child(comp, i);

        if (c)
            gp_dump(c, depth + 1);
    }
}
