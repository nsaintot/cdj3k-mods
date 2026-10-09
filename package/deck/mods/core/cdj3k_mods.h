/* SPDX-License-Identifier: MIT OR Apache-2.0 */
/*
 * cdj3k_mods.h - what a host shim links against.
 *
 * The mods install themselves from a constructor and register through the
 * `ep122_mods` linker section; the host starts none of this. The host only
 * forwards the file activity below, which the library watcher needs and only a
 * syscall interposer sees.
 */
#ifndef CDJ3K_MODS_H
#define CDJ3K_MODS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


/* Forward from the host's open/write/close interposers. `ra` is the return
 * address of the caller that issued the write. */
void db_watch_open(const char *path, int fd, int flags);
void db_watch_write(int fd, size_t count, uintptr_t ra);
void db_watch_close(int fd);


#ifdef __cplusplus
}
#endif

#endif /* CDJ3K_MODS_H */
