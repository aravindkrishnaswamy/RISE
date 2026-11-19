# r271 section 26: the SGS share of the tendency, per code, from the same state (FDS's 9.751 s state): the change over
# steps 0->n with SGS on minus the change with SGS off (RISE xp<n>_sgs - xp<n>_allsgs0; FDS fds<n>_sgs - fds<n>_nosgs at
# frame n). Same-start differences cancel the shared start-up. Per field and layer band, r < 0.30 m: rms of each code's
# SGS share, slope of RISE's on FDS's, correlation. argv: n.
import numpy as np,sys,types,io,contextlib
from pathlib import Path
B='/Volumes/SSD/RISE/fire_production_calibration/r271_small_scale';n=sys.argv[1]
def load(rise,fds):
    m=types.SimpleNamespace();src=(Path(__file__).parent/'step1_compare.py').read_text();old=sys.argv
    sys.argv=['x',rise,fds,B+'/fds_state_9p75.bin',f'0:{n}']
    with contextlib.redirect_stdout(io.StringIO()):exec(src[:src.index("fields={")],m.__dict__)
    sys.argv=old;return m
on=load(f'{B}/xp{n}_sgs/state.bin',f'{B}/fds4_sgs');off=load(f'{B}/xp{n}_allsgs0/state.bin',f'{B}/fds4_nosgs')
N=69;H=.030593116067411014;xc=(np.arange(N)-34)*H;xf=(np.arange(N+1)-34.5)*H
def mask(xs,ys,lo,hi):X,Y=np.meshgrid(xs,ys,indexing='xy');r=np.hypot(X,Y);return (r>=lo)&(r<hi)
F={'U':(lambda m,k:m.U1[k],lambda m,k:m.fU(k,1),(xf,xc)),'V':(lambda m,k:m.V1[k],lambda m,k:m.fV(k,1),(xc,xf)),
   'W':(lambda m,k:m.W1[k+1],lambda m,k:m.fW(k,1),(xc,xc)),'T':(lambda m,k:m.T1[k],lambda m,k:m.fT(k,1),(xc,xc))}
bands=[(0,1),(1,3),(3,8),(8,17),(17,34),(34,66)]
print(f'SGS share of the change over steps 0->{n} (SGS on - SGS off), r<0.10 core and 0.10-0.30 ring: rms R/F, slope, corr')
for name,(r,f,(xs,ys)) in F.items():
    for rg,(lo,hi) in (('core',(0,.1)),('ring',(.1,.3))):
        msk=mask(xs,ys,lo,hi);line=f'{name} {rg}'
        for a,b in bands:
            R=np.concatenate([(r(on,k)-r(off,k))[msk] for k in range(a,b)]);Fv=np.concatenate([(f(on,k)-f(off,k))[msk] for k in range(a,b)])
            sl=(R*Fv).sum()/(Fv*Fv).sum();cc=(R*Fv).sum()/np.sqrt((R*R).sum()*(Fv*Fv).sum())
            line+=f' | k{a}-{b-1}: {np.sqrt((R*R).mean()):.3g}/{np.sqrt((Fv*Fv).mean()):.3g} s{sl:.2f} c{cc:.2f}'
        print(line)
