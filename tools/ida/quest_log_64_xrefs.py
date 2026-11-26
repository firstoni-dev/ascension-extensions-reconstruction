"""Leftover audit for the 64-slot quest log (ASCENSION_QUEST_LOG_64).

Run headless on an IDA database of Ascension.exe (3.3.5a, 32-bit):

    idat.exe -A -L<log> "-S<repo>\\tools\\ida\\quest_log_64_xrefs.py --out <report.md>" <copy of Ascension.exe.i64>

Options (after the script path, inside the -S quotes):
    --repo PATH   repository root (default: two levels above this script)
    --out PATH    write the Markdown report there (default: stdout / the IDA output window only)
    --no-exit     do not call qexit (for the GUI or an MCP session); the result is in QL64_RESULT

Outside IDA, `python -I tools/ida/quest_log_64_xrefs.py --parse-only` only parses the DLL tables and prints them.

What it does
------------
1. Reads the covered set from the DLL source, never from a copy in this script: src/Ascension/AscQuestLog64*.cpp
   (kOperandPatches, kSlotSites, kFailedSite*, the ReplaceFunction table in Init, Detour calls). Detours in the other
   src/Ascension/*.cpp files are read too, for the harmless rules that depend on them.
2. Checks the tables against the database: every operand / bound / slot-site / failed-site original byte, and that each
   entry lies in the function it names. A mismatch is reported and fails the run.
3. Walks every code head of .text and collects
   a. each operand (memory address, displacement or immediate) that falls in a relocated client range (RANGES), and
   b. each read of a descriptor quest slot, found by a register heuristic (see DescriptorTracker),
   plus every aligned dword in the data segments that points into a relocated range.
4. Classifies every referencing function:
     detoured        the function is replaced whole (ReplaceFunction); its body never runs
     operand-patched every reference is an operand patch, inside a slot site or inside the failed-quest site
     harmless        the remaining references are explained: a rule in HARMLESS (with its reason), or the function is
                     only reachable from replaced functions (computed from the call graph)
     UNCOVERED       anything else
   and exits with status 1 when a row is UNCOVERED or a table entry does not match, else 0.

Descriptor heuristic and its limits
-----------------------------------
The player's quest slots are descriptor fields +0x278 + slot * 0x14 (25 slots, up to +0x46B). The client reaches them
two ways: through the player-field pointer `[player+1008h]` (= descriptor + 0x250, so slots are at +0x28..+0x21B) and
through the descriptor pointer `[obj+8]`. Per function, in address order, the tracker follows registers and [ebp+x]
stack slots that hold either pointer, with a constant offset (updated by `add/sub r, imm`, `add r, reg` and
`lea r, [r+...]`). It also keeps the constant part of plain integer registers (mov/lea/add/sub/shl/imul with
immediates; an unknown register counts as 0), so the client's computed slot address
`lea eax, [slot+slot*4+0Ah] / lea r, [fields+eax*4]` resolves to fields + 0x28 = descriptor + 0x278. It reports the
instruction that first forms or reads a descriptor address whose constant part is in +0x278..+0x46B (derefs of a
pointer already reported are not reported again), and, for the [player+1008h] pointer only, an indexed address whose
constant part is below +0x278 when the function also holds a strong slot marker (an immediate or displacement
21Ch / 46Ch / 278h). [obj+8] is any object's +8 field, so those hits count only in functions with a slot marker
(strong, or `cmp x, 19h`, or `add x, 14h`).
Limits: no control flow (straight address order), calls drop eax/ecx/edx, and pointers passed as arguments (thiscall
`this`), returned by callees, stored outside [ebp+x] or kept in FPU/SSE registers are not followed. So
CGPlayer::GetQuestLogEntry (0x5E0870), which gets player+0x1008 as `this`, is not seen as a reader; it is replaced,
and its callers (0x5E2950, 0x5E4070, 0x5E58C0) read through the replacement. As a recall check, every slot site and the
failed-quest site from the DLL table must produce a hit; one that does not is reported as a problem and fails the run.
"""

import os
import re
import sys

# ---------------------------------------------------------------------------------------------------------------------
# Client facts (Ascension.exe 3.3.5a 12340). These are the old arrays, not the patched set.

