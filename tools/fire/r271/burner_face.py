# r271 burner face (read-only): what the lowest cells above the burner hold in each code. RISE r270 frames vs FDS r263.
# argv[1]: dir with near_planes.cpp output (reaction option) r270q.bin (235 frames, 18 layers).
# 1. Radial profiles in the first layers: mean/rms T, mean/rms w, mean/rms u_r, mean/rms q.
# 2. Over the burner disk r<0.15 m, the w fluctuation split into its disk mean (the coherent pulse), the azimuthal-mean
#    remainder (axisymmetric rings) and the rest (non-axisymmetric, cell to cell); and the disk-mean w by frequency band.
# 3. Vertical growth of the disk-mean pulse and its lag behind the disk heat release, layer by layer.
# Caution (section 16): FDS CELL_CENTERED velocity slices are raw staggered face values (U on the +x face, V on +y, W on
# the top face; dump.f90 GAS_PHASE_OUTPUT), while RISE frames hold cell averages. Sections 1-3 velocities therefore compare
# different positions and filters; first_layer.cpp gives RISE's face values for the like-for-like check. T, q, density,
# mass fractions and divergence are true cell values in both.
import numpy as np,sys
from pathlib import Path
here=Path(__file__).parent
exec(open(here/'forcing_damping.py').read().split('\nruns={')[0])        # riseq, fdsq, band, constants, grid
src={'r270':riseq('r270q',235),'FDS r263':fdsq()}
rad=np.arange(0,.31,DX);shells=[np.abs(RR-a)<DX/2 for a in rad];disk=RR<.15
rings=[np.isclose(RR,r) & disk for r in np.unique(np.round(RR[disk],9))]
def urad(u,v):return (u*X+v*Y)/np.where(RR>0,RR,1.)
print('1. Radial profiles (shell mean of time mean / time rms): T (K), w (m/s), u_r (m/s), q (MW/m3)')
for k in (0,1,2,4):
    print(f'   z={(k+.5)*DX:.3f} m   r(m):  '+'  '.join(f'{a:5.3f}' for a in rad))
    for n,(T,u,v,w,q) in src.items():
        ur=urad(u[:,k],v[:,k])
        for lab,f,s in (('T',T[:,k],1),('w',w[:,k],1),('u_r',ur,1),('q',q[:,k],1e-6)):
            m=f.mean(0)*s;r=f.std(0)*s
            print(f'   {n:9s} {lab:3s} mean '+'  '.join(f'{m[sh].mean():5.2f}' if lab!='T' else f'{m[sh].mean():5.0f}' for sh in shells))
            print(f'   {"":9s} {lab:3s} rms  '+'  '.join(f'{r[sh].mean():5.2f}' if lab!='T' else f'{r[sh].mean():5.0f}' for sh in shells))
print('\n2. w fluctuation over the burner disk r<0.15 m: rms of total / disk-mean pulse / axisymmetric rest / non-axisymmetric (m/s);')
print('   disk-mean w in bands <2, 2-3.5, 3.5-8 Hz (m/s)')
for n,(T,u,v,w,q) in src.items():
    for k in (0,1,2,4,6,9):
        wp=w[:,k]-w[:,k].mean(0);tot=np.sqrt((wp[:,disk]**2).mean())
        dm=wp[:,disk].mean(1);ax=np.zeros_like(wp)
        for g in rings:ax[:,g]=wp[:,g].mean(1)[:,None]
        axr=ax-dm[:,None,None];rest=wp-ax
        print(f'   {n:9s} z={(k+.5)*DX:.3f}  {tot:5.3f}  {dm.std():5.3f}  {np.sqrt((axr[:,disk]**2).mean()):5.3f}  {np.sqrt((rest[:,disk]**2).mean()):5.3f}'
              f'   {band(dm,0,2):5.3f} {band(dm,2,3.5):5.3f} {band(dm,3.5,8):5.3f}')
print('\n3. Disk-mean pulse by layer: rms w (m/s), corr with the disk heat release below that layer at lags -2..+2 frames')
print('   (positive: w after q), and with the disk-mean T in that layer')
def lc(x,y,l):
    x=x-x.mean();y=y-y.mean();a=x[max(0,-l):len(x)-max(0,l)];b=y[max(0,l):len(y)-max(0,-l)];return float((a*b).sum()/np.sqrt((x**2).sum()*(y**2).sum()))
for n,(T,u,v,w,q) in src.items():
    for k in (0,1,2,4,6,9):
        dm=w[:,k][:,disk].mean(1);Q=q[:,:k+1][:,:,disk].sum((1,2))*V;Tm=T[:,k][:,disk].mean(1)
        print(f'   {n:9s} z={(k+.5)*DX:.3f}  {dm.std():5.3f}  q: '+' '.join(f'{lc(Q,dm,l):5.2f}' for l in range(-2,3))+'   T: '+' '.join(f'{lc(Tm,dm,l):5.2f}' for l in range(-2,3)))
# 4. FDS composition in the first layers, to set beside first_layer.cpp on the RISE checkpoints: mean T (K), molecular weight
#    M = rho R T / p (kg/kmol) from the DENSITY and TEMPERATURE slices, the METHANE and OXYGEN mass-fraction slices,
#    the DIVERGENCE slice (1/s) and HRRPUV (MW/m3).
print('\n4. FDS r263 first layers by radius: T, M, Y_CH4, Y_O2, div, q (RISE: first_layer.cpp)')
sl=[l for l in (RUN/(P+'.fds')).read_text().splitlines() if l.startswith('&SLCF')];sidx={}
for n,l in enumerate(sl,1):
    q=re.search(r"QUANTITY='([^']+)'",l).group(1);sp=re.search(r"SPEC_ID='([^']+)'",l);m=re.search(r"PBZ=([0-9.eE+-]+)",l)
    if m:sidx.setdefault((q+(':'+sp.group(1) if sp else ''),int(round(float(m.group(1))/DX-.5))),n)
print('   r(m)         '+' '.join(f'{a:6.3f}' for a in rad))
for k in range(4):
    T=read_slice(RUN/f'{P}_1_{sidx[("TEMPERATURE",k)]}.sf')+273.15;rho=read_slice(RUN/f'{P}_1_{sidx[("DENSITY",k)]}.sf')
    rows=(('T',T.mean(0),'6.0f'),('M',(rho*8314.46261815324*T/101325.).mean(0),'6.2f'),
          ('Y_CH4',read_slice(RUN/f'{P}_1_{sidx[("MASS FRACTION:METHANE",k)]}.sf').mean(0),'6.3f'),
          ('Y_O2',read_slice(RUN/f'{P}_1_{sidx[("MASS FRACTION:OXYGEN",k)]}.sf').mean(0),'6.3f'),
          ('div',read_slice(RUN/f'{P}_1_{sidx[("DIVERGENCE",k)]}.sf').mean(0),'6.2f'),
          ('q',read_slice(RUN/f'{P}_1_{sidx[("HRRPUV",k)]}.sf').mean(0)*1e-3,'6.2f'))
    for lab,f,fmt in rows:print(f'   layer {k} {lab:5s} '+' '.join(format(f[s].mean(),fmt) for s in shells))
