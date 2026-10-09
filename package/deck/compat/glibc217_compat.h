/* SPDX-License-Identifier: MIT OR Apache-2.0 */
/*
 * glibc217_compat.h - pin libm references to the deck's glibc.
 *
 * Force-included into every shim translation unit by deck/Makefile; no source
 * #includes it, so the shim sources stay byte-identical to their originals.
 *
 * The shim is built on glibc 2.27 (Ubuntu 18.04) and must load on the deck's
 * glibc 2.17. aarch64 glibc starts at 2.17, so nearly every symbol the shim
 * calls carries the 2.17 version. The exception is expf/exp2f: glibc 2.27
 * added new implementations and made them the default version, so an ordinary
 * link stamps
 *
 *     expf@GLIBC_2.27   exp2f@GLIBC_2.27
 *
 * into the shim. The deck's 2.17 ld.so has no such version and refuses the
 * object. Because this is an LD_PRELOAD, that refusal hits every process
 * apl_start.sh starts, /bin/sh included, and the deck comes up with no app.
 *
 * .symver rewrites those two references to the 2.17 symbols, which glibc still
 * exports for compatibility. It must be in the TU that references them, hence
 * a force-included header instead of a compat .c file.
 *
 * deck/Makefile checks the resulting version ceiling (`make abi-check`, also
 * run in the Docker build), so a source change that pulls in another post-2.17
 * symbol fails the build.
 */
#ifndef EP122_GLIBC217_COMPAT_H
#define EP122_GLIBC217_COMPAT_H

#if defined(__linux__) && defined(__aarch64__)
/* __GLIBC__ comes from <features.h>, not from the compiler. This header is
 * force-included ahead of every system header, so without including
 * features.h first, __GLIBC__ is undefined and the .symver lines below are
 * silently dropped. */
#  if defined(__has_include) && __has_include(<features.h>)
#    include <features.h>
#  endif
#  if defined(__GLIBC__)
__asm__(".symver expf,expf@GLIBC_2.17");
__asm__(".symver exp2f,exp2f@GLIBC_2.17");
#  endif
#endif

#endif /* EP122_GLIBC217_COMPAT_H */
