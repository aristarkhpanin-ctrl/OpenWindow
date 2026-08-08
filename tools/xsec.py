import numpy as np, sys, os
sys.path.insert(0,'/tmp/claude-0/-home-user-OpenWindow/59115f57-030f-5de7-91de-43c1797850bc/scratchpad')
from render import load, write_png
from slice import slice_segments

SRC='/home/user/OpenWindow/pok016'
OUT='/tmp/claude-0/-home-user-OpenWindow/59115f57-030f-5de7-91de-43c1797850bc/scratchpad/renders'

def draw_multi(items, axis, W=1000, H=1000, grid=10, fname='x.png'):
    """items: list of (segments, color, label)"""
    ax=[a for a in range(3) if a!=axis]
    allp=np.vstack([s.reshape(-1,3) for s,_ in items if len(s)])
    mn=np.array([allp[:,ax[0]].min(), allp[:,ax[1]].min()])
    mx=np.array([allp[:,ax[0]].max(), allp[:,ax[1]].max()])
    c=(mn+mx)/2; span=(mx-mn).max()*1.12
    sc=min(W,H)/span
    img=np.full((H,W,3),255,np.uint8)
    # grid every `grid` mm
    g0=np.floor((c[0]-span/2)/grid)*grid
    while g0 < c[0]+span/2:
        x=int((g0-c[0])*sc+W/2)
        if 0<=x<W: img[:,x]=(225,225,235) if abs(g0)>1e-9 else (180,180,255)
        g0+=grid
    g1=np.floor((c[1]-span/2)/grid)*grid
    while g1 < c[1]+span/2:
        y=int(H/2-(g1-c[1])*sc)
        if 0<=y<H: img[y,:]=(225,225,235) if abs(g1)>1e-9 else (180,180,255)
        g1+=grid
    for segs,col in items:
        for s in segs:
            x0=(s[0,ax[0]]-c[0])*sc+W/2; y0=H/2-(s[0,ax[1]]-c[1])*sc
            x1=(s[1,ax[0]]-c[0])*sc+W/2; y1=H/2-(s[1,ax[1]]-c[1])*sc
            n=int(max(abs(x1-x0),abs(y1-y0)))+1
            xs=np.linspace(x0,x1,n).astype(int); ys=np.linspace(y0,y1,n).astype(int)
            ok=(xs>=0)&(xs<W)&(ys>=0)&(ys<H)
            img[ys[ok],xs[ok]]=col
            img[np.clip(ys[ok]+1,0,H-1),xs[ok]]=col
    write_png(f'{OUT}/{fname}', img)
    names='XYZ'
    print(f"{fname}: axis={names[axis]} horiz={names[ax[0]]} vert={names[ax[1]]} "
          f"range {names[ax[0]]}[{mn[0]:.1f}..{mx[0]:.1f}] {names[ax[1]]}[{mn[1]:.1f}..{mx[1]:.1f}] "
          f"grid={grid}mm scale={sc:.2f}px/mm")

if __name__=='__main__':
    import json
    spec=json.loads(sys.argv[1])
    items=[]
    for p in spec['parts']:
        t=load(f"{SRC}/{p['file']}")
        segs=slice_segments(t, spec['axis'], p['val'])
        items.append((segs, tuple(p.get('color',(20,20,20)))))
    draw_multi(items, spec['axis'], grid=spec.get('grid',10), fname=spec['out'])
