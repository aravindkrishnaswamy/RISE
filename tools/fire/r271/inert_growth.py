# r271 non-reacting plume (section 22, read-only): where does the near-source fluctuation deficit form? Per near-field
# layer (18 layers, 6-14 s, near_planes.cpp for RISE; FDS via near_field.py fds_layers, cell averages): rms w' at the
# centre (3x3 cells) and on the burner-edge ring |r-R_b|<dx/2, and rms theta' at the edge, each in m/s or K.
# argv[1]: dir with <arm>_near.bin; argv[2:]: arm names.
import numpy as np,sys,os,re
from pathlib import Path
src=open(Path(__file__).with_name('inert_shear.py')).read().split('rad=np.arange')[0]
exec(src)
Rb=np.sqrt(69*DX*DX/np.pi);edge=np.abs(RR-Rb)<DX/2;c3=(np.abs(X)<1.6*DX)&(np.abs(Y)<1.6*DX)
def g(T,w):
    wp=w-w.mean(0);tp=T-T.mean(0)
    return [(np.sqrt((wp[:,k][:,c3]**2).mean()),np.sqrt((wp[:,k][:,edge]**2).mean()),np.sqrt((tp[:,k][:,edge]**2).mean())) for k in range(NZ)]
rows={}
for arm in ARMS:
    a=np.transpose(np.fromfile(W/f'{arm}_near.bin',dtype='<f4').reshape(-1,NZ,4,N,N),(0,1,2,4,3)).astype(np.float64)
    rows[arm]=g(a[:,:,0],a[:,:,3]);del a
T,w=fds_layers(idx,['TEMPERATURE','W-VELOCITY']);rows['FDS']=g(T,w)
print(f'rms per layer, 6-14 s: centre w (m/s) / burner-edge w (m/s) / burner-edge theta (K); R_b={Rb:.3f} m')
print('   z (m) '+''.join(f'{n:>26s}' for n in rows))
for k in range(NZ):print(f'   {(k+.5)*DX:5.3f} '+''.join(f'   {r[k][0]:5.3f} {r[k][1]:5.3f} {r[k][2]:6.1f}' for r in rows.values()))
