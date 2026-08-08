import numpy as np, sys, os
sys.path.insert(0,'/tmp/claude-0/-home-user-OpenWindow/59115f57-030f-5de7-91de-43c1797850bc/scratchpad')
from render import load, write_png

def slice_segments(tris, axis, val):
    d = tris[:,:,axis] - val
    pos = d > 0
    cnt = pos.sum(1)
    m = (cnt==1)|(cnt==2)
    t = tris[m]; dd = d[m]; pp = pos[m]
    segs = []
    for i in range(len(t)):
        pts=[]
        for a in range(3):
            b=(a+1)%3
            if pp[i,a] != pp[i,b]:
                f = dd[i,a]/(dd[i,a]-dd[i,b])
                pts.append(t[i,a] + f*(t[i,b]-t[i,a]))
        if len(pts)==2: segs.append(pts)
    return np.array(segs) if segs else np.zeros((0,2,3))

def draw(segs_sets, colors, axis, W=900, H=900, title=''):
    ax = [a for a in range(3) if a!=axis]
    allp = np.vstack([s.reshape(-1,3) for s in segs_sets if len(s)])
    if len(allp)==0: return None, None
    u = allp[:,ax[0]]; v = allp[:,ax[1]]
    mn = np.array([u.min(), v.min()]); mx = np.array([u.max(), v.max()])
    span = (mx-mn).max()*1.08
    c = (mn+mx)/2
    sc = min(W,H)/span
    img = np.full((H,W,3), 255, np.uint8)
    for segs, col in zip(segs_sets, colors):
        for s in segs:
            x0 = (s[0,ax[0]]-c[0])*sc + W/2; y0 = H/2 - (s[0,ax[1]]-c[1])*sc
            x1 = (s[1,ax[0]]-c[0])*sc + W/2; y1 = H/2 - (s[1,ax[1]]-c[1])*sc
            n = int(max(abs(x1-x0), abs(y1-y0)))+1
            xs = np.linspace(x0,x1,n).astype(int); ys = np.linspace(y0,y1,n).astype(int)
            ok = (xs>=0)&(xs<W)&(ys>=0)&(ys<H)
            img[ys[ok], xs[ok]] = col
            img[np.clip(ys[ok]+1,0,H-1), xs[ok]] = col
            img[ys[ok], np.clip(xs[ok]+1,0,W-1)] = col
    return img, (c, sc, span)
