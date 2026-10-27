"""MIPS R3000 (PlayStation CPU) instruction decoder and disassembler."""
import struct

REG = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
       "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
       "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
       "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]

GTE_CMDS = {
    0x01: "RTPS", 0x06: "NCLIP", 0x0C: "OP", 0x10: "DPCS", 0x11: "INTPL",
    0x12: "MVMVA", 0x13: "NCDS", 0x14: "CDP", 0x16: "NCDT", 0x1B: "NCCS",
    0x1C: "CC", 0x1E: "NCS", 0x20: "NCT", 0x28: "SQR", 0x29: "DCPL",
    0x2A: "DPCT", 0x2D: "AVSZ3", 0x2E: "AVSZ4", 0x30: "RTPT", 0x3D: "GPF",
    0x3E: "GPL", 0x3F: "NCCT",
}


def sx16(v):
    return v - 0x10000 if v & 0x8000 else v


class Insn:
    __slots__ = ("pc", "word", "op", "rs", "rt", "rd", "sa", "imm", "simm",
                 "target", "funct")

    def __init__(self, pc, word):
        self.pc = pc
        self.word = word
        self.rs = (word >> 21) & 31
        self.rt = (word >> 16) & 31
        self.rd = (word >> 11) & 31
        self.sa = (word >> 6) & 31
        self.funct = word & 63
        self.imm = word & 0xFFFF
        self.simm = sx16(self.imm)
        self.target = None
        self.op = decode_op(self)

    # control-flow classification -------------------------------------------------
    @property
    def is_branch(self):  # conditional, pc-relative
        return self.op in ("beq", "bne", "blez", "bgtz", "bltz", "bgez", "bltzal", "bgezal")

    @property
    def is_jump(self):
        return self.op in ("j", "jal", "jr", "jalr")

    @property
    def has_delay(self):
        return self.is_branch or self.is_jump

    def branch_target(self):
        return (self.pc + 4 + (self.simm << 2)) & 0xFFFFFFFF

    def jump_target(self):
        return ((self.pc + 4) & 0xF0000000) | ((self.word & 0x03FFFFFF) << 2)

    # register usage (for hazard checks) -------------------------------------------
    def writes(self):
        o = self.op
        if o in ("addi", "addiu", "slti", "sltiu", "andi", "ori", "xori", "lui",
                 "lb", "lh", "lwl", "lw", "lbu", "lhu", "lwr", "mfc0", "mfc2", "cfc2"):
            return {self.rt} - {0}
        if o in ("sll", "srl", "sra", "sllv", "srlv", "srav", "add", "addu", "sub",
                 "subu", "and", "or", "xor", "nor", "slt", "sltu", "mfhi", "mflo", "jalr"):
            return {self.rd} - {0}
        if o in ("jal", "bltzal", "bgezal"):
            return {31}
        return set()

    def reads(self):
        o = self.op
        if o in ("sll", "srl", "sra"):
            return {self.rt}
        if o in ("sllv", "srlv", "srav", "add", "addu", "sub", "subu", "and", "or",
                 "xor", "nor", "slt", "sltu", "mult", "multu", "div", "divu",
                 "beq", "bne"):
            return {self.rs, self.rt}
        if o in ("jr", "jalr", "mthi", "mtlo", "blez", "bgtz", "bltz", "bgez",
                 "bltzal", "bgezal", "addi", "addiu", "slti", "sltiu", "andi", "ori",
                 "xori", "lb", "lh", "lw", "lbu", "lhu", "lwc2"):
            return {self.rs}
        if o in ("lwl", "lwr", "sb", "sh", "sw", "swl", "swr"):
            return {self.rs, self.rt}
        if o in ("mtc0", "mtc2", "ctc2"):
            return {self.rt}
        if o == "swc2":
            return {self.rs}
        return set()

    def __str__(self):
        return disasm(self)


