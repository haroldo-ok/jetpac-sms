"""Measure per-object-slot update cost (T-states) in the original game by
stopping at the dispatch point of the main loop.
usage: zxslots.py NF [k:frame:row:mask | p:frame:addr:val]..."""
import sys, collections, statistics
from zxpass import make_sim
NF = int(sys.argv[1]) if len(sys.argv)>1 else 900
keys = {40:[(3,16)], 46:[(3,0)]}
pokes = {}
for extra in sys.argv[2:]:
    p = extra.split(':')
    if p[0]=='k':
        keys.setdefault(int(p[1]),[]).append((int(p[2]),int(p[3])))
    else:
        pokes.setdefault(int(p[1]),[]).append((int(p[2],16),int(p[3],16)))
sim, tracer, pc = make_sim()
mem = sim.memory
kstate=[0]*8; tracer.keyboard=kstate
count=[0]
def cb(scr, frame, border, keyboard):
    n=count[0]
    for (r,m) in keys.get(n, []): kstate[r]=m
    for (a,v) in pokes.get(n, []): mem[a]=v
    count[0]+=1
    return n < NF
DISPATCH = 0x6360
ev=[]
stop=DISPATCH
while count[0] < NF:
    sc, ops = sim.trace(pc, stop, 0, 0, True, cb, None, kstate, None, None)
    if sc != 3: break
    T=sim.registers[25]
    ix = (sim.registers[8]<<8)|sim.registers[9]
    ev.append((count[0], T, ix, mem[ix]&0x3F, mem[0x5D00]&0x3F, mem[0x5DD7]))
    pc=stop
print('events', len(ev))
cost=collections.defaultdict(list)
for i in range(len(ev)-1):
    f,T,ix,typ,jm,dly = ev[i]
    d = ev[i+1][1]-T
    key=(ix,typ) if not (ix==0x5d00 and dly) else (ix,'delay')
    cost[key].append(d)
for key in sorted(cost, key=str):
    cs=cost[key]
    print('ix=%04x type=%5s n=%5d mean=%6.0f med=%6.0f min=%6d max=%6d'%(key[0],key[1],len(cs),statistics.mean(cs),statistics.median(cs),min(cs),max(cs)))
# print per-frame summary of jetman and aliens every 25 frames
if '-v' in sys.argv:
    pass
