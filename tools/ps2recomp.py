"""PS2 EE static recompiler: ELF -> C.

Usage: python -I ps2recomp.py <SLUS_xxx.xx> <outdir>

Pipeline:
  1. load PT_LOAD segments into a flat image
  2. discover functions (entry, jal targets, code pointers, lui/addiu pairs,
     post-`jr ra` gaps) and walk each body by recursive descent
  3. emit one C function per guest function; delay slots folded into branches
  4. emit a sorted address->function table for indirect calls (jalr / jr)
"""
import json, os, struct, sys
from collections import Counter
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from r5900 import decode, branch_target

JR_RA = 0x03E00008
FUNCS_PER_FILE = 400


class Image:
    def __init__(self, path):
        d = open(path, 'rb').read()
        self.entry, phoff = struct.unpack_from('<II', d, 24)
        phentsize, phnum = struct.unpack_from('<HH', d, 42)
        self.segs = []
        for i in range(phnum):
            p = struct.unpack_from('<8I', d, phoff + i * phentsize)
            if p[0] == 1:
                self.segs.append((p[2], d[p[1]:p[1] + p[4]], p[5]))  # vaddr, bytes, memsz
        self.lo = min(s[0] for s in self.segs)
        self.hi = max(s[0] + s[2] for s in self.segs)

    def word(self, a):
        for base, data, _ in self.segs:
            if base <= a and a + 4 <= base + len(data):
                return struct.unpack_from('<I', data, a - base)[0]
        return None

    def words(self):
        for base, data, _ in self.segs:
            for off in range(0, len(data) - 3, 4):
                yield base + off, struct.unpack_from('<I', data, off)[0]