def decode_op(i):
    w = i.word
    op = w >> 26
    if op == 0:
        f = i.funct
        return {0: "sll", 2: "srl", 3: "sra", 4: "sllv", 6: "srlv", 7: "srav",
                8: "jr", 9: "jalr", 12: "syscall", 13: "break", 16: "mfhi",
                17: "mthi", 18: "mflo", 19: "mtlo", 24: "mult", 25: "multu",
                26: "div", 27: "divu", 32: "add", 33: "addu", 34: "sub",
                35: "subu", 36: "and", 37: "or", 38: "xor", 39: "nor",
                42: "slt", 43: "sltu"}.get(f, "invalid")
    if op == 1:
        rt = i.rt
        if rt == 0x10:
            return "bltzal"
        if rt == 0x11:
            return "bgezal"
        return "bgez" if rt & 1 else "bltz"
    if op == 0x10:  # COP0
        if i.rs == 0:
            return "mfc0"
        if i.rs == 4:
            return "mtc0"
        if i.rs == 0x10 and i.funct == 0x10:
            return "rfe"
        return "invalid"
    if op == 0x12:  # COP2 (GTE)
        if i.rs & 0x10:
            return "cop2"
        return {0: "mfc2", 2: "cfc2", 4: "mtc2", 6: "ctc2"}.get(i.rs, "invalid")
    return {2: "j", 3: "jal", 4: "beq", 5: "bne", 6: "blez", 7: "bgtz",
            8: "addi", 9: "addiu", 10: "slti", 11: "sltiu", 12: "andi",
            13: "ori", 14: "xori", 15: "lui", 0x20: "lb", 0x21: "lh",
            0x22: "lwl", 0x23: "lw", 0x24: "lbu", 0x25: "lhu", 0x26: "lwr",
            0x28: "sb", 0x29: "sh", 0x2A: "swl", 0x2B: "sw", 0x2E: "swr",
            0x32: "lwc2", 0x3A: "swc2"}.get(op, "invalid")


def disasm(i):
    o, r = i.op, REG
    if o in ("sll", "srl", "sra"):
        if i.word == 0:
            return "nop"
        return f"{o} {r[i.rd]}, {r[i.rt]}, {i.sa}"
    if o in ("sllv", "srlv", "srav"):
        return f"{o} {r[i.rd]}, {r[i.rt]}, {r[i.rs]}"
    if o == "jr":
        return f"jr {r[i.rs]}"
    if o == "jalr":
        return f"jalr {r[i.rd]}, {r[i.rs]}"
    if o in ("syscall", "break", "rfe"):
        return o
    if o in ("mfhi", "mflo"):
        return f"{o} {r[i.rd]}"
    if o in ("mthi", "mtlo"):
        return f"{o} {r[i.rs]}"
    if o in ("mult", "multu", "div", "divu"):
        return f"{o} {r[i.rs]}, {r[i.rt]}"
    if o in ("add", "addu", "sub", "subu", "and", "or", "xor", "nor", "slt", "sltu"):
        return f"{o} {r[i.rd]}, {r[i.rs]}, {r[i.rt]}"
    if o in ("bltz", "bgez", "bltzal", "bgezal", "blez", "bgtz"):
        return f"{o} {r[i.rs]}, 0x{i.branch_target():08x}"
    if o in ("beq", "bne"):
        return f"{o} {r[i.rs]}, {r[i.rt]}, 0x{i.branch_target():08x}"
    if o in ("j", "jal"):
        return f"{o} 0x{i.jump_target():08x}"
    if o in ("addi", "addiu", "slti", "sltiu"):
        return f"{o} {r[i.rt]}, {r[i.rs]}, {i.simm}"
    if o in ("andi", "ori", "xori"):
        return f"{o} {r[i.rt]}, {r[i.rs]}, 0x{i.imm:x}"
    if o == "lui":
        return f"lui {r[i.rt]}, 0x{i.imm:x}"
    if o in ("lb", "lh", "lwl", "lw", "lbu", "lhu", "lwr", "sb", "sh", "swl", "sw", "swr"):
        return f"{o} {r[i.rt]}, {i.simm}({r[i.rs]})"
    if o in ("lwc2", "swc2"):
        return f"{o} gte{i.rt}, {i.simm}({r[i.rs]})"
    if o in ("mfc0", "mtc0"):
        return f"{o} {r[i.rt]}, cop0_{i.rd}"
    if o in ("mfc2", "mtc2"):
        return f"{o} {r[i.rt]}, gd{i.rd}"
    if o in ("cfc2", "ctc2"):
        return f"{o} {r[i.rt]}, gc{i.rd}"
    if o == "cop2":
        return f"cop2 {GTE_CMDS.get(i.word & 0x3F, '?')} 0x{i.word & 0x1FFFFFF:07x}"
    return f".word 0x{i.word:08x}"


class Image:
    """A loaded executable image (one PS-X EXE body)."""

    def __init__(self, path, base, entry, name):
        self.data = open(path, "rb").read()
        self.base = base
        self.end = base + len(self.data)
        self.entry = entry
        self.name = name

    def contains(self, addr):
        return self.base <= addr < self.end

    def word(self, addr):
        return struct.unpack_from("<I", self.data, addr - self.base)[0]

    def insn(self, addr):
        return Insn(addr, self.word(addr))
