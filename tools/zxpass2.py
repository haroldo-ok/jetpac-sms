import sys, collections, statistics
from zxpass import make_sim
NF = int(sys.argv[1]) if len(sys.argv)>1 else 900
script = {40:[(3,16)], 46:[(3,0)]}
sim, tracer, pc = make_sim()
mem = sim.memory
kstate=[0]*8; tracer.keyboard=kstate
count=[0]
def cb(scr, frame, border, keyboard):
    n=count[0]
    for (r,m) in script.get(n, []): kstate[r]=m
    count[0]+=1
    return n < NF
FU=0x6965; PE=0x69C7
stop=PE
events=[]   # (kind, T, slots, jm)
while count[0] < NF:
    sc, ops = sim.trace(pc, stop, 0, 0, True, cb, None, kstate, None, None)
    if sc != 3: break
    T=sim.registers[25]
    if stop==FU:
        events.append(('FU', T, None, None)); stop=PE
    else:
        mode = 'F' if mem[0x699c+55]==0xC3 else 'P'
        slots=[mem[0x5D30+8*i]&0x3F for i in range(11)]
        events.append((mode, T, slots, mem[0x5D00]&0x3F))
        if mode=='P': stop=FU   # wait: next could be FU or PE; we must not miss PE...
        stop = PE if mode=='F' else stop
    pc = stop
# PROBLEM: after a P we don't know whether FU or PE comes first. Handle by using
# a different approach: instrument by stopping at PE only but also count interrupts via T: 
print(len(events))
