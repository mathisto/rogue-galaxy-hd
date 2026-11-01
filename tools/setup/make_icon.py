"""Draws the setup's icon, rgsetup/data/setup.ico: the launcher's ringed planet with a download arrow in a badge.

Drawn from nothing (no game art), on top of src/launcher/make_icon.py's planet. Needs Pillow:

    python tools/setup/make_icon.py
"""
import importlib.util
import sys
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[2]
OUT = Path(__file__).with_name('rgsetup') / 'data' / 'setup.ico'


def planet_module():
    spec = importlib.util.spec_from_file_location('rg_make_icon', ROOT / 'src' / 'launcher' / 'make_icon.py')
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main():
    m = planet_module()
    size = m.SIZE
    picture = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    picture.alpha_composite(m.ring(front=False))
    picture.alpha_composite(m.planet())
    picture.alpha_composite(m.ring(front=True))
    badge = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(badge)
    cx, cy, r = 760, 760, 250
    d.ellipse((cx - r - 22, cy - r - 22, cx + r + 22, cy + r + 22), fill=(18, 24, 48, 255))
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(38, 160, 98, 255))
    d.rectangle((cx - 42, cy - 150, cx + 42, cy + 20), fill=(255, 255, 255, 255))
    d.polygon([(cx - 130, cy), (cx + 130, cy), (cx, cy + 130)], fill=(255, 255, 255, 255))
    d.rectangle((cx - 140, cy + 160, cx + 140, cy + 196), fill=(255, 255, 255, 255))
    picture.alpha_composite(badge)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    picture.save(OUT, sizes=[(256, 256), (128, 128), (64, 64), (48, 48), (32, 32), (24, 24), (16, 16)])
    print('wrote', OUT)


if __name__ == '__main__':
    sys.exit(main())
