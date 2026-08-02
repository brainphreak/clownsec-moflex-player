#!/usr/bin/env python3
"""Rasterise the alphabetic scripts at 8x16 so subtitle lines are one height throughout.

Latin sat in an 8x8 cell while Hangul and kanji need 16x16, so a mixed line had two glyph
heights and accented letters (Turkish cedillas, Polish ogoneks) had no room for their marks.
Source: Noto Sans (OFL). FreeType mono mode, then a gap-preserving widen for the glyphs that
come out lighter than the rest -- a plain widen bridges 1px gaps and fills counters.
"""
from PIL import Image, ImageFont

W, H, PX = 8, 16, 14
FONT = 'NotoSans.ttf'

def cell(f, ch):
    m = f.getmask(ch, mode='1')
    if m.size[0] == 0 or m.size[1] == 0:
        return None
    im = Image.frombytes('L', m.size, bytes(m))
    bb = im.getbbox()
    if not bb:
        return [0] * H
    im = im.crop(bb)
    if im.width > W or im.height > H:
        im.thumbnail((W, H))
    out = Image.new('L', (W, H), 0)
    out.paste(im, ((W - im.width) // 2, (H - im.height) // 2))
    return [sum(1 << c for c in range(W) if out.getpixel((c, r))) for r in range(H)]

def widen(g):
    m = (1 << W) - 1
    return [(v | ((v << 1) & ~(v >> 1))) & m for v in g]

ink = lambda g: sum(bin(v).count('1') for v in g)

ranges = [(0x20, 0x7E), (0xA0, 0xFF), (0x100, 0x17F), (0x386, 0x3CE), (0x400, 0x45F),
          (0x590, 0x5FF)]
extra = [0x2010, 0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2026, 0x20AC, 0x266A, 0x266B]

f = ImageFont.truetype(FONT, PX)
out = []
for lo, hi in ranges:
    for cp in range(lo, hi + 1):
        g = cell(f, chr(cp))
        if g and any(g):
            out.append((cp, widen(g) if ink(g) < 30 else g))   # even out the weight
for cp in extra:
    g = cell(f, chr(cp))
    if g and any(g):
        out.append((cp, widen(g) if ink(g) < 30 else g))
out.sort()

with open('font8x16.h', 'w') as fh:
    fh.write('/* 8x16 alphabetic glyphs: Latin (+Extended-A), Greek, Cyrillic, Hebrew.\n'
             ' * Rasterised from Noto Sans (SIL Open Font License) by tools/gen_font8x16.py.\n'
             ' * Rows are 8 bits, LSB leftmost -- same order as font8x8_basic. Lines are one\n'
             ' * height throughout: these are 16 tall like the Hangul/kanji tables, so nothing\n'
             ' * has to be dropped to a baseline and accents finally have room. */\n'
             '#ifndef MOFLEX_FONT8X16_H\n#define MOFLEX_FONT8X16_H\n\n')
    fh.write('static const unsigned short font8x16_cp[] = {'
             + ','.join(f'0x{cp:04X}' for cp, _ in out) + '};\n')
    fh.write('#define FONT8X16_N ((int)(sizeof font8x16_cp / sizeof font8x16_cp[0]))\n')
    fh.write('static const unsigned char font8x16[][16] = {\n')
    for cp, g in out:
        fh.write('  {' + ','.join(f'0x{v:02X}' for v in g) + f'}},  /* U+{cp:04X} */\n')
    fh.write('};\n\n#endif\n')
print(f'{len(out)} glyphs, {len(out)*16/1024:.1f} KB')
