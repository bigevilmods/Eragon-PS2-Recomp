"""Dump PS2 EE ELF headers, sections, segments and symbol stats."""
import struct, sys

def main(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'\x7fELF', 'not ELF'
    (e_type, e_machine, e_version, e_entry, e_phoff, e_shoff, e_flags, e_ehsize,
     e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx) = struct.unpack_from('<HHIIIIIHHHHHH', d, 16)
    print(f'type={e_type} machine={e_machine} entry=0x{e_entry:08x} flags=0x{e_flags:08x}')
    print('-- program headers')
    for i in range(e_phnum):
        p_type, p_off, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = struct.unpack_from('<8I', d, e_phoff + i * e_phentsize)
        print(f'  type={p_type:#x} off={p_off:#08x} vaddr={p_vaddr:#010x} filesz={p_filesz:#x} memsz={p_memsz:#x} flags={p_flags:#x}')
    secs = []
    for i in range(e_shnum):
        secs.append(struct.unpack_from('<10I', d, e_shoff + i * e_shentsize))
    if not secs:
        print('no section headers'); return
    shstr = secs[e_shstrndx]
    name = lambda off, tab: d[tab[4] + off: d.index(b'\0', tab[4] + off)].decode('latin1')
    print('-- sections')
    for s in secs:
        print(f'  {name(s[0], shstr):<20} type={s[1]:<3} addr={s[3]:#010x} off={s[4]:#08x} size={s[5]:#x}')
    for s in secs:
        if s[1] == 2:  # SYMTAB
            strtab = secs[s[6]]
            n = s[5] // 16
            funcs = []
            for j in range(n):
                st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from('<IIIBBH', d, s[4] + j * 16)
                if st_info & 0xf == 2:
                    funcs.append((st_value, st_size, name(st_name, strtab)))
            print(f'-- symtab: {n} symbols, {len(funcs)} functions')
            for f in sorted(funcs)[:40]:
                print(f'  {f[0]:#010x} {f[1]:6} {f[2]}')

if __name__ == '__main__':
    main(sys.argv[1])
