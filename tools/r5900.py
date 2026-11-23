"""R5900 (PS2 Emotion Engine) instruction decoder.

decode(word) -> Insn(mnem, fmt, fields) or Insn('invalid', ...).
Formats name which fields the C macro receives; see emit.py / ps2_ops.h.
"""
from dataclasses import dataclass

@dataclass(frozen=True)
class Insn:
    mnem: str
    fmt: str          # operand layout key
    ops: tuple        # operand values, in fmt order
    word: int

    @property
    def is_branch(self):
        return self.mnem in BRANCHES

    @property
    def likely(self):
        return self.mnem in LIKELY


def sx16(v):
    return v - 0x10000 if v & 0x8000 else v


# Branches with a delay slot. Value = condition kind.
BRANCHES = {
    'beq', 'bne', 'blez', 'bgtz', 'beql', 'bnel', 'blezl', 'bgtzl',
    'bltz', 'bgez', 'bltzl', 'bgezl', 'bltzal', 'bgezal', 'bltzall', 'bgezall',
    'bc1f', 'bc1t', 'bc1fl', 'bc1tl', 'bc0f', 'bc0t', 'bc0fl', 'bc0tl',
    'bc2f', 'bc2t', 'bc2fl', 'bc2tl',
    'j', 'jal', 'jr', 'jalr',
}
LIKELY = {'beql', 'bnel', 'blezl', 'bgtzl', 'bltzl', 'bgezl', 'bltzall', 'bgezall',
          'bc1fl', 'bc1tl', 'bc0fl', 'bc0tl', 'bc2fl', 'bc2tl'}

PRIMARY_I = {8: 'addi', 9: 'addiu', 10: 'slti', 11: 'sltiu', 24: 'daddi', 25: 'daddiu'}
PRIMARY_IU = {12: 'andi', 13: 'ori', 14: 'xori'}
PRIMARY_MEM = {26: 'ldl', 27: 'ldr', 30: 'lq', 31: 'sq', 32: 'lb', 33: 'lh', 34: 'lwl', 35: 'lw',
               36: 'lbu', 37: 'lhu', 38: 'lwr', 39: 'lwu', 40: 'sb', 41: 'sh', 42: 'swl', 43: 'sw',
               44: 'sdl', 45: 'sdr', 46: 'swr', 55: 'ld', 63: 'sd',
               49: 'lwc1', 57: 'swc1', 54: 'lqc2', 62: 'sqc2'}
PRIMARY_BR2 = {4: 'beq', 5: 'bne', 20: 'beql', 21: 'bnel'}
PRIMARY_BR1 = {6: 'blez', 7: 'bgtz', 22: 'blezl', 23: 'bgtzl'}

SPECIAL = {
    0: ('sll', 'sh'), 2: ('srl', 'sh'), 3: ('sra', 'sh'), 4: ('sllv', 'shv'), 6: ('srlv', 'shv'),
    7: ('srav', 'shv'), 8: ('jr', 'jr'), 9: ('jalr', 'jalr'), 10: ('movz', 'r3'), 11: ('movn', 'r3'),
    12: ('syscall', 'code'), 13: ('break', 'code'), 15: ('sync', 'none'),
    16: ('mfhi', 'rd'), 17: ('mthi', 'rs'), 18: ('mflo', 'rd'), 19: ('mtlo', 'rs'),
    20: ('dsllv', 'shv'), 22: ('dsrlv', 'shv'), 23: ('dsrav', 'shv'),
    24: ('mult', 'r3'), 25: ('multu', 'r3'), 26: ('div', 'st'), 27: ('divu', 'st'),
    32: ('add', 'r3'), 33: ('addu', 'r3'), 34: ('sub', 'r3'), 35: ('subu', 'r3'),
    36: ('and', 'r3'), 37: ('or', 'r3'), 38: ('xor', 'r3'), 39: ('nor', 'r3'),
    40: ('mfsa', 'rd'), 41: ('mtsa', 'rs'), 42: ('slt', 'r3'), 43: ('sltu', 'r3'),
    44: ('dadd', 'r3'), 45: ('daddu', 'r3'), 46: ('dsub', 'r3'), 47: ('dsubu', 'r3'),
    48: ('tge', 'st'), 49: ('tgeu', 'st'), 50: ('tlt', 'st'), 51: ('tltu', 'st'),
    52: ('teq', 'st'), 54: ('tne', 'st'),
    56: ('dsll', 'sh'), 58: ('dsrl', 'sh'), 59: ('dsra', 'sh'),
    60: ('dsll32', 'sh'), 62: ('dsrl32', 'sh'), 63: ('dsra32', 'sh'),
}
REGIMM = {0: 'bltz', 1: 'bgez', 2: 'bltzl', 3: 'bgezl', 16: 'bltzal', 17: 'bgezal',
          18: 'bltzall', 19: 'bgezall'}
