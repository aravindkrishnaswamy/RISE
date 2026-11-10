# r271 section 30: floor-layer (k0, k3) plane rms T by scale and core-mean T over a 2.5 s transplant or FDS's original run
# (path containing fds_run: r271_inert from 9.75 s). argv: label=path ...
import numpy as np,sys,types
from pathlib import Path
xa=types.SimpleNamespace();src=Path('/Volumes/SSD/Working/RISE/tools/fire/r271/xp_analyze.py').read_text();exec(src[:src.index('\narms')],xa.__dict__)
N=69;DX=.030593116067411014;kx=np.fft.fftfreq(N)*2*np.pi;KX,KY=np.meshgrid(kx,kx,indexing='ij');K=np.hypot(KX,KY)
SC=[(K<.25*np.pi),(K>=.25*np.pi)&(K<.75*np.pi),(K>=.75*np.pi)]
for arg in sys.argv[1:]:
    lab,p=arg.split('=',1);t,T,W=(xa.load(p) if p.endswith('.bin') else (xa.fds(Path(p),'r271_inert',9.75,12.3) if 'fds_run' in p else xa.fds(Path(p),'xp',0,99)));t=t-t[0]
    print(lab,'layer k0 and k3: plane rms T by scale >=8dx/2.7-8dx/<2.7dx, core-mean T; shape',T.shape)
    for tt in (0,.1,.2,.3,.5,.75,1,1.5,2,2.5):
        i=int(np.argmin(abs(t-tt)));o=f'  t={t[i]:5.3f}'
        for li in (0,1):
            f=T[i,li].reshape(N,N);F=np.abs(np.fft.fft2(f-f.mean()))**2/(N*N)**2
            o+='   '+'/'.join(f'{np.sqrt(F[q].sum()):6.2f}' for q in SC)+f' core {T[i,li][xa.c].mean():6.0f}'
        print(o)
