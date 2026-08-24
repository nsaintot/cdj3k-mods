// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * common.c - safe memory access, slot patching, the settings record, and the
 * constructor that detects EP122, resolves its symbols and installs the mods.
 */
#include "core/mod_core.h"
#include "core/mod_settings.h"
#include "kit/mod.h"
/* The settings record's field widths and the theme name it logs; no feature's
 * install is named here. */
#include "stem/stem.h"
#include "theme/theme.h"
#include "cue/cue.h"
#include "xpad/ext.h"
#include "core/version.h"

#include <stddef.h>   /* offsetof: the settings CRC covers everything before it */

int  g_mod_log = MOD_LOG_ERROR;   /* quiet until EP122_MOD_LOGLEVEL says otherwise */
int  g_theme_id;
int  g_stems_on;
int  g_stem_manual;
char g_stem_addr[STEM_ADDR_MAX];
char g_stem_sep_id[STEM_SEP_ID_MAX];

static int  g_memfd = -1;     /* /proc/self/mem, O_RDONLY (reads) */
static int  g_memfd_rw = -1;  /* /proc/self/mem, O_RDWR   (writes) */
static long g_pagesize;

/* ================================================================== */
/* Safe memory access                                                 */
/* ================================================================== */

int mod_safe_read(uintptr_t addr, void *buf, size_t len)
{
    if (g_memfd < 0) {
        g_memfd = (int)syscall(SYS_openat, AT_FDCWD, "/proc/self/mem",
                               O_RDONLY | O_CLOEXEC, (long)0);
        if (g_memfd < 0) return -1;
    }
    long n = syscall(SYS_pread64, (long)g_memfd, (long)buf, (long)len, (long)addr);
    return (n == (long)len) ? 0 : -1;
}

int mod_safe_write(uintptr_t addr, const void *buf, size_t len)
{
    if (g_memfd_rw < 0) {
        g_memfd_rw = (int)syscall(SYS_openat, AT_FDCWD, "/proc/self/mem",
                                  O_RDWR | O_CLOEXEC, (long)0);
        if (g_memfd_rw < 0) return -1;
    }
    long n = syscall(SYS_pwrite64, (long)g_memfd_rw, (long)buf, (long)len, (long)addr);
    return (n == (long)len) ? 0 : -1;
}

/* ================================================================== */
/* Patch journal                                                      */
/* ================================================================== */

/*
 * Every slot this process has repointed, in order, tagged with the mod that did
 * it. An uninstall is addressed by owner, so no mod repeats its own vtable-and-
 * offset list. The owner is stamped by the registry loop, not by the mod.
 *
 * Fixed array: this runs during static init, where an allocation failure has
 * nowhere to go. A patch that cannot be journaled is refused rather than made --
 * an unrecorded slot could never be put back.
 */
#define MOD_PATCH_MAX 128

/* Displaced instructions for an inline hook: four, because the branch that
 * replaces them is four words (a literal load of the target, a register branch,
 * and the 8-byte target itself). */
#define MOD_HOOK_WORDS 4
#define MOD_HOOK_BYTES (MOD_HOOK_WORDS * 4)

struct mod_patch {
    uintptr_t   slot;
    uintptr_t   orig;    /* 0 once restored -- a second unpatch is a no-op */
    const char *owner;
    /* Inline hooks restore CODE rather than a pointer, so the bytes travel with
     * the entry. `orig` is the hooked function for those, and stays non-zero
     * for the same "already restored?" test. */
    uint8_t     code[MOD_HOOK_BYTES];
    uint8_t     is_fn;
};

static struct mod_patch g_patch[MOD_PATCH_MAX];
static int              g_npatch;
static const char      *g_patch_owner = "?";

void mod_patch_owner(const char *name)
{
    g_patch_owner = name ? name : "?";
}

