exec(open('inflow.py').read().split("z=(np.arange")[0])
z=(np.arange(NZ)+.5)*DX
print('Radial velocity u_r (m/s) near the floor, FDS/RISE, by radius band')
bands=[(0.15,0.25),(0.25,0.35),(0.35,0.5),(0.5,0.7),(0.7,0.9)]
print(' z_m   '+'  '.join(f'{a}-{b}'.ljust(13) for a,b in bands))
for k in range(0,12):
 print(f'{z[k]:5.3f} '+'  '.join(f'{radial(uF,vF,a,b)[k]:+.3f}/{radial(uR,vR,a,b)[k]:+.3f}' for a,b in bands))
print('\nInward mass flux through cylinders (kg/s), cumulative from floor to z')
for rc in [0.2,0.3,0.5,0.8]:
 m=(RR>=rc-DX/2)&(RR<rc+DX/2)
 fF=np.nan_to_num(-(rhoF[0]*X+rhoF[1]*Y)/RR)[:,m].mean(1)*2*np.pi*rc*DX
 fR=np.nan_to_num(-(rhoR[0]*X+rhoR[1]*Y)/RR)[:,m].mean(1)*2*np.pi*rc*DX
 print(f' r={rc}: '+'  '.join(f'z<{z[k]+DX/2:.2f}: {fF[:k+1].sum():.3f}/{fR[:k+1].sum():.3f}' for k in [3,9,16,32]))
print('\nMean temperature rise (K) near burner edge r=0.15-0.25, FDS/RISE')
for k in [0,2,4,9,16]:
 m=(RR>=.15)&(RR<.25);print(f' z {z[k]:.3f}: {F[3][k][m].mean()-300:.0f}/{R[0][k][m].mean()-300:.0f}')
