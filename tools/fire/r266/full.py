# r264 vs r251 vs FDS no-slip, full statistics window, from retained frame planes / FDS slices
import numpy as np
exec(open('meander.py').read().split("print('z  h")[0].replace("R=np.fromfile('r251_planes.bin'","R0=np.fromfile('r251_planes.bin'").replace("R=np.transpose(R,","R0=np.transpose(R0,"))
def L(f): return np.transpose(np.fromfile(f,dtype='<f4').reshape(235,8,4,69,69),(0,1,2,4,3))
runs={'r251':L('r251_planes.bin'),'r264':L('r264_planes.bin')}
def hw(m):
 rad=np.arange(0,.8,DX/2);p=np.array([m[np.abs(RR-a)<DX*.75].mean() for a in rad]);i=np.argmax(p<.5*p[0]);return np.interp(.5*p[0],[p[i],p[i-1]],[rad[i],rad[i-1]])
def st(T,w):
 th=T-300;mt=th.mean(0);mw=w.mean(0);b=hw(mt)
 wg=np.maximum(th,0);s=wg.sum((1,2));xc=(wg*X).sum((1,2))/s;yc=(wg*Y).sum((1,2))/s
 me=np.sqrt(((xc-xc.mean())**2+(yc-yc.mean())**2).mean())
 rhow=(352.6/T)*w; mr=rhow.mean(0)
 return dict(wc=mw[34,34],wr=w[:,34,34].std()/mw[34,34],tc=mt[34,34],tr=th[:,34,34].std()/mt[34,34],b=b,me=me,mr=mr)
print('z_m   code  wc    wrms/wc  dTc    Trms/dT  bT/z   meander/b  core(r<2bF)  outer(2bF<r<0.8)  kg/s')
for k,z in enumerate(Z):
 h=(z+.5)*DX
 def F(q):
  t,a=read_slice(RUN/f'{P}_1_{idx[(q,z)]}.sf');s=(t>=START)&(t<=END);return a[s]
 TF=F('TEMPERATURE')+273.15;WF=F('W-VELOCITY');MF=F('MASS FLUX Z')
 sF=st(TF,WF);sF['mr']=MF.mean(0);bF=sF['b']
 core=RR<2*bF;outer=(RR>=2*bF)&(RR<.8)
 res=[('FDS',sF)]+[(n,st(a[:,k,0],a[:,k,3])) for n,a in runs.items()]
 for n,s in res:
  print(f"{h:.2f}  {n:5s} {s['wc']:5.2f}  {s['wr']:.3f}  {s['tc']:6.1f}  {s['tr']:.3f}  {s['b']/h:.3f}  {s['me']/s['b']:.3f}   {s['mr'][core].sum()*DX*DX:.4f}   {s['mr'][outer].sum()*DX*DX:.4f}")
 print()
print('SG94 far plume: wrms/wc 0.33, Trms/dT 0.40, b/z 0.10')