int mod_unpatch_owner(const char *owner)
{
    int i, n = 0;

    /* Reverse order: a slot two mods both repointed ends up holding EP122's own
     * function, not the wrapper underneath. */
    for (i = g_npatch - 1; i >= 0; i--) {
        struct mod_patch *p = &g_patch[i];

        if (!p->orig || (owner && strcmp(p->owner, owner) != 0))
            continue;
        if (p->is_fn) {
            mod_restore_code(p->slot, p->code, MOD_HOOK_BYTES);
            MDBG("unpatch: %s code %#lx (%d bytes)\n", p->owner,
                 (unsigned long)p->slot, MOD_HOOK_BYTES);
        } else {
            mod_restore_slot(p->slot, p->orig);
            MDBG("unpatch: %s slot %#lx -> %#lx\n", p->owner,
                 (unsigned long)p->slot, (unsigned long)p->orig);
        }
        p->orig = 0;
        n++;
    }
    return n;
}

int mod_unpatch_current(void)
{
    return mod_unpatch_owner(g_patch_owner);
}

/* ================================================================== */
/* RO -> RW -> write -> RO  pointer-slot patcher                       */
/* ================================================================== */

static inline uintptr_t page_floor(uintptr_t a) { return a & ~((uintptr_t)g_pagesize - 1); }

int mod_prot(uintptr_t addr, size_t len, int flags)
{
    uintptr_t start = page_floor(addr);
    size_t span = (addr + len) - start;
    long r = syscall(SYS_mprotect, (long)start, (long)span, (long)flags);
    return (r < 0) ? -1 : 0;
}

int mod_patch_slot(const char *name, uintptr_t slot, uintptr_t expect_fn,
                   void *wrapper, uintptr_t *saved)
{
    uintptr_t cur = 0;
    if (mod_safe_read(slot, &cur, sizeof(cur)) != 0) {
        MDBG("%s: slot %#lx UNREADABLE -> skip\n", name, slot);
        return -1;
    }
    if (cur != expect_fn) {
        MDBG("%s: slot %#lx holds %#lx, expected %#lx -> skip\n",
             name, slot, (unsigned long)cur, (unsigned long)expect_fn);
        return -1;
    }
    /* Checked before the write: an unjournalable slot is not patched. */
    if (g_npatch >= MOD_PATCH_MAX) {
        MDBG("%s: patch journal full (%d entries) -> skip\n", name, MOD_PATCH_MAX);
        return -1;
    }
    if (mod_prot(slot, sizeof(uintptr_t), PROT_READ | PROT_WRITE) != 0) {
        MDBG("%s: mprotect RW failed errno=%d -> skip\n", name, errno);
        return -1;
    }
    *(uintptr_t *)slot = (uintptr_t)wrapper;
    mod_prot(slot, sizeof(uintptr_t), PROT_READ);
    g_patch[g_npatch].slot  = slot;
    g_patch[g_npatch].orig  = expect_fn;
    g_patch[g_npatch].owner = g_patch_owner;
    g_npatch++;
    if (saved) *saved = expect_fn;
    MDBG("%s: %#lx -> wrapper %p (stock %#lx)\n", name, slot, wrapper, (unsigned long)expect_fn);
    return 0;
}

/* Patch a virtual by naming the class and the slot rather than the address.
 *
 * There is no expect_fn and no prologue guard here, and their absence is the
 * point rather than an omission. Both existed to catch ONE failure: a hardcoded
 * address that a firmware revision had moved out from under us. Read the slot
 * out of a vtable the RTTI walk just found and that failure cannot happen --
 * whatever slot +0xd0 of juce::Label holds IS juce::Label::paint, so an
 * expect_fn taken from the same read is a tautology and a byte guard proves
 * nothing beyond it. What can still go wrong -- the class not existing, the
 * slot being past the end of the vtable -- is caught here instead.
 *
 * `off` is a byte offset from the address point, matching how the spec and
 * every ep122sym.py dump write it. */
int mod_patch_vslot(const char *name, int vt_sym, unsigned off,
                    void *wrapper, uintptr_t *saved)
{
    uintptr_t vt = ep122_sym(vt_sym), fn = 0;

    if (!vt) {
        MDBG("%s: %s unresolved -> skip\n", name, ep122_sym_name(vt_sym));
        return -1;
    }
    if (mod_safe_read(vt + off, &fn, sizeof(fn)) != 0 || !fn) {
        MDBG("%s: %s slot +%#x unreadable -> skip\n",
             name, ep122_sym_name(vt_sym), off);
        return -1;
    }
    return mod_patch_slot(name, vt + off, fn, wrapper, saved);
}

