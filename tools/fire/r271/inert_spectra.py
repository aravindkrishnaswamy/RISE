# r271 non-reacting plume (section 22, read-only): is the near-source deficit a missing puffing mode? Frequency spectra of
# the centre (3x3 cells) w at the lower five dump heights, 6-14 s (Hann window), FDS (cell averages) against RISE arms.
# argv[1]: dir holding <arm>_planes.bin and <arm>_times.txt (inert_compare.py inputs); argv[2:]: arm names.
import numpy as np,sys,os
from pathlib import Path
os.environ['R271_FDS_CELL_AVERAGE']='1'
exec(open(Path(__file__).with_name('inert_compare.py')).read().split('rows={}')[0])
def spec(t,w,label):
    s=(t>=6)&(t<=14);t=t[s];x=w[s];dt=np.median(np.diff(t));x=(x-x.mean())*np.hanning(len(x))
    f=np.fft.rfftfreq(len(x),dt);P=np.abs(np.fft.rfft(x))**2;b=f>0.3;tot=P[b].sum()
    sh=[P[(f>=a)&(f<c)].sum()/tot for a,c in ((0.3,1),(1,2),(2,4),(4,8))]
    print(f'    {label:22s} peak {f[b][np.argmax(P[b])]:4.2f} Hz  shares 0.3-1/1-2/2-4/4-8 Hz: '+' '.join(f'{v:.2f}' for v in sh))
D=np.sqrt(69*DX*DX*4/np.pi);print(f'Centre w spectra, 6-14 s. Burner D_eq {D:.3f} m, puffing estimate 1.5/sqrt(D) = {1.5/np.sqrt(D):.2f} Hz')
for i,k in enumerate(ZK[:5]):
    print(f'  z={(k+.5)*DX:4.2f} m');spec(tF,fds_plane(k)[3][:,c3].mean(1),'FDS')
    for arm in ARMS:
        t=np.loadtxt(W/f'{arm}_times.txt')
        a=np.fromfile(W/f'{arm}_planes.bin',dtype='<f4').reshape(len(t),8,4,N,N)[:,i,2].transpose(0,2,1)
        spec(t,a[:,c3].mean(1),arm)
