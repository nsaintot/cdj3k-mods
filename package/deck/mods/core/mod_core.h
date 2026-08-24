// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * mod_core.h - safe memory access, slot patching, the patch journal, logging.
 *
 * Shared by every mod; knows about none of them. A mod's own state and entry
 * points live in that mod's header.
 *
 * The install model, the naming rule for shared symbols, and the thread tags used
 * throughout: docs/mods.md.
 *
 * Safety model, implemented in common.c:
 *   - EP122's internals are named, not addressed. See resolve.h, ep122_syms.spec.
 *   - Installation is all or nothing.
 *   - Any address dereferenced directly is probed via /proc/self/mem first
 *     (EFAULT rather than SIGSEGV for a bad address).
 *   - vtable/.rodata slots are flipped RO -> RW -> write -> RO; never W+X.
 */
#ifndef EP122_MOD_CORE_H
#define EP122_MOD_CORE_H

#include "loglevel.h"
#include "core/mod_platform.h"
#include "core/resolve.h"

#ifdef __cplusplus
extern "C" {
#endif


/* aarch64 syscall numbers absent from some hosts' headers (mirrors link.c). */
#ifndef SYS_mprotect
#define SYS_mprotect 226
#endif
#ifndef SYS_pread64
#define SYS_pread64 67
#endif
#ifndef SYS_pwrite64
#define SYS_pwrite64 68
#endif
#ifndef SYS_clock_nanosleep
#define SYS_clock_nanosleep 115
#endif

/* ---- logging ----
 *
 * FIVE LEVELS, AND THE DECK SHIPS AT ERROR. A DJ's unit should say nothing while it
 * works; anything it does say should be worth reading. The level is read once from
 * EP122_MOD_LOGLEVEL, whose value is a name or a digit:
 *
 *     EP122_MOD_LOGLEVEL=error   (0)  the default -- only what is broken
 *     EP122_MOD_LOGLEVEL=warn    (1)  + a feature that refused to install, a value not saved
 *     EP122_MOD_LOGLEVEL=info    (2)  + what installed, what a track load decided
 *     EP122_MOD_LOGLEVEL=debug   (3)  + everything a DJ action produces
 *     EP122_MOD_LOGLEVEL=trace   (4)  + everything a FRAME produces
 *
 * Unset or empty leaves ERROR. A value that names no level ALSO leaves ERROR, but says
 * so at ERROR: silence is what someone who typed EP122_MOD_LOGLEVEL=verbose would read
 * as the variable not working at all, and they would be right.
 *
 * Volume is not verbosity, which is why trace is a level of its own rather than more
 * debug: measured over half an hour of ordinary use, per-draw tracing alone was 17k
 * lines and buried every line anyone reads. A grid decision or a pad claim arrives
 * once and cannot be found next to it.
 *
 * ERROR and WARN carry their level in the text because they are the two that appear
 * on a deck nobody is debugging, and whoever reads them is not reading this file.
 * The rest keep the bare prefix every existing grep already matches. */
#define MOD_LOG_ERROR LOG_ERROR
#define MOD_LOG_WARN  LOG_WARN
#define MOD_LOG_INFO  LOG_INFO
#define MOD_LOG_DEBUG LOG_DEBUG
#define MOD_LOG_TRACE LOG_TRACE

extern int g_mod_log;

/* True when `lvl` would be printed. For guarding work that only exists to be logged --
 * a census, a dump, a snapshot -- so the cost goes away with the line. */
#define MLOG_AT(lvl) (g_mod_log >= (lvl))

#define MLOG_(lvl, tag, ...) do { if (MLOG_AT(lvl)) { \
    fprintf(stderr, "[ep122_mod] " tag __VA_ARGS__); fflush(stderr); } } while (0)

#define MERR(...)   MLOG_(MOD_LOG_ERROR, "ERROR: ", __VA_ARGS__)
#define MWARN(...)  MLOG_(MOD_LOG_WARN,  "WARN: ",  __VA_ARGS__)
#define MINFO(...)  MLOG_(MOD_LOG_INFO,  "",        __VA_ARGS__)
#define MDBG(...)   MLOG_(MOD_LOG_DEBUG, "",        __VA_ARGS__)
#define MTRACE(...) MLOG_(MOD_LOG_TRACE, "",        __VA_ARGS__)