void mod_restore_slot(uintptr_t slot, uintptr_t val)
{
    if (mod_prot(slot, sizeof(uintptr_t), PROT_READ | PROT_WRITE) != 0) return;
    *(uintptr_t *)slot = val;
    mod_prot(slot, sizeof(uintptr_t), PROT_READ);
}

/* ================================================================== */
/* Inline function hooking                                            */
/* ================================================================== */

/* An absolute branch, in the four words the displaced instructions vacate:
 * load the target from the two words after the branch, and go. X16 is the
 * procedure-call scratch register (IP0) -- it is what a linker veneer uses for
 * exactly this, so no live value can be in it at a function's first
 * instruction. */
static void mod_hook_branch(uint32_t *w, uintptr_t target)
{
    w[0] = 0x58000050u;               /* ldr x16, #8 */
    w[1] = 0xd61f0200u;               /* br  x16     */
    memcpy(&w[2], &target, sizeof(target));
}

/* Can this instruction be moved somewhere else and still mean the same thing?
 * Everything PC-relative cannot, and that is the entire hazard of displacing a
 * prologue: the bytes copy fine and then compute the wrong address. */
static const char *mod_hook_immovable(uint32_t w)
{
    if ((w & 0x1f000000u) == 0x10000000u) return "ADR/ADRP";
    if ((w & 0x7c000000u) == 0x14000000u) return "B/BL";
    if ((w & 0xff000010u) == 0x54000000u) return "B.cond";
    if ((w & 0x7e000000u) == 0x34000000u) return "CBZ/CBNZ";
    if ((w & 0x7e000000u) == 0x36000000u) return "TBZ/TBNZ";
    if ((w & 0x3b000000u) == 0x18000000u) return "LDR literal";
    return NULL;
}

/* Trampolines live in one page, bump-allocated. They are executed but never
 * written again after they are built, and there are a handful at most. */
#define MOD_TRAMP_BYTES (MOD_HOOK_BYTES + 16)

static uint8_t *g_tramp;
static size_t   g_tramp_used;

static uint8_t *mod_tramp_alloc(void)
{
    if (!g_tramp) {
        long r = syscall(SYS_mmap, 0L, (long)g_pagesize,
                         (long)(PROT_READ | PROT_WRITE | PROT_EXEC),
                         (long)(MAP_PRIVATE | MAP_ANONYMOUS), -1L, 0L);

        if (r < 0 && r > -4096)
            return NULL;
        g_tramp = (uint8_t *)r;
        g_tramp_used = 0;
    }
    if (g_tramp_used + MOD_TRAMP_BYTES > g_pagesize)
        return NULL;
    {
        uint8_t *p = g_tramp + g_tramp_used;

        g_tramp_used += MOD_TRAMP_BYTES;
        return p;
    }
}

