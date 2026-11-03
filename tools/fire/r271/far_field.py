# r271 far plume (read-only), like for like: does RISE's turbulence approach FDS's with height (inherited from the flame
# zone) or stay low (local), and is RISE's expansion field noisier at grid scale (a grid-scale source)?
# argv[1]: dir with r270_planes.bin (dump.cpp, 235 frames, layers 9,16,24,32,49,65,81,98, cell-averaged velocity) and
#          r270_div.bin (div_planes.cpp: exact MAC divergence of 44 retained checkpoints, layers 1,3,6,9,16,32,65);
# argv[2] (optional): dir with A/B extracts ab_planes.bin / sc_planes.bin (64 frames, same layers) to add those arms.
# FDS r263 velocities are converted from face values to the same two-face cell averages (near_field.py fds_layers note).
# 1. Per height: centre excess T and w (time means, 3x3 centre), width b_w = sqrt(int w dA/(pi w_c)), centre rms w / w_c,
#    centre rms theta / theta_c, plane w'^2 integral over r<2 b_w normalised by (w_c b_w)^2, turbulent radial heat flux
#    <u_r' theta'> on r = b_w normalised by w_c theta_c, and the horizontal-spectrum shares of w' at k dx < 0.25 pi (large)
#    and >= 0.75 pi (grid).
# 2. Divergence planes: rms of the fluctuation about the time mean over r < 0.3 m, and its grid-scale share.
import numpy as np,sys,os,re
from pathlib import Path
__file__nf=Path(__file__).with_name('near_field.py')
src=open(__file__nf).read().split('runs={}')[0];exec(src.split('W=Path(sys.argv[1])')[0]+src.split('W=Path(sys.argv[1])')[1])
W=Path(sys.argv[1]);ZK=[9,16,24,32,49,65,81,98];DK=[1,3,6,9,16,32,65]
sl=[l for l in (RUN/(P+'.fds')).read_text().splitlines() if l.startswith('&SLCF')];idx={}
for n,l in enumerate(sl,1):
    q=re.search(r"QUANTITY='([^']+)'",l).group(1);m=re.search(r"PBZ=([0-9.eE+-]+)",l)
    if m and 'SPEC_ID' not in l:idx.setdefault((q,int(round(float(m.group(1))/DX-.5))),n)
get=lambda q,k,full=False:read_slice(RUN/f'{P}_1_{idx[(q,k)]}.sf',full)
def fds_plane(k):
    U=get('U-VELOCITY',k,True);V=get('V-VELOCITY',k,True)
    return (get('TEMPERATURE',k)+273.15,.5*(U[:,1:,1:]+U[:,:-1,1:]),.5*(V[:,1:,1:]+V[:,1:,:-1]),.5*(get('W-VELOCITY',k)+get('W-VELOCITY',k-1)))
def rise_planes(path,frames):
    a=np.transpose(np.fromfile(path,dtype='<f4').reshape(frames,8,4,N,N),(0,1,2,4,3)).astype(np.float64)
    return [tuple(a[:,i,c] for c in range(4)) for i in range(8)]
kx=np.fft.fftfreq(N)*2*np.pi;KX,KY=np.meshgrid(kx,kx,indexing='ij');K=np.hypot(KX,KY)
def shares(p):
    p=p-p.mean(0);E=(np.abs(np.fft.fft2(p))**2).mean(0);t=E[K>0].sum();return E[(K>0)&(K<.25*np.pi)].sum()/t,E[K>=.75*np.pi].sum()/t
c3=(np.abs(X)<1.6*DX)&(np.abs(Y)<1.6*DX)
def measures(T,u,v,w):
    th=T-300.;Tm,wm=th.mean(0),w.mean(0);thc,wc=Tm[c3].mean(),wm[c3].mean()
    bw=np.sqrt(np.clip(wm,0,None).sum()*DX*DX/(np.pi*wc))
    wp=w-wm;tp=th-Tm;ur=(u*X+v*Y)/np.where(RR>0,RR,1.);urp=ur-ur.mean(0)
    inside=RR<2*bw;ring=np.abs(RR-bw)<DX/2
    big,grid=shares(w)
    return (thc,wc,bw,np.sqrt((wp[:,c3]**2).mean())/wc,np.sqrt((tp[:,c3]**2).mean())/thc,
            (wp[:,inside]**2).mean(0).sum()*DX*DX/(wc*bw)**2,(urp*tp)[:,ring].mean()/(wc*thc),big,grid)
runs={'r270 full':rise_planes(W/'r270_planes.bin',235)}
runs['r270 73-136']=[tuple(q[73:137] for q in p) for p in runs['r270 full']]
if len(sys.argv)>2:
    A=Path(sys.argv[2])
    for f,n in (('ab','compact'),('sc','compact+scalar')):
        if (A/f'{f}_planes.bin').exists():runs[n]=rise_planes(A/f'{f}_planes.bin',64)
names=list(runs)+['FDS r263']
print('1. Far plume, like for like: theta_c (K), w_c (m/s), b_w (m), rms w/w_c, rms theta/theta_c (centre), int w\'^2/(w_c b_w)^2,')
print('   <u_r\'theta\'>/(w_c theta_c) at r=b_w, w\' spectral share large (k dx<0.25pi) / grid (>=0.75pi)')
for i,k in enumerate(ZK):
    print(f'  z={(k+.5)*DX:4.2f} m')
    rows={n:measures(*runs[n][i]) for n in runs};rows['FDS r263']=measures(*fds_plane(k))
    for n in names:
        r=rows[n];print(f'    {n:15s} {r[0]:6.0f} {r[1]:5.2f} {r[2]:6.3f}   {r[3]:5.3f}  {r[4]:5.3f}   {r[5]:6.3f}   {r[6]:7.4f}    {r[7]:5.3f} / {r[8]:5.3f}')
print('\n2. Divergence fluctuation (about the time mean) over r < 0.3 m: rms (1/s) and grid-scale share (k dx >= 0.75 pi)')
print('   RISE: exact MAC divergence of 44 r270 checkpoints (14.98-18.47 s); FDS: DIVERGENCE slice (235 frames)')
D=np.transpose(np.fromfile(W/'r270_div.bin',dtype='<f4').reshape(-1,7,N,N),(0,1,3,2)).astype(np.float64)
disk=RR<.3
for i,k in enumerate(DK):
    out=[]
    for d in (D[:,i],get('DIVERGENCE',k)):
        dp=d-d.mean(0);out.append((np.sqrt((dp[:,disk]**2).mean()),shares(d)[1],np.sqrt((d.mean(0)[disk]**2).mean())))
    print(f'  z={(k+.5)*DX:4.2f} m   RISE rms {out[0][0]:6.3f}  grid {out[0][1]:5.3f}  (mean-field rms {out[0][2]:6.3f})    FDS rms {out[1][0]:6.3f}  grid {out[1][1]:5.3f}  (mean-field rms {out[1][2]:6.3f})')
