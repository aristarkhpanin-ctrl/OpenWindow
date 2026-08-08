import numpy as np, sys, glob, os
sys.path.insert(0,'/tmp/claude-0/-home-user-OpenWindow/59115f57-030f-5de7-91de-43c1797850bc/scratchpad')
from render import load

# Detect cylindrical faces: group triangles by normal perpendicular to a chosen axis,
# fit circles to edge loops in projection.
def find_circles(tris, axis=2, tol=0.02):
    ax = [a for a in range(3) if a != axis]
    n = np.cross(tris[:,1]-tris[:,0], tris[:,2]-tris[:,0])
    ln = np.linalg.norm(n,axis=1); ln[ln==0]=1; n=n/ln[:,None]
    # cylinder walls: normal has ~zero component along axis
    m = np.abs(n[:,axis]) < 0.05
    t = tris[m]
    if len(t)==0: return []
    # each such triangle is vertical wall; take its 2D projected edge midpoints
    pts = t.reshape(-1,3)[:, ax]
    # cluster by connectivity in 2D using grid hashing then fit circles via algebraic fit
    return t

def circle_fit(P):
    x=P[:,0]; y=P[:,1]
    A = np.c_[2*x, 2*y, np.ones(len(P))]
    b = x**2+y**2
    sol,*_ = np.linalg.lstsq(A,b,rcond=None)
    cx,cy,c = sol
    r = np.sqrt(c+cx**2+cy**2)
    res = np.abs(np.hypot(x-cx,y-cy)-r).max()
    return cx,cy,r,res

def cylinders(tris, axis=2):
    ax=[a for a in range(3) if a!=axis]
    n = np.cross(tris[:,1]-tris[:,0], tris[:,2]-tris[:,0])
    ln=np.linalg.norm(n,axis=1); ln[ln==0]=1; n=n/ln[:,None]
    m = np.abs(n[:,axis])<0.03
    t = tris[m]
    if len(t)==0: return []
    # connected components by shared vertices (rounded)
    key = lambda p: (round(p[0],3), round(p[1],3), round(p[2],3))
    parent = list(range(len(t)))
    def find(a):
        while parent[a]!=a: parent[a]=parent[parent[a]]; a=parent[a]
        return a
    def uni(a,b):
        ra,rb=find(a),find(b)
        if ra!=rb: parent[rb]=ra
    vmap={}
    for i in range(len(t)):
        for j in range(3):
            k=key(t[i,j])
            if k in vmap: uni(vmap[k], i)
            else: vmap[k]=i
    groups={}
    for i in range(len(t)): groups.setdefault(find(i),[]).append(i)
    out=[]
    for g,idx in groups.items():
        if len(idx)<6: continue
        P = t[idx].reshape(-1,3)
        cx,cy,r,res = circle_fit(P[:,ax])
        if res < max(0.06, r*0.03) and r>0.5:
            lo,hi = P[:,axis].min(), P[:,axis].max()
            out.append((round(2*r,3), round(cx,2), round(cy,2), round(lo,2), round(hi,2), len(idx)))
    return sorted(out)

if __name__=='__main__':
    for p in sorted(glob.glob(sys.argv[1]+'/*.stl')):
        t = load(p)
        print('='*70); print(os.path.basename(p))
        for axis,label in [(2,'Z'),(0,'X'),(1,'Y')]:
            cy = cylinders(t, axis)
            if cy:
                print(f'  axis {label}: ' + '; '.join(f"D={d} @({a},{b}) span[{lo}..{hi}]" for d,a,b,lo,hi,nn in cy))
