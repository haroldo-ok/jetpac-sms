import json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from paths import SRC, TOOLS
blocks=json.load(open(os.path.join(TOOLS, 'skool_blocks.json')))
sna=open(os.path.join(SRC, 'JETPAC.SNA'),'rb').read()
ram=sna[27:]
def find_all(seq):
    seq=bytes(seq); res=[]; i=ram.find(seq)
    while i!=-1:
        res.append(i+0x4000); i=ram.find(seq,i+1)
    return res
offsets={}
for k,v in blocks.items():
    if not (k.startswith('gfx_') or k.startswith('tile_') or k in ('item_drop_positions_table','menu_text','explosion_sfx_defaults','default_alien_state','laser_beam_colours','item_level_object_types','default_player_state','default_rocket_state')): continue
    if len(v['bytes'])<8: 
        locs=find_all(v['bytes'])
    else:
        locs=find_all(v['bytes'])
    offs=[l-v['addr'] for l in locs]
    offsets[k]=offs
    print(f"{k:32s} skool=${v['addr']:04x} sna={[hex(l) for l in locs]} delta={offs}")
