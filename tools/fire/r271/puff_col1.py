# r271 (b): capstone method on the single centre-cell column (the capstone's own signal), frames, all three runs.
import numpy as np
src=open('puff.py').read();exec(src.split("runs={'FDS r263'")[0])
c1=[]
for k in range(NZ):
    t,a=read_slice(RUN/f'{P}_1_{idx[("HRRPUV",k)]}.sf');s=(t>=START)&(t<=END);c1.append(a[s][:,34,34]*DX**3*1e3)
x=np.sum(c1,0);print('FDS r263 single column: capstone',round(capstone(tF,x),2),'Welch 8 s',welch(tF,x,128)[0])
for n in ('r264','r270'):
    t,_=rise(n);x=np.fromfile(n+'_col1.bin');print(n,'single column: capstone',round(capstone(t,x),2),'Welch 8 s',welch(t,x,128)[0])
