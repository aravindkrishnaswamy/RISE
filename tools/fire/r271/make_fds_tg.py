#!/usr/bin/env python3
"""r271 section 25: FDS reference for the Taylor-Green test (tg_driver / build_tg.py): FDS r271_inert's input from rest
plus the same 2-D Taylor-Green array at t=0 through the CSVF UVWFILE: u = U f(z) sin(kX) cos(kY), v = -U f(z) cos(kX) sin(kY),
w = 0, k = m pi/L, X, Y from the domain's -x/-y edge, f(z) = 1 on 1.75-2.85 m with sin^2 ramps to 0 at 1.6 and 3.0 m (as
build_tg.py). T_END 0.5 s, NOISE=F. argv: run dir, m, U, Cv ('production' keeps 0.07)."""
import numpy as np,sys,re
from pathlib import Path
N=69;NZ=106;H=.030593116067411014;OUT=Path(sys.argv[1]);OUT.mkdir(parents=True);m=float(sys.argv[2]);U=float(sys.argv[3]);k=m*np.pi/(N*H)
def f(z):
    z=np.asarray(z,float);o=np.where((z>1.75)&(z<2.85),1.,0.)
    o=np.where((z>1.6)&(z<=1.75),np.sin(.5*np.pi*(z-1.6)/.15)**2,o);return np.where((z>=2.85)&(z<3.0),np.sin(.5*np.pi*(3.0-z)/.15)**2,o)
K,J,I=np.meshgrid(np.arange(NZ+1),np.arange(N+1),np.arange(N+1),indexing='ij');Z=(K-.5)*H
FU=np.where((J>=1)&(K>=1),U*f(Z)*np.sin(k*I*H)*np.cos(k*(J-.5)*H),0.)
FV=np.where((I>=1)&(K>=1),-U*f(Z)*np.cos(k*(I-.5)*H)*np.sin(k*J*H),0.)
with open(OUT/'uvw.csv','w') as fh:fh.write(f'0 {N} 0 {N} 0 {NZ}\n');np.savetxt(fh,np.stack([FU.ravel(),FV.ravel(),0*FU.ravel()],1),fmt='%.9e')
src=Path('/Volumes/SSD/RISE/fire_production_calibration/r271_inert_fds/fds_run/r271_inert.fds').read_text()
src=re.sub(r'&TIME T_END=[0-9.eE+-]+ /','&TIME T_END=0.5 /',src).replace("&MISC SIMULATION_MODE='LES',","&MISC NOISE=F, SIMULATION_MODE='LES',").replace("CHID='r271_inert'","CHID='tg'")
if sys.argv[4]!='production':src=src.replace('C_VREMAN=0.07',f'C_VREMAN={float(sys.argv[4])!r}')
i=src.index('&TAIL');src=src[:i]+"&CSVF UVWFILE='uvw.csv' /\n"+src[i:];(OUT/'tg.fds').write_text(src)
