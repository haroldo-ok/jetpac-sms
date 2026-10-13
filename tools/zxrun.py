#!/usr/bin/env python3
"""Run the original Jetpac SNA in skoolkit's C simulator with scripted keyboard
input and log game variables every frame.  Used to measure the original's
object-pass rate / alien speeds so the SMS port can be tuned to match.

usage: python3 zxrun.py <frames> [script]
script = comma separated list of  frame:row:mask  (press from that frame on)
         e.g. "50:3:16,60:3:0"  presses key '5' from frame 50 to 60
Keyboard half-rows: 0=Shift..V 1=A..G 2=Q..T 3=1..5 4=0..6 5=P..Y 6=Enter..H 7=Space..B
"""
import sys, json, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from paths import SRC
from skoolkit import CSimulator
from skoolkit.trace import Tracer
from skoolkit.simutils import from_snapshot
from skoolkit.snapshot import Snapshot

SNA = os.path.join(SRC, 'JETPAC.SNA')

def main():
    nframes = int(sys.argv[1]) if len(sys.argv) > 1 else 600
    script = {}
    if len(sys.argv) > 2 and sys.argv[2]:
        for item in sys.argv[2].split(','):
            f, r, m = (int(x) for x in item.split(':'))
            script.setdefault(f, []).append((r, m))
    snapshot = Snapshot.get(SNA)
    if snapshot.type == 'SNA' and len(snapshot.tail) == 0xC000:
        snapshot.sp = (snapshot.sp + 2) % 65536
    sim = from_snapshot(CSimulator, snapshot, {}, {'ay': [None]*16}, {'fast_djnz': False, 'fast_ldir': False}, None)
    tracer = Tracer(sim, snapshot.border, snapshot.out7ffd, snapshot.outfffd, list(snapshot.ay), snapshot.outfe, False)
    sim.set_tracer(tracer)
    mem = sim.memory
    log = []
    kstate = [0]*8
    count = [0]
    def cb(scr, frame, border, keyboard):
        n = count[0]
        if n in script:
            for r, m in script[n]:
                kstate[r] = m
        keyboard[:] = kstate
        timer = mem[0x5DCC] | (mem[0x5DCD] << 8)
        aliens = []
        for i in range(6):
            b = mem[0x5D50 + 8*i: 0x5D58 + 8*i]
            if b[0]:
                aliens.append([b[0], b[1], b[2], b[4], b[5], b[6]])
        log.append({'f': n, 't': timer, 'jm': list(mem[0x5D00:0x5D08]), 'delay': mem[0x5DD7],
                    'lvl': mem[0x5DF0], 'al': aliens, 'rk': list(mem[0x5D30:0x5D38]),
                    'it': list(mem[0x5D40:0x5D48]), 'col': list(mem[0x5D38:0x5D40])})
        count[0] += 1
        return n < nframes
    tracer.run(snapshot.pc, None, 0, 0, True, cb, None, None, None, '$', '02X', '04X')
    json.dump(log, open('/tmp/zxlog.json', 'w'))
    print('frames', len(log))

if __name__ == '__main__':
    main()
