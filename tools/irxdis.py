"""Disassemble an IOP module (.IRX, relocatable MIPS ELF) using its symbol table and relocations.

  python -I irxdis.py FILE.IRX syms            # list symbols
  python -I irxdis.py FILE.IRX fn <name|0xaddr>  # disassemble one function
  python -I irxdis.py FILE.IRX str <0xaddr>    # string at a .text/.data-relative address
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from r5900 import decode

REG = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7',
       's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7', 't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra']


class Irx:
    def __init__(self, path):
        d = self.d = open(path, 'rb').read()
        shoff, = struct.unpack_from('<I', d, 32)
        shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 46)
        self.secs = [struct.unpack_from('<10I', d, shoff + i * shentsize) for i in range(shnum)]
        names = self.secs[shstrndx]
        self.sname = [d[names[4] + s[0]: d.index(b'\0', names[4] + s[0])].decode() for s in self.secs]
        self.byname = {n: i for i, n in enumerate(self.sname)}
        # Sections are laid out consecutively in module memory: .text, .data, .bss (addresses from sh_addr)
        self.syms = []
        st = self.secs[self.byname['.symtab']] if '.symtab' in self.byname else None
        strtab = self.secs[st[6]] if st else None
        for k in range(st[5] // 16 if st else 0):
            nm, val, size, info, other, shndx = struct.unpack_from('<IIIBBH', d, st[4] + k * 16)
            name = d[strtab[4] + nm: d.index(b'\0', strtab[4] + nm)].decode('latin1')
            addr = val + (self.secs[shndx][3] if 0 < shndx < len(self.secs) else 0)
            self.syms.append((addr, size, name, info & 15, shndx))
        self.symat = {}
        for a, s, n, t, sh in self.syms:
            if n and t in (1, 2):
                self.symat.setdefault(a, n)
        self.parse_imports()
        self.relocs = {}
        rt = self.secs[self.byname['.rel.text']] if '.rel.text' in self.byname else None
        if rt:
            symtab_sec = self.secs[rt[6]]
            for k in range(rt[5] // 8):
                off, info = struct.unpack_from('<II', d, rt[4] + k * 8)
                self.relocs[off] = (info & 0xff, info >> 8)

    # Well-known IOP library exports (subset) for readable import names.
    KNOWN = {
        'sifcmd': {4: 'sceSifInitCmd', 6: 'sceSifGetSreg', 7: 'sceSifSetSreg', 8: 'sceSifSetCmdBuffer',
                   10: 'sceSifAddCmdHandler', 11: 'sceSifRemoveCmdHandler', 12: 'sceSifSendCmd', 13: 'isceSifSendCmd',
                   14: 'sceSifInitRpc', 15: 'sceSifBindRpc', 16: 'sceSifCallRpc', 17: 'sceSifRegisterRpc',
                   18: 'sceSifCheckStatRpc', 19: 'sceSifSetRpcQueue', 20: 'sceSifGetNextRequest',
                   21: 'sceSifExecRequest', 22: 'sceSifRpcLoop', 23: 'sceSifGetOtherData', 24: 'sceSifRemoveRpc'},
        'sifman': {5: 'sceSifInit', 7: 'sceSifSetDma', 8: 'sceSifDmaStat', 29: 'sceSifCheckInit'},
        'thbase': {4: 'CreateThread', 5: 'DeleteThread', 6: 'StartThread', 7: 'StartThreadArgs', 8: 'ExitThread',
                   10: 'TerminateThread', 14: 'ChangeThreadPriority', 20: 'GetThreadId', 24: 'SleepThread',
                   25: 'WakeupThread', 26: 'iWakeupThread', 33: 'DelayThread', 34: 'GetSystemTime',
                   35: 'SetAlarm', 36: 'iSetAlarm', 37: 'CancelAlarm', 38: 'iCancelAlarm', 39: 'USec2SysClock',
                   40: 'SysClock2USec', 41: 'GetSystemStatusFlag'},
        'thsemap': {4: 'CreateSema', 5: 'DeleteSema', 6: 'SignalSema', 7: 'iSignalSema', 8: 'WaitSema',
                    9: 'PollSema', 11: 'ReferSemaStatus'},
        'thevent': {4: 'CreateEventFlag', 5: 'DeleteEventFlag', 6: 'SetEventFlag', 7: 'iSetEventFlag',
                    8: 'ClearEventFlag', 9: 'iClearEventFlag', 10: 'WaitEventFlag', 11: 'PollEventFlag'},
        'sysmem': {4: 'AllocSysMemory', 5: 'FreeSysMemory', 6: 'QueryMemSize', 7: 'QueryMaxFreeMemSize',
                   8: 'QueryTotalFreeMemSize', 14: 'Kprintf'},
        'intrman': {4: 'RegisterIntrHandler', 5: 'ReleaseIntrHandler', 6: 'EnableIntr', 7: 'DisableIntr',
                    17: 'CpuSuspendIntr', 18: 'CpuResumeIntr', 23: 'QueryIntrContext'},
        'sysclib': {4: 'setjmp', 5: 'longjmp', 6: 'toupper', 7: 'tolower', 8: 'look_ctype_table', 11: 'bcmp',
                    12: 'bcopy', 13: 'bzero', 14: 'memchr', 15: 'memcmp', 16: 'memcpy', 17: 'memmove', 18: 'memset',
                    19: 'sprintf', 20: 'strcat', 21: 'strchr', 22: 'strcmp', 23: 'strcpy', 24: 'strcspn',
                    25: 'index', 26: 'rindex', 27: 'strlen', 28: 'strncat', 29: 'strncmp', 30: 'strncpy',
                    31: 'strpbrk', 32: 'strrchr', 33: 'strspn', 34: 'strstr', 35: 'strtok', 36: 'strtol',
                    37: 'atob', 38: 'strtoul', 40: 'wmemcopy', 41: 'wmemset', 42: 'vsprintf'},
        'stdio': {4: 'printf'},
        'ioman': {4: 'open', 5: 'close', 6: 'read', 7: 'write', 8: 'lseek', 9: 'ioctl', 10: 'remove',
                  11: 'mkdir', 12: 'rmdir', 13: 'dopen', 14: 'dclose', 15: 'dread', 16: 'getstat', 17: 'chstat'},
        'cdvdman': {4: 'sceCdInit', 5: 'sceCdStandby', 6: 'sceCdRead', 7: 'sceCdSeek', 8: 'sceCdGetError',
                    9: 'sceCdGetToc', 10: 'sceCdSearchFile', 11: 'sceCdSync', 12: 'sceCdGetDiskType',
                    13: 'sceCdDiskReady', 14: 'sceCdTrayReq', 15: 'sceCdStop', 16: 'sceCdPosToInt',
                    17: 'sceCdIntToPos', 21: 'sceCdCheckCmd', 22: 'sceCdReadILinkId', 24: 'sceCdReadClock',
                    28: 'sceCdStatus', 37: 'sceCdCallback', 38: 'sceCdPause', 39: 'sceCdBreak',
                    44: 'sceCdGetReadPos', 49: 'sceCdStInit', 50: 'sceCdStRead', 51: 'sceCdStSeek',
                    52: 'sceCdStStart', 53: 'sceCdStStat', 54: 'sceCdStStop', 56: 'sceCdReadChain',
                    57: 'sceCdStPause', 58: 'sceCdStResume', 74: 'sceCdMmode', 79: 'sceCdChangeThreadPriority'},
        'vblank': {4: 'WaitVblankStart', 5: 'WaitVblankEnd', 6: 'WaitVblank', 7: 'WaitNonVblank',
                   8: 'RegisterVblankHandler', 9: 'ReleaseVblankHandler'},
    }

    def parse_imports(self):
        """IRX import tables: magic 0x41e00000, 0, version, char name[8], then stubs 'jr ra; addiu zero,zero,N'."""
        self.imports = {}
        base, off, size = self.text()
        d = self.d
        a = 0
        while a < size:
            w = struct.unpack_from('<I', d, off + a)[0]
            if w == 0x41e00000:
                lib = d[off + a + 12: off + a + 20].split(b'\0')[0].decode('latin1')
                s = a + 20
                while s + 8 <= size:
                    w1, w2 = struct.unpack_from('<II', d, off + s)
                    if w1 != 0x03e00008 or (w2 >> 16) != 0x2400:
                        break
                    n = w2 & 0xffff
                    self.imports[base + s] = f'{lib}:' + self.KNOWN.get(lib, {}).get(n, f'#{n}')
                    s += 8
                a = s
                continue
            a += 4
        self.symat.update(self.imports)

    def text(self):
        s = self.secs[self.byname['.text']]
        return s[3], s[4], s[5]

    def word_at(self, addr):
        for s, n in zip(self.secs, self.sname):
            if n in ('.text', '.data') and s[3] <= addr < s[3] + s[5]:
                return struct.unpack_from('<I', self.d, s[4] + addr - s[3])[0]
        return None

    def cstr(self, addr):
        for s, n in zip(self.secs, self.sname):
            if n in ('.text', '.data', '.rodata') and s[3] <= addr < s[3] + s[5]:
                o = s[4] + addr - s[3]
                return self.d[o:self.d.index(b'\0', o)].decode('latin1')
        return None

    def func(self, name_or_addr):
        if isinstance(name_or_addr, str) and not name_or_addr.startswith('0x'):
            cand = [x for x in self.syms if x[2] == name_or_addr]
            if not cand: raise SystemExit(f'no symbol {name_or_addr}')
            a, size = cand[0][0], cand[0][1]
        else:
            a = int(name_or_addr, 16)
            size = next((x[1] for x in self.syms if x[0] == a and x[1]), 0x200)
        base, off, _ = self.text()
        out = []
        hi = {}
        for pc in range(a, a + size, 4):
            w = struct.unpack_from('<I', self.d, off + pc - base)[0]
            i = decode(w)
            note = ''
            if pc in self.symat and pc != a: note += f'   <{self.symat[pc]}>'
            if i.mnem in ('jal', 'j'):
                t = i.ops[0]
                note += f'   -> {self.symat.get(t, hex(t))}'
            if i.mnem == 'lui':
                hi[i.ops[0]] = i.ops[1] << 16
            elif i.mnem in ('addiu', 'ori') and i.ops[1] in hi:
                v = (hi[i.ops[1]] + i.ops[2]) & 0xffffffff if i.mnem == 'addiu' else hi[i.ops[1]] | i.ops[2]
                s = self.cstr(v)
                note += f'   ={hex(v)}' + (f' "{s[:50]}"' if s and len(s) > 2 else '') + (f' <{self.symat[v]}>' if v in self.symat else '')
            ops = ', '.join((REG[x] if k < 3 and i.fmt in ('r3', 'i', 'mem', 'sh', 'shv', 'br2', 'br1', 'jr', 'jalr', 'lui', 'st', 'rd', 'rs') and isinstance(x, int) and 0 <= x < 32 and not (i.fmt in ('i', 'mem', 'br1', 'br2', 'lui') and k == len(i.ops) - 1) and not (i.fmt == 'sh' and k == 2) else str(x))
                            for k, x in enumerate(i.ops))
            out.append(f'{pc:06x}: {i.mnem:8} {ops}{note}')
        return '\n'.join(out)


if __name__ == '__main__':
    irx = Irx(sys.argv[1])
    cmd = sys.argv[2]
    if cmd == 'syms':
        for a, s, n, t, sh in sorted(irx.syms):
            if n and t in (1, 2):
                print(f'{a:06x} {s:6} {"F" if t == 2 else "O"} {n}')
    elif cmd == 'fn':
        print(irx.func(sys.argv[3]))
    elif cmd == 'str':
        print(irx.cstr(int(sys.argv[3], 16)))

