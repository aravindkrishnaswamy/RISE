# r271 section 25: Taylor-Green mode amplitude in FDS slices (make_fds_tg.py runs), as tg_mode.cpp: projection of the U and V
# face values onto the initial TG shape over z 1.9-2.7 m, per slice frame. argv: run dirs (each with m in its name: _m<m>_).
import numpy as np,re,struct,sys
from pathlib import Path
N=69;H=.030593116067411014
for run in map(Path,sys.argv[1:]):
    m=float(re.search(r'_m(\d+)',run.name).group(1));k=m*np.pi/(N*H)
    sl=[l for l in (run/'tg.fds').read_text().splitlines() if l.startswith('&SLCF')];idx={}
    for n,l in enumerate(sl,1):
        q=re.search(r"QUANTITY='([^']+)'",l).group(1);mm=re.search(r"PBZ=([0-9.eE+-]+)",l)
        if mm:idx.setdefault((q,int(round(float(mm.group(1))/H-.5))),n)
    def read(q,kz):
        with (run/f'tg_1_{idx[(q,kz)]}.sf').open('rb') as fh:
            for _ in range(4):n=struct.unpack('<i',fh.read(4))[0];fh.seek(n+4,1)
            ts=[];fs=[]
            while True:
                h=fh.read(4)
                if len(h)<4:break
                ts.append(struct.unpack('<f',fh.read(4))[0]);fh.read(4);n=struct.unpack('<i',fh.read(4))[0]
                fs.append(np.frombuffer(fh.read(n),dtype='<f4').reshape((70,70),order='F'));fh.read(4)
        return np.array(ts),np.array(fs,dtype=np.float64)
    I=np.arange(70)[:,None];J=np.arange(70)[None,:]
    su=np.sin(k*I*H)*np.cos(k*(J-.5)*H);su[:,0]=0;sv=-np.cos(k*(I-.5)*H)*np.sin(k*J*H);sv[0,:]=0
    p=0;nrm=0
    for kz in range(106):
        if not 1.9<=(kz+.5)*H<=2.7:continue
        t,Uf=read('U-VELOCITY',kz);_,Vf=read('V-VELOCITY',kz);n=min(len(Uf),len(Vf))
        p=p+(Uf[:n]*su).sum((1,2))+(Vf[:n]*sv).sum((1,2));nrm=nrm+(su**2).sum()+(sv**2).sum()
    A=p/nrm;print(f'== {run.name}  time_s A/A0');[print(f'{a:.4f} {b/A[0]:.6f}') for a,b in zip(t[:n],A)]
