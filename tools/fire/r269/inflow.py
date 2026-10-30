# Ambient inflow diagnosis: r264 vs FDS no-slip, full-window time means
import numpy as np
DX=.030593116067411014;N=69;NZ=106
F=np.load('fds_mean3d.npy')
R=np.fromfile('r264_mean3d.bin').reshape(7,NZ,N,N).transpose(0,1,3,2)  # -> [c][z][x][y]
xy=(np.arange(N)-34)*DX;X,Y=np.meshgrid(xy,xy,indexing='ij');RR=np.hypot(X,Y)
rhoR=352.6*R[4:7]           # rho*u approx with rho=352.6/T (air)
rhoF=F[6:9]
uF,vF=F[0],F[1];uR,vR=R[1],R[2]
def side_in(ru,rv):
 # inward mass flux through the 4 side faces using outermost cell column, kg/s per height cell
 return (ru[:,0,:].sum(1)-ru[:,-1,:].sum(1)+rv[:,:,0].sum(1)-rv[:,:,-1].sum(1))*DX*DX
def radial(u,v,r0,r1):
 m=(RR>=r0)&(RR<r1);ur=(u*X+v*Y)/np.maximum(RR,1e-9);return ur[:,m].mean(1)
z=(np.arange(NZ)+.5)*DX
sF=side_in(rhoF[0],rhoF[1]);sR=side_in(rhoR[0],rhoR[1])
pF=rhoF[2].sum((1,2))*DX*DX;pR=rhoR[2].sum((1,2))*DX*DX
print('Side-boundary inflow (kg/s, cumulative from floor) and full-plane upward flux')
print(' z_m   FDS_side_cum  RISE_side_cum   FDS_plane  RISE_plane')
for k in [0,4,9,16,24,32,49,65,81,98,105]:
 print(f'{z[k]:5.2f}  {sF[:k+1].sum():9.3f}  {sR[:k+1].sum():9.3f}    {pF[k]:8.3f}  {pR[k]:8.3f}')
print(f'total side inflow: FDS {sF.sum():.3f}  RISE {sR.sum():.3f} kg/s')
print('\nMean radial velocity u_r (m/s, negative = inward) by radius band')
bands=[(0.3,0.5),(0.5,0.7),(0.7,0.9),(0.9,1.04)]
print(' z_m  '+'  '.join(f'{a}-{b} FDS/RISE ' for a,b in bands))
for k in [2,9,16,24,32,49,65,81,98]:
 print(f'{z[k]:5.2f} '+'  '.join(f'{radial(uF,vF,a,b)[k]:+.3f}/{radial(uR,vR,a,b)[k]:+.3f}' for a,b in bands))
print('\nMean vertical velocity w outside the plume (m/s) by radius band')
for k in [9,16,32,49,65,98]:
 print(f'{z[k]:5.2f} '+'  '.join(f'{F[2][k][(RR>=a)&(RR<b)].mean():+.3f}/{R[3][k][(RR>=a)&(RR<b)].mean():+.3f}' for a,b in bands))
print('\nFDS mean pressure perturbation (Pa) vs radius at selected heights, relative to the side-boundary mean')
for k in [2,9,32,65,98]:
 p=F[5][k];edge=np.concatenate([p[0],p[-1],p[:,0],p[:,-1]]).mean()
 print(f'{z[k]:5.2f} '+' '.join(f'r<{b}:{p[(RR>=a)&(RR<b)].mean()-edge:+.3f}' for a,b in [(0,.15),(.15,.3),(.3,.5),(.5,.7),(.7,.9),(.9,1.04)]))
# floor-level inflow: lowest cell layer radial
print('\nNear-floor (z<0.1 m) inward flux through r=0.6 m cylinder per unit height (kg/s/m):')
for k in range(4):
 m=(RR>=.55)&(RR<.65);fF=-(rhoF[0][k]*X+rhoF[1][k]*Y)/RR;fR=-(rhoR[0][k]*X+rhoR[1][k]*Y)/RR
 print(f' z {z[k]:.3f}: FDS {fF[m].mean()*2*np.pi*.6:.3f}  RISE {fR[m].mean()*2*np.pi*.6:.3f}')