int mod_patch_fn(const char *name, uintptr_t fn, void *hook, uintptr_t *tramp)
{
    uint32_t head[MOD_HOOK_WORDS];
    uint8_t *t;
    int i;

    if (!fn || !hook) {
        MDBG("%s: no address to hook\n", name);
        return -1;
    }
    if (mod_safe_read(fn, head, sizeof(head)) != 0) {
        MDBG("%s: %#lx unreadable -> skip\n", name, (unsigned long)fn);
        return -1;
    }
    for (i = 0; i < MOD_HOOK_WORDS; i++) {
        const char *why = mod_hook_immovable(head[i]);

        if (why) {
            MDBG("%s: word %d of %#lx is %s -- cannot be displaced -> skip\n",
                 name, i, (unsigned long)fn, why);
            return -1;
        }
    }
    /* Journalled before the write, like every other patch here: code that
     * cannot be recorded is code that could never be put back. */
    if (g_npatch >= MOD_PATCH_MAX) {
        MDBG("%s: patch journal full (%d entries) -> skip\n", name, MOD_PATCH_MAX);
        return -1;
    }
    t = mod_tramp_alloc();
    if (!t) {
        MDBG("%s: no trampoline memory -> skip\n", name);
        return -1;
    }

    /* The trampoline first: the stock prologue, then back into the original
     * just past what we are about to overwrite. */
    memcpy(t, head, sizeof(head));
    mod_hook_branch((uint32_t *)(t + MOD_HOOK_BYTES), fn + MOD_HOOK_BYTES);
    __builtin___clear_cache((char *)t, (char *)t + MOD_TRAMP_BYTES);

    if (mod_prot(fn, MOD_HOOK_BYTES, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        MDBG("%s: mprotect RWX at %#lx failed errno=%d -> skip\n",
             name, (unsigned long)fn, errno);
        return -1;
    }
    mod_hook_branch((uint32_t *)fn, (uintptr_t)hook);
    mod_prot(fn, MOD_HOOK_BYTES, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)fn, (char *)fn + MOD_HOOK_BYTES);

    g_patch[g_npatch].slot  = fn;
    g_patch[g_npatch].orig  = fn;
    g_patch[g_npatch].owner = g_patch_owner;
    g_patch[g_npatch].is_fn = 1;
    memcpy(g_patch[g_npatch].code, head, sizeof(head));
    g_npatch++;

    if (tramp) *tramp = (uintptr_t)t;
    MDBG("%s: %#lx -> hook %p (trampoline %p)\n",
         name, (unsigned long)fn, hook, (void *)t);
    return 0;
}

void mod_restore_code(uintptr_t fn, const uint8_t *code, size_t n)
{
    if (mod_prot(fn, n, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) return;
    memcpy((void *)fn, code, n);
    mod_prot(fn, n, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)fn, (char *)fn + n);
}

/* ================================================================== */
/* Persisted MOD SETTINGS                                             */
/* ================================================================== */

/*
 * /home/root/settings is /dev/mmcblk1p7 (ext4, rw), the only writable mount that
 * survives a reboot; / is a ramdisk. It already holds the device's own CDJ3K_*.DAT
 * files with .BAK siblings.
 *
 * Not on inserted media: EP122 starts before anything is mounted, and removing the
 * stick mid-set would change the UI.
 *
 * One fixed binary record, like the stock CDJ3K_*.DAT files beside it. There is no
 * parser: reading is sizeof(struct) plus four header checks, writing is the same
 * bytes back. The load path logs every value it read, since the file is not
 * greppable.
 */
#define MOD_SET_DIR  "/home/root/settings"
#define MOD_SET_PATH MOD_SET_DIR "/CDJ3K_MODSETTINGS.DAT"
#define MOD_SET_TMP  MOD_SET_PATH ".new"
#define MOD_SET_BAK  MOD_SET_PATH ".BAK"

#define MOD_SET_MAGIC   0x53444f4du   /* "MODS" */
#define MOD_SET_VERSION 1

/*
 * The on-disk record. Rules for changing it:
 *
 *   - A new setting takes bytes from `reserved`, from the front. `size` does not
 *     change, so older builds still read and validate the record and ignore the
 *     new field.
 *   - Moving or resizing an existing field bumps `version`; older builds then
 *     reject the record rather than misread it.
 *
 * Fixed-width types throughout -- this is a byte layout, and `int` is not a
 * width. The static assertions below enforce it at build time.
 */
struct mod_settings_v1 {
    uint32_t magic;                    /* MOD_SET_MAGIC   */
    uint16_t version;                  /* MOD_SET_VERSION */
    uint16_t size;                     /* sizeof(*this): a longer record is refused */
    uint8_t  gate;
    uint8_t  stems;
    uint8_t  stem_manual;
    uint8_t  smart;
    uint32_t theme_id;                 /* an index, stored at a fixed width       */
    char     sep_id[STEM_SEP_ID_MAX];
    char     stem_addr[STEM_ADDR_MAX];
    /* Stored INVERTED. A byte taken from `reserved` reads 0 out of every record
     * written before it existed, so 0 has to mean whatever the setting's default
     * was when the field was added -- and preview hot cue defaulted ON then.
     *
     * It no longer does: every setting now ships off (see mods_init). The polarity
     * stays anyway, because it describes records already written and flipping it
     * would silently re-read a DJ's saved ON as OFF. Nothing new needs it -- with
     * every default off, 0 is the right answer for any field taken from `reserved`
     * from here on, which is what `xpad` below already relies on. */
    uint8_t  preview_off;              /* from reserved, front */
    /* ENABLE X-PAD. Taken from `reserved`, and it defaults OFF -- so 0, which is
     * what every record written before this field existed reads back, is already
     * the right answer and it needs no inversion the way preview_off did. */
    uint8_t  xpad;
    uint8_t  reserved[62];
    uint32_t crc32;                    /* over every byte above                   */
};

