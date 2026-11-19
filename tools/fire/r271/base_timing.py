# r271 section 25 (read-only). RISE inputs are ckpt_layers/ckpt_div dumps of the inert compact run's checkpoints (paths
# relative to the working directory: run from /Volumes/SSD/RISE/fire_production_calibration/r271_small_scale/dumps; regenerate with
# tools/fire/r271/ckpt_layers.cpp / ckpt_div.cpp); FDS from r271_inert slices.
# Which height turns unsteady first? Per 0.5 s window: temporal rms of w within the window, mean over r<0.10 m, by layer.
import numpy as np,sys,os,struct
from pathlib import Path
os.environ['R271_FDS_CELL_AVERAGE']='1'
T0=Path('/Volumes/SSD/Working/RISE/tools/fire/r271');__file__=str(T0/'inert_shear.py')
sys.argv=['x','.','d'];exec(open(__file__).read().split('rad=np.arange')[0])
START,END=0.,8.
LK=(0,3,7,16,32,49,65);c=RR<.10
wins=[(a,a+.5) for a in np.arange(0,8,.5)]
def tab(n,t,w,T):
    print(n+'   rms w (m/s) [rms theta (K)] within 0.5 s windows, r<0.10 m')
    print('   window  '+''.join(f'     k{k:<3d} ' for k in LK))
    for a,b in wins:
        s=(t>=a)&(t<b)
        if s.sum()<3:continue
        print(f'  {a:3.1f}-{b:3.1f}'+''.join(f'  {np.sqrt(w[s,i][:,c].var(0)).mean():5.3f}[{np.sqrt(T[s,i][:,c].var(0)).mean():3.0f}]' for i in range(len(LK))))
t=np.loadtxt('compact_ck66.bin.times');a=np.fromfile('compact_ck66.bin',dtype='<f4').reshape(len(t),66,4,N,N)
a=np.transpose(a[:,list(LK)],(0,1,2,4,3)).astype(np.float64)
tab('RISE compact',t,a[:,:,3],a[:,:,0])
with (RUN/f'{P}_1_{idx[("TEMPERATURE",0)]}.sf').open('rb') as f:
    for _ in range(4):record(f)
    ts=[]
    while True:
        r=record(f)
        if r is None:break
        ts.append(struct.unpack('<f',r)[0]);record(f)
tF=np.array(ts);tF=tF[(tF>=0)&(tF<=8)]
def fw(k):
    top=read_slice(RUN/f'{P}_1_{idx[("W-VELOCITY",k)]}.sf');bot=read_slice(RUN/f'{P}_1_{idx[("W-VELOCITY",k-1)]}.sf') if k else top
    return .5*(top+bot)
w=np.stack([fw(k) for k in LK],1);T=np.stack([read_slice(RUN/f'{P}_1_{idx[("TEMPERATURE",k)]}.sf') for k in LK],1)
# sample FDS at the RISE checkpoint times (nearest frame) so the window statistics use the same sampling
j=np.array([np.argmin(abs(tF-x)) for x in t]);tab('FDS (at RISE checkpoint times)',tF[j],w[j],T[j])
