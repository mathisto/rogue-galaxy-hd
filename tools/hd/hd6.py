"""Rogue Galaxy (PS2, USA) HD6 index parser. Usage: python -I hd6.py <X.HD6> [N] [--legacy]  (see research/02-archive-format.md)"""
import struct, sys

def varint(b, p):
    v = 0; s = 0
    while True:
        c = b[p]; p += 1
        v |= (c & 0x7f) << s; s += 7
        if not c & 0x80: return v, p

def parse(path, legacy=False):
    """Return (strings, [(name, off, size16)]). off is the TRUE data start: the u24 field * 512 rounded down to a 2048
    boundary (7,438 DATA0_0 entries store +512; see research/13-extraction-offset-fix.md). legacy=True returns the raw
    field*512 (old, wrong for those entries)."""
    h = open(path, 'rb').read()
    assert h[:4] == b'HD6\0'
    (_m, stroff, strsize, nstr, _z, recoff, recsize, _z2, _k, nent, entoff, _z3, total) = struct.unpack('<13I', h[:52])
    strs = h[stroff:stroff + strsize].split(b'\0')[:nstr]
    strs = [s.decode('latin1') for s in strs]
    recs = h[recoff:recoff + recsize]
    ents = []; base = 0; prev = 0
    for i in range(nent):
        e = h[entoff + 8 * i: entoff + 8 * i + 8]
        n16 = e[0] | e[1] << 8
        if n16 < prev: base += 65536   # name offset is stored as u16; table is sorted so unwrap
        prev = n16; noff = base + n16
        off = (e[2] | e[3] << 8 | e[4] << 16) * 512
        if not legacy: off &= ~2047
        size16 = (e[5] | e[6] << 8 | e[7] << 16) * 16
        ents.append((noff, off, size16))
    out = []
    for noff, off, size in ents:
        if noff >= recsize and size == 0: continue
        p = noff; parts = []
        while p < len(recs):
            c = recs[p]
            if c == 0: break
            v, p = varint(recs, p); parts.append(strs[v])
        name = ''.join(parts)
        out.append((name, off, size))
    return strs, out

if __name__ == '__main__':
    strs, out = parse(sys.argv[1], '--legacy' in sys.argv)
    print(len(strs), len(out))
    for r in out[:int(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[2].isdigit() else 20]: print(r)
