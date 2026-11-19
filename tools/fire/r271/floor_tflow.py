# r271 section 27: the floor-layer (k0) temperature change in flow, steps 4->8, SGS off. Per code: full change, the
# at-rest run's change from the same temperature (source + expansion + diffusion), the flow part (full - rest), and two
# advection references on the code's own mean of the step-4 and step-8 states: horizontal -4 dt (u dT/dx + v dT/dy)
# (cell-centre velocity, central differences) and vertical -4 dt w_top (T1 - T0)/dx / 2 (top-face w of layer 0, one-sided).
# Core r < 0.10 m and ring 0.10-0.30 m. FDS T frames lag one step (frame i = step i-1): T steps 3->7 from frames 4->8.
# argv: RISE full step 4, 8; RISE rest step 4, 8 (ckpt_state); FDS full dir; FDS rest dir.
import numpy as np,sys,importlib.util
from pathlib import Path
spec=importlib.util.spec_from_file_location('hw',Path(__file__).parent/'hot_wflow.py')
N=69;NZ=106;DX=.030593116067411014;DT=.0016462659696117043
src=(Path(__file__).parent/'hot_wflow.py').read_text();ns={'sys':type('S',(),{'argv':['x']+['']*7})}
exec(src[:src.index('R2,R3,R4')],ns);exec(src[src.index('def fds('):src.index('FT,FU,FV,FW=')],ns)
split,fds=ns['split'],ns['fds']
R4,R8,Q4,Q8=(split(np.fromfile(p,'<f8')) for p in sys.argv[1:5])
FT,FU,FV,FW=fds(sys.argv[5],[4,8]);GT,GU,GV,GW=fds(sys.argv[6],[4,8])
xc=(np.arange(N)-34)*DX;X,Y=np.meshgrid(xc,xc,indexing='xy');r=np.hypot(X,Y);regions={'core':r<.10,'ring':(r>=.10)&(r<.30)}
def refs(T,U,V,W):
    uc=.5*(U[0][:,1:]+U[0][:,:-1]);vc=.5*(V[0][1:]+V[0][:-1])
    h=-4*DT*(uc*np.gradient(T[0],DX,axis=1)+vc*np.gradient(T[0],DX,axis=0));v=-4*DT*.5*W[1]*(T[1]-T[0])/DX;return h,v
mean=lambda a,b:tuple(.5*(x+y) for x,y in zip(a,b))
cases={'RISE':(R8[0][0]-R4[0][0],Q8[0][0]-Q4[0][0],refs(*mean(R4,R8))),
       'FDS':(FT[1][0]-FT[0][0],GT[1][0]-GT[0][0],refs(*mean((FT[0],FU[0],FV[0],FW[0]),(FT[1],FU[1],FV[1],FW[1]))))}
print('floor layer k0, 4-step T change: mean / rms of full, rest, flow part (full - rest); rms of horizontal and vertical')
print('advection references; slope and corr of the flow part on (horizontal + vertical)')
for reg,m in regions.items():
    for name,(full,rest,(h,v)) in cases.items():
        fl=full-rest;a=(h+v)[m];f=fl[m]
        print(f'  {reg:4s} {name:4s} full {full[m].mean():7.2f}/{np.sqrt((full[m]**2).mean()):6.2f}  rest {rest[m].mean():7.2f}/{np.sqrt((rest[m]**2).mean()):6.2f}'
              f'  flow {f.mean():7.2f}/{np.sqrt((f**2).mean()):6.2f}  | ref h {np.sqrt((h[m]**2).mean()):6.2f} v {np.sqrt((v[m]**2).mean()):6.2f}'
              f'  slope {(f*a).sum()/(a*a).sum():.3f} corr {np.corrcoef(f,a)[0,1]:.2f}')
