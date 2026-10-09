#!/usr/bin/env python3
"""Turn core/ep122_syms.spec into core/ep122_syms.h.

EP122's internals are named by identity, not by address: RTTI class names for
vtables, masked instruction signatures for free functions. This emits the table
the runtime resolver (core/resolve.c) walks at startup, so a mod is not pinned
to one firmware build.

Everything is resolved here, against the stock binary, so a spec that cannot
be resolved fails on the Mac. What ships is the description, not the answer: the
addresses in the generated header are comments, and the deck recomputes them
against whatever binary it is actually running.

    tools/gen-syms.py            # regenerate + verify
    tools/gen-syms.py --check    # verify only, non-zero if stale

The stock binary comes from $EP122 and must be the deck's own copy. It is not
in this repository.
"""

import argparse
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BIN = os.environ.get("EP122")
DEFAULT_ALT = os.environ.get("EP122_RENESAS")
SPEC = os.path.join(ROOT, "mods", "core", "ep122_syms.spec")
HEADER = os.path.join(ROOT, "mods", "core", "ep122_syms.h")

# Signature lengths tried, shortest first; past the last one an entry is
# ambiguous. 64 instructions is well past any shared prologue and still a cheap
# memcmp.
SIG_LENGTHS = [8, 12, 16, 24, 32, 48, 64, 96, 128]

# sig_matches() stops collecting here. A full list only means the window is too
# common: grow it, and do not read the truncation as a missing self-match.
SIG_MATCH_LIMIT = 64


# --------------------------------------------------------------------------
# ELF
# --------------------------------------------------------------------------

class Elf:
    """Just enough ELF64-LE to map virtual addresses onto file offsets."""

    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        if self.data[:4] != b"\x7fELF":
            raise SystemExit(f"{path}: not an ELF")
        e_phoff, = struct.unpack_from("<Q", self.data, 0x20)
        e_phentsize, e_phnum = struct.unpack_from("<HH", self.data, 0x36)
        self.segs = []
        for i in range(e_phnum):
            p = e_phoff + i * e_phentsize
            p_type, p_flags = struct.unpack_from("<II", self.data, p)
            if p_type != 1:  # PT_LOAD
                continue
            p_offset, p_vaddr = struct.unpack_from("<QQ", self.data, p + 0x08)
            p_filesz, p_memsz = struct.unpack_from("<QQ", self.data, p + 0x20)
            self.segs.append((p_vaddr, p_memsz, p_offset, p_filesz, p_flags))
        self.segs.sort()
        text = [s for s in self.segs if s[4] & 1]
        if len(text) != 1:
            raise SystemExit(f"{path}: expected one executable PT_LOAD, got {len(text)}")
        self.text_va, _, self.text_off, self.text_sz, _ = text[0]

    def off(self, va):
        for vaddr, memsz, offset, filesz, _ in self.segs:
            if vaddr <= va < vaddr + memsz:
                d = va - vaddr
                return offset + d if d < filesz else None
        return None

    def q(self, va):
        o = self.off(va)
        if o is None:
            return None
        return struct.unpack_from("<Q", self.data, o)[0]

    def w32(self, va):
        o = self.off(va)
        if o is None:
            return None
        return struct.unpack_from("<I", self.data, o)[0]

    def is_text(self, va):
        """Is `va` inside the one executable segment? True of a function, false
        of a string, a relocation table or a stray word that happens to be
        8-aligned next to the right pointer."""
        return va is not None and self.text_va <= va < self.text_va + self.text_sz

    def cstr(self, va, limit=512):
        o = self.off(va)
        if o is None:
            return None
        end = self.data.find(b"\0", o, o + limit)
        return self.data[o:end if end >= 0 else o + limit]

    def find_ptrs(self, target):
        """Every mapped, 8-aligned address holding a pointer to `target`."""
        needle = struct.pack("<Q", target)
        hits = []
        for vaddr, _, offset, filesz, _ in self.segs:
            i = self.data.find(needle, offset, offset + filesz)
            while i >= 0:
                if (i - offset) % 8 == 0:
                    hits.append(vaddr + (i - offset))
                i = self.data.find(needle, i + 1, offset + filesz)
        return hits

    def find_bytes(self, needle):
        """Every mapped address at which `needle` appears."""
        hits = []
        for vaddr, _, offset, filesz, _ in self.segs:
            i = self.data.find(needle, offset, offset + filesz)
            while i >= 0:
                hits.append(vaddr + (i - offset))
                i = self.data.find(needle, i + 1, offset + filesz)
        return hits


# --------------------------------------------------------------------------
# RTTI
# --------------------------------------------------------------------------

