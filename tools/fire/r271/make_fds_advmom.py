#!/usr/bin/env python3
"""r271 section 25: FDS reference for the momentum-advection test (bt_driver kind advmom): FDS r271_inert's input from rest
plus u = U f(z) on the U faces and v = A f(z) cos(k x') on the V faces (x' from the domain centre, f as build_bt.py), via
the CSVF UVWFILE; T_END 0.5 s, NOISE=F, slices every 0.01 s. argv: run dir, n (cells per wavelength), A, U."""
import numpy as np,sys,re
from pathlib import Path
N=69;NZ=106;H=.030593116067411014;OUT=Path(sys.argv[1]);OUT.mkdir(parents=True);n=float(sys.argv[2]);A=float(sys.argv[3]);U=float(sys.argv[4]);k=2*np.pi/(n*H)
def f(z):
    z=np.asarray(z,float);o=np.where((z>1.75)&(z<2.85),1.,0.)
    o=np.where((z>1.6)&(z<=1.75),np.sin(.5*np.pi*(z-1.6)/.15)**2,o);return np.where((z>=2.85)&(z<3.0),np.sin(.5*np.pi*(3.0-z)/.15)**2,o)
K,J,I=np.meshgrid(np.arange(NZ+1),np.arange(N+1),np.arange(N+1),indexing='ij');Z=(K-.5)*H
FU=np.where((J>=1)&(K>=1),U*f(Z),0.);FV=np.where((I>=1)&(K>=1),A*f(Z)*np.cos(k*((I-.5)*H-34.5*H)),0.)
with open(OUT/'uvw.csv','w') as fh:fh.write(f'0 {N} 0 {N} 0 {NZ}\n');np.savetxt(fh,np.stack([FU.ravel(),FV.ravel(),0*FU.ravel()],1),fmt='%.9e')
src=Path('/Volumes/SSD/RISE/fire_production_calibration/r271_inert_fds/fds_run/r271_inert.fds').read_text()
src=re.sub(r'&TIME T_END=[0-9.eE+-]+ /','&TIME T_END=0.5 /',src).replace("&MISC SIMULATION_MODE='LES',","&MISC NOISE=F, SIMULATION_MODE='LES',").replace("CHID='r271_inert'","CHID='am'").replace('DT_SLCF=0.0625','DT_SLCF=0.01')
i=src.index('&TAIL');src=src[:i]+"&CSVF UVWFILE='uvw.csv' /\n"+src[i:];(OUT/'am.fds').write_text(src)
