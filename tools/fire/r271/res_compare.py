# r271 section 33: resolution check of the non-reacting plume, RISE against FDS at D*/6, D*/8 and D*/10.
# argv[1]: dir holding RISE plane dumps <arm>_planes.bin (dump_res.cpp: [frame][8 layers][T,u,v,w][y][x]) and
#          <arm>_times.txt; argv[2:]: tier=arm=fdsdir triples, e.g. 6=res_t6_lc=/path/tier6 (arm may be empty: FDS only).
# Each fdsdir holds r271_inert.fds, its slices and grid.json (make_inert_fds_tier.py); a dir without grid.json is the
# original D*/8 run. Env WINDOW="a-b" (s, default 5-10).
# Per height: centre excess T and w (mean over cells within 0.05 m of the axis), width b_w, upward mass flux (RISE
# rho = 352.6/T; FDS its MASS FLUX Z slice), centre rms w/w_c, grid-scale share of w' (k dx >= 0.75 pi); then RISE/FDS
# ratios per tier and the entrainment rate between heights. FDS W is averaged from the two faces (near_field.py note).
import numpy as np,sys,os,re,json,struct
from pathlib import Path
W=Path(sys.argv[1]);LO,HI=(float(v) for v in os.environ.get('WINDOW','5-10').split('-'));P='r271_inert'
def record(f):
    b=f.read(4)
    if not b:return None
    n=struct.unpack('<i',b)[0];d=f.read(n);f.read(4);return d
def read_slice(p,N):
    with p.open('rb') as f:
        for _ in range(4):record(f)
        ts=[];fs=[]
        while True:
            t=record(f)
            if t is None:break
            d=record(f)
            if d is None or len(d)!=4*(N+1)**2:break
            ts.append(struct.unpack('<f',t)[0]);fs.append(np.frombuffer(d,dtype='<f4').reshape((N+1,N+1),order='F')[1:,1:].copy())
    t=np.array(ts);s=(t>=LO)&(t<=HI);return np.array(fs,dtype=np.float64)[s]
def geometry(N,dx):
    xy=(np.arange(N)-(N-1)/2)*dx;X,Y=np.meshgrid(xy,xy,indexing='ij');RR=np.hypot(X,Y)
    kx=np.fft.fftfreq(N)*2*np.pi;KX,KY=np.meshgrid(kx,kx,indexing='ij');return RR,np.hypot(KX,KY)
def measures(T,w,rw,N,dx):
    RR,K=geometry(N,dx);c=RR<.05;th=T-300.;Tm,wm=th.mean(0),w.mean(0);thc,wc=Tm[c].mean(),wm[c].mean()
    bw=np.sqrt(np.clip(wm,0,None).sum()*dx*dx/(np.pi*wc));wp=w-wm
    E=(np.abs(np.fft.fft2(wp))**2).mean(0)
    return thc,wc,bw,np.clip(rw.mean(0),0,None).sum()*dx*dx,np.sqrt((wp[:,c]**2).mean())/wc,E[K>=.75*np.pi].sum()/E[K>0].sum()
out={}
for spec in sys.argv[2:]:
    tier,arm,run=spec.split('=');run=Path(run)
    if (run/'grid.json').exists():g=json.loads((run/'grid.json').read_text());N,dx,ZK=g['n'],g['dx'],g['layers']
    else:N,dx,ZK=69,.030593116067411014,[9,16,24,32,49,65,81,98]
    idx={}
    for n,l in enumerate([l for l in (run/(P+'.fds')).read_text().splitlines() if l.startswith('&SLCF')],1):
        q=re.search(r"QUANTITY='([^']+)'",l).group(1);m=re.search(r"PBZ=([0-9.eE+-]+)",l)
        if m and 'SPEC_ID' not in l:idx.setdefault((q,int(round(float(m.group(1))/dx-.5))),n)
    get=lambda q,k:read_slice(run/f'{P}_1_{idx[(q,k)]}.sf',N)
    z=[(k+.5)*dx for k in ZK]
    fds=[measures(get('TEMPERATURE',k)+273.15,.5*(get('W-VELOCITY',k)+get('W-VELOCITY',k-1)),get('MASS FLUX Z',k),N,dx) for k in ZK]
    rise=None
    if arm:
        t=np.loadtxt(W/f'{arm}_times.txt');n=len(t)
        a=np.transpose(np.fromfile(W/f'{arm}_planes.bin',dtype='<f4').reshape(n,8,4,N,N),(0,1,2,4,3)).astype(np.float64)[(t>=LO)&(t<=HI)]
        rise=[measures(a[:,i,0],a[:,i,3],352.6*a[:,i,3]/a[:,i,0],N,dx) for i in range(8)]
    out[tier]=(dx,N,z,fds,rise)
print(f'Non-reacting plume resolution check, window {LO:g}-{HI:g} s. Columns: theta_c (K), w_c (m/s), b_w (m), mass flux (kg/s),')
print("centre rms w/w_c, grid-scale share of w'")
for tier,(dx,N,z,fds,rise) in out.items():
    print(f'\nD*/{tier}: dx={dx:.4f} m, {N}x{N}')
    for i,h in enumerate(z):
        for name,r in (('FDS ',fds),('RISE',rise)):
            if r:print(f'  z={h:4.2f} {name} {r[i][0]:6.0f} {r[i][1]:5.2f} {r[i][2]:6.3f} {r[i][3]:6.3f}  {r[i][4]:5.3f}  {r[i][5]:5.3f}')
print('\nRISE/FDS ratios per tier (theta_c, w_c, mass flux, rms w/w_c) by height index')
for tier,(dx,N,z,fds,rise) in out.items():
    if not rise:continue
    print(f'  D*/{tier}')
    for i,h in enumerate(z):print(f'    z={h:4.2f}  '+'  '.join(f'{rise[i][j]/fds[i][j]:5.2f}' for j in (0,1,3,4)))
print('\nEntrainment rate dm/dz (kg/s/m) between heights: FDS, RISE, ratio')
for tier,(dx,N,z,fds,rise) in out.items():
    print(f'  D*/{tier}')
    for i in range(7):
        f=(fds[i+1][3]-fds[i][3])/(z[i+1]-z[i]);s=f'    {z[i]:4.2f}-{z[i+1]:4.2f}  {f:6.3f}'
        if rise:r=(rise[i+1][3]-rise[i][3])/(z[i+1]-z[i]);s+=f'  {r:6.3f}  {r/f:5.2f}'
        print(s)
print('\nFDS against itself across tiers (mass flux, theta_c, rms w/w_c at each height index)')
for i in range(8):print('  '+'   '.join(f'D*/{t}: z={o[2][i]:4.2f} {o[3][i][3]:6.3f} {o[3][i][0]:5.0f} {o[3][i][4]:5.3f}' for t,o in out.items()))
if all(o[4] for o in out.values()):
    print('\nRISE against itself across tiers (mass flux, theta_c, rms w/w_c at each height index)')
    for i in range(8):print('  '+'   '.join(f'D*/{t}: z={o[2][i]:4.2f} {o[4][i][3]:6.3f} {o[4][i][0]:5.0f} {o[4][i][4]:5.3f}' for t,o in out.items()))