class Rtti:
    """Itanium type_info walking, keyed on the RAW mangled name.

    The demangled form is for humans; the bytes in the image are what we match,
    so there is no grammar to get wrong and no ambiguity to resolve.
    """

    def __init__(self, elf, seed_names):
        self.elf = elf
        self._kinds = None
        self._seeds = list(seed_names)

    def _looks_like_ti(self, x):
        """A type_info head: 8-aligned, mapped, and carrying a name string."""
        if not x or x % 8 or self.elf.off(x) is None:
            return False
        p = self.elf.q(x + 8)
        if not p or self.elf.off(p) is None:
            return False
        s = self.elf.cstr(p, 64)
        return bool(s) and all(0x20 <= c < 0x7F for c in s[:8])

    def _candidate_vptrs(self):
        """Every distinct vptr worn by a type_info reachable from a seed.

        __cxxabiv1's own classes carry no RTTI in this binary -- the three
        `N10__cxxabiv1..._type_infoE` strings that are present belong to the
        demangler and nothing points at them -- so the flavour vptrs cannot be
        found by name. They do not have to be: a type_info's first word IS its
        flavour vptr, so any class the spec asks for yields one, and walking
        the base graph from there reaches the rest. Both base layouts are tried
        on every record; a wrong reading yields addresses that fail
        _looks_like_ti and simply contributes nothing.
        """
        todo, seen, vptrs = [], set(), {}
        for nm in self._seeds:
            want = nm.encode() if isinstance(nm, str) else nm
            for sva in self.elf.find_bytes(want + b"\0"):
                todo += [p - 8 for p in self.elf.find_ptrs(sva)]
        while todo:
            ti = todo.pop()
            if ti in seen or not self._looks_like_ti(ti):
                continue
            seen.add(ti)
            v = self.elf.q(ti)
            if v:
                vptrs[v] = vptrs.get(v, 0) + 1
            nxt = self.elf.q(ti + 16)
            if nxt is None:
                continue
            todo.append(nxt)                                    # as __si_...
            cnt, flags = nxt >> 32, nxt & 0xFFFFFFFF            # as __vmi_...
            if 1 <= cnt <= 64 and flags < 0x20:
                todo += [self.elf.q(ti + 24 + i * 16) or 0 for i in range(cnt)]
        return vptrs

    def kinds(self):
        """vptr value -> flavour, so a type_info head can be recognised.

        Flavour comes from layout, not from a name. Past the 16-byte head:
        __si_class_type_info holds one type_info pointer; __vmi_class_type_info
        holds (flags:u32, base_count:u32); __class_type_info holds nothing, so
        whatever follows is the next record and satisfies neither rule. Every
        record of a flavour has to agree, which is what makes this a check
        rather than a guess.
        """
        if self._kinds is not None:
            return self._kinds

        cands = self._candidate_vptrs()
        if not cands:
            raise SystemExit("RTTI bootstrap: no spec class name resolved to a type_info")
        pool = set(cands)
        kinds = {}
        for v in sorted(cands, key=lambda k: -cands[k]):
            # find_ptrs matches any qword, and a vtable address point is also
            # referenced from places that are not type_info heads; keep only
            # the ones that carry a name.
            recs = [p for p in self.elf.find_ptrs(v) if self._looks_like_ti(p)][:64]
            # A real flavour vptr is used by hundreds of type_infos. Reading a
            # base-less record's tail as a base pointer occasionally lands on
            # something that passes _looks_like_ti, but such a stray is used by
            # a single record, so the count filters it out.
            if len(recs) < 4:
                continue
            si = vmi = 0
            for ti in recs:
                nxt = self.elf.q(ti + 16)
                if nxt is None:
                    continue
                if self.elf.off(nxt) is not None and self.elf.q(nxt) in pool:
                    si += 1
                if 1 <= (nxt >> 32) <= 64 and (nxt & 0xFFFFFFFF) < 0x20:
                    vmi += 1
            # __si_ tolerates a few misses: a base can be a pointer or
            # fundamental type_info, whose flavour vptr is not one of the three
            # class ones and so is not in the pool. __vmi_ and the base-less
            # flavour must be unanimous.
            n = len(recs)
            if vmi == n and si == 0:
                kinds[v] = "vmi"
            elif si * 10 >= n * 9 and vmi == 0:
                kinds[v] = "si"
            elif si == 0 and vmi == 0:
                kinds[v] = "class"
            else:
                raise SystemExit(f"RTTI bootstrap: vptr 0x{v:x} is {si}/{n} si "
                                 f"and {vmi}/{n} vmi -- cannot tell them apart")
        if sorted(kinds.values()) != ["class", "si", "vmi"]:
            raise SystemExit(
                "RTTI bootstrap: expected one vptr per flavour, got "
                + repr({hex(k): v for k, v in kinds.items()}))
        self._kinds = kinds
        return kinds

    def kind(self, ti):
        v = self.elf.q(ti)
        return self.kinds().get(v)

    def name(self, ti):
        p = self.elf.q(ti + 8)
        return self.elf.cstr(p) if p else None

    def find_class(self, mangled):
        """type_info address for an exact mangled name."""
        want = mangled.encode() if isinstance(mangled, str) else mangled
        hits = []
        for sva in self.elf.find_bytes(want + b"\0"):
            # must be the start of the string, not a suffix of a longer one
            if self.elf.data[self.elf.off(sva) - 1:self.elf.off(sva)] not in (b"\0", b""):
                prev = self.elf.data[self.elf.off(sva) - 1]
                if 0x20 <= prev < 0x7F:
                    continue
            for p in self.elf.find_ptrs(sva):
                ti = p - 8
                if self.kind(ti):
                    hits.append(ti)
        return sorted(set(hits))

    def bases(self, ti):
        """(type_info, offset, virtual) for each direct base.

        For a VIRTUAL base the third field is True and the second is NOT a
        layout offset: the ABI cannot store one, because where a virtual base
        sits depends on the complete object rather than on this class. What it
        stores instead is where in the vtable the real offset lives, which is
        what base_offset goes and reads.
        """
        k = self.kind(ti)
        if k == "si":
            b = self.elf.q(ti + 16)
            return [(b, 0, False)] if b else []
        if k == "vmi":
            n = self.elf.w32(ti + 20)
            if n is None or n > 64:
                return []
            out = []
            for i in range(n):
                p = ti + 24 + i * 16
                b = self.elf.q(p)
                off_flags = self.elf.q(p + 8)
                if not b:
                    continue
                off = off_flags >> 8
                if off & (1 << 55):
                    off -= 1 << 56
                out.append((b, off, bool(off_flags & 1)))
            return out
        return []

    def object_vtables(self, ti):
        """The vtables an OBJECT of `ti` ever points at: slot 0 holds code.

        `vtables` matches on a pointer to the typeinfo, and two other things
        carry one. A CONSTRUCTION VTABLE -- emitted per base while a class with
        a virtual base is being built -- has the same typeinfo AND the same
        offset-to-top as the real one, so it is pure ambiguity: "2 vtables at
        offset-to-top 0" is what it looks like from the caller. The ABI leaves
        its destructor slots null, because a half-built object cannot be
        deleted. The other is a plain false positive, an aligned word beside a
        string or a relocation table, whose slots are not code at all.

        SLOT 0 ONLY, deliberately. An interface with no virtual destructor puts
        its first method there and can be a single entry long -- reading slot 1
        of gui::SoftwareKeyboardPopupWidget::IListener's vtable lands on the
        next vtable's offset-to-top, which is not code, and threw out a vtable
        the mods use.
        """
        return [(v, top) for v, top in self.vtables(ti)
                if self.elf.is_text(self.elf.q(v))]

    def base_offset(self, ti, base_name, depth=0, at=0, whole=None):
        """Offset of `base_name`'s subobject inside a complete `whole` object,
        given that `ti` sits at `at` within it. None if it is not a base.

        A virtual edge is read out of the vtable, and only while `at` is still
        0: the offset is recorded relative to the vptr of the class that
        DECLARES the virtual base, so reaching it needs that class to share the
        complete object's vptr. Every case here does -- juce::Component is
        virtual in gui::WidgetBase, which every gui widget derives from through
        its primary base chain -- and a case that does not comes back as "not a
        base" rather than as a wrong number.
        """
        if depth > 8:
            return None
        if whole is None:
            whole = ti
        want = base_name.encode() if isinstance(base_name, str) else base_name
        for b, off, virtual in self.bases(ti):
            if virtual:
                if at != 0:
                    continue
                vts = [v for v, top in self.object_vtables(whole) if top == 0]
                if len(vts) != 1:
                    continue
                off = self.elf.q(vts[0] + off)
                if off is None:
                    continue
                if off & (1 << 63):
                    off -= 1 << 64
            if self.name(b) == want:
                return off
            sub = self.base_offset(b, base_name, depth + 1, at + off, whole)
            if sub is not None:
                return off + sub
        return None

    def vtables(self, ti):
        """(address point, offset-to-top) for every vtable naming `ti`."""
        out = []
        for p in self.elf.find_ptrs(ti):
            top = self.elf.q(p - 8)
            if top is None:
                continue
            s = top - (1 << 64) if top >> 63 else top
            if -0x100000 < s <= 0:
                out.append((p + 8, s))
        return sorted(out)


