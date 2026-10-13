"""Render every Spec256-coloured graphic (sprites, tiles, font) to a sheet PNG for inspection."""
import json, os, sys
from PIL import Image, ImageDraw
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from paths import SRC, TOOLS, OUT
blocks=json.load(open(os.path.join(TOOLS, 'skool_blocks.json')))
sna=open(os.path.join(SRC, 'JETPAC.SNA'),'rb').read(); ram=sna[27:]
gfx=open(os.path.join(SRC, 'JETPAC.GFX'),'rb').read()
romgfx=open(os.path.join(SRC, 'ROM0.GFX'),'rb').read()
rom=open(os.path.join(SRC, '48.rom'),'rb').read()
p=open(os.path.join(SRC, 'spec256.raw.pal'),'rb').read()
PAL=[(p[i*3],p[i*3+1],p[i*3+2]) for i in range(256)]

def pix_colors(addr):
    """return list of 8 (bit,colour) for byte at Spectrum address (RAM or ROM)."""
    if addr>=0x4000:
        b=ram[addr-0x4000]; base=(addr-0x4000)*8; src=gfx
    else:
        b=rom[addr]; base=addr*8; src=romgfx
    out=[]
    for x in range(8):
        bit=(b>>(7-x))&1
        col=src[base+(7-x)]
        out.append((bit,col))
    return out

DELTA=53
def sprite_image(name):
    v=blocks[name]; a=v['addr']+DELTA; data=v['bytes']
    if name.startswith('gfx_') and not name.startswith('gfx_params'):
        if data[0] in (0,8) and len(data)>3 and data[1] in (2,3):
            xoff,w,h=data[0],data[1],data[2]; off=3
        else:
            h=data[0]; w=2; xoff=0; off=1
        img=Image.new('RGBA',(w*8,h),(0,0,0,0)); grid=[]
        for y in range(h):
            row=[]
            for c in range(w):
                for bit,col in pix_colors(a+off+y*w+c):
                    row.append((bit,col))
            grid.append(row)
            for x,(bit,col) in enumerate(row):
                if bit: img.putpixel((x,y),PAL[col]+(255,))
        return img,grid,(xoff,w,h)
    else:  # 8x8 tile
        img=Image.new('RGBA',(8,8),(0,0,0,0)); grid=[]
        for y in range(8):
            row=pix_colors(a+y); grid.append(row)
            for x,(bit,col) in enumerate(row):
                if bit: img.putpixel((x,y),PAL[col]+(255,))
        return img,grid,(0,1,8)

names=[k for k in blocks if (k.startswith('gfx_') and k!='gfx_params_platforms') or k.startswith('tile_')]
SC=4
sheet=Image.new('RGBA',(1400,2000),(40,40,40,255)); d=ImageDraw.Draw(sheet)
x=y=10; rowh=0
stats={}
for n in names:
    img,grid,(xoff,w,h)=sprite_image(n)
    cols={}
    for row in grid:
        for bit,col in row:
            if bit: cols[col]=cols.get(col,0)+1
    zero_set=sum(1 for row in grid for bit,col in row if bit and col==0)
    colset_unset=sum(1 for row in grid for bit,col in row if (not bit) and col!=0)
    stats[n]={'cols':cols,'set_but_col0':zero_set,'unset_but_col':colset_unset,'xoff':xoff,'w':w,'h':h}
    big=img.resize((img.width*SC,img.height*SC),Image.NEAREST)
    if x+big.width+10>sheet.width: x=10; y+=rowh+28; rowh=0
    sheet.paste(big,(x,y+14),big); d.text((x,y),n[4:] if n.startswith('gfx_') else n,fill=(255,255,255,255))
    x+=max(big.width,110)+12; rowh=max(rowh,big.height+14)
# font
y+=rowh+40; x=10
for c in range(32,128):
    img=Image.new('RGBA',(8,8),(0,0,0,0)); cols={}
    for r in range(8):
        for xx,(bit,col) in enumerate(pix_colors(0x3D00+(c-32)*8+r)):
            if bit: img.putpixel((xx,r),PAL[col]+(255,)); cols[col]=cols.get(col,0)+1
    stats['font_%d'%c]={'cols':cols}
    big=img.resize((8*SC,8*SC),Image.NEAREST)
    if x+big.width+4>sheet.width: x=10; y+=big.height+6
    sheet.paste(big,(x,y),big); x+=big.width+4
sheet=sheet.crop((0,0,1400,y+60))
sheet.save(os.path.join(OUT, 'sheet.png'))
json.dump(stats,open(os.path.join(TOOLS, 'colour_stats.json'),'w'),indent=0)
for n,s in stats.items():
    if not n.startswith('font'):
        print(n, 'xoff',s['xoff'],'w',s['w'],'h',s['h'],'cols',sorted(s['cols'].items()), 'set&col0',s['set_but_col0'],'unset&col',s['unset_but_col'])
