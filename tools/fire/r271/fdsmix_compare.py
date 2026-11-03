# r271 FDS mixing-time A/B (read-only): the arm's frames 73-136 against r270's three 4 s windows (w0 = 0-63, same
# window 73-136, w2 = 137-200) and FDS r263, on the section 12 and 13 near-field measures.
# argv[1]: dir with near_planes.cpp outputs (reaction option) r270q.bin (235 frames) and the arm's file (64 frames);
# argv[2]/argv[3] (optional) = arm file stem and label, default fmq / 'fds mixing'.
import numpy as np,sys
from pathlib import Path
here=Path(__file__).parent
exec(open(here/'forcing_damping.py').read().split('\nruns={')[0])       # riseq, analyse, fdsq, band, ring, KS2
exec('def series'+open(here/'expansion_cv.py').read().split('def series')[1].split('def lagcorr')[0])
ARM=sys.argv[2] if len(sys.argv)>2 else 'fmq';LAB=sys.argv[3] if len(sys.argv)>3 else 'fds mixing'
fields=lambda T,u,v,w,q:dict(u=u,v=v,w=w,q=q,div=None)
src={'r270 w0':riseq('r270q',235,slice(0,64)),'r270 73-136':riseq('r270q',235,slice(73,137)),
     'r270 w2':riseq('r270q',235,slice(137,201)),LAB:riseq(ARM,64),'FDS r263':fdsq()}
runs={n:analyse(*a) for n,a in src.items()};names=list(runs)
print('1. Heat release below 0.31 m, plane total: mean kW, rms %, 2-3.5 Hz %, 3.5-8 Hz %')
for n in names:print(f'   {n:12s}  '+'  '.join(f'{x:6.2f}' for x in runs[n]['base']))
print('\n2. Control volume r<R, 0<z<z_top: heat release rms/mean (%), 2-3.5 Hz % and 3.5-8 Hz % of the mean')
print('   R    z_top '+''.join(f'{n:>22s}' for n in names))
for R in (.15,.20,.30):
    for ktop in (4,7,10):
        row=[]
        for n,a in src.items():
            S=series(fields(*a),R,ktop)[1];m=S.mean()
            row.append(f'{100*S.std()/m:5.2f} {100*band(S,2,3.5)/m:5.2f} {100*band(S,3.5,8)/m:5.2f}')
        print(f'   {R:.2f} {ktop*DX:5.2f}  '+''.join(f'{r:>22s}' for r in row))
print('\n3. Burner-edge ring 0.06-0.12 m: rms T (K), rms q (MW/m3), rms w (m/s), corr(w,T), production/TKE (1/s)')
print('   z(m)  '+''.join(f'{n:>30s}' for n in names))
for k in KS2:
    print(f'   {(k+.5)*DX:4.2f}  '+''.join('{:5.0f} {:5.2f} {:4.2f} {:5.2f} {:5.1f}'.format(*(runs[n]['ring'][k][i] for i in (0,1,2,3,7))).rjust(30) for n in names))
print('\n4. Shear-layer peak rms w (max over radius of the ring mean, m/s)')
for k in KS2:print(f'   {(k+.5)*DX:4.2f}  '+''.join(f"{runs[n]['peak'][k][2]:30.2f}" for n in names))
