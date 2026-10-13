import sys, collections, statistics, json, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from paths import SRC
from skoolkit import CSimulator
from skoolkit.trace import Tracer
from skoolkit.simutils import from_snapshot
from skoolkit.snapshot import Snapshot

def make_sim():
    snapshot = Snapshot.get(os.path.join(SRC, 'JETPAC.SNA'))
    snapshot.sp = (snapshot.sp + 2) % 65536
    sim = from_snapshot(CSimulator, snapshot, {}, {'ay': [None]*16}, {'fast_djnz': False, 'fast_ldir': False}, None)
    tracer = Tracer(sim, snapshot.border, snapshot.out7ffd, snapshot.outfffd, list(snapshot.ay), snapshot.outfe, False)
    sim.set_tracer(tracer)
    return sim, tracer, snapshot.pc

def run(NF, script, stop=0x69C7):
    sim, tracer, pc = make_sim()
    mem = sim.memory
    kstate=[0]*8
    tracer.keyboard = kstate
    count=[0]
    def cb(scr, frame, border, keyboard):
        n = count[0]
        for (r,m) in script.get(n, []): kstate[r]=m
        count[0]+=1
        return n < NF
    recs=[]
    while count[0] < NF:
        stop_cond, ops = sim.trace(pc, stop, 0, 0, True, cb, None, kstate, None, None)
        if stop_cond != 3: break
        T = sim.registers[25]
        mode = 'F' if mem[0x699c+55]==0xC3 else 'P'
        slots = [mem[0x5D30+8*i] & 0x3F for i in range(11)]
        recs.append((count[0], T, mode, slots, mem[0x5D00]))
        pc = stop
    return recs

if __name__ == '__main__':
    NF = int(sys.argv[1]) if len(sys.argv)>1 else 900
    script = {40:[(3,16)], 46:[(3,0)]}
    recs = run(NF, script)
    print('records', len(recs))
    Fc=[]; pt=[]; lastP=None; fcost=0
    for i,(f,T,mode,slots,jm) in enumerate(recs):
        if i==0: continue
        d = T-recs[i-1][1]
        if mode=='F':
            Fc.append(d); fcost += d
        else:
            if lastP is not None: pt.append((f, T-lastP-fcost, slots))
            lastP=T; fcost=0
    print('frame subpass cost: n=%d mean=%.0f min=%d max=%d'%(len(Fc),statistics.mean(Fc),min(Fc),max(Fc)))
    by=collections.defaultdict(list)
    for f,c,slots in pt: by[tuple(slots)].append(c)
    for key,cs in sorted(by.items(), key=lambda kv:-len(kv[1]))[:30]:
        print(key, 'n=%d mean=%.0f'%(len(cs), statistics.mean(cs)))
    idle=[c for f,c,s in pt if 60<f<170]
    if idle: print('idle pass mean', statistics.mean(idle), 'frames/pass', statistics.mean(idle)/69888)
    busy=[c for f,c,s in pt if 300<f<NF]
    if busy: print('busy pass mean', statistics.mean(busy), 'frames/pass', statistics.mean(busy)/69888)
