"""Minimal VU micro-mode disassembler.

  python -I vudis.py vu1_code.bin START_BYTE [COUNT]
"""
import struct, sys

UP_BC = ['ADD', 'SUB', 'MADD', 'MSUB', 'MAX', 'MINI', 'MUL']
UP_MISC = {0x1C: 'MULq', 0x1D: 'MAXi', 0x1E: 'MULi', 0x1F: 'MINIi', 0x20: 'ADDq', 0x21: 'MADDq', 0x22: 'ADDi',
           0x23: 'MADDi', 0x24: 'SUBq', 0x25: 'MSUBq', 0x26: 'SUBi', 0x27: 'MSUBi', 0x28: 'ADD', 0x29: 'MADD',
           0x2A: 'MUL', 0x2B: 'MAX', 0x2C: 'SUB', 0x2D: 'MSUB', 0x2E: 'OPMSUB', 0x2F: 'MINI'}
UP_SP = {0x1C: 'MULAq', 0x1D: 'ABS', 0x1E: 'MULAi', 0x1F: 'CLIPw', 0x20: 'ADDAq', 0x21: 'MADDAq', 0x22: 'ADDAi',
         0x23: 'MADDAi', 0x24: 'SUBAq', 0x25: 'MSUBAq', 0x26: 'SUBAi', 0x27: 'MSUBAi', 0x28: 'ADDA', 0x29: 'MADDA',
         0x2A: 'MULA', 0x2C: 'SUBA', 0x2D: 'MSUBA', 0x2E: 'OPMULA', 0x2F: 'NOP'}
LO_SP = {0x30: 'MOVE', 0x31: 'MR32', 0x34: 'LQI', 0x35: 'SQI', 0x36: 'LQD', 0x37: 'SQD', 0x38: 'DIV', 0x39: 'SQRT',
         0x3A: 'RSQRT', 0x3B: 'WAITQ', 0x3C: 'MTIR', 0x3D: 'MFIR', 0x3E: 'ILWR', 0x3F: 'ISWR', 0x40: 'RNEXT',
         0x41: 'RGET', 0x42: 'RINIT', 0x43: 'RXOR', 0x64: 'MFP', 0x68: 'XTOP', 0x69: 'XITOP', 0x6C: 'XGKICK',
         0x70: 'ESADD', 0x71: 'ERSADD', 0x72: 'ELENG', 0x73: 'ERLENG', 0x74: 'EATANxy', 0x75: 'EATANxz',
         0x76: 'ESUM', 0x78: 'ESQRT', 0x79: 'ERSQRT', 0x7A: 'ERCPR', 0x7B: 'WAITP', 0x7C: 'ESIN', 0x7D: 'EATAN',
         0x7E: 'EEXP'}
LO_OP = {0x00: 'LQ', 0x01: 'SQ', 0x04: 'ILW', 0x05: 'ISW', 0x08: 'IADDIU', 0x09: 'ISUBIU', 0x10: 'FCEQ',
         0x11: 'FCSET', 0x12: 'FCAND', 0x13: 'FCOR', 0x14: 'FSEQ', 0x15: 'FSSET', 0x16: 'FSAND', 0x17: 'FSOR',
         0x18: 'FMEQ', 0x1A: 'FMAND', 0x1B: 'FMOR', 0x1C: 'FCGET', 0x20: 'B', 0x21: 'BAL', 0x24: 'JR',
         0x25: 'JALR', 0x28: 'IBEQ', 0x29: 'IBNE', 0x2C: 'IBLTZ', 0x2D: 'IBGTZ', 0x2E: 'IBLEZ', 0x2F: 'IBGEZ'}


def dest(w):
    d = (w >> 21) & 15
    return ''.join(c for i, c in enumerate('xyzw') if d & (8 >> i))


def upper(w):
    op, ft, fs, fd, bc = w & 63, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, 'xyzw'[w & 3]
    d = dest(w)
    if op < 0x1C:
        return f'{UP_BC[op >> 2]}{bc}.{d} vf{fd}, vf{fs}, vf{ft}{bc}'
    if op in UP_MISC:
        n = UP_MISC[op]
        if n[-1] in 'qi' and n not in ('MINI',):
            return f'{n}.{d} vf{fd}, vf{fs}, {n[-1].upper()}'
        return f'{n}.{d} vf{fd}, vf{fs}, vf{ft}'
    if op < 0x3C:
        return f'?up{op:02x}'
    sp = ((w >> 4) & 0x7C) | (w & 3)
    if sp < 0x10:
        return f'{UP_BC[sp >> 2]}A{bc}.{d} ACC, vf{fs}, vf{ft}{bc}'
    if sp < 0x14:
        return f'ITOF{[0, 4, 12, 15][sp & 3]}.{d} vf{ft}, vf{fs}'
    if sp < 0x18:
        return f'FTOI{[0, 4, 12, 15][sp & 3]}.{d} vf{ft}, vf{fs}'
    if sp < 0x1C:
        return f'MULA{bc}.{d} ACC, vf{fs}, vf{ft}{bc}'
    n = UP_SP.get(sp, f'?usp{sp:02x}')
    if n == 'NOP':
        return 'nop'
    if n == 'ABS':
        return f'ABS.{d} vf{ft}, vf{fs}'
    if n == 'CLIPw':
        return f'CLIPw.xyz vf{fs}, vf{ft}w'
    if n[-1] in 'qi':
        return f'{n}.{d} ACC, vf{fs}, {n[-1].upper()}'
    return f'{n}.{d} ACC, vf{fs}, vf{ft}'


