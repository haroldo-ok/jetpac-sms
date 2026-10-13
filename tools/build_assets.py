"""Build all SMS assets for the Jetpac port.

Outputs:
  data/*.bin           binary blobs (folder2c turns them into C arrays)
  src/gen_assets.h     tile indices / sizes
  src/gen_tables.c     small lookup tables (sprite parts, palettes, glyph map, platform cells)
"""
import os, sys, json
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from jetlib import *
from images import *
from palette import *
from nebula import BG, nebula_indexed, reduce_tiles, render as neb_render

from paths import ROOT
DATA = os.path.join(ROOT, 'data'); os.makedirs(DATA, exist_ok=True)
SRCD = os.path.join(ROOT, 'src'); os.makedirs(SRCD, exist_ok=True)

K_NEBULA = int(os.environ.get('K_NEBULA', '128'))   # 128 nebula tiles leave 95 composite slots for the HUD and laser beams
HUD_SLOTS = 27
POOL_SLOTS = 20

# ------------------------------------------------------------------ palettes
CORE = [(3,1,0),(3,2,1),(2,1,1),(1,1,1),(2,2,2),(3,3,3),(1,1,2),(2,0,0),(2,1,2),(2,2,0),(3,3,0),(1,2,2)]
assert len(CORE) == 12
SPR_PAL_CORE = [(0,0,0)] + CORE                     # index 0 transparent; 1..12 core; 13,14 alien; 15 rocket

def hist_img(imgs):
    from collections import Counter
    c = Counter()
    for im in imgs:
        for row in im['rows']:
            for p in row:
                if p is not None:
                    c[q6(p)] += 1
    return c

J = jetman_images(); A = alien_images(); R = rocket_images(); I = item_images(); E = explosion_images(); F = flame_images()

alien_dyn = []
for t in range(8):
    pal = cluster(hist_img(A[t]), 14, fixed=CORE)
    dyn = [c for c in pal if c not in CORE]
    while len(dyn) < 2: dyn.append((0,0,0))
    alien_dyn.append(dyn[:2])
rocket_dyn = []
for r in range(4):
    h = hist_img(R[r][:3])
    for k, v in hist_img(R[r][3:]).items(): h[k] += v*0.3
    pal = cluster(h, 13, fixed=CORE)
    dyn = [c for c in pal if c not in CORE]
    while len(dyn) < 1: dyn.append((0,0,0))
    rocket_dyn.append(dyn[0])
print('alien dyn', alien_dyn); print('rocket dyn', rocket_dyn)

def encode_image(img, pal):
    """RGB rows -> rows of palette indices (0 transparent)."""
    cache = {}
    out = []
    for row in img['rows']:
        r = []
        for p in row:
            if p is None:
                r.append(0)
            else:
                c = q6(p)
                if c not in cache:
                    cache[c] = nearest(pal[1:], c) + 1
                r.append(cache[c])
        out.append(r)
    return out

def pair_bytes(idx_rows, x0, y0):
    """two stacked 8x8 tiles (8x16) from an index image at (x0,y0); rows beyond the image are 0."""
    out = bytearray()
    for ty in range(2):
        pix = []
        for y in range(8):
            yy = y0+ty*8+y
            row = idx_rows[yy] if yy < len(idx_rows) else [0]*len(idx_rows[0])
            pix.append([row[x0+x] if 0 <= x0+x < len(row) else 0 for x in range(8)])
        out += tile_planar(pix)
    return bytes(out)

# ------------------------------------------------------------------ core sprite tiles (deduped pairs)
pairs = []          # list of 64-byte blobs
pair_index = {}
def add_pair(b):
    if b not in pair_index:
        pair_index[b] = len(pairs); pairs.append(b)
    return pair_index[b]
BLANK64 = bytes(64)

jet_parts = []      # per frame: list of (dx, dy, tile)
for f, im in enumerate(J):
    idx = encode_image(im, SPR_PAL_CORE)
    parts = []
    for c in range(4):
        for h in range(2):
            b = pair_bytes(idx, c*8, h*16)
            if b != BLANK64:
                parts.append(((c-1)*8, h*16, add_pair(b)*2))
    jet_parts.append(parts)
print('jetman parts per frame', [len(p) for p in jet_parts], 'pairs so far', len(pairs))

