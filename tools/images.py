"""Build canonical RGB images for every sprite object of the game.

Each image: dict(name, w, h, rows) with rows = list of lists of RGB-or-None,
positioned in a canonical box whose origin is the object's "visual" X/Y:
  * Jetman: box x in [-8, 24) (32 wide), 24 tall. Visual origin Xe = X & ~1.
  * aliens / items / rockets: 16 wide, 16 tall, origin top-left.
  * explosions: 24 wide, 16 tall.  flames: 16 x 16.
"""
from jetlib import *

def blank(w, h):
    return [[None]*w for _ in range(h)]

def paste(dst, src_rows, dx, dy):
    for y, row in enumerate(src_rows):
        for x, px in enumerate(row):
            if px is not None:
                X, Y = x+dx, y+dy
                if 0 <= X < len(dst[0]) and 0 <= Y < len(dst):
                    dst[Y][X] = px
                else:
                    raise ValueError('pixel outside box at %d,%d' % (X, Y))

def mirror(rows):
    return [list(reversed(r)) for r in rows]

def tint(rows, ink_rgb):
    out = []
    for r in rows:
        out.append([None if p is None else ((p[0]+ink_rgb[0])//2, (p[1]+ink_rgb[1])//2, (p[2]+ink_rgb[2])//2) for p in r])
    return out

# ---------------------------------------------------------------- Jetman
# frame index = ((X&6)>>1) | (left<<2) | (walk<<3), as in the original lookup
JET_NAMES = ['gfx_jetman_fly_right1', 'gfx_jetman_fly_right2', 'gfx_jetman_fly_right3', 'gfx_jetman_fly_right4',
             'gfx_jetman_fly_left4', 'gfx_jetman_fly_left3', 'gfx_jetman_fly_left2', 'gfx_jetman_fly_left1',
             'gfx_jetman_walk_right1', 'gfx_jetman_walk_right2', 'gfx_jetman_walk_right3', 'gfx_jetman_walk_right4',
             'gfx_jetman_walk_left4', 'gfx_jetman_walk_left3', 'gfx_jetman_walk_left2', 'gfx_jetman_walk_left1']

def jetman_images():
    imgs = []
    for i, n in enumerate(JET_NAMES):
        s = (i & 3) * 2                      # X & 6 for which this frame is used
        xoff, w, h, rows = sprite_block(n)
        rgb = to_rgb_rows(rows, ink=7)       # Jetman attribute: white ink
        # screen pixel of box column bx = box_origin + bx; box_origin = ((X+xoff)&~7)
        # canonical x = screen - Xe, with Xe = X&~1.  Take X = 8k + s (k=0):
        X = s
        box_origin = (X + xoff) & ~7
        Xe = X & ~1
        dx = box_origin - Xe + 8             # +8 because canonical box starts at -8
        img = blank(32, 24)
        paste(img, rgb, dx, 0)
        imgs.append(dict(name=n, w=32, h=24, rows=img))
    return imgs

# ---------------------------------------------------------------- aliens
ALIEN_NAMES = [('gfx_meteor1', 'gfx_meteor2'), ('gfx_squidgy_alien1', 'gfx_squidgy_alien2'),
               ('gfx_sphere_alien1', 'gfx_sphere_alien2'), ('gfx_jet_fighter', 'gfx_jet_fighter'),
               ('gfx_ufo', 'gfx_ufo'), ('gfx_cross_ship', 'gfx_cross_ship'),
               ('gfx_space_craft', 'gfx_space_craft'), ('gfx_frog_alien', 'gfx_frog_alien')]

def alien_images():
    """returns list per type of 4 images: right f1, right f2, left f1, left f2 (16x16)."""
    out = []
    for f1, f2 in ALIEN_NAMES:
        imgs = []
        for n in (f1, f2):
            xoff, w, h, rows = sprite_block(n)
            rgb = to_rgb_rows(rows, ink=7)
            img = blank(16, 16); paste(img, rgb, 0, 0)
            imgs.append(dict(name=n, w=16, h=16, rows=img, hpx=h))
        for n in (f1, f2):
            xoff, w, h, rows = sprite_block(n)
            rgb = mirror(to_rgb_rows(rows, ink=7))
            img = blank(16, 16); paste(img, rgb, 0, 0)
            imgs.append(dict(name=n+'_L', w=16, h=16, rows=img, hpx=h))
        out.append(imgs)
    return out

# ---------------------------------------------------------------- rockets
ROCKET_TYPES = ['u1', 'u5', 'u3', 'u4']   # index = (level>>2)&3, as the original table order
MAGENTA = (191, 0, 191)

def rocket_images():
    """per type: [bottom, middle, top] normal + [bottom, middle, top] magenta-tinted."""
    out = []
    for t in ROCKET_TYPES:
        imgs = []
        for part in ('bottom', 'middle', 'top'):
            xoff, w, h, rows = sprite_block('gfx_rocket_%s_%s' % (t, part))
            rgb = to_rgb_rows(rows, ink=7)
            img = blank(16, 16); paste(img, rgb, 0, 0)
            imgs.append(dict(name='rocket_%s_%s' % (t, part), w=16, h=16, rows=img))
        for part, im in zip(('bottom', 'middle', 'top'), list(imgs)):
            imgs.append(dict(name='rocket_%s_%s_fuel' % (t, part), w=16, h=16, rows=tint(im['rows'], MAGENTA)))
        out.append(imgs)
    return out

# ---------------------------------------------------------------- items / fx
ITEM_NAMES = ['gfx_fuel_pod', 'gfx_gold_bar', 'gfx_radiation', 'gfx_chemical_weapon', 'gfx_plutonium', 'gfx_diamond']

def item_images():
    out = []
    for n in ITEM_NAMES:
        xoff, w, h, rows = sprite_block(n)
        rgb = to_rgb_rows(rows, ink=7)
        img = blank(16, 16); paste(img, rgb, 0, 0)
        out.append(dict(name=n, w=16, h=16, rows=img, hpx=h))
    return out

def explosion_images():
    out = []
    for n in ('gfx_explosion_small', 'gfx_explosion_medium', 'gfx_explosion_big'):
        xoff, w, h, rows = sprite_block(n)
        rgb = to_rgb_rows(rows, ink=7)
        # the few index-255 pixels flash random inks in the original; render them pink-white
        img = blank(24, 16); paste(img, rgb, 0, 0)
        out.append(dict(name=n, w=24, h=16, rows=img))
    return out

def flame_images():
    out = []
    for n in ('gfx_rocket_flames1', 'gfx_rocket_flames2'):
        xoff, w, h, rows = sprite_block(n)
        rgb = to_rgb_rows(rows, ink=7)
        img = blank(16, 16); paste(img, rgb, 0, 0)
        out.append(dict(name=n, w=16, h=16, rows=img, hpx=h))
    return out

def platform_tiles():
    """left, middle, right platform tiles for ink 4 (green) and ink 6 (yellow)."""
    out = {}
    for ink in (4, 6):
        for n in ('tile_platform_left', 'tile_platfor_mmiddle', 'tile_platform_right'):
            xoff, w, h, rows = sprite_block(n)
            out[(n, ink)] = to_rgb_rows(rows, ink=ink)
    return out

def life_icon():
    xoff, w, h, rows = sprite_block('tile_life_icon')
    return to_rgb_rows(rows, ink=7)
