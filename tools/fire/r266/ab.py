import numpy as np
DX=.030593116067411014;Z=[9,16,24,32,49,65,81,98]
xy=(np.arange(69)-34)*DX;X,Y=np.meshgrid(xy,xy,indexing='ij');RR=np.hypot(X,Y)
def load(f):
 a=np.fromfile(f,dtype='<f4').reshape(-1,8,4,69,69);return np.transpose(a,(0,1,2,4,3))[16:]  # skip first 1 s
def hw(m):
 rad=np.arange(0,.8,DX/2);p=np.array([m[np.abs(RR-a)<DX*.75].mean() for a in rad]);i=np.argmax(p<.5*p[0]);return np.interp(.5*p[0],[p[i],p[i-1]],[rad[i],rad[i-1]])
def stats(a,k):
 th=a[:,k,0]-300;w=a[:,k,3];mt=th.mean(0);mw=w.mean(0);b=hw(mt)
 wg=np.maximum(th,0);s=wg.sum((1,2));xc=(wg*X).sum((1,2))/s;yc=(wg*Y).sum((1,2))/s
 me=np.sqrt(((xc-xc.mean())**2+(yc-yc.mean())**2).mean())
 # plume mass proxy: ∫ρw over r<0.8 with rho=352.6/T
 rho=352.6/(a[:,k,0]);m=((rho*w)[:,RR<.8]).sum(1).mean()*DX*DX
 return mw[34,34],w[:,34,34].std()/mw[34,34],mt[34,34],th[:,34,34].std()/mt[34,34],b/((Z[k]+.5)*DX),me/b,m
runs=[('baseline r251','x_r251.bin'),('centered mom','x_r260_components_centered_momentum.bin'),('half dt','x_r260_components_half_dt.bin'),('CHARM','x_r257_charm_ab_run.bin'),('CHARM+centered','x_r262_combined_combined.bin')]
data={n:load(f) for n,f in runs}
print('height  run              wc   w_rms/wc  dTc  T_rms/dT  bT/z  meander/b  mass(kg/s)')
for k in [3,5,7]:
 for n,_ in runs:
  s=stats(data[n],k);print(f'{(Z[k]+.5)*DX:.2f}  {n:15s} {s[0]:5.2f}  {s[1]:.3f}  {s[2]:5.0f}  {s[3]:.3f}  {s[4]:.3f}  {s[5]:.3f}  {s[6]:.3f}')
 print()
print('FDS full window ref: 1m w_rms 0.348 T_rms 0.461 bT/z 0.109 meander 0.383; 2m 0.314 0.413 0.106 0.329; 3m 0.327 0.417 0.100 0.321; SG94 0.33 0.40 0.10')
