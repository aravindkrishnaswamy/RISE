exec(open('inflow.py').read().split("z=(np.arange")[0])
z=(np.arange(NZ)+.5)*DX
bands=[(0,.15),(.15,.2),(.2,.3),(.3,.4),(.4,.5),(.5,.7),(.7,.9),(.9,1.04)]
print('Mean temperature rise (K) vs radius, FDS/RISE')
print(' z_m   '+' '.join(f'{a}-{b}'.ljust(11) for a,b in bands))
for k in [0,1,2,3,5,9]:
 print(f'{z[k]:5.3f} '+' '.join(f'{F[3][k][(RR>=a)&(RR<b)].mean()-300:5.1f}/{R[0][k][(RR>=a)&(RR<b)].mean()-300:5.1f}' for a,b in bands))
print('\nMean vertical velocity (m/s) vs radius, FDS/RISE')
for k in [0,1,3,5,9,16]:
 print(f'{z[k]:5.3f} '+' '.join(f'{F[2][k][(RR>=a)&(RR<b)].mean():+.3f}/{R[3][k][(RR>=a)&(RR<b)].mean():+.3f}' for a,b in bands))
