import sys,re,json
from pathlib import Path
import numpy as np
import struct
DX=.030593116067411014;START=10.466040127212182;END=25.091040127212182
def record(f):
 b=f.read(4)
 if not b:return None
 n=struct.unpack('<i',b)[0];data=f.read(n);f.read(4);return data
def read_slice(path):
 with path.open('rb') as f:
  label=record(f).decode().strip();record(f);record(f);limits=struct.unpack('<6i',record(f));times=[];fields=[]
  while True:
   t=record(f)
   if t is None:break
   times.append(struct.unpack('<f',t)[0]);fields.append(np.frombuffer(record(f),dtype='<f4').reshape((70,70),order='F').copy())
 return label,limits,np.array(times),np.array(fields)
RUN=Path('/Volumes/SSD/RISE/fire_production_calibration/r263_fds_no_slip/fds_run');P='r263_fds'
sl=[l for l in (RUN/(P+'.fds')).read_text().splitlines() if l.startswith('&SLCF')]
want={}
for n,l in enumerate(sl,1):
    q=re.search(r"QUANTITY='([^']+)'",l).group(1); m=re.search(r"PBZ=([0-9.eE+-]+)",l)
    if m and q in ('TEMPERATURE','W-VELOCITY','DENSITY','U-VELOCITY','V-VELOCITY','MASS FLUX Z'):
        z=float(m.group(1)); k=int(round(z/DX-0.5))
        want.setdefault(q,{}).setdefault(k,n)
print({q:len(v) for q,v in want.items()})
out={}
for q in ['TEMPERATURE','W-VELOCITY','DENSITY','U-VELOCITY','V-VELOCITY']:
    for k,n in sorted(want.get(q,{}).items()):
        lab,lim,t,a=read_slice(RUN/f'{P}_1_{n}.sf')
        a=a[:,1:,1:]
        mean=np.array([[average(t,a[:,i,j].astype(np.float64)) for j in range(69)] for i in range(69)]) if False else None
        # vectorized trapezoid average
        tt=np.r_[START,t[(t>START)&(t<END)],END]
        idx=np.searchsorted(t,tt).clip(1,len(t)-1)
        f=(tt-t[idx-1])/(t[idx]-t[idx-1]); f=f.clip(0,1)
        A=a[idx-1]*(1-f)[:,None,None]+a[idx]*f[:,None,None]
        mean=np.trapezoid(A.astype(np.float64),tt,axis=0)/(END-START)
        out[f'{q}|{k}']=mean
    print(q,'done',len(want.get(q,{})))
# mass flux faces: mean over time
for k,n in sorted(want['MASS FLUX Z'].items()):
    lab,lim,t,a=read_slice(RUN/f'{P}_1_{n}.sf');a=a[:,1:,1:]
    tt=np.r_[START,t[(t>START)&(t<END)],END];idx=np.searchsorted(t,tt).clip(1,len(t)-1)
    f=((tt-t[idx-1])/(t[idx]-t[idx-1])).clip(0,1)
    A=a[idx-1]*(1-f)[:,None,None]+a[idx]*f[:,None,None]
    out[f'MFZ|{k}']=np.trapezoid(A.astype(np.float64),tt,axis=0)/(END-START)
np.savez_compressed(sys.argv[1],**{k.replace('|','__').replace(' ','_').replace('-','_'):v for k,v in out.items()})
print('saved',len(out))
