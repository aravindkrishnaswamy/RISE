# r271 section 26: unsteadiness about each arm's OWN time mean (not the reference continuation's), over a window, so
# arms that change the mean state (e.g. a scaled layer-0 source) are compared fairly. rms over r < 0.10 m of w and
# theta about the arm's own per-cell mean over [a, b] s since start; and the core-mean T and w.
# argv: a, b, then label=path (FDS run dir, or RISE ckpt_layers dump ending .bin).
import numpy as np,sys,types
from pathlib import Path
xa=types.SimpleNamespace();src=(Path(__file__).parent/'xp_analyze.py').read_text();exec(src[:src.index('\narms')],xa.__dict__)
a,b=float(sys.argv[1]),float(sys.argv[2]);c=xa.c
print(f'window {a}-{b} s: w rms [theta rms] about own mean, r<0.10 m; then core-mean T (K)')
print('                                  '+''.join(f'{"k"+str(k):>14s}' for k in xa.LK))
for arg in sys.argv[3:]:
    lab,p=arg.split('=',1);t,T,W=(xa.load(p) if p.endswith('.bin') else xa.fds(Path(p),'xp' if 'fds_run' not in p else 'r271_inert',0 if 'fds_run' not in p else 9.75,99))
    s=(t>=a)&(t<=b);T,W=T[s],W[s]
    wr=np.sqrt(((W-W.mean(0))**2)[:,:,c].mean((0,2)));tr=np.sqrt(((T-T.mean(0))**2)[:,:,c].mean((0,2)));tm=T.mean(0)[:,c].mean(1)
    print(f'{lab:34s}'+''.join(f'  {x:5.3f}[{y:4.0f}]' for x,y in zip(wr,tr))+'  T: '+' '.join(f'{x:.0f}' for x in tm[:3]))