/* No implicit padding: the CRC covers every byte, so a hole would make a record
 * written by one compiler fail on another. Both string fields are multiples of
 * four today; these catch it if one stops being. */
_Static_assert(sizeof(struct mod_settings_v1) == 220, "settings record resized");
_Static_assert(offsetof(struct mod_settings_v1, crc32) == 216, "settings record has padding");
_Static_assert(offsetof(struct mod_settings_v1, theme_id) == 12, "flags moved");
_Static_assert(STEM_SEP_ID_MAX % 4 == 0, "sep_id would pad");
_Static_assert(STEM_ADDR_MAX  % 4 == 0, "stem_addr would pad");

/* CRC-32 (IEEE, reflected), bitwise -- 216 bytes twice per write does not justify
 * a 1 KiB table. */
static uint32_t mod_crc32(const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t c = 0xffffffffu;
    size_t i;
    int k;

    for (i = 0; i < len; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(c & 1u));
    }
    return c ^ 0xffffffffu;
}

/* Whether `id` is a string this can store and use as a directory name.
 *
 * stemd guarantees [A-Za-z0-9._-] and at most 31 bytes -- see "Identifiers" in
 * its docs/api.md -- so this does not reshape the value, only decide whether it
 * meets that contract. Reading is bounded: the source may be a fixed-width field
 * off a disk or off the socket, so a missing terminator is one of the things
 * being checked rather than something assumed away.
 *
 * "." and ".." are rejected despite being legal characters: they are the two
 * values that are a valid string but not a usable path component. */
static int sep_id_is_valid(const char *id)
{
    size_t n;

    if (!id)
        return 0;
    for (n = 0; n < STEM_SEP_ID_MAX; n++) {
        char c = id[n];

        if (c == '\0')
            break;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_'))
            return 0;
    }
    if (n == 0 || n >= STEM_SEP_ID_MAX)
        return 0;                  /* empty, or no terminator within the field */
    if (id[0] == '.' && (n == 1 || (n == 2 && id[1] == '.')))
        return 0;
    return 1;
}

/* The separator's own storage identity.
 *
 * A value that fails the contract is refused rather than repaired: the server
 * already enforces it, so anything else means we are not talking to a stemd that
 * meets it, and quietly reshaping the id would hide that while still writing
 * directories under whatever came out. Refusing leaves the id empty, which
 * entry_path() already reads as "no server has identified itself" -- the cache
 * turns off instead of filing entries under a name nobody chose. */
void mods_set_sep_id(const char *id)
{
    if (!sep_id_is_valid(id)) {
        MDBG("sep_id: refused \"%.*s\" -> cache off\n",
             STEM_SEP_ID_MAX - 1, id ? id : "(null)");
        g_stem_sep_id[0] = '\0';
        return;
    }
    snprintf(g_stem_sep_id, sizeof(g_stem_sep_id), "%s", id);
}

