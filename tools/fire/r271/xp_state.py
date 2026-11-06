# r271 (section 25): FDS r271_inert's instantaneous 3-D state (TEMPERATURE and the staggered U/V/W face values of every
# cell layer) at the slice frame nearest argv[2] s, written in RISE's MAC layout for xp_driver (build_bt.py kind fds):
# float64 T[z][y][x] (K), U[z][y][x+1 faces], V[z][y+1 faces][x], W[z+1 faces][y][x]. FDS slice arrays are 70x70 with
# index 0 the -x/-y boundary face; U at face i of cell row j is a[i][j+1], V a[i+1][j], W top of layer k a[i+1][j+1].
# W on the floor plane (z face 0) is left 0 here: xp_driver keeps RISE's own floor/burner boundary values.
# argv[1]: output file, argv[2]: time s.
import numpy as np,re,struct,sys
from pathlib import Path
RUN=Path('/Volumes/SSD/RISE/fire_production_calibration/r271_inert_fds/fds_run');P='r271_inert';N=69;NZ=106
sl=[l for l in (RUN/(P+'.fds')).read_text().splitlines() if l.startswith('&SLCF')];DX=.030593116067411014;idx={}
for n,l in enumerate(sl,1):
    q=re.search(r"QUANTITY='([^']+)'",l).group(1);m=re.search(r"PBZ=([0-9.eE+-]+)",l)
    if m and 'SPEC_ID' not in l:idx.setdefault((q,int(round(float(m.group(1))/DX-.5))),n)
def frame(q,k,t):
    with (RUN/f'{P}_1_{idx[(q,k)]}.sf').open('rb') as f:
        for _ in range(4):n=struct.unpack('<i',f.read(4))[0];f.seek(n+4,1)
        best=None
        while True:
            b=f.read(4)
            if not b:break
            tt=struct.unpack('<f',f.read(4))[0];f.read(4)
            n=struct.unpack('<i',f.read(4))[0]
            if best is None or abs(tt-t)<abs(best[0]-t):best=(tt,np.frombuffer(f.read(n),dtype='<f4').reshape((70,70),order='F').astype(np.float64));f.read(4)
            else:f.seek(n+4,1)
    return best
t=float(sys.argv[2]);T=np.zeros((NZ,N,N));U=np.zeros((NZ,N,N+1));V=np.zeros((NZ,N+1,N));W=np.zeros((NZ+1,N,N));times=set()
for k in range(NZ):
    tt,a=frame('TEMPERATURE',k,t);times.add(tt);T[k]=a[1:,1:].T+273.15      # a[x][y] -> [y][x]
    tt,a=frame('U-VELOCITY',k,t);times.add(tt);U[k]=a[:,1:].T                 # faces i 0..69, rows j
    tt,a=frame('V-VELOCITY',k,t);times.add(tt);V[k]=a[1:,:].T                 # cells i, faces j 0..69
    tt,a=frame('W-VELOCITY',k,t);times.add(tt);W[k+1]=a[1:,1:].T
assert len(times)==1,times
np.concatenate([x.ravel() for x in (T,U,V,W)]).astype('<f8').tofile(sys.argv[1])
print(f'FDS frame t={times.pop():.4f} s: T {T.min():.1f}-{T.max():.1f} K, |u|max {abs(U).max():.2f} |w|max {abs(W).max():.2f} m/s; '
      f'mean T over the burner layer 0 {T[0][np.hypot(*np.meshgrid((np.arange(N)-34)*DX,(np.arange(N)-34)*DX))<.12].mean():.0f} K')
