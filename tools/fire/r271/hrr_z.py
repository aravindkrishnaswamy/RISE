# r271: vertical heat-release profile, FDS r263 HRRPUV slices vs RISE budgets/heat_release_height.csv. Run beside tools/fire/r270/fds_flame.py and meander.py.
import numpy as np,csv
exec(open('fds_flame.py').read().split("inst=np.array")[0])
fds=H.mean(0)  # W per cell layer
np.save('fds_hrr_z.npy',fds)
def rise(p):
    r=list(csv.DictReader(open(p)));return np.array([float(x['mean_heat_W']) for x in r])
R4=rise('/Volumes/SSD/RISE/fire_production_calibration/r264_centered_production/run/budgets/heat_release_height.csv')
R7=rise('/Volumes/SSD/RISE/fire_production_calibration/r270_floor_radiation/tier8_eps1/budgets/heat_release_height.csv')
print('totals kW',fds.sum()/1e3,R4.sum()/1e3,R7.sum()/1e3, len(fds),len(R4))
c=[np.cumsum(x)/x.sum() for x in (fds,R4,R7)]
print(' z_top(m)  layer W FDS/r264/r270     cum% FDS/r264/r270')
for k in list(range(0,8))+[9,12,16,20,24,28,32]:
    print(f'{(k+1)*DX:6.3f}  {fds[k]:7.0f} {R4[k]:7.0f} {R7[k]:7.0f}   {100*c[0][k]:5.1f} {100*c[1][k]:5.1f} {100*c[2][k]:5.1f}')
for q in (.25,.5,.75,.9):
    print(q,[round(float(np.interp(q,cc,(np.arange(len(cc))+1)*DX)),3) for cc in c])
