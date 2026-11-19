# r271 (a): grid-scale content of horizontal velocity/temperature planes, FDS r263 (cell-centred slices) vs RISE frames.
import numpy as np,os
exec(open('meander.py').read().split("xy=(np.arange")[0])
Rs={n:np.transpose(np.fromfile(n+'_planes.bin',dtype='<f4').reshape(235,8,4,69,69),(0,1,2,4,3)) for n in ('r251','r264','r270')}
kx=np.fft.fftfreq(69)*2*np.pi;KX,KY=np.meshgrid(kx,kx,indexing='ij');K=np.hypot(KX,KY)
def spec_share(planes):
    p=planes-planes.mean(0);E=(np.abs(np.fft.fft2(p))**2).mean(0);tot=E[K>0].sum()
    return E[K>=0.75*np.pi].sum()/tot, E[(K>0)&(K<0.25*np.pi)].sum()/tot
def checker(planes):
    p=planes-planes.mean(0);c=p[:,1:-1,1:-1]-0.25*(p[:,2:,1:-1]+p[:,:-2,1:-1]+p[:,1:-1,2:]+p[:,1:-1,:-2]);return np.sqrt((c**2).mean()/(p**2).mean())
print('z(m)  qty  run   share kD>=0.75pi   share kD<0.25pi   Laplacian/rms index')
for k,z in enumerate(Z[:6]):
    t,W=read_slice(RUN/f'{P}_1_{idx[("W-VELOCITY",z)]}.sf');W=W[(t>=START)&(t<=END)]
    t,T=read_slice(RUN/f'{P}_1_{idx[("TEMPERATURE",z)]}.sf');T=T[(t>=START)&(t<=END)]
    for q,F_,c in (('w',W,3),('T',T,0)):
        for run,pl in [('FDS',F_)]+[(n,R[:,k,c]) for n,R in Rs.items()]:
            hi,lo=spec_share(pl);print(f'{(z+.5)*DX:4.2f}  {q}  {run:5s}  {hi:8.4f}          {lo:8.3f}          {checker(pl):.3f}')
