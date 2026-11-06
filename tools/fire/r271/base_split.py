# r271 section 25 (read-only). RISE inputs are ckpt_layers/ckpt_div dumps of the inert compact run's checkpoints (paths
# relative to the working directory: run from /Volumes/SSD/RISE/fire_production_calibration/r271_small_scale/dumps; regenerate with
# tools/fire/r271/ckpt_layers.cpp / ckpt_div.cpp); FDS from r271_inert slices.
# 6-14 s: theta and w over the burner interior split into ring mean (axisymmetric, time mean), steady non-axisymmetric
# (time mean minus its ring mean) and unsteady (minus the time mean). rms over interior cells.
import numpy as np,sys,os
from pathlib import Path
os.environ['R271_FDS_CELL_AVERAGE']='1'
T0=Path('/Volumes/SSD/Working/RISE/tools/fire/r271');__file__=str(T0/'inert_shear.py')
sys.argv=['x','/Volumes/SSD/claude-tmp/claude-501/-Volumes-SSD-RISE/41335ae0-a8c4-4063-a4ea-ddf39b3401b1/scratchpad/r271_inert','inert_compact','inert_compact_seed02','inert_legacy'];exec(open(__file__).read().split('rad=np.arange')[0])
Rb=np.sqrt(69*DX*DX/np.pi);inn=RR<Rb-DX;rb=np.floor(RR/DX).astype(int)
def ringdev(p):
    o=p.copy()
    for b in np.unique(rb[inn]):m=rb==b;o[m]=p[m]-p[m].mean()
    return o
def tab(n,T,w):
    print(n)
    for k in (0,1,3,7,12):
        for q,nm in ((T[:,k],'th'),(w[:,k],'w ')):
            m=q.mean(0);st=np.sqrt((ringdev(m)[inn]**2).mean());un=np.sqrt(q[:,inn].var(0).mean())
            print(f'   k{k:2d} {nm} steady-nonaxi {st:7.3f}  unsteady {un:7.3f}  ratio {un/st:5.2f}',end='')
        print()
for arm in ARMS:
    a=np.transpose(np.fromfile(W/f'{arm}_near.bin',dtype='<f4').reshape(-1,NZ,4,N,N),(0,1,2,4,3)).astype(np.float64)
    tab(arm,a[:,:,0],a[:,:,3]);del a
T,w=fds_layers(idx,['TEMPERATURE','W-VELOCITY']);tab('FDS',T,w)
