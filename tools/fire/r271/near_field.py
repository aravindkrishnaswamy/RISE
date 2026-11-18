# r271 near field (read-only): where RISE loses resolved mixing below 0.5 m, against FDS r263.
# argv[1]: dir with near_planes.cpp outputs r270.bin (235 frames), compact.bin, scalar.bin (64 frames each, 73-136).
# FDS: r263 horizontal slices of U,V,W,TEMPERATURE,DENSITY at the same cell-centred heights, full window 10.47-25.09 s.
# On cylinders r=R (cells with |r-R|<dx/2, ring mean x 2 pi R), per unit height:
#   entrainment  -2 pi R <rho u_r>              (kg/s/m)
#   heat out     2 pi R cp <rho u_r theta>, split into mean-flow 2 pi R cp <rho u_r><theta> and the turbulent rest (kW/m)
#   rms of u_r and w on the cylinder (m/s)
# RISE rho = p0 M/(R T) = 352.6/T (as ab_compare.py); FDS uses its DENSITY slice.
import numpy as np,re,struct,sys
from pathlib import Path
DX=.030593116067411014;NZ=18;cp=1150.;N=69
xy=(np.arange(N)-34)*DX;X,Y=np.meshgrid(xy,xy,indexing='ij');RR=np.hypot(X,Y)  # [x][y]
RS=(.10,.15,.20,.30);KS=(1,3,6,9,12,16)
W=Path(sys.argv[1])
def rise(name,frames):
    a=np.fromfile(W/f'{name}.bin',dtype='<f4').reshape(frames,NZ,4,N,N).astype(np.float64)
    a=np.transpose(a,(0,1,2,4,3))  # -> [f][z][c][x][y]
    T,u,v,w=a[:,:,0],a[:,:,1],a[:,:,2],a[:,:,3];return T,u,v,w,352.6/T
RUN=Path('/Volumes/SSD/RISE/fire_production_calibration/r263_fds_no_slip/fds_run');P='r263_fds'
START=10.466040127212182;END=25.091040127212182
def record(f):
    b=f.read(4)
    if not b:return None
    n=struct.unpack('<i',b)[0];d=f.read(n);f.read(4);return d
def read_slice(p,full=False):
    with p.open('rb') as f:
        for _ in range(4):record(f)
        ts=[];fs=[]
        while True:
            t=record(f)
            if t is None:break
            a=np.frombuffer(record(f),dtype='<f4').reshape((70,70),order='F');ts.append(struct.unpack('<f',t)[0]);fs.append(a.copy() if full else a[1:,1:].copy())
    t=np.array(ts);s=(t>=START)&(t<=END);return np.array(fs,dtype=np.float64)[s]
# FDS CELL_CENTERED velocity slices are raw staggered face values (dump.f90 GAS_PHASE_OUTPUT: U on the cell's +x face, V on
# +y, W on the top face; index 0 of the 70x70 array is the domain's -x/-y boundary face), while RISE frames hold cell
# averages of two faces. With R271_FDS_CELL_AVERAGE=1, fds_layers returns FDS velocities as the same two-face cell
# averages. The bottom face of layer 0 is recovered exactly from the DIVERGENCE slice (the face divergence reproduces it
# to round-off). Default off, so committed outputs reproduce.
CELL_AVERAGE=__import__('os').environ.get('R271_FDS_CELL_AVERAGE')=='1'
def fds_layers(idx,Q):
    if CELL_AVERAGE:print('# FDS velocities converted from face values to cell averages (R271_FDS_CELL_AVERAGE=1)')
    def get(q,k,full=False):return read_slice(RUN/f'{P}_1_{idx[(q,k)]}.sf',full)
    out=[]
    for q in Q:
        if not CELL_AVERAGE or q not in ('U-VELOCITY','V-VELOCITY','W-VELOCITY'):
            out.append(np.stack([get(q,k) for k in range(NZ)],1));continue
        layers=[]
        for k in range(NZ):
            if q=='U-VELOCITY':a=get(q,k,True);layers.append(.5*(a[:,1:,1:]+a[:,:-1,1:]))
            elif q=='V-VELOCITY':a=get(q,k,True);layers.append(.5*(a[:,1:,1:]+a[:,1:,:-1]))
            else:
                top=get(q,k)
                if k:bottom=get(q,k-1)
                else:
                    U=get('U-VELOCITY',0,True);Vv=get('V-VELOCITY',0,True)
                    bottom=top-DX*get('DIVERGENCE',0)+(U[:,1:,1:]-U[:,:-1,1:])+(Vv[:,1:,1:]-Vv[:,1:,:-1])
                layers.append(.5*(top+bottom))
        out.append(np.stack(layers,1))
    return out