class Analyzer:
    def __init__(self, img):
        self.img = img
        self.cache = {}
        base, data, _ = img.segs[0]
        self.code_lo, self.code_hi = base, base + len(data)
        self.code_hi = self.find_text_end()
        self.ranges = [(self.code_lo, self.code_hi)]   # + extra ranges from regions.txt
        self.funcs = {}       # start -> dict(end, labels, jumptables, calls, bad)
        self.starts = set()

    def add_range(self, lo, hi):
        self.ranges.append((lo, hi))
        self.ranges.sort()

    def range_of(self, a):
        for lo, hi in self.ranges:
            if lo <= a < hi:
                return lo, hi
        return None

    def insn(self, a):
        if a not in self.cache:
            w = self.img.word(a)
            self.cache[a] = None if w is None else decode(w)
        return self.cache[a]

    def find_text_end(self):
        """.text has no section header; stop at the first 256-byte block containing
        undecodable words, then back up to just after the last `jr ra` + delay slot."""
        for blk in range(self.code_lo, self.code_hi, 256):
            if any(self.insn(a) is None or self.insn(a).mnem == 'invalid'
                   for a in range(blk, min(blk + 256, self.code_hi), 4)):
                a = blk
                while a > self.code_lo and self.img.word(a - 8) != JR_RA:
                    a -= 4
                return a
        return self.code_hi

    def in_code(self, a):
        return not a & 3 and self.range_of(a) is not None

    # --- seeds -----------------------------------------------------------
    def looks_like_func(self, a):
        if not self.in_code(a):
            return False
        w = self.img.word(a)
        if w is None:
            return False
        if (w >> 16) == 0x27BD and (w & 0x8000):  # addiu sp,sp,-N
            return True
        return self.img.word(a - 8) == JR_RA      # starts right after a return + delay slot

    def seed_pointers(self):
        new = set()
        for a, w in self.img.words():
            if self.looks_like_func(w):
                new.add(w)
        return new

    def seed_lui_pairs(self, lo, hi):
        """lui rX,hi ; addiu rY,rX,lo forming a code address (thread entries, callbacks)."""
        new = set()
        lui = {}
        for a in range(lo, hi, 4):
            i = self.insn(a)
            if i is None:
                continue
            if i.mnem == 'lui':
                lui[i.ops[0]] = (i.ops[1] << 16, a)
            elif i.mnem in ('addiu', 'ori') and i.ops[1] in lui and a - lui[i.ops[1]][1] <= 32:
                val = lui[i.ops[1]][0]
                val = (val + i.ops[2]) & 0xffffffff if i.mnem == 'addiu' else val | (i.ops[2] & 0xffff)
                if self.looks_like_func(val):
                    new.add(val)
        return new

    # --- body walk -------------------------------------------------------
    def jump_table(self, pc):
        """Recognise GCC switch tables: sltiu t,idx,N ; ... lui b,%hi ; addu ; lw r,%lo(b) ; jr r."""
        count = base_hi = lw_off = None
        addiu_lo = 0
        for back in range(1, 16):
            i = self.insn(pc - 4 * back)
            if i is None or i.is_branch and back > 1:
                if i is not None and i.mnem in ('beq', 'bne', 'beql', 'bnel'):
                    continue  # the bounds-check branch
                break
            if i.mnem == 'lw' and lw_off is None:
                lw_off = i.ops[2]
            elif i.mnem == 'addiu' and lw_off is not None and base_hi is None and i.ops[0] == i.ops[1]:
                addiu_lo = i.ops[2]
            elif i.mnem == 'lui' and lw_off is not None and base_hi is None:
                base_hi = i.ops[1] << 16
            elif i.mnem == 'sltiu' and count is None:
                count = i.ops[2] & 0xffff
                if base_hi is not None:
                    break
        if base_hi is None or lw_off is None:
            return None
        table = (base_hi + addiu_lo + lw_off) & 0xffffffff
        n = count if count and count <= 1024 else 256
        targets = []
        for k in range(n):
            t = self.img.word(table + 4 * k)
            if t is None or not self.in_code(t):
                break
            targets.append(t)
        return (table, targets) if targets else None

    def walk(self, start, next_start):
        labels, jts, calls, tails = set(), {}, set(), set()
        seen, work, bad, end = set(), [start], False, start
        rng_hi = self.range_of(start)[1]
        limit = min(next_start, rng_hi) if next_start else rng_hi
        while work:
            pc = work.pop()
            while pc not in seen:
                if pc < start or pc >= rng_hi or self.insn(pc) is None:
                    bad = True          # left the code range
                    break
                if pc >= limit:
                    break               # falls through into the next function: emitted as a jump there
                seen.add(pc)
                i = self.insn(pc)
                end = max(end, pc + 4)
                if i.mnem == 'invalid':
                    bad = True
                    break
                if not i.is_branch:
                    pc += 4
                    continue
                end = max(end, pc + 8)
                seen.add(pc + 4)
                t = branch_target(pc, i)
                if i.mnem == 'jal':
                    calls.add(t)
                    pc += 8
                    continue
                if i.mnem == 'jalr':
                    pc += 8
                    continue
                if i.mnem == 'jr':
                    if i.ops[0] != 31:
                        jt = self.jump_table(pc)
                        if jt:
                            jts[pc] = jt
                            for tt in jt[1]:
                                if start <= tt < limit:
                                    labels.add(tt)
                                    work.append(tt)
                    break
                if i.mnem == 'j' or (i.fmt == 'br2' and i.ops[0] == i.ops[1] and i.mnem == 'beq'):
                    if start <= t < limit and t not in self.starts:
                        labels.add(t)
                        pc = t
                        continue
                    tails.add(t)
                    break
                # conditional branch
                if start <= t < limit and t not in self.starts:
                    labels.add(t)
                    work.append(t)
                else:
                    tails.add(t)
                pc += 8
        return dict(end=end, labels=labels, jumptables=jts, calls=calls, tails=tails, bad=bad)

    def run(self, extra=()):
        img = self.img
        self.starts = {img.entry} | set(extra) | {lo for lo, _ in self.ranges[1:]}
        # pass 0: crude code end from pointer seeds before any walking
        self.starts |= self.seed_pointers()
        for it in range(64):
            ordered = sorted(s for s in self.starts if self.in_code(s))
            self.funcs = {}
            new = set()
            for idx, s in enumerate(ordered):
                nxt = ordered[idx + 1] if idx + 1 < len(ordered) else None
                f = self.walk(s, nxt)
                self.funcs[s] = f
                for c in f['calls'] | f['tails']:
                    if self.in_code(c) and c not in self.starts:
                        new.add(c)
            # gaps: code after a function that nobody reached
            ordered = sorted(self.funcs)
            for idx, s in enumerate(ordered):
                e = self.funcs[s]['end']
                nxt = ordered[idx + 1] if idx + 1 < len(ordered) else None
                hi = self.range_of(s)[1]
                nxt = min(nxt, hi) if nxt else hi
                a = e
                while nxt and a < nxt and img.word(a) == 0:
                    a += 4
                if nxt and a < nxt and a not in self.starts:
                    i = self.insn(a)
                    if i and i.mnem != 'invalid':
                        new.add(a)
            if it == 0:
                for lo, hi in self.ranges:
                    new |= self.seed_lui_pairs(lo, hi)
            print(f'  pass {it}: {len(self.funcs)} funcs, {len(new)} new', file=sys.stderr)
            if not new:
                break
            self.starts |= new
        good = [f for f in self.funcs.values() if not f['bad']]
        # functions that ran into data are dropped
        self.funcs = {s: f for s, f in self.funcs.items() if not f['bad'] or s == img.entry}


