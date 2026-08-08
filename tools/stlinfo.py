import numpy as np, struct, sys, glob, os

def load(path):
    with open(path,'rb') as f:
        head = f.read(84)
        n = struct.unpack('<I', head[80:84])[0]
        data = f.read()
    if len(data) < n*50:
        return None, None
    arr = np.frombuffer(data[:n*50], dtype=np.uint8).reshape(n,50)
    tri = arr[:,12:48].copy().view('<f4').reshape(n,3,3)
    nrm = arr[:,0:12].copy().view('<f4').reshape(n,3)
    return tri, nrm

def volume(tri):
    a,b,c = tri[:,0,:], tri[:,1,:], tri[:,2,:]
    return np.abs(np.sum(np.einsum('ij,ij->i', a, np.cross(b,c)))/6.0)

for p in sorted(glob.glob(sys.argv[1]+'/*.stl')):
    tri, nrm = load(p)
    if tri is None:
        print(p, 'ASCII/parse fail'); continue
    pts = tri.reshape(-1,3)
    mn, mx = pts.min(0), pts.max(0)
    size = mx-mn
    v = volume(tri)
    print(f"{os.path.basename(p):34s} tris={len(tri):6d}  size(mm)= {size[0]:7.2f} x {size[1]:7.2f} x {size[2]:7.2f}   vol={v/1000:8.2f} cm3   min={np.round(mn,2)} max={np.round(mx,2)}")
