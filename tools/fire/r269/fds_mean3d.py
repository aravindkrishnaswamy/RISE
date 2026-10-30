# FDS no-slip time means of U,V,W,T,DENSITY,PRESSURE at all 106 heights (cell centred), same window as RISE frames
import numpy as np
exec(open('meander.py').read().split("xy=(np.arange")[0].replace("R=np.fromfile('r251_planes.bin',dtype='<f4').reshape(235,8,4,69,69)","").replace("R=np.transpose(R,(0,1,2,4,3))",""))
Q=['U-VELOCITY','V-VELOCITY','W-VELOCITY','TEMPERATURE','DENSITY','PRESSURE']
out=np.zeros((len(Q)+3,106,69,69))
for k in range(106):
 fs={}
 for qi,q in enumerate(Q):
  t,a=read_slice(RUN/f'{P}_1_{idx[(q,k)]}.sf');s=(t>=START)&(t<=END);fs[q]=a[s]
  out[qi,k]=fs[q].mean(0)
 for c,q in enumerate(Q[:3]):out[6+c,k]=(fs['DENSITY']*fs[q]).mean(0)
out[3]+=273.15
np.save('fds_mean3d.npy',out)  # [U,V,W,T,rho,p,rhoU,rhoV,rhoW][z][x][y]
print('done',out.shape,len(t[s]))
