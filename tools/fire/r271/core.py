# r271: plume width, upward mass flux and mean-flow heat flux by height, FDS r263 / r264 / r270. Run in a dir with inflow.py (tools/fire/r270), fds_mean3d.npy and r264/r270_mean3d.bin.
exec(open('inflow.py').read().split("z=(np.arange")[0])
z=(np.arange(NZ)+.5)*DX;cp=1005;A=DX*DX
runs={'FDS':(F[3],F[2],F[8]),'r264':None,'r270':None}
import numpy as np,os
out={}
for name in ['FDS','r264','r270']:
    if name=='FDS':T,w,rw=F[3],F[2],F[8]
    else:
        Q=np.fromfile(name+'_mean3d.bin').reshape(7,NZ,N,N).transpose(0,1,3,2);T,w,rw=Q[0],Q[3],352.6*Q[6]
    out[name]=(T,w,rw)
print(' z    run   theta_c  w_c   b_T(m)  b_w(m)  up-mass(kg/s)  mean-flow H(kW)  int theta dA (K m2)')
for k in [9,16,24,32,49,65,98]:
    for name,(T,w,rw) in out.items():
        th=T[k]-300;thc=th[33:36,33:36].mean();wc=w[k][33:36,33:36].mean()
        bT=np.sqrt(th[th>0].sum()*A/(np.pi*thc));bw=np.sqrt(np.clip(w[k],0,None).sum()*A/(np.pi*wc))
        up=np.clip(rw[k],0,None).sum()*A;H=(cp*rw[k]*th).sum()*A/1e3
        print(f'{z[k]:5.2f} {name:5s} {thc:7.1f} {wc:5.2f}  {bT:.3f}  {bw:.3f}   {up:.4f}   {H:7.2f}   {th[th>0].sum()*A:.2f}')