# ---------------------------------------------------------------- emission
def cname(a):
    return f'f_{a:08x}'


class Emitter:
    def __init__(self, an):
        self.an = an
        self.stats = Counter()
        self.unimpl = Counter()

    def op(self, pc, i):
        """C statement for a non-branch instruction."""
        m = i.mnem
        self.stats[m] += 1
        if m in ('sll',) and i.word == 0:
            return ''  # nop
        if m in ('sync', 'cache', 'pref'):
            return ''
        name = 'OP_' + m.upper().replace('.', '_')
        if m == 'invalid':
            self.unimpl[f'{i.word:08x}'] += 1
            return f'ps2_invalid(c, 0x{pc:08x}, 0x{i.word:08x});'
        # OP_* macros reach the context through the enclosing function's `c`.
        if i.fmt in ('none', 'raw'):
            return f'{name}(0x{pc:08x}, 0x{i.word:08x});'
        if m in ('syscall', 'break'):
            return f'{name}(0x{pc:08x}, 0x{i.ops[0]:x});'
        args = ', '.join(str(x) for x in i.ops)
        return f'{name}({args});'

    def cond(self, i):
        o = i.ops
        R = lambda r: f'GPR_S64({r})'
        k = i.mnem
        if k in ('beq', 'beql'): return f'{R(o[0])} == {R(o[1])}'
        if k in ('bne', 'bnel'): return f'{R(o[0])} != {R(o[1])}'
        if k in ('blez', 'blezl'): return f'{R(o[0])} <= 0'
        if k in ('bgtz', 'bgtzl'): return f'{R(o[0])} > 0'
        if k in ('bltz', 'bltzl', 'bltzal', 'bltzall'): return f'{R(o[0])} < 0'
        if k in ('bgez', 'bgezl', 'bgezal', 'bgezall'): return f'{R(o[0])} >= 0'
        if k in ('bc1t', 'bc1tl'): return 'FPU_COND'
        if k in ('bc1f', 'bc1fl'): return '!FPU_COND'
        if k in ('bc0t', 'bc0tl'): return 'COP0_COND'
        if k in ('bc0f', 'bc0fl'): return '!COP0_COND'
        if k in ('bc2t', 'bc2tl'): return 'VU0_COND'
        if k in ('bc2f', 'bc2fl'): return '!VU0_COND'
        raise ValueError(k)

    def goto(self, start, f, t, pc=None):
        if t in f['labels'] and start <= t < f['end']:
            # backward edge = potential busy-wait: give the runtime a chance to deliver interrupts
            poll = f'PS2_POLL(0x{t:08x}); ' if pc is not None and t <= pc else ''
            return f'{poll}goto L_{t:08x};'
        if t in self.an.funcs:
            return f'{cname(t)}(c); return;'
        return f'ps2_jump(c, 0x{t:08x}); return;'

    def function(self, start, f):
        out = [f'void {cname(start)}(R5900Context *c) {{']
        pc = start
        end = f['end']
        while pc < end:
            i = self.an.insn(pc)
            if pc in f['labels']:
                out.append(f'L_{pc:08x}:;')
            if i is None:
                break
            if not i.is_branch:
                s = self.op(pc, i)
                if s:
                    out.append('    ' + s)
                pc += 4
                continue
            self.stats[i.mnem] += 1
            ds = self.an.insn(pc + 4)
            dss = self.op(pc + 4, ds) if ds and not ds.is_branch else ''
            t = branch_target(pc, i)
            m = i.mnem
            if m == 'jal':
                callee = cname(t) + '(c);' if t in self.an.funcs else f'ps2_call(c, 0x{t:08x});'
                out.append(f'    {dss} SET_GPR_U64(31, 0x{pc + 8:08x}); {callee}')
            elif m == 'jalr':
                rd, rs = i.ops
                out.append(f'    {{ u32 t = GPR_U32({rs}); {dss} SET_GPR_U64({rd}, 0x{pc + 8:08x}); ps2_call(c, t); }}')
            elif m == 'jr':
                rs = i.ops[0]
                if rs == 31:
                    out.append(f'    {dss} return;')
                elif pc in f['jumptables']:
                    table, targets = f['jumptables'][pc]
                    cases = ' '.join(f'case 0x{tt:08x}: goto L_{tt:08x};' for tt in sorted(set(targets))
                                     if tt in f['labels'])
                    out.append(f'    {{ u32 t = GPR_U32({rs}); {dss} switch (t) {{ {cases} default: ps2_jump(c, t); return; }} }}')
                else:
                    out.append(f'    {{ u32 t = GPR_U32({rs}); {dss} ps2_jump(c, t); return; }}')
            elif m == 'j':
                out.append(f'    {dss} {self.goto(start, f, t, pc)}')
            else:
                link = f' SET_GPR_U64(31, 0x{pc + 8:08x});' if m in ('bltzal', 'bgezal', 'bltzall', 'bgezall') else ''
                if i.likely:
                    out.append(f'    if ({self.cond(i)}) {{{link} {dss} {self.goto(start, f, t, pc)} }}')
                else:
                    out.append(f'    {{ int b = {self.cond(i)};{link} {dss} if (b) {{ {self.goto(start, f, t, pc)} }} }}')
            if pc + 4 in f['labels']:  # something jumps into the delay slot
                out.append(f'    if (0) {{ L_{pc + 4:08x}:; {dss} }}')
            pc += 8
        # fell off the end: continue into the next function (rare; e.g. crt0)
        out.append(f'    ps2_jump(c, 0x{end:08x});')
        out.append('}')
        return '\n'.join(out)