item_tiles = []
for im in I:
    idx = encode_image(im, SPR_PAL_CORE)
    item_tiles.append([add_pair(pair_bytes(idx, 0, 0))*2, add_pair(pair_bytes(idx, 8, 0))*2])
expl_tiles = []
for im in E:
    idx = encode_image(im, SPR_PAL_CORE)
    expl_tiles.append([add_pair(pair_bytes(idx, c*8, 0))*2 for c in range(3)])
flame_tiles = []
for im in F:
    idx = encode_image(im, SPR_PAL_CORE)
    flame_tiles.append([add_pair(pair_bytes(idx, 0, 0))*2, add_pair(pair_bytes(idx, 8, 0))*2])
N_CORE_PAIRS = len(pairs)
N_CORE_TILES = N_CORE_PAIRS*2
print('core pairs', N_CORE_PAIRS, 'tiles', N_CORE_TILES)
open(os.path.join(DATA, 'spr_core.bin'), 'wb').write(b''.join(pairs))

ALIEN_TILE = N_CORE_TILES          # 16 tiles
ROCKET_TILE = ALIEN_TILE + 16      # 36 tiles
N_SPR_TILES = ROCKET_TILE + 36

# ------------------------------------------------------------------ aliens (per type, 4 images x 2 cols)
alien_blob = bytearray()
alien_h = []
for t in range(8):
    pal = SPR_PAL_CORE + alien_dyn[t] + [(0,0,0)]
    for im in A[t]:
        idx = encode_image(im, pal)
        alien_blob += pair_bytes(idx, 0, 0) + pair_bytes(idx, 8, 0)
    alien_h.append([A[t][0]['hpx'], A[t][1]['hpx']])
open(os.path.join(DATA, 'spr_alien.bin'), 'wb').write(alien_blob)
assert len(alien_blob) == 8*16*32

# ------------------------------------------------------------------ rockets (per type: module x variant x col)
rocket_blob = bytearray()
for r in range(4):
    pal = SPR_PAL_CORE + [(0,0,0), (0,0,0)] + [rocket_dyn[r]]
    normal = [encode_image(R[r][m], pal) for m in range(3)]
    tinted = [encode_image(R[r][3+m], pal) for m in range(3)]
    for m in range(3):
        for v in range(3):           # 0 = NN, 1 = N top / T bottom, 2 = TT
            for c in range(2):
                top = pair_bytes(normal[m] if v < 2 else tinted[m], c*8, 0)[:32]
                bot = pair_bytes(normal[m] if v == 0 else tinted[m], c*8, 0)[32:]
                rocket_blob += top + bot
open(os.path.join(DATA, 'spr_rocket.bin'), 'wb').write(rocket_blob)
assert len(rocket_blob) == 4*36*32

# ------------------------------------------------------------------ nebula
neb_img = nebula_indexed()
neb_tiles, neb_map = reduce_tiles(neb_img, K_NEBULA)
K = len(neb_tiles)
NEB_TILE = N_SPR_TILES
neb_blob = b''.join(tile_planar([list(t[y*8:y*8+8]) for y in range(8)]) for t in neb_tiles)
open(os.path.join(DATA, 'neb_tiles.bin'), 'wb').write(neb_blob)
open(os.path.join(DATA, 'neb_map.bin'), 'wb').write(bytes(neb_map))
neb_render(neb_tiles, neb_map, os.path.join(ROOT, 'tools', 'nebula_preview.png'))

# ------------------------------------------------------------------ platform composites
PLATFORMS = [(4, 0x80, 0x60, 0x1B), (6, 0x78, 0xB8, 0x88), (4, 0x30, 0x48, 0x23), (4, 0xD0, 0x30, 0x23)]
ptiles = platform_tiles()
def bg_index(rgb):
    return nearest(BG, q6(rgb))
