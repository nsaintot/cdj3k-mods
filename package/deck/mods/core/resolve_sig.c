// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * mods/core/resolve_sig.c - finding a free function by its instructions: masked signatures and branch targets.
 */
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>
#include <elf.h>
#include "core/resolve_internal.h"
#include "core/resolve.h"

/* BL targets inside [start, start+span), in program order. */
int bl_targets(uintptr_t start, unsigned span, uintptr_t *out, int max)
{
    unsigned i;
    int n = 0;

    for (i = 0; i + 4 <= span && n < max; i += 4) {
        uint32_t insn;
        int32_t imm;

        if (!in_text(start + i))
            break;
        memcpy(&insn, (const void *)(start + i), sizeof(insn));
        if ((insn & 0xFC000000u) != 0x94000000u)
            continue;
        imm = (int32_t)(insn << 6) >> 6;        /* sign-extend imm26 */
        out[n++] = start + i + (intptr_t)imm * 4;
    }
    return n;
}

/* The address computed by the ADRP/ADD (or ADRP/LDR) pair at `insn`. */
uintptr_t adrp_pair(uintptr_t fn, unsigned insn)
{
    uintptr_t va = fn + (uintptr_t)insn * 4, page;
    uint32_t w, rd;
    int32_t imm;
    int k;

    if (!in_text(va))
        return 0;
    memcpy(&w, (const void *)va, sizeof(w));
    if ((w & 0x9F000000u) != 0x90000000u)
        return 0;
    imm = (int32_t)((((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 3));
    imm = (imm << 11) >> 11;                    /* sign-extend imm21 */
    page = (va & ~(uintptr_t)0xFFF) + (intptr_t)imm * 4096;
    rd = w & 0x1F;

    /* The ADD need not follow the ADRP: the compiler can schedule other work
     * between them (eight apart in the Renesas POPUP_CTOR). Keep this window the
     * same size as tools/gen-syms.py's, or a symbol can resolve on the Mac and
     * not on the deck. */
    for (k = 1; k < 16; k++) {
        uint32_t w2;

        if (!in_text(va + 4 * k))
            return 0;
        memcpy(&w2, (const void *)(va + 4 * k), sizeof(w2));
        if (((w2 >> 5) & 0x1F) != rd)
            continue;
        if ((w2 & 0xFFC00000u) == 0x91000000u)          /* ADD x, x, #imm    */
            return page + ((w2 >> 10) & 0xFFF);
        if ((w2 & 0xFFC00000u) == 0xF9400000u)          /* LDR x, [x, #imm]  */
            return page + ((w2 >> 10) & 0xFFF) * 8;
        if ((w2 & 0xFFC00000u) == 0xB9400000u)          /* LDR w, [x, #imm]  */
            return page + ((w2 >> 10) & 0xFFF) * 4;
    }
    return 0;
}

uintptr_t bl_target(uintptr_t fn, unsigned insn)
{
    uintptr_t va = fn + (uintptr_t)insn * 4;
    uint32_t w;
    int32_t imm;

    if (!in_text(va))
        return 0;
    memcpy(&w, (const void *)va, sizeof(w));
    if ((w & 0xFC000000u) != 0x94000000u)
        return 0;
    imm = (int32_t)(w & 0x03FFFFFFu);
    imm = (imm << 6) >> 6;                      /* sign-extend imm26 */
    return va + (intptr_t)imm * 4;
}
