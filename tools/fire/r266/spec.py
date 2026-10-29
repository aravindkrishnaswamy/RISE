exec(open('meander.py').read().split("print('z  h")[0])
def psd(x,dt=0.0625):
 x=x-x.mean();n=len(x);
 # Welch with 64-sample segments (4 s), 50% overlap, Hann
 L=64;st=32;win=np.hanning(L);P=0;c=0
 for s in range(0,n-L+1,st):
  seg=x[s:s+L]*win;P=P+np.abs(np.fft.rfft(seg))**2;c+=1
 P=P/c/(win**2).sum()*dt*2;f=np.fft.rfftfreq(L,dt);return f,P
bands=[(0.2,1),(1,2),(2,3),(3,4),(4,5),(5,6),(6,8)]
for k,z in enumerate(Z[:5]):
 tF,TF=read_slice(RUN/f'{P}_1_{idx[("TEMPERATURE",z)]}.sf');s=(tF>=START)&(tF<=END);TF=TF[s]+273.15-300
 tW,WF=read_slice(RUN/f'{P}_1_{idx[("W-VELOCITY",z)]}.sf');WF=WF[(tW>=START)&(tW<=END)]
 thR=R[:,k,0]-300.0;wR=R[:,k,3]
 # plane-integrated heat-weighted quantity: ring-mean m0 signal and centerline
 for name,sF,sR in [('cl_theta',TF[:,34,34],thR[:,34,34]),('cl_w',WF[:,34,34],wR[:,34,34]),('m0_ring_theta',TF[:,RR<0.25].mean(1),thR[:,RR<0.25].mean(1))]:
  f,pF=psd(sF);f,pR=psd(sR)
  line=f'{(z+.5)*DX:.2f} {name:14s} peak FDS {f[1:][np.argmax(pF[1:])]:.2f}Hz RISE {f[1:][np.argmax(pR[1:])]:.2f}Hz | band power RISE/FDS: '
  for a,b in bands:
   m=(f>=a)&(f<b);line+=f'{a}-{b}:{pR[m].sum()/pF[m].sum():.2f} '
  print(line)