RANGES = [
    # (start, end inclusive, name)
    (0xC23618, 0xC2367B, "m_sortTypes[25]"),          # 0xC23674/78 are also m_questWatches[-1] (RemoveQuestWatch)
    (0xC23680, 0xC237AB, "m_questWatches[25]"),       # 12-byte QuestWatch
    (0xC237B0, 0xC23AD7, "m_quests[50] / count / numSortTypes"),
    (0xC23B00, 0xC23FAF, "m_questPOIs[25]"),          # 0x30-byte QuestPOIInfo
]
SLOT_FIRST, SLOT_END = 0x278, 0x278 + 25 * 0x14      # descriptor offsets of the 25 quest slots: 0x278..0x46B
FIELDS_BASE = 0x250                                   # [player+1008h] points at descriptor + 0x250
PLAYER_FIELDS = 0x1008
SLOT_MARKERS = (0x21C, 0x46C, 0x278)

# Explained references. Key: function start (or the first byte of code IDA has no function for, with 'span').
# 'sites': only these instruction addresses are explained (None = every reference in the function). 'needs_detour':
# the rule holds only while the DLL source still detours that address. Rules that match nothing are listed in the report
# so stale ones are seen.
HARMLESS = {
    0x5E71A0: dict(name="CGQuestLog::InitializeGame", sites={0x5E71AD, 0x5E71BE, 0x5E71D5}, needs_detour=0x5E71A0,
                   reason="memset of the old m_quests, its count clear and the memset of the old m_questWatches; the "
                          "DLL's detour runs this original first and then empties the DLL list and s_watches (its POI "
                          "loop is operand-patched)"),
    0x9CB280: dict(name="static constructor of m_questPOIs", sites=None, span=0x30,
                   reason="CRT initialiser of the old array, run before the DLL loads; that array is unused once the "
                          "POI group is relocated, and s_pois starts zeroed as this constructor would leave it"),
}

# Display names for rows whose function is in no table.
NAMES = {0x5E08F0: "QSortQuestLogEntries", 0x6D8030: "Packet_Group_17 (SMSG_QUESTUPDATE_*)"}

# ---------------------------------------------------------------------------------------------------------------------
# DLL source tables


def _block(text, start_pattern):
    """The text from start_pattern to the matching `};`."""
    m = re.search(start_pattern, text)
    if not m:
        return ""
    end = text.find("};", m.end())
    return text[m.end():end if end >= 0 else len(text)]


