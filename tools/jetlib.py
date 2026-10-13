"""Shared decoding helpers: Spec256 Jetpac graphics -> coloured pixel arrays.

Pixel arrays are lists of rows; each row is a list of palette indices (Spec256
0..255, 0 = transparent / unset).  Attribute "mixing" (Spec256 rule: indices
>=192 are averaged with the Spectrum ink colour) is applied here where the game
uses a fixed ink for an object.
"""
import json, os
HERE = os.path.dirname(os.path.abspath(__file__))
from paths import SRC
blocks = json.load(open(os.path.join(HERE, 'skool_blocks.json')))
sna = open(os.path.join(SRC, 'JETPAC.SNA'), 'rb').read(); ram = sna[27:]
gfx = open(os.path.join(SRC, 'JETPAC.GFX'), 'rb').read()
romgfx = open(os.path.join(SRC, 'ROM0.GFX'), 'rb').read()
rom = open(os.path.join(SRC, '48.rom'), 'rb').read()
_p = open(os.path.join(SRC, 'spec256.raw.pal'), 'rb').read()
PAL = [(_p[i*3], _p[i*3+1], _p[i*3+2]) for i in range(256)]
DELTA = 53   # SNA address = skool (cartridge) address + 53 for gfx blocks

# Spectrum ink colours as rendered by the Spec256 mixer (non-bright levels)
INK = {0: (0, 0, 0), 1: (0, 0, 191), 2: (191, 0, 0), 3: (191, 0, 191),
       4: (0, 191, 0), 5: (0, 191, 191), 6: (191, 191, 0), 7: (191, 191, 191)}


def pix_colors(addr):
    """8 (bit, colour) pairs for the byte at a Spectrum address (RAM or ROM)."""
    if addr >= 0x4000:
        b = ram[addr-0x4000]; base = (addr-0x4000)*8; src = gfx
    else:
        b = rom[addr]; base = addr*8; src = romgfx
    return [((b >> (7-x)) & 1, src[base+(7-x)]) for x in range(8)]


def sprite_block(name):
    """Decode a gfx_/tile_ block -> (xoff, w_tiles, h, rows) with rows of (bit,col)."""
    v = blocks[name]; a = v['addr']+DELTA; data = v['bytes']
    if name == 'tile_life_icon':
        a += 2          # this block sits two bytes further in the tape snapshot
    if name.startswith('gfx_'):
        if data[0] in (0, 8) and len(data) > 3 and data[1] in (2, 3):
            xoff, w, h = data[0], data[1], data[2]; off = 3
        else:
            h = data[0]; w = 2; xoff = 0; off = 1
        # actor sprites are stored bottom-up: the first data row is the
        # lowest screen row (MaskSprite walks ScreenPosOnePixelAbove)
        rows = []
        for y in range(h):
            row = []
            for c in range(w):
                row += pix_colors(a+off+y*w+c)
            rows.append(row)
        rows.reverse()
        return xoff, w, h, rows
    rows = [pix_colors(a+y) for y in range(8)]
    return 0, 1, 8, rows


def rgb_of(idx, ink=None):
    """Spec256 colour index -> RGB, applying attribute mixing for idx>=192."""
    r, g, b = PAL[idx]
    if ink is not None and idx >= 192:
        ir, ig, ib = INK[ink]
        return ((r+ir)//2, (g+ig)//2, (b+ib)//2)
    return (r, g, b)


def to_rgb_rows(rows, ink=None):
    """(bit,col) rows -> rows of RGB-or-None."""
    out = []
    for row in rows:
        out.append([rgb_of(col, ink) if col != 0 else None for bit, col in row])
    return out


def font_glyph(ch):
    """ROM font glyph -> 8 rows of RGB-or-None (set pixels with colour 0 are transparent)."""
    base = 0x3D00 + (ch-32)*8
    rows = []
    for r in range(8):
        rows.append([rgb_of(col) if col != 0 else None for bit, col in pix_colors(base+r)])
    return rows


def q6(rgb):
    """RGB -> nearest SMS 2-bit-per-channel tuple."""
    return tuple(min(3, max(0, int(round(v/85.0)))) for v in rgb)


def sms_byte(c):
    return c[0] | (c[1] << 2) | (c[2] << 4)


def tile_planar(pix):
    """8x8 list-of-rows of palette indices -> 32 bytes SMS planar."""
    out = bytearray()
    for y in range(8):
        for plane in range(4):
            b = 0
            for x in range(8):
                if (pix[y][x] >> plane) & 1:
                    b |= 0x80 >> x
            out.append(b)
    return bytes(out)
