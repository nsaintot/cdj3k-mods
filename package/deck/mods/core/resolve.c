// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * resolve.c - find EP122's internals at run time.
 *
 * The resolution mechanisms: docs/mods.md (Symbols).
 *
 * Image reads here do not go through mod_safe_read: the segment bounds come from
 * the program headers via getauxval(AT_PHDR), so a scan confined to a PT_LOAD's
 * file-backed range only reads pages this process runs from. A pread per 8 bytes
 * across ~30 MB would only be slower.
 *
 * Scans stay inside p_filesz, not p_memsz: .bss holds nothing being searched
 * for, and its tail may be short.
 *
 * Cost on the deck: one pass for the names, two for the pointers and one over
 * .text. Nothing here allocates or writes.
 */

#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>
#include <elf.h>

#define EP122_SYMS_IMPL
#include "core/resolve_internal.h"
#include "core/resolve.h"

/* How many times one class name may appear before the walker stops. */
#define NAME_OCCURRENCES 2

#define VT_GROUP_WORDS 4096

struct vt_work {
    uintptr_t name[NAME_OCCURRENCES];
    int       nname;
    uintptr_t ti;
    int       nti;         /* >1 means the name does not identify a class */
    long      want_top;
    /* Non-zero for a virtual base: where in the vtable its real offset lives.
     * want_top is then 0, so the sweep finds the class's primary vtable and
     * vt_virtual_base walks from there to the wanted one. */
    long      vbase_at;
    uintptr_t vt;
    int       nvt;
};


static int ti_collect(uintptr_t at, int which, void *user);
static int vt_collect(uintptr_t at, int which, void *user);

static void sig_scan_all(void);
static void resolve_fn(int idx);

uintptr_t g_ep122_sym[EP122_SYM__COUNT];
static uintptr_t g_ep122_cap[EP122_CAP__COUNT];
static int g_resolved, g_missing;

const char *ep122_sym_name(int id)
{
    if (id < 0 || id >= EP122_SYM__COUNT)
        return "?";
    return k_ep122_sym_name[id];
}

void ep122_capture_set(int id, uintptr_t value)
{
    if (id >= 0 && id < EP122_CAP__COUNT && value)
        __atomic_store_n(&g_ep122_cap[id], value, __ATOMIC_RELEASE);
}

uintptr_t ep122_capture_get(int id)
{
    if (id < 0 || id >= EP122_CAP__COUNT)
        return 0;
    return __atomic_load_n(&g_ep122_cap[id], __ATOMIC_ACQUIRE);
}

int ep122_resolve_missing(void) { return g_missing; }

/* Logs the names of the missing symbols, for the refusal path. Wrapped, not one
 * per line, because a failed RTTI bootstrap leaves every symbol missing. */
void ep122_resolve_log_missing(void)
{
    int i, col = 0;

    if (!g_missing)
        return;
    MERR("resolve: unresolved:");
    for (i = 0; i < EP122_SYM__COUNT; i++) {
        const char *n;

        if (g_ep122_sym[i])
            continue;
        n = ep122_sym_name(i);
        if (col > 60) {
            fprintf(stderr, "\n[ep122_mod] resolve:  ");
            col = 0;
        }
        fprintf(stderr, " %s", n);
        col += (int)strlen(n) + 1;
    }
    fputc('\n', stderr);
    fflush(stderr);
}

/* ------------------------------------------------------------------ image */

/* The loaded segments, from this process's own program headers. */
#define MAX_SEG 8

struct seg {
    uintptr_t va;        /* first mapped address                              */
    size_t    file_len;  /* file-backed length: the only part worth scanning  */
    size_t    mem_len;   /* including .bss, which is mapped but holds nothing */
    int       exec;
};

static struct seg g_seg[MAX_SEG];
static int g_nseg;
uintptr_t g_text_va;
size_t    g_text_len;

