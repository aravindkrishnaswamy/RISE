# r271 near field (read-only): does heat release become expansion at the burner? Control volumes V = {r<R, 0<z'<z_top}.
# Outflow Q(t) = sum over the top layer (r<R) of w dA + sum over the ring |r-R|<dx/2 below z_top of u_r (2 pi R dz)/(ring cells
# per layer), from cell-centred velocities (both codes alike). Source S(t) = sum over V of q dV. For FDS also the direct
# integral of its DIVERGENCE slice, D(t), to check the velocity-based Q. Ideal-gas gain R/(p cp) ~ 2.4e-6 m3/J (cp 1200).
# argv[1]: dir with near_planes.cpp output r270q.bin (235 frames, 18 layers, with heat release). FDS r263 slices, same window.
import numpy as np,sys,re
from pathlib import Path
exec(open(Path(__file__).with_name('near_field.py')).read().split('runs={}')[0])
W=Path(sys.argv[1]);V=DX**3
def fds_all():
    sl=[l for l in (RUN/(P+'.fds')).read_text().splitlines() if l.startswith('&SLCF')];idx={}
    for n,l in enumerate(sl,1):
        q=re.search(r"QUANTITY='([^']+)'",l).group(1);m=re.search(r"PBZ=([0-9.eE+-]+)",l)
        if m:idx.setdefault((q,int(round(float(m.group(1))/DX-.5))),n)
    Q=['U-VELOCITY','V-VELOCITY','W-VELOCITY','HRRPUV','DIVERGENCE']
    out=[np.stack([read_slice(RUN/f'{P}_1_{idx[(q,k)]}.sf') for k in range(NZ)],1) for q in Q]
    out[3]=out[3]*1e3;return out
a=np.transpose(np.fromfile(W/'r270q.bin',dtype='<f4').reshape(235,NZ,5,N,N),(0,1,2,4,3)).astype(np.float64)
rise=dict(u=a[:,:,1],v=a[:,:,2],w=a[:,:,3],q=a[:,:,4],div=None);del a
fu,fv,fw,fq,fd=fds_all();fds=dict(u=fu,v=fv,w=fw,q=fq,div=fd)
def series(d,R,ktop):
    ur=(d['u']*X+d['v']*Y)/np.where(RR>0,RR,1.);ring=np.abs(RR-R)<DX/2;disk=RR<R
    side=sum(ur[:,k][:,ring].mean(1) for k in range(ktop))*2*np.pi*R*DX
    top=d['w'][:,ktop][:,disk].sum(1)*DX*DX
    S=sum(d['q'][:,k][:,disk].sum(1) for k in range(ktop))*V
    D=None if d['div'] is None else sum(d['div'][:,k][:,disk].sum(1) for k in range(ktop))*V
    return top+side,S,D
def lagcorr(x,y,L=3):
    x=x-x.mean();y=y-y.mean();return [float(np.dot(x[max(0,-l):len(x)-max(0,l)],y[max(0,l):len(y)-max(0,-l)])/np.sqrt((x**2).sum()*(y**2).sum())) for l in range(-L,L+1)]
print('Control volume r<R, 0<z<z_top. Q outflow (m3/s), S heat release (kW). mean gain Q/S and fluctuation slope dQ/dS (1e-6 m3/J);')
print('corr(Q,S) at lags -3..+3 samples of 0.0625 s (positive lag: Q after S). FDS line 2: same from its DIVERGENCE slice.')
for R in (.15,.20,.30):
    for ktop in (2,4,7,10,16):
        for name,d in (('r270',rise),('FDS r263',fds)):
            Q,S,D=series(d,R,ktop)
            def row(Qx,lab):
                sl=np.polyfit(S-S.mean(),Qx-Qx.mean(),1)[0]
                print(f'  R={R:.2f} z_top={ktop*DX:.2f} {lab:14s} Q={Qx.mean():7.4f} S={S.mean()/1e3:6.2f} gain={1e6*Qx.mean()/S.mean():5.2f} slope={1e6*sl:6.2f} '
                      f'rmsQ/Q={Qx.std()/abs(Qx.mean()):5.3f} rmsS/S={S.std()/S.mean():5.3f} corr='+' '.join(f'{c:5.2f}' for c in lagcorr(S,Qx)))
            row(Q,name)
            if D is not None:row(D,'FDS div slice')
