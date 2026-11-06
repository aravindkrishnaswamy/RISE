# r271 section 25 (read-only). RISE inputs are ckpt_layers/ckpt_div dumps of the inert compact run's checkpoints (paths
# relative to the working directory: run from /Volumes/SSD/RISE/fire_production_calibration/r271_small_scale/dumps; regenerate with
# tools/fire/r271/ckpt_layers.cpp / ckpt_div.cpp); FDS from r271_inert slices.
# Build-up of non-axisymmetric structure over the burner: per snapshot, theta and w minus their ring (azimuthal) mean
# in dx-wide radius bins, rms over the burner interior, at layers 0,1,3,7. FDS slices (cell averages) vs RISE checkpoints.
import numpy as np,sys,os
from pathlib import Path
os.environ['R271_FDS_CELL_AVERAGE']='1'
T0=Path('/Volumes/SSD/Working/RISE/tools/fire/r271');__file__=str(T0/'inert_shear.py')
sys.argv=['x','.','dummy'];exec(open(__file__).read().split('rad=np.arange')[0])
START,END=0.,30.
Rb=np.sqrt(69*DX*DX/np.pi);inn=RR<Rb-DX;rb=np.floor(RR/DX).astype(int)
def nonax(p):  # p [t][x][y]
    out=np.empty_like(p);
    for b in np.unique(rb[inn]):
        m=rb==b;out[:,m]=p[:,m]-p[:,m].mean(1,keepdims=True)
    return np.sqrt((out[:,inn]**2).mean(1))
LK=(0,1,3,7)
def report(name,t,T,w,tb):
    print(name)
    for a,b in tb:
        s=(t>=a)&(t<b)
        if s.sum()==0:continue
        print(f'  {a:4.1f}-{b:4.1f} s n={s.sum():3d}  '+'  '.join(f'k{k}: th {nonax(T[s,k]).mean():5.1f} w {nonax(w[s,k]).mean():5.3f}' for k in LK))
tb=[(0,.25),(.25,.5),(.5,.75),(.75,1),(1,1.5),(1.5,2),(2,3),(3,4),(4,6)]
t=np.loadtxt('compact_ck.bin.times');a=np.transpose(np.fromfile('compact_ck.bin',dtype='<f4').reshape(len(t),8,4,N,N),(0,1,2,4,3)).astype(np.float64)
report('RISE compact (checkpoints)',t,a[:,:,0],a[:,:,3],tb)
exec(open(T0/'inert_compare.py').read().split('START,END=0.,1e9;tF=fds_times()')[0].split('W=Path(sys.argv[1])')[0].split("RUN=Path")[0]) if False else None
with (RUN/f'{P}_1_{idx[("TEMPERATURE",0)]}.sf').open('rb') as f:
    for _ in range(4):record(f)
    ts=[]
    while True:
        r=record(f)
        if r is None:break
        ts.append(struct.unpack('<f',r)[0]);record(f)
tF=np.array(ts)
T=np.stack([read_slice(RUN/f'{P}_1_{idx[("TEMPERATURE",k)]}.sf') for k in range(8)],1)+273.15
NZs=NZ;
w=fds_layers(idx,['W-VELOCITY'])[0][:,:8]
report('FDS',tF[:len(T)],T,w,tb)
