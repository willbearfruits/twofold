"""Volume (mm^3) and bounding box of an ASCII/binary STL; used by check.sh to grade intersections."""
import struct, sys
import numpy as np

def load(p):
    d = open(p, 'rb').read()
    if d[:5] == b'solid' and b'facet' in d[:300]:
        v = [list(map(float, l.split()[1:4])) for l in d.decode().splitlines() if l.strip().startswith('vertex')]
        return np.array(v).reshape(-1, 3, 3)
    n = struct.unpack('<I', d[80:84])[0]
    a = np.frombuffer(d[84:84 + n * 50], dtype=np.dtype([('n', '<3f4'), ('v', '<9f4'), ('a', '<u2')]))
    return a['v'].reshape(-1, 3, 3).astype(float)

t = load(sys.argv[1])
vol = abs(np.einsum('ij,ij->i', t[:, 0], np.cross(t[:, 1], t[:, 2])).sum()) / 6
lo, hi = t.reshape(-1, 3).min(0), t.reshape(-1, 3).max(0)
print(f"{vol:.3f} " + " ".join(f"{a:.1f}..{b:.1f}" for a, b in zip(lo, hi)))