static int image_map(void)
{
    const Elf64_Phdr *ph = (const Elf64_Phdr *)getauxval(AT_PHDR);
    unsigned long n = getauxval(AT_PHNUM);
    unsigned long ent = getauxval(AT_PHENT);
    unsigned long i;

    if (g_nseg)
        return 0;               /* idempotent: the segment table is append-only */
    if (!ph || !n || ent != sizeof(Elf64_Phdr)) {
        MDBG("resolve: no usable program headers (phdr %p n %lu ent %lu)\n",
             (const void *)ph, n, ent);
        return -1;
    }
    /* EP122 is ET_EXEC and non-PIE, so p_vaddr is the runtime address with no
     * load bias. Checked below: AT_PHDR must fall inside a mapped segment. */
    for (i = 0; i < n && g_nseg < MAX_SEG; i++) {
        if (ph[i].p_type != PT_LOAD || !ph[i].p_filesz)
            continue;
        g_seg[g_nseg].va = (uintptr_t)ph[i].p_vaddr;
        g_seg[g_nseg].file_len = (size_t)ph[i].p_filesz;
        g_seg[g_nseg].mem_len = (size_t)ph[i].p_memsz;
        g_seg[g_nseg].exec = (ph[i].p_flags & PF_X) != 0;
        if (g_seg[g_nseg].exec) {
            g_text_va = g_seg[g_nseg].va;
            g_text_len = g_seg[g_nseg].file_len;
        }
        g_nseg++;
    }
    for (i = 0; i < (unsigned long)g_nseg; i++)
        if ((uintptr_t)ph >= g_seg[i].va &&
            (uintptr_t)ph < g_seg[i].va + g_seg[i].file_len)
            break;
    if (i == (unsigned long)g_nseg) {
        MDBG("resolve: AT_PHDR %p outside every PT_LOAD -- image is relocated, "
             "refusing to scan\n", (const void *)ph);
        g_nseg = 0;
        return -1;
    }
    if (!g_text_len) {
        MDBG("resolve: no executable PT_LOAD\n");
        g_nseg = 0;
        return -1;
    }
    MTRACE("resolve: %d segments, text %#lx+%#lx\n", g_nseg,
         (unsigned long)g_text_va, (unsigned long)g_text_len);
    return 0;
}

/* Is `va` inside a mapped segment? Everything dereferenced below is checked here
 * first.
 *
 * Against p_memsz, not p_filesz: some resolved symbols live in .bss, e.g. the
 * skin's zero-initialised juce::Colour globals. The scans still stop at
 * p_filesz, since a region that is zero at load time holds no names, vtables or
 * code. */
int in_image(uintptr_t va, size_t len)
{
    int i;

    for (i = 0; i < g_nseg; i++)
        if (va >= g_seg[i].va && len <= g_seg[i].mem_len &&
            va - g_seg[i].va <= g_seg[i].mem_len - len)
            return 1;
    return 0;
}

uintptr_t peek(uintptr_t va)
{
    uintptr_t v;

    if (!in_image(va, sizeof(v)))
        return 0;
    memcpy(&v, (const void *)va, sizeof(v));
    return v;
}

int in_text(uintptr_t va)
{
    return va >= g_text_va && va < g_text_va + g_text_len;
}

/* ------------------------------------------------------------------- RTTI */

/* The three __cxxabiv1 type_info flavour vptrs.
 *
 * They cannot be found by name: this binary has no RTTI for __cxxabiv1's own
 * classes (the three `N10__cxxabiv1..._type_infoE` strings belong to the
 * demangler and nothing points at them). A type_info's first word is its
 * flavour vptr, so the first spec class looked up yields one, and walking its
 * base graph reaches the other two.
 *
 * Flavour is then identified by layout. Past the 16-byte head, __si_ holds one
 * type_info pointer, __vmi_ holds (flags:u32, count:u32), and the base-less
 * flavour holds nothing, so whatever follows it satisfies neither rule.
 */
uintptr_t g_ti_class, g_ti_si, g_ti_vmi;

/* Every 8-aligned qword in the image equal to any of `targets`, handed to `fn`
 * along with which target matched.
 *
 * Batched: 44 classes need two lookups each, which would be 88 sweeps of a 45 MB
 * image. With the targets sorted, the inner test is a range check plus an
 * occasional binary search, so the whole set costs one pass.
 *
 * `targets` must be sorted ascending and free of duplicates. */
typedef int (*ptr_batch_fn)(uintptr_t at, int which, void *user);

