#!/usr/bin/env python3
"""r271 section 29: growth of a small divergence-free perturbation (gr_perturb.py) in each code from the same base state.
delta = perturbed run minus control run of the same code, on cell-centred T, u, v, w (layers 0..65; velocities as two-face
cell averages in both codes). Reported per height band, normalised by the injected perturbation's value at t = 0 summed
over all layers (computed from the state files), so the bands add up to 1.0 at t = 0 for a column that does not grow:
  E      kinetic energy of delta over r < 0.25 m;
  w      energy of delta-w over r < 0.25 m;
  m0..m4 energy of delta-w in azimuthal mode m about the plume axis (per 1-cell radial ring, r < 0.25 m);
  L/M/G  energy of delta-w by horizontal scale over the whole plane: >= 8 dx (K < pi/4), 2.7-8 dx, < 2.7 dx (K >= 0.75 pi);
  T      delta-T variance over r < 0.25 m, relative to delta-w's injected energy (K^2 per m^2/s^2; T is not perturbed).
argv: base state, then label=ctrl,pert,state,scale ... where ctrl/pert are a RISE ckpt_layers 66 dump (.bin) or an FDS run
dir (xp.fds, slices from make_fds_xp.py + DT_SLCF 0.025), state is the perturbed state file and scale divides delta.
Frames are paired by nearest time (within 2.5 ms; offsets are reported), so ctrl/pert may also be two codes' controls.
Optional env GR_BANDS='0-3,4-9,...' (layer ranges), GR_FIT='a:b' (s) for the per-band growth rate 0.5 d ln E/dt,
GR_RATIO='armA/armB,...': armA over armB per column at armA's times (armB log-interpolated in time)."""
import numpy as np,sys,re,struct,os
from pathlib import Path
DX=.030593116067411014;N=69;NZ=106;NL=66
BANDS=[tuple(int(x) for x in b.split('-')) for b in os.environ.get('GR_BANDS','0-3,4-9,10-19,20-34,35-65').split(',')]
FIT=[float(x) for x in os.environ.get('GR_FIT','0.05:0.30').split(':')]
xy=(np.arange(N)-34)*DX;X,Y=np.meshgrid(xy,xy,indexing='ij');R=np.hypot(X,Y);TH=np.arctan2(Y,X);DISK=R<.25
RING=np.minimum((R/DX+.5).astype(int),99);RINGS=[RING==i for i in range(int(.25/DX)+1)]
kx=np.fft.fftfreq(N)*2*np.pi;KX,KY=np.meshgrid(kx,kx,indexing='ij');K=np.hypot(KX,KY)
SC=[(K<.25*np.pi),(K>=.25*np.pi)&(K<.75*np.pi),(K>=.75*np.pi)]
def state(p):  # -> T,u,v,w [z][x][y] cell-centred, layers 0..NL-1
    a=np.fromfile(p,dtype='<f8');nc=N*N*NZ;nu=(N+1)*N*NZ;nw=N*N*(NZ+1)
    T=a[:nc].reshape(NZ,N,N);U=a[nc:nc+nu].reshape(NZ,N,N+1);V=a[nc+nu:nc+2*nu].reshape(NZ,N+1,N);W=a[nc+2*nu:nc+2*nu+nw].reshape(NZ+1,N,N)
    f=[T,.5*(U[:,:,1:]+U[:,:,:-1]),.5*(V[:,1:]+V[:,:-1]),.5*(W[1:]+W[:-1])]
    return np.stack([np.transpose(x[:NL],(0,2,1)) for x in f],1)  # [z][c][x][y]
def rise(p):
    t=np.loadtxt(str(p)+'.times',ndmin=1);a=np.fromfile(p,dtype='<f4').reshape(len(t),NL,4,N,N)
    return t,np.transpose(a,(0,1,2,4,3)).astype(np.float64)
def fds(run):
    sl=[l for l in (run/'xp.fds').read_text().splitlines() if l.startswith('&SLCF')];idx={}
    for n,l in enumerate(sl,1):
        q=re.search(r"QUANTITY='([^']+)'",l).group(1);m=re.search(r"PBZ=([0-9.eE+-]+)",l)
        if m and 'SPEC_ID' not in l:idx.setdefault((q,int(round(float(m.group(1))/DX-.5))),n)
    def frames(q,k):
        out=[];ts=[]
        with (run/f'xp_1_{idx[(q,k)]}.sf').open('rb') as f:
            for _ in range(4):n=struct.unpack('<i',f.read(4))[0];f.seek(n+4,1)
            while True:
                h=f.read(4)
                if len(h)<4:break
                tt=struct.unpack('<f',f.read(4))[0];f.read(4);hn=f.read(4)
                if len(hn)<4:break
                n=struct.unpack('<i',hn)[0];d=f.read(n)
                if len(d)<n:break
                out.append(np.frombuffer(d,dtype='<f4').reshape((70,70),order='F').astype(np.float64));f.read(4);ts.append(tt)
        return np.array(ts),np.array(out)
    t=frames('TEMPERATURE',0)[0];m=len(t);A=np.zeros((m,NL,4,N,N));wprev=0.
    for k in range(NL):
        A[:,k,0]=frames('TEMPERATURE',k)[1][:m,1:,1:]+273.15  # FDS slices are deg C
        u=frames('U-VELOCITY',k)[1][:m];A[:,k,1]=.5*(u[:,:-1,1:]+u[:,1:,1:])
        v=frames('V-VELOCITY',k)[1][:m];A[:,k,2]=.5*(v[:,1:,:-1]+v[:,1:,1:])
        w=frames('W-VELOCITY',k)[1][:m,1:,1:];A[:,k,3]=.5*(w+wprev);wprev=w
    return t,A
