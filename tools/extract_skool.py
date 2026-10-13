"""Parse the mrcook jetpac.skool to pull all data blocks (defb) with their labels & addresses."""
import re, json, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from paths import SKOOL, TOOLS
skool = open(SKOOL).read().splitlines()
blocks = {}   # label -> {'addr': int, 'bytes': [...]}
cur = None
label = None
addr_re = re.compile(r'^[bwtcsi ]?\$([0-9a-f]{4}) (defb|defw|defm|defs) (.*)$', re.I)
pending_label = None
for line in skool:
    if line.startswith('@label='):
        pending_label = line[7:].strip()
        continue
    m = addr_re.match(line)
    if not m:
        if line.strip()=='' :
            pass
        continue
    addr = int(m.group(1),16); kind=m.group(2).lower(); rest=m.group(3)
    rest = rest.split(';')[0].strip()
    data=[]
    if kind=='defb':
        for tok in rest.split(','):
            tok=tok.strip()
            if tok.startswith('$'): data.append(int(tok[1:],16))
            elif tok.isdigit(): data.append(int(tok))
            elif tok.startswith('"'):
                data += [ord(c) for c in tok[1:-1]]
    elif kind=='defm':
        # defm "...." possibly with ,$xx
        s = re.findall(r'"([^"]*)"', rest)
        for part in s: data += [ord(c) for c in part]
        tail = rest.split('"')[-1]
        for tok in tail.split(','):
            tok=tok.strip()
            if tok.startswith('$'): data.append(int(tok[1:],16))
    elif kind=='defw':
        for tok in rest.split(','):
            tok=tok.strip()
            if tok.startswith('$'):
                v=int(tok[1:],16); data += [v&255, v>>8]
    elif kind=='defs':
        n=int(rest.split(',')[0].strip()[1:],16) if rest.startswith('$') else int(rest.split(',')[0])
        data=[0]*n
    if line[0] in 'bwts' and pending_label:
        label = pending_label
        blocks[label] = {'addr': addr, 'bytes': []}
    pending_label=None
    if label and label in blocks:
        # only append if contiguous
        b = blocks[label]
        if b['addr'] + len(b['bytes']) == addr:
            b['bytes'] += data
json.dump(blocks, open(os.path.join(TOOLS, 'skool_blocks.json'),'w'))
for k,v in blocks.items():
    if k.startswith('gfx_') or k.startswith('tile_'):
        print(f"{k:32s} ${v['addr']:04x} len={len(v['bytes'])} head={v['bytes'][:3]}")