def fds():
    sl=[l for l in (RUN/(P+'.fds')).read_text().splitlines() if l.startswith('&SLCF')];idx={}
    for n,l in enumerate(sl,1):
        q=re.search(r"QUANTITY='([^']+)'",l).group(1);m=re.search(r"PBZ=([0-9.eE+-]+)",l)
        if m:idx.setdefault((q,int(round(float(m.group(1))/DX-.5))),n)
    Q=['TEMPERATURE','U-VELOCITY','V-VELOCITY','W-VELOCITY','DENSITY']
    out=fds_layers(idx,Q)
    out[0]=out[0]+273.15;return tuple(out)
def stats(T,u,v,w,rho):
    th=T-300.;ur=(u*X+v*Y)/np.where(RR>0,RR,1.)
    m=lambda q:q.mean(0)
    rur=m(rho*ur);rurth=m(rho*ur*th);mean_part=m(rho)*m(ur)*m(th)
    out={}
    for R in RS:
        ring=np.abs(RR-R)<DX/2;L=2*np.pi*R
        for k in KS:
            g=lambda q:q[k][ring].mean()
            out[(R,k)]=(-L*g(rur),L*cp*g(rurth)/1e3,L*cp*g(mean_part)/1e3,L*cp*(g(rurth)-g(mean_part))/1e3,
                        np.sqrt(g(ur.var(0))),np.sqrt(g(w.var(0))))
    prof={}
    for k in (3,9,16):
        rad=np.arange(0,.42,DX);wr=np.sqrt(w[:,k].var(0));tr=np.sqrt(T[:,k].var(0));wm=m(w)[k];ab=np.abs(m(ur))[k]
        prof[k]=[(a,wm[np.abs(RR-a)<DX/2].mean(),wr[np.abs(RR-a)<DX/2].mean(),tr[np.abs(RR-a)<DX/2].mean()) for a in rad]
    return out,prof
runs={}
T,u,v,w,rho=rise('r270',235);runs['r270 full']=stats(T,u,v,w,rho)
runs['r270 73-136']=stats(*(q[73:137] for q in (T,u,v,w,rho)));del T,u,v,w,rho
runs['compact']=stats(*rise('compact',64));runs['compact+scalar']=stats(*rise('scalar',64))
F=fds();print(f'FDS samples in window: {F[0].shape[0]}');runs['FDS r263']=stats(*F);del F
names=list(runs)
labels=['1. Mean entrainment through the cylinder, -2piR<rho u_r> (kg/s per m; + = inflow)',
        '2. Total resolved heat out, 2piR cp<rho u_r theta> (kW per m; + = outward)',
        '3. Mean-flow part, 2piR cp<rho><u_r><theta> (kW per m)',
        '4. Turbulent part, total - mean-flow (kW per m)',
        '5. rms u_r on the cylinder (m/s)','6. rms w on the cylinder (m/s)']
for i,lab in enumerate(labels):
    print('\n'+lab+'\n   R(m)  z(m)  '+''.join(f'{n:>15s}' for n in names))
    for R in RS:
        for k in KS:print(f'   {R:4.2f}  {(k+.5)*DX:4.2f}  '+''.join(f'{runs[n][0][(R,k)][i]:15.4f}' for n in names))
print('\n7. Radial profiles: mean w / rms w / rms T (m/s, m/s, K)')
for k in (3,9,16):
    print(f'   z={(k+.5)*DX:.2f} m   r(m)'+''.join(f'{n:>22s}' for n in names))
    for j in range(len(runs[names[0]][1][k])):
        print(f'              {runs[names[0]][1][k][j][0]:4.2f}'+''.join('   {:5.2f} {:4.2f} {:5.0f}   '.format(*runs[n][1][k][j][1:]) for n in names))
# 8. Scale split: fluctuation energy of w in the core (r<0.15 m) after removing the time mean, split by horizontal
# wavenumber at kD=pi/4 (wavelength 8 cells = 0.24 m). Large = kD<pi/4, small = the rest. Per-run arrays reread.
print('\n8. Core (r<0.15 m) w fluctuation rms by horizontal scale: large (wavelength > 0.24 m) / smaller (m/s)')
kx=np.fft.fftfreq(N)*2*np.pi;K=np.hypot(*np.meshgrid(kx,kx,indexing='ij'));core=RR<.15
def split(w,k):
    p=w[:,k]-w[:,k].mean(0);Fp=np.fft.fft2(p);lo=np.real(np.fft.ifft2(np.where(K<np.pi/4,Fp,0)));hi=p-lo
    return np.sqrt((lo[:,core]**2).mean()),np.sqrt((hi[:,core]**2).mean())
sources={'r270 full':lambda:rise('r270',235)[3],'r270 73-136':lambda:rise('r270',235)[3][73:137],
         'compact':lambda:rise('compact',64)[3],'compact+scalar':lambda:rise('scalar',64)[3],'FDS r263':lambda:fds()[3]}
res={n:(lambda w:{k:split(w,k) for k in KS})(f()) for n,f in sources.items()}
print('   z(m)  '+''.join(f'{n:>17s}' for n in res))
for k in KS:print(f'   {(k+.5)*DX:4.2f}  '+''.join('    {:5.3f} / {:5.3f}'.format(*res[n][k]) for n in res))