REGIMM_TRAP = {8: 'tgei', 9: 'tgeiu', 10: 'tlti', 11: 'tltiu', 12: 'teqi', 14: 'tnei'}

MMI = {0: 'madd', 1: 'maddu', 4: 'plzcw', 16: 'mfhi1', 17: 'mthi1', 18: 'mflo1', 19: 'mtlo1',
       24: 'mult1', 25: 'multu1', 26: 'div1', 27: 'divu1', 32: 'madd1', 33: 'maddu1',
       48: 'pmfhl', 49: 'pmthl', 52: 'psllh', 54: 'psrlh', 55: 'psrah',
       60: 'psllw', 62: 'psrlw', 63: 'psraw'}
MMI0 = {0: 'paddw', 1: 'psubw', 2: 'pcgtw', 3: 'pmaxw', 4: 'paddh', 5: 'psubh', 6: 'pcgth',
        7: 'pmaxh', 8: 'paddb', 9: 'psubb', 10: 'pcgtb', 16: 'paddsw', 17: 'psubsw',
        18: 'pextlw', 19: 'ppacw', 20: 'paddsh', 21: 'psubsh', 22: 'pextlh', 23: 'ppach',
        24: 'paddsb', 25: 'psubsb', 26: 'pextlb', 27: 'ppacb', 30: 'pext5', 31: 'ppac5'}
MMI1 = {1: 'pabsw', 2: 'pceqw', 3: 'pminw', 4: 'padsbh', 5: 'pabsh', 6: 'pceqh', 7: 'pminh',
        10: 'pceqb', 16: 'padduw', 17: 'psubuw', 18: 'pextuw', 20: 'padduh', 21: 'psubuh',
        22: 'pextuh', 24: 'paddub', 25: 'psubub', 26: 'pextub', 27: 'qfsrv'}
MMI2 = {0: 'pmaddw', 2: 'psllvw', 3: 'psrlvw', 4: 'pmsubw', 8: 'pmfhi', 9: 'pmflo', 10: 'pinth',
        12: 'pmultw', 13: 'pdivw', 14: 'pcpyld', 16: 'pmaddh', 17: 'phmadh', 18: 'pand',
        19: 'pxor', 20: 'pmsubh', 21: 'phmsbh', 26: 'pexeh', 27: 'prevh', 28: 'pmulth',
        29: 'pdivbw', 30: 'pexew', 31: 'prot3w'}
MMI3 = {0: 'pmadduw', 3: 'psravw', 8: 'pmthi', 9: 'pmtlo', 10: 'pinteh', 12: 'pmultuw',
        13: 'pdivuw', 14: 'pcpyud', 18: 'por', 19: 'pnor', 26: 'pexch', 27: 'pcpyh', 30: 'pexcw'}

COP1_S = {0: 'add.s', 1: 'sub.s', 2: 'mul.s', 3: 'div.s', 4: 'sqrt.s', 5: 'abs.s', 6: 'mov.s',
          7: 'neg.s', 22: 'rsqrt.s', 24: 'adda.s', 25: 'suba.s', 26: 'mula.s', 28: 'madd.s',
          29: 'msub.s', 30: 'madda.s', 31: 'msuba.s', 36: 'cvt.w.s', 40: 'max.s', 41: 'min.s',
          48: 'c.f.s', 50: 'c.eq.s', 52: 'c.lt.s', 54: 'c.le.s'}

INVALID = None


