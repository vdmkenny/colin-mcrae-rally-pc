#!/usr/bin/env python3
"""Static recompiler: PS1 MIPS R3000 executables -> C.

Each discovered function becomes `void <mod>_<addr>(CPU *c)`. jal becomes a C call,
`jr ra` a C return, jumps leaving a function become tail calls, and `jr reg` jump
tables become switch statements. Hardware, BIOS and GTE access go through the
runtime (see runtime/cpu.h).

Usage: recomp.py <bins dir> <output dir> [ghidra out dir]

Function starts come from tools/functions/<module>.txt (exported from a Ghidra
analysis), or from <ghidra out dir>/<module>.funcs.txt when given.
"""
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mips import Insn, Image  # noqa: E402

MODULES = [
    # name, exe on disc, load address, entry point
    ("scus", "scus.bin", 0x80013000, 0x800EC4D4),
    ("frontend", "frontend.bin", 0x80130000, 0x801CA434),
    ("game", "game.bin", 0x80013000, 0x800BAE1C),
    ("rschool", "rschool.bin", 0x80013000, 0x800B2A80),
]

FUNCS_PER_FILE = 150


class Module:
    def __init__(self, name, path, base, entry, ghidra_funcs, extra_entries):
        self.name = name
        self.img = Image(path, base, entry, name)
        self.base = base
        self.end = self.img.end
        self.words = struct.unpack("<%dI" % (len(self.img.data) // 4), self.img.data[:len(self.img.data) // 4 * 4])
        self.ghidra = ghidra_funcs
        self.extra = extra_entries
        self.cache = {}
        self.warnings = []
        self.pgxp_regs = set()
        self.track_prov = False
        self.hooks = {}

    def insn(self, a):
        i = self.cache.get(a)
        if i is None:
            i = Insn(a, self.words[(a - self.base) >> 2])
            self.cache[a] = i
        return i

    def in_code(self, a):
        return self.base <= a < self.end and (a & 3) == 0

    def clean_start(self, a):
        """True if a looks like the first instruction of a compiled function."""
        if not self.in_code(a):
            return False
        i = self.insn(a)
        if i.op == "invalid":
            return False
        if a - 8 >= self.base and self.words[(a - 8 - self.base) >> 2] == 0x03E00008:
            return True
        if (i.word & 0xFFFF8000) == 0x27BD8000:  # addiu sp,sp,-N
            return True
        return False

    # -------------------------------------------------------------------- discovery
    def discover(self):
        jal_targets = set()
        for k, w in enumerate(self.words):
            if (w >> 26) == 3:
                pc = self.base + k * 4
                t = ((pc + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
                if self.in_code(t):
                    jal_targets.add(t)
        # Function pointers: data words and lui/addiu pairs that point at clean starts.
        ptrs = set()
        for k, w in enumerate(self.words):
            if self.in_code(w) and self.clean_start(w):
                ptrs.add(w)
        for k in range(len(self.words) - 1):
            w = self.words[k]
            if (w >> 26) == 0x0F:  # lui
                rt = (w >> 16) & 31
                hi = (w & 0xFFFF) << 16
                for j in range(k + 1, min(k + 8, len(self.words))):
                    w2 = self.words[j]
                    if (w2 >> 26) in (0x09, 0x0D) and ((w2 >> 21) & 31) == rt:
                        lo = w2 & 0xFFFF
                        v = (hi + (lo - 0x10000 if (w2 >> 26) == 0x09 and lo & 0x8000 else lo)) & 0xFFFFFFFF
                        if self.in_code(v) and self.clean_start(v):
                            ptrs.add(v)
                        break
        # jal targets must decode as code; drop ones landing in data.
        jal_targets = {t for t in jal_targets if self.insn(t).op != "invalid"}
        ghidra_ok = {a for a in self.ghidra if self.clean_start(a)}
        self.boundaries = {self.img.entry} | jal_targets | ghidra_ok | ptrs | set(self.extra)
        self.entries = set(self.boundaries)
        # setjmp returns a second time when the BIOS resumes libetc's interrupt hook, so the
        # instruction after every call to it must be enterable on its own.
        setjmp_sig = [0xAC9F0000, 0xAC9C002C, 0xAC9D0004, 0xAC9E0008]
        for k in range(len(self.words) - 4):
            if list(self.words[k:k + 4]) == setjmp_sig:
                sj = self.base + k * 4
                for j, w in enumerate(self.words):
                    pc = self.base + j * 4
                    if (w >> 26) == 3 and (((pc + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)) == sj:
                        self.entries.add(pc + 8)

    # ----------------------------------------------------------------- jump tables
    def const_before(self, reg, pc, lo_limit):
        """Constant value of reg just before pc (lui/addiu/ori chains), or None."""
        for a in range(pc - 4, max(lo_limit, pc - 96) - 4, -4):
            i = self.insn(a)
            if reg not in i.writes():
                continue
            if i.op == "lui":
                return i.imm << 16
            if i.op == "addiu" and i.rs == reg:
                v = self.const_before(reg, a, lo_limit)
                return None if v is None else (v + i.simm) & 0xFFFFFFFF
            if i.op == "ori" and i.rs == reg:
                v = self.const_before(reg, a, lo_limit)
                return None if v is None else v | i.imm
            if i.op == "addiu" and i.rs == 0:
                return i.simm & 0xFFFFFFFF
            return None
        return None

    def jump_table(self, jr_pc, fstart):
        """Resolve the targets of `jr reg` at jr_pc (a switch jump table), or None."""
        reg = self.insn(jr_pc).rs
        lw = None
        for a in range(jr_pc - 4, max(fstart, jr_pc - 64) - 4, -4):
            i = self.insn(a)
            if i.op == "lw" and i.rt == reg:
                lw = i
                break
            if reg in i.writes():
                return None
        if lw is None:
            return None
        table = None
        for a in range(lw.pc - 4, max(fstart, lw.pc - 64) - 4, -4):
            i = self.insn(a)
            if lw.rs not in i.writes():
                continue
            if i.op == "addu" and i.rd == lw.rs:
                for r in (i.rs, i.rt):
                    v = self.const_before(r, a, fstart)
                    if v is not None:
                        table = (v + lw.simm) & 0xFFFFFFFF
                        break
            break
        if table is None:
            return None
        count = None
        for a in range(lw.pc - 4, max(fstart, lw.pc - 128) - 4, -4):
            i = self.insn(a)
            if i.op == "sltiu":
                count = i.imm
                break
        targets = []
        for n in range(count if count else 1024):
            ta = table + n * 4
            if not (self.base <= ta < self.end):
                break
            t = self.words[(ta - self.base) >> 2]
            if not self.in_code(t) or abs(t - jr_pc) > 0x8000:
                break
            targets.append(t)
        return targets or None

    # ---------------------------------------------------------------------- walking
    def walk(self, entry):
        """Find the instructions belonging to the function at entry.

        Branch targets are always followed (even onto another function's entry, which
        just duplicates that code); straight-line flow into another entry stops there and
        becomes a tail call, as does a forward `j` to a known entry.
        """
        seen = set()
        work = [entry]
        tables = {}
        while work:
            a = work.pop()
            first = True
            while True:
                if a in seen or not self.in_code(a):
                    break
                if not first and a in self.boundaries:
                    break
                first = False
                i = self.insn(a)
                seen.add(a)
                if i.op == "invalid":
                    break
                if not i.has_delay:
                    a += 4
                    continue
                if self.in_code(a + 4):
                    seen.add(a + 4)
                if i.is_branch:
                    t = i.branch_target()
                    if self.in_code(t):
                        work.append(t)
                    a += 8
                    continue
                if i.op == "j":
                    t = i.jump_target()
                    if self.in_code(t) and (t not in self.boundaries or entry <= t <= a):
                        work.append(t)
                    break
                if i.op == "jr":
                    if i.rs != 31:
                        tbl = self.jump_table(a, entry)
                        if tbl:
                            tables[a] = tbl
                            work.extend(tbl)
                        else:
                            self.warnings.append(f"{a:08x}: unresolved jr {i}")
                    break
                a += 8  # jal / jalr return here
        return seen, tables

    # -------------------------------------------------------------------- emitting
    def fname(self, a):
        return f"{self.name}_{a:08x}"


def load_ghidra(path):
    out = set()
    if os.path.exists(path):
        for line in open(path):
            parts = line.split()
            if parts:
                out.add(int(parts[0], 16))
    return out


def load_hooks(path):
    """hooks.txt: '<module> <address> <C function>' per line; the function runs on entry."""
    out = collections.defaultdict(dict)
    if os.path.exists(path):
        for line in open(path):
            line = line.split("#")[0].split()
            if len(line) == 3:
                out[line[0]][int(line[1], 16)] = line[2]
    return out


def load_extra(path):
    out = set()
    if os.path.exists(path):
        for line in open(path):
            line = line.split("#")[0].strip()
            if line:
                out.add(int(line, 16))
    return out


# ============================================================================ codegen
def R(n):
    return "0" if n == 0 else f"R({n})"


def emit_insn(m, i, out):
    """Emit C for a non-control-flow instruction."""
    o = i.op
    rs, rt, rd = R(i.rs), R(i.rt), R(i.rd)

    def setr(r, expr):
        if r != 0:
            out.append(f"R({r}) = {expr};")

    addr = f"{rs} + {i.simm}u" if i.simm >= 0 else f"{rs} - {-i.simm}u"
    if i.rs == 0:
        addr = f"0x{i.simm & 0xFFFFFFFF:x}u"
    if o == "sll":
        if i.word != 0:
            setr(i.rd, f"{rt} << {i.sa}")
    elif o == "srl":
        setr(i.rd, f"{rt} >> {i.sa}")
    elif o == "sra":
        setr(i.rd, f"(uint32_t)((int32_t){rt} >> {i.sa})")
    elif o == "sllv":
        setr(i.rd, f"{rt} << ({rs} & 31)")
    elif o == "srlv":
        setr(i.rd, f"{rt} >> ({rs} & 31)")
    elif o == "srav":
        setr(i.rd, f"(uint32_t)((int32_t){rt} >> ({rs} & 31))")
    elif o == "mfhi":
        setr(i.rd, "c->hi")
    elif o == "mflo":
        setr(i.rd, "c->lo")
    elif o == "mthi":
        out.append(f"c->hi = {rs};")
    elif o == "mtlo":
        out.append(f"c->lo = {rs};")
    elif o == "mult":
        out.append(f"{{ int64_t p = (int64_t)(int32_t){rs} * (int32_t){rt}; c->lo = (uint32_t)p; c->hi = (uint32_t)((uint64_t)p >> 32); }}")
    elif o == "multu":
        out.append(f"{{ uint64_t p = (uint64_t){rs} * {rt}; c->lo = (uint32_t)p; c->hi = (uint32_t)(p >> 32); }}")
    elif o == "div":
        out.append(f"op_div(c, {rs}, {rt});")
    elif o == "divu":
        out.append(f"op_divu(c, {rs}, {rt});")
    elif o in ("add", "addu"):
        setr(i.rd, f"{rs} + {rt}")
    elif o in ("sub", "subu"):
        setr(i.rd, f"{rs} - {rt}")
    elif o == "and":
        setr(i.rd, f"{rs} & {rt}")
    elif o == "or":
        setr(i.rd, f"{rs} | {rt}")
    elif o == "xor":
        setr(i.rd, f"{rs} ^ {rt}")
    elif o == "nor":
        setr(i.rd, f"~({rs} | {rt})")
    elif o == "slt":
        setr(i.rd, f"(int32_t){rs} < (int32_t){rt}")
    elif o == "sltu":
        setr(i.rd, f"{rs} < {rt}")
    elif o in ("addi", "addiu"):
        if i.rs == 0:
            setr(i.rt, f"0x{i.simm & 0xFFFFFFFF:x}u")
        else:
            setr(i.rt, f"{rs} + 0x{i.simm & 0xFFFFFFFF:x}u")
    elif o == "slti":
        setr(i.rt, f"(int32_t){rs} < {i.simm}")
    elif o == "sltiu":
        setr(i.rt, f"{rs} < 0x{i.simm & 0xFFFFFFFF:x}u")
    elif o == "andi":
        setr(i.rt, f"{rs} & 0x{i.imm:x}u")
    elif o == "ori":
        setr(i.rt, f"{rs} | 0x{i.imm:x}u")
    elif o == "xori":
        setr(i.rt, f"{rs} ^ 0x{i.imm:x}u")
    elif o == "lui":
        setr(i.rt, f"0x{i.imm << 16:x}u")
    elif o == "lw" and m.track_prov and i.rt:
        # remember where the value came from: it may become a GTE input vertex
        out.append(f"{{ uint32_t a = {addr}; R({i.rt}) = RD32(a); pgxp_prov({i.rt}, a, R({i.rt})); }}")
    elif o in ("lb", "lbu", "lh", "lhu", "lw"):
        fn = {"lb": "(uint32_t)(int8_t)RD8", "lbu": "RD8", "lh": "(uint32_t)(int16_t)RD16",
              "lhu": "RD16", "lw": "RD32"}[o]
        if i.rt == 0:
            out.append(f"(void){fn}({addr});")
        else:
            out.append(f"R({i.rt}) = {fn}({addr});")
    elif o == "lwl":
        setr(i.rt, f"op_lwl({addr}, {rt})")
    elif o == "lwr":
        setr(i.rt, f"op_lwr({addr}, {rt})")
    elif o == "sb":
        out.append(f"WR8({addr}, {rt});")
    elif o == "sh":
        out.append(f"WR16({addr}, {rt});")
    elif o == "sw":
        if i.rt in m.pgxp_regs:
            out.append(f"pgxp_sw({addr}, {i.rt}, {rt});")
        else:
            out.append(f"WR32({addr}, {rt});")
    elif o == "swl":
        out.append(f"op_swl({addr}, {rt});")
    elif o == "swr":
        out.append(f"op_swr({addr}, {rt});")
    elif o == "mfc0":
        setr(i.rt, f"cop0_read(c, {i.rd})")
    elif o == "mtc0":
        out.append(f"cop0_write(c, {i.rd}, {rt});")
    elif o == "rfe":
        out.append("cop0_rfe(c);")
    elif o == "mfc2":
        setr(i.rt, f"gte_read_data({i.rd})")
        if 12 <= i.rd <= 15 and i.rt:
            out.append(f"pgxp_mfc2({i.rt}, {i.rd}, R({i.rt}));")
    elif o == "cfc2":
        setr(i.rt, f"gte_read_ctrl({i.rd})")
    elif o == "mtc2":
        out.append(f"gte_write_data({i.rd}, {rt});")
        if i.rd in (0, 2, 4):
            out.append(f"pgxp_vsrc_reg({i.rd >> 1}, {i.rt}, {rt});")
    elif o == "ctc2":
        out.append(f"gte_write_ctrl({i.rd}, {rt});")
    elif o == "lwc2":
        if i.rt in (0, 2, 4):
            out.append(f"{{ uint32_t a = {addr}; gte_write_data({i.rt}, RD32(a)); pgxp_vsrc_addr({i.rt >> 1}, a); }}")
        else:
            out.append(f"gte_write_data({i.rt}, RD32({addr}));")
    elif o == "swc2":
        if 12 <= i.rt <= 15:
            out.append(f"pgxp_swc2({addr}, {i.rt});")
        else:
            out.append(f"WR32({addr}, gte_read_data({i.rt}));")
    elif o == "cop2":
        out.append(f"gte_command(0x{i.word & 0x1FFFFFF:x}u);")
    elif o == "syscall":
        out.append(f"psx_syscall(c, 0x{i.pc:08x}u);")
    elif o == "break":
        out.append(f"psx_break(c, 0x{i.pc:08x}u);")
    else:
        out.append(f"psx_bad_insn(c, 0x{i.pc:08x}u, 0x{i.word:08x}u); return;")


def branch_cond(i):
    rs, rt = R(i.rs), R(i.rt)
    return {
        "beq": f"{rs} == {rt}",
        "bne": f"{rs} != {rt}",
        "blez": f"(int32_t){rs} <= 0",
        "bgtz": f"(int32_t){rs} > 0",
        "bltz": f"(int32_t){rs} < 0",
        "bgez": f"(int32_t){rs} >= 0",
        "bltzal": f"(int32_t){rs} < 0",
        "bgezal": f"(int32_t){rs} >= 0",
    }[i.op]


def bios_stub(m, entry):
    """Recognise `li t2,0xA0/B0/C0; jr t2; li t1,N` BIOS call stubs."""
    if not m.in_code(entry + 8):
        return None
    k = (entry - m.base) >> 2
    w0, w1, w2 = m.words[k:k + 3]
    if w0 in (0x240A00A0, 0x240A00B0, 0x240A00C0) and w1 == 0x01400008 and (w2 >> 16) == 0x2409:
        return (w0 & 0xFF, w2 & 0xFFFF)
    return None


def gen_function(m, entry, protos):
    stub = bios_stub(m, entry)
    if stub:
        return [f"void {m.fname(entry)}(CPU *c) {{ R(9) = 0x{stub[1]:x}; bios_call(c, 0x{stub[0]:x}, 0x{stub[1]:x}); }}"]
    body, tables = m.walk(entry)
    addrs = sorted(body)
    # registers that may carry precise screen coordinates (mfc2 from SXY0-2/SXYP)
    m.pgxp_regs = {m.insn(a).rt for a in addrs if m.insn(a).op == "mfc2" and 12 <= m.insn(a).rd <= 15}
    m.track_prov = any(m.insn(a).op == "mtc2" and m.insn(a).rd in (0, 2, 4) for a in addrs)

    # A delay slot is "owned" by the branch before it and emitted together with it.
    slots = set()
    for a in addrs:
        if a not in slots and m.insn(a).has_delay:
            slots.add(a + 4)

    def is_slot(a):
        return a in slots

    labels = set()
    for a in addrs:
        if is_slot(a):
            continue
        i = m.insn(a)
        if i.is_branch and i.branch_target() in body:
            labels.add(i.branch_target())
        if i.op == "j" and i.jump_target() in body:
            labels.add(i.jump_target())
    for tbl in tables.values():
        labels.update(t for t in tbl if t in body)
    for a in list(labels):
        if is_slot(a) and (a + 4) in body:
            labels.add(a + 4)  # executing a slot directly continues after it

    out = [f"void {m.fname(entry)}(CPU *c) {{", f"FN_ENTER(0x{entry:08x}u);"]
    hook = m.hooks.get(entry)
    if hook:
        out.append(f"if ({hook}(c)) return;")
    # Hand-written code sometimes keeps ra in another register and returns with
    # `jr reg`; an unresolved indirect jump to the entry return address is a return.
    if any(m.insn(a).op == "jr" and m.insn(a).rs != 31 and a not in tables for a in addrs if not is_slot(a)):
        out.append("const uint32_t entry_ra = R(31);")
    if addrs[0] != entry:
        labels.add(entry)
        out.append(f"goto L_{entry:08x};")
    ncyc = 0

    def flush():
        nonlocal ncyc
        if ncyc:
            out.append(f"CYC({ncyc});")
            ncyc = 0

    def call(target):
        if m.in_code(target):
            if target not in m.entries:
                m.entries.add(target)
            protos.add(target)
            return f"{m.fname(target)}(c);"
        return f"dispatch_call(c, 0x{target:08x}u);"

    def goto_or_tail(target, back):
        if target in body:
            return ("CHECK_EVENTS(); " if back else "") + f"goto L_{target:08x};"
        return f"{call(target)} return;"

    fall = None  # address execution continues at after the last emitted code
    for a in addrs:
        if is_slot(a) and a not in labels:
            continue
        if fall is not None and fall != a:
            flush()
            out.append(goto_or_tail(fall, False))
        fall = None
        if a in labels:
            flush()
            out.append(f"L_{a:08x}:;")
        i = m.insn(a)
        out.append(f"/* {a:08x} {i} */")
        ncyc += 1
        if not i.has_delay or is_slot(a):
            if i.op == "invalid" or i.has_delay:
                flush()
                out.append(f"psx_bad_insn(c, 0x{a:08x}u, 0x{i.word:08x}u); return;")
                continue
            emit_insn(m, i, out)
            if i.op in ("syscall", "mtc0", "rfe"):
                flush()
                out.append("CHECK_EVENTS();")
            fall = a + 4
            continue
        # Control flow with a delay slot.
        d = m.insn(a + 4) if m.in_code(a + 4) else None
        dl = []
        if d is not None:
            if d.has_delay:
                m.warnings.append(f"{a:08x}: branch in delay slot")
            else:
                emit_insn(m, d, dl)
                ncyc += 1
        n = ncyc
        ncyc = 0
        if i.is_branch:
            t = i.branch_target()
            out.append(f"{{ int take = {branch_cond(i)};")
            if i.op in ("bltzal", "bgezal"):
                out.append(f"R(31) = 0x{a + 8:08x}u;")
                out.extend(dl)
                out.append(f"CYC({n}); if (take) {{ {call(t)} }} }}")
            else:
                out.extend(dl)
                out.append(f"CYC({n}); if (take) {{ {goto_or_tail(t, t <= a)} }} }}")
            fall = a + 8
        elif i.op == "j":
            t = i.jump_target()
            out.extend(dl)
            out.append(f"CYC({n});")
            out.append(goto_or_tail(t, t <= a))
        elif i.op == "jal":
            out.append(f"R(31) = 0x{a + 8:08x}u;")
            out.extend(dl)
            out.append(f"CYC({n});")
            out.append(call(i.jump_target()))
            fall = a + 8
        elif i.op == "jalr":
            out.append(f"{{ uint32_t t = {R(i.rs)};")
            if i.rd:
                out.append(f"R({i.rd}) = 0x{a + 8:08x}u;")
            out.extend(dl)
            out.append(f"CYC({n}); dispatch_call(c, t); }}")
            fall = a + 8
        elif i.op == "jr":
            if i.rs == 31:
                out.extend(dl)
                out.append(f"CYC({n}); return;")
            else:
                out.append(f"{{ uint32_t t = {R(i.rs)};")
                out.extend(dl)
                out.append(f"CYC({n});")
                tbl = tables.get(a)
                if tbl:
                    out.append("switch (t) {")
                    for t in sorted(set(tbl)):
                        if t in body:
                            out.append(f"case 0x{t:08x}u: CHECK_EVENTS(); goto L_{t:08x};")
                    out.append("}")
                if not tbl:
                    out.append("if (t == entry_ra) return;")
                out.append("dispatch_jump(c, t); return; }")
    if fall is not None:
        flush()
        out.append(goto_or_tail(fall, False))
    flush()
    out.append("}")
    return out


def generate(m, outdir):
    os.makedirs(outdir, exist_ok=True)
    done = set()
    order = []
    protos = set()
    files = []
    chunk = []
    pending = sorted(m.entries)
    while pending:
        e = pending.pop(0)
        if e in done:
            continue
        done.add(e)
        order.append(e)
        chunk.append(gen_function(m, e, protos))
        if len(chunk) >= FUNCS_PER_FILE:
            files.append(chunk)
            chunk = []
        # functions discovered while generating (tail-call targets etc.)
        new = sorted(m.entries - done - set(pending))
        pending.extend(new)
    if chunk:
        files.append(chunk)
    for n, ch in enumerate(files):
        with open(os.path.join(outdir, f"{m.name}_{n:03d}.c"), "w") as f:
            f.write(f'#include "{m.name}.h"\n\n')
            for fn in ch:
                f.write("\n".join(fn))
                f.write("\n\n")
    with open(os.path.join(outdir, f"{m.name}.h"), "w") as f:
        f.write('#pragma once\n#include "recomp.h"\n\n')
        for h in sorted(set(m.hooks.values())):
            f.write(f"int {h}(CPU *c);\n")
        for e in sorted(done | protos):
            f.write(f"void {m.fname(e)}(CPU *c);\n")
    with open(os.path.join(outdir, f"{m.name}_table.c"), "w") as f:
        f.write(f'#include "{m.name}.h"\n\n')
        f.write(f"const FuncEntry {m.name}_funcs[] = {{\n")
        for e in sorted(done):
            f.write(f"    {{0x{e:08x}u, {m.fname(e)}}},\n")
        f.write("};\n")
        f.write(f"const unsigned {m.name}_nfuncs = {len(done)};\n")
        sig = struct.unpack_from("<4I", m.img.data, (m.img.entry - m.base))
        f.write(f"const Module {m.name}_module = {{\"{m.name}\", 0x{m.base:08x}u, 0x{m.end:08x}u, 0x{m.img.entry:08x}u, "
                f"{{0x{sig[0]:08x}u, 0x{sig[1]:08x}u, 0x{sig[2]:08x}u, 0x{sig[3]:08x}u}}, {m.name}_funcs, {len(done)}}};\n")
    return len(done), len(files)


def main():
    bins, outdir = sys.argv[1], sys.argv[2]
    tools_dir = os.path.dirname(os.path.abspath(__file__))
    gdir = sys.argv[3] if len(sys.argv) > 3 else None
    extra_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "entries")
    hooks = load_hooks(os.path.join(os.path.dirname(os.path.abspath(__file__)), "hooks.txt"))
    sources = []
    for name, fn, base, entry in MODULES:
        m = Module(name, os.path.join(bins, fn), base, entry,
                   load_ghidra(os.path.join(gdir, f"{name}.funcs.txt") if gdir
                               else os.path.join(tools_dir, "functions", f"{name}.txt")),
                   load_extra(os.path.join(extra_dir, f"{name}.txt")))
        m.discover()
        m.hooks = hooks.get(name, {})
        for a in m.hooks:
            m.entries.add(a)
            m.boundaries.add(a)
        nf, nfiles = generate(m, os.path.join(outdir, name))
        sources.append((name, nfiles))
        print(f"{name}: {nf} functions in {nfiles} files, {len(m.warnings)} warnings")
        with open(os.path.join(outdir, name, "warnings.txt"), "w") as f:
            f.write("\n".join(m.warnings) + "\n")
    with open(os.path.join(outdir, "modules.c"), "w") as f:
        f.write('#include "recomp.h"\n\n')
        for name, _ in sources:
            f.write(f"extern const Module {name}_module;\n")
        f.write("const Module *const all_modules[] = {\n")
        for name, _ in sources:
            f.write(f"    &{name}_module,\n")
        f.write("};\n")
        f.write(f"const unsigned num_modules = {len(sources)};\n")
    with open(os.path.join(outdir, "sources.cmake"), "w") as f:
        f.write("set(GENERATED_SOURCES\n")
        f.write("    ${GEN_DIR}/modules.c\n")
        for name, nfiles in sources:
            f.write(f"    ${{GEN_DIR}}/{name}/{name}_table.c\n")
            for n in range(nfiles):
                f.write(f"    ${{GEN_DIR}}/{name}/{name}_{n:03d}.c\n")
        f.write(")\n")


if __name__ == "__main__":
    main()
