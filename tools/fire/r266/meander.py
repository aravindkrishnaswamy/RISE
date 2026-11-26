import numpy as np,re,struct
from pathlib import Path
DX=.030593116067411014;START=10.466040127212182;END=25.091040127212182
Z=[9,16,24,32,49,65,81,98]
R=np.fromfile('r251_planes.bin',dtype='<f4').reshape(235,8,4,69,69)  # [f,z,c,y,x]
R=np.transpose(R,(0,1,2,4,3))  # -> [f,z,c,x,y]
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
xy=(np.arange(69)-34)*DX;X,Y=np.meshgrid(xy,xy,indexing='ij');RR=np.hypot(X,Y);TH=np.arctan2(Y,X)
def analyze(th,w,b):
 # th: [t,x,y] temperature rise
 wgt=np.maximum(th,0);s=wgt.sum(axis=(1,2))
 xc=(wgt*X).sum(axis=(1,2))/s;yc=(wgt*Y).sum(axis=(1,2))/s
 dis=np.sqrt(((xc-xc.mean())**2+(yc-yc.mean())**2).mean())
 ring=(RR>0.6*b)&(RR<1.4*b)
 m=[]
 for mm in [0,1,2,3]:
  c=(th[:,ring]*np.exp(-1j*mm*TH[ring])).mean(axis=1)
  c=c-c.mean() if mm==0 else c
  m.append(np.sqrt((np.abs(c)**2).mean()))
 return xc.mean(),yc.mean(),dis,m
print('z  h   code  centroid_mean(x,y)mm  meander_rms_mm  meander/b   ring modes |m0| |m1| |m2| |m3| (K)')
for k,z in enumerate(Z):
 tF,TF=read_slice(RUN/f'{P}_1_{idx[("TEMPERATURE",z)]}.sf');s=(tF>=START)&(tF<=END);TF=TF[s]+273.15-300
 thR=R[:,k,0]-300.0
 bF=None
 mF=TF.mean(0);rad=np.arange(0,.8,DX/2);p=np.array([mF[np.abs(RR-a)<DX*.75].mean() for a in rad]);i=np.argmax(p<.5*p[0]);bF=np.interp(.5*p[0],[p[i],p[i-1]],[rad[i],rad[i-1]])
 for code,th in [('FDS',TF),('RISE',thR)]:
  x0,y0,d,m=analyze(th,None,bF)
  print(f'{z:3d} {(z+.5)*DX:.2f} {code:4s} ({x0*1e3:6.1f},{y0*1e3:6.1f})  {d*1e3:6.1f}  {d/bF:.3f}   '+' '.join(f'{v:7.1f}' for v in m)+f'   n={len(th)}')
