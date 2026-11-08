# r271 section 28: spectral shares of the unsteady w' over a time window, transplant arms against FDS (as inert_compare.py's
# "w' large / grid share", which needs the 14 s runs). w' = w minus the arm's own time mean over the window, per cell; 2D FFT
# over the full 69x69 plane at layers 16, 32, 49, 65; shares of the w' energy at K < 0.25 pi (>= 8 dx) and K >= 0.75 pi (grid).
# argv: a, b (s since start), then label=path (RISE ckpt_layers dump .bin, or an FDS run dir with xp.fds).
import numpy as np,sys
from pathlib import Path
src=(Path(__file__).parent/'xp_analyze.py').read_text().split('\narms')[0].replace('LK=(0,3,7,16,32,49,65)','LK=(16,32,49,65)')
ns={};exec(src,ns)
a,b=float(sys.argv[1]),float(sys.argv[2]);N=ns['N'];LK=ns['LK']
kx=np.fft.fftfreq(N)*2*np.pi;KX,KY=np.meshgrid(kx,kx,indexing='ij');K=np.hypot(KX,KY)
large=(K>0)&(K<.25*np.pi);grid=K>=.75*np.pi
print(f"window {a}-{b} s: w' energy share at >= 8 dx / grid (>= 0.75 pi), whole plane, and rms w' (m/s) at r < 0.10 m")
print('                       '+''.join(f'{"k"+str(k):>22s}' for k in LK))
for arg in sys.argv[3:]:
    lab,p=arg.split('=',1)
    t,T,W=ns['load'](p) if p.endswith('.bin') else ns['fds'](Path(p),'xp',0,99)
    s=(t>=a)&(t<b);Wp=W[s]-W[s].mean(0);line=f'  {lab:20s} n={s.sum():2d}'
    for i in range(len(LK)):
        F=np.abs(np.fft.fft2(Wp[:,i],axes=(1,2)))**2;tot=F.sum()
        line+=f'   {F[:,large].sum()/tot:.3f}/{F[:,grid].sum()/tot:.3f} {np.sqrt((Wp[:,i][:,ns["c"]]**2).mean()):.3f}'
    print(line)
