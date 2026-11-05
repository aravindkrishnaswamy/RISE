# r271 non-reacting plume (section 23, read-only): does the base feel the plume's unsteadiness? 6-14 s, frame cadence.
# Signals (azimuthal means, so the axisymmetric puffing mode):
#   ur0, ur1   radial velocity on the floor annulus just outside the burner (R_b+dx/2 < r < R_b+5dx/2), layers 0 and 1
#   wE0        w on the burner-edge ring |r-R_b|<dx/2, layer 0
#   wc50, wc99 centre (3x3) w at 0.50 m and 0.99 m
#   p0, p99    FDS only: centre perturbation pressure at layer 0 and at 0.99 m (RISE stores no pressure)
# Per signal: rms. Per pair, in the 2-4 Hz puffing band (Welch, 2 s Hann segments, 50% overlap, 7 segments; coherence
# noise floor about 0.15): magnitude-squared coherence and the gain |S_xy|/S_yy, the base response per unit plume signal.
# argv[1]: dir with <arm>_near.bin (near_planes.cpp, frames 64-192) and <arm>_planes.bin/<arm>_times.txt; argv[2:]: arms.
import numpy as np,sys,os
from pathlib import Path
os.environ['R271_FDS_CELL_AVERAGE']='1'
exec(open(Path(__file__).with_name('inert_shear.py')).read().split('rad=np.arange')[0])
Rb=np.sqrt(69*DX*DX/np.pi);ann=(RR>Rb+DX/2)&(RR<Rb+2.5*DX);edge=np.abs(RR-Rb)<DX/2;c3=(np.abs(X)<1.6*DX)&(np.abs(Y)<1.6*DX)
ER=np.where(RR>0,1/np.where(RR>0,RR,1),0)
def ur(u,v):return (u*X+v*Y)*ER
def welch(x,y,dt=0.0625,L=32):
    x=x-x.mean();y=y-y.mean();h=np.hanning(L);S=[]
    for i in range(0,len(x)-L+1,L//2):
        a=np.fft.rfft(x[i:i+L]*h);b=np.fft.rfft(y[i:i+L]*h);S.append((a*a.conj(),b*b.conj(),a*b.conj()))
    S=np.array(S).mean(0);f=np.fft.rfftfreq(L,dt);m=(f>=2)&(f<=4)
    Sxx,Syy,Sxy=S[0].real[m].sum(),S[1].real[m].sum(),S[2][m].sum()
    return abs(Sxy)**2/(Sxx*Syy),abs(Sxy)/Syy
def report(name,sig):
    print(f'  {name}');print('    rms: '+'  '.join(f'{k} {np.std(v):.4f}' for k,v in sig.items()))
    for a in ('ur0','ur1','wE0','p0'):
        for b in ('wc50','wc99','p99'):
            if a in sig and b in sig and not (a=='p0' and b=='p99' and False):
                c,g=welch(sig[a],sig[b]);print(f'    {a:4s} vs {b:4s}: coherence {c:4.2f}  gain {g:7.4f}')
for arm in ARMS:
    a=np.transpose(np.fromfile(W/f'{arm}_near.bin',dtype='<f4').reshape(-1,NZ,4,N,N),(0,1,2,4,3)).astype(np.float64)
    t=np.loadtxt(W/f'{arm}_times.txt');s=(t>=START-1e-6)&(t<=END+1e-6)
    pl=np.fromfile(W/f'{arm}_planes.bin',dtype='<f4').reshape(len(t),8,4,N,N)[s][:,3,2].transpose(0,2,1)
    assert len(pl)==len(a)
    sig={'ur0':ur(a[:,0,1],a[:,0,2])[:,ann].mean(1),'ur1':ur(a[:,1,1],a[:,1,2])[:,ann].mean(1),
         'wE0':a[:,0,3][:,edge].mean(1),'wc50':a[:,16,3][:,c3].mean(1),'wc99':pl[:,c3].mean(1)};del a
    report(arm,sig)
T,u,v,w=fds_layers(idx,['TEMPERATURE','U-VELOCITY','V-VELOCITY','W-VELOCITY'])
g=lambda q,k:read_slice(RUN/f'{P}_1_{idx[(q,k)]}.sf')
w99=.5*(g('W-VELOCITY',32)+g('W-VELOCITY',31));p0=g('PRESSURE',0)[:,c3].mean(1);p99=g('PRESSURE',32)[:,c3].mean(1)
sig={'ur0':ur(u[:,0],v[:,0])[:,ann].mean(1),'ur1':ur(u[:,1],v[:,1])[:,ann].mean(1),'wE0':w[:,0][:,edge].mean(1),
     'wc50':w[:,16][:,c3].mean(1),'wc99':w99[:,c3].mean(1),'p0':p0,'p99':p99}
report('FDS r271_inert',sig)
print(f'  mean ur0 (m/s, negative = inflow): FDS {sig["ur0"].mean():.4f}')