def parse_sources(repo):
    src = os.path.join(repo, "src", "Ascension")
    own = sorted(f for f in os.listdir(src) if f.startswith("AscQuestLog64") and f.endswith(".cpp"))
    if not own:
        raise SystemExit("no src/Ascension/AscQuestLog64*.cpp under %s" % repo)
    t = dict(files=own, operands=[], slot_sites=[], failed=None, replaced={}, detours={}, other_detours={}, names={})
    for f in own:
        text = open(os.path.join(src, f), encoding="utf-8", errors="replace").read()
        body = _block(text, r"OperandPatch\s+kOperandPatches\[\]\s*=\s*\{")
        for line in body.splitlines():
            if line.strip().startswith("//"):
                continue
            m = re.search(r"\{\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+),\s*(k\w+)\s*\}", line)
            if not m:
                continue
            fn, at, orig, kind = int(m.group(1), 16), int(m.group(2), 16), int(m.group(3), 16), m.group(4)
            t["operands"].append(dict(function=fn, at=at, original=orig, kind=kind))
            c = re.search(r"//\s*([A-Za-z_]\w*(?:::\w+)*)", line)
            if c and fn not in t["names"]:
                t["names"][fn] = c.group(1)
        body = _block(text, r"SlotSite\s+kSlotSites\[\]\s*=\s*\{")
        for m in re.finditer(r"\{\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+),\s*"
                             r"(k\w+),\s*(k\w+),\s*(-?(?:0x[0-9A-Fa-f]+|\d+)),\s*(\d+),\s*\{([^}]*)\}", body):
            original = [int(b, 16) for b in re.findall(r"0x[0-9A-Fa-f]+", m.group(9))]
            t["slot_sites"].append(dict(function=int(m.group(1), 16), start=int(m.group(2), 16),
                                        null=int(m.group(3), 16), size=int(m.group(8)), original=original))
        m = re.search(r"kFailedSite\s*=\s*(0x[0-9A-Fa-f]+),\s*kFailedSiteDone\s*=\s*(0x[0-9A-Fa-f]+)", text)
        if m:
            original = [int(b, 16) for b in re.findall(r"0x[0-9A-Fa-f]+",
                                                       _block(text, r"kFailedSiteOriginal\[\]\s*=\s*\{"))]
            t["failed"] = dict(start=int(m.group(1), 16), done=int(m.group(2), 16), original=original)
        body = _block(text, r"Replacement\s+replacements\[\]\s*=\s*\{")
        for m in re.finditer(r"\{\s*(0x[0-9A-Fa-f]+),\s*reinterpret_cast<void\s*\*>\(&(\w+)\)\s*\}", body):
            t["replaced"][int(m.group(1), 16)] = m.group(2)
        for m in re.finditer(r"Detour\(\s*(0x[0-9A-Fa-f]+),\s*(\d+)", text):
            t["detours"][int(m.group(1), 16)] = (f, int(m.group(2)))
    for f in sorted(os.listdir(src)):
        if f.endswith(".cpp") and f not in own:
            text = open(os.path.join(src, f), encoding="utf-8", errors="replace").read()
            for m in re.finditer(r"Detour\(\s*(0x[0-9A-Fa-f]+),\s*(\d+)", text):
                t["other_detours"][int(m.group(1), 16)] = (f, int(m.group(2)))
    # every client address the DLL code names (comments stripped): a function named there may be called by address,
    # so it never counts as "only reachable from replaced functions"
    t["dll_addresses"] = set()
    for root, _, files in os.walk(os.path.join(repo, "src")):
        for f in files:
            if f.endswith((".cpp", ".hpp", ".h", ".inc")):
                text = open(os.path.join(root, f), encoding="utf-8", errors="replace").read()
                text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
                text = re.sub(r"//[^\n]*", " ", text)
                t["dll_addresses"].update(int(v, 16) for v in re.findall(r"\b0x([0-9A-Fa-f]{6,8})\b", text))
    for key in ("operands", "slot_sites", "replaced"):
        if not t[key]:
            raise SystemExit("parsed no %s from %s: the source layout changed, update parse_sources" % (key, own))
    return t


def bound_size(kind):
    return 3 if kind in ("kBound", "kWatchBound", "kPoiBound") else 4


# ---------------------------------------------------------------------------------------------------------------------
# IDA side

try:
    import ida_auto
    import ida_bytes
    import ida_funcs
    import ida_idp
    import ida_kernwin
    import ida_segment
    import ida_ua
    import idautils
    import idc
    IN_IDA = True
except ImportError:
    IN_IDA = False

EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI = range(8)


def range_of(value):
    for lo, hi, name in RANGES:
        if lo <= value <= hi:
            return name
    return None


def reg32(op):
    """The 32-bit register an o_reg operand writes (al/ah/ax -> eax), or None."""
    r = op.reg
    if r < 8:
        return r
    if 16 <= r <= 23:
        return (r - 16) & 3
    return None


def mem_parts(op):
    """(base, index, scale, disp) of an x86 memory operand; base/index None when absent."""
    if op.type == ida_ua.o_mem:
        if op.specflag1:
            sib = op.specflag2
            idx = (sib >> 3) & 7
            return None, (None if idx == 4 else idx), 1 << (sib >> 6), op.addr
        return None, None, 1, op.addr
    if op.type not in (ida_ua.o_phrase, ida_ua.o_displ):
        return None
    disp = op.addr if op.type == ida_ua.o_displ else 0
    if disp & 0x80000000:
        disp -= 1 << 32
    if op.specflag1:
        sib = op.specflag2
        base, idx = sib & 7, (sib >> 3) & 7
        return base, (None if idx == 4 else idx), 1 << (sib >> 6), disp
    return op.phrase & 7, None, 1, disp


def func_start(ea):
    f = ida_funcs.get_func(ea)
    return f.start_ea if f else None


def code_segments():
    for s in idautils.Segments():
        seg = ida_segment.getseg(s)
        if seg.perm & ida_segment.SEGPERM_EXEC or idc.get_segm_name(s) == ".text":
            yield s, idc.get_segm_end(s)


