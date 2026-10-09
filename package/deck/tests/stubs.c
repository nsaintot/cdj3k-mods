// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * stubs.c - the shim state the tested sources read.
 *
 * On the deck these live in mods/core/common.c and mods/juce/draw.cc, which
 * reach the settings file, the resolver and JUCE. These definitions let
 * roles.c and presets.c link into a host test binary; the tests set them
 * directly. Nothing in PURE_SRCS may need them: `make purity` rejects a source
 * that does.
 */
#include "mods/core/mod_core.h"
#include "mods/juce/draw.h"
#include "mods/theme/theme.h"

int g_mod_log = MOD_LOG_ERROR;
int g_theme_id;

void mod_draw_ground(int light)
{
    (void)light;
}
