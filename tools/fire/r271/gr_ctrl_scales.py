# r271 section 30: rms by horizontal scale of a twin CONTROL run's T, u, w per layer band over time (loaders from gr_analyze.py).
# argv: base state, label=ctrl ...   env GR_BANDS.
import numpy as np,sys,os
from pathlib import Path
src=Path('/Volumes/SSD/Working/RISE/tools/fire/r271/gr_analyze.py').read_text().split('\nbase=state(')[0];exec(src)
base=state(sys.argv[1])
def sc(f):  # [z][x][y] -> rms by scale (mean removed per layer)
    F=np.abs(np.fft.fft2(f-f.mean((1,2),keepdims=True),axes=(1,2)))**2/(N*N)**2
    return [np.sqrt(F[:,q].sum()/f.shape[0]) for q in SC]
names=['T','u','v','w']
for arg in sys.argv[2:]:
    lab,p=arg.split('=');t,C=load(p)
    for a,b in BANDS:
        print(f'{lab} layers {a}-{b}: rms by scale  >=8dx / 2.7-8dx / <2.7dx   (T K; u,w m/s)')
        rows=[(0.0,base)]+[(t[i],C[i]) for i in range(len(t)) if min(abs(t[i]-x) for x in (.025,.05,.1,.15,.2))<6e-3]
        for tt,c in rows:
            print(f'  t={tt:5.3f} '+'  '.join(f'{names[j]} '+'/'.join(f'{x:7.3f}' for x in sc(c[a:b+1,j])) for j in (0,1,3)))
