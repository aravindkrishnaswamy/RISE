# r271 (b): puffing strength, band-limited rms (2-3.5 Hz) and total rms of heat-release signals, as % of the mean.
import numpy as np
exec(open('puff.py').read().split("runs={'FDS r263'")[0])
runs={'FDS r263':(tF,FDS)};runs.update({n:rise(n) for n in ('r264','r270')})
def band_rms(t,x,a,b):
    dt=np.median(np.diff(t));u=np.arange(t[0],t[-1],dt);y=np.interp(u,t,x);y=y-y.mean()
    Y=np.fft.rfft(y);f=np.fft.rfftfreq(len(y),dt);Y[(f<a)|(f>=b)]=0;return np.sqrt(np.mean(np.fft.irfft(Y,len(y))**2))
print('signal        run       mean(kW)  rms all %  rms 2-3.5 Hz %  rms 0.2-2 Hz %  rms 3.5-8 Hz %')
for sig in ('total','column','column_base'):
    for run,(t,d) in runs.items():
        x=d[sig];m=x.mean()
        print(f'{sig:12s}  {run:9s} {m/1e3:7.2f}   {100*x.std()/m:6.1f}    {100*band_rms(t,x,2,3.5)/m:6.1f}         {100*band_rms(t,x,.2,2)/m:6.1f}          {100*band_rms(t,x,3.5,8)/m:6.1f}')
