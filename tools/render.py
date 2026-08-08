import numpy as np, struct, zlib, sys, glob, os

def load(path):
    with open(path,'rb') as f:
        head = f.read(84); n = struct.unpack('<I', head[80:84])[0]; data = f.read()
    arr = np.frombuffer(data[:n*50], dtype=np.uint8).reshape(n,50)
    return arr[:,12:48].copy().view('<f4').reshape(n,3,3).astype(np.float64)

def write_png(path, img):  # img uint8 HxWx3
    h,w,_ = img.shape
    raw = b''.join(b'\x00' + img[y].tobytes() for y in range(h))
    def chunk(t,d):
        c = t+d; return struct.pack('>I',len(d))+c+struct.pack('>I', zlib.crc32(c)&0xffffffff)
    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB',w,h,8,2,0,0,0))
    png += chunk(b'IDAT', zlib.compress(raw,6))
    png += chunk(b'IEND', b'')
    open(path,'wb').write(png)

def rot(az, el):
    a = np.radians(az); e = np.radians(el)
    Rz = np.array([[np.cos(a),-np.sin(a),0],[np.sin(a),np.cos(a),0],[0,0,1]])
    Rx = np.array([[1,0,0],[0,np.cos(e),-np.sin(e)],[0,np.sin(e),np.cos(e)]])
    return Rx @ Rz

def render(tris_list, colors, az, el, W=760, H=760, bounds=None):
    R = rot(az, el)
    allp = np.vstack([t.reshape(-1,3) for t in tris_list])
    if bounds is None:
        c = (allp.max(0)+allp.min(0))/2
    else:
        c = (np.array(bounds[0])+np.array(bounds[1]))/2
    v = (allp - c) @ R.T
    if bounds is None:
        span = max(v[:,0].max()-v[:,0].min(), v[:,2].max()-v[:,2].min())*1.10
    else:
        bp = np.array([[x,y,z] for x in bounds[0][0:1]+bounds[1][0:1] for y in bounds[0][1:2]+bounds[1][1:2] for z in bounds[0][2:3]+bounds[1][2:3]])
        span = 1.0
    scale = min(W,H)/max(span,1e-6)
    zbuf = np.full((H,W), -1e18); img = np.zeros((H,W,3), np.uint8); img[:] = 250
    light = np.array([0.35,-0.75,0.55]); light/=np.linalg.norm(light)
    for tris, col in zip(tris_list, colors):
        p = (tris.reshape(-1,3) - c) @ R.T
        p = p.reshape(-1,3,3)
        n = np.cross(p[:,1]-p[:,0], p[:,2]-p[:,0])
        ln = np.linalg.norm(n,axis=1); ln[ln==0]=1; n = n/ln[:,None]
        sh = np.clip(np.abs(n @ light),0,1)*0.75 + 0.25
        sx = p[:,:,0]*scale + W/2; sy = -p[:,:,2]*scale + H/2; sz = p[:,:,1]
        for i in range(len(p)):
            x0,x1,x2 = sx[i]; y0,y1,y2 = sy[i]
            minx = max(int(np.floor(min(x0,x1,x2))),0); maxx = min(int(np.ceil(max(x0,x1,x2))),W-1)
            miny = max(int(np.floor(min(y0,y1,y2))),0); maxy = min(int(np.ceil(max(y0,y1,y2))),H-1)
            if minx>maxx or miny>maxy: continue
            xs = np.arange(minx,maxx+1); ys = np.arange(miny,maxy+1)
            X,Y = np.meshgrid(xs,ys)
            d = (y1-y2)*(x0-x2)+(x2-x1)*(y0-y2)
            if abs(d)<1e-9: continue
            w0 = ((y1-y2)*(X-x2)+(x2-x1)*(Y-y2))/d
            w1 = ((y2-y0)*(X-x2)+(x0-x2)*(Y-y2))/d
            w2 = 1-w0-w1
            m = (w0>=-1e-6)&(w1>=-1e-6)&(w2>=-1e-6)
            if not m.any(): continue
            zz = w0*sz[i,0]+w1*sz[i,1]+w2*sz[i,2]
            sub = zbuf[miny:maxy+1, minx:maxx+1]
            upd = m & (zz > sub)
            if not upd.any(): continue
            sub[upd] = zz[upd]
            px = (np.array(col)*sh[i]).astype(np.uint8)
            img[miny:maxy+1, minx:maxx+1][upd] = px
    return img

if __name__ == '__main__':
    src = sys.argv[1]; out = sys.argv[2]
    os.makedirs(out, exist_ok=True)
    for p in sorted(glob.glob(src+'/*.stl')):
        t = load(p); name = os.path.basename(p)[:-4]
        views = [render([t],[(90,140,220)],az,el) for az,el in [(35,65),(125,65),(0,90)]]
        img = np.hstack(views)
        write_png(f"{out}/{name}.png", img)
        print(name)
