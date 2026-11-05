import re
# r271 small-scale response tests (section 24): reduce bt_driver logs (R271_BT lines; build_bt.py) against references.
# argv[1]: run directory (holds buoy_n*.log, adv_n*.log).
# buoy: response R = d(Aw)/dt / Ab, fitted from step 2 (step 1 carries the start-up projection transient). References for
#   the isotropic cos^3 pattern (k_x=k_y=k_z=k): continuum k_h^2/|k|^2 = 2/3; ideal MAC (two-cell face average of
#   buoyancy, cos(k dx/2), and the exact discrete projection, still 2/3 for the isotropic pattern) = 2/3 cos(k dx/2).
# adv: amplitude |a| = hypot(ac, as) of T-300 on cos/sin(k x'), decay per cell crossed -ln(|a|_end/|a|_1)/(U (t_end-t_1)/dx)
#   with the molecular-diffusion share exp(-kappa k^2 t) removed (kappa 2.2e-5 m2/s, air at 300 K), and the phase-speed
#   ratio. Reference: FDS's scalar scheme (CHARM-limited face values, explicit predictor-corrector, flux form) on a
#   periodic 1-D line with the same dx, U, and RISE's own accepted time steps.
import numpy as np,re,sys,math
from pathlib import Path
R=Path(sys.argv[1]);DX=.030593116067411014;KAP=2.2e-5
def parse(p):
    rows=[[float(v) for v in re.findall(r'=([-0-9.eE+]+)',l)[:7]] for l in p.read_text().splitlines() if l.startswith('R271_BT step=')]
    a=np.array(rows);_,i=np.unique(a[:,0],return_index=True);return a[np.sort(i)]   # the launch probe repeats step 0
def charm(phi,U,dts):
    def flux(q):
        up=np.roll(q,1);dn=q;uu=np.roll(q,2)   # face i-1/2 between cells i-1 (upwind, U>0) and i
        d=dn-up;r=np.where(np.abs(d)>1e-300,(up-uu)/np.where(np.abs(d)>1e-300,d,1),0.)
        B=np.where(r>0,r*(3*r+1)/(r+1)**2,0.);return U*(up+.5*B*d)
    for dt in dts:
        F=flux(phi);s=phi-dt/DX*(np.roll(F,-1)-F);F2=flux(s);phi=.5*(phi+s-dt/DX*(np.roll(F2,-1)-F2))
    return phi
print('Buoyancy response R = dAw/dt / Ab (isotropic cos^3 pattern, 2 K)')
print('   n   steps   R RISE   ideal MAC   continuum   RISE/ideal')
for n in (3,4,6,8):
    p=R/f'buoy_n{n}.log'
    if not p.exists():continue
    a=parse(p);s=a[a[:,0]>=2];k=2*np.pi/(n*DX);slope=np.polyfit(s[:,1],s[:,3],1)[0];Ab=a[a[:,0]>=1,2].mean()
    ideal=2/3*math.cos(k*DX/2);print(f'  {n:2d}   {len(a):4d}   {slope/Ab:6.3f}     {ideal:6.3f}      {2/3:6.3f}      {slope/Ab/ideal:6.3f}')
print('\nAdvection, U = 0.5 m/s, theta = 1 K (1 + cos(k x\')) from per-step lines (equal to the retained checkpoints at their')
print('steps). The uniform offset A projects onto cos/sin over the 49 core cells; its projection is removed. Fit from step 6')
print('(steps 1-5 hold the start-up transient) while the amplitude exceeds 5% of its step-6 value: decay per cell crossed (molecular')
print('share removed) and phase speed / U, against FDS CHARM over the same steps.')
print('   n   steps fitted  cells   RISE decay/cell   CHARM decay/cell   RISE c/U   CHARM c/U')
xc=(np.arange(10,59)+.5)*DX-34.5*DX
for n in (3,4,6,8):
    p=R/f'adv_n{n}.log'
    if not p.exists():continue
    a=parse(p);k=2*np.pi/(n*DX);U=0.5;cs,sn=np.cos(k*xc),np.sin(k*xc)
    A0=float(re.search(r' A=([0-9.eE+-]+)',p.read_text()).group(1))  # offset equals the amplitude A
    oc=A0*cs.sum()/(cs**2).sum();os_=A0*sn.sum()/(sn**2).sum()
    a=a[a[:,0]>=6]
    if len(a)<10:print(f'  {n:2d}   run stopped at step {int(parse(p)[-1,0])} (see the log)');continue
    t=a[:,1];ac=a[:,4]-oc;as_=a[:,5]-os_;amp=np.hypot(ac,as_);ph=np.unwrap(np.arctan2(as_,ac))
    m=amp>0.05*amp[0];m=np.cumprod(m).astype(bool);t,amp,ph=t[m],amp[m],ph[m];cells=U*(t-t[0])/DX
    dR=-np.polyfit(cells,np.log(amp)+KAP*k*k*(t-t[0]),1)[0];cR=np.polyfit(t,ph,1)[0]/(k*U)
    L=24*n;x=(np.arange(L)+.5)*DX;phi=np.cos(k*x);A=[];P=[]
    for dt in np.diff(np.r_[t[0],t]):
        if dt>0:phi=charm(phi,U,[dt])
        A.append(math.hypot((phi*np.cos(k*x)).sum(),(phi*np.sin(k*x)).sum())*2/L);P.append(math.atan2((phi*np.sin(k*x)).sum(),(phi*np.cos(k*x)).sum()))
    dC=-np.polyfit(cells,np.log(A),1)[0];cC=np.polyfit(t,np.unwrap(P),1)[0]/(k*U)
    print(f'  {n:2d}   {len(t):5d}       {cells[-1]:5.2f}     {dR:7.3f}            {dC:7.3f}         {cR:6.3f}     {cC:6.3f}')