def data_segments():
    for s in idautils.Segments():
        seg = ida_segment.getseg(s)
        if not (seg.perm & ida_segment.SEGPERM_EXEC) and idc.get_segm_name(s) != ".text":
            yield s, idc.get_segm_end(s)


class DescriptorTracker(object):
    """Follows registers / [ebp+x] that hold the descriptor ("D", from [obj+8]) or the player fields ("F", from
    [player+1008h], descriptor + 0x250), with a constant offset, and the constant part of plain integer registers
    (from mov/lea/add/shl/imul with immediates; an unknown register counts as 0). See the module docstring."""

    def __init__(self, broad_marker, strong_marker):
        self.ptr, self.lin, self.stack, self.hits = {}, {}, {}, []
        self.broad, self.strong = broad_marker, strong_marker

    def _slot(self, ea, kind, k, indexed, mnem):
        """Reports a slot address; True when it did."""
        if kind == "D" and not self.broad:
            return False   # any object's +8: only in functions that also look like a slot walk
        if SLOT_FIRST <= k < SLOT_END:
            self.hits.append((ea, "%s desc+%Xh%s" % (mnem, k, " indexed" if indexed else "")))
            return True
        if indexed and kind == "F" and self.strong and FIELDS_BASE <= k < SLOT_FIRST:
            self.hits.append((ea, "%s desc+%Xh+index (slot walk marker)" % (mnem, k)))
            return True
        return False

    def _c(self, r):
        return self.lin.get(r, 0) if r is not None else 0

    def _kill(self, r):
        self.ptr.pop(r, None)
        self.lin.pop(r, None)

    def step(self, insn):
        ea = insn.ea
        mnem = insn.get_canon_mnem()
        ops = [insn.ops[i] for i in range(8) if insn.ops[i].type != ida_ua.o_void]
        feat = insn.get_canon_feature()

        # every memory operand (and lea) based on a tracked pointer
        formed = None   # (kind, offset, reported) of the address a lea computes
        for op in ops:
            parts = mem_parts(op)
            if not parts:
                continue
            base, idx, scale, disp = parts
            if base is not None and base in self.ptr:
                p, other, s = self.ptr[base], idx, scale
            elif idx is not None and scale == 1 and idx in self.ptr:
                p, other, s = self.ptr[idx], base, 1
            else:
                continue
            kind, off, reported = p
            k = off + disp + self._c(other) * s
            hit = not reported and self._slot(ea, kind, k, other is not None, mnem)
            formed = (kind, k, reported or hit)

        dst = ops[0] if ops else None
        dreg = reg32(dst) if dst is not None and dst.type == ida_ua.o_reg else None
        src = ops[1] if len(ops) > 1 else None
        handled = False
        if dreg is not None and src is not None:
            sreg = reg32(src) if src.type == ida_ua.o_reg else None
            parts = mem_parts(src)
            if mnem == "mov" and parts and parts[1] is None and parts[0] is not None:
                base, _, _, disp = parts
                self._kill(dreg)
                if base == EBP and disp in self.stack:
                    self.ptr[dreg] = self.stack[disp]
                elif disp == PLAYER_FIELDS:
                    self.ptr[dreg] = ("F", FIELDS_BASE, False)
                elif disp == 8 and base not in (EBP, ESP):
                    self.ptr[dreg] = ("D", 0, False)
                handled = True
            elif mnem == "mov" and src.type == ida_ua.o_imm:
                self._kill(dreg)
                self.lin[dreg] = src.value & 0xFFFFFFFF
                handled = True
            elif mnem == "mov" and sreg is not None:
                p, c = self.ptr.get(sreg), self.lin.get(sreg)
                self._kill(dreg)
                if p:
                    self.ptr[dreg] = p
                if c is not None:
                    self.lin[dreg] = c
                handled = True
            elif mnem == "lea" and parts:
                self._kill(dreg)
                if formed:
                    self.ptr[dreg] = formed
                else:
                    base, idx, scale, disp = parts
                    self.lin[dreg] = self._c(base) + self._c(idx) * scale + disp
                handled = True
            elif mnem in ("add", "sub") and src.type == ida_ua.o_imm:
                v = src.value & 0xFFFFFFFF
                v = (v - (1 << 32) if v & 0x80000000 else v) * (1 if mnem == "add" else -1)
                if dreg in self.ptr:
                    kind, off, rep = self.ptr[dreg]
                    self.ptr[dreg] = (kind, off + v, rep)
                self.lin[dreg] = self._c(dreg) + v
                handled = True
            elif mnem == "add" and sreg is not None and dreg in self.ptr:
                kind, off, rep = self.ptr[dreg]
                k = off + self._c(sreg)
                hit = not rep and self._slot(ea, kind, k, True, mnem)
                self.lin.pop(dreg, None)
                self.ptr[dreg] = (kind, k, rep or hit)
                handled = True
            elif mnem == "shl" and src.type == ida_ua.o_imm:
                self.ptr.pop(dreg, None)
                self.lin[dreg] = (self._c(dreg) << (src.value & 31)) & 0xFFFFFFFF
                handled = True
            elif mnem == "imul" and len(ops) == 3 and ops[2].type == ida_ua.o_imm:
                c = self._c(sreg) * ops[2].value if sreg is not None else 0
                self._kill(dreg)
                self.lin[dreg] = c & 0xFFFFFFFF
                handled = True
        # stores to [ebp+x]
        if mnem == "mov" and dst is not None and src is not None:
            parts = mem_parts(dst)
            if parts and parts[0] == EBP and parts[1] is None:
                if src.type == ida_ua.o_reg and reg32(src) in self.ptr:
                    self.stack[parts[3]] = self.ptr[reg32(src)]
                else:
                    self.stack.pop(parts[3], None)
        # any other register write, and the implicit ones
        if not handled and dreg is not None and (feat & ida_idp.CF_CHG1):
            self._kill(dreg)
        if mnem == "xchg":
            for op in ops:
                if op.type == ida_ua.o_reg and reg32(op) is not None:
                    self._kill(reg32(op))
        if mnem == "call" or (mnem in ("mul", "imul", "div", "idiv", "cdq", "rdtsc", "cpuid") and not handled):
            for r in (EAX, ECX, EDX):
                self._kill(r)
        if mnem.startswith(("rep", "lods", "stos", "movs", "scas", "cmps")):
            for r in (EAX, ECX, ESI, EDI):
                self._kill(r)