def load(p):return rise(p) if p.endswith('.bin') else fds(Path(p))
def diag(d,bands=None):  # d [z][c][x][y] -> per band: E, m0..m4, m5+, L, M, G, T
    out=[]
    for a,b in bands or BANDS:
        s=d[a:b+1];w=s[:,3];E=(s[:,1:]**2).sum(1)[:,DISK].sum()
        em=np.zeros(6);tot=(w[:,DISK]**2).sum()
        for rg in RINGS:
            if not rg.any():continue
            for m in range(5):
                c=(w[:,rg]*np.exp(-1j*m*TH[rg])).sum(1);em[m]+=((abs(c)**2)*(1 if m==0 else 2)/rg.sum()).sum()
        em[5]=tot
        F=np.abs(np.fft.fft2(w,axes=(1,2)))**2/(N*N);sc=[F[:,q].sum() for q in SC]
        out.append(np.r_[E,em,sc,(s[:,0][:,DISK]**2).sum()])
    return np.array(out)
base=state(sys.argv[1]);arms=[]
for arg in sys.argv[2:]:
    lab,rest=arg.split('=',1);cp,pp,sp,scale=rest.split(',');scale=float(scale)
    d0=diag((state(sp)-base)/scale,[(0,NL-1)]);d0[:,10]=d0[:,6]  # whole-column injected values; T by injected w energy
    tc,C=load(cp);tp,P=load(pp);ip=[int(np.argmin(abs(tp-x))) for x in tc];ok=[i for i in range(len(tc)) if abs(tp[ip[i]]-tc[i])<2.5e-3]
    dt=max(abs(tp[ip[i]]-tc[i]) for i in ok)
    if dt>1e-6:print(f'# {lab}: control/perturbed matched to the nearest frame, max time offset {dt:.2e} s')
    D=np.array([diag((P[ip[i]]-C[i])/scale) for i in ok])/d0[None]
    arms.append((lab,tc[ok],D))
cols=['E','m0','m1','m2','m3','m4','w','L','M','G','T']
for bi,(a,b) in enumerate(BANDS):
    z0,z1=a*DX,(b+1)*DX;print(f'\nlayers {a}-{b} (z {z0:.2f}-{z1:.2f} m): delta energy / injected (T: K^2 per injected w energy)')
    print(f'  {"arm":14s} {"t s":>6s} '+''.join(f'{c:>8s}' for c in cols))
    for lab,t,D in arms:
        for i in range(len(t)):print(f'  {lab:14s} {t[i]:6.3f} '+''.join(f'{x:8.3g}' for x in D[i,bi]))
print(f'\ngrowth rate 0.5 d ln(.)/dt (1/s), least squares over {FIT[0]}-{FIT[1]} s')
print(f'  {"band":8s} {"arm":14s} '+''.join(f'{c:>8s}' for c in cols))
for bi,(a,b) in enumerate(BANDS):
    for lab,t,D in arms:
        s=(t>=FIT[0]-1e-6)&(t<=FIT[1]+1e-6)
        g=[.5*np.polyfit(t[s],np.log(np.maximum(D[s,bi,j],1e-30)),1)[0] if s.sum()>1 else np.nan for j in range(len(cols))]
        print(f'  {a:2d}-{b:<5d} {lab:14s} '+''.join(f'{x:8.2f}' for x in g))
rat=[r.split('/') for r in os.environ.get('GR_RATIO','').split(',') if r]
if rat:
    A={l:(t,D) for l,t,D in arms};print('\nratio armA/armB at armA times (armB log-interpolated)')
    for bi,(a,b) in enumerate(BANDS):
        print(f'layers {a}-{b}:  {"pair":14s} {"t s":>6s} '+''.join(f'{c:>8s}' for c in cols))
        for x,y in rat:
            (ta,Da),(tb,Db)=A[x],A[y]
            for i in range(len(ta)):
                if ta[i]>tb[-1]+1e-6:continue
                r=[Da[i,bi,j]/np.exp(np.interp(ta[i],tb,np.log(np.maximum(Db[:,bi,j],1e-30)))) for j in range(len(cols))]
                print(f'           {x+"/"+y:14s} {ta[i]:6.3f} '+''.join(f'{v:8.3g}' for v in r))
