// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * resolve.h - EP122 symbols by identity instead of by address.
 *
 * EP122 ships stripped. mods/core/ep122_syms.spec names what the mods need (an
 * RTTI class, a vtable slot, a masked instruction signature) and this resolves
 * those against the binary the deck is running, so no address is tied to one
 * firmware build.
 *
 * Call ep122_resolve() once from the constructor, before any mod installs. An
 * unresolved symbol stays 0, which the mods treat like a failed prologue guard:
 * skip the hook, log why, leave the deck stock. Nothing here writes to EP122's
 * memory.
 */
#ifndef EP122_RESOLVE_H
#define EP122_RESOLVE_H

#include <stdint.h>

#include "core/ep122_syms.h"

#ifdef __cplusplus
extern "C" {
#endif


/* Resolve everything in the spec. Returns the number of symbols that resolved;
 * ep122_resolve_missing() is how many did not. Safe to call twice (the second
 * call is a no-op). */
int ep122_resolve(void);
int ep122_resolve_missing(void);

/* Print the names of the unresolved symbols, unconditionally. */
void ep122_resolve_log_missing(void);

/* Cheap check that this process is EP122, without the scan.
 *
 * Reads the program headers and checks the code size: EP122 carries ~45 MB,
 * the shell helpers the shim is also preloaded into carry a fraction of that.
 * It lets the boot-strike flag be raised before the scan runs, so the scan is
 * guarded, without apl_start.sh's bash children also writing the flag. */
int ep122_image_is_deck(void);

/* The resolved addresses, indexed by enum ep122_sym. 0 means not found; every
 * mod must handle it. */
extern uintptr_t g_ep122_sym[EP122_SYM__COUNT];

static inline uintptr_t ep122_sym(int id)
{
    return (id >= 0 && id < EP122_SYM__COUNT) ? g_ep122_sym[id] : 0;
}

/* For the log line listing what did not resolve. */
const char *ep122_sym_name(int id);

/* The address of the single live object of a class, found by scanning the
 * writable segments for its vptr. This is the only way to reach a global
 * singleton whose address no code in .text computes (every caller already
 * holds the pointer). Returns 0 unless exactly one instance exists.
 *
 * Call it late: a global's vptr is written by static initialisation, so a call
 * from the shim's own constructor can find nothing. */
uintptr_t ep122_find_instance(int vt_sym);

/* Late-bound values that no static description can reach, recorded from a live
 * call instead. See the `capture` entries in ep122_syms.spec. */
void      ep122_capture_set(int id, uintptr_t value);
uintptr_t ep122_capture_get(int id);

/* Ids for the captured values. Not in enum ep122_sym because there is nothing
 * to resolve; they give the writer and reader of each value a shared slot. */
enum ep122_capture {
    EP122_CAP_READER_FACTORY,
    EP122_CAP_VALUE_COLOUR,
    EP122_CAP__COUNT
};


#ifdef __cplusplus
}
#endif

#endif /* EP122_RESOLVE_H */