def imm11(w):
    v = w & 0x7FF
    return v - 0x800 if v & 0x400 else v


def lower(w, pc):
    if w == 0x8000033C:
        return 'nop'
    op, ft, fs, fd = w >> 25, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
    it, is_, id_ = ft & 15, fs & 15, fd & 15
    d = dest(w)
    fsf, ftf = 'xyzw'[(w >> 21) & 3], 'xyzw'[(w >> 23) & 3]
    if op == 0x40:
        f = w & 63
        if f == 0x30: return f'IADD vi{id_}, vi{is_}, vi{it}'
        if f == 0x31: return f'ISUB vi{id_}, vi{is_}, vi{it}'
        if f == 0x32:
            im = (w >> 6) & 31
            if im & 16: im -= 32
            return f'IADDI vi{it}, vi{is_}, {im}'
        if f == 0x34: return f'IAND vi{id_}, vi{is_}, vi{it}'
        if f == 0x35: return f'IOR vi{id_}, vi{is_}, vi{it}'
        if f < 0x3C: return f'?lo40_{f:02x}'
        sp = ((w >> 4) & 0x7C) | (w & 3)
        n = LO_SP.get(sp, f'?lsp{sp:02x}')
        if n in ('MOVE', 'MR32'): return f'{n}.{d} vf{ft}, vf{fs}'
        if n in ('LQI',): return f'LQI.{d} vf{ft}, (vi{is_}++)'
        if n in ('LQD',): return f'LQD.{d} vf{ft}, (--vi{is_})'
        if n in ('SQI',): return f'SQI.{d} vf{fs}, (vi{it}++)'
        if n in ('SQD',): return f'SQD.{d} vf{fs}, (--vi{it})'
        if n in ('DIV', 'RSQRT'): return f'{n} Q, vf{fs}{fsf}, vf{ft}{ftf}'
        if n == 'SQRT': return f'SQRT Q, vf{ft}{ftf}'
        if n == 'MTIR': return f'MTIR vi{it}, vf{fs}{fsf}'
        if n == 'MFIR': return f'MFIR.{d} vf{ft}, vi{is_}'
        if n in ('ILWR', 'ISWR'): return f'{n}.{d} vi{it}, (vi{is_})'
        if n in ('XTOP', 'XITOP'): return f'{n} vi{it}'
        if n == 'XGKICK': return f'XGKICK vi{is_}'
        if n in ('MFP', 'RNEXT', 'RGET'): return f'{n}.{d} vf{ft}'
        if n in ('RINIT', 'RXOR', 'ESQRT', 'ERSQRT', 'ERCPR', 'ESIN', 'EATAN', 'EEXP'): return f'{n} vf{fs}{fsf}'
        if n.startswith('E'): return f'{n} vf{fs}'
        return n
    n = LO_OP.get(op, f'?lo{op:02x}')
    if n in ('LQ',): return f'LQ.{d} vf{ft}, {imm11(w)}(vi{is_})'
    if n in ('SQ',): return f'SQ.{d} vf{fs}, {imm11(w)}(vi{it})'
    if n in ('ILW', 'ISW'): return f'{n}.{d} vi{it}, {imm11(w)}(vi{is_})'
    if n in ('IADDIU', 'ISUBIU'):
        return f'{n} vi{it}, vi{is_}, 0x{((w >> 10) & 0x7800) | (w & 0x7FF):x}'
    if n in ('FCEQ', 'FCSET', 'FCAND', 'FCOR'): return f'{n} vi1, 0x{w & 0xFFFFFF:06x}'
    if n in ('FSEQ', 'FSSET', 'FSAND', 'FSOR'): return f'{n} vi{it}, 0x{((w >> 10) & 0x800) | (w & 0x7FF):03x}'
    if n in ('FMEQ', 'FMAND', 'FMOR'): return f'{n} vi{it}, vi{is_}'
    if n == 'FCGET': return f'FCGET vi{it}'
    tgt = (pc + 1 + imm11(w)) * 8
    if n == 'B': return f'B 0x{tgt:04x}'
    if n == 'BAL': return f'BAL vi{it}, 0x{tgt:04x}'
    if n == 'JR': return f'JR vi{is_}'
    if n == 'JALR': return f'JALR vi{it}, vi{is_}'
    if n in ('IBEQ', 'IBNE'): return f'{n} vi{it}, vi{is_}, 0x{tgt:04x}'
    if n.startswith('IB'): return f'{n} vi{is_}, 0x{tgt:04x}'
    return n


def main():
    code = open(sys.argv[1], 'rb').read()
    start = int(sys.argv[2], 16) // 8
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 64
    for pc in range(start, min(start + count, len(code) // 8)):
        lo, hi = struct.unpack_from('<II', code, pc * 8)
        flags = ''.join(c for b, c in ((31, 'I'), (30, 'E'), (29, 'M'), (28, 'D'), (27, 'T')) if hi >> b & 1)
        lo_s = f'loi 0x{lo:08x}' if hi >> 31 & 1 else lower(lo, pc)
        print(f'{pc * 8:04x}: {flags:2s} {upper(hi):34s} {lo_s}')


if __name__ == '__main__':
    main()