void mods_settings_load(void)
{
    struct mod_settings_v1 s;
    int fd = open(MOD_SET_PATH, O_RDONLY | O_CLOEXEC);
    ssize_t n;

    if (fd < 0) {                       /* first boot with the mod: keep the defaults */
        MDBG("settings: none at %s -> defaults (gate=%d smart=%d theme=%s stems=%d)\n",
             MOD_SET_PATH, g_gate_on, g_smart_on, mod_theme_name(g_theme_id), g_stems_on);
        return;
    }
    n = read(fd, &s, sizeof(s));
    close(fd);

    /* Any check failing leaves every compiled default in place: a record that does
     * not validate as a whole says nothing about its individual fields. */
    if (n != (ssize_t)sizeof(s)) {
        MWARN("settings: %s is %d bytes, want %d -> defaults\n",
             MOD_SET_PATH, (int)n, (int)sizeof(s));
        return;
    }
    if (s.magic != MOD_SET_MAGIC || s.size != sizeof(s)) {
        MDBG("settings: magic/size mismatch (%#x/%u) -> defaults\n",
             s.magic, s.size);
        return;
    }
    if (s.version != MOD_SET_VERSION) {
        /* A build that moved a field. Refusing costs the saved settings once;
         * reading it anyway would configure the deck from other fields' bytes. */
        MDBG("settings: version %u, this build speaks %u -> defaults\n",
             s.version, MOD_SET_VERSION);
        return;
    }
    if (mod_crc32(&s, offsetof(struct mod_settings_v1, crc32)) != s.crc32) {
        MWARN("settings: CRC mismatch -> defaults (try %s)\n", MOD_SET_BAK);
        return;
    }

    g_gate_on     = s.gate ? 1 : 0;
    g_smart_on    = s.smart ? 1 : 0;
    g_preview_on  = s.preview_off ? 0 : 1;
    g_stems_on    = s.stems ? 1 : 0;
    xpad_g_on     = s.xpad ? 1 : 0;
    g_stem_manual = s.stem_manual ? 1 : 0;
    /* Stored by index, so the registry's ORDER is part of the on-disk format:
     * append themes, never insert or reorder. */
    g_theme_id    = (s.theme_id < (uint32_t)MOD_THEME_MAX) ? (int)s.theme_id : 0;

    /* Off a disk: neither is trusted to be terminated. */
    s.sep_id[STEM_SEP_ID_MAX - 1]  = '\0';
    s.stem_addr[STEM_ADDR_MAX - 1] = '\0';
    /* Terminated above, because mods_set_sep_id() reads it as a C string. */
    mods_set_sep_id(s.sep_id);
    memcpy(g_stem_addr, s.stem_addr, STEM_ADDR_MAX);

    MDBG("settings: loaded gate=%d smart=%d preview=%d theme=%d(%s) xpad=%d stems=%d stemloc=%d sepid=\"%s\" stemaddr=\"%s\"\n",
         g_gate_on, g_smart_on, g_preview_on, g_theme_id, mod_theme_name(g_theme_id),
         xpad_g_on, g_stems_on, g_stem_manual, g_stem_sep_id, g_stem_addr);
}

void mods_settings_save(void)
{
    struct mod_settings_v1 s;
    int fd;

    /* Zeroed whole: the CRC covers `reserved` and `pad0`, so they must be defined
     * bytes rather than stack residue. */
    memset(&s, 0, sizeof(s));
    s.magic       = MOD_SET_MAGIC;
    s.version     = MOD_SET_VERSION;
    s.size        = (uint16_t)sizeof(s);
    s.gate        = g_gate_on ? 1u : 0u;
    s.smart       = g_smart_on ? 1u : 0u;
    s.preview_off = g_preview_on ? 0u : 1u;
    s.xpad        = xpad_g_on ? 1u : 0u;
    s.stems       = g_stems_on ? 1u : 0u;
    s.stem_manual = g_stem_manual ? 1u : 0u;
    s.theme_id    = (uint32_t)g_theme_id;
    /* The string, not the buffer: both globals keep bytes after their terminator
     * from any longer earlier value, which would make the CRC depend on them. The
     * memset above supplied the padding and the terminator. */
    memcpy(s.sep_id, g_stem_sep_id, strnlen(g_stem_sep_id, STEM_SEP_ID_MAX - 1));
    memcpy(s.stem_addr, g_stem_addr, strnlen(g_stem_addr, STEM_ADDR_MAX - 1));
    s.crc32       = mod_crc32(&s, offsetof(struct mod_settings_v1, crc32));

    /* Temp file, fsync, swap: a power cut can lose the new value but never leave a
     * half-written one, and the previous copy stays as .BAK. */
    fd = open(MOD_SET_TMP, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        MERR("settings: cannot write %s (errno=%d) -> not persisted\n", MOD_SET_TMP, errno);
        return;
    }
    {
        ssize_t n = write(fd, &s, sizeof(s));

        if (n != (ssize_t)sizeof(s)) {
            /* SAY WHICH, because the two read identically from the DJ's side and
             * only one of them is a bug of ours. A full partition returns 0 with
             * ENOSPC, and this is a 57 MB eMMC partition that EP122 also drops
             * multi-megabyte crash logs onto -- measured full, and every setting
             * changed for a day after that was silently discarded. */
            MERR("settings: wrote %d of %d bytes to %s (errno=%d)%s"
                 " -> NOT PERSISTED\n",
                 (int)n, (int)sizeof(s), MOD_SET_TMP, errno,
                 errno == ENOSPC || n == 0 ? " -- the partition is FULL" : "");
            close(fd);
            unlink(MOD_SET_TMP);
            return;
        }
    }
    fsync(fd);
    close(fd);

    rename(MOD_SET_PATH, MOD_SET_BAK);          /* no-op on first save */
    if (rename(MOD_SET_TMP, MOD_SET_PATH) != 0) {
        MDBG("settings: rename failed (errno=%d)\n", errno);
        unlink(MOD_SET_TMP);
        return;
    }
    MDBG("settings: saved gate=%d smart=%d preview=%d theme=%d(%s) xpad=%d stems=%d stemloc=%d stemaddr=\"%s\"\n",
         g_gate_on, g_smart_on, g_preview_on, g_theme_id, mod_theme_name(g_theme_id),
         xpad_g_on, g_stems_on, g_stem_manual, g_stem_addr);
}