# --------------------------------------------------------------------------
# aarch64 signatures
# --------------------------------------------------------------------------

def mask_word(w, adrp_regs):
    """Mask for one instruction: 1 bits are compared, 0 bits are ignored.

    Only PC- and data-relative fields are dropped. Everything that encodes what
    the code DOES - registers, shifts, non-address immediates, and the intra-
    function branches (B.cond/CBZ/TBZ, which move with the function) - is kept,
    because that is what makes a signature mean something.
    """
    # B / BL: imm26 is where the callee ended up.
    if (w & 0x7C000000) == 0x14000000:
        return 0xFC000000
    # ADR / ADRP: both immediate halves point at a global that may have moved.
    if (w & 0x1F000000) == 0x10000000:
        if w & 0x80000000:          # ADRP: remember the register holding a page
            adrp_regs.add(w & 0x1F)
        return 0x9F00001F
    # LDR/LDRSW (literal): imm19 into the literal pool.
    if (w & 0x3B000000) == 0x18000000:
        return 0xFF00001F
    # ADD (immediate) on an ADRP result: the low half of the same reference.
    if (w & 0x7F800000) in (0x11000000, 0x91000000):
        rn = (w >> 5) & 0x1F
        if rn in adrp_regs:
            adrp_regs.add(w & 0x1F)
            return 0xFFC003FF
    # LDR/STR (unsigned immediate) through an ADRP result: likewise.
    if (w & 0x3B000000) == 0x39000000:
        if ((w >> 5) & 0x1F) in adrp_regs:
            return 0xFFC003FF
    return 0xFFFFFFFF


FNV64_OFF = 0xcbf29ce484222325
FNV64_PRIME = 0x100000001b3


