# r271 non-reacting plume (section 21, read-only): is RISE's near-source shear layer over-damped by SGS viscosity or
# smeared by scalar transport? RISE inert arms (near_planes.cpp, 18 layers, frames 64-192 = 6-14 s) against FDS
# r271_inert over the same window, FDS velocities converted to cell averages (near_field.py fds_layers).
# argv[1]: dir with <arm>_near.bin; argv[2:]: arm names.
# Per height (z 0.05-0.50 m), from time means and shells |r-a|<dx/2:
#  - mean w and theta profiles; shear-layer vorticity thickness d_w = w_c/max|dW/dr| and its radius r*, thermal thickness
#    d_T = theta_c/max|dtheta/dr|;
#  - Vreman nu_t from the resolved cell-averaged velocity of each code by one formula (2dx central gradients, Cv=0.07,
#    nu = Cv sqrt(B/alpha.alpha), as frame_nut.cpp and FDS velo.f90:244), time-mean at r*; for FDS also its actual
#    (VISCOSITY - MOLECULAR VISCOSITY)/DENSITY there, which calibrates the formula against FDS's own discretisation;
#  - molecular nu at r* (Sutherland air, from the mean T there), and the shear-layer Reynolds number w_c d_w/(nu+nu_t).
import numpy as np,sys,os,re
from pathlib import Path
src=open(Path(__file__).with_name('near_field.py')).read().split('runs={}')[0]
os.environ['R271_FDS_CELL_AVERAGE']='1'
exec(src.split('W=Path(sys.argv[1])')[0]+src.split('W=Path(sys.argv[1])')[1])
RUN=Path('/Volumes/SSD/RISE/fire_production_calibration/r271_inert_fds/fds_run');P='r271_inert';START,END=6.0,14.0
W=Path(sys.argv[1]);ARMS=sys.argv[2:];KS=(1,3,6,9,12,16)
sl=[l for l in (RUN/(P+'.fds')).read_text().splitlines() if l.startswith('&SLCF')];idx={}
for n,l in enumerate(sl,1):
    q=re.search(r"QUANTITY='([^']+)'",l).group(1);m=re.search(r"PBZ=([0-9.eE+-]+)",l)
    if m and 'SPEC_ID' not in l:idx.setdefault((q,int(round(float(m.group(1))/DX-.5))),n)
def vreman(u,v,w,k):
    # u,v,w: [f][z][x][y]; central differences at layer k
    g=np.empty((3,3)+u[:,k].shape)
    for c,q in enumerate((u,v,w)):
        g[0,c]=np.gradient(q[:,k],DX,axis=1);g[1,c]=np.gradient(q[:,k],DX,axis=2);g[2,c]=(q[:,k+1]-q[:,k-1])/(2*DX)
    a2=(g**2).sum((0,1));B=np.einsum('mi...,mj...->ij...',g,g)*DX*DX
    bb=B[0,0]*B[1,1]-B[0,1]**2+B[0,0]*B[2,2]-B[0,2]**2+B[1,1]*B[2,2]-B[1,2]**2
    return np.where(a2>0,.07*np.sqrt(np.clip(bb,0,None)/np.where(a2>0,a2,1)),0.).mean(0)
rad=np.arange(0,.31,DX);shells=[np.abs(RR-a)<DX/2 for a in rad]
prof=lambda f:np.array([f[s].mean() for s in shells])
def numol(T):return 1.458e-6*T**1.5/(T+110.4)/(352.6/T)
def analyse(T,u,v,w,actual=None):
    out=[]
    for k in KS:
        wm=prof(w[:,k].mean(0));th=prof(T[:,k].mean(0))-300.;nt=prof(vreman(u,v,w,k))
        dW=-np.gradient(wm,DX);dT=-np.gradient(th,DX);i=int(np.argmax(dW));
        dw=wm[0]/dW[i];dTh=th[0]/dT.max();Tr=th[i]+300.;nm=numol(Tr)
        na=prof(actual[k]) [i] if actual is not None else np.nan
        out.append((k,wm[0],th[0],rad[i],dw,dTh,nm,nt[i],na,wm[0]*dw/(nm+nt[i]),wm[0]*dw/(nm+(na if actual is not None else nt[i]))))
    return out
runs={}
for arm in ARMS:
    a=np.transpose(np.fromfile(W/f'{arm}_near.bin',dtype='<f4').reshape(-1,NZ,4,N,N),(0,1,2,4,3)).astype(np.float64)
    runs[arm]=analyse(a[:,:,0],a[:,:,1],a[:,:,2],a[:,:,3]);del a
T,u,v,w=fds_layers(idx,['TEMPERATURE','U-VELOCITY','V-VELOCITY','W-VELOCITY']);T=T+273.15
act={k:((read_slice(RUN/f'{P}_1_{idx[("VISCOSITY",k)]}.sf')-read_slice(RUN/f'{P}_1_{idx[("MOLECULAR VISCOSITY",k)]}.sf'))
        /read_slice(RUN/f'{P}_1_{idx[("DENSITY",k)]}.sf')).mean(0) for k in KS}
runs['FDS r271_inert']=analyse(T,u,v,w,act)
print('Near-source shear layer, 6-14 s. z (m): w_c (m/s), theta_c (K), r* (m), d_w (m), d_T (m), nu_mol, nu_t formula,')
print('nu_t FDS actual (1e-4 m2/s), Re = w_c d_w/(nu+nu_t) with the formula nu_t [and with FDS actual]')
for n,rows in runs.items():
    print(f'  {n}')
    for k,wc,thc,rs,dw,dT,nm,nt,na,re1,re2 in rows:
        print(f'    {(k+.5)*DX:4.2f}  {wc:5.2f} {thc:6.0f}  {rs:5.3f}  {dw:6.3f} {dT:6.3f}   {1e4*nm:6.3f} {1e4*nt:6.3f} '
              +(f'{1e4*na:6.3f}' if na==na else '   -  ')+f'   Re {re1:6.0f}'+(f' [{re2:6.0f}]' if na==na else ''))