def function_markers(fstart):
    """(broad, strong) slot markers of a function. strong: an immediate or displacement 21Ch / 46Ch / 278h (the end of
    the 25 slots from the fields pointer, from the descriptor, and the first slot). broad: strong, or `cmp x, 19h`, or
    `add x, 14h` (the slot stride)."""
    f = ida_funcs.get_func(fstart)
    insn = ida_ua.insn_t()
    broad = False
    for ea in idautils.FuncItems(f.start_ea):
        if ida_ua.decode_insn(insn, ea) <= 0:
            continue
        mnem = insn.get_canon_mnem()
        for i in range(8):
            op = insn.ops[i]
            if op.type == ida_ua.o_void:
                break
            if op.type in (ida_ua.o_imm, ida_ua.o_displ):
                v = op.value if op.type == ida_ua.o_imm else op.addr
                if v in SLOT_MARKERS:
                    return True, True
                if op.type == ida_ua.o_imm and i == 1 and ((mnem == "cmp" and v == 0x19) or
                                                           (mnem == "add" and v == 0x14)):
                    broad = True
    return broad, False


def check_tables(t):
    """Every table entry against the database bytes and functions; returns a list of problems."""
    problems = []
    for p in t["operands"]:
        n = bound_size(p["kind"])
        got = int.from_bytes(ida_bytes.get_bytes(p["at"], n) or b"\0" * n, "little")
        if got != p["original"]:
            problems.append("operand 0x%X (%s): database has 0x%X, table 0x%X" % (p["at"], p["kind"], got,
                                                                                p["original"]))
        if func_start(p["at"]) != p["function"]:
            problems.append("operand 0x%X: in function %s, table says 0x%X"
                            % (p["at"], hex(func_start(p["at"]) or 0), p["function"]))
    for s in t["slot_sites"]:
        got = list(ida_bytes.get_bytes(s["start"], s["size"]) or b"")
        if got != s["original"][:s["size"]] or s["start"] + s["size"] != s["null"]:
            problems.append("slot site 0x%X: bytes or size do not match" % s["start"])
        if func_start(s["start"]) != s["function"]:
            problems.append("slot site 0x%X: not in function 0x%X" % (s["start"], s["function"]))
    fs = t["failed"]
    if fs:
        got = list(ida_bytes.get_bytes(fs["start"], len(fs["original"])) or b"")
        if got != fs["original"]:
            problems.append("failed-quest site 0x%X: bytes do not match" % fs["start"])
    for a in list(t["replaced"]) + list(t["detours"]):
        if func_start(a) != a:
            problems.append("hook 0x%X is not a function start in the database" % a)
    return problems