def fnv1a64(data):
    """FNV-1a, because the resolver has to compute the same number in C and this
    is the shortest thing that is unambiguous to reimplement."""
    h = FNV64_OFF
    for b in data:
        h = ((h ^ b) * FNV64_PRIME) & 0xFFFFFFFFFFFFFFFF
    return h


def window_digest(elf, addr, n_insn):
    """Digest of the masked window, with every mask taken from the window's OWN
    instructions.

    The resolver recomputes this from whatever it is looking at, so nothing of
    the stock code needs to be carried in the tree -- not the bytes, and not
    the masks derived from them. Gives the same result as masking with the
    stock binary's own masks.
    """
    off = elf.off(addr)
    if off is None:
        return None
    out, adrp = bytearray(), set()
    for i in range(n_insn):
        w = struct.unpack_from("<I", elf.data, off + i * 4)[0]
        out += struct.pack("<I", w & mask_word(w, adrp))
    return fnv1a64(bytes(out))


def signature(elf, addr, n_insn):
    off = elf.off(addr)
    if off is None:
        return None
    raw = elf.data[off:off + n_insn * 4]
    if len(raw) < n_insn * 4:
        return None
    pat, msk, adrp = bytearray(), bytearray(), set()
    for i in range(n_insn):
        w = struct.unpack_from("<I", raw, i * 4)[0]
        mk = mask_word(w, adrp)
        pat += struct.pack("<I", w & mk)
        msk += struct.pack("<I", mk)
    return bytes(pat), bytes(msk)


def sig_matches(elf, pat, msk, limit=SIG_MATCH_LIMIT):
    """4-aligned .text addresses matching, anchored on a fully-kept word."""
    n = len(pat)
    anchor = next((i for i in range(0, n, 4)
                   if msk[i:i + 4] == b"\xff\xff\xff\xff"), None)
    if anchor is None:
        return None
    data = elf.data
    lo, hi = elf.text_off, elf.text_off + elf.text_sz - n
    needle = pat[anchor:anchor + 4]
    hits = []
    i = data.find(needle, lo + anchor, hi + anchor + 4)
    while i >= 0 and len(hits) < limit:
        start = i - anchor
        if start >= lo and (start - elf.text_off) % 4 == 0:
            cand = data[start:start + n]
            if len(cand) == n and all((cand[k] & msk[k]) == pat[k] for k in range(n)):
                hits.append(elf.text_va + (start - elf.text_off))
        i = data.find(needle, i + 1, hi + anchor + 4)
    return hits


def bl_targets(elf, start, span):
    """Every BL target inside [start, start+span), in program order."""
    out = []
    for i in range(0, span, 4):
        w = elf.w32(start + i)
        if w is None:
            break
        if (w & 0xFC000000) == 0x94000000:
            imm = w & 0x03FFFFFF
            if imm >> 25:
                imm -= 1 << 26
            out.append(start + i + imm * 4)
    return out


def adrp_pair_target(elf, fn, insn):
    """The address computed by the ADRP/ADD or ADRP/LDR pair at `insn`."""
    va = fn + insn * 4
    w = elf.w32(va)
    if w is None or (w & 0x9F000000) != 0x90000000:
        return None, f"instruction {insn} of 0x{fn:x} is not an ADRP"
    immlo = (w >> 29) & 3
    immhi = (w >> 5) & 0x7FFFF
    imm = (immhi << 2) | immlo
    if imm >> 20:
        imm -= 1 << 21
    page = (va & ~0xFFF) + imm * 4096
    rd = w & 0x1F
    # The ADD need not directly follow the ADRP: on the Renesas build
    # POPUP_CTOR's pair is eight instructions apart. Every completion found is
    # checked against the address the spec declares, so a wide search cannot
    # produce a wrong result.
    for k in range(1, 16):
        w2 = elf.w32(va + 4 * k)
        if w2 is None or ((w2 >> 5) & 0x1F) != rd:
            continue
        if (w2 & 0xFFC00000) == 0x91000000:                 # ADD  x, x, #imm
            return page + ((w2 >> 10) & 0xFFF), None
        if (w2 & 0xFFC00000) == 0xF9400000:                 # LDR  x, [x, #imm]
            return page + (((w2 >> 10) & 0xFFF) * 8), None
        if (w2 & 0xFFC00000) == 0xB9400000:                 # LDR  w, [x, #imm]
            return page + (((w2 >> 10) & 0xFFF) * 4), None
    return None, f"no ADRP pair completion within 16 instructions of 0x{va:x}"


def bl_target(elf, fn, insn):
    """The function the BL at `insn` of `fn` calls."""
    va = fn + insn * 4
    w = elf.w32(va)
    if w is None or (w & 0xFC000000) != 0x94000000:
        return None, f"instruction {insn} of 0x{fn:x} is not a BL"
    imm = w & 0x03FFFFFF
    if imm >> 25:
        imm -= 1 << 26
    return va + imm * 4, None


# --------------------------------------------------------------------------
# spec
# --------------------------------------------------------------------------

def parse_spec(path):
    entries = []
    with open(path) as f:
        for lineno, line in enumerate(f, 1):
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            kind, name, rest = parts[0], parts[1], parts[2:]
            kv = {}
            for p in rest:
                if "=" not in p:
                    raise SystemExit(f"{path}:{lineno}: bad token {p!r}")
                k, v = p.split("=", 1)
                kv[k] = v
            entries.append((lineno, kind, name, kv))
    return entries


