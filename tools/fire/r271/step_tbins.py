# r271 section 26: the one-step-window comparison (step1_compare.py, steps 2->4) binned by the cell's temperature at the
# earlier step: slope of RISE's change on FDS's and the rms ratio, for T (cells) and W (top faces, cell-pair mean T).
# argv[1]: RISE later state, argv[2]: FDS run dir, argv[3]: RISE earlier state, argv[4]: 'a:b' FDS frames.
import numpy as np,sys,types
from pathlib import Path
m=types.SimpleNamespace();src=(Path(__file__).parent/'step1_compare.py').read_text()
sys_argv=sys.argv;exec(src[:src.index("fields={")],m.__dict__)
T0=m.T0;bins=[300,330,400,600,900,1300,2500];K=range(3,66)
def stats(R,F,Tc,name):
    print(name);
    for a,b in zip(bins[:-1],bins[1:]):
        s=(Tc>=a)&(Tc<b);
        if s.sum()<50:continue
        r,f=R[s],F[s];print(f'  T {a:4d}-{b:4d} K  n={s.sum():7d}  slope {(r*f).sum()/(f*f).sum():.3f}  rms R/F {np.sqrt((r*r).mean()/(f*f).mean()):.3f}  corr {(r*f).sum()/np.sqrt((r*r).sum()*(f*f).sum()):.3f}')
R=np.concatenate([(m.T1[k]-T0[k]).ravel() for k in K]);F=np.concatenate([(m.fT(k,1)-m.fT(k,0)).ravel() for k in K]);Tc=np.concatenate([T0[k].ravel() for k in K])
stats(R,F,Tc,'T change, layers 3-65')
R=np.concatenate([(m.W1[k+1]-m.W0[k+1]).ravel() for k in K]);F=np.concatenate([(m.fW(k,1)-m.fW(k,0)).ravel() for k in K]);Tc=np.concatenate([(.5*(T0[k]+T0[k+1])).ravel() for k in K])
stats(R,F,Tc,'W change (top faces), layers 3-65')
Tu=lambda k:np.concatenate([T0[k][:,:1],.5*(T0[k][:,1:]+T0[k][:,:-1]),T0[k][:,-1:]],1)
R=np.concatenate([(m.U1[k]-m.U0[k]).ravel() for k in K]);F=np.concatenate([(m.fU(k,1)-m.fU(k,0)).ravel() for k in K]);Tc=np.concatenate([Tu(k).ravel() for k in K])
stats(R,F,Tc,'U change (x faces), layers 3-65')