def collect(t):
    """refs[fstart or ('nofunc', ea)] = list of (ea, at, kind, what)."""
    refs = {}
    insn = ida_ua.insn_t()
    trackers = {}
    for seg_start, seg_end in code_segments():
        ea = seg_start
        while ea < seg_end and ea != idc.BADADDR:
            flags = ida_bytes.get_flags(ea)
            if ida_bytes.is_code(flags) and ida_ua.decode_insn(insn, ea) > 0:
                fs = func_start(ea)
                key = fs if fs is not None else ("nofunc", ea)
                # (a) relocated ranges
                for i in range(8):
                    op = insn.ops[i]
                    if op.type == ida_ua.o_void:
                        break
                    if op.type in (ida_ua.o_mem, ida_ua.o_displ):
                        value = op.addr
                    elif op.type == ida_ua.o_imm:
                        value = op.value
                    else:
                        continue
                    name = range_of(value & 0xFFFFFFFF)
                    if name:
                        refs.setdefault(key, []).append((ea, ea + op.offb, "range", "%s 0x%X" % (name, value)))
                # (b) descriptor slots
                if fs is not None:
                    if fs not in trackers:
                        trackers[fs] = DescriptorTracker(*function_markers(fs))
                    tr = trackers[fs]
                    before = len(tr.hits)
                    tr.step(insn)
                    for hit_ea, what in tr.hits[before:]:
                        refs.setdefault(fs, []).append((hit_ea, hit_ea, "slot", what))
                ea += insn.size
            else:
                ea = ida_bytes.next_head(ea, seg_end)
    # data segments: aligned dwords pointing into a range
    for seg_start, seg_end in data_segments():
        ea = (seg_start + 3) & ~3
        while ea + 4 <= seg_end:
            if ida_bytes.is_loaded(ea):
                v = ida_bytes.get_dword(ea)
                name = range_of(v)
                if name:
                    refs.setdefault(("data", ea), []).append((ea, ea, "data", "%s 0x%X" % (name, v)))
            ea += 4
    return refs


def callers(fstart):
    """(set of caller function starts, any reference from outside a function)."""
    out, foreign = set(), False
    for x in idautils.XrefsTo(fstart):
        if x.frm == idc.BADADDR:
            continue
        fs = func_start(x.frm)
        if fs is None or not ida_bytes.is_code(ida_bytes.get_flags(x.frm)):
            foreign = True
        elif fs != fstart:
            out.add(fs)
    return out, foreign