plat_tiles = []; plat_tile_index = {}; plat_cells = []
for ink, X, Y, W in PLATFORMS:
    x0 = ((X - (W & ~3) + 16) & 0xFF)
    n_mid = (W >> 2) - 4
    col0 = x0 >> 3; row = Y >> 3
    seq = [('tile_platform_left', ink)] + [('tile_platfor_mmiddle', ink)]*n_mid + [('tile_platform_right', ink)]
    for i, key in enumerate(seq):
        col = col0 + i
        cell = row*32 + col
        base = list(neb_tiles[neb_map[cell]])
        rows = ptiles[key]
        pix = [[base[y*8+x] for x in range(8)] for y in range(8)]
        for y in range(8):
            for x in range(8):
                if rows[y][x] is not None:
                    pix[y][x] = bg_index(rows[y][x])
        b = tile_planar(pix)
        if b not in plat_tile_index:
            plat_tile_index[b] = len(plat_tiles); plat_tiles.append(b)
        plat_cells.append((col, row, plat_tile_index[b]))
P = len(plat_tiles)
PLAT_TILE = NEB_TILE + K
open(os.path.join(DATA, 'plat_tiles.bin'), 'wb').write(b''.join(plat_tiles))
HUD_TILE = PLAT_TILE + P
POOL_TILE = HUD_TILE + HUD_SLOTS
TOTAL = POOL_TILE + POOL_SLOTS
print('K nebula', K, 'P platform', P, 'tiles: spr', N_SPR_TILES, 'neb', NEB_TILE, 'plat', PLAT_TILE, 'hud', HUD_TILE, 'pool', POOL_TILE, 'total', TOTAL)
assert TOTAL <= 448, TOTAL

# ------------------------------------------------------------------ font
GLYPHS = [ord(c) for c in '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ.>:;<=?@'] + [0x7F]   # :;<=?@ = lives 10..16 as the original prints lives+'0'
font_blob = bytearray()
glyph_map = {}
for gi, ch in enumerate(GLYPHS):
    rows = font_glyph(ch)
    pix = [[0]*8 for _ in range(8)]; mask = bytearray(8)
    for y in range(8):
        for x in range(8):
            if rows[y][x] is not None:
                pix[y][x] = bg_index(rows[y][x]); mask[y] |= 0x80 >> x
    font_blob += tile_planar(pix) + bytes(mask)
    glyph_map[ch] = gi
# life icon as glyph too
rows = life_icon(); pix = [[0]*8 for _ in range(8)]; mask = bytearray(8)
for y in range(8):
    for x in range(8):
        if rows[y][x] is not None:
            pix[y][x] = bg_index(rows[y][x]); mask[y] |= 0x80 >> x
LIFE_GLYPH = len(GLYPHS)
font_blob += tile_planar(pix) + bytes(mask)
open(os.path.join(DATA, 'font.bin'), 'wb').write(font_blob)

# ------------------------------------------------------------------ palettes
def palbytes(pal):
    return bytes(sms_byte(c) for c in pal)
open(os.path.join(DATA, 'pal_bg.bin'), 'wb').write(palbytes(BG))

# ------------------------------------------------------------------ generated C
h = []
h.append('/* generated by tools/build_assets.py - do not edit */')
h.append('#ifndef GEN_ASSETS_H\n#define GEN_ASSETS_H')
for name, val in [('N_CORE_TILES', N_CORE_TILES), ('ALIEN_TILE', ALIEN_TILE), ('ROCKET_TILE', ROCKET_TILE),
                  ('N_SPR_TILES', N_SPR_TILES), ('NEB_TILE', NEB_TILE), ('K_NEBULA', K), ('PLAT_TILE', PLAT_TILE),
                  ('N_PLAT_TILES', P), ('N_PLAT_CELLS', len(plat_cells)), ('HUD_TILE', HUD_TILE), ('HUD_SLOTS', HUD_SLOTS),
                  ('POOL_TILE', POOL_TILE), ('POOL_SLOTS', POOL_SLOTS), ('TOTAL_TILES', TOTAL),
                  ('N_GLYPHS', len(GLYPHS)+1), ('LIFE_GLYPH', LIFE_GLYPH), ('GLYPH_COPYRIGHT', glyph_map[0x7F]),
                  ('GLYPH_DOT', glyph_map[ord('.')]), ('GLYPH_CURSOR', glyph_map[ord('>')]),
                  ('BG_WHITE', BG.index((3,3,3))), ('BG_PINK', BG.index((3,1,3))), ('BG_CYAN', BG.index((2,3,3))),
                  ('SPR_PAL_ALIEN0', 13), ('SPR_PAL_ROCKET', 15)]:
    h.append('#define %s %d' % (name, val))
