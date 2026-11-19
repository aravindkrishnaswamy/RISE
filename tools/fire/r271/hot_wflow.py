# r271 section 27: the flow part of the W change (full run minus the at-rest run from the same temperature, steps 2->4,
# SGS off) against a common advection reference -2 dt (u.grad) w on each code's own full midpoint state (step 3), at the
# top faces of layers 3-65, binned by the face's mean temperature. The reference is advective-form central differences
# (cell-centre u, v and w averaged to the z face). Slope of the code's flow part on its reference, and correlation; the
# same pressure response acts on both, so a code that is weak here is weak in advection or in its pressure response.
# argv: RISE full step 2, 3, 4 states, RISE rest step 2, 4 states, FDS full dir, FDS rest dir (velocity frames = steps).
import numpy as np,re,struct,sys
from pathlib import Path
N=69;NZ=106;DX=.030593116067411014;DT=.0016462659696117043
def split(a):
    nc=N*N*NZ;nu=(N+1)*N*NZ;nw=N*N*(NZ+1)
    return (a[:nc].reshape(NZ,N,N),a[nc:nc+nu].reshape(NZ,N,N+1),a[nc+nu:nc+2*nu].reshape(NZ,N+1,N),a[nc+2*nu:nc+2*nu+nw].reshape(NZ+1,N,N))
R2,R3,R4,Q2,Q4=(split(np.fromfile(p,'<f8')) for p in sys.argv[1:6])
def fds(run,frames_wanted):
    run=Path(run);sl=[l for l in (run/'xp.fds').read_text().splitlines() if l.startswith('&SLCF')];idx={}
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
        return [out[i] for i in frames_wanted]
    g=lambda q,cut:np.array([[cut(fr) for fr in frames(q,k)] for k in range(NZ)]).transpose(1,0,2,3)
    T=g('TEMPERATURE',lambda a:a[1:,1:].T+273.15);U=g('U-VELOCITY',lambda a:a[:,1:].T);V=g('V-VELOCITY',lambda a:a[1:,:].T)
    W=g('W-VELOCITY',lambda a:a[1:,1:].T);W=np.concatenate([np.zeros_like(W[:,:1]),W],1)
    return T,U,V,W
FT,FU,FV,FW=fds(sys.argv[6],[2,3,4]);GT,GU,GV,GW=fds(sys.argv[7],[2,4])
def reference(U,V,W):
    uc=.5*(U[:,:,1:]+U[:,:,:-1]);vc=.5*(V[:,1:,:]+V[:,:-1,:])
    uz=.5*(uc[1:]+uc[:-1]);vz=.5*(vc[1:]+vc[:-1])          # at interior z faces 1..NZ-1
    Wi=W[1:-1];dwdx=np.gradient(Wi,DX,axis=2);dwdy=np.gradient(Wi,DX,axis=1);dwdz=(W[2:]-W[:-2])/(2*DX)
    return -2*DT*(uz*dwdx+vz*dwdy+Wi*dwdz)                 # index j -> face j+1 = top face of layer j
K=slice(3,66)
def faceT(T):return .5*(T[1:]+T[:-1])
codes={'RISE':((R4[3]-R2[3])-(Q4[3]-Q2[3]),reference(R3[1],R3[2],R3[3]),faceT(R3[0]),R4[3]-R2[3]),
       'FDS':((FW[2]-FW[0])-(GW[1]-GW[0]),reference(FU[1],FV[1],FW[1]),faceT(FT[2]),FW[2]-FW[0])}  # FDS T frame 4 = step 3
bins=[300,330,400,600,900,1300,2500]
print('W flow part (full - rest), steps 2->4, top faces of layers 3-65, against -2 dt (u.grad) w on the own step-3 state')
print('  per T bin: slope of flow part on reference / corr | rms flow part, rms full change')
for name,(d,a,Tf,full) in codes.items():
    d,a,Tf,full=d[1:-1][K].ravel(),a[K].ravel(),Tf[K].ravel(),full[1:-1][K].ravel();line=f'  {name:4s}'
    for lo,hi in zip(bins[:-1],bins[1:]):
        s=(Tf>=lo)&(Tf<hi)
        if s.sum()<50:continue
        line+=f' | {lo}-{hi}: {(d[s]*a[s]).sum()/(a[s]*a[s]).sum():.3f}/{np.corrcoef(d[s],a[s])[0,1]:.2f} {np.sqrt((d[s]**2).mean()):.4f} {np.sqrt((full[s]**2).mean()):.4f}'
    print(line)