def classify(t, refs):
    replaced = set(t["replaced"])
    patch_at = {p["at"] for p in t["operands"]}
    covered_ranges = [(s["start"], s["null"]) for s in t["slot_sites"]]
    if t["failed"]:
        covered_ranges.append((t["failed"]["start"], t["failed"]["start"] + len(t["failed"]["original"])))
    all_detours = dict(t["other_detours"])
    all_detours.update(t["detours"])

    # Functions only reachable from replaced functions: every code reference to them (call or jump) comes from a
    # replaced function or, recursively, from such a function; no data reference (function pointer, vtable) and no
    # address literal in the DLL source (it may call by address). Cycles and depth > 8 count as reachable.
    memo = {}

    def dead(f, path=()):
        if f in replaced:
            return True
        if f in memo:
            return memo[f]
        if f in path or len(path) > 8 or f in t["dll_addresses"]:
            return False
        cs, foreign = callers(f)
        result = bool(cs) and not foreign and all(dead(c, path + (f,)) for c in cs)
        memo[f] = result
        return result

    only_from_replaced = {k for k in refs if isinstance(k, int) and k not in replaced and dead(k)}

    def rule_for(key):
        if isinstance(key, int):
            return key, HARMLESS.get(key)
        for start, r in HARMLESS.items():   # code IDA has no function for (e.g. a CRT initialiser)
            if start <= key[1] < start + r.get("span", 0):
                return start, r
        return None, None

    rows, used_rules = [], set()
    for key, items in refs.items():
        is_func = isinstance(key, int)
        fstart = key if is_func else None
        rule_key, rule = rule_for(key)
        if is_func:
            name = (t["replaced"].get(fstart) or t["names"].get(fstart) or NAMES.get(fstart) or
                    (rule or {}).get("name") or idc.get_func_name(fstart))
        else:
            name = (rule or {}).get("name") or "(no function)"
        what = sorted({w.split(" 0x")[0] if k != "slot" else "descriptor slots" for _, _, k, w in items})
        uncovered, by_rule, by_reach = [], 0, 0
        if fstart in replaced:
            cls, reason, patched = "detoured", "ReplaceFunction -> %s" % t["replaced"][fstart], 0
        else:
            patched = 0
            rule_ok = rule is not None and (rule.get("needs_detour") is None or rule["needs_detour"] in all_detours)
            for ea, at, kind, w in items:
                if kind == "range" and at in patch_at:
                    patched += 1
                elif kind != "data" and any(lo <= ea < hi for lo, hi in covered_ranges):
                    patched += 1
                elif rule_ok and (rule.get("sites") is None or ea in rule["sites"]):
                    by_rule += 1
                    used_rules.add(rule_key)
                elif is_func and fstart in only_from_replaced:
                    by_reach += 1
                else:
                    uncovered.append("0x%X %s" % (ea, w))
            why = []
            if by_rule:
                why.append(rule["reason"])
            if by_reach:
                cs, _ = callers(fstart)
                why.append("only reachable from replaced functions (callers: %s)"
                           % ", ".join("0x%X" % c for c in sorted(cs)))
            if uncovered:
                cls = "UNCOVERED"
                reason = "; ".join(uncovered[:6]) + (" ..." if len(uncovered) > 6 else "")
            elif not why:
                cls, reason = "operand-patched", "%d site(s) patched" % patched
            elif patched:
                cls, reason = "operand-patched", "%d site(s) patched; %d harmless: %s" % (
                    patched, by_rule + by_reach, "; ".join(why))
            else:
                cls, reason = "harmless", "; ".join(why)
        rows.append(dict(key=key, fstart=fstart, name=name, what=", ".join(what), n=len(items), cls=cls,
                         reason=reason, items=items))

    def sort_key(r):
        k = r["key"]
        return k if isinstance(k, int) else k[1]
    rows.sort(key=sort_key)
    unused = sorted(set(HARMLESS) - used_rules)
    return rows, unused


def report(t, rows, unused, problems):
    order = ["UNCOVERED", "detoured", "operand-patched", "harmless"]
    counts = {c: sum(1 for r in rows if r["cls"] == c) for c in order}
    lines = ["# Quest log 64: xref audit of Ascension.exe", "",
             "Covered set read from %s: %d operand patches, %d slot sites, %s failed-quest site, %d replaced "
             "functions, %d detours in AscQuestLog64*, %d detours elsewhere."
             % (", ".join(t["files"]), len(t["operands"]), len(t["slot_sites"]), "1" if t["failed"] else "no",
                len(t["replaced"]), len(t["detours"]), len(t["other_detours"])), "",
             "Ranges: " + "; ".join("%s 0x%X..0x%X" % (n, lo, hi) for lo, hi, n in RANGES)
             + "; descriptor quest slots desc+0x%X..+0x%X." % (SLOT_FIRST, SLOT_END - 1), "",
             "Result: " + ", ".join("%s %d" % (c, counts[c]) for c in order)
             + ", problems %d." % len(problems), ""]
    if problems:
        lines += ["## Problems (table entries that do not match the database, heuristic recall)", ""]
        lines += ["- " + p for p in problems] + [""]
    lines += ["| Function | Name | References | # | Class | Reason |", "|---|---|---|---|---|---|"]
    for r in rows:
        k = r["key"]
        where = "0x%X" % k if isinstance(k, int) else "%s 0x%X" % k
        lines.append("| %s | %s | %s | %d | %s | %s |" % (where, r["name"], r["what"], r["n"], r["cls"],
                                                          r["reason"].replace("|", "/")))
    detail = [r for r in rows if r["cls"] in ("harmless", "UNCOVERED") or
              (r["cls"] != "detoured" and any(k == "slot" for _, _, k, _ in r["items"]))]
    if detail:
        lines += ["", "## Sites of the harmless, UNCOVERED and descriptor-slot rows (detoured rows omitted)", ""]
        for r in detail:
            k = r["key"]
            lines.append("- %s %s: %s" % ("0x%X" % k if isinstance(k, int) else "%s 0x%X" % k, r["name"],
                                          "; ".join("0x%X %s" % (ea, w) for ea, _, _, w in sorted(r["items"]))))
    if unused:
        lines += ["", "Harmless rules that matched nothing (stale, or not needed on this build): "
                  + ", ".join("0x%X %s" % (a, HARMLESS[a]["name"]) for a in unused)]
    return "\n".join(lines) + "\n", counts


