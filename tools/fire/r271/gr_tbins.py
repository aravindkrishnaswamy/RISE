#!/usr/bin/env python3
"""r271 section 29: where the perturbation grows, by local temperature. For each pair (gr_analyze.py syntax, label=ctrl,pert,
state,scale), delta = pert - ctrl over layers 0..65, r < 0.25 m, binned by the control's cell temperature. Per bin: delta
kinetic energy (all scales) and its grid-scale part (delta minus its horizontal 1-2-1 filter in x and y), normalised by the
injected perturbation's total energy. GR_RATIO='A/B,...' prints armA/armB per bin at armA's times (armB log-interpolated)."""
import numpy as np,sys,os
ns={'__name__':'x'};exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)),'gr_analyze.py')).read().split('\nbase=state(')[0],ns)
TB=[300,350,600,900,2000];D=ns['DISK']
def f121(a):
    b=.5*a+.25*(np.roll(a,1,-2)+np.roll(a,-1,-2));return .5*b+.25*(np.roll(b,1,-1)+np.roll(b,-1,-1))
def bins(Tc,d):
    v=d[:,1:4];g=v-f121(v);ke=(v**2).sum(1);kg=(g**2).sum(1);o=[]
    for a,b in zip(TB[:-1],TB[1:]):
        m=(Tc>=a)&(Tc<b)&D[None];o+= [ke[m].sum(),kg[m].sum()]
    return np.array(o)
base=ns['state'](sys.argv[1]);arms={}
for arg in sys.argv[2:]:
    lab,rest=arg.split('=',1);cp,pp,sp,sc=rest.split(',');sc=float(sc)
    i0=(ns['state'](sp)-base)/sc;e0=(i0[:,1:4]**2).sum()
    tc,C=ns['load'](cp);tp,P=ns['load'](pp);ip=[int(np.argmin(abs(tp-x))) for x in tc];ok=[i for i in range(len(tc)) if abs(tp[ip[i]]-tc[i])<2.5e-3]
    arms[lab]=(tc[ok],np.array([bins(C[i][:,0],(P[ip[i]]-C[i])/sc) for i in ok])/e0)
hdr=''.join(f'{f"{a}-{b}K":>18s}' for a,b in zip(TB[:-1],TB[1:]))
print('delta KE (all / grid-scale) per control-temperature bin, r<0.25 m, layers 0-65, / injected KE\n  arm      t s'+hdr)
for lab,(t,X) in arms.items():
    for i in range(len(t)):print(f'  {lab:6s} {t[i]:6.3f}'+''.join(f'  {X[i,2*j]:8.3g}/{X[i,2*j+1]:<7.3g}' for j in range(4)))
for pr in [r.split('/') for r in os.environ.get('GR_RATIO','').split(',') if r]:
    (ta,A),(tb,Bv)=arms[pr[0]],arms[pr[1]];print(f'\n{pr[0]}/{pr[1]} per bin (all / grid-scale)\n  t s   '+hdr)
    for i in range(len(ta)):
        if ta[i]>tb[-1]+1e-6:continue
        r=[A[i,j]/np.exp(np.interp(ta[i],tb,np.log(np.maximum(Bv[:,j],1e-30)))) for j in range(8)]
        print(f'  {ta[i]:6.3f}'+''.join(f'  {r[2*j]:8.3g}/{r[2*j+1]:<7.3g}' for j in range(4)))
