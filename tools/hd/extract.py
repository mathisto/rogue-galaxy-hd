"""Extract Rogue Galaxy DATA0_n.DAT using its HD6 index. Inputs are opened read-only.
usage: python -I extract.py <game_dir> <out_dir> [--list] [--legacy]
--legacy reproduces the pre-2026-10-28 behaviour (offsets 512 bytes late for 'off % 2048 == 512' entries; wrong).
Index sizes are padded to 16 bytes; RIFF files are trimmed to their true length.
Zero-size entries (empty files / directory markers) and the end sentinel are only
recorded in manifest.tsv, not written."""
import os, sys, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I drops the script dir from sys.path
import hd6

def main():
    game, out = sys.argv[1], sys.argv[2]
    listonly = '--list' in sys.argv
    legacy = '--legacy' in sys.argv
    os.makedirs(out, exist_ok=True)
    manifest = []; seen = set()
    for n in (0, 1):
        base = os.path.join(game, f'DATA0_{n}')
        strs, ents = hd6.parse(base + '.HD6', legacy)
        dat = open(base + '.DAT', 'rb'); dsize = os.path.getsize(base + '.DAT')
        for name, off, size in ents:
            rel = name.replace(chr(92), '/').lstrip('/')
            if not rel or size == 0:
                manifest.append((n, rel, off, 0, 'skipped-empty')); continue
            if '..' in rel.split('/') or ':' in rel: raise SystemExit('bad path ' + rel)
            k = 1; orig = rel
            while (n, rel) in seen:   # genuine duplicate names exist (2 in the game)
                root, ext = os.path.splitext(orig); rel = f'{root}__dup{k}{ext}'; k += 1
            seen.add((n, rel))
            if off + size > dsize: size = max(0, dsize - off)
            dat.seek(off); b = dat.read(size)
            if b[:4] == b'RIFF' and len(b) >= 8:
                b = b[:min(len(b), struct.unpack('<I', b[4:8])[0] + 8)]
            manifest.append((n, rel, off, len(b), 'ok'))  # off = true start offset in the DAT
            if listonly: continue
            p = os.path.join(out, f'DATA0_{n}', rel)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, 'wb') as f: f.write(b)
    with open(os.path.join(out, 'manifest.tsv'), 'w', encoding='utf-8') as f:
        for m in manifest: f.write('\t'.join(map(str, m)) + '\n')
    print(len(manifest), 'entries;', sum(m[3] for m in manifest), 'bytes written')
main()
