import os
from paths import OUT
from images import *
from palette import *
from collections import Counter
from PIL import Image, ImageDraw
J=jetman_images(); A=alien_images(); R=rocket_images(); I=item_images(); E=explosion_images(); F=flame_images()
def hist(imgs, norm=True):
    c=Counter()
    for im in imgs:
        h=Counter()
        for row in im['rows']:
            for p in row:
                if p is not None: h[q6(p)]+=1
        tot=sum(h.values())
        for k,v in h.items(): c[k]+= (v/tot*100 if norm else v)
    return c
import sys
K=int(sys.argv[1]) if len(sys.argv)>1 else 10
core=Counter()
for grp,wt in ((J,1.0),(I,1.5),(E,0.5),(F,0.5)):
    for k,v in hist(grp).items(): core[k]+=v*wt
CORE12=[(3,1,0),(3,2,1),(2,1,1),(1,1,1),(2,2,2),(3,3,3),(1,1,2),(2,0,0),(2,1,2),(2,2,0),(3,3,0),(1,2,2)]
CORE13=CORE12+[(2,3,0)]
corepal=CORE12 if K==12 else CORE13
print('core', corepal)
def render(imgs, pal, S=3):
    W=sum(im['w'] for im in imgs)*S+len(imgs)*6; H=max(im['h'] for im in imgs)*S
    sheet=Image.new('RGB',(W,H),(40,40,60)); x=0
    for im in imgs:
        for y,row in enumerate(im['rows']):
            for xx,p in enumerate(row):
                if p is not None:
                    c=pal[nearest(pal,q6(p))]
                    for dy in range(S):
                        for dx in range(S): sheet.putpixel((x+xx*S+dx,y*S+dy),rgb255(c))
        x+=im['w']*S+6
    return sheet
rows=[render(J,corepal),render(I+E+F,corepal)]
for lvl in range(16):
    al=A[lvl&7]; rk=R[(lvl>>2)&3]
    dyn=Counter()
    for k,v in hist(al).items(): dyn[k]+=v
    for k,v in hist(rk[:3]).items(): dyn[k]+=v*0.7
    for k,v in hist(rk[3:]).items(): dyn[k]+=v*0.4
    pal=cluster(dyn,15,fixed=corepal)
    print(lvl, 'dyn', [c for c in pal if c not in corepal])
    rows.append(render(al+rk,pal))
W=max(r.width for r in rows); H=sum(r.height+4 for r in rows)
out=Image.new('RGB',(W,H),(0,0,0)); y=0
for r in rows: out.paste(r,(0,y)); y+=r.height+4
out.save(os.path.join(OUT, 'palcheck%d.png'%K))
