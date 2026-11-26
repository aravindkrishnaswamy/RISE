exec(open('inflow.py').read().split("z=(np.arange")[0])
z=(np.arange(NZ)+.5)*DX;cp=1005
print('Mean-flow upward sensible heat flux and mass flux in the annulus outside the flame, FDS/RISE')
for k in [3,5,9,16]:
 for a,b in [(0.2,0.5),(0.5,1.04)]:
  m=(RR>=a)&(RR<b)
  hF=(rhoF[2][k]*cp*(F[3][k]-300))[m].sum()*DX*DX/1e3;hR=(rhoR[2][k]*cp*(R[0][k]-300))[m].sum()*DX*DX/1e3
  mF=rhoF[2][k][m].sum()*DX*DX;mR=rhoR[2][k][m].sum()*DX*DX
  print(f' z {z[k]:.2f} r {a}-{b}: heat {hF:.3f}/{hR:.3f} kW   mass up {mF:.4f}/{mR:.4f} kg/s')