def decode(w):
    op = w >> 26
    rs = (w >> 21) & 31
    rt = (w >> 16) & 31
    rd = (w >> 11) & 31
    sa = (w >> 6) & 31
    fn = w & 63
    imm = w & 0xffff
    simm = sx16(imm)

    def I(m, f, *o):
        return Insn(m, f, tuple(o), w)

    if op == 0:
        e = SPECIAL.get(fn)
        if not e:
            return I('invalid', 'none')
        m, f = e
        if f == 'r3': return I(m, f, rd, rs, rt)
        if f == 'sh': return I(m, f, rd, rt, sa)
        if f == 'shv': return I(m, f, rd, rt, rs)
        if f == 'jr': return I(m, f, rs)
        if f == 'jalr': return I(m, f, rd, rs)
        if f == 'rd': return I(m, f, rd)
        if f == 'rs': return I(m, f, rs)
        if f == 'st': return I(m, f, rs, rt)
        if f == 'code': return I(m, f, (w >> 6) & 0xfffff)
        return I(m, 'none')
    if op == 1:
        if rt in REGIMM: return I(REGIMM[rt], 'br1', rs, simm)
        if rt in REGIMM_TRAP: return I(REGIMM_TRAP[rt], 'i1', rs, simm)
        if rt == 24: return I('mtsab', 'i1', rs, simm)
        if rt == 25: return I('mtsah', 'i1', rs, simm)
        return I('invalid', 'none')
    if op in (2, 3):
        return I('j' if op == 2 else 'jal', 'jt', (w & 0x3ffffff) << 2)
    if op in PRIMARY_BR2: return I(PRIMARY_BR2[op], 'br2', rs, rt, simm)
    if op in PRIMARY_BR1: return I(PRIMARY_BR1[op], 'br1', rs, simm)
    if op in PRIMARY_I: return I(PRIMARY_I[op], 'i', rt, rs, simm)
    if op in PRIMARY_IU: return I(PRIMARY_IU[op], 'i', rt, rs, imm)
    if op == 15: return I('lui', 'lui', rt, imm)
    if op in PRIMARY_MEM: return I(PRIMARY_MEM[op], 'mem', rt, rs, simm)
    if op == 47: return I('cache', 'none')
    if op == 51: return I('pref', 'none')
    if op == 16:  # COP0
        if rs == 0: return I('mfc0', 'cop0', rt, rd)
        if rs == 4: return I('mtc0', 'cop0', rt, rd)
        if rs == 8:
            m = {0: 'bc0f', 1: 'bc0t', 2: 'bc0fl', 3: 'bc0tl'}.get(rt)
            return I(m, 'brc', simm) if m else I('invalid', 'none')
        if rs == 16:
            m = {1: 'tlbr', 2: 'tlbwi', 6: 'tlbwr', 8: 'tlbp', 24: 'eret', 56: 'ei', 57: 'di'}.get(fn)
            return I(m, 'none') if m else I('invalid', 'none')
        return I('invalid', 'none')
    if op == 17:  # COP1
        if rs == 0: return I('mfc1', 'fmv', rt, rd)
        if rs == 2: return I('cfc1', 'fmv', rt, rd)
        if rs == 4: return I('mtc1', 'fmv', rt, rd)
        if rs == 6: return I('ctc1', 'fmv', rt, rd)
        if rs == 8:
            m = {0: 'bc1f', 1: 'bc1t', 2: 'bc1fl', 3: 'bc1tl'}.get(rt)
            return I(m, 'brc', simm) if m else I('invalid', 'none')
        if rs == 16:
            m = COP1_S.get(fn)
            if not m: return I('invalid', 'none')
            return I(m, 'f3', sa, rd, rt)  # fd, fs, ft
        if rs == 20 and fn == 32:
            return I('cvt.s.w', 'f3', sa, rd, rt)
        return I('invalid', 'none')
    if op == 18:  # COP2 / VU0 macro mode
        if rs == 1: return I('qmfc2', 'fmv', rt, rd)
        if rs == 2: return I('cfc2', 'fmv', rt, rd)
        if rs == 5: return I('qmtc2', 'fmv', rt, rd)
        if rs == 6: return I('ctc2', 'fmv', rt, rd)
        if rs == 8:
            m = {0: 'bc2f', 1: 'bc2t', 2: 'bc2fl', 3: 'bc2tl'}.get(rt)
            return I(m, 'brc', simm) if m else I('invalid', 'none')
        if rs >= 16:
            return I('vu0', 'raw')  # interpreted by the runtime's VU0 macro unit
        return I('invalid', 'none')
    if op == 28:  # MMI
        if fn == 8: tbl = MMI0
        elif fn == 40: tbl = MMI1
        elif fn == 9: tbl = MMI2
        elif fn == 41: tbl = MMI3
        else:
            m = MMI.get(fn)
            if not m: return I('invalid', 'none')
            if m in ('psllh', 'psrlh', 'psrah', 'psllw', 'psrlw', 'psraw'):
                return I(m, 'sh', rd, rt, sa)
            if m in ('pmfhl', 'pmthl'):
                return I(m, 'mmisub', rd, rs, sa)
            return I(m, 'r3', rd, rs, rt)
        m = tbl.get(sa)
        if not m: return I('invalid', 'none')
        return I(m, 'r3', rd, rs, rt)
    return I('invalid', 'none')


def branch_target(pc, insn):
    """Static target of a branch, or None for register jumps."""
    if insn.fmt == 'jt':
        return ((pc + 4) & 0xf0000000) | insn.ops[0]
    if insn.fmt in ('br1', 'br2', 'brc'):
        return (pc + 4 + (insn.ops[-1] << 2)) & 0xffffffff
    return None
