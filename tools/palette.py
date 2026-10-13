"""Palette reduction helpers (agglomerative clustering on the SMS 6-bit colour cube)."""
import math

def rgb255(c6):
    return tuple(v*85 for v in c6)

def dist2(a, b):
    # perceptual-ish weights
    return 0.3*(a[0]-b[0])**2 + 0.59*(a[1]-b[1])**2 + 0.11*(a[2]-b[2])**2

def snap6(rgb):
    return tuple(min(3, max(0, int(round(v/85.0)))) for v in rgb)

def cluster(weighted, k, fixed=()):
    """weighted: dict {rgb6: weight}.  fixed: colours that must be in the result
    (they act as centroids that absorb nearby colours).  Returns list of rgb6 (len<=k)
    including the fixed ones first."""
    fixed = [tuple(f) for f in fixed]
    # clusters: list of [sum_r, sum_g, sum_b, weight, members, is_fixed, colour]
    cl = []
    for f in fixed:
        cl.append({'w': 0.0, 'sum': [0.0, 0.0, 0.0], 'fixed': True, 'col': f})
    for c, w in weighted.items():
        c = tuple(c)
        if c in fixed:
            continue
        cl.append({'w': float(w), 'sum': [w*c[0], w*c[1], w*c[2]], 'fixed': False, 'col': c})

    def centroid(x):
        if x['fixed']:
            return x['col']
        return tuple(s/x['w'] for s in x['sum'])

    while len(cl) > k:
        best = None
        for i in range(len(cl)):
            for j in range(i+1, len(cl)):
                a, b = cl[i], cl[j]
                if a['fixed'] and b['fixed']:
                    continue
                ca, cb = centroid(a), centroid(b)
                if a['fixed']:
                    cost = b['w'] * dist2(ca, cb)
                elif b['fixed']:
                    cost = a['w'] * dist2(ca, cb)
                else:
                    cost = (a['w']*b['w']/(a['w']+b['w'])) * dist2(ca, cb)
                if best is None or cost < best[0]:
                    best = (cost, i, j)
        _, i, j = best
        a, b = cl[i], cl[j]
        if a['fixed'] or b['fixed']:
            f = a if a['fixed'] else b
            cl = [x for n, x in enumerate(cl) if n not in (i, j)] + [f]
        else:
            m = {'w': a['w']+b['w'], 'sum': [a['sum'][n]+b['sum'][n] for n in range(3)], 'fixed': False, 'col': None}
            m['col'] = snap6(rgb255(centroid(m)))
            cl = [x for n, x in enumerate(cl) if n not in (i, j)] + [m]
    out = []
    for x in cl:
        c = x['col'] if x['fixed'] else snap6(rgb255(centroid(x)))
        if c not in out:
            out.append(c)
    return out

def nearest(pal, c6):
    best = None
    for i, p in enumerate(pal):
        d = dist2(p, c6)
        if best is None or d < best[0]:
            best = (d, i)
    return best[1]
