#!/usr/bin/env python3
"""Profile build: compile with statics exposed, run a smstest script with
profstart/profstop, aggregate cycles by nearest preceding symbol."""
import os, subprocess, sys, re, collections
DK = os.path.expanduser('~/.devkitsms')
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PB = '/tmp/profbuild'
os.makedirs(PB, exist_ok=True)
CFLAGS = f"-mz80 -I{DK}/include --peep-file {DK}/include/peep-rules.txt -I{ROOT}/src -I{ROOT}/build --opt-code-speed -Dstatic=".split()
objs = []
for m in ['main', 'game', 'render', 'sound', 'gen_tables', 'gen_sfx']:
    o = f'{PB}/{m}.rel'
    subprocess.check_call(['sdcc'] + CFLAGS + ['-c', f'{ROOT}/src/{m}.c', '-o', o])
    objs.append(o)
banks = [f'{ROOT}/build/bank2.rel', f'{ROOT}/build/bank3.rel', f'{ROOT}/build/isr.rel']
cmd = ['sdcc', '-o', f'{PB}/jetpac.ihx', '-mz80', '--no-std-crt0', '--data-loc', '0xC000',
       '-Wl-b_BANK2=0x28000', '-Wl-b_BANK3=0x38000', f'{DK}/lib/crt0_sms.rel'] + objs + [f'{DK}/lib/SMSlib.lib', f'{DK}/lib/PSGlib.lib'] + banks
subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
subprocess.check_call([f'{DK}/bin/ihx2sms', f'{PB}/jetpac.ihx', f'{PB}/jetpac.sms'], stdout=subprocess.DEVNULL)
# symbols
syms = []
for line in open(f'{PB}/jetpac.noi'):
    m = re.match(r'DEF (\S+) 0x([0-9A-Fa-f]+)', line)
    if m:
        a = int(m.group(2), 16)
        if a < 0x10000 and not m.group(1).startswith(('l__', 's__', '.__')):
            syms.append((a, m.group(1)))
syms.sort()
# script
pre = int(sys.argv[1]) if len(sys.argv) > 1 else 200
span = int(sys.argv[2]) if len(sys.argv) > 2 else 100
extra = sys.argv[3] if len(sys.argv) > 3 else ''
script = f"symbols {PB}/jetpac.noi\nrun {pre}\n{extra}\nprint debug_frames w1\nprofstart\nrun {span}\nprofstop\nprofdump {PB}/prof.txt\nprint debug_frames w1\n"
open(f'{PB}/prof.in', 'w').write(script)
out = subprocess.run([f'{DK}/bin/smstest', f'{PB}/jetpac.sms', f'{PB}/prof.in'], capture_output=True, text=True)
print(out.stdout[-600:])
import bisect
addrs = [a for a, _ in syms]
agg = collections.Counter()
total = 0
for line in open(f'{PB}/prof.txt'):
    p = line.split()
    if len(p) != 2: continue
    a = int(p[0], 16); c = int(p[1])
    i = bisect.bisect_right(addrs, a) - 1
    name = syms[i][1] if i >= 0 else '?'
    agg[name] += c; total += c
print(f'total cycles {total}  ({total/span:.0f}/frame, frame = 59736)')
for name, c in agg.most_common(40):
    print(f'{c:10d} {100*c/total:5.1f}% {name}')
