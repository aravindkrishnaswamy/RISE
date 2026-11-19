# r271 (c): FDS r263 outermost-cell rings (side columns at all heights, top layer) per slice frame in 10.466-25.091 s -> fds_ring.npz (read-only).
import numpy as np,re,struct
from pathlib import Path
DX=.030593116067411014;START=10.466040127212182;END=25.091040127212182;N=69;NZ=106
RUN=Path('/Volumes/SSD/RISE/fire_production_calibration/r263_fds_no_slip/fds_run');P='r263_fds'
def record(f):
 b=f.read(4)
 if not b:return None
 n=struct.unpack('<i',b)[0];d=f.read(n);f.read(4);return d
def read_slice(p):
 with p.open('rb') as f:
  for _ in range(4):record(f)
  ts=[];fs=[]
  while True:
   t=record(f)
   if t is None:break
   ts.append(struct.unpack('<f',t)[0]);fs.append(np.frombuffer(record(f),dtype='<f4').reshape((70,70),order='F')[1:,1:].copy())
 return np.array(ts),np.array(fs)  # [t][x][y]
sl=[l for l in (RUN/(P+'.fds')).read_text().splitlines() if l.startswith('&SLCF')]
idx={}
for n,l in enumerate(sl,1):
 q=re.search(r"QUANTITY='([^']+)'",l).group(1);m=re.search(r"PBZ=([0-9.eE+-]+)",l)
 if m:idx.setdefault((q,int(round(float(m.group(1))/DX-.5))),n)
Q=['U-VELOCITY','V-VELOCITY','W-VELOCITY','TEMPERATURE','DENSITY','PRESSURE']
side=None;top=None
for k in range(NZ):
 for qi,q in enumerate(Q):
  t,a=read_slice(RUN/f'{P}_1_{idx[(q,k)]}.sf');s=(t>=START-1e-6)&(t<=END+1e-6);a=a[s]
  if side is None:side=np.zeros((s.sum(),len(Q),4,NZ,N),np.float32);times=t[s]
  side[:,qi,0,k]=a[:,0,:];side[:,qi,1,k]=a[:,-1,:];side[:,qi,2,k]=a[:,:,0];side[:,qi,3,k]=a[:,:,-1]
  if k==NZ-1:
   if top is None:top=np.zeros((s.sum(),len(Q)+1,N,N),np.float32)
   top[:,qi]=a
t,a=read_slice(RUN/f'{P}_1_{idx[("MASS FLUX Z",NZ-1)]}.sf');s=(t>=START-1e-6)&(t<=END+1e-6);top[:,len(Q)]=a[s]
side[:,3]+=273.15;top[:,3]+=273.15
np.savez('fds_ring.npz',side=side,top=top,times=times)  # side [t][U,V,W,T,rho,p][xmin,xmax,ymin,ymax][z][along]; top [t][..,MASSFLUXZ][x][y]
print('frames',len(times),times[0],times[-1])