/* ---- safe memory access ---- */

/* Read `len` bytes from `addr` via /proc/self/mem. 0 on a full read, else -1. */
int  mod_safe_read(uintptr_t addr, void *buf, size_t len);

/* Write `len` bytes to `addr` via pwrite64 on /proc/self/mem: reaches RO pages,
 * and returns -1 (EFAULT) on a bad address instead of raising SIGSEGV. Used for
 * live pointer/flag pokes into JUCE view state. 0 on success, -1 else. */
int  mod_safe_write(uintptr_t addr, const void *buf, size_t len);

/* mprotect the page(s) spanning [addr, addr+len). 0 on success, -1 on error. */
int  mod_prot(uintptr_t addr, size_t len, int flags);

/* ---- slot patching ---- */

/* Repoint one pointer slot (vtable/.rodata) at `wrapper`, saving the previous
 * target in *saved. Verifies the slot holds `expect_fn`; a mismatch or
 * unreadable address returns -1 without touching memory. `expect_fn` is
 * normally read from the same slot a moment earlier (see mod_patch_vslot), so
 * the check is a guard against a slot that moved out from under that read. */
int  mod_patch_slot(const char *name, uintptr_t slot, uintptr_t expect_fn,
                    void *wrapper, uintptr_t *saved);

/* Patch a virtual named by (resolved vtable symbol, byte offset from the address
 * point). Preferred: the stock function comes out of the slot itself, so there
 * is no address to state and nothing to guard. -1 if the class or slot is
 * absent. */
int  mod_patch_vslot(const char *name, int vt_sym, unsigned off,
                     void *wrapper, uintptr_t *saved);

/* Write `val` back into an already-patched slot. */
void mod_restore_slot(uintptr_t slot, uintptr_t val);

/* Put `n` bytes of code back at `fn`, flushing the instruction cache. */
void mod_restore_code(uintptr_t fn, const uint8_t *code, size_t n);

/* ---- inline function hooking ----
 *
 * For a FREE function -- one reached by a direct call rather than through a
 * vtable, so there is no slot to repoint. The first four instructions are
 * replaced with a branch to `hook`, and *tramp is set to a copy of those four
 * followed by a branch back into the original, so a hook calls *tramp to get
 * the stock behaviour.
 *
 * Prefer mod_patch_vslot wherever a virtual will do. This rewrites executable
 * memory and can only be as safe as the check below; a slot patch cannot be
 * wrong in that way.
 *
 * REFUSES rather than corrupts. Every displaced instruction is checked for
 * PC-relative encodings -- ADR/ADRP, B/BL, B.cond, CBZ/CBNZ, TBZ/TBNZ and
 * literal loads all mean something different once moved -- and a function whose
 * prologue holds any of them is left alone with a log line saying which word.
 * That check is the whole safety argument: everything else here is mechanical.
 *
 * Journalled like a slot patch, so mod_unpatch_owner puts the bytes back.
 */
int  mod_patch_fn(const char *name, uintptr_t fn, void *hook, uintptr_t *tramp);

/* ---- patch journal ----
 * Every successful patch is recorded against the mod being installed, so an
 * uninstall needs no second copy of that mod's vtable-and-offset list. The
 * registry stamps the owner; mods do not call mod_patch_owner(). */
void mod_patch_owner(const char *name);

/* Restore every slot `owner` patched, newest first. NULL means all. Returns the
 * count; already-restored slots are skipped, so calling twice is safe.
 *
 * There is no process-wide uninstall. The only place one could be called from is
 * a destructor, which does not run when the process is signalled -- and EP122 is
 * stopped with SIGTERM. Restoring slots in an address space about to be unmapped
 * would be unobservable in any case. */
int  mod_unpatch_owner(const char *owner);

/* The same for the mod currently installing: the rollback an install uses when
 * it gets part-way in and stops. */
int  mod_unpatch_current(void);

/* ---- the registry (common.c) ----
 * Install every mod that declared a KIT_MOD descriptor (kit/mod.h), in (prio,
 * name) order, stamping the patch journal and printing the one-line summary. */
void mods_install_all(void);


#ifdef __cplusplus
}
#endif

#endif /* EP122_MOD_CORE_H */
