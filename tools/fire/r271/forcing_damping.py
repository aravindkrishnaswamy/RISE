# r271 near field (read-only): is the plume's weak eddy field under-forced or over-damped? RISE frames vs FDS r263.
# argv[1]: dir with near_planes.cpp outputs (reaction option) r270q.bin (235 frames) and compactq.bin (frames 73-136).
# Forcing: temperature and heat-release fluctuations, and the base heat-release time series.
# Response: velocity fluctuations, their growth with height, and the TKE production they get from that forcing.
# Ring r in [0.06,0.12) m is the flame sheet / burner-edge shear layer (pool radius 0.15 m).
import numpy as np,sys
from pathlib import Path
exec(open(Path(__file__).with_name('near_field.py')).read().split('runs={}')[0])  # constants, grid, FDS slice reader
W=Path(sys.argv[1]);g=9.81;V=DX**3
def fdsq():
    sl=[l for l in (RUN/(P+'.fds')).read_text().splitlines() if l.startswith('&SLCF')];idx={}
    for n,l in enumerate(sl,1):
        q=re.search(r"QUANTITY='([^']+)'",l).group(1);m=re.search(r"PBZ=([0-9.eE+-]+)",l)
        if m:idx.setdefault((q,int(round(float(m.group(1))/DX-.5))),n)
    Q=['TEMPERATURE','U-VELOCITY','V-VELOCITY','W-VELOCITY','HRRPUV']
    out=fds_layers(idx,Q)
    out[0]=out[0]+273.15;out[4]=out[4]*1e3;return out
def riseq(name,frames,sel=slice(None)):
    a=np.transpose(np.fromfile(W/f'{name}.bin',dtype='<f4').reshape(frames,NZ,5,N,N),(0,1,2,4,3))[sel].astype(np.float64)
    return [a[:,:,c] for c in range(5)]
ring=(RR>=.06)&(RR<.12);KS2=(0,1,3,6,9,12,16)
rad=np.arange(0,.21,DX);shells=[np.abs(RR-a)<DX/2 for a in rad]
def band(x,a,b,dt=.0625):
    y=x-x.mean();Y=np.fft.rfft(y);f=np.fft.rfftfreq(len(y),dt);Y[(f<a)|(f>=b)]=0;return np.sqrt(np.mean(np.fft.irfft(Y,len(y))**2))
def analyse(T,u,v,w,q):
    ur=(u*X+v*Y)/np.where(RR>0,RR,1.);ut=(v*X-u*Y)/np.where(RR>0,RR,1.)
    d=lambda a:a-a.mean(0)
    Tp,wp,urp,utp,qp=d(T),d(w),d(ur),d(ut),d(q);Tm,wm=T.mean(0),w.mean(0)
    sd=lambda a:np.sqrt((a**2).mean(0))
    sT,sw,sq,sur=sd(Tp),sd(wp),sd(qp),sd(urp)
    peak=lambda f,k:max(f[k][s].mean() for s in shells)
    res={'peak':{k:(peak(sT,k),peak(sq,k)/1e6,peak(sw,k),peak(sur,k)) for k in KS2}}
    # production on the shear-layer ring: buoyancy g<w'T'>/T, shear -<u_r'w'> dW/dr; TKE; their ratio = removal rate
    dWdr=np.gradient(wm,DX,axis=1)*X/np.where(RR>0,RR,1.)+np.gradient(wm,DX,axis=2)*Y/np.where(RR>0,RR,1.)
    out={}
    for k in KS2:
        R_=lambda a:a[k][ring].mean()
        wT=R_((wp*Tp).mean(0));Pb=g*R_((wp*Tp).mean(0)/Tm);Ps=-R_((urp*wp).mean(0)*dWdr)
        kt=.5*R_(((urp**2)+(utp**2)+(wp**2)).mean(0))
        corr=wT/np.sqrt(R_((wp**2).mean(0))*R_((Tp**2).mean(0)))
        out[k]=(R_(sT),R_(sq)/1e6,R_(sw),corr,Pb,Ps,kt,(Pb+Ps)/kt)
    res['ring']=out
    base=(q[:,:10].sum((2,3))*V).sum(1);m=base.mean()
    res['base']=(m/1e3,100*base.std()/m,100*band(base,2,3.5)/m,100*band(base,3.5,8)/m)
    return res