def main(elf, outdir, extra_file=None):
    os.makedirs(outdir, exist_ok=True)
    img = Image(elf)
    print(f'entry 0x{img.entry:08x}, image 0x{img.lo:08x}-0x{img.hi:08x}', file=sys.stderr)
    an = Analyzer(img)
    # Extra entry points, one hex address per line (from runtime "no function starts at" errors).
    extra = []
    if extra_file and os.path.exists(extra_file):
        extra = [int(l.split('#')[0], 16) for l in open(extra_file) if l.split('#')[0].strip()]
    # Extra code ranges ("lo hi" hex per line) for code outside the main .text
    # (e.g. static-constructor code placed after .data).
    regions = os.path.join(os.path.dirname(extra_file or elf), 'regions.txt')
    if os.path.exists(regions):
        for l in open(regions):
            l = l.split('#')[0].split()
            if len(l) == 2:
                an.add_range(int(l[0], 16), int(l[1], 16))
    an.run(extra)
    em = Emitter(an)
    starts = sorted(an.funcs)
    with open(os.path.join(outdir, 'funcs.h'), 'w') as h:
        h.write('#pragma once\n#include "ps2_runtime.h"\n')
        for s in starts:
            h.write(f'void {cname(s)}(R5900Context *c);\n')
    nfiles = 0
    for k in range(0, len(starts), FUNCS_PER_FILE):
        with open(os.path.join(outdir, f'recomp_{nfiles:03d}.c'), 'w') as o:
            o.write('#include "funcs.h"\n#include "ps2_ops.h"\n\n')
            for s in starts[k:k + FUNCS_PER_FILE]:
                o.write(em.function(s, an.funcs[s]) + '\n\n')
        nfiles += 1
    with open(os.path.join(outdir, 'functable.c'), 'w') as o:
        o.write('#include "funcs.h"\n\nconst PS2FuncEntry ps2_func_table[] = {\n')
        for s in starts:
            o.write(f'    {{0x{s:08x}u, 0x{an.funcs[s]["end"]:08x}u, {cname(s)}}},\n')
        o.write(f'}};\nconst unsigned ps2_func_count = {len(starts)};\n')
        o.write(f'const unsigned ps2_entry_point = 0x{img.entry:08x}u;\n')
    total = sum(em.stats.values())
    report = dict(
        elf=os.path.basename(elf), entry=f'0x{img.entry:08x}',
        code_range=[f'0x{an.code_lo:08x}', f'0x{an.code_hi:08x}'],
        functions=len(starts), files=nfiles,
        bad_functions=sum(1 for f in an.funcs.values() if f['bad']),
        jump_tables=sum(len(f['jumptables']) for f in an.funcs.values()),
        instructions=total,
        mnemonics=dict(em.stats.most_common()),
        invalid_words=dict(em.unimpl.most_common(50)),
    )
    with open(os.path.join(outdir, 'report.json'), 'w') as o:
        json.dump(report, o, indent=1)
    print(json.dumps({k: v for k, v in report.items() if k not in ('mnemonics', 'invalid_words')}, indent=1))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else None)
