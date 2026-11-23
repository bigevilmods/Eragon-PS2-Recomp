"""Embed a text file as a C string: python embed_text.py input output.h variable_name"""
import sys

src, dst, name = sys.argv[1], sys.argv[2], sys.argv[3]
text = open(src, encoding='utf-8-sig').read()   # tolerate a BOM
out = ['/* generated from %s by tools/embed_text.py */' % src.replace('\\', '/').split('/')[-1],
       'static const char %s[] =' % name]
for line in text.splitlines():
    out.append('    "%s\\n"' % line.replace('\\', '\\\\').replace('"', '\\"'))
out.append(';')
open(dst, 'w', encoding='utf-8').write('\n'.join(out) + '\n')
