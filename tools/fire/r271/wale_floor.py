# r271 section 27: near-wall WALE opt-in check. argv: ckpt_state dump(s) (float64 T[z][y][x], U[z][y][x+1], V[z][y+1][x],
# W[z+1][y][x], Y[z][y][x]; ckpt_state.cpp). Per dump: core (r < 0.10 m) floor-layer T, and offline Vreman (C_v 0.07) and
# WALE (C_W 0.60) nu_t on layers 0-2 from the dump's own velocity with RISE's compact stencil (diagonal from faces,
# off-diagonal central cell differences, no-slip floor ghost -u, floor w from the face).
import numpy as np,sys
N=69;NZ=106;H=.030593116067411014;C=N*N*NZ
def load(p):
    a=np.fromfile(p,'<f8');o=0
    def take(shape):
        nonlocal o;n=int(np.prod(shape));x=a[o:o+n].reshape(shape);o+=n;return x
    T=take((NZ,N,N));U=take((NZ,N,N+1));V=take((NZ,N+1,N));W=take((NZ+1,N,N));return T,U,V,W
def gradients(U,V,W,K=3):
    uc=.5*(U[:,:,1:]+U[:,:,:-1]);vc=.5*(V[:,1:,:]+V[:,:-1,:]);wc=.5*(W[1:]+W[:-1])
    out=[]
    for k in range(K):
        g=np.zeros((N,N,3,3))  # g[...,d,c] = d u_c / d x_d
        for c,(cell,face) in enumerate([(uc,U),(vc,V),(wc,W)]):
            for d in range(3):
                if c==d:
                    if c==0:g[...,0,0]=(U[k,:,1:]-U[k,:,:-1])/H
                    if c==1:g[...,1,1]=(V[k,1:,:]-V[k,:-1,:])/H
                    if c==2:g[...,2,2]=(W[k+1]-W[k])/H
                    continue
                f=cell[k]
                if d==0:p=np.pad(f,((0,0),(1,1)),mode='edge');g[...,0,c]=(p[:,2:]-p[:,:-2])/(2*H)
                if d==1:p=np.pad(f,((1,1),(0,0)),mode='edge');g[...,1,c]=(p[2:]-p[:-2])/(2*H)
                if d==2:
                    lo=-cell[0] if k==0 else cell[k-1];g[...,2,c]=(cell[k+1]-lo)/(2*H)
        out.append(g)
    return out
def vreman(g,cv=.07):
    aa=(g**2).sum((-1,-2));b=H*H*np.einsum('...mi,...mj->...ij',g,g)
    bb=b[...,0,0]*b[...,1,1]-b[...,0,1]**2+b[...,0,0]*b[...,2,2]-b[...,0,2]**2+b[...,1,1]*b[...,2,2]-b[...,1,2]**2
    return np.where(aa>0,cv*np.sqrt(np.maximum(bb,0)/np.where(aa>0,aa,1)),0.)
def wale(g,cw=.60):
    s=.5*(g+np.swapaxes(g,-1,-2));o=.5*(g-np.swapaxes(g,-1,-2));s2=(s**2).sum((-1,-2));o2=(o**2).sum((-1,-2))
    iv=np.einsum('...ik,...kj,...jl,...li->...',s,s,o,o);sd2=np.maximum(0,(s2*s2+o2*o2)/6+2/3*s2*o2+2*iv)
    den=s2**2.5+sd2**1.25;return np.where(den>0,(cw*H)**2*sd2**1.5/np.where(den>0,den,1),0.)
xy=(np.arange(N)-34)*H;X,Y=np.meshgrid(xy,xy,indexing='xy');core=np.hypot(X,Y)<.10
for p in sys.argv[1:]:
    T,U,V,W=load(p);gs=gradients(U,V,W)
    line=f'{p.split("/")[-2] if "/" in p else p}: core T k0 {T[0][core].mean():7.1f} K  k1 {T[1][core].mean():7.1f} K'
    for k,g in enumerate(gs):
        v=vreman(g)[core].mean();w=wale(g)[core].mean();line+=f' | k{k} nu_t 1e-4: Vreman {v*1e4:5.2f} WALE {w*1e4:5.2f} ({w/v:.2f})'
    print(line)
