#!/usr/bin/env python3
"""r271 section 30: budget of the twin perturbation (gr_perturb.py pairs, loaders from gr_analyze.py) per layer band and
horizontal scale. delta = perturbed minus control of the same code. From the cell-centred frames, at interval midpoints:
  dT/dt = P + A + R,  P = -delta_u . grad T_ctrl (stirring of the control's gradients),  A = -u_ctrl . grad delta_T,
  R = the rest (sub-grid and numerical diffusion, sources, compression, and the estimator's own error).
Centred differences (one-sided at the floor and at the top layer); the same estimator is applied to both codes.
Each term is projected on delta_T within a horizontal wavenumber mask and divided by the delta_T variance in that mask,
integrated over the time window: a contribution to the amplitude growth rate of delta_T (1/s); tot = P + A + R.
For delta_w: tot = its growth rate, B = the buoyancy part <delta_w g delta_T / T_ctrl> / <delta_w^2>, rest = tot - B.
argv: label=ctrl,pert ...   env GR_BANDS, GR_WIN='a:b,c:d' (s)."""
import numpy as np,sys,os
from pathlib import Path
src=(Path(__file__).parent/'gr_analyze.py').read_text().split('\nbase=state(')[0];sys_argv=sys.argv;exec(src)
WIN=[tuple(float(x) for x in w.split(':')) for w in os.environ.get('GR_WIN','0.025:0.1,0.1:0.2').split(',')]
MASK={'all':K>=0,'>=8dx':SC[0],'2.7-8dx':SC[1],'<2.7dx':SC[2]}
def grad(f):  # f [z][x][y] -> d/dx, d/dy, d/dz
    gx=np.gradient(f,DX,axis=1);gy=np.gradient(f,DX,axis=2);gz=np.gradient(f,DX,axis=0);return gx,gy,gz
def proj(a,b,m):  # sum over layers of <a b> within mask
    return (np.conj(np.fft.fft2(a,axes=(1,2)))*np.fft.fft2(b,axes=(1,2))).real[:,m].sum()/(N*N)
for arg in sys.argv[1:]:
    lab,rest=arg.split('=',1);cp,pp=rest.split(',');tc,C=load(cp);tp,P_=load(pp)
    ip=[int(np.argmin(abs(tp-x))) for x in tc];ok=[i for i in range(len(tc)) if abs(tp[ip[i]]-tc[i])<2.5e-3]
    t=tc[ok];C=C[ok];D=P_[[ip[i] for i in ok]]-C
    print(f'\n{lab}: growth-rate contributions (1/s)')
    print(f'  {"window":>11s} {"band":>6s} {"scale":>8s} | {"T tot":>7s} {"P":>7s} {"A":>7s} {"R":>7s} | {"w tot":>7s} {"B":>7s} {"rest":>7s} | {"rms dT":>8s} {"rms dw":>8s}')
    for w0,w1 in WIN:
        acc={}
        for i in range(len(t)-1):
            tm=.5*(t[i]+t[i+1])
            if tm<w0 or tm>w1:continue
            dt=t[i+1]-t[i];d=.5*(D[i]+D[i+1]);c=.5*(C[i]+C[i+1])  # [z][c][x][y]
            dT,du,dv,dw=(d[:,j] for j in range(4));Tc,uc,vc,wc=(c[:,j] for j in range(4))
            gT=grad(Tc);gd=grad(dT);Pt=-(du*gT[0]+dv*gT[1]+dw*gT[2]);At=-(uc*gd[0]+vc*gd[1]+wc*gd[2])
            rT=(D[i+1,:,0]-D[i,:,0])/dt;Rt=rT-Pt-At;rw=(D[i+1,:,3]-D[i,:,3])/dt;Bw=9.80665*dT/Tc
            for a,b in BANDS:
                s=slice(a,b+1)
                for mn,m in MASK.items():
                    v=np.array([proj(dT[s],x[s],m) for x in (dT,rT,Pt,At,Rt)]+[proj(dw[s],x[s],m) for x in (dw,rw,Bw)])
                    acc[(a,b,mn)]=acc.get((a,b,mn),0)+v*dt
        for (a,b,mn),v in acc.items():
            n=(b-a+1)*N*N*(w1-w0)
            print(f'  {w0:5.3f}-{w1:5.3f} {a:3d}-{b:<2d} {mn:>8s} | '+''.join(f'{x:7.2f} ' for x in v[1:5]/v[0])+'| '+f'{v[6]/v[5]:7.2f} {v[7]/v[5]:7.2f} {(v[6]-v[7])/v[5]:7.2f} | {np.sqrt(v[0]/n):8.2e} {np.sqrt(v[5]/n):8.2e}')