def parse_args(argv):
    opts = dict(repo=None, out=None, exit=True, parse_only=False)
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--repo":
            opts["repo"], i = argv[i + 1], i + 1
        elif a == "--out":
            opts["out"], i = argv[i + 1], i + 1
        elif a == "--no-exit":
            opts["exit"] = False
        elif a == "--parse-only":
            opts["parse_only"] = True
        i += 1
    return opts


def script_path():
    if IN_IDA and idc.ARGV:
        return idc.ARGV[0]
    return globals().get("__file__") or sys.argv[0]


def main():
    argv = list(idc.ARGV[1:]) if IN_IDA else sys.argv[1:]
    opts = parse_args(argv)
    repo = opts["repo"] or os.environ.get("QL64_REPO") or \
        os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(script_path()))))
    t = parse_sources(repo)
    if opts["parse_only"] or not IN_IDA:
        print("files: %s" % t["files"])
        print("operand patches: %d in %d functions" % (len(t["operands"]),
                                                       len({p["function"] for p in t["operands"]})))
        print("slot sites: %s" % ", ".join("0x%X+%d" % (s["start"], s["size"]) for s in t["slot_sites"]))
        print("failed site: %s" % (t["failed"] and "0x%X+%d" % (t["failed"]["start"], len(t["failed"]["original"]))))
        print("replaced: %s" % ", ".join("0x%X" % a for a in sorted(t["replaced"])))
        print("detours: %s; %d detours elsewhere in src/Ascension" % (
            ", ".join("0x%X" % a for a in sorted(t["detours"])), len(t["other_detours"])))
        return 0
    ida_auto.auto_wait()
    problems = check_tables(t)
    refs = collect(t)
    # recall check of the descriptor heuristic: each site the DLL rewrites must be found
    slot_hits = [ea for items in refs.values() for ea, _, kind, _ in items if kind == "slot"]
    sites = [(s["start"], s["null"]) for s in t["slot_sites"]]
    if t["failed"]:
        sites.append((t["failed"]["start"], t["failed"]["start"] + len(t["failed"]["original"])))
    for lo, hi in sites:
        if not any(lo <= ea < hi for ea in slot_hits):
            problems.append("descriptor heuristic found no slot read in the DLL site 0x%X..0x%X" % (lo, hi - 1))
    # and the range scan: each address operand the DLL rewrites must be one of the references found
    seen = {at for items in refs.values() for _, at, kind, _ in items if kind == "range"}
    for p in t["operands"]:
        if p["kind"] in ("kRows", "kCount", "kWatches", "kPois") and p["at"] not in seen:
            problems.append("operand patch 0x%X (%s) is not among the references the scan found" % (p["at"],
                                                                                                  p["kind"]))
    rows, unused = classify(t, refs)
    text, counts = report(t, rows, unused, problems)
    if opts["out"]:
        with open(opts["out"], "w", encoding="utf-8") as f:
            f.write(text)
    print(text)
    status = 1 if counts["UNCOVERED"] or problems else 0
    globals()["QL64_RESULT"] = dict(status=status, counts=counts, problems=problems, rows=rows)
    if opts["exit"] and ida_kernwin.cvar.batch:
        idc.qexit(status)
    return status


if __name__ == "__main__":
    rc = main()
    if not IN_IDA:
        sys.exit(rc)
