# FDS r263 no-slip flame height (same definitions as r251_flame_height.cpp / r250_acceptance) and centreline
# time means at McCaffrey stations, statistics window. Run beside tools/fire/r270/meander.py and fds_mean3d.npy.
import numpy as np
exec(open('meander.py').read().split("xy=(np.arange")[0].replace("R=np.fromfile(os.environ.get('PLANES','r270_planes.bin'),dtype='<f4').reshape(235,8,4,69,69)","").replace("R=np.transpose(R,(0,1,2,4,3))",""))
NZ=106;planes=[]
for k in range(NZ):
 t,a=read_slice(RUN/f'{P}_1_{idx[("HRRPUV",k)]}.sf');s=(t>=START)&(t<=END);planes.append(a[s].sum((1,2))*DX**3*1e3)  # kW/m3 -> W
t=t[s];H=np.array(planes).T  # [time][z] W
def height(p,target):
 c=0.
 for z,v in enumerate(p):
  if v>0 and c+v>=target:return (z+(target-c)/v)*DX
  c+=v
 return np.nan
inst=np.array([height(p,.99*p.sum()) for p in H]);m=H.mean(0)
print(f'FDS r263 samples {len(t)} ({t[0]:.3f}-{t[-1]:.3f} s), mean HRR {H.sum(1).mean()/1e3:.2f} kW')
print(f'time mean of instantaneous h99 (realized): {np.trapezoid(inst,t)/(t[-1]-t[0]):.3f} m')
print(f'mean-profile h99 realized: {height(m,.99*m.sum()):.3f} m   h99 of nominal 33 kW: {height(m,.99*33000):.3f} m')
F=np.load('fds_mean3d.npy');print('centreline (x=y=34) mean dT (K) / w (m/s); McCaffrey dT / w')
for k in [16,20,24,26,28,32,40,49,65,81,98]:
 h=(k+.5)*DX;e=23.3*(h/33**.4)**(-5/3);u=1.13*33**.2*(h/33**.4)**(-1/3)
 print(f' {h:.2f}  {F[3][k][34,34]-300:6.1f} / {F[2][k][34,34]:.2f}   McC {e:6.1f} / {u:.2f}   err {(F[3][k][34,34]-300)/e-1:+.2f} / {F[2][k][34,34]/u-1:+.2f}')
