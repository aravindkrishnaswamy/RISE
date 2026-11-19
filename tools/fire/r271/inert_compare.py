# r271 non-reacting plume pair (section 19), like for like: RISE frames (cell-averaged velocity) against FDS r271_inert
# slices converted to the same two-face cell averages (near_field.py fds_layers note). Heights: dump.cpp's 8 layers.
# argv[1]: dir holding RISE plane dumps <arm>_planes.bin (dump.cpp, frames from origin) with <arm>_times.txt (one time per
#          frame); argv[2:]: arm names (may be none: FDS only). Env WINDOWS="a-b,c-d" (s) selects the time windows.
# Per height and window: centre excess T and w, width b_w, upward mass flux (RISE rho = 352.6/T as ab_compare.py; FDS its
# MASS FLUX Z slice), centre rms w/w_c, int w'^2 dA/(w_c b_w)^2, <u_r'theta'>/(w_c theta_c) at r=b_w, and the horizontal
# spectral shares of w' (large k dx<0.25 pi / grid >=0.75 pi); then the entrainment rate dm/dz between heights.
import numpy as np,sys,os,re
from pathlib import Path
src=open(Path(__file__).with_name('near_field.py')).read().split('runs={}')[0]
exec(src.split('W=Path(sys.argv[1])')[0]+src.split('W=Path(sys.argv[1])')[1])
RUN=Path('/Volumes/SSD/RISE/fire_production_calibration/r271_inert_fds/fds_run');P='r271_inert'
W=Path(sys.argv[1]);ARMS=sys.argv[2:];ZK=[9,16,24,32,49,65,81,98]
WIN=[tuple(float(v) for v in w.split('-')) for w in os.environ.get('WINDOWS','6-14,10.47-25.09').split(',')]
sl=[l for l in (RUN/(P+'.fds')).read_text().splitlines() if l.startswith('&SLCF')];idx={}
for n,l in enumerate(sl,1):
    q=re.search(r"QUANTITY='([^']+)'",l).group(1);m=re.search(r"PBZ=([0-9.eE+-]+)",l)
    if m and 'SPEC_ID' not in l:idx.setdefault((q,int(round(float(m.group(1))/DX-.5))),n)
def fds_times():
    with (RUN/f'{P}_1_{idx[("TEMPERATURE",0)]}.sf').open('rb') as f:
        for _ in range(4):record(f)
        ts=[]
        while True:
            t=record(f)
            if t is None:break
            ts.append(struct.unpack('<f',t)[0]);record(f)
    return np.array(ts)
START,END=0.,1e9;tF=fds_times()
get=lambda q,k,full=False:read_slice(RUN/f'{P}_1_{idx[(q,k)]}.sf',full)
def fds_plane(k):
    U=get('U-VELOCITY',k,True);V=get('V-VELOCITY',k,True)
    return (get('TEMPERATURE',k)+273.15,.5*(U[:,1:,1:]+U[:,:-1,1:]),.5*(V[:,1:,1:]+V[:,1:,:-1]),
            .5*(get('W-VELOCITY',k)+get('W-VELOCITY',k-1)),get('MASS FLUX Z',k))
kx=np.fft.fftfreq(N)*2*np.pi;KX,KY=np.meshgrid(kx,kx,indexing='ij');K=np.hypot(KX,KY)
def shares(p):
    p=p-p.mean(0);E=(np.abs(np.fft.fft2(p))**2).mean(0);t=E[K>0].sum();return E[(K>0)&(K<.25*np.pi)].sum()/t,E[K>=.75*np.pi].sum()/t
c3=(np.abs(X)<1.6*DX)&(np.abs(Y)<1.6*DX)
def measures(T,u,v,w,rw):
    th=T-300.;Tm,wm=th.mean(0),w.mean(0);thc,wc=Tm[c3].mean(),wm[c3].mean()
    bw=np.sqrt(np.clip(wm,0,None).sum()*DX*DX/(np.pi*wc))
    wp=w-wm;tp=th-Tm;ur=(u*X+v*Y)/np.where(RR>0,RR,1.);urp=ur-ur.mean(0)
    inside=RR<2*bw;ring=np.abs(RR-bw)<DX/2;big,grid=shares(w)
    return (thc,wc,bw,np.clip(rw.mean(0),0,None).sum()*DX*DX,np.sqrt((wp[:,c3]**2).mean())/wc,
            (wp[:,inside]**2).mean(0).sum()*DX*DX/(wc*bw)**2,(urp*tp)[:,ring].mean()/(wc*thc),big,grid)
rows={}
FD=[fds_plane(k) for k in ZK]
for a,b in WIN:
    s=(tF>=a)&(tF<=b)
    if s.sum()>8:rows[(f'FDS {a:g}-{b:g}',)]=[measures(*(q[s] for q in p)) for p in FD]
for arm in ARMS:
    t=np.loadtxt(W/f'{arm}_times.txt');n=len(t)
    a=np.transpose(np.fromfile(W/f'{arm}_planes.bin',dtype='<f4').reshape(n,8,4,N,N),(0,1,2,4,3)).astype(np.float64)
    for lo,hi in WIN:
        s=(t>=lo)&(t<=hi)
        if s.sum()>8:rows[(f'{arm} {lo:g}-{hi:g}',)]=[measures(a[s,i,0],a[s,i,1],a[s,i,2],a[s,i,3],352.6*a[s,i,3]/a[s,i,0]) for i in range(8)]
names=[k[0] for k in rows]
print('Non-reacting plume, like for like. Per height: theta_c (K), w_c (m/s), b_w (m), upward mass flux (kg/s), centre rms w/w_c,')
print("int w'^2/(w_c b_w)^2, <u_r'theta'>/(w_c theta_c) at r=b_w, w' large / grid spectral share")
for i,k in enumerate(ZK):
    print(f'  z={(k+.5)*DX:4.2f} m')
    for n in names:
        r=rows[(n,)][i];print(f'    {n:24s} {r[0]:6.0f} {r[1]:5.2f} {r[2]:6.3f}  {r[3]:6.3f}   {r[4]:5.3f}  {r[5]:6.3f}  {r[6]:7.4f}   {r[7]:5.3f} / {r[8]:5.3f}')
print('\nEntrainment rate dm/dz (kg/s/m) between heights')
z=[(k+.5)*DX for k in ZK]
print('   interval     '+''.join(f'{n:>26s}' for n in names))
for i in range(7):
    print(f'   {z[i]:4.2f}-{z[i+1]:4.2f}  '+''.join(f"{(rows[(n,)][i+1][3]-rows[(n,)][i][3])/(z[i+1]-z[i]):26.3f}" for n in names))
