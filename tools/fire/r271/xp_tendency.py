# r271 section 26: early tendency from the same initial state (FDS's 9.751 s state). Per layer, the change of T and of the
# cell-average w between t=0 and each early snapshot, RISE (xp_driver dump) against FDS (fdsab2/fdsxp slices):
# mean change (heating / mean acceleration), rms of the change, and the regression slope and correlation of RISE's
# change on FDS's, over r < 0.10 m (core) and 0.10 < r < 0.30 m (ring).
# argv[1]: RISE dump (ckpt_layers), argv[2]: FDS run dir, argv[3]: initial state (xp_state.py file).
import numpy as np,sys
from pathlib import Path
import types
xa=types.SimpleNamespace();_src=(Path(__file__).parent/'xp_analyze.py').read_text()
exec(_src[:_src.index('\narms')],xa.__dict__)  # helpers only (load, fds, grid)
N=69;NZ=106;LK=xa.LK;DX=xa.DX
r=np.hypot(xa.X,xa.Y);reg={'core':r<.10,'ring':(r>=.10)&(r<.30)}
s=np.fromfile(sys.argv[3],dtype='<f8');o=0
T0=s[o:o+N*N*NZ].reshape(NZ,N,N);o+=N*N*NZ;o+=NZ*N*(N+1);o+=NZ*(N+1)*N
W0=s[o:o+(NZ+1)*N*N].reshape(NZ+1,N,N)
T0=np.transpose(T0,(0,2,1))[list(LK)];W0=np.transpose(.5*(W0[:-1]+W0[1:]),(0,2,1))[list(LK)]
tR=np.loadtxt(sys.argv[1]+'.times',ndmin=1);tR=tR-tR[0] if tR[0]>1 else tR
a=np.fromfile(sys.argv[1],dtype='<f4').reshape(len(tR),66,4,N,N)[:,list(LK)];a=np.transpose(a,(0,1,2,4,3)).astype(float)
run=Path(sys.argv[2]);tF,TF,WF=xa.fds(run,'xp',0,.5)
print('initial state rms check: none (shared file)')
for i in range(3):
    j=int(np.argmin(abs(tF-tR[i])))
    print(f'== RISE t={tR[i]:.4f}  FDS t={tF[j]:.4f}  (changes scaled to per-second by each code\'s own t)')
    print(' layer  reg   dTmean R/F (K/s)      dTrms R/F       slope  corr |  dwmean R/F (m/s2)   dwrms R/F    slope  corr')
    for n,k in enumerate(LK):
        for name,m in reg.items():
            dTR=(a[i,n,0]-T0[n])[m]/tR[i];dTF=(TF[j,n]-T0[n])[m]/tF[j]
            dwR=(a[i,n,3]-W0[n])[m]/tR[i];dwF=(WF[j,n]-W0[n])[m]/tF[j]
            def st(R,F):
                Rc=R-R.mean();Fc=F-F.mean();sl=(Rc*Fc).sum()/(Fc*Fc).sum();cc=(Rc*Fc).sum()/np.sqrt((Rc*Rc).sum()*(Fc*Fc).sum())
                return f'{R.mean():8.2f}/{F.mean():8.2f} {Rc.std():7.2f}/{Fc.std():7.2f} {sl:6.2f} {cc:5.2f}'
            print(f' k{k:<3} {name:5} {st(dTR,dTF)} | {st(dwR,dwF)}')