static int target_index(const uintptr_t *targets, int n, uintptr_t v)
{
    int lo = 0, hi = n - 1;

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;

        if (targets[mid] == v) return mid;
        if (targets[mid] < v) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

static void for_each_ptr_to_any(const uintptr_t *targets, int ntarget,
                                ptr_batch_fn fn, void *user)
{
    uintptr_t lo, hi;
    int i;

    if (ntarget <= 0)
        return;
    lo = targets[0];
    hi = targets[ntarget - 1];

    for (i = 0; i < g_nseg; i++) {
        const uintptr_t *p = (const uintptr_t *)g_seg[i].va;
        size_t n = g_seg[i].file_len / sizeof(uintptr_t), k;

        for (k = 0; k < n; k++) {
            uintptr_t v = p[k];
            int w;

            /* The range test rejects almost every word in two compares. */
            if (v < lo || v > hi)
                continue;
            w = target_index(targets, ntarget, v);
            if (w < 0)
                continue;
            /* One address can appear several times with different owners, so
             * report the whole run of equal targets. */
            while (w > 0 && targets[w - 1] == v)
                w--;
            for (; w < ntarget && targets[w] == v; w++)
                if (fn(g_seg[i].va + k * sizeof(uintptr_t), w, user))
                    return;
        }
    }
}

/* Single-target form. */
typedef int (*ptr_visit_fn)(uintptr_t at, void *user);

static void for_each_ptr_to(uintptr_t target, ptr_visit_fn fn, void *user)
{
    int i;

    for (i = 0; i < g_nseg; i++) {
        const uintptr_t *p = (const uintptr_t *)g_seg[i].va;
        size_t n = g_seg[i].file_len / sizeof(uintptr_t), k;

        for (k = 0; k < n; k++)
            if (p[k] == target &&
                fn(g_seg[i].va + k * sizeof(uintptr_t), user))
                return;
    }
}



struct vt_work g_vtw[EP122_N_VT];

/* Pass 1: every spec class name, in one sweep per distinct first byte.
 * Itanium mangling gives these the same leading character ('N' for a nested
 * name, '*' for the unique-name form), so this is one or two sweeps; each
 * candidate position is compared against the names sharing that byte.
 *
 * Returns how many spec classes were found. Zero means the process does not
 * contain EP122's C++ (see ep122_image_is_deck()). */
static int g_names_done, g_names_found;

static int names_pass(void)
{
    /* Lengths computed once: a 45 MB image has ~180k 'N' bytes and 44 names to
     * test at each, so a strlen in the inner loop costs ~250 MB of reads. */
    static unsigned short len[EP122_N_VT];
    unsigned char firsts[4];
    int nfirst = 0, i, j, s;

    if (g_names_done)
        return g_names_found;
    g_names_done = 1;

    for (i = 0; i < EP122_N_VT; i++) {
        unsigned char c = (unsigned char)k_ep122_vt[i].cls[0];

        len[i] = (unsigned short)strlen(k_ep122_vt[i].cls);
        for (j = 0; j < nfirst; j++)
            if (firsts[j] == c)
                break;
        if (j == nfirst && nfirst < (int)sizeof(firsts))
            firsts[nfirst++] = c;
    }

    for (s = 0; s < g_nseg; s++)
        for (j = 0; j < nfirst; j++) {
            const char *p = (const char *)g_seg[s].va;
            size_t left = g_seg[s].file_len;

            while (left > 1) {
                const char *hit = memchr(p, firsts[j], left - 1);
                size_t room;

                if (!hit)
                    break;
                room = g_seg[s].file_len -
                       (size_t)((uintptr_t)hit - g_seg[s].va);
                for (i = 0; i < EP122_N_VT; i++) {
                    if (g_vtw[i].nname >= NAME_OCCURRENCES ||
                        (unsigned char)k_ep122_vt[i].cls[0] != firsts[j])
                        continue;
                    if (len[i] + 1u <= room &&
                        !memcmp(hit, k_ep122_vt[i].cls, len[i] + 1u)) {
                        if (!g_vtw[i].nname)
                            g_names_found++;
                        g_vtw[i].name[g_vtw[i].nname++] = (uintptr_t)hit;
                    }
                }
                left -= (size_t)(hit - p) + 1;
                p = hit + 1;
            }
        }
    return g_names_found;
}

/* Build the sorted, de-duplicated target array the batched sweep wants, plus a
 * parallel array saying which class each target belongs to. */
static int build_targets(uintptr_t *targets, unsigned char *owner, int max,
                         int use_ti)
{
    int n = 0, i, k;

    for (i = 0; i < EP122_N_VT; i++) {
        uintptr_t vals[NAME_OCCURRENCES];
        int nval, v;

        if (use_ti) {
            vals[0] = g_vtw[i].ti;
            nval = (g_vtw[i].nti == 1 && g_vtw[i].ti) ? 1 : 0;
        } else {
            for (k = 0; k < g_vtw[i].nname; k++)
                vals[k] = g_vtw[i].name[k];
            nval = g_vtw[i].nname;
        }
        for (v = 0; v < nval && n < max; v++) {
            int pos = n, j;

            /* Insertion sort: at most 88 entries, negligible next to the sweep.
             *
             * Duplicates are kept: three spec entries name gui::UtilityView
             * (the view and two secondary vtables selected by base), so one
             * address can have several owners. Dropping repeats would resolve
             * only the first. */
            while (pos > 0 && targets[pos - 1] > vals[v])
                pos--;
            for (j = n; j > pos; j--) {
                targets[j] = targets[j - 1];
                owner[j] = owner[j - 1];
            }
            targets[pos] = vals[v];
            owner[pos] = (unsigned char)i;
            n++;
        }
    }
    return n;
}

unsigned char g_owner[EP122_N_VT * NAME_OCCURRENCES];





static int rtti_bootstrap(void)
{
    uintptr_t todo[64], seen_ti[64], vptr[8];
    int ntodo = 0, nseen = 0, nvptr = 0, i;

    /* The first spec class whose name the sweep found seeds the walk. Finding
     * its type_info costs one more sweep; the base graph is then followed
     * through pointers. */
    for (i = 0; i < EP122_N_VT && !ntodo; i++) {
        struct ti_hunt h = { 0, 0 };

        if (!g_vtw[i].nname)
            continue;
        for_each_ptr_to(g_vtw[i].name[0], ti_from_name_ref, &h);
        if (h.ti)
            todo[ntodo++] = h.ti;
    }
    if (!ntodo) {
        MDBG("resolve: no spec class name found in the image at all\n");
        return -1;
    }

    while (ntodo) {
        uintptr_t ti = todo[--ntodo], nxt;
        uint32_t cnt, flags;
        int dup = 0;

        for (i = 0; i < nseen; i++)
            if (seen_ti[i] == ti) { dup = 1; break; }
        if (dup || !ti_looks_real(ti))
            continue;
        if (nseen < (int)(sizeof(seen_ti) / sizeof(seen_ti[0])))
            seen_ti[nseen++] = ti;
        seen_vptr(vptr, &nvptr, peek(ti));

        /* Try both base layouts: a wrong reading yields addresses that fail
         * ti_looks_real, so the flavour need not be known yet. */
        nxt = peek(ti + 16);
        if (ntodo < (int)(sizeof(todo) / sizeof(todo[0])))
            todo[ntodo++] = nxt;
        flags = (uint32_t)(nxt & 0xFFFFFFFFu);
        cnt = (uint32_t)(nxt >> 32);
        if (cnt >= 1 && cnt <= 64 && flags < 0x20)
            for (i = 0; i < (int)cnt; i++)
                if (ntodo < (int)(sizeof(todo) / sizeof(todo[0])))
                    todo[ntodo++] = peek(ti + 24 + (size_t)i * 16);
    }

    /* Classify: for every record wearing a candidate vptr, does the word past
     * the head read as a base pointer, as a (flags, count) pair, or neither?
     *
     * Sampled by scanning the image for that vptr: the walk above reaches too
     * few records to tell a real flavour from a stray. */
    for (i = 0; i < nvptr; i++) {
        struct ti_sample s = { vptr[i], 0, 0, 0 };

        for_each_ptr_to(vptr[i], ti_classify, &s);
        /* A real flavour vptr is used by hundreds of type_infos. Misreading a
         * base-less record's tail as a base pointer occasionally passes
         * ti_looks_real, but such a stray is used by only one. */
        if (s.n < 4)
            continue;
        /* __si_ may have stragglers: a base can be a pointer or fundamental
         * type_info, whose vptr is not among the candidates. __vmi_ and the
         * base-less flavour must be unanimous. */
        if (s.vmi == s.n && s.si == 0)
            g_ti_vmi = vptr[i];
        else if (s.si * 10 >= s.n * 9 && s.vmi == 0)
            g_ti_si = vptr[i];
        else if (s.si == 0 && s.vmi == 0)
            g_ti_class = vptr[i];
    }

    if (!g_ti_si || !g_ti_vmi) {
        MDBG("resolve: RTTI bootstrap failed (class %#lx si %#lx vmi %#lx from "
             "%d records, %d vptrs)\n", (unsigned long)g_ti_class,
             (unsigned long)g_ti_si, (unsigned long)g_ti_vmi, nseen, nvptr);
        return -1;
    }
    MDBG("resolve: type_info vptrs class %#lx si %#lx vmi %#lx\n",
         (unsigned long)g_ti_class, (unsigned long)g_ti_si,
         (unsigned long)g_ti_vmi);
    return 0;
}

/* rtti_base_offset() (resolve_rtti.c): where `base` sits inside `ti`; returns 0
 * when it is a base, -1 when not. Depth-limited: the base graph is a DAG, and
 * UtilityView alone has 40-odd bases.
 *
 * *virt is set for a virtual base, and *off is then not an offset (where a
 * virtual base sits depends on the complete object) but where in the vtable
 * the real offset lives; the caller reads it from there. juce::Component is a
 * virtual base of gui::WidgetBase, so every gui:: widget takes this path.
 * *off can be negative, so found/not-found is the return value, not an
 * in-band -1.
 *
 * A virtual edge is only followed while the walk is at offset 0: the vtable
 * offset is relative to the vptr of the class declaring the base, so that
 * class must share the complete object's vptr. That holds along a primary
 * base chain; anywhere else the result is "not a base", not a wrong number. */
/* vt_virtual_base() (resolve_rtti.c): the vtable serving a virtual base, given
 * the class's primary one.
 *
 * A class's vtables are emitted as one object, primary first, so the wanted
 * one is a bounded walk ahead instead of another image sweep. Each is preceded
 * by its offset-to-top and typeinfo, which is what this matches on.
 *
 * A second match is refused. The other candidate with this typeinfo and
 * offset-to-top is a construction vtable, whose slot 0 is null (a half-built
 * object cannot be deleted), so the slot 0 code test separates them. */

/* -------------------------------------------------------------- signatures */

/* bl_target() (resolve_sig.c): the function the BL at `insn` of `fn` calls.
 *
 * For a callee whose signature is useless (a template instantiation with a
 * stock prologue matches hundreds of functions, more than the scan holds) but
 * whose position in a known caller is exact. Like adrp_pair(), it reads the
 * address the deck's own code computes. */
/* ---------------------------------------------------------------- instances */

uintptr_t ep122_find_instance(int vt_sym)
{
    uintptr_t vt = ep122_sym(vt_sym), found = 0;
    int n = 0, i;

    if (!vt)
        return 0;
    /* Over p_memsz, not p_filesz: a global C++ object lives in .bss and gets
     * its vptr during static init. This function exists for such objects, whose
     * address no .text code computes because every caller already holds it.
     *
     * Writable segments only. The vtable address also appears in .rodata
     * next to the typeinfo and in other classes' vtables; those are not
     * objects. */
    for (i = 0; i < g_nseg && n < 2; i++) {
        const uintptr_t *p = (const uintptr_t *)g_seg[i].va;
        size_t words = g_seg[i].mem_len / sizeof(uintptr_t), k;

        if (g_seg[i].exec)
            continue;
        for (k = 0; k < words; k++)
            if (p[k] == vt) {
                found = g_seg[i].va + k * sizeof(uintptr_t);
                if (++n >= 2)
                    break;
            }
    }
    if (n != 1) {
        MDBG("resolve: %s has %d live instances, want exactly 1\n",
             ep122_sym_name(vt_sym), n);
        return 0;
    }
    return found;
}

/* ------------------------------------------------------------------- drive */


/* Every signature's matches, filled by one sweep of .text.
 *
 * Every pattern has an anchor (its first fully-unmasked instruction word), so
 * one sweep tests all anchors at each position and only does a full compare on
 * a hit, which is rare for a 32-bit word. */
static struct {
    uintptr_t hit[SIG_HITS_MAX];
    int       n;              /* may exceed SIG_HITS_MAX; only the count matters */
} g_sig[EP122_N_FN];


/* Anchor index.
 *
 * For every .text word the scan must find the patterns that could start there.
 * Testing each pattern is O(words x patterns): 11.4M words x 70 patterns is
 * 0.8G compares per EP122 start. Hashing each pattern's anchor word makes it
 * one bucket lookup, independent of the pattern count, so the second
 * processor's patterns cost nothing.
 *
 * Buckets chain, since two patterns may share an anchor word; the key is still
 * compared inside the chain, so a hash collision only costs a test.
 */
#define SIG_HASH_BITS 8
#define SIG_HASH_SIZE (1u << SIG_HASH_BITS)
static int g_sig_head[SIG_HASH_SIZE];
static int g_sig_next[EP122_N_FN];

#define FNV64_OFF   0xcbf29ce484222325ULL
#define FNV64_PRIME 0x100000001b3ULL

/* Which bits of an instruction a signature compares.
 *
 * Must match mask_word() in tools/gen-syms.py exactly. Only PC- and
 * data-relative fields are dropped, since they move when the linker relocates
 * the function; everything describing what the code does is kept.
 *
 * The mask is derived from the instruction being tested, not stored in the
 * table, so the tree holds none of the stock code or masks. This is sound
 * because the mask keeps the bits that decide an instruction's class: two
 * windows that agree once masked produced the same masks. This holds for every
 * symbol in the spec.
 */
static uint32_t mask_word(uint32_t w, uint32_t *adrp_regs)
{
    if ((w & 0x7C000000u) == 0x14000000u)         /* B / BL: imm26 */
        return 0xFC000000u;
    if ((w & 0x1F000000u) == 0x10000000u) {       /* ADR / ADRP */
        if (w & 0x80000000u)
            *adrp_regs |= 1u << (w & 0x1F);       /* this reg now holds a page */
        return 0x9F00001Fu;
    }
    if ((w & 0x3B000000u) == 0x18000000u)         /* LDR/LDRSW literal: imm19 */
        return 0xFF00001Fu;
    if ((w & 0x7F800000u) == 0x11000000u) {       /* ADD imm, on an ADRP result */
        if (*adrp_regs & (1u << ((w >> 5) & 0x1F))) {
            *adrp_regs |= 1u << (w & 0x1F);       /* the sum is still that address */
            return 0xFFC003FFu;
        }
    }
    if ((w & 0x3B000000u) == 0x39000000u) {       /* LDR/STR imm, through one */
        if (*adrp_regs & (1u << ((w >> 5) & 0x1F)))
            return 0xFFC003FFu;
    }
    return 0xFFFFFFFFu;
}

/* FNV-1a over the window, each instruction masked by its own encoding. */
static unsigned long long window_digest(uintptr_t start, unsigned insns)
{
    const uint32_t *w = (const uint32_t *)start;
    unsigned long long h = FNV64_OFF;
    uint32_t adrp = 0;
    unsigned i, b;

    for (i = 0; i < insns; i++) {
        uint32_t v = w[i] & mask_word(w[i], &adrp);

        for (b = 0; b < 4; b++)
            h = (h ^ ((v >> (b * 8)) & 0xff)) * FNV64_PRIME;
    }
    return h;
}

/* The 16-bit key the table indexes an anchor word by.
 *
 * Runs on every word of .text, so it is a single multiply. 16 bits of the
 * product do not identify the instruction, so the tree carries a key, never the
 * stock instruction word. A collision only costs a window digest, which rejects it.
 */
static unsigned anchor_key(uint32_t w)
{
    return (unsigned)((w * 2654435761u) >> 16);
}

int ep122_image_is_deck(void)
{
    int found;

    if (image_map() != 0)
        return 0;

    /* Does this process contain EP122's own C++ classes, by name?
     *
     * AT_EXECFN or /proc/self/exe would break on a rename or wrapper script, and
     * a marker string like "EP122Application" may not survive a build; the
     * classes are what the mods depend on anyway.
     *
     * names_pass() is the first step of resolving, so this costs nothing extra
     * and lets the rest be skipped. On a shell helper it is a memchr over a
     * megabyte or two. */
    found = names_pass();
    if (!found) {
        /* TRACE: every shell helper apl_start.sh spawns is preloaded too and
         * logs this. */
        MTRACE("resolve: no EP122 class names in this image -- not the deck\n");
        return 0;
    }
    MDBG("resolve: %d/%d spec classes present -> this is the deck\n",
         found, EP122_N_VT);
    return 1;
}

int ep122_resolve(void)
{
    int i;

    if (g_resolved || g_missing)
        return g_resolved;
    if (!ep122_image_is_deck())
        return 0;

    if (rtti_bootstrap() != 0) {
        g_missing = EP122_SYM__COUNT;
        return 0;
    }

    {
        uintptr_t targets[EP122_N_VT * NAME_OCCURRENCES];
        int n;

        /* name -> type_info, then type_info -> vtable. One sweep each. */
        n = build_targets(targets, g_owner, (int)(sizeof(targets) / sizeof(targets[0])), 0);
        for_each_ptr_to_any(targets, n, ti_collect, NULL);

        for (i = 0; i < EP122_N_VT; i++) {
            struct vt_work *w = &g_vtw[i];

            if (w->nti != 1) {
                if (w->nti > 1)
                    MDBG("resolve: %s names %d type_infos -- refusing\n",
                         ep122_sym_name(k_ep122_vt[i].sym), w->nti);
                w->ti = 0;
                continue;
            }
            /* A secondary vtable is named by the base it serves, so the base's
             * offset must be known before the sweep. */
            w->want_top = 0;
            w->vbase_at = 0;
            if (k_ep122_vt[i].base) {
                int virt = 0;
                long off = 0;

                if (rtti_base_offset(w->ti, k_ep122_vt[i].base, 0, 0, &off,
                                     &virt) != 0) {
                    MDBG("resolve: %s is not a base of %s\n",
                         k_ep122_vt[i].base, k_ep122_vt[i].cls);
                    w->ti = 0;
                    continue;
                }
                /* A virtual base's offset is in the vtable: the sweep finds the
                 * primary first, then the offset is read from it. */
                if (virt)
                    w->vbase_at = off;
                else
                    w->want_top = -off;
            }
        }

        n = build_targets(targets, g_owner, (int)(sizeof(targets) / sizeof(targets[0])), 1);
        for_each_ptr_to_any(targets, n, vt_collect, NULL);

        for (i = 0; i < EP122_N_VT; i++) {
            struct vt_work *w = &g_vtw[i];

            if (w->nvt == 1 && w->vbase_at) {
                long off = (long)peek(w->vt + w->vbase_at);

                w->vt = vt_virtual_base(w->vt, w->ti, -off);
                if (!w->vt) {
                    MDBG("resolve: %s: no single vtable for virtual base %s "
                         "at offset %ld\n", ep122_sym_name(k_ep122_vt[i].sym),
                         k_ep122_vt[i].base, off);
                    continue;
                }
            }
            if (w->nvt == 1)
                g_ep122_sym[k_ep122_vt[i].sym] = w->vt;
            else if (w->nvt > 1)
                MDBG("resolve: %s has %d vtables at offset-to-top %ld\n",
                     ep122_sym_name(k_ep122_vt[i].sym), w->nvt, w->want_top);
        }
    }

    for (i = 0; i < EP122_N_SLOT; i++) {
        uintptr_t vt = ep122_sym(k_ep122_slot[i].vt), fn;

        if (!vt)
            continue;
        fn = peek(vt + k_ep122_slot[i].off);
        if (in_text(fn))
            g_ep122_sym[k_ep122_slot[i].sym] = fn;
    }

    /* A symbol may carry a descriptor per application processor: the .UPD ships
     * two EP122 binaries built by different compilers. Rows are emitted
     * reference-first and every write below requires the symbol to be empty, so
     * the other processor's row can only fill a gap, never overwrite. */
    sig_scan_all();
    for (i = 0; i < EP122_N_FN; i++)
        resolve_fn(i);

    for (i = 0; i < EP122_N_DATA; i++) {
        uintptr_t src = ep122_sym(k_ep122_data[i].src);
        uintptr_t addr = src ? adrp_pair(src, k_ep122_data[i].insn) : 0;

        if (addr && in_image(addr, 8) && !g_ep122_sym[k_ep122_data[i].sym])
            g_ep122_sym[k_ep122_data[i].sym] = addr;
    }

    for (i = 0; i < EP122_N_CALL; i++) {
        uintptr_t src = ep122_sym(k_ep122_call[i].src);
        uintptr_t addr = src ? bl_target(src, k_ep122_call[i].insn) : 0;

        if (addr && in_text(addr) && !g_ep122_sym[k_ep122_call[i].sym])
            g_ep122_sym[k_ep122_call[i].sym] = addr;
    }

    for (i = 0; i < EP122_SYM__COUNT; i++) {
        if (g_ep122_sym[i])
            g_resolved++;
        else {
            g_missing++;
            MDBG("resolve: UNRESOLVED %s\n", ep122_sym_name(i));
        }
    }
    /* INFO: useful when a mod does nothing on an unseen firmware, but quiet by
     * default; the refusal path already logs at ERROR. */
    MINFO("resolve: %d/%d symbols\n", g_resolved, EP122_SYM__COUNT);
    return g_resolved;
}

/*
 * Signature scan, scoped resolution and the sweep callbacks.
 */

static void sig_scan_all(void)
{
    const uint32_t *base = (const uint32_t *)g_text_va;
    size_t words = g_text_len / 4, w;
    int i;

    for (w = 0; w < SIG_HASH_SIZE; w++)
        g_sig_head[w] = -1;
    for (i = 0; i < EP122_N_FN; i++) {
        unsigned h = k_ep122_fn[i].akey & (SIG_HASH_SIZE - 1);

        g_sig_next[i] = g_sig_head[h];
        g_sig_head[h] = i;
    }

    for (w = 0; w < words; w++) {
        uint32_t v = base[w];

        unsigned key = anchor_key(v);

        for (i = g_sig_head[key & (SIG_HASH_SIZE - 1)]; i >= 0; i = g_sig_next[i]) {
            const struct ep122_fn_spec *f = &k_ep122_fn[i];
            uintptr_t start;
            unsigned back = (unsigned)f->anchor * 4;

            if (key != f->akey)
                continue;              /* same bucket, different anchor word */
            if (w * 4 < back)
                continue;
            start = g_text_va + (w * 4 - back);
            if (!in_text(start) || !in_text(start + (uintptr_t)f->insns * 4 - 1))
                continue;
            if (window_digest(start, f->insns) != f->digest)
                continue;
            if (g_sig[i].n < SIG_HITS_MAX)
                g_sig[i].hit[g_sig[i].n] = start;
            g_sig[i].n++;
        }
    }
}

static void resolve_fn(int idx)
{
    const struct ep122_fn_spec *f = &k_ep122_fn[idx];
    const uintptr_t *hits = g_sig[idx].hit;
    int n = g_sig[idx].n;

    /* A truncated hit list would let the scoped path pick from a subset, so
     * overflow is a refusal. */
    if (n > SIG_HITS_MAX) {
        MDBG("resolve: %s matched %d times, more than the %d recorded\n",
             ep122_sym_name(f->sym), n, SIG_HITS_MAX);
        return;
    }

    if (f->scope == 0xffff) {
        if (g_sig[idx].n == 1 && !g_ep122_sym[f->sym])
            g_ep122_sym[f->sym] = hits[0];
        else
            MDBG("resolve: %s signature matched %d times, want 1\n",
                 ep122_sym_name(f->sym), g_sig[idx].n);
        return;
    }

    /* Scoped: the signature cannot separate siblings that differ only in the
     * global they touch, so take the nth one the enclosing function calls. If
     * the count differs from `total`, a firmware added or dropped one, and
     * nothing is resolved rather than the wrong sibling. */
    {
        uintptr_t anchor = ep122_sym(f->scope), calls[64], ordered[32];
        int ncall, nord = 0, i, j;

        if (!anchor) {
            MDBG("resolve: %s has no enclosing function\n",
                 ep122_sym_name(f->sym));
            return;
        }
        ncall = bl_targets(anchor, f->span, calls,
                           (int)(sizeof(calls) / sizeof(calls[0])));
        for (i = 0; i < ncall && nord < (int)(sizeof(ordered) / sizeof(ordered[0])); i++) {
            int match = 0, dup = 0;

            for (j = 0; j < n; j++)
                if (hits[j] == calls[i]) { match = 1; break; }
            if (!match)
                continue;
            for (j = 0; j < nord; j++)
                if (ordered[j] == calls[i]) { dup = 1; break; }
            if (!dup)
                ordered[nord++] = calls[i];
        }
        if (nord != f->total) {
            MDBG("resolve: %s found %d scoped matches, spec expects %d "
                 "-- refusing to guess\n",
                 ep122_sym_name(f->sym), nord, f->total);
            return;
        }
        if (f->nth < nord && !g_ep122_sym[f->sym])
            g_ep122_sym[f->sym] = ordered[f->nth];
    }
}

static int ti_collect(uintptr_t at, int which, void *user)
{
    struct vt_work *w = &g_vtw[g_owner[which]];
    uintptr_t ti = at - 8;

    (void)user;
    /* See ti_from_name_ref: once the flavours are known, insist on one. */
    if (g_ti_si ? !ti_kind(ti) : !ti_looks_real(ti))
        return 0;
    if (w->nti && w->ti == ti)
        return 0;                       /* same record, second pointer to it */
    w->ti = ti;
    w->nti++;
    return 0;
}

static int vt_collect(uintptr_t at, int which, void *user)
{
    struct vt_work *w = &g_vtw[g_owner[which]];
    long top = (long)peek(at - 8);

    (void)user;
    /* Itanium: [offset-to-top][typeinfo][slot0...]; the address point (what a
     * vptr holds) is &slot0. */
    if (top > 0 && top < 0x100000)
        return 0;                       /* not an offset-to-top */
    if (top != w->want_top)
        return 0;
    /* Slot 0 holds code in a vtable an object really points at. Two other
     * things carry a typeinfo pointer at a plausible offset-to-top: a
     * construction vtable, whose destructor slots the ABI leaves null, and an
     * aligned word beside a string or relocation table. Both would make a class
     * with a virtual base report several vtables and stay unresolved.
     *
     * Slot 0 only: an interface with no virtual destructor can be a single
     * entry long, so slot 1 of SoftwareKeyboardPopupWidget::IListener's vtable
     * is the next vtable's offset-to-top. */
    if (!in_text(peek(at + 8)))
        return 0;
    w->vt = at + 8;
    w->nvt++;
    return 0;
}
