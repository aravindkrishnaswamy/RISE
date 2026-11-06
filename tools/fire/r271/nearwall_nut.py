# r271 section 26: FDS's near-wall eddy viscosity. FDS (velo.f90 WALL_LOOP, NEAR_WALL_TURBULENCE_MODEL default WALE)
# replaces mu in every cell adjacent to a solid wall (the floor, burner vents included) by mu_DNS + rho nu_WALE; RISE uses
# Vreman there. From an FDS run's slices: FDS's own nu_t = (VISCOSITY - MOLECULAR VISCOSITY)/DENSITY per layer, against
# Vreman recomputed offline on the same velocity field with FDS's stencil (velo.f90 VREMAN; no-slip floor ghost u,v = -u,v,
# floor w = 0). k >= 1 validates the offline Vreman; k = 0 shows the WALE replacement. argv: run dir, chid, frame stride.
import numpy as np,re,struct,sys
from pathlib import Path
N=69;H=.030593116067411014;run=Path(sys.argv[1]);chid=sys.argv[2];stride=int(sys.argv[3]);CV=0.07
sl=[l for l in (run/f'{chid}.fds').read_text().splitlines() if l.startswith('&SLCF')];idx={}
for n,l in enumerate(sl,1):
    q=re.search(r"QUANTITY='([^']+)'",l).group(1);m=re.search(r"PBZ=([0-9.eE+-]+)",l)
    if m and 'SPEC_ID' not in l:idx.setdefault((q,int(round(float(m.group(1))/H-.5))),n)
def frames(q,k):
    out=[]
    with (run/f'{chid}_1_{idx[(q,k)]}.sf').open('rb') as f:
        for _ in range(4):n=struct.unpack('<i',f.read(4))[0];f.seek(n+4,1)
        i=0
        while True:
            h=f.read(4)
            if len(h)<4:break
            f.read(8);n=struct.unpack('<i',f.read(4))[0];d=f.read(n);f.read(4)
            if len(d)<n:break
            if i%stride==0:out.append(np.frombuffer(d,'<f4').reshape((70,70),order='F').astype(float))
            i+=1
    return np.array(out)
K=4;U={k:frames('U-VELOCITY',k) for k in range(K)};V={k:frames('V-VELOCITY',k) for k in range(K)};W={k:frames('W-VELOCITY',k) for k in range(K)}
nf=min(len(U[0]),*(len(x) for x in W.values()))
xy=(np.arange(N)-34)*H;X,Y=np.meshgrid(xy,xy,indexing='ij');core=np.hypot(X,Y)<.10
# FDS slice arrays a[i][j], i,j = 0..69: U face i of cell row j is a[i][j]; cells are i,j = 1..69. Pad layers: k=-1 ghost.
def lay(D,k):
    if k<0:return -D[0][:nf] if D is not W else np.zeros_like(W[0][:nf])
    return D[k][:nf]
print(f'{nf} frames; core r<0.10 m; nu_t in 1e-4 m2/s: FDS own (mu - mu_mol)/rho | offline Vreman, FDS stencil')
for k in range(K-1):
    mu=frames('VISCOSITY',k)[:nf];mm=frames('MOLECULAR VISCOSITY',k)[:nf];rho=frames('DENSITY',k)[:nf]
    own=((mu-mm)/rho)[:,1:,1:]
    u0,u1,um=lay(U,k),lay(U,k+1),lay(U,k-1);v0,v1,vm=lay(V,k),lay(V,k+1),lay(V,k-1);w0,wm=lay(W,k),lay(W,k-1)
    I=slice(1,69);Ip=slice(2,70);Im=slice(0,68)  # interior cells 1..68 (skip the last edge cell)
    def A(i,j):return i,j
    dudx=(u0[:,I,I]-u0[:,Im,I])/H;dvdy=(v0[:,I,I]-v0[:,I,Im])/H;dwdz=(w0[:,I,I]-wm[:,I,I])/H
    dudy=.25/H*(u0[:,I,Ip]-u0[:,I,Im]+u0[:,Im,Ip]-u0[:,Im,Im]);dudz=.25/H*(u1[:,I,I]-um[:,I,I]+u1[:,Im,I]-um[:,Im,I])
    dvdx=.25/H*(v0[:,Ip,I]-v0[:,Im,I]+v0[:,Ip,Im]-v0[:,Im,Im]);dvdz=.25/H*(v1[:,I,I]-vm[:,I,I]+v1[:,I,Im]-vm[:,I,Im])
    w1=lay(W,k);wmm=lay(W,k-1)
    dwdx=.25/H*(w1[:,Ip,I]-w1[:,Im,I]+wmm[:,Ip,I]-wmm[:,Im,I]);dwdy=.25/H*(w1[:,I,Ip]-w1[:,I,Im]+wmm[:,I,Ip]-wmm[:,I,Im])
    a=np.stack([np.stack([dudx,dvdx,dwdx],-1),np.stack([dudy,dvdy,dwdy],-1),np.stack([dudz,dvdz,dwdz],-1)],-2)  # a[...,i,j]=A_IJ(i,j)
    AA=(a**2).sum((-1,-2));Bm=H*H*np.einsum('...mi,...mj->...ij',a,a)
    BB=Bm[...,0,0]*Bm[...,1,1]-Bm[...,0,1]**2+Bm[...,0,0]*Bm[...,2,2]-Bm[...,0,2]**2+Bm[...,1,1]*Bm[...,2,2]-Bm[...,1,2]**2
    nuv=np.where((AA>1e-30)&(BB>1e-30),CV*np.sqrt(np.maximum(BB,0)/np.where(AA>0,AA,1)),0.)
    c=core[0:68,0:68]
    print(f'  k{k}: FDS own {own[:,0:68,0:68][:,c].mean()*1e4:6.3f}   offline Vreman {nuv[:,c].mean()*1e4:6.3f}   ratio own/Vreman {own[:,0:68,0:68][:,c].mean()/nuv[:,c].mean():.2f}')
