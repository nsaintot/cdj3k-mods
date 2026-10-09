// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * kit/mod.h - what makes a feature a mod.
 *
 * A feature declares itself next to its own install function. The descriptors
 * land in one linker section that common.c walks, so adding a mod only means
 * adding a file; there is no central list of features.
 *
 * Declaration is a static initialiser; install is [init].
 */
#ifndef EP122_MOD_KIT_MOD_H
#define EP122_MOD_KIT_MOD_H

struct kit_mod {
    /* Tags every slot this mod patches; uninstall and the install summary use it. */
    const char *name;

    const char *what;   /* one line, in the install log */

    /* Install order, ascending; ties broken by name. The mods use 10..80 in
     * steps of ten, leaving room to insert one without renumbering. */
    short prio;

    /* 0 when installed, -1 when the deck runs stock for this feature. The
     * caller unwinds whatever a failed install patched. */
    int (*install)(void);   /* [init] */
};

/* One descriptor. `used` because only the section refers to it. */
#define KIT_MOD(sym, ...) \
    static const struct kit_mod sym __attribute__((used, \
        section("ep122_mods"))) = { __VA_ARGS__ }

/* The section bounds, defined by GNU ld for any C-identifier section name.
 * Hidden explicitly because -fvisibility=hidden does not cover linker-defined
 * symbols, and every symbol the shim exports interposes that name in EP122. */
extern const struct kit_mod __start_ep122_mods[] __attribute__((visibility("hidden")));
extern const struct kit_mod __stop_ep122_mods[] __attribute__((visibility("hidden")));

#endif /* EP122_MOD_KIT_MOD_H */
