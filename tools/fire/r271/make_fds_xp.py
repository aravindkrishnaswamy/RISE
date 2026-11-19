#!/usr/bin/env python3
"""r271 section 25, FDS side of the transplant: FDS r271_inert's input restarted from a given instantaneous state through
FDS's CSVF initial-field files (init.f90 UVW_INIT / TMP_INIT / SPEC_INIT), run for argv[3] s with NOISE=F.
argv[1]: run dir; argv[2]: state file in xp_state.py's layout (T, U, V, W) followed by the methane mass fraction Y
(ckpt_state.cpp writes all five; for an FDS state from xp_state.py pass argv[4] = a ckpt_state file to take Y from).
FDS indices: U(I,J,K) I = face 0..69, J,K = cells 1..; V(I,J,K) J = face; W(I,J,K) K = face 0..106; index 0 of a cell
axis is a ghost (written 0; FDS resets ghosts). The floor plane W(:,:,0) is written 0 as in a fresh start: a nonzero
value there stays in FDS's wall-face W and stalls its pressure iteration (constant velocity error, 10 iterations a step). Tracked species order AIR, METHANE (r271_inert.out)."""
import numpy as np,sys,re
from pathlib import Path
N=69;NZ=106;OUT=Path(sys.argv[1]);OUT.mkdir(parents=True)
a=np.fromfile(sys.argv[2],dtype='<f8');nc=N*N*NZ;nu=(N+1)*N*NZ;nw=N*N*(NZ+1)
T=a[:nc].reshape(NZ,N,N);U=a[nc:nc+nu].reshape(NZ,N,N+1);V=a[nc+nu:nc+2*nu].reshape(NZ,N+1,N);W=a[nc+2*nu:nc+2*nu+nw].reshape(NZ+1,N,N)
rest=a[nc+2*nu+nw:]
Y=(rest if rest.size==nc else np.fromfile(sys.argv[4],dtype='<f8')[-nc:]).reshape(NZ,N,N)
FU=np.zeros((NZ+1,N+1,N+1));FV=np.zeros_like(FU);FW=np.zeros_like(FU)  # [K][J][I]
FU[1:,1:,:]=U;FV[1:,:,1:]=V;FW[:,1:,1:]=W;FW[0]=0  # floor plane: FDS sets its own wall velocities (a fresh start has 0)
with open(OUT/'uvw.csv','w') as f:
    f.write(f'0 {N} 0 {N} 0 {NZ}\n')
    np.savetxt(f,np.stack([FU.ravel(),FV.ravel(),FW.ravel()],1),fmt='%.9e')
with open(OUT/'tmp.csv','w') as f:f.write(f'1 {N} 1 {N} 1 {NZ}\n');np.savetxt(f,T.ravel(),fmt='%.6f')
with open(OUT/'spec.csv','w') as f:f.write(f'1 {N} 1 {N} 1 {NZ}\n');np.savetxt(f,np.stack([1-Y.ravel(),Y.ravel()],1),fmt='%.9e')
src=Path('/Volumes/SSD/RISE/fire_production_calibration/r271_inert_fds/fds_run/r271_inert.fds').read_text()
src=re.sub(r'&TIME T_END=[0-9.eE+-]+ /',f'&TIME T_END={float(sys.argv[3])!r} /',src)
src=src.replace("&MISC SIMULATION_MODE='LES',","&MISC NOISE=F, SIMULATION_MODE='LES',").replace("CHID='r271_inert'","CHID='xp'")
i=src.index('&TAIL');src=src[:i]+"&CSVF UVWFILE='uvw.csv', TMPFILE='tmp.csv', SPECFILE='spec.csv' /\n"+src[i:]
assert 'NOISE=F' in src and "CHID='xp'" in src and '&CSVF' in src
(OUT/'xp.fds').write_text(src)
print(f'{OUT}: T {T.min():.1f}-{T.max():.1f} K, Y_CH4 max {Y.max():.3f}, |w| max {abs(W).max():.2f}')