h.append('extern const unsigned char spr_pal_core[16];')
h.append('extern const unsigned char alien_dyn_pal[8][2];')
h.append('extern const unsigned char rocket_dyn_pal[4];')
h.append('extern const signed char jet_parts[16][19];')
h.append('extern const unsigned char item_tiles[6][2];')
h.append('extern const unsigned char expl_tiles[3][3];')
h.append('extern const unsigned char flame_tiles[2][2];')
h.append('extern const unsigned char alien_heights[8][2];')
h.append('extern const unsigned char plat_cells[%d][3];' % len(plat_cells))
h.append('extern const unsigned char glyph_of_ascii[96];')
h.append('#endif')
open(os.path.join(SRCD, 'gen_assets.h'), 'w').write('\n'.join(h)+'\n')

c = []
c.append('/* generated by tools/build_assets.py - do not edit */')
c.append('#include "gen_assets.h"')
c.append('const unsigned char spr_pal_core[16] = {%s};' % ','.join(str(b) for b in palbytes(SPR_PAL_CORE + [(0,0,0)]*3)))
c.append('const unsigned char alien_dyn_pal[8][2] = {%s};' % ','.join('{%d,%d}' % (sms_byte(d[0]), sms_byte(d[1])) for d in alien_dyn))
c.append('const unsigned char rocket_dyn_pal[4] = {%s};' % ','.join(str(sms_byte(d)) for d in rocket_dyn))
rows = []
for parts in jet_parts:
    flat = [len(parts)]
    for dx, dy, t in parts: flat += [dx, dy, t]
    while len(flat) < 19: flat.append(0)
    rows.append('{%s}' % ','.join(str(v) for v in flat))
c.append('const signed char jet_parts[16][19] = {%s};' % ','.join(rows))
c.append('const unsigned char item_tiles[6][2] = {%s};' % ','.join('{%d,%d}' % tuple(t) for t in item_tiles))
c.append('const unsigned char expl_tiles[3][3] = {%s};' % ','.join('{%d,%d,%d}' % tuple(t) for t in expl_tiles))
c.append('const unsigned char flame_tiles[2][2] = {%s};' % ','.join('{%d,%d}' % tuple(t) for t in flame_tiles))
c.append('const unsigned char alien_heights[8][2] = {%s};' % ','.join('{%d,%d}' % tuple(t) for t in alien_h))
c.append('const unsigned char plat_cells[%d][3] = {%s};' % (len(plat_cells), ','.join('{%d,%d,%d}' % t for t in plat_cells)))
gm = [glyph_map.get(ch, 255) for ch in range(32, 128)]
gm[0x7F-32] = glyph_map[0x7F]
c.append('const unsigned char glyph_of_ascii[96] = {%s};' % ','.join(str(v) for v in gm))
open(os.path.join(SRCD, 'gen_tables.c'), 'w').write('\n'.join(c)+'\n')

# ------------------------------------------------------------------ preview sheet of the encoded sprites
from PIL import Image
def decode_pair(b):
    px = [[0]*8 for _ in range(16)]
    for t in range(2):
        for y in range(8):
            for p in range(4):
                v = b[t*32+y*4+p]
                for x in range(8):
                    if v & (0x80 >> x): px[t*8+y][x] |= 1 << p
    return px
sheet = Image.new('RGB', (len(pairs)*9+4, 20), (30, 30, 50))
for i, b in enumerate(pairs):
    px = decode_pair(b)
    for y in range(16):
        for x in range(8):
            if px[y][x]:
                sheet.putpixel((i*9+x, y), rgb255(SPR_PAL_CORE[px[y][x]]))
sheet = sheet.resize((sheet.width*2, sheet.height*2), Image.NEAREST)
sheet.save(os.path.join(ROOT, 'tools', 'core_pairs_preview.png'))
json.dump({'jet_parts': jet_parts, 'item_tiles': item_tiles, 'expl_tiles': expl_tiles, 'flame_tiles': flame_tiles,
           'alien_dyn': alien_dyn, 'rocket_dyn': rocket_dyn, 'K': K, 'P': P, 'TOTAL': TOTAL},
          open(os.path.join(ROOT, 'tools', 'build_report.json'), 'w'), indent=1)
print('done')
