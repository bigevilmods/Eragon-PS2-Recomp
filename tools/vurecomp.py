"""Static translator for VU microcode pages -> C (out/vu_pages.inc).

The runtime (PS2_VUDUMP=1) saves every 64-instruction page of VU code memory it sees as
vu_pages/u<unit>_p<page>_<hash>.bin. This tool turns each page into a C function with the same
semantics as interp_step() in runtime/vu1.c (cycle model, flag pipeline, delay slots, E bit), but
with decoding, hazard masks and dispatch resolved at build time. vu1.c #includes the output and picks
pages by content hash whenever code memory changes; unknown pages fall back to the interpreter.

  python -I tools/vurecomp.py vu_pages out/vu_pages.inc
"""
import os, re, struct, sys

K_ADD, K_SUB, K_MADD, K_MSUB, K_MAX, K_MIN, K_MUL = range(7)
S_VEC, S_BC, S_Q, S_I = range(4)
KN = ['K_ADD', 'K_SUB', 'K_MADD', 'K_MSUB', 'K_MAX', 'K_MIN', 'K_MUL']
SN = ['S_VEC', 'S_BC', 'S_Q', 'S_I']


def fnv(b):
    h = 1469598103934665603
    for c in b:
        h = ((h ^ c) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


# ---------------------------------------------------------------- decode (mirrors vu1.c)
def upper_hazard(w):
    op, dest, bc = w & 63, (w >> 21) & 15, w & 3
    fs, ft = (w >> 11) & 31, (w >> 16) & 31
    fsm = ftm = accm = 0
    if op < 0x1C:
        fsm, ftm = dest, 8 >> bc
        if (op >> 2) in (2, 3): accm = dest
    elif op < 0x28:
        fsm = dest
        if op in (0x21, 0x23, 0x25, 0x27): accm = dest
    elif op < 0x30:
        if op == 0x2E: fsm = ftm = accm = 0xE
        else:
            fsm = ftm = dest
            if op in (0x29, 0x2D): accm = dest
    elif op < 0x3C:
        pass
    else:
        sp = ((w >> 4) & 0x7C) | (w & 3)
        if sp < 0x10:
            fsm, ftm = dest, 8 >> bc
            if (sp >> 2) >= 2: accm = dest
        elif sp < 0x18 or sp == 0x1D: fsm = dest
        elif sp < 0x1C: fsm, ftm = dest, 8 >> bc
        elif sp in (0x1C, 0x1E): fsm = dest
        elif sp == 0x1F: fsm, ftm = 0xE, 0x1
        elif sp < 0x28:
            fsm = dest
            if sp in (0x21, 0x23, 0x25, 0x27): accm = dest
        elif sp < 0x2E:
            fsm = ftm = dest
            if sp in (0x29, 0x2D): accm = dest
        elif sp == 0x2E: fsm = ftm = 0xE
    return dict(fs=fs, fsm=fsm, ft=ft, ftm=ftm, accm=accm, wr=0, wrm=0, q=0, p=0)


def lower_hazard(w):
    ft, fs, dest = (w >> 16) & 31, (w >> 11) & 31, (w >> 21) & 15
    fsf, ftf = 8 >> ((w >> 21) & 3), 8 >> ((w >> 23) & 3)
    op = w >> 25
    h = dict(fs=fs, fsm=0, ft=ft, ftm=0, accm=0, wr=0, wrm=0, q=0, p=0)
    if op == 0x00: h.update(wr=ft, wrm=dest); return h
    if op == 0x01: h.update(fsm=dest); return h
    if op != 0x40 or (w & 63) < 0x3C: return h
    sp = ((w >> 4) & 0x7C) | (w & 3)
    if sp == 0x30: h.update(fsm=dest, wr=ft, wrm=dest)
    elif sp == 0x31: h.update(fsm=0xF, wr=ft, wrm=dest)
    elif sp in (0x34, 0x36): h.update(wr=ft, wrm=dest)
    elif sp in (0x35, 0x37): h.update(fsm=dest)
    elif sp in (0x38, 0x3A): h.update(fsm=fsf, ftm=ftf, q=1)
    elif sp == 0x39: h.update(ftm=ftf, q=1)
    elif sp == 0x3B: h.update(q=2)
    elif sp in (0x3C, 0x42, 0x43): h.update(fsm=fsf)
    elif sp in (0x3D, 0x40, 0x41, 0x64): h.update(wr=ft, wrm=dest)
    elif sp in (0x70, 0x71, 0x72, 0x73): h.update(fsm=0xE, p=1)
    elif sp == 0x74: h.update(fsm=0xC, p=1)
    elif sp == 0x75: h.update(fsm=0xA, p=1)
    elif sp == 0x76: h.update(fsm=0xF, p=1)
    elif sp in (0x78, 0x79, 0x7A, 0x7C, 0x7D, 0x7E): h.update(fsm=fsf, p=1)
    elif sp == 0x7B: h.update(p=2)
    return h


def fmac_class(w):
    op = w & 63
    bck = [K_ADD, K_SUB, K_MADD, K_MSUB, K_MAX, K_MIN, K_MUL]
    if op < 0x1C: return bck[op >> 2], S_BC, 0
    if op < 0x30 and op != 0x2E:
        k1 = [K_MUL, K_MAX, K_MUL, K_MIN, K_ADD, K_MADD, K_ADD, K_MADD, K_SUB, K_MSUB, K_SUB, K_MSUB,
              K_ADD, K_MADD, K_MUL, K_MAX, K_SUB, K_MSUB, 0, K_MIN]
        s1 = [S_Q, S_I, S_I, S_I, S_Q, S_Q, S_I, S_I, S_Q, S_Q, S_I, S_I,
              S_VEC, S_VEC, S_VEC, S_VEC, S_VEC, S_VEC, 0, S_VEC]
        return k1[op - 0x1C], s1[op - 0x1C], 0
    if op < 0x3C: return None
    sp = ((w >> 4) & 0x7C) | (w & 3)
    if sp < 0x10: return [K_ADD, K_SUB, K_MADD, K_MSUB][sp >> 2], S_BC, 1
    if 0x18 <= sp <= 0x1B: return K_MUL, S_BC, 1
    t = {0x1C: (K_MUL, S_Q), 0x1E: (K_MUL, S_I), 0x20: (K_ADD, S_Q), 0x21: (K_MADD, S_Q), 0x22: (K_ADD, S_I),
         0x23: (K_MADD, S_I), 0x24: (K_SUB, S_Q), 0x25: (K_MSUB, S_Q), 0x26: (K_SUB, S_I), 0x27: (K_MSUB, S_I),
         0x28: (K_ADD, S_VEC), 0x29: (K_MADD, S_VEC), 0x2A: (K_MUL, S_VEC), 0x2C: (K_SUB, S_VEC), 0x2D: (K_MSUB, S_VEC)}
    if sp in t: return t[sp][0], t[sp][1], 1
    return None


def imm11(w):
    v = w & 0x7FF
    return v - 0x800 if v & 0x400 else v


def fields(mask):
    return [i for i in range(4) if mask & (8 >> i)]


# ---------------------------------------------------------------- lower op emission
def emit_lower(lo, pc):
    """C statements for the lower op (sets `taken`/`tgt` for branches). None -> generic lower()."""
    ft, fs, fd, dest = (lo >> 16) & 31, (lo >> 11) & 31, (lo >> 6) & 31, (lo >> 21) & 15
    it, is_, id_ = ft & 15, fs & 15, fd & 15
    op = lo >> 25
    br = pc + 1 + imm11(lo)

    def wr_vf(reg, src):
        if not reg: return ''
        return ' '.join(f'v->vf[{reg}].uw[{i}] = {src}.uw[{i}];' for i in fields(dest))

    if op == 0x40:
        f = lo & 63
        if f == 0x30: return f'set_vi(v, {id_}, v->vi[{is_}] + v->vi[{it}]);'
        if f == 0x31: return f'set_vi(v, {id_}, v->vi[{is_}] - v->vi[{it}]);'
        if f == 0x32:
            im = (lo >> 6) & 31
            if im & 16: im -= 32
            return f'set_vi(v, {it}, v->vi[{is_}] + ({im}));'
        if f == 0x34: return f'set_vi(v, {id_}, v->vi[{is_}] & v->vi[{it}]);'
        if f == 0x35: return f'set_vi(v, {id_}, v->vi[{is_}] | v->vi[{it}]);'
        if f < 0x3C: return ''
        sp = ((lo >> 4) & 0x7C) | (lo & 3)
        fsf = (lo >> 21) & 3
        if sp == 0x30: return f'{{ u128 s_ = v->vf[{fs}]; {wr_vf(ft, "s_")} }}' if ft else ''
        if sp == 0x31:
            if not ft: return ''
            return (f'{{ u128 s_ = v->vf[{fs}], r_; r_.uw[0] = s_.uw[1]; r_.uw[1] = s_.uw[2]; r_.uw[2] = s_.uw[3]; '
                    f'r_.uw[3] = s_.uw[0]; {wr_vf(ft, "r_")} }}')
        if sp == 0x34: return f'{{ u128 r_; memcpy(&r_, qw(v, v->vi[{is_}]), 16); {wr_vf(ft, "r_")} set_vi(v, {is_}, v->vi[{is_}] + 1); }}'
        if sp == 0x36: return f'{{ u128 r_; set_vi(v, {is_}, v->vi[{is_}] - 1); memcpy(&r_, qw(v, v->vi[{is_}]), 16); {wr_vf(ft, "r_")} }}'
        if sp in (0x35, 0x37):
            st = ' '.join(f'memcpy(m_ + {4 * i}, &s_.uw[{i}], 4);' for i in fields(dest))
            if sp == 0x35: return f'{{ u128 s_ = v->vf[{fs}]; u8 *m_ = qw(v, v->vi[{it}]); {st} set_vi(v, {it}, v->vi[{it}] + 1); }}'
            return f'{{ u128 s_ = v->vf[{fs}]; set_vi(v, {it}, v->vi[{it}] - 1); u8 *m_ = qw(v, v->vi[{it}]); {st} }}'
        if sp in (0x3B, 0x7B): return ''                                   # WAITQ/WAITP: stall handled above
        if sp == 0x3C: return f'set_vi(v, {it}, v->vf[{fs}].uw[{fsf}]);'
        if sp == 0x3D: return ' '.join(f'v->vf[{ft}].sw[{i}] = (s16)v->vi[{is_}];' for i in fields(dest)) if ft else ''
        if sp == 0x68: return f'set_vi(v, {it}, v->unit ? vif1_top() : 0);'
        if sp == 0x69: return f'set_vi(v, {it}, v->unit ? vif1_itop() : 0);'
        if sp == 0x6C: return f'if (v->unit) xgkick(v, v->vi[{is_}]);'
        return None
    if op == 0x00:
        return f'{{ u128 r_; memcpy(&r_, qw(v, v->vi[{is_}] + ({imm11(lo)})), 16); {wr_vf(ft, "r_")} }}' if ft else ''
    if op == 0x01:
        st = ' '.join(f'memcpy(m_ + {4 * i}, &s_.uw[{i}], 4);' for i in fields(dest))
        return f'{{ u8 *m_ = qw(v, v->vi[{it}] + ({imm11(lo)})); u128 s_ = v->vf[{fs}]; {st} }}'
    if op == 0x04:
        fl = fields(dest)
        if not fl: return ''
        return f'{{ u32 x_; memcpy(&x_, qw(v, v->vi[{is_}] + ({imm11(lo)})) + {4 * fl[0]}, 4); set_vi(v, {it}, x_); }}'
    if op == 0x05:
        st = ' '.join(f'memcpy(m_ + {4 * i}, &x_, 4);' for i in fields(dest))
        return f'{{ u8 *m_ = qw(v, v->vi[{is_}] + ({imm11(lo)})); u32 x_ = v->vi[{it}] & 0xFFFF; {st} }}'
    if op in (0x08, 0x09):
        im = ((lo >> 10) & 0x7800) | (lo & 0x7FF)
        return f'set_vi(v, {it}, v->vi[{is_}] {"+" if op == 0x08 else "-"} {im}u);'
    if op == 0x11: return f'v->clip = 0x{lo & 0xFFFFFF:06x};'
    if op == 0x12: return f'set_vi(v, 1, (v->clip & 0x{lo & 0xFFFFFF:06x}) != 0);'
    if op == 0x13: return f'set_vi(v, 1, ((v->clip | 0x{lo & 0xFFFFFF:06x}) & 0xFFFFFF) == 0xFFFFFF);'
    if op == 0x1A: return f'set_vi(v, {it}, v->mac & v->vi[{is_}]);'
    if op == 0x1C: return f'set_vi(v, {it}, v->clip & 0xFFF);'
    if op == 0x20: return f'taken = 1; tgt = {br};'
    if op == 0x21: return f'set_vi(v, {it}, {pc + 2}); taken = 1; tgt = {br};'
    if op == 0x24: return f'taken = 1; tgt = v->vi[{is_}];'
    if op == 0x25: return f'{{ u32 t_ = v->vi[{is_}]; set_vi(v, {it}, {pc + 2}); taken = 1; tgt = t_; }}'
    if op == 0x28: return f'if ((v->vi[{it}] & 0xFFFF) == (v->vi[{is_}] & 0xFFFF)) {{ taken = 1; tgt = {br}; }}'
    if op == 0x29: return f'if ((v->vi[{it}] & 0xFFFF) != (v->vi[{is_}] & 0xFFFF)) {{ taken = 1; tgt = {br}; }}'
    if op == 0x2C: return f'if ((s16)v->vi[{is_}] < 0) {{ taken = 1; tgt = {br}; }}'
    if op == 0x2D: return f'if ((s16)v->vi[{is_}] > 0) {{ taken = 1; tgt = {br}; }}'
    if op == 0x2E: return f'if ((s16)v->vi[{is_}] <= 0) {{ taken = 1; tgt = {br}; }}'
    if op == 0x2F: return f'if ((s16)v->vi[{is_}] >= 0) {{ taken = 1; tgt = {br}; }}'
    return None


# ---------------------------------------------------------------- instruction emission
BRANCH_OPS = {0x20, 0x21, 0x24, 0x25, 0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F}
MAC_READERS = {0x18, 0x1A, 0x1B}            # FMEQ, FMAND, FMOR


def page_info(data):
    """Per instruction of a page: (lo, hi, branchy, ebit, mac_reader, mac_producer)."""
    info = []
    for i in range(64):
        lo, hi = struct.unpack_from('<II', data, i * 8)
        ibit, ebit = (hi >> 31) & 1, (hi >> 30) & 1
        op = None if ibit else lo >> 25
        unop = (hi & 0x7FF) == 0x2FF
        fc = None if unop else fmac_class(hi)
        info.append(dict(lo=lo, hi=hi, branchy=op in BRANCH_OPS, ebit=bool(ebit),
                         reader=op in MAC_READERS, producer=bool(fc) and fc[0] not in (K_MAX, K_MIN)))
    return info


def mac_dead(info, i):
    """The MAC result of instruction i can never be observed by FMEQ/FMAND/FMOR: a later producer j on the
    same branch-free path replaces it, and no reader runs up to 3 instructions after j (flags become
    visible 4 cycles after issue and every instruction takes at least one cycle)."""
    if not info[i]['producer'] or info[i]['branchy'] or info[i]['ebit']:
        return False
    for j in range(i + 1, 64):
        if info[j]['reader'] or info[j]['branchy'] or info[j]['ebit']:
            return False
        if info[j]['producer']:
            if j + 3 >= 64:
                return False
            for k in range(j + 1, j + 4):
                if info[k]['reader'] or info[k]['branchy'] or info[k]['ebit']:
                    return False
            return True
    return False


def emit_insn(lo, hi, pc, page_lo, page_hi, out, macdead=False):
    ibit, ebit = (hi >> 31) & 1, (hi >> 30) & 1
    hu = upper_hazard(hi)
    hl = lower_hazard(lo) if not ibit else dict(fs=0, fsm=0, ft=0, ftm=0, accm=0, wr=0, wrm=0, q=0, p=0)
    lnop = ibit or lo == 0x8000033C
    unop = (hi & 0x7FF) == 0x2FF
    fc = None if unop else fmac_class(hi)
    dest, ft, fs, fd, bc = (hi >> 21) & 15, (hi >> 16) & 31, (hi >> 11) & 31, (hi >> 6) & 31, hi & 3
    o = out.append
    o(f'L_{pc}: {{  /* {hi:08x} {lo:08x} */')
    o(f'    if (x->steps >= 4000000) {{ x->pc = {pc}; return; }}')
    # stall
    stall = []
    for reg, m in ((hu['fs'], hu['fsm']), (hu['ft'], hu['ftm']), (hl['fs'], hl['fsm']), (hl['ft'], hl['ftm'])):
        if reg and m:
            for i in fields(m): stall.append(f'if (v->vf_ready[{reg}][{i}] > t_) t_ = v->vf_ready[{reg}][{i}];')
    # no stall on ACC: the accumulate path is forwarded (see interp_step in vu1.c)
    if hl['q']: stall.append('if (v->q_wait && v->q_ready > t_) t_ = v->q_ready;')
    if hl['p']: stall.append('if (v->p_wait && v->p_ready > t_) t_ = v->p_ready;')
    if stall: o('    { u64 t_ = v->cyc; ' + ' '.join(stall) + ' v->cyc = t_; }')
    o('    if (v->cyc >= v->next_event) pipes_advance(v);')
    # upper
    has_upper = not unop
    if fc:
        k, s, acc = fc
        o(f'    fmac_sse(v, {KN[k]}, {SN[s]}, {acc}, {dest}, {fd}, {fs}, {ft}, {bc}, &ur, {1 if macdead else 0});')
    elif has_upper:
        o(f'    upper(v, 0x{hi:08x}u, &ur);')
    # lower
    branchy = False
    if ibit:
        o(f'    v->vi[VI_I] = 0x{lo:08x}u;')
    elif not lnop:
        code = emit_lower(lo, pc)
        if code is None:
            o(f'    taken = lower(v, 0x{lo:08x}u, {pc}, &tgt);')
            branchy = True
        elif code:
            o('    ' + code)
            branchy = 'taken' in code
    # commit + flags + readiness
    if fc:
        k, s, acc = fc
        o('    commit_upper(v, &ur);')
        if k not in (K_MAX, K_MIN): o('    if (v->new_mac_valid) flags_push(v);' if macdead else '    flags_push(v);')
        tgt_ready = 'v->acc_ready' if acc else (f'v->vf_ready[{fd}]' if fd else None)
        if tgt_ready:
            o('    ' + ' '.join(f'{tgt_ready}[{i}] = v->cyc + 4;' for i in fields(dest)))
    elif has_upper:
        o('    commit_upper(v, &ur);')
        o('    if (v->new_mac_valid | v->new_clip_valid) flags_push(v);')
        o('    if (ur.valid) { if (ur.to_acc) { for (int i = 0; i < 4; i++) if (HAS(ur.dest, i)) v->acc_ready[i] = v->cyc + 4; } '
          'else if (ur.reg) { for (int i = 0; i < 4; i++) if (HAS(ur.dest, i)) v->vf_ready[ur.reg][i] = v->cyc + 4; } }')
    if hl['wr'] and hl['wrm']:
        o('    ' + ' '.join(f'v->vf_ready[{hl["wr"]}][{i}] = v->cyc + 4;' for i in fields(hl['wrm'])))
    # control
    o('    v->cyc++; x->steps++;')
    o(f'    pc = {pc + 1};')
    o('    if (x->in_delay) { pc = x->delay_target; x->in_delay = 0; }')
    if branchy: o('    if (taken) { x->delay_target = tgt; x->in_delay = 1; taken = 0; }')
    o('    if (x->stop_after) { x->pc = pc; x->done = 1; return; }')
    if ebit: o('    x->stop_after = 1;')
    if pc + 1 < page_hi:
        o(f'    if (pc == {pc + 1}) goto L_{pc + 1};')
    o('    goto dispatch;')
    o('}')


def emit_page(unit, page, h, data, out):
    code_n = 2048 if unit else 512
    lo_pc, hi_pc = page * 64, page * 64 + 64
    name = f'vp_u{unit}_p{page:02d}_{h:016x}'
    out.append(f'static void {name}(Vu *v, VuExec *x) {{')
    out.append('    u32 pc = x->pc; int taken = 0; u32 tgt = 0; UpperResult ur; (void)tgt; (void)taken; (void)ur;')
    out.append('dispatch:')
    out.append(f'    pc &= {code_n - 1};')
    out.append('    switch (pc) {')
    for i in range(64):
        out.append(f'    case {lo_pc + i}: goto L_{lo_pc + i};')
    out.append('    default: x->pc = pc; return;')
    out.append('    }')
    info = page_info(data)
    for i in range(64):
        lo, hi = info[i]['lo'], info[i]['hi']
        emit_insn(lo, hi, lo_pc + i, lo_pc, hi_pc, out, mac_dead(info, i))
    out.append('}')
    return name


def main():
    src, dst = sys.argv[1], sys.argv[2]
    pages = []
    for fn in sorted(os.listdir(src)):
        m = re.match(r'u(\d)_p(\d+)_([0-9a-f]{16})\.bin$', fn)
        if not m: continue
        data = open(os.path.join(src, fn), 'rb').read()
        h = fnv(data)
        if h != int(m.group(3), 16):
            print('hash mismatch, skipping', fn); continue
        pages.append((int(m.group(1)), int(m.group(2)), h, data))
    out = ['/* Generated by tools/vurecomp.py from %d VU microcode pages. Do not edit. */' % len(pages)]
    names = []
    for unit, page, h, data in pages:
        names.append((unit, page, h, emit_page(unit, page, h, data, out)))
    out.append(f'static const VuPageEntry g_vu_page_table[{max(1, len(names))}] = {{')
    for unit, page, h, name in names:
        out.append(f'    {{{unit}, {page}, 0x{h:016x}ull, {name}}},')
    if not names: out.append('    {0, 0, 0, 0},')
    out.append('};')
    out.append(f'#define G_VU_NPAGES {len(names)}')
    open(dst, 'w').write('\n'.join(out) + '\n')
    print(f'{len(names)} pages -> {dst}')


if __name__ == '__main__':
    main()
