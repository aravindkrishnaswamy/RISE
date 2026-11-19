# r271 section 25: 2x2 transplant. Dynamics {RISE, FDS} x initial state {RISE's at 9.757 s, FDS's at 9.751 s}, plus the
# two codes' own continuations (no transplant) as references. Per snapshot: rms over r < 0.10 m of w and theta minus the
# dynamics' own time mean (RISE dynamics: the RISE own continuation's mean; FDS dynamics: the FDS own continuation's),
# i.e. the instantaneous unsteady amplitude; averaged over 0.25 s bins of time since the start.
# argv[1]: RISE-dynamics/FDS-state dump (xp_driver checkpoints via ckpt_layers), argv[2]: RISE own dump (from 9.757 s),
# argv[3:]: optional further RISE-dynamics dumps, 'label=path' (label starting 'RISE dyn'). FDS arms are read from r271_small_scale/fdsxp_from_{rise,fds},
# and FDS ablation arms (FDS dynamics from RISE's state with one ingredient changed) from r271_small_scale/fdsab_*,
# and from FDS's state from fdsab2_*.
import numpy as np,sys,re,struct
from pathlib import Path
DX=.030593116067411014;N=69;NL=66;LK=(0,3,7,16,32,49,65)
xy=(np.arange(N)-34)*DX;X,Y=np.meshgrid(xy,xy,indexing='ij');c=np.hypot(X,Y)<.10
def load(p):
    t=np.loadtxt(str(p)+'.times',ndmin=1);a=np.fromfile(p,dtype='<f4').reshape(len(t),NL,4,N,N)[:,list(LK)]
    a=np.transpose(a,(0,1,2,4,3)).astype(np.float64);return t-t[0] if t[0]>1 else t,a[:,:,0],a[:,:,3]
def fds(run,chid,a,b):
    sl=[l for l in (run/(chid+'.fds')).read_text().splitlines() if l.startswith('&SLCF')];idx={}
    for n,l in enumerate(sl,1):
        q=re.search(r"QUANTITY='([^']+)'",l).group(1);m=re.search(r"PBZ=([0-9.eE+-]+)",l)
        if m and 'SPEC_ID' not in l:idx.setdefault((q,int(round(float(m.group(1))/DX-.5))),n)
    def frames(q,k):
        out=[];ts=[]
        with (run/f'{chid}_1_{idx[(q,k)]}.sf').open('rb') as f:
            for _ in range(4):n=struct.unpack('<i',f.read(4))[0];f.seek(n+4,1)
            while True:
                h=f.read(4)
                if len(h)<4:break
                tt=struct.unpack('<f',f.read(4))[0];f.read(4);hn=f.read(4)
                if len(hn)<4:break
                n=struct.unpack('<i',hn)[0]
                if a<=tt<=b:
                    d=f.read(n)
                    if len(d)<n:break
                    out.append(np.frombuffer(d,dtype='<f4').reshape((70,70),order='F')[1:,1:].astype(np.float64));f.read(4);ts.append(tt)
                else:f.seek(n+4,1)
        return np.array(ts),np.array(out)
    t=frames('TEMPERATURE',0)[0];m=len(t)
    T=np.stack([frames('TEMPERATURE',k)[1][:m] for k in LK],1)+273.15
    W=np.stack([.5*(frames('W-VELOCITY',k)[1][:m]+frames('W-VELOCITY',max(k-1,0))[1][:m]) for k in LK],1)
    return t-t[0],T,W
S=Path('/Volumes/SSD/RISE/fire_production_calibration/r271_small_scale')
arms={}
arms['RISE dyn, RISE own']=load(sys.argv[2])
arms['FDS dyn, FDS own']=fds(Path('/Volumes/SSD/RISE/fire_production_calibration/r271_inert_fds/fds_run'),'r271_inert',9.7509-1e-3,9.7509+2.6)
arms['RISE dyn, FDS state']=load(sys.argv[1])
for extra in sys.argv[3:]:  # further RISE-dynamics dumps as 'label=path' (a bare path is the RISE-state control)
    lab,_,path=extra.rpartition('=');arms[lab or 'RISE dyn, RISE state']=load(path)
for st in ('rise','fds'):
    run=S/f'fdsxp_from_{st}'
    if (run/'xp.fds').exists():
        try:arms[f'FDS dyn, {st.upper()} state']=fds(run,'xp',0,9)
        except Exception as e:print('#',run,e)
for run in sorted(S.glob('fdsab*_*')):  # FDS ablations from RISE's state (one FDS ingredient changed; see the run's xp.fds)
    if (run/'xp.fds').exists() and any(run.glob('xp_1_*.sf')):
        try:arms[(f'FDS dyn, FDS state, {run.name[7:]}' if run.name.startswith('fdsab2_') else f'FDS dyn, RISE state, {run.name[6:]}')]=fds(run,'xp',0,9)
        except Exception as e:print('#',run,e)
mean={'RISE':tuple(q.mean(0) for q in arms['RISE dyn, RISE own'][1:]),'FDS':tuple(q.mean(0) for q in arms['FDS dyn, FDS own'][1:])}
print("Unsteady amplitude: rms over r < 0.10 m of the deviation from the dynamics' own time mean, w (m/s) [theta (K)]")
print('time since start                  '+''.join(f'        k{k:<3d}' for k in LK))
for b in np.arange(0,2.5,.25):
    for n,(t,T,W) in arms.items():
        s=(t>=b)&(t<b+.25)
        if s.sum()==0:continue
        Tm,Wm=mean[n.split()[0]];aT=np.sqrt(((T[s]-Tm)[:,:,c]**2).mean(2)).mean(0);aW=np.sqrt(((W[s]-Wm)[:,:,c]**2).mean(2)).mean(0)
        print(f'{b:4.2f}-{b+.25:4.2f} {n:24s} n={s.sum():2d}'+''.join(f'  {aW[i]:5.3f}[{aT[i]:3.0f}]' for i in range(len(LK))))
    print()