/* ================================================================== */
/* Constructor                                                        */

/* ================================================================== */

static void __attribute__((constructor)) mods_init(void)
{
    const char *bad_level = NULL;

    /* Read before anything can log. A value that names no level leaves the level
     * at ERROR and is REPORTED -- but not from here: this constructor also runs
     * in apl_start.sh and every shell helper it spawns, so complaining at this
     * point says it once per process. It waits for the deck gate below. */
    {
        const char *lvl = getenv("EP122_MOD_LOGLEVEL");
        int         n   = log_level_from(lvl);

        g_mod_log = n < 0 ? MOD_LOG_ERROR : n;
        if (n < 0)
            bad_level = lvl;
    }

    /* Defaults, until the saved settings are read below.
     *
     * EVERY SETTING SHIPS OFF, and that is the rule rather than the sum of seven
     * separate judgements. A deck with the package on it and nothing switched on
     * behaves exactly like a deck without it -- so the first thing a DJ can check,
     * before trusting any of this on a live set, is that nothing has moved. Gate cue
     * and preview hot cue used to opt themselves in on the grounds that neither
     * changes a stored value; that is true and it is still a change to how the deck
     * answers a press, made by us and not by them.
     *
     * Written out rather than left to the BSS, because "off" being the default is a
     * decision and this is where it is stated. The rest of the settings are zero,
     * and zero is off for all of them: THEME 0 is ORIGINAL, which is the absence of
     * a theme, and STEM SERVER LOCATION 0 is AUTO, which asks nothing of the DJ. */
    g_gate_on    = 0;
    g_smart_on   = 0;
    g_preview_on = 0;
    g_stems_on   = 0;
    xpad_g_on    = 0;
    g_theme_id   = 0;

    g_pagesize = sysconf(_SC_PAGESIZE);
    if (g_pagesize <= 0) g_pagesize = 4096;

    /* Cheap first: the shim is preloaded into apl_start.sh and every shell helper
     * it spawns, none of which should pay for a 45 MB scan. */
    if (!ep122_image_is_deck())
        return;

    /* Past the gate, so this is the deck and it is said once. Silence would give
     * someone who typed "verbose" exactly what an unset variable gives, with no
     * way to tell the two apart. */
    if (bad_level)
        MERR("EP122_MOD_LOGLEVEL=\"%s\" names no level "
             "(error|warn|info|debug|trace, or 0-4); staying at error\n",
             bad_level);

    /* Also below the gate, and for the same reason: this constructor runs in every
     * shell helper apl_start.sh spawns, so a notice above the gate is a notice ~180
     * times. Nothing is remembered between boots -- there is no per-mod environment
     * switch: what a DJ turns off lives in MOD SETTINGS, what a developer turns off
     * lives in a build. This takes the mods out without taking the shim out, which
     * dropping LD_PRELOAD cannot do: the shim is also the guest's time-shift, jog
     * and DRM plumbing. */
    if (getenv("EP122_NO_MODS")) {
        MINFO("init: EP122_NO_MODS set -> mods not installed\n");
        return;
    }

    /* Resolving is also the detection: it asserts the process contains ~130 named
     * classes, not that one address holds a common prologue. */
    ep122_resolve();
    if (!ep122_sym(EP122_DJSET_NUMROWS))
        return;   /* big enough to be the deck, but not the deck */

    /* Every symbol in the spec resolved, or no mod runs. One symbol used by one
     * feature takes all of them down: this is the intended trade, since "mostly
     * modded" on an uncharacterised firmware is not a supportable state. The
     * refusal names what it could not find. */
    if (ep122_resolve_missing()) {
        MERR("init: %d/%d symbols unresolved -> mods INACTIVE (deck runs stock)\n",
             ep122_resolve_missing(), EP122_SYM__COUNT);
        ep122_resolve_log_missing();
        return;
    }

    /* One line per EP122 start, naming the build. The gates above already returned
     * for every shell helper. INFO rather than unconditional: a working deck should
     * not talk, and everything that reports a FAILURE above this is louder. */
    MINFO("init: mods %s (build %s), all %d symbols resolved\n",
          EP122_MOD_VERSION, EP122_MOD_BUILD, EP122_SYM__COUNT);

    /* The MOD SETTINGS rows own their state from here. */
    mods_settings_load();

    mods_install_all();
}

