"""List primitives from an F9 capture draw log (capture/NN_draws.txt) that touch a screen rectangle.

  python -I drawsin.py NN_draws.txt x0 y0 x1 y1 [max]
Groups consecutive primitives with the same GS state and prints the state once per group.
"""
import re, sys


def main():
    fn = sys.argv[1]
    x0, y0, x1, y1 = map(float, sys.argv[2:6])
    mx = int(sys.argv[6]) if len(sys.argv) > 6 else 60
    last = None; shown = 0
    for ln, line in enumerate(open(fn, encoding='latin1')):
        head, *verts = line.rstrip('\n').split(' | ')
        pts = []
        for v in verts:
            m = re.match(r'([-\d.]+),([-\d.]+)', v)
            if m: pts.append((float(m.group(1)), float(m.group(2))))
        if not pts: continue
        bx0 = min(p[0] for p in pts); bx1 = max(p[0] for p in pts)
        by0 = min(p[1] for p in pts); by1 = max(p[1] for p in pts)
        if bx1 < x0 or bx0 > x1 or by1 < y0 or by0 > y1: continue
        if head != last:
            print(f'--- line {ln}: {head}')
            last = head
        print('   ', ' | '.join(verts))
        shown += 1
        if shown >= mx: break


if __name__ == '__main__':
    main()
