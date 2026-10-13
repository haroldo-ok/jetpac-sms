#!/usr/bin/env python3
"""Annotate a function from the profile build with per-instruction cycles."""
import sys, re, bisect
from skoolkit.disassembler import Disassembler
PB='/tmp/profbuild'
fn = sys.argv[1]
top = int(sys.argv[2]) if len(sys.argv) > 2 else 40
syms=[]
for line in open(f'{PB}/jetpac.noi'):
    m = re.match(r'DEF (\S+) 0x([0-9A-Fa-f]+)', line)
    if m:
        a=int(m.group(2),16)
        if a < 0x10000 and not m.group(1).startswith(('l__','s__','.__')): syms.append((a,m.group(1)))
syms.sort()
start=[a for a,n in syms if n==fn][0]
idx=[a for a,_ in syms].index(start)
end=syms[idx+1][0]
prof={}
for line in open(f'{PB}/prof.txt'):
    p=line.split()
    if len(p)==2: prof[int(p[0],16)]=int(p[1])
rom=open(f'{PB}/jetpac.sms','rb').read()
mem=bytearray(65536); mem[:0x10000]=rom[:0x10000]
class Cfg: pass
cfg=Cfg(); cfg.defb_size=8; cfg.defm_size=65; cfg.defw_size=1; cfg.handle_rst=0; cfg.imaker=None; cfg.wrap=0; cfg.opcodes='ALL'
cfg.asm_hex=0; cfg.asm_lower=1; cfg.title_hex=1; cfg.title_lower=1; cfg.text_hex=1
try:
    d=Disassembler(mem, cfg)
except Exception as e:
    print('cfg err', e); raise
from skoolkit.snaskool import Instruction
import skoolkit.disassembler as D
class Ins:
    def __init__(self, address, operation, data):
        self.address=address; self.operation=operation; self.data=data
cfg.imaker=Ins
d=Disassembler(mem, cfg)
rows=[]
for ins in d.disassemble(start, end, 'h'):
    a=ins.address; data=ins.data
    rows.append((a, sum(prof.get(a+k,0) for k in range(len(data))), ins.operation))
tot=sum(c for _,c,_ in rows)
print(f'{fn} {start:04x}-{end:04x} total {tot}')
for a,c,op in rows:
    print(f'{a:04x} {c:9d} {100*c/max(tot,1):5.1f}%  {op}')