/* ================================================================== */
/* The mods                                                           */
/* ================================================================== */

/*
 * Every mod that declared a KIT_MOD descriptor (kit/mod.h). Nothing is listed
 * here: the linker collects the descriptors into the `ep122_mods` section and
 * this walks it.
 *
 * Link order is the Makefile's wildcard, i.e. whatever order the filesystem
 * hands back, so the section is sorted by (prio, name) before anything runs.
 * mods_settings_load() has already run: stem_job reads the saved settings.
 */
#define MOD_MAX 32

static int mod_before(const struct kit_mod *a, const struct kit_mod *b)
{
    if (a->prio != b->prio) return a->prio < b->prio;
    return strcmp(a->name, b->name) < 0;
}

void mods_install_all(void)
{
    const struct kit_mod *ord[MOD_MAX];
    const struct kit_mod *m;
    char line[256];
    int n = 0, i, len = 0, ok = 0;

    for (m = __start_ep122_mods; m < __stop_ep122_mods; m++) {
        int j;

        if (n >= MOD_MAX) {
            MERR("install: %d mods fit; \"%s\" and any after it are NOT "
                 "installed\n", MOD_MAX, m->name);
            break;
        }
        for (j = n; j > 0 && mod_before(m, ord[j - 1]); j--)
            ord[j] = ord[j - 1];
        ord[j] = m;
        n++;
    }

    for (i = 0; i < n; i++) {
        int r;

        /* Stamped here; mods never name their own patches. */
        mod_patch_owner(ord[i]->name);
        r = ord[i]->install();
        if (r == 0) {
            ok++;
        } else {
            /* A refusal can leave slots repointed; the journal knows which. */
            int undone = mod_unpatch_owner(ord[i]->name);

            if (undone)
                MDBG("install: %s refused -> %d slot(s) put back\n",
                     ord[i]->name, undone);
        }
        if (len < (int)sizeof(line) - 1)
            len += snprintf(line + len, sizeof(line) - (size_t)len, "%s%s%s",
                            i ? " " : "", r == 0 ? "" : "-", ord[i]->name);
    }
    mod_patch_owner(NULL);

    /* Every mod, '-' in front of the ones that did not go in. A PARTIAL install is a
     * warning, because some feature the DJ switched on is simply absent and the '-'
     * names it; a full one is information, so a healthy deck stays silent at the
     * default level. */
    if (ok < n)
        MWARN("install: %d/%d [%s]\n", ok, n, line);
    else
        MINFO("install: %d/%d [%s]\n", ok, n, line);
}

