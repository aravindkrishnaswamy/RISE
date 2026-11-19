import numpy as np,csv
DX=.030593116067411014
F=np.load('fds_means.npz')
R={}
with open('/Volumes/SSD/RISE/fire_production_calibration/r254_turbulence_diagnosis/cell_moments.csv') as f:
    rd=csv.reader(f);h=next(rd)
    data=np.array([[float(x) for x in r] for r in rd])
col={n:i for i,n in enumerate(h)}
shape=(106,69,69)
def grid(name):
    g=np.zeros(shape);g[data[:,0].astype(int),data[:,1].astype(int),data[:,2].astype(int)]=data[:,col[name]];return g
for n in ['w','theta','rho','rhow','u','v']:R[n]=grid(n)
np.savez_compressed('rise_means.npz',**R)
xy=(np.arange(69)-34)*DX;X,Y=np.meshgrid(xy,xy,indexing='ij');RR=np.hypot(X,Y)
A=DX*DX
def fds(q,k):return F[f'{q}__{k}']
print('z   h   | r<0.8 up mass R/F | theta-halfwidth bF | core(r<2b) R/F | outer R/F | dT_c R/F | w_c R/F')
for k in [9,16,24,32,49,65,81,98]:
    mR=R['rhow'][k];mF=F[f'MFZ__{k}']
    TF=fds('TEMPERATURE',k)+273.15-300.0; TR=R['theta'][k]
    # FDS theta half width
    rad=np.arange(0,0.8,DX)
    def prof(T):
        return np.array([T[(RR>=a)&(RR<a+DX)].mean() for a in rad])
    pF=prof(TF);b=np.interp(0.5*pF[0],pF[::-1],rad[::-1])
    m8=RR<0.8;core=RR<2*b;outer=m8&~core
    s=lambda m,mask:np.maximum(m,0)[mask].sum()*A
    print(f'{k:3d} {(k+.5)*DX:5.2f} | {s(mR,m8):.4f}/{s(mF,m8):.4f} ({s(mR,m8)/s(mF,m8)-1:+.0%}) | {b:.3f} | {s(mR,core):.4f}/{s(mF,core):.4f} ({s(mR,core)/s(mF,core)-1:+.0%}) | {s(mR,outer):.4f}/{s(mF,outer):.4f} | {TR[34,34]:.0f}/{TF[34,34]:.0f} | {R["w"][k][34,34]:.2f}/{fds("W_VELOCITY",k)[34,34]:.2f}')
