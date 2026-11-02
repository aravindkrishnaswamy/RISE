# r271 scalar mixing: FDS r263 eddy viscosity nu_t=(VISCOSITY-MOLECULAR VISCOSITY)/DENSITY, ring means by height,
# same bands and rings as scalar_mix.cpp. Run beside tools/fire/r270/meander.py.
import numpy as np
exec(open('meander.py').read().split("xy=(np.arange")[0])
xy=(np.arange(69)-34)*DX;X,Y=np.meshgrid(xy,xy,indexing='ij');RR=np.hypot(X,Y)
bands=[(.06,.16),(.24,.34),(.45,.55),(.70,.80),(.94,1.04),(1.46,1.56)];rings=[0,.1,.2,.3,.5]
print('FDS r263 mean nu_t (m2/s x1e3), full window, by ring');print('   band(m)    r<0.1   0.1-0.2  0.2-0.3  0.3-0.5')
for a,b in bands:
    ks=[k for k in range(106) if a<=(k+.5)*DX<b];acc=np.zeros((69,69));n=0
    for k in ks:
        t,mu=read_slice(RUN/f'{P}_1_{idx[("VISCOSITY",k)]}.sf');_,mm=read_slice(RUN/f'{P}_1_{idx[("MOLECULAR VISCOSITY",k)]}.sf');_,rho=read_slice(RUN/f'{P}_1_{idx[("DENSITY",k)]}.sf')
        s=(t>=START)&(t<=END);acc+=((mu[s]-mm[s])/rho[s]).mean(0);n+=1
    acc/=n;print(f'  {a:.2f}-{b:.2f}  '+'  '.join(f'{1e3*acc[(RR>=r0)&(RR<r1)].mean():6.3f}' for r0,r1 in zip(rings,rings[1:])))
