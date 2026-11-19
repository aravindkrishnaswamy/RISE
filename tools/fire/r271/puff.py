# r271 (b): puffing frequency by the capstone method and by Welch spectra, same signals in FDS r263, r264, r270.
import numpy as np,csv
exec(open('fds_flame.py').read().split("NZ=106;planes=[]")[0])
NZ=106;tot=[];col=[];base=[];lay=[]
for k in range(NZ):
    t,a=read_slice(RUN/f'{P}_1_{idx[("HRRPUV",k)]}.sf');s=(t>=START)&(t<=END);a=a[s]*DX**3*1e3
    tot.append(a.sum((1,2)));c=a[:,33:36,33:36].sum((1,2));col.append(c)
    if k<10:base.append(c)
tF=t[s];FDS={'total':np.sum(tot,0),'column':np.sum(col,0),'column_base':np.sum(base,0)}
def rise(name):
    d=np.fromfile(name+'_hrr.bin').reshape(235,109)
    tt=np.array([float(r['simulation_time_s']) for r in csv.DictReader(open({'r264':'/Volumes/SSD/RISE/fire_production_calibration/r264_centered_production/run','r270':'/Volumes/SSD/RISE/fire_production_calibration/r270_floor_radiation/tier8_eps1'}[name]+'/frame_times.csv'))])
    return tt,{'total':d[:,0],'column':d[:,1],'column_base':d[:,2]}
def capstone(t,x,n=512):
    u=np.linspace(t[0],t[-1],n);y=np.interp(u,t,x);i=np.arange(n);A=np.vstack([i,np.ones(n)]).T
    y=y-A@np.linalg.lstsq(A,y,rcond=None)[0];w=0.5-0.5*np.cos(2*np.pi*i/(n-1))
    P=np.abs(np.fft.rfft(y*w))**2;f=np.arange(len(P))*(n-1)/(n*(t[-1]-t[0]));b=1+np.argmax(P[1:]);return f[b]
def welch(t,x,L=64):
    dt=np.median(np.diff(t));u=np.arange(t[0],t[-1],dt);x=np.interp(u,t,x);x=x-x.mean();w=np.hanning(L);P=0;c=0
    for s0 in range(0,len(x)-L+1,L//2):P=P+np.abs(np.fft.rfft(x[s0:s0+L]*w))**2;c+=1
    f=np.fft.rfftfreq(L,dt);P=P/c;m=(f>0.4)&(f<6);return f[m][np.argmax(P[m])],f,P
runs={'FDS r263':(tF,FDS)};runs.update({n:rise(n) for n in ('r264','r270')})
print('expected 1.5/sqrt(D) =',round(1.5/np.sqrt(0.3),3),'Hz (D from case)')
print('signal        run        capstone-method(Hz)  Welch 4s peak  Welch 8s peak  power share 2-3.5 Hz')
spec={}
for sig in ('total','column','column_base'):
    for run,(t,d) in runs.items():
        x=d[sig];p4,f,P=welch(t,x,64);p8,f8,P8=welch(t,x,128);m=(f8>=2)&(f8<3.5);share=P8[m].sum()/P8[(f8>0.2)&(f8<8)].sum()
        print(f'{sig:12s}  {run:9s}  {capstone(t,x):8.2f}             {p4:5.2f}          {p8:5.2f}          {share:.2f}')
        spec[(sig,run)]=(f8,P8)
np.save('puff_spec.npy',{f'{a}|{b}':v for (a,b),v in spec.items()},allow_pickle=True)
