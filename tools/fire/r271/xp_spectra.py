# r271 section 25: from the identical FDS state at 9.751 s, how does each code's dynamics redistribute w' energy by
# horizontal scale? Horizontal spectra of w (minus the plane mean) at layers 3, 7, 16, 32 on the full 69x69 plane, energy in
# bands >=8 dx, 4-8 dx, 2.7-4 dx, grid (>= 0.75 pi), relative to t=0, for RISE dynamics (xp_driver checkpoints) and FDS
# dynamics (fdsxp_from_fds slices) at matched times. argv[1]: RISE-dynamics/FDS-state dump.
import numpy as np,sys
from pathlib import Path
exec(open(Path(__file__).with_name('xp_analyze.py')).read().split('S=Path(')[0].replace('LK=(0,3,7,16,32,49,65)','LK=(3,7,16,32)'))
tR,_,WR=load(sys.argv[1]);tF,_,WF=fds(Path('/Volumes/SSD/RISE/fire_production_calibration/r271_small_scale/fdsxp_from_fds'),'xp',0,9)
kx=np.fft.fftfreq(N)*2*np.pi;KX,KY=np.meshgrid(kx,kx,indexing='ij');K=np.hypot(KX,KY)
bands=[(K>0)&(K<.25*np.pi),(K>=.25*np.pi)&(K<.5*np.pi),(K>=.5*np.pi)&(K<.75*np.pi),K>=.75*np.pi]
def E(p):F=np.abs(np.fft.fft2(p-p.mean()))**2;return np.array([F[b].sum() for b in bands])
print('w energy by band relative to t=0 (same initial field): >=8dx 4-8dx 2.7-4dx grid')
for i,k in enumerate(LK):
    print(f'  layer {k}')
    E0R=E(WR[0,i]);E0F=E(WF[0,i])
    print('     t(s)    RISE dynamics                 FDS dynamics          (t=0 check: RISE/FDS band energy '+' '.join(f'{x:.2f}' for x in E0R/E0F)+')')
    for tt in (0.05,0.1,0.2,0.3,0.5,0.75,1.0):
        jr=np.argmin(abs(tR-tt));jf=np.argmin(abs(tF-tt))
        if abs(tR[jr]-tt)>.03 or abs(tF[jf]-tt)>.04:continue
        print(f'    {tt:4.2f}  '+' '.join(f'{x:5.2f}' for x in E(WR[jr,i])/E0R)+'      '+' '.join(f'{x:5.2f}' for x in E(WF[jf,i])/E0F))