def resolve(elf, rtti, entries, alt=None, rtti_alt=None):
    """Resolve every entry; returns (results, errors)."""
    out, errors = [], []
    by_name = {}
    # Same names resolved against the other processor's binary, so a from=/in=
    # parent can be followed there too. Empty when no second binary is given.
    by_name_alt = {}

    def fail(lineno, name, msg):
        errors.append(f"{SPEC}:{lineno}: {name}: {msg}")

    for lineno, kind, name, kv in entries:
        if kind == "vtable":
            cls = kv.get("class")
            if not cls:
                fail(lineno, name, "missing class=")
                continue
            tis = rtti.find_class(cls)
            if len(tis) != 1:
                fail(lineno, name, f"{len(tis)} type_info records for {cls!r}")
                continue
            ti = tis[0]
            want_top = 0
            base = kv.get("base")
            if base:
                off = rtti.base_offset(ti, base)
                if off is None:
                    fail(lineno, name, f"{base!r} is not a base of {cls!r}")
                    continue
                want_top = -off
            vts = [v for v, top in rtti.object_vtables(ti) if top == want_top]
            if len(vts) != 1:
                fail(lineno, name, f"{len(vts)} vtables at offset-to-top {want_top}")
                continue
            by_name[name] = vts[0]
            if rtti_alt is not None:
                atis = rtti_alt.find_class(cls)
                if len(atis) == 1:
                    atop = want_top
                    if base:
                        aoff = rtti_alt.base_offset(atis[0], base)
                        atop = -aoff if aoff is not None else None
                    if atop is not None:
                        avts = [v for v, t in rtti_alt.object_vtables(atis[0])
                                if t == atop]
                        if len(avts) == 1:
                            by_name_alt[name] = avts[0]
            out.append(dict(kind="vtable", name=name, addr=vts[0], cls=cls,
                            base=base))

        elif kind == "slot":
            vt = by_name.get(kv.get("vtable", ""))
            if vt is None:
                fail(lineno, name, f"unknown vtable={kv.get('vtable')!r}")
                continue
            off = int(kv["off"], 0)
            fn = elf.q(vt + off)
            if not fn or elf.off(fn) is None:
                fail(lineno, name, f"slot +0x{off:x} of 0x{vt:x} holds {fn and hex(fn)}")
                continue
            by_name[name] = fn
            if "ren" in kv:
                # Given explicitly, not derived. The child's ren_insn check
                # reads this address and compares the result with its own ren=,
                # so a wrong parent fails the build, not the deck.
                by_name_alt[name] = int(kv["ren"], 0)
            out.append(dict(kind="slot", name=name, addr=fn,
                            vtable=kv["vtable"], off=off))

        elif kind == "func":
            addr = int(kv["addr"], 0)
            scope = kv.get("in")
            chosen = None
            # `min=` forces a longer signature than uniqueness in the stock
            # application binary requires. A signature that is barely unique here can collide in a
            # neighbouring firmware; the resolver then sees a match count that
            # differs from the recorded one and refuses the symbol.
            floor = int(kv.get("min", "0"), 0)
            for n in [x for x in SIG_LENGTHS if x >= floor]:
                sig = signature(elf, addr, n)
                if not sig:
                    fail(lineno, name, "unreadable at 0x%x" % addr)
                    break
                hits = sig_matches(elf, *sig)
                if hits is None:
                    fail(lineno, name, "no unmasked anchor word")
                    break
                if len(hits) >= SIG_MATCH_LIMIT:
                    continue           # saturated: certainly not unique, grow it
                if addr not in hits:
                    fail(lineno, name, "signature does not match its own address")
                    break
                if scope:
                    chosen = (n, sig, hits)
                    break
                if len(hits) == 1:
                    chosen = (n, sig, hits)
                    break
            else:
                fail(lineno, name,
                     f"ambiguous at {SIG_LENGTHS[-1]} instructions "
                     f"({len(hits)} matches) - scope it with in=/nth=")
                continue
            if chosen is None:
                continue
            n, sig, hits = chosen
            if scope:
                anchor = by_name.get(scope)
                if anchor is None:
                    fail(lineno, name, f"unknown in={scope!r}")
                    continue
                span = int(kv.get("span", "0x400"), 0)
                nth = int(kv.get("nth", "0"), 0)
                targets = bl_targets(elf, anchor, span)
                matched = [t for t in targets if t in set(hits)]
                # de-duplicate while keeping call order
                seen, ordered = set(), []
                for t in matched:
                    if t not in seen:
                        seen.add(t)
                        ordered.append(t)
                if nth >= len(ordered) or ordered[nth] != addr:
                    fail(lineno, name,
                         f"nth={nth} of {len(ordered)} scoped matches is "
                         f"{ordered[nth] if nth < len(ordered) else None} "
                         f"not 0x{addr:x}")
                    continue
                by_name[name] = addr
                out.append(dict(kind="func", name=name, addr=addr, insn=n,
                                sig=sig, scope=scope, span=span, nth=nth,
                                total=len(ordered),
                                digest=window_digest(elf, addr, n)))
            else:
                by_name[name] = addr
                out.append(dict(kind="func", name=name, addr=addr, insn=n,
                                sig=sig, scope=None,
                                digest=window_digest(elf, addr, n)))

            # The same function on the other application processor.
            #
            # A CDJ-3000 .UPD carries two EP122 binaries built by different
            # compilers, so one signature cannot cover both. Neither variant's
            # window matches in the other image, so a symbol carries both
            # descriptors and the resolver keeps whichever hits its recorded
            # count. No variant detection is needed, and a miss is still a
            # refusal.
            if "ren" in kv:
                if alt is None:
                    fail(lineno, name,
                         "ren= given but $EP122_RENESAS is unset")
                    continue
                raddr = int(kv["ren"], 0)
                rfloor = int(kv.get("ren_min", "0"), 0)
                rscope = kv.get("ren_in")
                rchosen = None
                for rn_ in [x for x in SIG_LENGTHS if x >= rfloor]:
                    rsig = signature(alt, raddr, rn_)
                    if not rsig:
                        fail(lineno, name, "ren=0x%x unreadable" % raddr)
                        break
                    rhits = sig_matches(alt, *rsig)
                    if rhits is None:
                        fail(lineno, name, "ren= has no unmasked anchor word")
                        break
                    if len(rhits) >= SIG_MATCH_LIMIT:
                        continue       # saturated: certainly not unique, grow it
                    if raddr not in rhits:
                        fail(lineno, name,
                             "ren= signature does not match its own address")
                        break
                    if rscope:
                        # Scoped: position decides, not uniqueness. Siblings
                        # that differ only in the global they touch look
                        # identical once masked, so take the shortest signature
                        # and let the enclosing function separate them.
                        rchosen = (rn_, rsig, rhits)
                        break
                    if len(rhits) == 1:
                        rchosen = (rn_, rsig, rhits)
                        break
                else:
                    fail(lineno, name,
                         f"ren= ambiguous at {SIG_LENGTHS[-1]} instructions "
                         f"({len(rhits)} matches) - scope it with ren_in=/ren_nth=")
                    continue
                if rchosen is None:
                    continue
                rn_, rsig, rhits = rchosen
                by_name_alt[name] = raddr
                if rscope:
                    ranchor = by_name_alt.get(rscope)
                    if ranchor is None:
                        fail(lineno, name,
                             f"ren_in={rscope!r} has no Renesas address -- give it ren=")
                        continue
                    rspan = int(kv.get("ren_span", "0x400"), 0)
                    rnth = int(kv.get("ren_nth", "0"), 0)
                    rtargets = bl_targets(alt, ranchor, rspan)
                    rmatched = [t for t in rtargets if t in set(rhits)]
                    rseen, rordered = set(), []
                    for t in rmatched:
                        if t not in rseen:
                            rseen.add(t)
                            rordered.append(t)
                    if rnth >= len(rordered) or rordered[rnth] != raddr:
                        fail(lineno, name,
                             f"ren_nth={rnth} of {len(rordered)} scoped matches is "
                             f"{('0x%x' % rordered[rnth]) if rnth < len(rordered) else None} "
                             f"not 0x{raddr:x}")
                        continue
                    out.append(dict(kind="func_alt", name=name, addr=raddr,
                                    insn=rn_, sig=rsig, scope=rscope,
                                    span=rspan, nth=rnth, total=len(rordered),
                                    digest=window_digest(alt, raddr, rn_)))
                else:
                    out.append(dict(kind="func_alt", name=name, addr=raddr,
                                    insn=rn_, sig=rsig, scope=None,
                                    digest=window_digest(alt, raddr, rn_)))

        elif kind == "data":
            addr = int(kv["addr"], 0)
            src = by_name.get(kv.get("from", ""))
            if src is None:
                fail(lineno, name, f"unknown from={kv.get('from')!r}")
                continue
            insn = int(kv["insn"], 0)
            got, err = adrp_pair_target(elf, src, insn)
            if got is None:
                fail(lineno, name, err)
                continue
            if got != addr:
                fail(lineno, name,
                     f"pair at insn {insn} of {kv['from']} yields 0x{got:x}, "
                     f"spec says 0x{addr:x}")
                continue
            by_name[name] = addr
            out.append(dict(kind="data", name=name, addr=addr,
                            src=kv["from"], insn=insn))
            if "ren" in kv:
                asrc = by_name_alt.get(kv["from"])
                if alt is None or asrc is None:
                    fail(lineno, name,
                         "ren= needs $EP122_RENESAS and a from= that resolves there")
                    continue
                raddr, rinsn = int(kv["ren"], 0), int(kv["ren_insn"], 0)
                rgot, rerr = adrp_pair_target(alt, asrc, rinsn)
                if rgot != raddr:
                    fail(lineno, name,
                         f"ren_insn={rinsn} of {kv['from']} yields "
                         f"{('0x%x' % rgot) if rgot else rerr}, ren= says 0x{raddr:x}")
                    continue
                out.append(dict(kind="data_alt", name=name, addr=raddr,
                                src=kv["from"], insn=rinsn))

        elif kind == "call":
            addr = int(kv["addr"], 0)
            src = by_name.get(kv.get("from", ""))
            if src is None:
                fail(lineno, name, f"unknown from={kv.get('from')!r}")
                continue
            insn = int(kv["insn"], 0)
            got, err = bl_target(elf, src, insn)
            if got is None:
                fail(lineno, name, err)
                continue
            if got != addr:
                fail(lineno, name,
                     f"BL at insn {insn} of {kv['from']} calls 0x{got:x}, "
                     f"spec says 0x{addr:x}")
                continue
            by_name[name] = addr
            out.append(dict(kind="call", name=name, addr=addr,
                            src=kv["from"], insn=insn))
            if "ren" in kv:
                asrc = by_name_alt.get(kv["from"])
                if alt is None or asrc is None:
                    fail(lineno, name,
                         "ren= needs $EP122_RENESAS and a from= that resolves there")
                    continue
                raddr, rinsn = int(kv["ren"], 0), int(kv["ren_insn"], 0)
                rgot, rerr = bl_target(alt, asrc, rinsn)
                if rgot != raddr:
                    fail(lineno, name,
                         f"ren_insn={rinsn} of {kv['from']} calls "
                         f"{('0x%x' % rgot) if rgot else rerr}, ren= says 0x{raddr:x}")
                    continue
                out.append(dict(kind="call_alt", name=name, addr=raddr,
                                src=kv["from"], insn=rinsn))


        elif kind == "capture":
            out.append(dict(kind="capture", name=name))

        else:
            fail(lineno, name, f"unknown entry kind {kind!r}")

    return out, errors


