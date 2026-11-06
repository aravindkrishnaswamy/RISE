# r271 section 27: is RISE's hot-gas scalar change weak against a common advection reference? With SGS off and no
# source above the floor, each code's T change over a two-step window should follow -2 dt u.grad(T) on its own midpoint
# state. Reference: cell-centred u (face mean) times central dT/dx (one-sided at the domain edges), the same operator for
# both codes. Per T bin (midpoint T, layers 3-65): slope of the code's actual change on its own reference and the
# correlation. RISE: ckpt_state dumps at steps 2, 3, 4. FDS: slices (T frames lag one step: frame i is T at step i-1),
# change over T frames 2->4 (steps 1->3) with the midpoint T (frame 3) and velocity (frame 2) at step 2.
# argv: RISE step-2, step-3, step-4 states, FDS run dir.
import numpy as np,re,struct,sys
from pathlib import Path
N=69;NZ=106;DX=.030593116067411014;DT=.0016462659696117043
def split(a):
    nc=N*N*NZ;nu=(N+1)*N*NZ;nw=N*N*(NZ+1)
    return (a[:nc].reshape(NZ,N,N),a[nc:nc+nu].reshape(NZ,N,N+1),a[nc+nu:nc+2*nu].reshape(NZ,N+1,N),a[nc+2*nu:nc+2*nu+nw].reshape(NZ+1,N,N))
R2,R3,R4=(split(np.fromfile(p,'<f8')) for p in sys.argv[1:4])
run=Path(sys.argv[4]);sl=[l for l in (run/'xp.fds').read_text().splitlines() if l.startswith('&SLCF')];idx={}
for n,l in enumerate(sl,1):
    q=re.search(r"QUANTITY='([^']+)'",l).group(1);m=re.search(r"PBZ=([0-9.eE+-]+)",l)
    if m and 'SPEC_ID' not in l:idx.setdefault((q,int(round(float(m.group(1))/DX-.5))),n)
def frames(q,k):
    out=[]
    with (run/f'xp_1_{idx[(q,k)]}.sf').open('rb') as f:
        for _ in range(4):n=struct.unpack('<i',f.read(4))[0];f.seek(n+4,1)
        while True:
            h=f.read(4)
            if len(h)<4:break
            f.read(8);n=struct.unpack('<i',f.read(4))[0];d=f.read(n);f.read(4)
            if len(d)<n:break
            out.append(np.frombuffer(d,dtype='<f4').reshape((70,70),order='F').astype(np.float64))
    return out
FT=np.array([[fr[1:,1:].T+273.15 for fr in frames('TEMPERATURE',k)[:5]] for k in range(NZ)]).transpose(1,0,2,3)  # [frame][z][y][x]
FU=np.array([[fr[:,1:].T for fr in frames('U-VELOCITY',k)[:5]] for k in range(NZ)]).transpose(1,0,2,3)
FV=np.array([[fr[1:,:].T for fr in frames('V-VELOCITY',k)[:5]] for k in range(NZ)]).transpose(1,0,2,3)
FWt=np.array([[fr[1:,1:].T for fr in frames('W-VELOCITY',k)[:5]] for k in range(NZ)]).transpose(1,0,2,3)  # top faces
FW=np.concatenate([np.zeros_like(FWt[:,:1]),FWt],1)  # floor face 0 (burner inflow ignored: layers >= 3 only)
def reference(T,U,V,W):
    uc=.5*(U[:,:,1:]+U[:,:,:-1]);vc=.5*(V[:,1:,:]+V[:,:-1,:]);wc=.5*(W[1:]+W[:-1])
    g=[np.gradient(T,DX,axis=a) for a in (2,1,0)]
    return -2*DT*(uc*g[0]+vc*g[1]+wc*g[2])
codes={'RISE':(R4[0]-R2[0],reference(*R3),R3[0]),'FDS':(FT[4]-FT[2],reference(FT[3],FU[2],FV[2],FW[2]),FT[3])}
bins=[300,330,400,600,900,1300,2500];K=slice(3,66)
print('T change over two steps against -2 dt u.grad(T) on the own midpoint state, layers 3-65: slope / corr / n')
for name,(d,a,Tm) in codes.items():
    d,a,Tm=d[K].ravel(),a[K].ravel(),Tm[K].ravel();line=f'  {name:4s}'
    for lo,hi in zip(bins[:-1],bins[1:]):
        s=(Tm>=lo)&(Tm<hi)
        if s.sum()<50:continue
        line+=f' | {lo}-{hi} K: {(d[s]*a[s]).sum()/(a[s]*a[s]).sum():.3f} / {np.corrcoef(d[s],a[s])[0,1]:.3f} / {s.sum()}'
    print(line)
