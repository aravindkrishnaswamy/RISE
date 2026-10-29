import re,struct,numpy as np
from pathlib import Path
DX=.030593116067411014;START=10.466040127212182;END=25.091040127212182
RUN=Path('/Volumes/SSD/RISE/fire_production_calibration/r263_fds_no_slip/fds_run');P='r263_fds'
def record(f):
 b=f.read(4)
 if not b:return None
 n=struct.unpack('<i',b)[0];d=f.read(n);f.read(4);return d
def read_slice(p):
 with p.open('rb') as f:
  record(f);record(f);record(f);record(f);ts=[];fs=[]
  while True:
   t=record(f)
   if t is None:break
   ts.append(struct.unpack('<f',t)[0]);fs.append(np.frombuffer(record(f),dtype='<f4').reshape((70,70),order='F')[1:,1:].copy())
 return np.array(ts),np.array(fs,dtype=np.float64)
sl=[l for l in (RUN/(P+'.fds')).read_text().splitlines() if l.startswith('&SLCF')]
idx={}
for n,l in enumerate(sl,1):
 q=re.search(r"QUANTITY='([^']+)'",l).group(1);m=re.search(r"PBZ=([0-9.eE+-]+)",l)
 if m: idx.setdefault((q,int(round(float(m.group(1))/DX-.5))),n)
def stats(t,a):
 s=(t>=START)&(t<=END);t=t[s];a=a[s]
 w=np.gradient(t);w/=w.sum()
 m=np.tensordot(w,a,1);v=np.tensordot(w,(a-m)**2,1);return m,np.sqrt(v)
xy=(np.arange(69)-34)*DX;X,Y=np.meshgrid(xy,xy,indexing='ij');RR=np.hypot(X,Y)
def hw(field,frac=.5):
 rad=np.arange(0,.8,DX/2);p=np.array([field[(RR>=a-DX/2)&(RR<a+DX/2)].mean() if ((RR>=a-DX/2)&(RR<a+DX/2)).any() else np.nan for a in rad])
 p=np.array([field[np.abs(RR-a)<DX*0.75].mean() for a in rad])
 i=np.argmax(p<frac*p[0]);return np.interp(frac*p[0],[p[i],p[i-1]],[rad[i],rad[i-1]])
print('z h | FDS: wc w_rms/wc  dTc  T_rms/dT  b_w  b_T')
for k in [9,16,32,49,65,81,98]:
 t,W=read_slice(RUN/f'{P}_1_{idx[("W-VELOCITY",k)]}.sf');mw,sw=stats(t,W)
 t,T=read_slice(RUN/f'{P}_1_{idx[("TEMPERATURE",k)]}.sf');mT,sT=stats(t,T+273.15-300)
 c=(34,34)
 print(f'{k} {(k+.5)*DX:.2f} | {mw[c]:.2f} {sw[c]/mw[c]:.3f} {mT[c]:.0f} {sT[c]/mT[c]:.3f} {hw(mw):.3f} {hw(mT):.3f}  bT/z={hw(mT)/((k+.5)*DX):.3f}')
