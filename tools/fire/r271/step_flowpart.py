# r271 section 26: split the steps 4->8 change into a buoyancy/pressure/source part (the same steps from the same T
# field at rest) and the remainder (flow part = full-state change - at-rest change), per code; compare the codes' flow
# parts by temperature bin and region. Superposition is approximate (the two runs' fields differ slightly by step 4).
import numpy as np,sys,types
from pathlib import Path
B='/Volumes/SSD/RISE/fire_production_calibration/r271_small_scale'
def load(argv):
    m=types.SimpleNamespace();src=(Path(__file__).parent/'step1_compare.py').read_text()
    old=sys.argv;sys.argv=['x']+argv
    import io,contextlib
    with contextlib.redirect_stdout(io.StringIO()):exec(src[:src.index("fields={")],m.__dict__)
    sys.argv=old;return m
full=load([B+'/xp8_allsgs0/state.bin',B+'/fds8_nosgs',B+'/xp4_allsgs0/state.bin','4:8'])
rest=load([B+'/xrest8/state.bin',B+'/fds8_rest',B+'/xrest4/state.bin','4:8'])
K=range(3,66);T0=full.T0
def parts(m,f):
    R=np.concatenate([f[0](m,k).ravel() for k in K]);F=np.concatenate([f[1](m,k).ravel() for k in K]);return R,F
W=(lambda m,k:m.W1[k+1]-m.W0[k+1],lambda m,k:m.fW(k,1)-m.fW(k,0))
T=(lambda m,k:m.T1[k]-m.T0[k],lambda m,k:m.fT(k,1)-m.fT(k,0))
U=(lambda m,k:m.U1[k]-m.U0[k],lambda m,k:m.fU(k,1)-m.fU(k,0))
Tc={'W':np.concatenate([(.5*(T0[k]+T0[k+1])).ravel() for k in K]),'T':np.concatenate([T0[k].ravel() for k in K]),
    'U':np.concatenate([np.concatenate([T0[k][:,:1],.5*(T0[k][:,1:]+T0[k][:,:-1]),T0[k][:,-1:]],1).ravel() for k in K])}
bins=[300,330,400,600,900,1300]
for name,f in (('W',W),('T',T),('U',U)):
    Rf,Ff=parts(full,f);Rr,Fr=parts(rest,f);Rx,Fx=Rf-Rr,Ff-Fr;print(f'{name}: flow part (full - at rest), steps 4->8')
    for a,b in zip(bins[:-1],bins[1:]):
        s=(Tc[name]>=a)&(Tc[name]<b);r,fv=Rx[s],Fx[s];rr,fr=Rr[s],Fr[s]
        print(f'  T {a:4d}-{b:4d} K  flow: rms R/F {np.sqrt((r*r).mean()):.4g}/{np.sqrt((fv*fv).mean()):.4g} slope {(r*fv).sum()/(fv*fv).sum():.3f} corr {(r*fv).sum()/np.sqrt((r*r).sum()*(fv*fv).sum()):.3f} | at-rest part rms R/F {np.sqrt((rr*rr).mean()):.4g}/{np.sqrt((fr*fr).mean()):.4g}')
