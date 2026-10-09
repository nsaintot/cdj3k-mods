// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * mods/core/resolve_rtti.c - walking EP122's RTTI: typeinfo, class names and vtable bases.
 */
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>
#include <elf.h>
#include "core/resolve_internal.h"
#include "core/resolve.h"

int ti_looks_real(uintptr_t ti)
{
    uintptr_t name;
    int i;

    if (!ti || (ti & 7) || !in_image(ti, 16))
        return 0;
    name = peek(ti + 8);
    if (!name || !in_image(name, 8))
        return 0;
    for (i = 0; i < 8; i++) {
        unsigned char c = ((const unsigned char *)name)[i];

        if (c < 0x20 || c >= 0x7F)
            return 0;
    }
    return 1;
}

int ti_kind(uintptr_t ti)
{
    uintptr_t v = peek(ti);

    if (v && v == g_ti_si) return 1;    /* __si_class_type_info  */
    if (v && v == g_ti_vmi) return 2;   /* __vmi_class_type_info */
    if (v && v == g_ti_class) return 3; /* __class_type_info     */
    return 0;
}

int ti_from_name_ref(uintptr_t at, void *user)
{
    struct ti_hunt *h = user;
    uintptr_t ti = at - 8;

    /* Once the flavour vptrs are known, require one: with tail-merged strings
     * several unrelated records can point into the same name bytes (this made
     * audio_format::FileReadFlac look ambiguous).
     *
     * Before the bootstrap has run, the weaker structural test is used; it only
     * has to seed the walk that discovers the vptrs. */
    if (g_ti_si ? !ti_kind(ti) : !ti_looks_real(ti))
        return 0;
    h->ti = ti;
    h->count++;
    return 0;
}

int ti_classify(uintptr_t at, void *user)
{
    struct ti_sample *s = user;
    uintptr_t nxt;

    /* for_each_ptr_to matches any qword, and a vtable address point is
     * referenced from places that are not type_info heads; keep only the ones
     * that carry a name. */
    if (!ti_looks_real(at))
        return 0;
    s->n++;
    nxt = peek(at + 16);
    if (ti_looks_real(nxt))
        s->si++;
    if ((uint32_t)(nxt >> 32) >= 1 && (uint32_t)(nxt >> 32) <= 64 &&
        (uint32_t)(nxt & 0xFFFFFFFFu) < 0x20)
        s->vmi++;
    return s->n >= TI_SAMPLE_MAX;
}

/* Bootstrap helper: 1 if `v` is non-zero and not yet in seen[], which it is
 * added to while there is room (8). */
int seen_vptr(uintptr_t *seen, int *n, uintptr_t v)
{
    int i;

    if (!v)
        return 0;
    for (i = 0; i < *n; i++)
        if (seen[i] == v)
            return 0;
    if (*n < 8)
        seen[(*n)++] = v;
    return 1;
}

int rtti_base_offset(uintptr_t ti, const char *base, int depth,
                            long at, long *off_out, int *virt)
{
    uintptr_t nxt, name;
    uint32_t cnt;
    int k, i;

    if (depth > 8 || !ti_looks_real(ti))
        return -1;
    k = ti_kind(ti);
    if (k == 1) {                          /* __si_: one base at offset 0 */
        nxt = peek(ti + 16);
        name = peek(nxt + 8);
        if (name && in_image(name, 1) && !strcmp((const char *)name, base)) {
            *off_out = 0;
            return 0;
        }
        return rtti_base_offset(nxt, base, depth + 1, at, off_out, virt);
    }
    if (k != 2)
        return -1;
    cnt = (uint32_t)(peek(ti + 16) >> 32);
    if (cnt > 64)
        return -1;
    for (i = 0; i < (int)cnt; i++) {
        uintptr_t b = peek(ti + 24 + (size_t)i * 16);
        uintptr_t flags = peek(ti + 24 + (size_t)i * 16 + 8);
        long off = (long)(flags >> 8), sub = 0;

        if (off & (1L << 55))
            off -= 1L << 56;
        name = peek(b + 8);
        if (flags & 1) {                   /* virtual: off is a vtable offset */
            if (at != 0)
                continue;
            if (name && in_image(name, 1) && !strcmp((const char *)name, base)) {
                *off_out = off;
                *virt = 1;
                return 0;
            }
            continue;                      /* and not walked through */
        }
        if (name && in_image(name, 1) && !strcmp((const char *)name, base)) {
            *off_out = off;
            return 0;
        }
        if (rtti_base_offset(b, base, depth + 1, at + off, &sub, virt) == 0) {
            *off_out = *virt ? sub : off + sub;
            return 0;
        }
    }
    return -1;
}

uintptr_t vt_virtual_base(uintptr_t primary, uintptr_t ti, long want_top)
{
    uintptr_t at, found = 0;
    int n = 0;

    for (at = primary; at < primary + VT_GROUP_WORDS * sizeof(uintptr_t);
         at += sizeof(uintptr_t)) {
        if (peek(at) != ti || (long)peek(at - 8) != want_top)
            continue;
        if (!in_text(peek(at + 8)))
            continue;
        found = at + 8;
        n++;
    }
    return n == 1 ? found : 0;
}
