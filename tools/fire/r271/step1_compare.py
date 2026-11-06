# r271 section 26: one-step operator comparison. Both codes take exactly one step of the same dt (FDS LOCK_TIME_STEP at
# RISE's first accepted step) from the same state (FDS's 9.751 s state, fds_state_9p75.bin). Per field (T at cells;
# U, V, W at their faces) and per layer band: the change over the step in each code, and RISE's against FDS's
# (rms of each, regression slope of RISE's on FDS's, correlation, and rms of the difference over rms of FDS's).
# FDS values are its slices at t=0 and t=dt (CELL_CENTERED=T: U/V/W are staggered face values; xp_state.py indexing).
# argv[1]: RISE ckpt_state file at the later step, argv[2]: FDS run dir, argv[3]: RISE state at the earlier step (the
# initial state file for step 0), argv[4] (optional) 'a:b' = FDS frame indices of the two steps (default 0:last).
# FDS's slices at the first step show velocity and pressure updated but T and density not yet (identical to t=0), so
# scalar rows are meaningful only from 1:n.
import numpy as np,re,struct,sys
FA,FB=(int(x) for x in (sys.argv[4] if len(sys.argv)>4 else '0:-1').split(':'))
from pathlib import Path
N=69;NZ=106;DX=.030593116067411014
def split(a):
    nc=N*N*NZ;nu=(N+1)*N*NZ;nw=N*N*(NZ+1)
    return (a[:nc].reshape(NZ,N,N),a[nc:nc+nu].reshape(NZ,N,N+1),a[nc+nu:nc+2*nu].reshape(NZ,N+1,N),a[nc+2*nu:nc+2*nu+nw].reshape(NZ+1,N,N))
T0,U0,V0,W0=split(np.fromfile(sys.argv[3],dtype='<f8'));T1,U1,V1,W1=split(np.fromfile(sys.argv[1],dtype='<f8'))
run=Path(sys.argv[2]);sl=[l for l in (run/'xp.fds').read_text().splitlines() if l.startswith('&SLCF')];idx={}
for n,l in enumerate(sl,1):
    q=re.search(r"QUANTITY='([^']+)'",l).group(1);m=re.search(r"PBZ=([0-9.eE+-]+)",l)
    if m and 'SPEC_ID' not in l:idx.setdefault((q,int(round(float(m.group(1))/DX-.5))),n)
def frames(q,k):
    out=[]
    with (run/f'xp_1_{idx[(q,k)]}.sf').open('rb') as f:
        for _ in range(4):n=struct.unpack('<i',f.read(4))[0];f.seek(n+4,1)
        while True:
            h=f.read(4)
            if len(h)<4:break
            f.read(8);n=struct.unpack('<i',f.read(4))[0];d=f.read(n);f.read(4)
            if len(d)<n:break
            out.append(np.frombuffer(d,dtype='<f4').reshape((70,70),order='F').astype(np.float64))
    return out[FA],out[FB]
# FDS arrays a[i][j] (i = x index incl. -x face at 0). RISE arrays [z][y][x].
F={}
for q in ('TEMPERATURE','U-VELOCITY','V-VELOCITY','W-VELOCITY'):
    F[q]=[frames(q,k) for k in range(NZ)]
def fT(k,s):return F['TEMPERATURE'][k][s][1:,1:].T+273.15          # [y][x]
def fU(k,s):return F['U-VELOCITY'][k][s][:,1:].T                    # [y][x+1 faces]
def fV(k,s):return F['V-VELOCITY'][k][s][1:,:].T                    # [y+1 faces][x]
def fW(k,s):return F['W-VELOCITY'][k][s][1:,1:].T                   # top face of layer k, [y][x]
xc=(np.arange(N)-34)*DX;xf=(np.arange(N+1)-34.5)*DX
def mask(xs,ys,lo,hi):X,Y=np.meshgrid(xs,ys,indexing='xy');r=np.hypot(X,Y);return (r>=lo)&(r<hi)
regions={'core r<.1':(0,.1),'ring .1-.3':(.1,.3)}
bands=[(0,1),(1,3),(3,8),(8,17),(17,34),(34,66),(66,105)]
init_check=[]
for k in (0,3,16):init_check.append(f'k{k} max|dT| {abs(T0[k]-fT(k,0)).max():.3g} K, max|dW| {abs(W0[k+1]-fW(k,0)).max():.3g} m/s')
print('earlier-step agreement, RISE vs FDS (float32 slices):',' ; '.join(init_check))
fields={'T':(lambda k:T1[k]-T0[k],lambda k:fT(k,1)-fT(k,0),(xc,xc)),
 'U':(lambda k:U1[k]-U0[k],lambda k:fU(k,1)-fU(k,0),(xf,xc)),
 'V':(lambda k:V1[k]-V0[k],lambda k:fV(k,1)-fV(k,0),(xc,xf)),
 'W':(lambda k:W1[k+1]-W0[k+1],lambda k:fW(k,1)-fW(k,0),(xc,xc))}
print('per-step change: rms RISE / rms FDS, slope of RISE on FDS, corr, rms(R-F)/rms(F); W row k = top face of layer k')
for name,(r,f,(xs,ys)) in fields.items():
    for rg,(lo,hi) in regions.items():
        m=mask(xs,ys,lo,hi);line=f'{name} {rg:11}'
        for a,b in bands:
            R=np.concatenate([r(k)[m] for k in range(a,b)]);Fv=np.concatenate([f(k)[m] for k in range(a,b)])
            sl=(R*Fv).sum()/(Fv*Fv).sum();cc=(R*Fv).sum()/np.sqrt((R*R).sum()*(Fv*Fv).sum());d=np.sqrt(((R-Fv)**2).mean()/(Fv**2).mean())
            line+=f' | k{a}-{b-1}: {np.sqrt((R*R).mean()):.3g}/{np.sqrt((Fv*Fv).mean()):.3g} s{sl:.2f} c{cc:.2f} d{d:.2f}'
        print(line)