runs={'r270 full':analyse(*riseq('r270q',235)),'r270 73-136':analyse(*riseq('r270q',235,slice(73,137))),
      'compact':analyse(*riseq('compactq',64)),'FDS r263':analyse(*fdsq())}
names=list(runs)
print('1. Shear-layer peak (max over radius of the ring mean): rms T (K) / rms q (MW/m3) / rms w / rms u_r (m/s)')
print('   z(m)  '+''.join(f'{n:>30s}' for n in names))
for k in KS2:print(f'   {(k+.5)*DX:4.2f}  '+''.join('   {:4.0f} {:5.2f} {:4.2f} {:4.2f}'.format(*runs[n]['peak'][k]).rjust(30) for n in names))
print('\n2. Growth with height of the peak rms w, relative to z=0.05 m')
for k in KS2:print(f'   {(k+.5)*DX:4.2f}  '+''.join(f"{runs[n]['peak'][k][2]/runs[n]['peak'][1][2]:30.2f}" for n in names))
print('\n3. Ring 0.06-0.12 m: rms T (K), rms q (MW/m3), rms w (m/s), corr(w,T), buoyancy prod g<wT>/T and shear prod -<u_r w>dW/dr (m2/s3),')
print('   TKE (m2/s2), and production/TKE (1/s: the rate at which the field must lose energy to stay steady)')
for n in names:
    print(f'   {n}')
    for k in KS2:print(f'     {(k+.5)*DX:4.2f}  '+'  '.join(f'{x:7.3f}' for x in runs[n]['ring'][k]))
print('\n4. Heat release below 0.31 m, plane total: mean kW, rms %, 2-3.5 Hz %, 3.5-8 Hz %')
for n in names:print(f'   {n:12s}  '+'  '.join(f'{x:6.2f}' for x in runs[n]['base']))
# 5. Expansion: velocity divergence from cell-centred velocity (2dx central differences, both codes alike), its rms and
# correlation with heat release q on the ring, and the slope <div'q'>/<q'^2> (1/s per MW/m3). Heat release drives the
# divergence through the pressure constraint; a weak slope means the velocity does not see the expansion.
# The vortical part of the fluctuation is what is left: rms of the horizontal vorticity component w_z (1/s).
def expansion(T,u,v,w,q):
    dv=(np.gradient(u,DX,axis=2)+np.gradient(v,DX,axis=3)+np.gradient(w,DX,axis=1))
    oz=np.gradient(v,DX,axis=2)-np.gradient(u,DX,axis=3)
    d=lambda a:a-a.mean(0);dp,qp,op=d(dv),d(q)/1e6,d(oz);out={}
    for k in (1,2,3,6,9,12,16):
        R_=lambda a:a[k][ring].mean()
        out[k]=(R_(dv.mean(0)),np.sqrt(R_((dp**2).mean(0))),R_((dp*qp).mean(0))/np.sqrt(R_((dp**2).mean(0))*R_((qp**2).mean(0))),
                R_((dp*qp).mean(0))/R_((qp**2).mean(0)),R_(q.mean(0))/1e6,np.sqrt(R_((op**2).mean(0))))
    return out
ex={'r270 full':expansion(*riseq('r270q',235)),'compact':expansion(*riseq('compactq',64)),'FDS r263':expansion(*fdsq())}
print('\n5. Ring 0.06-0.12 m expansion: mean div (1/s), rms div (1/s), corr(div,q), slope d(div)/dq (1/s per MW/m3), mean q (MW/m3), rms vertical vorticity (1/s)')
for n in ex:
    print(f'   {n}')
    for k in ex[n]:print(f'     {(k+.5)*DX:4.2f}  '+'  '.join(f'{x:7.3f}' for x in ex[n][k]))
