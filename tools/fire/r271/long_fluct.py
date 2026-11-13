# r271 section 36: fluctuation levels and plume-only mass flux of a reacting RISE run against FDS r263.
# Run from the scratch dir holding fds_mean3d.npy ([U,V,W,T,rho,p,rhoU,rhoV,rhoW][z][x][y]).
# argv[1]: dir; then arm=frames pairs, each with <arm>_planes.bin (dump.cpp) and <arm>_mean3d.bin (mean3d.cpp).
# Env FDS_RUN = the r263 FDS run dir (slices), window 10.47-25.09 s.
# Per height: centre rms w/w_c (r < 0.05 m), the same in the ring 0.1-0.2 m, grid-scale share of w' (k dx >= 0.75 pi),
# centre rms T / excess T, resolved turbulent / mean-field vertical heat flux; then mass flux inside the plume
# (mean w > 0.1 w_c), the flux outside it, and the velocity width b_w.
import re,struct,sys,os,numpy as np
from pathlib import Path
DX=.030593116067411014;A=DX*DX;START=10.466040127212182;END=25.091040127212182;N=69;NZ=106;ZK=[9,16,24,32,49,65,81,98]
RUN=Path(os.environ.get('FDS_RUN','/Volumes/SSD/RISE/fire_production_calibration/r263_fds_no_slip/fds_run'));W=sys.argv[1]
def record(f):
    b=f.read(4)
    if not b:return None
    n=struct.unpack('<i',b)[0];d=f.read(n);f.read(4);return d
def read_slice(p):
    with p.open('rb') as f:
        for _ in range(4):record(f)
        ts=[];fs=[]
        while True:
            t=record(f)
            if t is None:break
            ts.append(struct.unpack('<f',t)[0]);fs.append(np.frombuffer(record(f),dtype='<f4').reshape((70,70),order='F')[1:,1:].copy())
    t=np.array(ts);return np.array(fs,dtype=np.float64)[(t>=START)&(t<=END)]
idx={}
for n,l in enumerate([l for l in (RUN/'r263_fds.fds').read_text().splitlines() if l.startswith('&SLCF')],1):
    q=re.search(r"QUANTITY='([^']+)'",l).group(1);m=re.search(r"PBZ=([0-9.eE+-]+)",l)
    if m:idx.setdefault((q,int(round(float(m.group(1))/DX-.5))),n)
get=lambda q,k:read_slice(RUN/f'r263_fds_1_{idx[(q,k)]}.sf')
xy=(np.arange(N)-(N-1)/2)*DX;X,Y=np.meshgrid(xy,xy,indexing='ij');R=np.hypot(X,Y);c=R<.05;ring=(R>.1)&(R<.2)
kx=np.fft.fftfreq(N)*2*np.pi;KX,KY=np.meshgrid(kx,kx,indexing='ij');K=np.hypot(KX,KY)
def fluct(w,T):
    wm=w.mean(0);Tm=T.mean(0);wc=wm[c].mean();wp=w-wm;Tp=T-Tm;E=(np.abs(np.fft.fft2(wp))**2).mean(0)
    return (np.sqrt((wp[:,c]**2).mean())/wc,np.sqrt((wp[:,ring]**2).mean())/wc,E[K>=.75*np.pi].sum()/E[K>0].sum(),
            np.sqrt((Tp[:,c]**2).mean())/(Tm[c].mean()-300),(wp*Tp).mean(0).sum()/(wm*(Tm-300)).sum())
def plume(w,rw):
    wc=w[33:36,33:36].mean();m=w>.1*wc;return rw[m].sum()*A,np.clip(rw[~m],0,None).sum()*A,np.sqrt(np.clip(w,0,None)[m].sum()*A/(np.pi*wc))
F=np.load('fds_mean3d.npy')
ff=[fluct(.5*(get('W-VELOCITY',k)+get('W-VELOCITY',k-1)),get('TEMPERATURE',k)+273.15) for k in ZK];fp=[plume(F[2][k],F[8][k]) for k in ZK]
for spec in sys.argv[2:]:
    arm,nf=spec.split('=');nf=int(nf)
    P=np.transpose(np.fromfile(f'{W}/{arm}_planes.bin',dtype='<f4').reshape(nf,8,4,N,N),(0,1,2,4,3)).astype(np.float64)
    Q=np.fromfile(f'{W}/{arm}_mean3d.bin').reshape(7,NZ,N,N).transpose(0,1,3,2)
    print(f"\n{arm} ({nf} frames). z | FDS: rms w/w_c centre, ring, grid share, rms T/theta_c, turbulent/mean heat flux | RISE | RISE/FDS")
    for i,k in enumerate(ZK):
        r=fluct(P[:,i,3],P[:,i,0]);print(f'  {(k+.5)*DX:5.2f} | '+' '.join(f'{v:6.3f}' for v in ff[i])+' | '+' '.join(f'{v:6.3f}' for v in r)+' | '+' '.join(f'{y/x:5.2f}' for x,y in zip(ff[i],r)))
    print('  z | FDS: plume mass flux (kg/s), upward flux outside, b_w (m) | RISE | RISE/FDS')
    for i,k in enumerate(ZK):
        r=plume(Q[3][k],352.6*Q[6][k]);print(f'  {(k+.5)*DX:5.2f} | '+' '.join(f'{v:6.4f}' for v in fp[i])+' | '+' '.join(f'{v:6.4f}' for v in r)+' | '+' '.join(f'{y/x:5.2f}' for x,y in zip(fp[i],r)))
