"""Nebula background: crop, map to the BG palette and reduce to K unique tiles."""
import numpy as np
from jetlib import *
from palette import *

BG = [(0,0,0),(1,0,0),(2,0,0),(1,1,1),(2,1,1),(2,2,2),(3,2,2),(3,3,3),
      (0,2,1),(1,2,1),(1,3,1),(0,2,2),(1,2,3),(2,3,3),(0,3,0),(3,1,3)]
BG255 = np.array([rgb255(c) for c in BG], dtype=np.float64)
WGT = np.array([0.3, 0.59, 0.11])

def nebula_indexed():
    d = np.frombuffer(open(SRC+'/JETPAC.B00', 'rb').read(), dtype=np.uint8).reshape(200, 320)
    crop = d[4:196, 32:288]
    lut = {}
    for idx in set(crop.flatten().tolist()):
        rgb = PAL[idx]
        lut[idx] = min(range(16), key=lambda i: dist2(rgb255(BG[i]), rgb))
    return np.vectorize(lut.get)(crop).astype(np.uint8)

def reduce_tiles(img, K):
    """img: 192x256 palette indices. Returns (tiles: list of 64-index tuples, cellmap: 768 ints)."""
    cells = []
    for ty in range(24):
        for tx in range(32):
            cells.append(tuple(img[ty*8:ty*8+8, tx*8:tx*8+8].flatten().tolist()))
    uniq = {}
    for c in cells:
        uniq.setdefault(c, []).append(c)
    reps = list(uniq.keys())
    members = [[i for i, c in enumerate(cells) if c == r] for r in reps]
    # cluster state
    weights = np.array([len(m) for m in members], dtype=np.float64)
    vec = np.array([BG255[list(r)].flatten() for r in reps])      # N x 192
    wv = np.tile(WGT, 64)
    hist = [np.zeros((64, 16)) for _ in reps]                     # per-pixel index histograms
    for i, r in enumerate(reps):
        for p, idx in enumerate(r):
            hist[i][p, idx] += weights[i]
    alive = list(range(len(reps)))

    def pair_cost(i, j):
        d = ((vec[i]-vec[j])**2 * wv).sum()
        return weights[i]*weights[j]/(weights[i]+weights[j]) * d

    # full cost matrix
    N = len(reps)
    cost = np.full((N, N), np.inf)
    for a in range(N):
        diff = (vec[a+1:] - vec[a])**2 @ wv
        w = weights[a]*weights[a+1:]/(weights[a]+weights[a+1:])
        cost[a, a+1:] = w*diff
    while len(alive) > K:
        a, b = np.unravel_index(np.argmin(cost), cost.shape)
        # merge b into a
        hist[a] = hist[a] + hist[b]
        members[a] += members[b]
        weights[a] += weights[b]
        newrep = tuple(int(np.argmax(hist[a][p])) for p in range(64))
        reps[a] = newrep
        vec[a] = BG255[list(newrep)].flatten()
        alive.remove(b)
        cost[b, :] = np.inf; cost[:, b] = np.inf
        for o in alive:
            if o == a: continue
            d = ((vec[o]-vec[a])**2 * wv).sum()
            c = weights[a]*weights[o]/(weights[a]+weights[o]) * d
            if o < a: cost[o, a] = c
            else: cost[a, o] = c
    tiles = []
    cellmap = [0]*768
    for n, i in enumerate(alive):
        tiles.append(reps[i])
        for m in members[i]:
            cellmap[m] = n
    # make tile 0 the all-black tile if present
    black = tuple([0]*64)
    if black in tiles:
        k = tiles.index(black)
        if k != 0:
            tiles[0], tiles[k] = tiles[k], tiles[0]
            cellmap = [0 if c == k else (k if c == 0 else c) for c in cellmap]
    return tiles, cellmap

def render(tiles, cellmap, path, scale=2):
    from PIL import Image
    im = Image.new('RGB', (256, 192))
    for cell, t in enumerate(cellmap):
        ty, tx = divmod(cell, 32)
        for p, idx in enumerate(tiles[t]):
            y, x = divmod(p, 8)
            im.putpixel((tx*8+x, ty*8+y), rgb255(BG[idx]))
    im.resize((256*scale, 192*scale), Image.NEAREST).save(path)
    return im

if __name__ == '__main__':
    import sys, time, os
    from paths import OUT
    K = int(sys.argv[1]) if len(sys.argv) > 1 else 160
    img = nebula_indexed()
    t = time.time()
    tiles, cellmap = reduce_tiles(img, K)
    print('K', len(tiles), 'time', time.time()-t)
    render(tiles, cellmap, os.path.join(OUT, 'nebula_k%d.png' % K))
    # also the unreduced version for comparison
    full, fmap = reduce_tiles(img, 10000)
    render(full, fmap, os.path.join(OUT, 'nebula_full_q.png'))
