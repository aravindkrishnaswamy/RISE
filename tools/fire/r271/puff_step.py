# r271 (b): capstone puffing method on r264's per-step total heat release (from accepted-stage budgets).
import numpy as np
src=open('puff.py').read();exec('def capstone'+src.split('def capstone')[1].split("runs={'FDS r263'")[0])
d=np.loadtxt('r264_step_hrr.csv',delimiter=',');d=d[np.argsort(d[:,0])];t=d[:,0]+d[:,1];q=d[:,2]/d[:,1]
s=t>=10.466040127;t,q=t[s],q[s]
print('steps',len(t),'mean kW',q.mean()/1e3)
for n in (512,1024,4096,len(t)//2*2):print('capstone method, n =',n,':',round(capstone(t,q,n),3),'Hz')
for L in (2048,4096):
    p,f,P=welch(t,q,L);print('Welch on per-step signal, segment',round(L*np.median(np.diff(t)),1),'s: peak',round(p,3),'Hz')