# --------------------------------------------------------------------------
# emit
# --------------------------------------------------------------------------

def c_bytes(b):
    return ", ".join(f"0x{x:02x}" for x in b)


def emit(results):
    L = []
    a = L.append
    a("// SPDX-License-Identifier: MIT OR Apache-2.0")
    a("/*")
    a(" * ep122_syms.h - GENERATED by tools/gen-syms.py. Do not edit.")
    a(" *")
    a(" * Source of truth: mods/core/ep122_syms.spec")
    a(" *")
    a(" * The addresses in this file are comments only. What ships is the")
    a(" * description (an RTTI class name, a masked instruction signature, a")
    a(" * vtable slot offset); mods/core/resolve.c recomputes each address on")
    a(" * the deck against the running binary. The recorded values are what the")
    a(" * stock application binary resolved to at generation time, so a firmware")
    a(" * that moves a symbol shows up as a diff here.")
    a(" */")
    a("#ifndef EP122_SYMS_H")
    a("#define EP122_SYMS_H")
    a("")
    a("/* Symbol ids. EP122_SYM__COUNT sizes the resolver's table. */")
    a("enum ep122_sym {")
    for r in results:
        if r["kind"] in ("capture", "func_alt", "call_alt", "data_alt"):
            continue                    # func_alt shares its symbol's id
        a(f"    EP122_{r['name']},")
    a("    EP122_SYM__COUNT")
    a("};")
    a("")
    a("/* The spec tables are static, so only one translation unit may include")
    a(" * them: mods/core/resolve.c defines EP122_SYMS_IMPL. Every other file gets")
    a(" * only the enum above. */")
    a("#ifdef EP122_SYMS_IMPL")
    a("")

    vt = [r for r in results if r["kind"] == "vtable"]
    a("/* ---- vtables: resolved by walking RTTI for an exact mangled name ---- */")
    a("static const struct ep122_vt_spec {")
    a("    unsigned short sym;")
    a("    const char    *cls;    /* raw Itanium mangled name */")
    a("    const char    *base;   /* names a secondary vtable, or NULL */")
    a("} k_ep122_vt[] = {")
    for r in vt:
        base = f'"{r["base"]}"' if r["base"] else "NULL"
        a(f'    {{ EP122_{r["name"]}, "{r["cls"]}", {base} }},'
          f'   /* 0x{r["addr"]:x} */')
    a("};")
    a(f"#define EP122_N_VT {len(vt)}")
    a("")

    sl = [r for r in results if r["kind"] == "slot"]
    a("/* ---- virtuals: the slot's contents are the implementation ---- */")
    a("static const struct ep122_slot_spec {")
    a("    unsigned short sym;")
    a("    unsigned short vt;     /* the vtable symbol it lives in */")
    a("    unsigned short off;    /* byte offset from the address point */")
    a("} k_ep122_slot[] = {")
    for r in sl:
        a(f'    {{ EP122_{r["name"]}, EP122_{r["vtable"]}, 0x{r["off"]:x} }},'
          f'   /* 0x{r["addr"]:x} */')
    a("};")
    a(f"#define EP122_N_SLOT {len(sl)}")
    a("")

    fn = [r for r in results if r["kind"] in ("func", "func_alt")]
    maxn = max((r["insn"] for r in fn), default=1)   # widest window, for the resolver's bound
    a("/* ---- free functions: masked aarch64 signatures ---- */")
    a(f"#define EP122_SIG_INSNS_MAX {maxn}")
    a("static const struct ep122_fn_spec {")
    a("    unsigned short sym;")
    a("    unsigned short scope;  /* enclosing function symbol, or 0xffff */")
    a("    unsigned short nth;    /* which scoped match to take */")
    a("    unsigned short total;  /* how many the stock binary had */")
    a("    unsigned int   span;   /* bytes of the enclosing function to scan */")
    a("    unsigned short insns;  /* window length, in instructions */")
    a("    unsigned short anchor; /* instruction index of the fully-kept word */")
    a("    unsigned short akey;   /* 16-bit digest of that word, to index on */")
    a("    unsigned long long digest;  /* FNV-1a of the masked window */")
    a("} k_ep122_fn[] = {")
    for r in fn:
        pat, msk = r["sig"]
        scope = f"EP122_{r['scope']}" if r["scope"] else "0xffff"
        n = len(pat) // 4
        aoff = next(i for i in range(n)
                    if msk[i * 4:i * 4 + 4] == b"\xff\xff\xff\xff")
        aword = struct.unpack_from("<I", pat, aoff * 4)[0]
        akey = ((aword * 2654435761) & 0xFFFFFFFF) >> 16
        a(f'    {{ EP122_{r["name"]}, {scope}, '
          f'{r.get("nth", 0)}, {r.get("total", 0)}, {r.get("span", 0)}, '
          f'{n}, {aoff}, 0x{akey:04x}, 0x{r["digest"]:016x}ULL }},'
          f'   /* 0x{r["addr"]:x} */')
    a("};")
    a(f"#define EP122_N_FN {len(fn)}")
    a("")

    dt = [r for r in results if r["kind"] in ("data", "data_alt")]
    a("/* ---- globals: read the ADRP pair the deck's own code executes ---- */")
    a("static const struct ep122_data_spec {")
    a("    unsigned short sym;")
    a("    unsigned short src;    /* the function symbol to read it out of */")
    a("    unsigned short insn;   /* instruction index of the ADRP */")
    a("} k_ep122_data[] = {")
    for r in dt:
        a(f'    {{ EP122_{r["name"]}, EP122_{r["src"]}, {r["insn"]} }},'
          f'   /* 0x{r["addr"]:x} */')
    a("};")
    a(f"#define EP122_N_DATA {len(dt)}")
    a("")

    ct = [r for r in results if r["kind"] in ("call", "call_alt")]
    a("/* ---- callees: decode the BL the deck's own code executes ---- */")
    a("static const struct ep122_call_spec {")
    a("    unsigned short sym;")
    a("    unsigned short src;    /* the function symbol that calls it */")
    a("    unsigned short insn;   /* instruction index of the BL */")
    a("} k_ep122_call[] = {")
    for r in ct:
        a(f'    {{ EP122_{r["name"]}, EP122_{r["src"]}, {r["insn"]} }},'
          f'   /* 0x{r["addr"]:x} */')
    a("};")
    a(f"#define EP122_N_CALL {len(ct)}")
    a("")

    a("/* Names, for the log line listing what did and did not resolve. */")
    a("static const char *const k_ep122_sym_name[] = {")
    for r in results:
        if r["kind"] in ("capture", "func_alt", "call_alt", "data_alt"):
            continue                    # one name per symbol id, not per table row
        a(f'    "{r["name"]}",')
    a("};")
    a("")
    cap = [r["name"] for r in results if r["kind"] == "capture"]
    if cap:
        a("/* Not resolved statically; captured from a live call:")
        for c in cap:
            a(f" *   {c}")
        a(" */")
        a("")
    a("#endif /* EP122_SYMS_IMPL */")
    a("")
    a("#endif /* EP122_SYMS_H */")
    return "\n".join(L) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-b", "--binary", default=DEFAULT_BIN)
    ap.add_argument("--renesas", default=DEFAULT_ALT,
                    help="the Renesas EP122, for spec entries carrying ren=")
    ap.add_argument("--check", action="store_true",
                    help="verify only; non-zero exit if the header is stale")
    args = ap.parse_args()

    if not os.path.exists(args.binary):
        raise SystemExit(
            f"{args.binary}: missing. Pull the deck's own copy:\n"
            f"  ssh root@<deck-ip> 'cat /home/root/pdj/EP122' > build/EP122-deck")

    elf = Elf(args.binary)
    alt = Elf(args.renesas) if args.renesas and os.path.exists(args.renesas) else None
    entries = parse_spec(SPEC)
    # The spec's own class names seed the RTTI bootstrap; see Rtti.kinds().
    rtti = Rtti(elf, [kv["class"] for _, kind, _, kv in entries
                      if kind == "vtable" and "class" in kv])
    # The RTTI bootstrap samples the $EP122 binary, and its result may not
    # hold for the Renesas binary. It is not needed there: derived entries
    # follow only parents named explicitly with ren=.
    rtti_alt = None
    results, errors = resolve(elf, rtti, entries, alt, rtti_alt)

    for e in errors:
        print(e, file=sys.stderr)

    kinds = {}
    for r in results:
        kinds[r["kind"]] = kinds.get(r["kind"], 0) + 1
    summary = "  ".join(f"{k} {v}" for k, v in sorted(kinds.items()))
    print(f"resolved: {summary}", file=sys.stderr)
    if errors:
        print(f"{len(errors)} unresolved -- header NOT written", file=sys.stderr)
        return 1

    text = emit(results)
    old = open(HEADER).read() if os.path.exists(HEADER) else None
    if args.check:
        if old != text:
            print(f"{HEADER} is stale; run scripts/gen-syms.py",
                  file=sys.stderr)
            return 1
        print("header up to date", file=sys.stderr)
        return 0
    if old == text:
        print("header unchanged", file=sys.stderr)
        return 0
    with open(HEADER, "w") as f:
        f.write(text)
    print(f"wrote {HEADER}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
